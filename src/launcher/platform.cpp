// What the launcher asks of the system beyond SDL: disk space, a file manager with the log selected, one instance,
// the dark title bar, the settings file. Windows first; on Linux the simple equivalents.
#include "launcher.h"
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#ifndef _WIN32
static int g_instance_fd = -1;   // the one-instance lock (flock), close-on-exec: the game and the builder do not hold it
#endif
#ifdef _WIN32
static HANDLE g_instance;   // the one-instance mutex

static std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif

std::string exe_dir() {
    std::string d = SDL_GetBasePath() ? SDL_GetBasePath() : "./";
    while (d.size() > 1 && (d.back() == '/' || d.back() == '\\'))
        d.pop_back();
    return d;
}

// Started from an AppImage the launcher's folder is the image's read-only mount (gone when it exits): its settings and
// the game go beside the AppImage file instead ($APPIMAGE, set by the AppImage runtime).
static std::string appimage_dir() {
#ifdef _WIN32
    return "";
#else
    const char *a = SDL_getenv("APPIMAGE");
    if (!a || !*a)
        return "";
    std::string d = a;
    size_t cut = d.find_last_of('/');
    return cut == std::string::npos ? std::string(".") : cut == 0 ? std::string("/") : d.substr(0, cut);
#endif
}
std::string own_dir() {
    std::string d = appimage_dir();
    return d.empty() ? exe_dir() : d;
}
// The package's own folder; from an AppImage: the folder of a game beside it (the AppImage put in a game folder to
// update it, as one from a Steam Deck's tar), else a new UltraTears folder there.
std::string default_install() {
    std::string d = appimage_dir();
    if (d.empty())
        return exe_dir();
    if (path_is_file(path_join(d, "ultratears.cfg")))
        return d;
    return path_join(d, "UltraTears");
}

std::string path_join(const std::string &a, const std::string &b) {
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (a.empty())
        return b;
    return a.back() == '/' || a.back() == '\\' ? a + b : a + sep + b;
}

static bool path_type(const std::string &p, SDL_PathType want) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == want;
}
bool path_is_dir(const std::string &p) { return path_type(p, SDL_PATHTYPE_DIRECTORY); }
bool path_is_file(const std::string &p) { return path_type(p, SDL_PATHTYPE_FILE); }

static std::string existing_ancestor(std::string p) {   // the folder may not exist yet: its nearest parent that does
    while (!p.empty() && !path_is_dir(p)) {
        size_t cut = p.find_last_of("/\\");
        if (cut == std::string::npos || cut == 0)
            break;
        p.resize(cut);
        if (p.size() == 2 && p[1] == ':')
            p += '\\';
    }
    return p;
}

double disk_free_gb(const std::string &folder) {
    std::string p = existing_ancestor(folder);
#ifdef _WIN32
    ULARGE_INTEGER free;
    if (!GetDiskFreeSpaceExW(wide(p).c_str(), &free, nullptr, nullptr))
        return -1;
    return free.QuadPart / 1073741824.0;
#else
    struct statvfs s;
    if (statvfs(p.c_str(), &s) != 0)
        return -1;
    return (double)s.f_bavail * s.f_frsize / 1073741824.0;
#endif
}

std::string drive_of(const std::string &folder) {
#ifdef _WIN32
    if (folder.size() >= 2 && folder[1] == ':')
        return folder.substr(0, 2);
#endif
    return existing_ancestor(folder);
}

void reveal_in_folder(const std::string &file) {
#ifdef _WIN32
    std::wstring w = wide(file);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(w.c_str());
    if (pidl) {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr))
            return;
    }
    std::wstring dir = w.substr(0, w.find_last_of(L"\\/"));
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    // [temporary] Linux: the folder only; the file selected needs D-Bus org.freedesktop.FileManager1.ShowItems
    std::string dir = file.substr(0, file.find_last_of('/'));
    SDL_OpenURL(("file://" + dir).c_str());
#endif
}

bool single_instance(const char *title) {
#ifdef _WIN32
    g_instance = CreateMutexW(nullptr, TRUE, L"Local\\UltraTearsLauncher");
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return true;
    HWND other = FindWindowW(nullptr, wide(title).c_str());
    if (other) {
        ShowWindow(other, SW_RESTORE);
        SetForegroundWindow(other);
    }
    return false;
#else
    // The lock held by another process: a launcher is running (its window is not raised here: no portable way).
    // Before 2026-11-01 the descriptor was inherited by the game and the builder the launcher starts, so a game still
    // running (or a build) kept the next launcher from starting at all, silently (Steam Deck, Game Mode).
    const char *run = SDL_getenv("XDG_RUNTIME_DIR");
    std::string lock = std::string(run ? run : "/tmp") + "/ultratears-launcher.lock";
    int fd = open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return true;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        SDL_Log("another launcher is running (%s is locked): this one ends", lock.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, T("already_running"), nullptr);
        return false;
    }
    g_instance_fd = fd;   // stays open while the launcher runs
    return true;
#endif
}

void window_dark_title(SDL_Window *w) {
#ifdef _WIN32
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd)
        return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark);   // DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1+)
    COLORREF caption = RGB(15, 15, 15);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof caption);   // DWMWA_CAPTION_COLOR (Windows 11; ignored on 10)
#else
    (void)w;
#endif
}

Lang system_lang() {
    int n = 0;
    SDL_Locale **loc = SDL_GetPreferredLocales(&n);
    Lang l = LANG_EN;
    if (loc && n > 0 && loc[0]->language)
        l = lang_from_code(loc[0]->language);
    SDL_free(loc);
    return l;
}

