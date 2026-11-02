// Updates of the builder package from GitHub Releases: one question at every start (ultratears-launcher.ini, updates=off
// turns it off; no answer, no network: nothing is shown), the newest release's builder zip (Windows) or AppImage (Linux) downloaded into update\ of the package,
// its size and SHA-256 (the digest GitHub keeps for every release file) checked before tools/apply_update.py puts the
// zip in place, or the AppImage takes UltraTears.AppImage's place (platform.cpp appimage_update).
// HTTP: WinHTTP on Windows, the system's libcurl on Linux; SHA-256: BCrypt on Windows, its own on Linux.
#include "launcher.h"
#include <atomic>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#include <mutex>
#endif

#ifdef _WIN32
static std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif

// GET url (https, redirects followed); sink gets the body in pieces and returns false to stop. total: Content-Length.
template <class Sink> static bool http_get(const std::string &url, Sink sink, uint64_t *total, std::string &err) {
#ifdef _WIN32
    std::wstring w = wide(url);
    URL_COMPONENTS u = {sizeof u};
    wchar_t host[256], path[2048];
    u.lpszHostName = host, u.dwHostNameLength = 256;
    u.lpszUrlPath = path, u.dwUrlPathLength = 2048;
    wchar_t extra[2048];
    u.lpszExtraInfo = extra, u.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &u)) {
        err = "bad url";
        return false;
    }
    std::wstring object = std::wstring(path, u.dwUrlPathLength) + std::wstring(extra, u.dwExtraInfoLength);
    HINTERNET s = WinHttpOpen(L"UltraTears-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!s)   // before Windows 8.1
        s = WinHttpOpen(L"UltraTears-Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, std::wstring(host, u.dwHostNameLength).c_str(), u.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", object.c_str(), nullptr, nullptr, nullptr,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len, nullptr);
        wchar_t cl[32];
        DWORD cl_len = sizeof cl;
        if (total)
            *total = WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, cl, &cl_len, nullptr) ? _wcstoui64(cl, nullptr, 10) : 0;
        if (status != 200) {
            err = "HTTP " + std::to_string(status);
        } else {
            ok = true;
            std::vector<char> buf(1 << 16);
            for (;;) {
                DWORD n = 0;
                if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n)) {
                    ok = false, err = "connection lost (" + std::to_string(GetLastError()) + ")";
                    break;
                }
                if (n == 0)
                    break;
                if (!sink(buf.data(), (size_t)n)) {
                    ok = false, err = "stopped";
                    break;
                }
            }
        }
    } else {
        err = "no connection (" + std::to_string(GetLastError()) + ")";
    }
    for (HINTERNET h : {r, c, s})
        if (h)
            WinHttpCloseHandle(h);
    return ok;
