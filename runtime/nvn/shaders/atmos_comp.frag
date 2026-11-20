#version 450
// SPDX-License-Identifier: MIT
// Atmosphere, composite (nvn_atmos.cpp): one read-modify-write of the linear HDR scene colour for all four effects. Blend ONE / ONE_MINUS_SRC_ALPHA on the
// float scene target, so  result = src.rgb + dst * (1 - src.a)  with
//   src.a   = 1 - gain * (1 - fog)                 (gain = eye adaptation, fog = height fog amount; may be negative: the target is RGBA16F, nothing clamps)
//   src.rgb = gain * (fogColour * fog + shafts + bloom)
// i.e. result = gain * (scene * (1 - fog) + fogColour * fog + shafts + bloom).
layout(set = 0, binding = 0) uniform sampler2D depthTex;    // nearest
layout(set = 0, binding = 1) uniform sampler2D stateTex;    // nearest, 2x1
layout(set = 0, binding = 2) uniform sampler2D shaftTex;    // nearest (half resolution, a = view depth)
layout(set = 0, binding = 3) uniform sampler2D bloomTex;    // linear (chain level 0)
//#include atmos_common
layout(location = 0) out vec4 outMul;

float fogAmount(vec3 P, float groundY) {
    vec3 ray = P - u.eye.xyz;
    float t = length(ray);
    if (t < 1e-3) return 0.0;
    vec3 dir = ray / t;
    float tt = max(t - u.fogB.x, 0.0);
    float b = max(u.fog.y, 1e-4);
    float h0 = u.eye.y - (groundY + u.fog.z);
    float k = b * dir.y;
    float base = u.fog.x * exp(clamp(-b * h0, -30.0, 30.0));
    float integral = abs(k * tt) < 1e-4 ? base * tt : base * (1.0 - exp(clamp(-k * tt, -30.0, 30.0))) / k;
    return min(1.0 - exp(-max(integral, 0.0)), u.fog.w);
}

