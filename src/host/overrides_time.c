/* nn::time for the functions the game imports, backed by the host clock and time zone.
 * Ported from MacNativeGames games/totk/runtime/time_host.c @b75be8c (MIT). Changes: guest-register wrappers;
 * Windows clock and time-zone calls. User and network system clocks both follow the host wall clock; the steady clock
 * counts seconds of host monotonic time with one source id per process. ClockSnapshot uses the public 0xD0-byte
 * layout because game code may read its fields through inline accessors.
 * TODO(port): the snapshot's location name is "UTC"; map the Windows time zone to its IANA name. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#ifdef _WIN32
#include <windows.h>
#endif
#endif

#include "host.h"
#include "../runtime/hle.h"

typedef struct { int64_t value; uint8_t source[16]; } SteadyClockTimePoint;
typedef struct { int64_t offset; SteadyClockTimePoint point; } SystemClockContext;
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;
typedef struct { int32_t day_of_week, day_of_year; char name[8]; uint8_t dst, pad[3]; int32_t utc_offset; } CalendarAdditionalInfo;
typedef struct {
    SystemClockContext user, network; int64_t user_time, network_time;
    CalendarTime user_calendar, network_calendar; CalendarAdditionalInfo user_info, network_info;
    SteadyClockTimePoint steady; char location[0x24]; uint8_t automatic_correction, type, pad[2];
} ClockSnapshot;
_Static_assert(sizeof(SteadyClockTimePoint) == 0x18 && sizeof(CalendarTime) == 8 && sizeof(CalendarAdditionalInfo) == 0x18, "nn::time types");
_Static_assert(sizeof(ClockSnapshot) == 0xd0, "nn::time::ClockSnapshot");

#define R_INVALID 0x74u          /* time module 116: invalid argument family */
#define R_SOURCE_MISMATCH 0x18074u
static uint8_t source_id[16];
static int initialized;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void make_source(void) {
    uint64_t a = runtime_now_ns() ^ ((uint64_t)rand() << 32), b = (uint64_t)time(NULL) * 0x9e3779b97f4a7c15ULL ^ (uint64_t)rand();
    memcpy(source_id, &a, 8); memcpy(source_id + 8, &b, 8);
}

static int64_t steady_seconds(void) { return (int64_t)(runtime_now_ns() / 1000000000u); }
static SteadyClockTimePoint steady_now(void) {
    pthread_once(&once, make_source);
    SteadyClockTimePoint p = {steady_seconds(), {0}};
    memcpy(p.source, source_id, 16);
    return p;
}
static int64_t posix_now(void) {
    static long off = -1;
    if (off < 0) { const char *e = getenv("TOTK_TIME_OFFSET"); off = e ? atol(e) : 0; if (off < 0) off = 0; }   /* diagnostic */
    return (int64_t)time(NULL) + off;
}

static int32_t local_utc_offset(time_t t, int *dst) {
#ifdef _WIN32
    TIME_ZONE_INFORMATION tz;
    DWORD r = GetTimeZoneInformation(&tz);
    *dst = r == TIME_ZONE_ID_DAYLIGHT;
    return -(int32_t)(tz.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : r == TIME_ZONE_ID_STANDARD ? tz.StandardBias : 0)) * 60;
#else
    struct tm local;
    if (!localtime_r(&t, &local)) runtime_fail("local time zone conversion");
    *dst = local.tm_isdst > 0;
    return (int32_t)local.tm_gmtoff;
#endif
}