#else
    // libcurl of the system, loaded when first needed (SteamOS and nearly every distribution have libcurl.so.4; Debian's
    // GnuTLS build is libcurl-gnutls.so.4): no headers needed, the few calls and their option numbers declared here
    static void *lib = dlopen("libcurl.so.4", RTLD_NOW);
    if (!lib)
        lib = dlopen("libcurl-gnutls.so.4", RTLD_NOW);
    if (!lib) {
        err = "libcurl is missing on this system";
        return false;
    }
    using easy_init_t = void *(*)();
    using easy_setopt_t = int (*)(void *, int, ...);
    using easy_perform_t = int (*)(void *);
    using easy_getinfo_t = int (*)(void *, int, ...);
    using easy_cleanup_t = void (*)(void *);
    using easy_strerror_t = const char *(*)(int);
    using slist_append_t = void *(*)(void *, const char *);
    using slist_free_t = void (*)(void *);
    using global_init_t = int (*)(long);
    auto easy_init = (easy_init_t)dlsym(lib, "curl_easy_init");
    auto easy_setopt = (easy_setopt_t)dlsym(lib, "curl_easy_setopt");
    auto easy_perform = (easy_perform_t)dlsym(lib, "curl_easy_perform");
    auto easy_getinfo = (easy_getinfo_t)dlsym(lib, "curl_easy_getinfo");
    auto easy_cleanup = (easy_cleanup_t)dlsym(lib, "curl_easy_cleanup");
    auto easy_strerror = (easy_strerror_t)dlsym(lib, "curl_easy_strerror");
    auto slist_append = (slist_append_t)dlsym(lib, "curl_slist_append");
    auto slist_free = (slist_free_t)dlsym(lib, "curl_slist_free_all");
    auto global_init = (global_init_t)dlsym(lib, "curl_global_init");
    if (!easy_init || !easy_setopt || !easy_perform || !easy_getinfo || !easy_cleanup || !easy_strerror ||
        !slist_append || !slist_free || !global_init) {
        err = "libcurl lacks a function the launcher uses";
        return false;
    }
    static std::once_flag once;   // curl_global_init is not thread-safe: once, from whichever thread asks first
    std::call_once(once, [&] { global_init(3); });   // CURL_GLOBAL_DEFAULT
    enum { URL = 10002, WRITEDATA = 10001, WRITEFUNCTION = 20011, USERAGENT = 10018, HTTPHEADER = 10023,
           FOLLOWLOCATION = 52, FAILONERROR = 45, CONNECTTIMEOUT = 78, NOSIGNAL = 99 };
    enum { RESPONSE_CODE = 0x200002, CONTENT_LENGTH_DOWNLOAD_T = 0x60000F };
    struct Ctx { Sink *sink; uint64_t *total; void *h; easy_getinfo_t getinfo; bool stopped; } ctx = {&sink, total, nullptr, easy_getinfo, false};
    void *h = easy_init();
    if (!h) {
        err = "libcurl did not start";
        return false;
    }
    ctx.h = h;
    if (total)
        *total = 0;
    void *headers = slist_append(nullptr, "Accept: application/vnd.github+json");
    size_t (*write)(char *, size_t, size_t, void *) = [](char *p, size_t size, size_t n, void *u) -> size_t {
        Ctx *c = (Ctx *)u;
        if (c->total && !*c->total) {   // the final answer's length, known with its headers (after redirects)
            int64_t len = -1;
            c->getinfo(c->h, CONTENT_LENGTH_DOWNLOAD_T, &len);
            *c->total = len > 0 ? (uint64_t)len : 0;
        }
        if (!(*c->sink)(p, size * n)) {
            c->stopped = true;
            return 0;   // ends the transfer
        }
        return size * n;
    };
    easy_setopt(h, URL, url.c_str());
    easy_setopt(h, USERAGENT, "UltraTears-Launcher");
    easy_setopt(h, HTTPHEADER, headers);
    easy_setopt(h, FOLLOWLOCATION, 1L);
    easy_setopt(h, FAILONERROR, 1L);
    easy_setopt(h, CONNECTTIMEOUT, 20L);
    easy_setopt(h, NOSIGNAL, 1L);   // on a thread of its own
    easy_setopt(h, WRITEFUNCTION, write);
    easy_setopt(h, WRITEDATA, &ctx);
    int rc = easy_perform(h);
    long status = 0;
    easy_getinfo(h, RESPONSE_CODE, &status);
    bool ok = rc == 0;
    if (!ok)
        err = ctx.stopped ? "stopped" : status >= 400 ? "HTTP " + std::to_string(status) : std::string(easy_strerror(rc));
    slist_free(headers);
    easy_cleanup(h);
    return ok;
#endif
}

// "0.3.0" from "v0.3.0-beta" / "UltraTears 0.3.0 (commit …)"; compared as numbers
static bool parse_version(const std::string &s, int v[3]) {
    size_t i = s.find_first_of("0123456789");
    if (i == std::string::npos)
        return false;
    return SDL_sscanf(s.c_str() + i, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}

static bool newer(const std::string &a, const std::string &b) {   // a > b
    int x[3], y[3];
    if (!parse_version(a, x) || !parse_version(b, y))
        return false;
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i])
            return x[i] > y[i];
    return false;
}

bool version_newer(const std::string &a, const std::string &b) { return newer(a, b); }

std::string package_version_line(const std::string &root) {
    size_t n = 0;
    char *t = (char *)SDL_LoadFile(path_join(root, "VERSION.txt").c_str(), &n);
    if (!t)
        return "";
    std::string s(t, n);
    SDL_free(t);
    if (s.rfind("UltraTears ", 0) != 0)
        return "";
    s = s.substr(11);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

std::string package_version(const std::string &root) {
    size_t n = 0;
    char *t = (char *)SDL_LoadFile(path_join(root, "VERSION.txt").c_str(), &n);
    if (!t)
        return "";
    std::string s(t, n);
    SDL_free(t);
    int v[3];
    if (s.rfind("UltraTears ", 0) != 0 || !parse_version(s, v))
        return "";
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]);
}

// ---------------------------------------------------------------- the question to GitHub, on a thread
static struct {
    SDL_Mutex *lock = SDL_CreateMutex();
    std::atomic<int> check{0};   // 0 idle, 1 asking, 2 answered
    UpdateInfo found;
    bool has = false;
    std::string current;
    // the download
    std::atomic<int> dl{0};      // 0 idle, 1 running, 2 done, 3 failed
    std::atomic<uint64_t> got{0}, total{0};
    std::atomic<bool> stop{false};
    std::string error, zip;
    UpdateInfo want;
    std::string dir;
} G;

