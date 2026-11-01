#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define LOG(...) do {fprintf(stderr,"[totk-gfx] " __VA_ARGS__);fputc('\n',stderr);}while(0)
// TOTK_STATS=1 (or TOTK_MEMMAP): the periodic statistics lines (every 300 frames and the like) - off for players, whose
// log keeps the errors, the long frames and the start-up lines.
static inline int dm_stats(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_STATS"); v = (e && *e && *e != '0') || getenv("TOTK_MEMMAP") != NULL; }
    return v;
}
void plat_fatal_notice(void);   // runtime/platform.h: the error in a message box (no console), why when out of memory
#define die(...) do {LOG("FATAL: " __VA_ARGS__);fflush(stderr);plat_fatal_notice();exit(2);}while(0)
typedef struct {const char *name;uint8_t *base;size_t size;} module_t;
module_t *module_of(const void *p);
uint64_t now_ns(void);uint64_t guest_ns(void);
int dm_detclock_enabled(void);
void dm_fault_hook_register(int (*fn)(void *,int,void *));
void dm_guest_write_prep_register(void (*fn)(void *,size_t));
void dm_res_report_register(void (*fn)(uint32_t,const char *));
void dm_guest_write_prepare(void *addr,size_t n);
void crash_set_wait_dump(void (*fn)(void));
void dm_guest_frame_tick(void);
void dm_shot_written(const char *path,int ok);
