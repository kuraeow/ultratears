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


# ---- Frame rate (60 fps and other limits without the game running fast). The sites and what each one does are from
# MacNativeGames games/totk/runtime/game_patches_v143.c (MIT, Copyright (c) 2026 MacNativeGames contributors), each word
# checked against the 1.4.3 main; there they are machine-code veneers, here each site is C that reads the step from the
# runtime (src/host/frame_rate.c) and takes the original instruction when its switch is off. The switches are read every
# time (the limit can change while playing: 30 is the shipping code).
MAIN = 0x800000000


def word_or(gen, addr, orig, repl, cond, what):
    """The replacement word when cond holds, the original otherwise."""
    expect(gen, addr, orig, what)
    return f'if ({cond}) {{ {gen.c_of(repl, addr)} }} else {{ {gen.c_of(orig, addr)} }}'


def fr(fn):   # a frame-rate runtime function, declared where it is used
    return f'({{ extern {"float" if fn.startswith("totk_fr_step") else "int"} {fn}(void); {fn}(); }})'


def set_s(reg, expr):   # an S register (the rest of the vector register cleared), from a float expression
    return f'{{ float f_ = (float)({expr}); uint32_t u_; __builtin_memcpy(&u_, &f_, 4); c->v[{reg}] = u_; }}'


def get_s(reg):
    return f'({{ uint32_t u_ = (uint32_t)c->v[{reg}]; float f_; __builtin_memcpy(&f_, &u_, 4); f_; }})'


# ---- Scene (render) resolution. Where and what from MacNativeGames games/totk/runtime/game_patches_v143.c
# (patch_render_res, MIT, Copyright (c) 2026 MacNativeGames contributors): every path that picks the scene size goes
# through main+0x585e48, which converts the graphics-parameter entry's size (docked 1600x900) to floats and stores them
# as the renderer's width (+0xcc4) and height (+0xcc8); the game derives its scene targets (and their half and quarter
# sizes) from these, and its own FSR 1 (EASU) draws the scene onto its canvas. Here the two stores take the size
# src/host/render_res.c chose instead, when there is one. The output (canvas, UI, swapchain): the swapchain size nnMain
# writes (main+0xbdb584) and, as the display configuration publishes its active output (main+0x3dcb28), its canvas
# (+0x10) and that output (+0x508, the original store that follows). Also from MacNativeGames (patch_output_res), as
# totk-pc-recomp's patches-v143.txt carries them.
def scene_size(axis):   # str s0, [x19, #0xcc4 / #0xcc8]
    orig = {0: 0xbd0cc660, 1: 0xbd0cca60}[axis]

    def f(gen, addr):
        expect(gen, addr, orig, 'scene_size')
        return ('{ extern int totk_scene_size(float *w, float *h); float w_, h_; '
                f'if (totk_scene_size(&w_, &h_)) {set_s(0, "w_" if axis == 0 else "h_")} }} {gen.c_of(orig, addr)}')
    return f


def output_size(gen, addr):   # movk x8, #0x438, lsl #32: the swapchain size the game asks for (1920x1080)
    expect(gen, addr, 0xf2c08708, 'output_size')
    return ('{ extern int totk_output_size(uint64_t *packed); uint64_t s_; '
            f'if (totk_output_size(&s_)) c->x[8] = s_; else {{ {gen.c_of(0xf2c08708, addr)} }} }}')


def output_canvas(gen, addr):   # before lsr x4, x3, #32: the display configuration's canvas (+0x10) and active output (+0x508)
    expect(gen, addr, 0xd360fc64, 'output_canvas')
    return ('{ extern int totk_output_size(uint64_t *packed); uint64_t s_; '
            'if (totk_output_size(&s_)) { c->x[3] = s_; ST64(c->x[19] + 0x10, s_); } }')


