// SPDX-License-Identifier: MIT
// Shared header of the atmosphere shaders (nvn_atmos.cpp). make_atmos_spv.py pastes it in place of the "//#include atmos_common" line.
// One uniform block for every pass; the host writes a fresh copy before each pass type (vkCmdUpdateBuffer into slots of one buffer).
layout(set = 0, binding = 4, std140) uniform U {
    mat4 ivp;      // (ndc.xy, depth, 1) -> homogeneous world, w = 1 / clip.w (reversed infinite z, the course camera)
    mat4 vp;       // clip = vp * (world, 1)
    vec4 eye;      // xyz: camera position
    vec4 light;    // xyz: unit world direction toward the sun, w: frame counter
    vec4 size;     // xy: render size in pixels, zw: scene viewport jitter in pixels
    vec4 cam;      // x: near plane, y: sun valid (shadow map usable), z: dt seconds, w: history valid
    vec4 fog;      // x: density at the ground, y: height falloff per world unit, z: ground height offset, w: strongest fog (0..1)
    vec4 fogB;     // x: fog start distance
    vec4 shaft;    // x: intensity, y: scattering density per world unit, z: ray length limit, w: steps
    vec4 shaftB;   // x: HG anisotropy g, y: depth bias, z: debug / convention flags, w: night fade (sky luminance where the shafts start to appear; full at 3x; 0 = no fade)
    vec4 sunCol;   // rgb: sun tint
    vec4 expo;     // x: target log2 luminance, y: strength (0..1), z: gain min, w: gain max
    vec4 expoB;    // x: adaptation time constant (s)
    vec4 bloom;    // x: strength, y: threshold, z: knee, w: unused
    vec4 flags;    // x: fog on, y: shafts on, z: exposure on, w: bloom on
    vec4 pass;     // per pass: x first downsample (1) or not, zw: destination size in pixels
    vec4 dbg;
    vec4 cloud;    // cloud shadows: x strength, y world units per noise cell, z wind speed (units/s), w coverage 0..1
    vec4 cloudB;   // x time (s), y fade-out distance, zw: unused
    vec4 sky;      // sky gradient: x strength (all terms), y horizon haze, z sun glow, w zenith darkening
    vec4 flags2;   // x: cloud shadows on, y: sky gradient on
} u;

float lumOf(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 ndcOf(vec2 pix, float d) {
    vec2 win = pix + 0.5 - u.size.zw;
    return vec3(win / u.size.xy * vec2(2.0, -2.0) + vec2(-1.0, 1.0), d);
}
// world position of a full-resolution pixel with depth d (> 0); zv = view-space depth
vec3 worldAt(vec2 pix, float d, out float zv) {
    vec4 h = u.ivp * vec4(ndcOf(pix, d), 1.0);
    zv = 1.0 / h.w;
    return h.xyz / h.w;
}
// sky = nothing nearer than about 150 world units (reversed infinite z: depth = near / viewZ). The sky is drawn with a tiny but not always exactly zero depth.
bool isSkyDepth(float d) { return d <= u.cam.x / 150.0; }
