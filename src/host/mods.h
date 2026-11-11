/* Player mods (mods.c): one folder per mod in mods/ beside the game (ultratears.cfg mods_dir=), laid out as the
 * Switch emulators take them: <mod>/romfs, <mod>/exefs (.ips / .pchtxt code and data patches), <mod>/cheats
 * (Atmosphere cheat files), also under atmosphere/contents/<title id>/ or contents/<title id>/ or <title id>/ inside the
 * mod's folder, and atmosphere/exefs_patches/<name>/. Order and switches: ultratears.cfg mods_order= (names, top
 * first, separated by |), mods_off= (names switched off), cheats_on= (cheat ids, see mods_cheat_id). The top mod wins a
 * file or a patched byte that two mods give.
 *
 * Nothing about a mod is built into the game: RomFS files are served from the mod's folder when the game opens them
 * (fs.c), data patches are stored into the loaded module before it is relocated, data cheats run in a cheat VM at every
 * present. Patches and cheats that change the game's code need the game's code for those functions translated again:
 * tools/mods_code.py (the launcher runs it when the patch set changes, a few seconds) builds data/mods/code.dll with
 * the changed functions, each changed instruction behind its mod's or cheat's switch; mods_code_load puts them in place
 * of the built ones. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char name[256];          // folder name (UTF-8)
    char romfs[4096];        // its RomFS root, "" none
    char exefs[4096];        // its folder(s) of ExeFS patches, "" none
    char cheats[4096];       // its cheats folder, "" none
    int enabled;
    uint32_t replaced, added, other_version, overridden;   // RomFS files (fs.c)
    uint32_t data_patches, code_patches, other_build;      // ExeFS patch files' writes (bytes / words) and files for another build
    uint32_t cheats_on, cheats_total;
    int code_active;         // its code patches are in the loaded code.dll
} Mod;

void mods_scan(void);              // once; later calls return at once
int mods_count(void);
Mod *mods_at(int i);               // 0 = the top of the order
const char *mods_dir(void);

/* RomFS: every file of the enabled mods' RomFS roots, the top mod's where two give the same path, sorted by path.
   The path is the RomFS spelling (a mod path that differs only by case takes the RomFS's when has() knows it). */
typedef struct { const char *rel; const char *full; uint64_t size; int mod; } ModFile;
size_t mods_romfs_files(const ModFile **files, int (*has)(const char *rel, const char **spelling, void *ctx), void *ctx);
int mods_open_file(const char *full);   // read-only fd of a mod file (UTF-8 path), -1 on failure

/* ExeFS: the enabled mods' patches for module <name> with build id bid (32 bytes), applied to its image before it is
   relocated (loader.c). text_off/text_size: the module's code; writes there are code patches (kept for the log and the
   check of code.dll), writes elsewhere data patches. */
void mods_patch_module(const char *name, uint8_t *image, uint64_t size, uint64_t text_off, uint64_t text_size,
                       const uint8_t bid[32]);
// After dispatch_init: the code built by tools/mods_code.py in place of the game's functions, the switches set.
void mods_code_load(void);
// Every present (frame_rate.c): the enabled data cheats run once.
void mods_cheats_frame(void);
uint32_t mods_cheat_id(const char *mod, const char *cheat);   // FNV-1a 32 of "<mod>/<cheat name>"

/* restbl.c: the game's resource size table rebuilt for the mods' files (fs.c), compressed, or NULL (the game's as it
   is). game: the game's compressed table; shipped[k]: the compressed tables the mods ship; files[k]: each mod file
   served (RomFS path, its first bytes for the zstd header, its size; the game's copy's first bytes and size, orig_size
   0 when the game lacks it). read(rel, mod, ...) gives a whole file (malloc) for the packs: mod 1 the mod's, 0 the
   game's. dicts_z: Pack/ZsDic.pack.zs, the dictionaries the packs are compressed with. */
typedef struct {
    const char *rel;
    const uint8_t *head; size_t head_n; uint64_t size;
    const uint8_t *orig_head; size_t orig_head_n; uint64_t orig_size;
} RestblFile;
uint8_t *totk_restbl_build(const uint8_t *game, size_t game_n, const uint8_t *const *shipped, const size_t *shipped_n,
                           int nshipped, const RestblFile *files, size_t nfiles,
                           void *(*read)(const char *rel, int mod, size_t *n, void *ctx), void *ctx,
                           const uint8_t *dicts_z, size_t dicts_zn, size_t *out_n);