float hash21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x), mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; i++) { s += a * vnoise(p); p = p * 2.03 + vec2(17.1, 9.2); a *= 0.5; }
    return s;   // 0 .. about 0.94, centred near 0.47
}
// cloud cover over a ground position: 0 clear .. 1 under a cloud. Clouds drift along a fixed wind direction (world space, not camera relative).
float cloudCover(vec2 xz) {
    vec2 q = xz / max(u.cloud.y, 1.0) + vec2(0.8, 0.6) * (u.cloudB.x * u.cloud.z / max(u.cloud.y, 1.0));
    float n = fbm(q);
    float thr = mix(0.62, 0.34, clamp(u.cloud.w, 0.0, 1.0));
    return smoothstep(thr, thr + 0.28, n);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depthTex, p, 0).r;
    vec4 st0 = texelFetch(stateTex, ivec2(0, 0), 0);
    vec4 st1 = texelFetch(stateTex, ivec2(1, 0), 0);
    float gain = 1.0;
    if (u.flags.z > 0.5) gain = clamp(exp2((u.expo.x - st0.a) * u.expo.y), u.expo.z, u.expo.w);
    float zv = 1e6;
    vec3 P = vec3(0.0);
    if (d > 0.0) P = worldAt(vec2(p), d, zv);
    float fogA = 0.0;
    if (u.flags.x > 0.5 && d > 0.0) fogA = fogAmount(P, st1.a);
    vec3 add = vec3(0.0);
    if (u.flags.y > 0.5) {
        vec2 hc = (vec2(p) + 0.5) * 0.5 - 0.5;
        ivec2 b0 = ivec2(floor(hc));
        vec2 f = hc - vec2(b0);
        ivec2 hs = ivec2(u.size.xy * 0.5 + 0.5);   // half-resolution image size
        vec3 acc = vec3(0.0); float wsum = 0.0;
        for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
            ivec2 q = clamp(b0 + ivec2(i, j), ivec2(0), hs - 1);
            vec4 s = texelFetch(shaftTex, q, 0);
            float wb = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            float wd = 1.0 / (1.0 + 12.0 * abs(s.a - zv) / max(min(s.a, zv), 0.5));
            float w = wb * wd + 1e-5;
            acc += s.rgb * w; wsum += w;
        }
        // The shaft term is 1.0 at scene scale 1: scale it to this scene's mean brightness. The light comes through the sky, so it is never
        // brighter than the sky the frame measured, and it fades out when that sky is dark (night: there is no sun, and a term sized by the
        // mean of a dark scene with a few lamps in it lay over everything as a gray haze).
        float skyLum = lumOf(st0.rgb);
        float shaftScale = clamp(exp2(st0.a), 0.05, 16.0);
        if (u.shaftB.w >= 0.0) shaftScale = min(shaftScale, 2.0 * skyLum);   // below 0: the old rule (A/B tests only), its 0.05 floor is a gray haze over a night scene
        float day = u.shaftB.w > 0.0 ? smoothstep(u.shaftB.w, 3.0 * u.shaftB.w, skyLum) : 1.0;
        add += acc / wsum * (shaftScale * day);
    }
    if (u.flags.w > 0.5) {
        vec2 uv = (vec2(p) + 0.5) / u.size.xy;
        add += textureLod(bloomTex, uv, 0.0).rgb * u.bloom.x;
    }
    float mulExtra = 1.0;   // scalar darkening from cloud shadows and the sky's zenith
    vec3 cloudDbg = vec3(0.0);
    vec3 nrm = normalize(cross(dFdx(P), dFdy(P)) + vec3(0.0, 1e-12, 0.0));   // derivatives outside any branch
    if (u.flags2.x > 0.5 && d > 0.0 && !isSkyDepth(d)) {   // moving cloud shadows on the ground: surfaces facing up, fading out with distance
        // every surface below the clouds is shadowed; steep faces (walls, trunks) get 60% of it. The depth-derived normal is too noisy at world scale to gate on.
        float up = mix(0.6, 1.0, smoothstep(0.3, 0.8, abs(nrm.y)));
        float dist = length(P - u.eye.xyz);
        float fade = 1.0 - smoothstep(u.cloudB.y * 0.5, u.cloudB.y, dist);
        float cov = cloudCover(P.xz);
        cloudDbg = vec3(cov, up, clamp((P.y - st1.a) / 50.0, 0.0, 1.0));   // debug 6 on the ground: red cloud cover, green normal mask, blue height over the ground state (50 units = 1)
        mulExtra *= 1.0 - u.cloud.x * cov * up * fade;
    }
    vec3 skyAdd = vec3(0.0);
    if (u.flags2.y > 0.5 && isSkyDepth(d)) {   // sky gradient from the sun's direction: warm glow near the sun, pale haze at the horizon, a deeper zenith
        float zv0;
        vec3 Pfar = worldAt(vec2(p), 1e-6, zv0);
        vec3 dir = normalize(Pfar - u.eye.xyz);
        float elev = dir.y;
        float horizon = pow(clamp(1.0 - elev, 0.0, 1.0), 4.0);
        float zen = smoothstep(0.15, 0.95, elev);
        float cs = max(dot(dir, u.light.xyz), 0.0);
        float glow = 0.6 * pow(cs, 6.0) + 0.9 * pow(cs, 48.0);
        float lumRef = max(lumOf(st0.rgb), 0.1 * clamp(exp2(st0.a), 0.05, 16.0));   // the sky colour's brightness the frame measured (fog colour state)
        skyAdd = u.sky.x * lumRef * (u.sky.z * glow * u.sunCol.rgb + u.sky.y * horizon * vec3(0.55, 0.62, 0.75));
        mulExtra *= 1.0 - u.sky.x * u.sky.w * zen;
        cloudDbg = vec3(glow);
    }
    add += skyAdd;
    if (u.dbg.x > 5.5 && u.dbg.x < 7.5) { outMul = vec4(cloudDbg, 1.0); return; }   // 6: cloud cover on the ground / sky glow
    if (u.dbg.x > 0.5) {   // 1: the shaft buffer alone, 3: the bloom alone, 4: fog amount, 5: exposure gain
        vec3 v = vec3(0.0);
        if (u.dbg.x < 1.5 || u.dbg.x > 7.5) { ivec2 q = clamp(p / 2, ivec2(0), ivec2(u.size.xy * 0.5) - 1); v = texelFetch(shaftTex, q, 0).rgb; }   // 1: the shaft buffer
        else if (u.dbg.x < 3.5) v = textureLod(bloomTex, (vec2(p) + 0.5) / u.size.xy, 0.0).rgb * u.bloom.x;
        else if (u.dbg.x < 4.5) v = vec3(fogA);
        else v = vec3(gain * 0.5);
        outMul = vec4(v, 1.0);
        return;
    }
    outMul = vec4(gain * (st0.rgb * fogA + add), 1.0 - gain * (1.0 - fogA) * mulExtra);
}