int logical_cpus() { return SDL_max(1, SDL_GetNumLogicalCPUCores()); }

// ---- settings: ultratears-launcher.ini next to the launcher, key=value lines in UTF-8
static std::string settings_path() { return path_join(own_dir(), "ultratears-launcher.ini"); }

Settings settings_load() {
    Settings s;
    size_t size = 0;
    char *text = (char *)SDL_LoadFile(settings_path().c_str(), &size);
    if (!text)
        return s;
    std::string all(text, size);
    SDL_free(text);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find('\n', pos);
        if (end == std::string::npos)
            end = all.size();
        std::string line = all.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#')
            continue;
        s.set(line.substr(0, eq).c_str(), line.substr(eq + 1));
    }
    return s;
}

void settings_save(const Settings &s) {
    std::string text = "# UltraTears Launcher\n";
    for (auto &kv : s.kv)
        text += kv.first + "=" + kv.second + "\n";
    SDL_SaveFile(settings_path().c_str(), text.data(), text.size());
}

std::string Settings::get(const char *key) const {
    for (auto &kv : this->kv)
        if (kv.first == key)
            return kv.second;
    return "";
}

void Settings::set(const char *key, const std::string &value) {
    for (auto &kv : this->kv)
        if (kv.first == key) {
            kv.second = value;
            return;
        }
    kv.emplace_back(key, value);
}

void single_instance_release() {
#ifdef _WIN32
    if (g_instance)
        CloseHandle(g_instance);
    g_instance = nullptr;
#else
    if (g_instance_fd >= 0)
        close(g_instance_fd);
    g_instance_fd = -1;
#endif
}

bool restart_self() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2)
        return false;
    single_instance_release();   // the new launcher must not find this one running
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    // an AppImage runs from a mount that goes when it exits: the new launcher starts from the AppImage file itself
    const char *appimage = SDL_getenv("APPIMAGE");
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (!appimage && n <= 0)
        return false;
    if (n > 0)
        self[n] = 0;
    std::string exe = appimage ? appimage : self;
    single_instance_release();
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(exe.c_str(), exe.c_str(), (char *)nullptr);
        _exit(127);
    }
    return pid > 0;
#endif
}

bool appimage_update(const std::string &downloaded, const std::string &root, std::string &err) {
#ifdef _WIN32
    (void)downloaded, (void)root;
    err = "not on Windows";
    return false;
#else
    // The game folder's UltraTears.AppImage is what Steam starts (--play) and what the launcher is: the new one takes its
    // place (a rename on the same disk: update/ is inside the folder; a running old one keeps its own file until it
    // exits) and is started, without arguments. Newer than the folder's package, it copies its builder into the folder
    // (tools/install_to.py) and offers to update the game, rebuilding what changed.
    std::string dst = path_join(root, "UltraTears.AppImage");
    if (chmod(downloaded.c_str(), 0755) != 0 || rename(downloaded.c_str(), dst.c_str()) != 0) {
        err = "UltraTears.AppImage: " + std::string(strerror(errno));
        SDL_Log("update: %s -> %s failed: %s", downloaded.c_str(), dst.c_str(), strerror(errno));
        return false;
    }
    SDL_Log("update: %s is the new UltraTears.AppImage, starting it", downloaded.c_str());
    single_instance_release();
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (chdir(root.c_str()) == 0)
            execl(dst.c_str(), dst.c_str(), (char *)nullptr);
        _exit(127);
    }
    if (pid < 0) {
        err = "fork: " + std::string(strerror(errno));
        return false;
    }
    return true;
#endif
}

std::string game_exe(const std::string &folder) {
#ifdef _WIN32
    std::string exe = path_join(folder, "UltraTears.exe");
    return path_is_file(exe) ? exe : "";
#else
    // a build on Linux links it into linux/ (tools/build_game.py); the Steam Deck's tar has it beside its data, and a
    // folder from the tar rebuilt on the Deck has both: the one built there is the current one
    for (const char *name : {"linux/ultratears", "ultratears"}) {
        std::string exe = path_join(folder, name);
        if (path_is_file(exe))
            return exe;
    }
    return "";
#endif
}

bool exec_game(const std::string &folder) {
#ifdef _WIN32
    (void)folder;
    return false;
#else
    std::string exe = game_exe(folder);
    if (exe.empty())
        return false;
    std::string dir = exe.substr(0, exe.find_last_of('/'));
    SDL_Log("play: %s in place of the launcher", exe.c_str());
    single_instance_release();   // the lock is close-on-exec anyway
    if (chdir(dir.c_str()) == 0)
        execl(exe.c_str(), exe.c_str(), (char *)nullptr);
    SDL_Log("play: exec failed (%s)", strerror(errno));
    return false;
#endif
}

bool start_game(const std::string &folder) {
    std::string exe = game_exe(folder);
    SDL_Log("play: %s", exe.empty() ? "no game in the folder" : exe.c_str());
    if (exe.empty())
        return false;
#ifdef _WIN32
    std::wstring cmd = L"\"" + wide(exe) + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, wide(folder).c_str(), &si, &pi)) {
        SDL_Log("play: CreateProcess failed (%lu)", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    std::string dir = exe.substr(0, exe.find_last_of('/'));   // the game reads its folder itself (/proc/self/exe)
    pid_t pid = fork();
    if (pid == 0) {
        setsid();   // not a child of the launcher's session: closing the launcher leaves the game running
        if (chdir(dir.c_str()) == 0)
            execl(exe.c_str(), exe.c_str(), (char *)nullptr);
        _exit(127);
    }
    return pid > 0;
#endif
}
