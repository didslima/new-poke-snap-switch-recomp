#version 450
// SPDX-License-Identifier: MIT
// Enhanced lighting, pass 1 of 3 (nvn_lighting_fx.cpp): ground-truth ambient occlusion (GTAO, Jimenez et al. 2016: horizon search in a few screen-space
// slices, cosine-weighted analytic integration against the surface normal) and, when a sun is known, the short screen-space contact-shadow march of
// contact.frag. One full-screen triangle at render resolution; output r = AO visibility, g = contact-shadow visibility (1 = lit). Both are noisy by design
// (per-pixel slice rotation and step jitter, shifted every frame): lfx_resolve.frag filters and accumulates them.
// World positions come from the scene depth (reversed infinite Z, the upscaler viewport jitter undone like upscale_mv.comp).
// Half resolution (q.w >> 4 = 1..4): the target has half the width and height and each of its pixels works out one pixel of its 2x2 block of the
// picture, a different one each frame (q.w >> 4 picks it), so over four frames every pixel gets its own result; lfx_resolve.frag spreads and accumulates.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(push_constant) uniform P {
    mat4 ivp;     // (ndc.xy, depth, 1) -> homogeneous world, w = 1 / clip.w
    vec4 dclip;   // VP * (L, 0): clip-space change per world unit toward the sun (contact shadows)
    vec4 light;   // xyz: unit world direction toward the sun, w: frame counter
    vec4 size;    // xy: render size in pixels, zw: scene viewport jitter in pixels
    vec4 q;       // x: AO radius (world units), y: contact ray length (0 = no contact shadows), z: contact thickness, w: AO quality (slices) + 16 * (0 full resolution, 1..4 half resolution: the pixel of the 2x2 block + 1)
} pc;
layout(location = 0) out vec4 outV;

const float PI = 3.14159265;

vec3 ndcOf(vec2 pix, float d) {
    vec2 win = pix + 0.5 - pc.size.zw;
    return vec3(win / pc.size.xy * vec2(2.0, -2.0) + vec2(-1.0, 1.0), d);
}
vec3 posAt(ivec2 p, out float w) {
    p = clamp(p, ivec2(0), ivec2(pc.size.xy) - 1);
    float d = texelFetch(depthTex, p, 0).r;
    if (d <= 0.0) { w = 1e9; return vec3(0.0); }
    vec4 h = pc.ivp * vec4(ndcOf(vec2(p), d), 1.0);
    w = 1.0 / h.w;
    return h.xyz / h.w;
}