# fps60: the adaptive present-interval controller (main+0x2db174) gets the minimum interval 1 (60 Hz ticks) instead of
# the shipping data's 2. The controller still raises the interval when a frame does not fit.
FPS60 = [(0x2db204, 0x6b08015f, 0x7100055f),   # clamp: cmp w10, #1 (was cmp w10, w8 = minimum)
         (0x2db208, 0x1a89b10a, 0x1a9fa52a),   # clamp: csinc w10, w9, wzr, ge (desired = max(desired, 1))
         (0x2db22c, 0xb940468a, 0xd503201f),   # lower trigger: nop the stored-candidate load
         (0x2db360, 0x6b0801bf, 0x710005bf),   # raise path: cmp w13, #1
         (0x2db36c, 0x1a88c1ac, 0x1a9fc5ac),   # raise path: csinc w12, w13, wzr, gt (next lower candidate floors at 1)
         (0x2db340, 0xb9400288, 0x52800028),   # lower path: mov w8, #1 (minimum interval 1)
         (0x2db34c, 0x2a0a03f5, 0x1a9fa555)]   # lower path: csinc w21, w10, wzr, ge (result never below 1)


def fps60(orig, repl):
    return lambda gen, addr: word_or(gen, addr, orig, repl, fr('totk_fr_fps60'), 'fps60')


# realtime: the game is stepped by the time frames really take (the runtime's average of the last display intervals),
# not by whole 60 Hz ticks of the interval it chose. Two places: the frame-step producer feeds FrameRateMgr s8 = the
# nominal interval in ticks (60 / target fps, s0) times a 0.5 / 1.0 coefficient (s1); s0 becomes the measured step in
# ticks, the coefficient and FrameRateMgr's own arithmetic stay. And after the controller call the global 30 Hz ratio
# (stored at 0x2daa7c) is computed as 60 / target fps (+0x250) * 0.5; it becomes the measured step * 30.
def rt_step_ticks(gen, addr):   # before fmul s8, s0, s1
    expect(gen, addr, 0x1e210808, 'realtime producer')
    return f'if ({fr("totk_fr_realtime")}) {set_s(0, fr("totk_fr_step_ticks60"))}'


def rt_ratio_load(gen, addr):   # ldr s0, [x19, #0x250] (the target fps, an integer)
    expect(gen, addr, 0xbd425260, 'realtime ratio')
    return f'if ({fr("totk_fr_realtime")}) {set_s(0, fr("totk_fr_step_ratio30"))} else {{ {gen.c_of(0xbd425260, addr)} }}'


def rt_ratio_skip(orig):   # scvtf s0 / fdiv s0 / fmul s0 of the shipping ratio: s0 already holds it
    def f(gen, addr):
        expect(gen, addr, orig, 'realtime ratio')
        return f'if (!{fr("totk_fr_realtime")}) {{ {gen.c_of(orig, addr)} }}'
    return f


# cloth: the actor animation update builds a second clock {seconds = factor / 30, ratio = factor} for hair and cloth
# every rendered frame (at 60 fps they swing twice as fast). 1.4.3 inlines it in two actor paths, where the factor was
# just stored to the ratio slot: there the ratio slot gets measured ratio * factor and the seconds measured seconds *
# factor. Two callers of the shared update (main+0x2a0e40) get s2 (seconds) and s3 (ratio) scaled by the measured ratio;
# its third caller already passes measured time.
def cloth_inline(orig, dst, factor, slot):   # fmul s<dst>, ..., s<dst>: seconds = factor / 30
    def f(gen, addr):
        expect(gen, addr, orig, 'cloth clock')
        on = fr('totk_fr_cloth')
        return (f'if ({on}) {{ float k_ = {get_s(factor)}; '
                f'{{ float r_ = {fr("totk_fr_step_ratio30")} * k_; uint32_t u_; __builtin_memcpy(&u_, &r_, 4); ST32(c->sp + {slot:#x}, u_); }} '
                f'{set_s(dst, fr("totk_fr_step_seconds") + " * k_")} }} else {{ {gen.c_of(orig, addr)} }}')
    return f


