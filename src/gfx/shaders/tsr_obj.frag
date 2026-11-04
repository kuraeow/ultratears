#version 450
// UltraTears: per-object motion (src/gfx/vkb_tsr.c). SPDX-License-Identifier: GPL-3.0-or-later
// Drawn right after a moving object's own draw, with its vertex shader's last-frame position variant (the translator's
// PreviousPosition): writes last frame's normalized device position of the surface, its depth now, and 1, into the
// port's object motion target. The camera motion pass (tsr_mv.comp) turns it into a vector where this surface is still
// the one in the scene depth. The two locations below are placeholders the runtime patches (vkb_tsr.c obj_fs): the
// input is the variant's output location, the output the free target of the pass.
layout(location = 31) in vec4 prev_clip;
layout(location = 7) out vec4 o;
void main() {
    if (prev_clip.w <= 1e-6) { o = vec4(0.0); return; }
    o = vec4(prev_clip.xy / prev_clip.w, gl_FragCoord.z, 1.0);
}