void main() {
    outV = vec4(1.0);
    int qw = int(pc.q.w), hm = qw >> 4;
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (hm > 0) p = min(p * 2 + ivec2((hm - 1) & 1, (hm - 1) >> 1), ivec2(pc.size.xy) - 1);
    vec2 fc = vec2(p) + 0.5;   // the picture pixel this result is for
    float d = texelFetch(depthTex, p, 0).r;
    if (d <= 0.0) return;   // sky / infinity (reversed Z)
    float wC, wR, wL, wU, wD;
    vec3 pC = posAt(p, wC);
    vec3 pR = posAt(p + ivec2(1, 0), wR), pL = posAt(p - ivec2(1, 0), wL);
    vec3 pD = posAt(p + ivec2(0, 1), wD), pU = posAt(p - ivec2(0, 1), wU);
    // normal from the depth: central differences on smooth surfaces, else the one-sided difference that agrees better (contact.frag)
    vec3 dx = abs(wR - wC) < abs(wC - wL) ? pR - pC : pC - pL;
    vec3 dy = abs(wD - wC) < abs(wC - wU) ? pD - pC : pC - pU;
    if (abs(wR + wL - 2.0 * wC) < 0.01 * wC) dx = 0.5 * (pR - pL);
    if (abs(wD + wU - 2.0 * wC) < 0.01 * wC) dy = 0.5 * (pD - pU);
    vec3 n = cross(dx, dy);
    float nl = length(n);
    if (!(nl > 1e-12)) return;
    n /= nl;
    vec4 hf = pc.ivp * vec4(ndcOf(vec2(p), d * 0.5), 1.0);   // farther along the pixel's ray
    vec3 V = normalize(pC - hf.xyz / hf.w);                   // toward the camera
    if (dot(n, V) < 0.0) n = -n;

    float frame = pc.light.w;
    float fr = fract(frame * 0.6180339887);
    float n1 = fract(52.9829189 * fract(dot(fc, vec2(0.06711056, 0.00583715))) + fr);
    float n2 = fract(0.7548776662 * fc.x + 0.5698402910 * fc.y + fr * 3.7);

    // ---- ambient occlusion -------------------------------------------------------------------------------------------------------------------------
    float ao = 1.0;
    if (pc.q.x > 0.0) {
        // world units per pixel at this depth: how far the radius reaches on screen
        vec4 h1 = pc.ivp * vec4(ndcOf(vec2(p) + vec2(1.0, 0.0), d), 1.0), h2 = pc.ivp * vec4(ndcOf(vec2(p) + vec2(0.0, 1.0), d), 1.0);
        vec3 rightW = h1.xyz / h1.w - pC, downW = h2.xyz / h2.w - pC;
        float upp = max(length(rightW), 1e-6);
        float radius = pc.q.x;
        float radiusPix = clamp(radius / upp, 0.0, 0.1 * pc.size.y);
        if (radiusPix >= 2.0) {
            // The AO normal is taken over a wider footprint than one pixel (a quarter of the AO radius): the one-pixel normal is the triangle's,
            // and the game shades with smooth normals, so on a low-poly curved surface in a close-up (a sleeve, a chin) every triangle got its own
            // AO level and the mesh showed as light and dark patches. A wide tap pair is used only when the depth runs evenly through it (no edge).
            vec3 nA = n;
            {
                int k = int(clamp(radiusPix * 0.25, 1.0, 24.0));
                if (k > 1) {
                    float wr, wl, wu, wd;
                    vec3 qR = posAt(p + ivec2(k, 0), wr), qL = posAt(p - ivec2(k, 0), wl);
                    vec3 qD = posAt(p + ivec2(0, k), wd), qU = posAt(p - ivec2(0, k), wu);
                    float tol = 0.02 * wC + 0.25 * radius;
                    vec3 ax = abs(wr + wl - 2.0 * wC) < tol && abs(wr - wl) < 4.0 * radius ? qR - qL : dx;
                    vec3 ay = abs(wd + wu - 2.0 * wC) < tol && abs(wd - wu) < 4.0 * radius ? qD - qU : dy;
                    vec3 m = cross(ax, ay);
                    float ml = length(m);
                    if (ml > 1e-12) { m /= ml; if (dot(m, V) < 0.0) m = -m; if (dot(m, n) > 0.5) nA = m; }
                }
            }
            const int STEPS = 8;
            int slices = qw & 15;
            float falloffRange = 0.4 * radius, falloffFrom = radius - falloffRange;
            float falloffMul = -1.0 / falloffRange, falloffAdd = falloffFrom / falloffRange + 1.0;
            float vis = 0.0;
            for (int s = 0; s < slices; s++) {
                float phi = (float(s) + n1) * PI / float(slices);
                vec2 om = vec2(cos(phi), -sin(phi));                    // pixel direction (y down)
                vec3 dir3 = normalize(rightW * om.x + downW * om.y);     // the same direction in the world
                vec3 orthoDir = dir3 - dot(dir3, V) * V;
                vec3 axis = normalize(cross(dir3, V));
                vec3 projN = nA - axis * dot(nA, axis);
                float projNLen = length(projN);
                if (projNLen < 1e-4) { vis += 1.0; continue; }
                float signN = sign(dot(orthoDir, projN));
                if (signN == 0.0) signN = 1.0;
                float cosN = clamp(dot(projN, V) / projNLen, 0.0, 1.0);
                float nAng = signN * acos(cosN);
                float low0 = cos(nAng + 0.5 * PI), low1 = cos(nAng - 0.5 * PI);
                float hc0 = low0, hc1 = low1;
                for (int j = 0; j < STEPS; j++) {
                    float t = (float(j) + n2) / float(STEPS);
                    float off = max(t * t * radiusPix, 1.0 + float(j));
                    for (int side = 0; side < 2; side++) {
                        vec2 sp = vec2(p) + 0.5 + (side == 0 ? om : -om) * off;
                        ivec2 ip = ivec2(floor(sp));
                        if (any(lessThan(ip, ivec2(0))) || any(greaterThanEqual(ip, ivec2(pc.size.xy)))) continue;
                        float sd = texelFetch(depthTex, ip, 0).r;
                        if (sd <= 0.0) continue;
                        vec4 hs = pc.ivp * vec4(ndcOf(vec2(ip), sd), 1.0);
                        vec3 delta = hs.xyz / hs.w - pC;
                        float dist = length(delta);
                        float shc = dot(delta, V) / max(dist, 1e-6);
                        float wgt = clamp(dist * falloffMul + falloffAdd, 0.0, 1.0);
                        if (side == 0) hc0 = max(hc0, mix(low0, shc, wgt)); else hc1 = max(hc1, mix(low1, shc, wgt));
                    }
                }
                float h0 = -acos(clamp(hc1, -1.0, 1.0)), h1a = acos(clamp(hc0, -1.0, 1.0));
                h0 = nAng + max(h0 - nAng, -0.5 * PI);
                h1a = nAng + min(h1a - nAng, 0.5 * PI);
                // Angle bias: a horizon less than ANGLE_BIAS above the tangent plane does not occlude, and the rest of the range is stretched to
                // keep full occlusion. The normal here is the triangle's (from the depth), while the game shades with smooth normals: on a low-poly
                // curved surface (a sleeve in a cutscene close-up) every shallow crease between two triangles darkened one of them and the mesh
                // showed as jagged light and dark patches. Real creases and contact lines are far steeper than the bias.
                const float ANGLE_BIAS = 0.35, ANGLE_SCALE = (0.5 * PI) / (0.5 * PI - ANGLE_BIAS);
                float e0 = (h0 - nAng) + 0.5 * PI, e1 = 0.5 * PI - (h1a - nAng);   // each horizon's elevation above the tangent plane
                if (e0 < 0.5 * PI) h0 = nAng - 0.5 * PI + max(e0 - ANGLE_BIAS, 0.0) * ANGLE_SCALE;
                if (e1 < 0.5 * PI) h1a = nAng + 0.5 * PI - max(e1 - ANGLE_BIAS, 0.0) * ANGLE_SCALE;
                float sn = sin(nAng);
                float iarc0 = (cosN + 2.0 * h0 * sn - cos(2.0 * h0 - nAng)) * 0.25;
                float iarc1 = (cosN + 2.0 * h1a * sn - cos(2.0 * h1a - nAng)) * 0.25;
                vis += projNLen * (iarc0 + iarc1);
            }
            ao = clamp(vis / float(slices), 0.0, 1.0);
        }
    }

    // ---- contact shadow ----------------------------------------------------------------------------------------------------------------------------
    float contact = 1.0;
    if (pc.q.y > 0.0) {
        float ndl = dot(n, pc.light.xyz);
        float facing = smoothstep(0.02, 0.3, ndl);   // surfaces turned from the sun are already dark
        if (facing > 0.0) {
            float bias = 0.012 + 0.004 * wC, thick = pc.q.z + 0.01 * wC;
            vec4 C0 = vec4(ndcOf(vec2(p), d) * wC, wC);
            vec4 C1 = C0 + pc.dclip * pc.q.y;
            float pixLen = (C1.w > 1e-3) ? length((C1.xy / C1.w - C0.xy / C0.w) * 0.5 * pc.size.xy) : 64.0;
            int N = int(clamp(pixLen * 0.5, 8.0, 32.0));
            float g = max(max(abs(wR - wC), abs(wC - wL)), max(abs(wD - wC), abs(wC - wU)));   // slope-scaled bias (see contact.frag)
            vec2 win0 = (C0.xy / C0.w * vec2(0.5, -0.5) + 0.5) * pc.size.xy;
            float occ = 0.0, wsum = 0.0;   // fractional occlusion: the weighted share of the ray's steps that end behind a surface (near steps count more), so a single noisy step moves it by 1/N, not by a whole level
            for (int i = 0; i < N; i++) {
                float t = (float(i) + n1) / float(N);
                float wt = 1.0 - 0.8 * t;
                vec4 C = C0 + pc.dclip * (pc.q.y * t);
                if (C.w < 1e-3) break;
                vec2 win = (C.xy / C.w * vec2(0.5, -0.5) + 0.5) * pc.size.xy;
                ivec2 tp = ivec2(floor(win + pc.size.zw));
                if (any(lessThan(tp, ivec2(0))) || any(greaterThanEqual(tp, ivec2(pc.size.xy)))) break;
                wsum += wt;
                float sd = texelFetch(depthTex, tp, 0).r;
                if (sd <= 0.0) continue;
                float sw = 1.0 / (pc.ivp * vec4(ndcOf(vec2(tp), sd), 1.0)).w;
                float diff = C.w - sw;
                if (diff > bias + 2.5 * g * length(win - win0) && diff < thick) occ += wt;
            }
            occ = wsum > 0.0 ? occ / wsum : 0.0;
            contact = 1.0 - occ * facing;
        }
    }
    outV = vec4(ao, contact, 0.0, 1.0);
}