def cloth_call(gen, addr):   # before bl main+0x2a0e40
    expect(gen, addr, 0x94000000 | ((0x2a0e40 - (addr - MAIN)) >> 2 & 0x3ffffff), 'cloth call')
    r = fr('totk_fr_step_ratio30')
    return (f'if ({fr("totk_fr_cloth")}) {{ float r_ = {r}; {set_s(2, get_s(2) + " * r_")} {set_s(3, get_s(3) + " * r_")} '
            f'{set_s(4, "r_")} }}')


# bounce (MacNativeGames #61, trampolines on the sky islands): the contact code scales the bounce vector by 0.1 when the
# time object's +0x28 (30 / (ratio * time scale)) is over 100: at 30 Hz "time slowed below 0.3", above 100 fps also with
# time running normally. Compare +0x28 * +0x38 (the ratio): 30 / time scale, the same at 30 Hz at any frame rate. Before
# mov w9, #100.0, x9 still holds the time object and s0 its +0x28.
def bounce(gen, addr):
    expect(gen, addr - 4, 0xbd402920, 'bounce')   # ldr s0, [x9, #0x28]
    expect(gen, addr, 0x52a85909, 'bounce')       # mov w9, #0x42c80000 (100.0)
    return (f'if ({fr("totk_fr_highfps")}) {{ uint32_t u_ = (uint32_t)LD32(c->x[9] + 0x38); float m_; __builtin_memcpy(&m_, &u_, 4); '
            f'{set_s(0, get_s(0) + " * m_")} }}')


# fade (MacNativeGames #466 / #556 / #538, translucent rocks, lanterns and walls above ~63 fps): distance / LOD mesh fades
# and pre-actor fades advance a byte counter by trunc(2.1 * ratio), 0 once the ratio is below 1 / 2.1; the counter stops
# with the object invisible or half faded. A positive step that truncates to 0 becomes 1 (an increment of 0 stays 0:
# MacNativeGames #1134, paused time must not fade).
def fade(reg):
    def f(gen, addr):
        orig = 0x1e380000 | reg   # fcvtzs w<reg>, s0
        expect(gen, addr, orig, 'fade')
        return (f'{gen.c_of(orig, addr)} if ({fr("totk_fr_highfps")} && (uint32_t)c->x[{reg}] == 0 && {get_s(0)} > 0.0f) '
                f'c->x[{reg}] = 1;')
    return f


PATCHES = {
    0x8005843e0: drs_off,
    MAIN + 0x585e50: scene_size(0),
    MAIN + 0x585e60: scene_size(1),
    MAIN + 0xbdb584: output_size,
    **{MAIN + a: fps60(o, r) for a, o, r in FPS60},
    MAIN + 0x2daa54: rt_ratio_load,
    MAIN + 0x2daa6c: rt_ratio_skip(0x5e21d800),   # scvtf s0, s0
    MAIN + 0x2daa70: rt_ratio_skip(0x1e201820),   # fdiv s0, s1, s0
    MAIN + 0x2daa78: rt_ratio_skip(0x1e210800),   # fmul s0, s0, s1
    MAIN + 0x246eac: cloth_inline(0x1e200940, 0, 10, 0x364),   # fmul s0, s10, s0 (factor s10, ratio slot sp+0x364)
    MAIN + 0x345904: cloth_inline(0x1e210801, 1, 0, 0x114),    # fmul s1, s0, s1 (factor s0, ratio slot sp+0x114)
    MAIN + 0xa7ac4: fade(8), MAIN + 0x95dd24: fade(8), MAIN + 0x6155e4: fade(10), MAIN + 0x615660: fade(10),
}
PRE_PATCHES = {
    MAIN + 0x3e5658: rt_step_ticks,
    MAIN + 0x24944c: cloth_call, MAIN + 0x28bc98: cloth_call,
    MAIN + 0x562a68: bounce,
    MAIN + 0x3dcb28: output_canvas,
}
