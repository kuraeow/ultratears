// Steam Deck: the gyro and accelerometer of the Deck's own controller, read from its HID device. In game mode Steam
// Input hands the game a virtual gamepad, which has buttons and sticks but no motion sensors, so motion aiming did
// nothing on the Deck; turning Steam Input off for the game would lose the player's Steam layout. The controller's HID
// device (Valve 28de:1205) stays readable beside Steam (Steam's udev rules give the logged-in user access to it): this
// thread reads its state reports and feeds their motion to motion_fusion.c, while buttons and sticks keep coming
// through Steam Input. Started only when the gamepad SDL opened has no motion sensors of its own (window_sdl.c).
// Report layout and scales as SDL's Steam Deck driver reads them (SDL3 src/joystick/hidapi/SDL_hidapi_steamdeck.c,
// zlib; no code taken): a 64-byte report, bytes 0-1 the version 0x0001, byte 2 the type (0x09: the controller's state),
// the state from byte 4: packet number (4), buttons (8), the two pads (8), then accelerometer x, y, z and gyro x, y, z
// as int16, little-endian; 32768 = 2 G and 2000 deg/s. Steam keeps the controller's motion off unless the player's
// Steam Input layout uses it (the reports carried zeros, 2026-11-02): as SDL's driver does, the controller is told to
// send raw gyro and accelerometer data (feature report 0x87 "set settings", register SETTING_IMU_MODE), and told again
// every few seconds in case Steam sets it back. Only that register is written; Steam's layout stays as it is.
// TOTK_DECK_GYRO=0: off.
#include <stdint.h>

int deck_imu_start(void);

#ifndef __linux__
int deck_imu_start(void) { return 0; }
#else
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host.h"

enum { REPORT = 64, STATE = 0x09, SET_SETTINGS = 0x87, SETTING_IMU_MODE = 48, IMU_RAW_ACCEL = 0x08, IMU_RAW_GYRO = 0x10 };
static int g_writable;

static int imu_on(int fd) {   // 1: the controller took the setting
    unsigned char f[REPORT + 1] = {0};   // f[0]: the report number, 0 (the Deck's reports are not numbered)
    f[1] = SET_SETTINGS;
    f[2] = 3;   // one setting: register, value low, value high
    f[3] = SETTING_IMU_MODE;
    f[4] = IMU_RAW_ACCEL | IMU_RAW_GYRO;
    return ioctl(fd, HIDIOCSFEATURE(sizeof f), f) >= 0;
}
static int g_fd = -1, g_started;

static int le16(const unsigned char *p) { return (int16_t)(p[0] | p[1] << 8); }
static int is_state(const unsigned char *b, ssize_t n) { return n >= 36 && b[0] == 0x01 && b[1] == 0x00 && b[2] == STATE; }

// The hidraw device of the Deck's controller interface: the one of 28de:1205 that sends state reports.
static int open_deck(void) {
    int denied = 0;
    for (int i = 0; i < 64; i++) {
        char path[96], line[256];
        snprintf(path, sizeof path, "/sys/class/hidraw/hidraw%d/device/uevent", i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int match = 0;
        while (fgets(line, sizeof line, f)) if (!strncmp(line, "HID_ID=", 7) && strstr(line, "000028DE:00001205")) match = 1;
        fclose(f);
        if (!match) continue;
        snprintf(path, sizeof path, "/dev/hidraw%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        g_writable = fd >= 0;
        if (fd < 0) fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { if (errno == EACCES) denied = 1; continue; }
        unsigned char b[REPORT];
        for (int tries = 0; tries < 10; tries++) {   // up to ~0.2 s for a report
            struct pollfd pf = {fd, POLLIN, 0};
            if (poll(&pf, 1, 20) <= 0) continue;
            ssize_t n = read(fd, b, sizeof b);
            if (is_state(b, n)) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
                int on = g_writable && imu_on(fd);
                fprintf(stderr, "[totk] Steam Deck controller motion read from %s (beside Steam Input); raw motion %s\n", path,
                        on ? "turned on" : g_writable ? "could not be turned on" : "not writable: as Steam has it");
                return fd;
            }
        }
        close(fd);
    }
    if (denied) fprintf(stderr, "[totk] the Steam Deck controller's HID device is not readable (permissions): no gyro aiming\n");
    return -1;
}

static void *deck_main(void *arg) {
    (void)arg;
    unsigned char b[REPORT];
    uint64_t quiet = 0, said_quiet = 0, last_on = runtime_now_ns(), said_live = 0;
    for (;;) {
        uint64_t now = runtime_now_ns();
        if (g_writable && now - last_on > 3000000000ull) { imu_on(g_fd); last_on = now; }   // Steam may set it back
        ssize_t n = read(g_fd, b, sizeof b);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[totk] Steam Deck controller motion: read failed (%s); stopped\n", strerror(errno));
            return NULL;
        }
        if (!is_state(b, n)) continue;
        const unsigned char *s = b + 4;
        int ax = le16(s + 20), ay = le16(s + 22), az = le16(s + 24), gx = le16(s + 26), gy = le16(s + 28), gz = le16(s + 30);
        if (!(ax | ay | az | gx | gy | gz)) {   // Steam has the IMU off: nothing to feed
            if (++quiet == 1000 && !said_quiet++) fprintf(stderr, "[totk] Steam Deck controller: its motion sensors report nothing\n");
            continue;
        }
        quiet = 0;
        if (!said_live++) fprintf(stderr, "[totk] Steam Deck controller: motion data arriving\n");
        // SDL's gamepad frame from the report (as its Steam Deck driver), then the Switch frame (as window_sdl.c's
        // pad_sensor): the gyro reads in the report's own axes, the acceleration negated (gravity, not the push).
        const double k = 2000.0 * M_PI / 180.0 / 32768.0, a = 2.0 / 32768.0;
        double acc[3] = {-ax * a, -ay * a, -az * a}, gyro[3] = {gx * k, gy * k, gz * k};
        runtime_motion_feed(acc, gyro, runtime_now_ns(), 0);
    }
}

// 1: the Deck's motion is fed (or already was).
int deck_imu_start(void) {
    if (g_started) return g_fd >= 0;
    g_started = 1;
    const char *e = getenv("TOTK_DECK_GYRO");
    if (e && *e == '0') return 0;
    if ((g_fd = open_deck()) < 0) return 0;
    pthread_t t;
    if (pthread_create(&t, NULL, deck_main, NULL) != 0) { close(g_fd); g_fd = -1; return 0; }
    pthread_detach(t);
    return 1;
}
#endif