static int check_thread(void *) {
    std::string body, err;
    bool ok = http_get(std::string("https://api.github.com/repos/") + ULTRS_REPO_PATH + "/releases?per_page=10",
                       [&](const void *p, size_t n) { body.append((const char *)p, n); return body.size() < (8u << 20); },
                       nullptr, err);
    Json list;
    UpdateInfo best;
    if (ok && json_parse(body, list)) {
        for (size_t i = 0; i < list.size(); i++) {   // newest first; drafts and pre-releases are not offered
            const Json &r = list[i];
            if (r["draft"].truth() || r["prerelease"].truth())
                continue;
            std::string tag = r["tag_name"].str();
            if (!newer(tag, G.current) || (best.version.size() && !newer(tag, best.version)))
                continue;
            const Json &assets = r["assets"];
            for (size_t k = 0; k < assets.size(); k++) {
                std::string name = assets[k]["name"].str(), digest = assets[k]["digest"].str();
#ifdef _WIN32
                // UltraTears-<v>-windows-x86_64.zip (from 0.2.0; before, UltraTears-builder-<v>-windows-x86_64.zip)
                bool mine = name.rfind("UltraTears-", 0) == 0 && name.size() > 19 &&
                            name.compare(name.size() - 19, 19, "-windows-x86_64.zip") == 0;
#else
                // the AppImage (decision 2026-11-01): put in the game folder in place of UltraTears.AppImage and
                // started, it brings the folder up to date itself (appimage_update)
                // named UltraTears.AppImage as the game folder has it (from 0.2.0), or UltraTears-<v>-x86_64.AppImage
                bool mine = name == "UltraTears.AppImage" ||
                            (name.rfind("UltraTears-", 0) == 0 && name.size() > 16 && name.compare(name.size() - 16, 16, "-x86_64.AppImage") == 0);
#endif
                if (!mine || digest.rfind("sha256:", 0) != 0)   // no hash, no update
                    continue;
                int v[3];
                parse_version(tag, v);
                best.version = std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]);
                best.name = name;
                best.url = assets[k]["browser_download_url"].str();
                best.size = (uint64_t)assets[k]["size"].num();
                best.sha256 = digest.substr(7);
                best.page = r["html_url"].str();
            }
        }
    }
    SDL_LockMutex(G.lock);
    G.found = best;
    G.has = !best.version.empty();
    SDL_UnlockMutex(G.lock);
    if (!ok)
        SDL_Log("update check: %s", err.c_str());
    G.check = 2;
    return 0;
}

void update_check_start(const std::string &current) {
    if (G.check == 1 || current.empty())
        return;
    G.current = current;
    G.check = 1;
    SDL_DetachThread(SDL_CreateThread(check_thread, "update-check", nullptr));
}

int update_check_poll(UpdateInfo *out) {
    if (G.check != 2)
        return 0;
    G.check = 0;
    SDL_LockMutex(G.lock);
    bool has = G.has;
    if (has && out)
        *out = G.found;
    SDL_UnlockMutex(G.lock);
    return has ? 1 : -1;
}

// ---------------------------------------------------------------- the download
#ifdef _WIN32
struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    Sha256() {
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    }
    void add(const void *p, size_t n) { BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0); }
    std::string hex() {
        unsigned char d[32];
        BCryptFinishHash(h, d, 32, 0);
        char s[65];
        for (int i = 0; i < 32; i++)
            SDL_snprintf(s + 2 * i, 3, "%02x", d[i]);
        return s;
    }
    ~Sha256() {
        if (h)
            BCryptDestroyHash(h);
        if (alg)
            BCryptCloseAlgorithmProvider(alg, 0);
    }
};
#else
struct Sha256 {   // FIPS 180-4, for the downloaded package's digest
    uint32_t s[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf[64];
    size_t used = 0;
    uint64_t bytes = 0;
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char *p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
            0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
            0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++)
            w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
                   (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
        uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        s[0] += a, s[1] += b, s[2] += c, s[3] += d, s[4] += e, s[5] += f, s[6] += g, s[7] += h;
    }
    void add(const void *data, size_t n) {
        const unsigned char *p = (const unsigned char *)data;
        bytes += n;
        while (n) {
            size_t k = SDL_min(n, 64 - used);
            SDL_memcpy(buf + used, p, k);
            used += k, p += k, n -= k;
            if (used == 64)
                block(buf), used = 0;
        }
    }
    std::string hex() {
        uint64_t bits = bytes * 8;
        unsigned char pad = 0x80, zero = 0;
        add(&pad, 1);
        while (used != 56)
            add(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; i++)
            len[i] = (unsigned char)(bits >> (56 - 8 * i));
        add(len, 8);
        char out[65];
        for (int i = 0; i < 32; i++)
            SDL_snprintf(out + 2 * i, 3, "%02x", (s[i / 4] >> (24 - 8 * (i % 4))) & 0xff);
        return out;
    }
};
#endif

