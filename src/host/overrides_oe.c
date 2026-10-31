/* Host replacements of SDK functions (application environment, add-on content, error/parental-control/diagnostics
 * setup), bound by symbol name. Ported from MacNativeGames games/totk/runtime/services.c runtime_override() @b75be8c
 * (MIT): the same behavior, written against the guest registers (AAPCS64: x0.. integer arguments and results). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifdef _WIN32
#include <windows.h>
#endif
#else
#ifndef LOCALE_NAME_MAX_LENGTH
#define LOCALE_NAME_MAX_LENGTH 85
#endif
#endif

#include "host.h"
#include "../runtime/hle.h"

/* TOTK_OPERATION_MODE=handheld reports the Switch handheld mode (oe OperationMode 0, PerformanceMode Normal 0) instead
   of docked (1, Boost 1): the game then uses its own handheld settings (1280x720 output, lower internal resolution). */
static int handheld_mode(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_OPERATION_MODE");
        if (!e || !strcmp(e, "docked")) v = 0;
        else if (!strcmp(e, "handheld")) v = 1;
        else runtime_fail("TOTK_OPERATION_MODE must be docked or handheld");
        fprintf(stderr, "[totk] operation mode reported to the game: %s\n", v ? "handheld (performance normal)" : "docked (performance boost)");
    }
    return v;
}
int runtime_handheld_mode(void) { return handheld_mode(); }

/* The game's language: TOTK_LANG as given (a Switch language code), otherwise from TOTK_LOCALE or the user's
   Windows UI language. Brazil maps to "pt-BR" in 1.4.3, whose RomFS has the official Brazilian text. */
static uint64_t desired_language(void) {
    const char *explicit_code = getenv("TOTK_LANG");
    char code[8] = {0};
    if (explicit_code) {
        if (strlen(explicit_code) > 7) runtime_fail("language code too long");
        memcpy(code, explicit_code, strlen(explicit_code));
    } else {
        char tag[LOCALE_NAME_MAX_LENGTH] = {0};
        const char *env = getenv("TOTK_LOCALE");
        if (env && *env) snprintf(tag, sizeof tag, "%s", env);
#ifdef _WIN32
        else {
            wchar_t w[LOCALE_NAME_MAX_LENGTH];
            if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH)) WideCharToMultiByte(CP_UTF8, 0, w, -1, tag, sizeof tag, NULL, NULL);
        }
#endif
        char language[16] = {0}, region[4] = {0};
        size_t n = strcspn(tag, "-_");
        if (n < sizeof language) memcpy(language, tag, n);
        for (const char *p = tag + n; *p;) {
            p += strspn(p, "-_");
            size_t k = strcspn(p, "-_");
            if (k == 2 && isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1])) {
                region[0] = (char)toupper((unsigned char)p[0]);
                region[1] = (char)toupper((unsigned char)p[1]);
            }
            p += k;
        }
        const char *portuguese = !strcmp(region, "BR") ? "pt-BR" : "pt";
        const char *selected = !strcmp(language, "ja") ? "ja" : !strcmp(language, "de") ? "de" : !strcmp(language, "fr") ? "fr"
            : !strcmp(language, "es") ? "es" : !strcmp(language, "it") ? "it" : !strcmp(language, "nl") ? "nl"
            : !strcmp(language, "ru") ? "ru" : !strcmp(language, "ko") ? "ko" : !strcmp(language, "zh") ? "zh-Hans"
            : !strcmp(language, "pt") ? portuguese : "en-US";
        memcpy(code, selected, strlen(selected));
    }
    uint64_t result;
    memcpy(&result, code, 8);
    return result;
}

static int focus_policy;
// What the game asked to be told; kept for when the port sends these (the mode never changes yet). Unused till then.
__attribute__((unused)) static unsigned resume_notification, operation_notification, performance_notification;
static uint32_t performance_configuration[2];

HLE_FUNC("_ZN2nn2oe18GetDesiredLanguageEv", oe_desired_language) { c->x[0] = desired_language(); }
HLE_FUNC("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", oe_performance_config) {
    int mode = (int)c->x[0];
    if (mode < 0 || mode > 1) runtime_fail("invalid performance mode");
    performance_configuration[mode] = (uint32_t)c->x[1];
    fprintf(stderr, "[totk] performance request mode=%d cfg=%x: host scheduling\n", mode, (uint32_t)c->x[1]);
}
HLE_FUNC("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", oe_cpu_boost) {
    if ((int)c->x[0] < 0 || (int)c->x[0] > 2) runtime_fail("unknown CPU boost mode");
}
HLE_FUNC("_ZN2nn2oe16GetOperationModeEv", oe_operation_mode) { c->x[0] = handheld_mode() ? 0 : 1; }
HLE_FUNC("_ZN2nn2oe18GetPerformanceModeEv", oe_performance_mode) { c->x[0] = handheld_mode() ? 0 : 1; }
HLE_FUNC("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", oe_focus_mode) {
    int mode = (int)c->x[0];
    if (mode < 0 || mode > 3) runtime_fail("unknown focus handling mode");
    focus_policy = mode;
    fprintf(stderr, "[totk] focus policy stored: %d (window lifecycle pending)\n", focus_policy);
}
HLE_FUNC("_ZN2nn2oe20GetCurrentFocusStateEv", oe_focus_state) { c->x[0] = 1; }   /* InFocus (no window yet) */
HLE_FUNC("_ZN2nn2oe28SetResumeNotificationEnabledEb", oe_resume_enabled) { resume_notification = c->x[0] & 255; }
HLE_FUNC("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", oe_operation_enabled) { operation_notification = c->x[0] & 255; }
HLE_FUNC("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", oe_performance_enabled) { performance_notification = c->x[0] & 255; }
HLE_FUNC("nndiagStartup", host_diag_init) { (void)c; fprintf(stderr, "[totk] native diagnostics initialized\n"); }
HLE_FUNC("_ZN2nn2oe10InitializeEv", host_oe_init) { (void)c; fprintf(stderr, "[totk] application environment: local Windows process\n"); }
HLE_FUNC("_ZN2nn3aoc10InitializeEv", host_aoc_init) { (void)c; fprintf(stderr, "[totk] add-on content: no DLC\n"); }
HLE_FUNC("_ZN2nn3aoc17CountAddOnContentEv", host_aoc_count) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3aoc16ListAddOnContentEPiii", host_aoc_list) {
    if ((int)c->x[1] < 0 || (int)c->x[2] < 0) runtime_fail("invalid add-on content list range");
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3err26InitializeContextWriterApiEv", host_error_init) { (void)c; fprintf(stderr, "[totk] error contexts: local diagnostics\n"); }
HLE_FUNC("_ZN2nn4pctl10InitializeEv", host_pctl_init) { (void)c; fprintf(stderr, "[totk] local application parental controls initialized\n"); }
HLE_FUNC("_ZN2nn4diag34InitializeApplicationAbortObserverEv", host_abort_init) { (void)c; fprintf(stderr, "[totk] native crash observer initialized\n"); }
