#version 450
// SPDX-License-Identifier: MIT
// Enhanced lighting, pass 3 of 3 (nvn_lighting_fx.cpp): applies the denoised AO and contact-shadow factors to the linear HDR scene colour,
// colour = colour * out (RGB only, blend factor DST_COLOR / ZERO). With the debug pipeline (blend off) the factor replaces the picture.
layout(set = 0, binding = 0) uniform sampler2D resTex;
layout(push_constant) uniform P {
    vec4 k;      // x: AO strength, y: contact-shadow strength, z: fade-out distance (view units), w: AO contrast (power)
    vec4 size;   // xy: render size in pixels, z: debug view (0 off, 1 AO, 2 contact, 4 frames accumulated, white = 256), w unused
} pc;
layout(location = 0) out vec4 outMul;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 r = texelFetch(resTex, p, 0);
    float w = r.b;
    float fadeAo = 1.0 - smoothstep(0.6 * pc.k.z, pc.k.z, w);
    vec2 edge = min(gl_FragCoord.xy, pc.size.xy - gl_FragCoord.xy) / (0.04 * pc.size.y);   // rays that leave the screen read clamped depth: fade near the border
    float fadeCt = fadeAo * clamp(min(edge.x, edge.y), 0.0, 1.0);
    float ao = 1.0 - pc.k.x * fadeAo * (1.0 - pow(clamp(r.r, 0.0, 1.0), pc.k.w));
    float ct = 1.0 - pc.k.y * fadeCt * (1.0 - r.g);
    if (pc.size.z > 3.5) { outMul = vec4(vec3(r.a / 256.0), 1.0); return; }
    if (pc.size.z > 0.5) { outMul = vec4(pc.size.z < 1.5 ? vec3(ao) : vec3(ct), 1.0); return; }
    outMul = vec4(vec3(ao * ct), 1.0);
}