static void fill_calendar(CalendarTime *c, CalendarAdditionalInfo *info, int64_t posix, int local) {
    time_t t = (time_t)posix;
    struct tm tm;
#ifdef _WIN32
    if ((local ? localtime_s(&tm, &t) : gmtime_s(&tm, &t)) != 0) runtime_fail("calendar conversion");
#else
    if (!(local ? localtime_r(&t, &tm) : gmtime_r(&t, &tm))) runtime_fail("calendar conversion");
#endif
    *c = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour,
                        (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (info) {
        memset(info, 0, sizeof *info);
        info->day_of_week = tm.tm_wday;
        info->day_of_year = tm.tm_yday;
        int dst = 0;
        int32_t offset = local ? local_utc_offset(t, &dst) : 0;
        snprintf(info->name, sizeof info->name, "%s", local ? "LOCAL" : "UTC");
        info->dst = (uint8_t)(local && dst);
        info->utc_offset = offset;
    }
}

static ClockSnapshot *snap(uint64_t a) { return (ClockSnapshot *)(uintptr_t)a; }

HLE_FUNC("_ZN2nn4time10InitializeEv", time_initialize) { initialized = 1; pthread_once(&once, make_source); c->x[0] = 0; }
HLE_FUNC("_ZN2nn4time8FinalizeEv", time_finalize) { initialized = 0; c->x[0] = 0; }
HLE_FUNC("_ZN2nn4time13IsInitializedEv", time_is_initialized) { c->x[0] = (unsigned char)initialized; }
HLE_FUNC("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", time_user_now) {
    int64_t *out = (int64_t *)(uintptr_t)c->x[0];
    if (!out) { c->x[0] = R_INVALID; return; }
    *out = posix_now(); c->x[0] = 0;
}
HLE_FUNC("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", time_network_now) {
    int64_t *out = (int64_t *)(uintptr_t)c->x[0];
    if (!out) { c->x[0] = R_INVALID; return; }
    *out = posix_now(); c->x[0] = 0;
}
/* StandardSteadyClock::now() returns the chrono-style time_point: the int64 seconds value only (in x0). */
HLE_FUNC("_ZN2nn4time19StandardSteadyClock3nowEv", time_steady_now) { c->x[0] = (uint64_t)steady_now().value; }
HLE_FUNC("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", time_to_calendar) {
    CalendarTime *cal = (CalendarTime *)(uintptr_t)c->x[0];
    CalendarAdditionalInfo *info = (CalendarAdditionalInfo *)(uintptr_t)c->x[1];
    const int64_t *posix = (const int64_t *)(uintptr_t)c->x[2];
    if (!cal || !posix) { c->x[0] = R_INVALID; return; }
    fill_calendar(cal, info, *posix, 1);
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", time_to_calendar_utc) {   /* returns the 8-byte struct in x0 */
    const int64_t *posix = (const int64_t *)(uintptr_t)c->x[0];
    CalendarTime cal;
    fill_calendar(&cal, NULL, posix ? *posix : 0, 0);
    uint64_t v; memcpy(&v, &cal, 8); c->x[0] = v;
}
HLE_FUNC("_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", time_to_posix_utc) {
    const CalendarTime *cal = (const CalendarTime *)(uintptr_t)c->x[0];
    struct tm tm = {0};
    tm.tm_year = cal->year - 1900; tm.tm_mon = cal->month - 1; tm.tm_mday = cal->day;
    tm.tm_hour = cal->hour; tm.tm_min = cal->minute; tm.tm_sec = cal->second;
#ifdef _WIN32
    c->x[0] = (uint64_t)(int64_t)_mkgmtime(&tm);
#else
    c->x[0] = (uint64_t)(int64_t)timegm(&tm);
#endif
}
HLE_FUNC("_ZN2nn4time13ClockSnapshotC1Ev", time_snapshot_init) { memset(snap(c->x[0]), 0, sizeof(ClockSnapshot)); }
HLE_FUNC("_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", time_snapshot_create) {
    ClockSnapshot *s = snap(c->x[0]);
    if (!s) { c->x[0] = R_INVALID; return; }
    SteadyClockTimePoint steady = steady_now();
    int64_t now = posix_now();
    memset(s, 0, sizeof *s);
    s->user = (SystemClockContext){now - steady.value, steady};
    s->network = s->user;
    s->user_time = now; s->network_time = now;
    fill_calendar(&s->user_calendar, &s->user_info, now, 1);
    fill_calendar(&s->network_calendar, &s->network_info, now, 1);
    s->steady = steady;
    snprintf(s->location, sizeof s->location, "UTC");
    s->automatic_correction = 1; s->type = 0;
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", time_span_between) {
    int64_t *out = (int64_t *)(uintptr_t)c->x[0];
    const ClockSnapshot *a = snap(c->x[1]), *b = snap(c->x[2]);
    if (!out || !a || !b) { c->x[0] = R_INVALID; return; }
    if (memcmp(a->steady.source, b->steady.source, 16)) { c->x[0] = R_SOURCE_MISMATCH; return; }
    *out = (b->steady.value - a->steady.value) * 1000000000LL;
    c->x[0] = 0;
}
/* Change of the user clock offset between snapshots (manual adjustments), as a TimeSpan in x0. */
HLE_FUNC("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", time_user_difference) {
    const ClockSnapshot *a = snap(c->x[0]), *b = snap(c->x[1]);
    c->x[0] = memcmp(a->user.point.source, b->user.point.source, 16) ? 0 : (uint64_t)((b->user.offset - a->user.offset) * 1000000000LL);
}