static int download_thread(void *) {
    std::string part = path_join(G.dir, G.want.name + ".part"), zip = path_join(G.dir, G.want.name);
    SDL_CreateDirectory(G.dir.c_str());
    SDL_IOStream *f = SDL_IOFromFile(part.c_str(), "wb");
    std::string err;
    bool ok = false;
    if (!f) {
        err = SDL_GetError();
    } else {
        Sha256 sha;
        uint64_t total = 0;
        ok = http_get(G.want.url, [&](const void *p, size_t n) {
            if (G.stop || SDL_WriteIO(f, p, n) != n)
                return false;
            sha.add(p, n);
            G.got += n;
            return true;
        }, &total, err);
        if (!SDL_CloseIO(f) && ok)
            ok = false, err = "cannot write " + part;
        if (ok && G.got != G.want.size)
            ok = false, err = "the size is wrong: " + std::to_string((uint64_t)G.got) + " bytes, expected " + std::to_string(G.want.size);
        if (ok) {
            std::string h = sha.hex();
            if (h != G.want.sha256)
                ok = false, err = "the SHA-256 is wrong: " + h;
        }
        if (ok && !SDL_RenamePath(part.c_str(), zip.c_str()))
            ok = false, err = SDL_GetError();
        if (!ok)
            SDL_RemovePath(part.c_str());
    }
    SDL_LockMutex(G.lock);
    G.error = err;
    G.zip = zip;
    SDL_UnlockMutex(G.lock);
    G.dl = ok ? 2 : 3;
    return 0;
}

void update_download_start(const UpdateInfo &u, const std::string &dir) {
    if (G.dl == 1)
        return;
    G.want = u, G.dir = dir;
    G.got = 0, G.total = u.size, G.stop = false;
    G.dl = 1;
    SDL_DetachThread(SDL_CreateThread(download_thread, "update-download", nullptr));
}

int update_download_poll(uint64_t *got, uint64_t *total, std::string *zip_or_error) {
    if (got)
        *got = G.got;
    if (total)
        *total = G.total;
    int st = G.dl;
    if (st >= 2 && zip_or_error) {
        SDL_LockMutex(G.lock);
        *zip_or_error = st == 2 ? G.zip : G.error;
        SDL_UnlockMutex(G.lock);
    }
    return st;
}

void update_download_cancel() { G.stop = true; }

// The files apply_update.py moved aside (in use while it ran) and the downloaded zip: deleted at the next start.
void update_cleanup(const std::string &root) {
    std::string dir = path_join(root, "update"), list = path_join(dir, "cleanup.txt");
    size_t n = 0;
    char *t = (char *)SDL_LoadFile(list.c_str(), &n);
    std::string left;
    if (t) {
        std::string s(t, n);
        SDL_free(t);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t e = s.find('\n', pos);
            if (e == std::string::npos)
                e = s.size();
            std::string p = s.substr(pos, e - pos);
            pos = e + 1;
            if (!p.empty() && p.back() == '\r')
                p.pop_back();
            if (p.empty() || p.find(".old-update") == std::string::npos)   // only what apply_update.py moved aside
                continue;
            for (int i = 0; i < 20 && path_is_file(p) && !SDL_RemovePath(p.c_str()); i++)
                SDL_Delay(100);   // the launcher that restarted us may still be closing
            if (path_is_file(p))
                left += p + "\n";
        }
    }
    if (left.empty()) {
        SDL_RemovePath(list.c_str());
        SDL_EnumerateDirectory(dir.c_str(), [](void *d, const char *, const char *name) {
            std::string f = path_join(*(std::string *)d, name);
            if (path_is_file(f) && (SDL_strstr(name, ".zip") || SDL_strstr(name, ".AppImage") || SDL_strstr(name, ".log")))
                SDL_RemovePath(f.c_str());
            return SDL_ENUM_CONTINUE;
        }, &dir);
        SDL_RemovePath(dir.c_str());   // only when empty
    } else {
        SDL_SaveFile(list.c_str(), left.data(), left.size());
    }
}
