// tools/setup.py as a child process: its output read line by line on a thread (and written to the log), answers to
// its questions on its stdin. Windows: no console window, and a job object, so that Cancel stops setup and every tool
// it started (ninja, clang, the shader translator); the build resumes next time.
#include "launcher.h"
#include <deque>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

struct SetupProc {
    SDL_Mutex *lock = nullptr;
    SDL_Thread *reader = nullptr;
    std::deque<std::string> lines;
    std::string partial;
    SDL_IOStream *log = nullptr;
    bool eof = false;
    int exit_code = -1;
#ifdef _WIN32
    HANDLE process = nullptr, job = nullptr, out = nullptr, in = nullptr;
#else
    pid_t pid = -1;
    int out = -1, in = -1;
#endif
};

static void take(SetupProc *p, const char *buf, size_t n) {
    if (p->log)
        SDL_WriteIO(p->log, buf, n);
    SDL_LockMutex(p->lock);
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            if (!p->partial.empty() && p->partial.back() == '\r')
                p->partial.pop_back();
            p->lines.push_back(p->partial);
            p->partial.clear();
        } else {
            p->partial += buf[i];
        }
    }
    SDL_UnlockMutex(p->lock);
}

static int read_loop(void *arg) {
    SetupProc *p = (SetupProc *)arg;
    char buf[4096];
    for (;;) {
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(p->out, buf, sizeof buf, &n, nullptr) || n == 0)
            break;
#else
        ssize_t n = read(p->out, buf, sizeof buf);
        if (n <= 0)
            break;
#endif
        take(p, buf, (size_t)n);
    }
    SDL_LockMutex(p->lock);
    if (!p->partial.empty())
        p->lines.push_back(p->partial);
    p->partial.clear();
    p->eof = true;
    SDL_UnlockMutex(p->lock);
    if (p->log)
        SDL_FlushIO(p->log);
    return 0;
}

#ifdef _WIN32
static std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static std::wstring quote(const std::wstring &a) {   // CommandLineToArgvW's rules
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
        return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            slashes++;
            continue;
        }
        q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}
#endif

std::string python_path(const std::string &root) {
#ifdef _WIN32
    std::string own = path_join(root, "deps\\python\\python.exe");
#else
    std::string own = path_join(root, "deps/python/bin/python3");
#endif
    if (path_is_file(own))
        return own;
    const char *env = SDL_getenv("ULTRS_PYTHON");   // a source checkout: the developer's Python 3.14
    return env && *env ? env : "";
}

