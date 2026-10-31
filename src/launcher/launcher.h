// UltraTears Launcher: checks the player's dump and PC, runs tools/setup.py --json with a progress window, starts the
// game. No code of the game in it. Design: 800x480 logical pixels (Nintendo/launcher-design), scaled as a whole.
#pragma once
#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include "imgui.h"

#define ULTRS_REPO_PATH "kuraeow/ultratears"
#define ULTRS_REPO "https://github.com/" ULTRS_REPO_PATH

// ---- json.cpp: the events of setup.py --json (one object a line)
struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;          // Arr: the values; Obj: the values of keys
    std::vector<std::string> keys;    // Obj
    const Json &operator[](const char *key) const;
    const Json &operator[](size_t i) const;
    size_t size() const { return items.size(); }
    std::string str(const char *def = "") const;   // a string, or a number / bool as text
    double num(double def = 0) const;
    bool truth() const { return kind == Bool ? b : kind == Num ? n != 0 : kind == Str ? !s.empty() : false; }
};
bool json_parse(const std::string &text, Json &out);

// ---- text.cpp: the words in the player's language
enum Lang { LANG_EN, LANG_RU, LANG_COUNT };
extern Lang g_lang;
const char *T(const char *key);
// T(key) with {name} replaced by the values: Tf("need_free", {{"need", "40"}, {"free", "86"}})
std::string Tf(const char *key, std::initializer_list<std::pair<const char *, std::string>> args);
std::string Tf(const char *key, const Json &args);
const char *lang_name(Lang l);
Lang lang_from_code(const char *code);   // "ru" / "en"
const char *lang_code(Lang l);
std::string fmt_duration(double secs);   // "3м 40с" / "3m 40s"
std::string fmt_minutes(double secs);    // "≈ 4 мин"
bool has_text(const char *key);

// ---- platform.cpp
std::string exe_dir();                                   // the launcher's folder, UTF-8, no trailing separator
std::string path_join(const std::string &a, const std::string &b);
bool path_is_dir(const std::string &p);
bool path_is_file(const std::string &p);
double disk_free_gb(const std::string &folder);           // of the drive the folder is (or would be) on; <0 unknown
std::string drive_of(const std::string &folder);
void reveal_in_folder(const std::string &file);            // a file manager with the file selected
bool single_instance(const char *title);                   // false: another launcher runs (brought to the front)
void window_dark_title(SDL_Window *w);
Lang system_lang();
int logical_cpus();
// ultratears-launcher.ini: lang ("" = the system's), dump (the folder with exefs and romfs), install (another folder
// than the launcher's), updates ("off": no questions to GitHub), update_checked (unix time), update_* (a release
// found). Which package the game was built by is not here but with the build (data/setup.json "package").
struct Settings {
    std::vector<std::pair<std::string, std::string>> kv;
    std::string get(const char *key) const;
    void set(const char *key, const std::string &value);
};
Settings settings_load();
void settings_save(const Settings &s);
bool start_game(const std::string &folder);                // UltraTears.exe in folder, detached
void single_instance_release();
bool restart_self();                                       // a new launcher started (after an update)

// ---- update.cpp: new builder packages from GitHub Releases
struct UpdateInfo { std::string version, name, url, sha256, page; uint64_t size = 0; };
std::string package_version(const std::string &root);      // VERSION.txt of the package ("" in a source tree)
bool version_newer(const std::string &a, const std::string &b);   // a > b, as x.y.z numbers
std::string package_version_line(const std::string &root); // "0.2.0 (commit abc1234)", as the game's log writes it
void update_check_start(const std::string &current);       // the newest release, on a thread
int update_check_poll(UpdateInfo *out);                    // 0 not yet, 1 a newer one, -1 none (or no answer)
void update_download_start(const UpdateInfo &u, const std::string &dir);
int update_download_poll(uint64_t *got, uint64_t *total, std::string *zip_or_error);   // 1 running, 2 done, 3 failed
void update_download_cancel();
void update_cleanup(const std::string &root);              // what the last update left (update\cleanup.txt)

// ---- setup_proc.cpp: tools/setup.py as a child process, its lines read by a thread
struct SetupProc;
SetupProc *setup_start(const std::vector<std::string> &args, const std::string &cwd, const std::string &log_path);
bool setup_line(SetupProc *p, std::string &line);          // the next line read, if any (never blocks)
void setup_answer(SetupProc *p, const char *text);         // a line to its stdin
bool setup_finished(SetupProc *p, int *exit_code);         // its process and every line read
void setup_kill(SetupProc *p);                              // it and everything it started
void setup_free(SetupProc *p);
std::string python_path(const std::string &root);         // deps/python/python.exe, else the one on PATH

// ---- media.cpp
struct Icon { SDL_Texture *tex = nullptr; float w = 0, h = 0; };   // w, h: logical size (design pixels)
struct Media {
    ImFont *regular = nullptr, *medium = nullptr, *small = nullptr, *title = nullptr, *brand = nullptr;
    ImFont *label = nullptr, *control = nullptr;   // the Play screen: setting names; tabs, choices, lists
    Icon emblem, pattern, check, error, folder, warn, chevron;
};
extern Media g_media;
void media_load(SDL_Renderer *r, float scale);   // fonts and icons at this scale (again when it changes)
float font_line(ImFont *f);                       // the line height of a font's text style, design pixels

// ---- ui.cpp
void ui_init(SDL_Window *w);
void ui_frame(float scale);
void ui_drop(const char *path);            // a folder dropped onto the window
bool ui_close_request();                   // the window's close button: true to close now (a build asks first)
void ui_demo(const char *name);             // development: a screen with sample data
void ui_quit();                            // closing: stop the build (it resumes next time)
