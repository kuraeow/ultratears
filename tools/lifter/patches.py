"""Per-instruction replacements applied by gen.py: guest address -> function(gen, addr) returning C code.

Every patch checks the original instruction word first, so lifting another game version (or a different build)
fails loudly instead of changing the wrong code. Runtime switches live in src/gfx/nvn/hle_nvn.c.
"""


def expect(gen, addr, word, what):
    got = gen.word(addr)
    if got != word:
        raise SystemExit(f'patch {what} at {addr:#x}: expected {word:#010x}, found {got:#010x} (not TOTK 1.4.3?)')


def drs_off(gen, addr):
    """Dynamic resolution off (1.4.3). main+0x584300 is the scene resolution controller: each frame it fits the
    recent frame times and returns {scale (float), state} to the renderer (main+0x581968), which turns the scale into
    the scene size (+0xccc). Its first test is the controller's enable bit (+0x110 bit 0); with the bit clear it
    takes its own disabled path (main+0x584c3c): scale 1.0, history untouched. The patch takes that path unless
    TOTK_DRS=1. Why: the game flips the scene targets between 1600x896 and 1432x808 over the same memory and our
    per-size images then hand stale content to the other size (terrain gone, clouds over the scene);
    image quality is better at a fixed size and upscaling will come from FSR."""
    expect(gen, addr, 0x360042e8, 'drs_off')   # tbz w8, #0, 0x800584c3c
    return ('{ extern int totk_drs_enabled(void); '
            'if (!totk_drs_enabled() || ((c->x[8] >> 0) & 1) == 0) { goto L_800584c3c; } }')


def actor_component_pointer_guard(gen, addr):
    """Guard against dangling / recycled slab chunks in actor component update.

    At main+0x1bd01c8 (1.4.3), c->x[21] is loaded from *(x8 + 0). Valid 64-bit guest pointers
    are >= 0x800000000; small integer values like 0x80 (from a freed slab header) pass Nintendo's
    odd-bit tag check and crash in f_8000155ec with access violation at 0x90.
    Branching to L_801bd01f0 cleanly skips the invalid entity, matching NULL/tagged pointer handling.
    """
    expect(gen, addr, 0xb4000155, 'actor_component_pointer_guard')  # cbz x21, #0x801bd01f0
    return 'if (c->x[21] < 0x800000000ull) { goto L_801bd01f0; }'


def object_link_pointer_guard(gen, addr):
    """Guard against invalid guest object pointer in f_8000155ec entry (main+0x15618).

    Guards against any non-pointer value in c->x[1] (like 0x80) when accessing +0x10.
    """
    expect(gen, addr, 0xb4000361, 'object_link_pointer_guard')  # cbz x1, #0x800015684
    return 'if (c->x[1] < 0x800000000ull) { goto L_800015684; }'


PATCHES = {
    0x8005843e0: drs_off,
    0x801bd01c8: actor_component_pointer_guard,
    0x800015618: object_link_pointer_guard,
}