SetupProc *setup_start(const std::vector<std::string> &args, const std::string &cwd, const std::string &log_path) {
    SetupProc *p = new SetupProc;
    p->lock = SDL_CreateMutex();
    std::string line;
    for (const std::string &a : args)
        line += (line.empty() ? "" : " ") + a;
    SDL_Log("builder: %s (in %s, its log %s)", line.c_str(), cwd.c_str(), log_path.c_str());
    if (!log_path.empty()) {
        std::string prev = log_path.substr(0, log_path.size() - 4) + "-previous.log";
        SDL_RemovePath(prev.c_str());
        SDL_RenamePath(log_path.c_str(), prev.c_str());
        p->log = SDL_IOFromFile(log_path.c_str(), "wb");
    }
#ifdef _WIN32
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE out_r, out_w, in_r, in_w;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&in_r, &in_w, &sa, 0)) {
        p->eof = true;
        return p;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd;
    for (const std::string &a : args)
        cmd += (cmd.empty() ? L"" : L" ") + quote(wide(a));
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi;
    // a hidden console: setup's tools (ninja, clang) inherit it instead of each opening a window
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             wide(cwd).c_str(), &si, &pi);
    CloseHandle(out_w);
    CloseHandle(in_r);
    if (!ok) {
        CloseHandle(out_r);
        CloseHandle(in_w);
        std::string msg = "cannot start: " + args[0] + "\n";
        take(p, msg.data(), msg.size());
        p->eof = true;
        return p;
    }
    p->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;   // the launcher gone: setup too
    SetInformationJobObject(p->job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    AssignProcessToJobObject(p->job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    p->out = out_r;
    p->in = in_w;
#else
    setenv("PYTHONIOENCODING", "utf-8", 1);
    setenv("PYTHONUNBUFFERED", "1", 1);
    int o[2], i[2];
    if (pipe(o) != 0 || pipe(i) != 0) {
        p->eof = true;
        return p;
    }
    // made before fork: the child of a process with threads may only make async-signal-safe calls until exec (no malloc)
    std::vector<char *> argv;
    for (const std::string &a : args)
        argv.push_back((char *)a.c_str());
    argv.push_back(nullptr);
    p->pid = fork();
    if (p->pid == 0) {
        setsid();   // its own process group: kill() reaches the tools it starts
        dup2(i[0], 0);
        dup2(o[1], 1);
        dup2(o[1], 2);
        close(o[0]), close(o[1]), close(i[0]), close(i[1]);
        if (chdir(cwd.c_str()) != 0)
            _exit(127);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(o[1]);
    close(i[0]);
    p->out = o[0];
    p->in = i[1];
#endif
    p->reader = SDL_CreateThread(read_loop, "setup-output", p);
    return p;
}

bool setup_line(SetupProc *p, std::string &line) {
    SDL_LockMutex(p->lock);
    bool got = !p->lines.empty();
    if (got) {
        line = std::move(p->lines.front());
        p->lines.pop_front();
    }
    SDL_UnlockMutex(p->lock);
    return got;
}

void setup_answer(SetupProc *p, const char *text) {
    std::string s = std::string(text) + "\n";
#ifdef _WIN32
    DWORD n;
    if (p->in)
        WriteFile(p->in, s.data(), (DWORD)s.size(), &n, nullptr);
#else
    if (p->in >= 0 && write(p->in, s.data(), s.size()) < 0) {
    }
#endif
}

bool setup_finished(SetupProc *p, int *exit_code) {
    SDL_LockMutex(p->lock);
    bool done = p->eof && p->lines.empty();
    SDL_UnlockMutex(p->lock);
    if (!done)
        return false;
#ifdef _WIN32
    if (p->process) {
        if (WaitForSingleObject(p->process, 0) != WAIT_OBJECT_0)
            return false;
        DWORD code = 1;
        GetExitCodeProcess(p->process, &code);
        p->exit_code = (int)code;
        CloseHandle(p->process);
        p->process = nullptr;
    }
#else
    if (p->pid > 0) {
        int st = 0;
        pid_t w = waitpid(p->pid, &st, WNOHANG);
        if (w == 0)
            return false;
        p->exit_code = w == p->pid && WIFEXITED(st) ? WEXITSTATUS(st) : 1;   // -1: its status is lost, not a success
        SDL_Log("builder ended: exit %d%s", p->exit_code, w == p->pid ? "" : " (status lost)");
        p->pid = -1;
    }
#endif
    if (exit_code)
        *exit_code = p->exit_code;
    return true;
}

void setup_kill(SetupProc *p) {
#ifdef _WIN32
    if (p->job)
        TerminateJobObject(p->job, 1);
#else
    if (p->pid > 0)
        kill(-p->pid, SIGTERM);
#endif
}

void setup_free(SetupProc *p) {
    if (!p)
        return;
    setup_kill(p);
    if (p->reader)
        SDL_WaitThread(p->reader, nullptr);
#ifdef _WIN32
    for (HANDLE h : {p->process, p->job, p->out, p->in})
        if (h)
            CloseHandle(h);
#else
    if (p->pid > 0)
        waitpid(p->pid, nullptr, 0);
    if (p->out >= 0)
        close(p->out);
    if (p->in >= 0)
        close(p->in);
#endif
    if (p->log)
        SDL_CloseIO(p->log);
    SDL_DestroyMutex(p->lock);
    delete p;
}
