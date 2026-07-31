/**
 * @file RTComputeShaders.hpp
 * @brief GLSL sources for the three-pass compute ray tracer (MoltenVK-compatible).
 *
 * MoltenVK exposes no hardware RT, so we traverse a CPU-built compact BVH
 * (RTBVH.hpp) in compute. Geometry is EYE-SPACE (camera at origin); primary rays
 * are unprojected through the inverse projection matrix.
 *
 * Rendered in THREE compute passes (see VKComputeRaytracer::RenderHybrid).
 * Passes 1–2 run at HALF RESOLUTION (¼ of the rays — the perf win); pass 3
 * composites at full resolution so the game's raster image stays sharp:
 *
 *   Pass 1 — TRACE (kRTTraceSrc, half res): primary ray + soft-shadow rays +
 *   stochastic AO per pixel. Writes:
 *     • lit  (rgba8)   — rgb = metal reflection color × mix (additive extras)
 *     • shd  (rgba8)   — rgb = interpolated BAKED VERTEX SHADE at the hit
 *                        (the N64's own lighting result), a = lit flag.
 *                        Used by the composite to DIVIDE the baked lighting
 *                        back out of the raster (de-lighting).
 *     • occ  (rgba8)   — r = shadow visibility, g = raw AO, b = reflection mix,
 *                        a = hit mask
 *     • gbuf (rgba16f) — rgb = SMOOTH eye-space shading normal (interpolated
 *                        vertex normals when lit, geometric fallback otherwise),
 *                        a = normalized depth.
 *
 *   Pass 2 — DENOISE (kRTDenoiseSrc, half res): edge-aware bilateral blur of the
 *   occ term guided by gbuf, then RT64-style temporal accumulation (history
 *   validated against last frame's G-buffer, capped 64 frames; the two
 *   frames-in-flight ping-pong as history storage). Writes hist.
 *
 *   Pass 3 — COMPOSITE (kRTCompositeSrc, full res): DE-LIGHT + RELIGHT.
 *   This is the key to matching sm64rt/RT64: those mods patch the game to
 *   output raw albedo and REPLACE all lighting. We can't patch the ROM, but we
 *   know the baked shade at every pixel (shd), so:
 *     albedo = raster / max(shade, ε)                  — remove N64 lighting
 *     final  = albedo × (ambient·AO + (1−ambient)·sun·NdotL·shadow)
 *              + specular + reflections                 — RT64 relight
 *   Unlit geometry (sky, effects — no vertex normals) passes through untouched.
 *
 * Modeled on RT64 (Lights.hlsli disk sampling, DirectRayGen.hlsl temporal
 * accumulation, ComposePS.hlsl compose: albedo × (direct + indirect)).
 *
 * Layout contract (must match RTBVH.hpp / RTUBOData in VKComputeRaytracer.hpp):
 *   BVHNode = 32 bytes : vec3 aabbMin; uint leftFirst; vec3 aabbMax; uint triCount;
 *   RTTri              : v0 v1 v2 (vec4 pos) n0 n1 n2 (vec4 nrm) c0 c1 c2 (vec4 col) uvec4 mat
 *   UBO lightDir.w     : sun angular size (0 = light off)
 *   UBO lightColor.w   : frame counter (temporal sampling rotation)
 *   UBO ambient.xyzw   : x=blur radius, y=normal power, z=depth threshold,
 *                        w=ambient level (0..1)
 */
// SPDX-License-Identifier: MIT
#pragma once

namespace Nova64::Video::RT {

// ─────────────────────────────────────────────────────────────────────────────
//  Pass 1 — TRACE (half resolution)
// ─────────────────────────────────────────────────────────────────────────────
inline const char* kRTTraceSrc = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;

struct BVHNode {
    vec3 aabbMin; uint leftFirst;
    vec3 aabbMax; uint triCount;
};
struct RTTri {
    vec4 v0; vec4 v1; vec4 v2;
    vec4 n0; vec4 n1; vec4 n2;
    vec4 c0; vec4 c1; vec4 c2;
    uvec4 mat;            // mat.x = flags
};

layout(std140, binding = 0) uniform UBO {
    mat4  invProj;    // clip -> eye (column-vector form, transposed by CPU)
    vec4  lightDir;   // xyz eye-space dir TO light, w = sun angular size (0=off)
    vec4  lightColor; // rgb, w = frame counter
    vec4  ambient;    // denoise params (unused in trace)
    vec4  viewport;   // N64 viewport {scaleX, transX, scaleY, transY}
    vec4  extra;      // x=history cap/64, y=exposure (unused in trace)
    mat4  proj;       // eye -> clip (forward; raster-depth prediction)
    vec4  misc;       // x = scene AABB diagonal
    uvec4 dims;       // x=FULL width y=FULL height z=nodeCount w=flags
} u;

// Window pixel -> NDC through the N64 viewport (screen = ndc*scale + trans, fb
// units, 320×240-class; scaleY is negative = y-flip). Fullscreen viewports
// reduce to the plain 2u-1 mapping, but title screens / letterboxed cutscenes
// use partial viewports — ignoring them offsets/scales every RT term against
// the raster (ghost images, misplaced shadows).
vec2 PixelToNdc(vec2 uv) {
    vec2 scale = vec2(u.viewport.x, u.viewport.z);
    vec2 trans = vec2(u.viewport.y, u.viewport.w);
    if (abs(scale.x) < 1.0 || abs(scale.y) < 1.0) {  // unset → fullscreen
        scale = vec2(160.0, -120.0);
        trans = vec2(160.0,  120.0);
    }
    vec2 fb = uv * vec2(320.0, 240.0);
    return (fb - trans) / scale;
}

layout(std430, binding = 1) readonly buffer Nodes { BVHNode nodes[]; };
layout(std430, binding = 2) readonly buffer Tris  { RTTri  tris[];  };
layout(rgba8,  binding = 3) uniform writeonly image2D litImg;   // rgb=refl extras
layout(rgba8,  binding = 4) uniform writeonly image2D occImg;   // r=shadow g=ao b=reflMix a=hit
layout(rgba16f,binding = 5) uniform writeonly image2D gbufImg;  // rgb=normal a=depth
layout(rgba8,  binding = 6) uniform writeonly image2D shdImg;   // rgb=baked shade a=litFlag
layout(rgba8,  binding = 7) uniform writeonly image2D giImg;    // rgb=raw indirect irradiance

const uint RT_F_SHADOWS     = 1u;
const uint RT_F_LIGHTING    = 2u;
const uint RT_F_REFLECTIONS = 4u;
const uint RT_F_DEBUG       = 8u;

const uint RT_MAT_TRANSLUCENT = 1u;   // blended (water): hit, never occlude
const uint RT_MAT_REFLECTIVE  = 2u;   // env-mapped metal
const uint RT_MAT_CUTOUT      = 4u;   // billboard: invisible to all rays

const float T_MIN = 1e-3;
const float T_MAX = 1e30;

struct Hit { float t; vec3 bary; uint tri; };

bool slabHit(vec3 ro, vec3 invDir, vec3 bmin, vec3 bmax, float tmax) {
    vec3 t0 = (bmin - ro) * invDir;
    vec3 t1 = (bmax - ro) * invDir;
    vec3 ts = min(t0, t1);
    vec3 tb = max(t0, t1);
    float tN = max(max(ts.x, ts.y), ts.z);
    float tF = min(min(tb.x,  tb.y),  tb.z);
    return tN <= tF && tF >= 0.0 && tN < tmax;
}

// Double-sided Möller–Trumbore (N64 winding is not globally consistent).
bool triHit(vec3 ro, vec3 rd, RTTri tri, float tmax, out float tOut, out vec3 baryOut) {
    vec3  e1  = tri.v1.xyz - tri.v0.xyz;
    vec3  e2  = tri.v2.xyz - tri.v0.xyz;
    vec3  p   = cross(rd, e2);
    float det = dot(e1, p);
    if (abs(det) < 1e-8) return false;
    float idet = 1.0 / det;
    vec3  tv  = ro - tri.v0.xyz;
    float uu  = dot(tv, p) * idet;
    if (uu < 0.0 || uu > 1.0) return false;
    vec3  q   = cross(tv, e1);
    float vv  = dot(rd, q) * idet;
    if (vv < 0.0 || uu + vv > 1.0) return false;
    float t   = dot(e2, q) * idet;
    if (t <= T_MIN || t >= tmax) return false;
    tOut    = t;
    baryOut = vec3(1.0 - uu - vv, uu, vv);
    return true;
}

// Alpha-material visibility (the BVH has no texture alpha):
//   • CUTOUT billboards are invisible to ALL rays. Their transparent regions
//     show the background, and the composite relights the background — any
//     special treatment of the quad draws a glowing/dark seam around the
//     sprite. Passing through gives corners the EXACT background treatment.
//   • TRANSLUCENT (water/glass) IS hit by primary rays (reflections, shadow
//     modulation on the surface) but never occludes shadow/AO rays.
bool traverse(vec3 ro, vec3 rd, float tmax, out Hit hit) {
    hit.t = tmax; hit.tri = 0xffffffffu; hit.bary = vec3(0.0);
    vec3 invDir = 1.0 / rd;
    uint stack[48]; int sp = 0;
    stack[sp++] = 0u;
    bool found = false;
    while (sp > 0) {
        uint ni = stack[--sp];
        BVHNode n = nodes[ni];
        if (!slabHit(ro, invDir, n.aabbMin, n.aabbMax, hit.t)) continue;
        if (n.triCount > 0u) {
            for (uint i = 0u; i < n.triCount; ++i) {
                uint ti = n.leftFirst + i;
                if ((tris[ti].mat.x & RT_MAT_CUTOUT) != 0u) continue;
                float t; vec3 b;
                if (triHit(ro, rd, tris[ti], hit.t, t, b)) {
                    hit.t = t; hit.tri = ti; hit.bary = b; found = true;
                }
            }
        } else if (sp < 46) {
            stack[sp++] = n.leftFirst;
            stack[sp++] = n.leftFirst + 1u;
        }
    }
    return found;
}

// Any-hit for shadow/AO rays. tmin = minimum occluder distance: hits closer
// than this are ignored, which stops adjacent polygons of the same low-poly
// surface from shadowing each other (per-polygon shadow patches). RT64 solves
// the same problem with its per-material shadowRayBias on the ray's tMin.
bool occluded(vec3 ro, vec3 rd, float tmin, float tmax, uint skipTri) {
    vec3 invDir = 1.0 / rd;
    uint stack[48]; int sp = 0;
    stack[sp++] = 0u;
    while (sp > 0) {
        uint ni = stack[--sp];
        BVHNode n = nodes[ni];
        if (!slabHit(ro, invDir, n.aabbMin, n.aabbMax, tmax)) continue;
        if (n.triCount > 0u) {
            for (uint i = 0u; i < n.triCount; ++i) {
                uint ti = n.leftFirst + i;
                if (ti == skipTri) continue;
                if ((tris[ti].mat.x & (RT_MAT_TRANSLUCENT | RT_MAT_CUTOUT)) != 0u) continue;
                float t; vec3 b;
                if (triHit(ro, rd, tris[ti], tmax, t, b) && t > tmin) return true;
            }
        } else if (sp < 46) {
            stack[sp++] = n.leftFirst;
            stack[sp++] = n.leftFirst + 1u;
        }
    }
    return false;
}

// Raw (noisy) cosine-weighted AO + ONE-BOUNCE GI from the same 4 hemisphere
// rays — denoised in pass 2. Closest-hit rays (short range): a hit contributes
// the hit surface's baked shade as BOUNCE LIGHT (color bleeding — grass tints
// feet, lava glows on walls) and counts as occlusion; a miss contributes sky.
// `phase` rotates the golden-angle spiral per pixel AND per frame so the
// temporal accumulator converges instead of re-averaging one pattern.
// Returns irradiance rgb; AO scalar via `aoOut`.
vec3 gatherIndirect(vec3 pos, vec3 n, uint skipTri, float aoDist, float tmin,
                    float phase, out float aoOut) {
    const uint K = 4u;
    const vec3 kSky = vec3(0.94, 0.98, 1.06);
    vec3 up = (abs(n.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t  = normalize(cross(up, n));
    vec3 b  = cross(n, t);
    float occ = 0.0;
    vec3  irr = vec3(0.0);
    for (uint i = 0u; i < K; ++i) {
        float xi = (float(i) + 0.5) / float(K);
        float st = sqrt(xi);
        float ct = sqrt(1.0 - xi);
        float phi = float(i) * 2.3999632 + phase;   // rotated golden-angle spiral
        vec3 d   = t * (cos(phi) * st) + b * (sin(phi) * st) + n * ct;
        Hit gh;
        if (traverse(pos, d, aoDist, gh)) {
            if (gh.t > tmin) occ += 1.0;
            RTTri gt = tris[gh.tri];
            vec3 bounce = gh.bary.x * gt.c0.rgb + gh.bary.y * gt.c1.rgb
                        + gh.bary.z * gt.c2.rgb;
            irr += bounce * 0.75 * (1.0 - gh.t / aoDist);
        } else {
            irr += kSky;
        }
    }
    aoOut = 1.0 - (occ / float(K)) * 0.60;
    return min(irr / float(K), vec3(1.5));
}

void main() {
    uvec2 rdim = (u.dims.xy + 1u) / 2u;              // half-res target size
    uvec2 pix = gl_GlobalInvocationID.xy;
    if (pix.x >= rdim.x || pix.y >= rdim.y) return;
    ivec2 ip = ivec2(pix);

    // Defaults (miss).
    vec3  extras    = vec3(0.0);
    float shadowVis = 1.0;
    float aoOut     = 1.0;
    float reflMix   = 0.0;
    float hitMask   = 0.0;
    vec4  gOut      = vec4(0.0, 0.0, 1.0, -1.0);
    vec4  shdOut    = vec4(1.0, 1.0, 1.0, 0.0);   // shade=1 (no de-light), unlit
    vec3  irrOut    = vec3(1.0);                  // neutral indirect

    // Primary ray (eye-space; camera at origin) through the half-res pixel,
    // mapped through the N64 viewport (scaleY sign supplies the y-flip).
    vec2 ndc = PixelToNdc((vec2(pix) + 0.5) / vec2(rdim));
    vec4 tgt = u.invProj * vec4(ndc, 1.0, 1.0);
    vec3 rd  = normalize(tgt.xyz / tgt.w);
    vec3 ro  = vec3(0.0);

    uint flags = u.dims.w;

    Hit h;
    if (traverse(ro, rd, T_MAX, h)) {
        hitMask = 1.0;
        RTTri tri = tris[h.tri];
        vec3 pos  = h.bary.x * tri.v0.xyz + h.bary.y * tri.v1.xyz + h.bary.z * tri.v2.xyz;

        // Flat geometric normal (robust, used for the ray-origin offset).
        vec3 fn = normalize(cross(tri.v1.xyz - tri.v0.xyz, tri.v2.xyz - tri.v0.xyz));
        if (dot(fn, rd) > 0.0) fn = -fn;

        // SMOOTH shading normal: interpolated eye-space vertex normals
        // (submitted by GBIHandler for lit geometry; zero when unlit → fall
        // back to the geometric normal). This removes the per-polygon faceting
        // in both lighting and shadow gating.
        vec3 ns = h.bary.x * tri.n0.xyz + h.bary.y * tri.n1.xyz + h.bary.z * tri.n2.xyz;
        bool isLit = dot(ns, ns) > 0.25;
        ns = isLit ? normalize(ns) : fn;
        if (dot(ns, rd) > 0.0) ns = -ns;

        // Baked vertex shade at the hit — the N64's own lighting output, which
        // the composite divides out of the raster to recover raw albedo.
        //
        // TRANSLUCENT hits (water/glass — cutouts never reach here): the pixel
        // blends surface and background, so de-lighting with the surface's
        // shade is wrong. Force shade=1 (no de-light) + unlit path: shadow
        // modulation and reflections only.
        bool translucentHit = (tri.mat.x & RT_MAT_TRANSLUCENT) != 0u;
        vec3 shade = h.bary.x * tri.c0.rgb + h.bary.y * tri.c1.rgb + h.bary.z * tri.c2.rgb;
        shdOut = translucentHit ? vec4(1.0, 1.0, 1.0, 0.0)
                                : vec4(shade, isLit ? 1.0 : 0.0);

        float sceneDiag = max(length(nodes[0].aabbMax - nodes[0].aabbMin), 1e-3);

        float sunSize    = u.lightDir.w;
        bool  validLight = ((flags & RT_F_SHADOWS) != 0u)
                        && (sunSize > 1e-4)
                        && (dot(u.lightDir.xyz, u.lightDir.xyz) > 1e-6);
        vec3  L     = validLight ? normalize(u.lightDir.xyz) : vec3(0.0, 1.0, 0.0);
        float NdotL = dot(ns, L);

        // Ray-origin offset + minimum occluder distance (both scene-scaled).
        // shadowMin is the anti-per-polygon guard: occluders within it are
        // treated as part of this surface. It must stay SMALL — at 0.006 of a
        // full SM64 level it exceeded a character's height, so characters cast
        // no shadow at all (their whole body was inside the ignore radius).
        float slope     = clamp(1.0 - max(NdotL, 0.0), 0.0, 1.0);
        float bias      = max(sceneDiag * 4e-4, h.t * 0.0015) * (1.0 + 2.5 * slope);
        float shadowMin = sceneDiag * 0.0015;
        vec3  shadowOrig = pos + fn * bias;

        // Per-pixel interleaved-gradient noise + per-frame golden-ratio rotation
        // (textureless stand-in for RT64's animated blue-noise texture). Shared
        // by the shadow disk sampling and the AO spiral.
        float ign   = fract(52.9829189 * fract(0.06711056 * float(pix.x)
                                             + 0.00583715 * float(pix.y)));
        uint  frame = uint(u.lightColor.w);

        // RT64-style soft shadow: jittered rays across the sun's disk, averaged;
        // temporal accumulation in pass 2 converges it. Back-lit surfaces
        // (NdotL <= 0) skip the rays — the composite's NdotL darkens them.
        if (validLight && NdotL > 0.0) {
            vec3 sup = (abs(L.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            vec3 stx = normalize(cross(sup, L));
            vec3 sty = cross(L, stx);
            const uint S = 4u;
            float vis = 0.0;
            for (uint i = 0u; i < S; ++i) {
                float fi = float(frame * S + i);
                float a  = 6.2831853 * fract(ign + fi * 0.61803398875);
                float r  = sqrt(fract(ign * 13.0 + fi * 0.38196601125));
                vec3 dir = normalize(L + (stx * cos(a) + sty * sin(a)) * (r * sunSize));
                if (!occluded(shadowOrig, dir, shadowMin, sceneDiag, h.tri)) vis += 1.0;
            }
            shadowVis = vis / float(S);
        }

        // Raw AO + one-bounce GI (denoised in pass 2), sampling spiral rotated
        // per pixel+frame. Skipped on translucent hits (through-water darkening).
        if ((flags & RT_F_LIGHTING) != 0u && !translucentHit) {
            float aoPhase = 6.2831853 * fract(ign + float(frame) * 0.61803398875);
            irrOut = gatherIndirect(shadowOrig, ns, h.tri, sceneDiag * 0.08,
                                    shadowMin * 0.25, aoPhase, aoOut);
        }

        // Reflections — MATERIAL-GATED to metal and water only:
        //   metal = env-mapped surfaces (RT_MAT_REFLECTIVE from G_TEXTURE_GEN),
        //           fixed strong mix;
        //   water = translucent AND up-facing (eye-space; N64 cameras stay
        //           near-upright so eye-up ≈ world-up), Fresnel-weighted so it
        //           mirrors hard at grazing angles and stays subtle from above.
        // A reflection ray that escapes the scene picks up a synthetic sky
        // gradient — water/metal always reflect *something*, like RT64.
        bool metal = (tri.mat.x & RT_MAT_REFLECTIVE) != 0u;
        bool water = translucentHit && ns.y > 0.55;
        vec3 reflCol = vec3(0.0);
        if ((flags & RT_F_REFLECTIONS) != 0u && (metal || water)) {
            vec3 refl = reflect(rd, ns);
            Hit  rh;
            if (traverse(shadowOrig, refl, sceneDiag, rh)) {
                RTTri rt = tris[rh.tri];
                reflCol  = rh.bary.x * rt.c0.rgb + rh.bary.y * rt.c1.rgb + rh.bary.z * rt.c2.rgb;
            } else {
                float upw = clamp(refl.y * 0.5 + 0.5, 0.0, 1.0);
                reflCol = mix(vec3(0.55, 0.65, 0.85), vec3(0.88, 0.94, 1.0), upw);
            }
            float fres = pow(1.0 - abs(dot(ns, rd)), 2.0);
            reflMix = metal ? 0.45 : (0.18 + 0.32 * fres);
        }
        extras = reflCol * reflMix;

        gOut = vec4(ns, h.t / sceneDiag);
    }

    imageStore(litImg,  ip, vec4(extras, 0.0));
    imageStore(shdImg,  ip, shdOut);
    imageStore(occImg,  ip, vec4(shadowVis, aoOut, reflMix, hitMask));
    imageStore(gbufImg, ip, gOut);
    imageStore(giImg,   ip, vec4(irrOut * 0.66, 1.0));   // ×0.66: keep ≤1 in rgba8
}
)GLSL";

// ─────────────────────────────────────────────────────────────────────────────
//  Pass 2 — DENOISE (half resolution)
// ─────────────────────────────────────────────────────────────────────────────
inline const char* kRTDenoiseSrc = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;

layout(std140, binding = 0) uniform UBO {
    mat4  invProj;
    vec4  lightDir;   // w = sun angular size
    vec4  lightColor; // w = frame counter
    vec4  ambient;    // x=blur radius, y=normal power, z=depth threshold, w=ambient
    vec4  viewport;   // N64 viewport (unused here; keeps std140 layout in sync)
    vec4  extra;      // x = history cap/64 (CPU lowers it while the camera moves)
    mat4  proj;       // (layout sync)
    vec4  misc;       // (layout sync)
    uvec4 dims;       // x=FULL width y=FULL height z=nodeCount w=flags
} u;

layout(rgba8,  binding = 1) uniform readonly  image2D occImg;      // r=shadow g=ao b=reflMix a=hit
layout(rgba16f,binding = 2) uniform readonly  image2D gbufImg;     // rgb=normal a=depth
layout(rgba8,  binding = 3) uniform readonly  image2D prevHistImg; // last frame's hist
layout(rgba16f,binding = 4) uniform readonly  image2D prevGbufImg; // last frame's gbuf
layout(rgba8,  binding = 5) uniform writeonly image2D histImg;     // r=shadow g=ao a=len/64
layout(rgba8,  binding = 6) uniform readonly  image2D giImg;       // raw indirect irradiance
layout(rgba8,  binding = 7) uniform readonly  image2D prevGiHistImg; // last frame's GI accum
layout(rgba8,  binding = 8) uniform writeonly image2D giHistImg;     // GI accum out

void main() {
    uvec2 rdim = (u.dims.xy + 1u) / 2u;
    uvec2 pix = gl_GlobalInvocationID.xy;
    if (pix.x >= rdim.x || pix.y >= rdim.y) return;
    ivec2 ip = ivec2(pix);

    vec4 oc0 = imageLoad(occImg, ip);
    // Miss → reset history so stale terms don't bleed back in.
    if (oc0.a < 0.5) {
        imageStore(histImg,   ip, vec4(1.0, 1.0, 0.0, 0.0));
        imageStore(giHistImg, ip, vec4(0.66, 0.66, 0.66, 0.0));   // neutral irr
        return;
    }

    vec4  g0 = imageLoad(gbufImg, ip);
    vec3  n0 = g0.xyz;
    float d0 = g0.w;

    int   R       = int(u.ambient.x + 0.5);
    float nPow    = max(u.ambient.y, 1.0);
    float dThresh = max(u.ambient.z, 1e-4);

    // ── Spatial: edge-aware (bilateral) blur of the shadow/AO/GI terms ───────
    // Also tracks the neighborhood min/max, used below to clamp temporal
    // history (TAA-style) so moving shadows don't ghost.
    float sSum = 0.0, aSum = 0.0, wSum = 0.0;
    vec3  gSum = vec3(0.0);
    float sMin = 1.0, sMax = 0.0, aMin = 1.0, aMax = 0.0;
    float sig2 = 2.0 * float(R) * float(R) + 1.0;
    for (int dy = -R; dy <= R; ++dy) {
        for (int dx = -R; dx <= R; ++dx) {
            ivec2 p = ip + ivec2(dx, dy);
            if (p.x < 0 || p.y < 0 || p.x >= int(rdim.x) || p.y >= int(rdim.y)) continue;
            vec4 oc = imageLoad(occImg, p);
            if (oc.a < 0.5) continue;                       // skip background
            vec4 gi = imageLoad(gbufImg, p);
            float wn = pow(max(dot(n0, gi.xyz), 0.0), nPow); // normal similarity
            float wd = exp(-abs(d0 - gi.w) / dThresh);        // depth similarity
            float ws = exp(-float(dx * dx + dy * dy) / sig2); // spatial gaussian
            float wgt = wn * wd * ws;
            sSum += oc.r * wgt;
            aSum += oc.g * wgt;
            gSum += imageLoad(giImg, p).rgb * wgt;
            wSum += wgt;
            if (wgt > 1e-3) {
                sMin = min(sMin, oc.r); sMax = max(sMax, oc.r);
                aMin = min(aMin, oc.g); aMax = max(aMax, oc.g);
            }
        }
    }
    float sSpat = (wSum > 0.0) ? (sSum / wSum) : 1.0;
    float aSpat = (wSum > 0.0) ? (aSum / wSum) : 1.0;
    vec3  gSpat = (wSum > 0.0) ? (gSum / wSum) : vec3(0.66);

    // ── Temporal: RT64-style history accumulation ─────────────────────────────
    // historyWeight = depth/normal-validated; blend = lerp(hist, new, 1/len).
    // Anti-ghosting guards on top of RT64's scheme (we have no motion vectors):
    //  • history clamped to the spatial neighborhood min/max;
    //  • CPU cuts the cap while the camera TRANSLATES (AABB-center motion);
    //  • the normal validation is SHARP (pow 32): an object ROTATING in place
    //    (menu Mario head) barely moves its AABB and keeps per-pixel depth, so
    //    only the rotating normals betray it — pow 4 let its shadow terms lag
    //    the rotation ("shadows floating just in front of the head").
    vec4  ph      = imageLoad(prevHistImg, ip);
    vec4  pgi     = imageLoad(prevGiHistImg, ip);
    vec4  pg      = imageLoad(prevGbufImg, ip);
    float wNormal = pow(max(dot(n0, pg.xyz), 0.0), 32.0);
    float wDepth  = exp(-abs(d0 - pg.w) / dThresh);
    float histCap = max(u.extra.x * 64.0, 1.0);
    float histLen = ph.a * 64.0 * wNormal * wDepth;   // reject stale history
    histLen = min(histLen + 1.0, histCap);
    float alpha = 1.0 / histLen;
    float sPrev = clamp(ph.r, sMin, sMax);
    float aPrev = clamp(ph.g, aMin, aMax);
    float sT = mix(sPrev, sSpat, alpha);
    float aT = mix(aPrev, aSpat, alpha);
    vec3  gT = mix(pgi.rgb, gSpat, alpha);
    imageStore(histImg,   ip, vec4(sT, aT, 0.0, histLen / 64.0));
    imageStore(giHistImg, ip, vec4(gT, 1.0));
}
)GLSL";

// ─────────────────────────────────────────────────────────────────────────────
//  Pass 3 — COMPOSITE (full resolution)
// ─────────────────────────────────────────────────────────────────────────────
inline const char* kRTCompositeSrc = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;

layout(std140, binding = 0) uniform UBO {
    mat4  invProj;
    vec4  lightDir;   // xyz eye-space dir TO light, w = sun angular size
    vec4  lightColor; // rgb sun color, w = frame counter
    vec4  ambient;    // w = ambient level (0..1)
    vec4  viewport;   // N64 viewport {scaleX, transX, scaleY, transY}
    vec4  extra;      // y = exposure
    mat4  proj;       // eye -> clip (forward; raster-depth prediction)
    vec4  misc;       // x = scene AABB diagonal
    uvec4 dims;       // x=FULL width y=FULL height z=nodeCount w=flags
} u;

// Window pixel -> NDC through the N64 viewport (see trace shader).
vec2 PixelToNdc(vec2 uv) {
    vec2 scale = vec2(u.viewport.x, u.viewport.z);
    vec2 trans = vec2(u.viewport.y, u.viewport.w);
    if (abs(scale.x) < 1.0 || abs(scale.y) < 1.0) {
        scale = vec2(160.0, -120.0);
        trans = vec2(160.0,  120.0);
    }
    vec2 fb = uv * vec2(320.0, 240.0);
    return (fb - trans) / scale;
}

layout(rgba8,  binding = 1) uniform readonly  image2D rasImg;    // full-res raster
layout(rgba8,  binding = 2) uniform readonly  image2D litImg;    // half: refl extras
layout(rgba8,  binding = 3) uniform readonly  image2D occImg;    // half: b=reflMix a=hit
layout(rgba8,  binding = 4) uniform readonly  image2D histImg;   // half: r=shadow g=ao
layout(rgba16f,binding = 5) uniform readonly  image2D gbufImg;   // half: rgb=normal
layout(rgba8,  binding = 6) uniform readonly  image2D shdImg;    // half: rgb=shade a=lit
layout(rgba8,  binding = 7) uniform readonly  image2D giHistImg; // half: accumulated irr
layout(rgba8,  binding = 8) uniform writeonly image2D outImg;
layout(binding = 9) uniform sampler2D depthTex;                  // full-res raster depth

const uint RT_F_SHADOWS    = 1u;
const uint RT_F_DEBUG      = 8u;
const uint RT_F_DEBUG_TERM = 16u;   // visualize the lighting term (grayscale)

void main() {
    uvec2 pix = gl_GlobalInvocationID.xy;
    if (pix.x >= u.dims.x || pix.y >= u.dims.y) return;
    ivec2 ip = ivec2(pix);
    uvec2 rdim = (u.dims.xy + 1u) / 2u;
    uint flags = u.dims.w;
    vec3 dbgTerm = vec3(1.0);   // term-only debug view (RT_F_DEBUG_TERM)

    vec3 ras = imageLoad(rasImg, ip).rgb;

    // Joint bilateral upsample of the half-res RT term: 4 nearest half-res
    // taps, bilinear weights × hit mask × DEPTH similarity. The depth guide is
    // what stops a near object's dark self-shadow terms from bleeding onto the
    // background beside its silhouette (a floating shadow rim around close-up
    // models). Reference depth = the strongest valid tap.
    vec2  hc = (vec2(pix) + 0.5) * 0.5 - 0.5;
    ivec2 b0 = ivec2(floor(hc));
    vec2  f  = hc - vec2(b0);
    vec4  tOcc[4]; vec4 tGbuf[4]; float tW[4];
    float bestW = 0.0, refDepth = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 p = clamp(b0 + o, ivec2(0), ivec2(rdim) - 1);
        tOcc[k]  = imageLoad(occImg, p);
        tGbuf[k] = imageLoad(gbufImg, p);
        float wb = ((o.x == 0) ? (1.0 - f.x) : f.x) * ((o.y == 0) ? (1.0 - f.y) : f.y);
        tW[k] = wb * tOcc[k].a;
        if (tW[k] > bestW) { bestW = tW[k]; refDepth = tGbuf[k].w; }
    }
    float shadow = 0.0, ao = 0.0, reflMix = 0.0, litW = 0.0, wSum = 0.0;
    vec3  extras = vec3(0.0), nrm = vec3(0.0), shade = vec3(0.0), irr = vec3(0.0);
    for (int k = 0; k < 4; ++k) {
        float wgt = tW[k] * exp(-abs(tGbuf[k].w - refDepth) * 20.0);
        if (wgt <= 1e-4) continue;
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 p = clamp(b0 + o, ivec2(0), ivec2(rdim) - 1);
        vec4 hs = imageLoad(histImg, p);
        vec4 sh = imageLoad(shdImg, p);
        shadow  += hs.r * wgt;
        ao      += hs.g * wgt;
        reflMix += tOcc[k].b * wgt;
        litW    += sh.a * wgt;
        shade   += sh.rgb * wgt;
        extras  += imageLoad(litImg, p).rgb * wgt;
        irr     += imageLoad(giHistImg, p).rgb * wgt;
        nrm     += tGbuf[k].xyz * wgt;
        wSum    += wgt;
    }

    float ambientL = clamp(u.ambient.w, 0.0, 1.0);

    // View ray for this pixel (shared by the depth test, eye light, specular).
    vec2 vndc = PixelToNdc((vec2(pix) + 0.5) / vec2(u.dims.xy));
    vec4 vtgt = u.invProj * vec4(vndc, 1.0, 1.0);
    vec3 rd   = normalize(vtgt.xyz / vtgt.w);

    // ── RASTER-DEPTH GUARD ────────────────────────────────────────────────────
    // The BVH can't see ray-invisible geometry (cutout billboards, decals), so
    // an RT term computed for the surface BEHIND a tree would paint that
    // surface's shadows over the sprite. Compare in LINEAR eye depth — the
    // depth-buffer value is (ndc.z+1)/2 of the projection, which compresses
    // hugely with distance, so a fixed ndc-space tolerance goes blind past
    // short range (billboards leaked terms). Unproject the raster depth back
    // to eye space instead and compare ray distances relatively.
    bool covered = false;
    if (wSum >= 1e-3) {
        float zr   = texelFetch(depthTex, ip, 0).r;
        vec4  rEye = u.invProj * vec4(vndc, zr * 2.0 - 1.0, 1.0);
        if (abs(rEye.w) > 1e-6) {
            float tRas = length(rEye.xyz / rEye.w);        // raster surface distance
            float tRT  = refDepth * max(u.misc.x, 1e-3);   // RT surface distance
            covered = tRas < tRT * 0.97 - u.misc.x * 0.002;
        }
    }

    vec3 outColor;
    if (wSum < 1e-3 || covered) {
        outColor = ras;             // no geometry here / raster surface closer
    } else if (litW < wSum * 0.5) {
        // Unlit geometry (vertex-colored art, billboards, water): no de-light,
        // no NdotL — shadows cast ONTO it still land, and water's reflection
        // extras composite here. CAPPED AT 1: this path may only darken.
        // Exposure here would brighten the background seen through billboard
        // transparency, drawing a glowing quad around every sprite.
        float inv = 1.0 / wSum;
        shadow *= inv; ao *= inv; reflMix *= inv; extras *= inv;
        float mod = min((ambientL * ao + (1.0 - ambientL) * shadow) * u.extra.y, 1.0);
        dbgTerm  = vec3(mod);
        outColor = ras * mod * (1.0 - reflMix) + extras;
    } else {
        float inv = 1.0 / wSum;
        shadow *= inv; ao *= inv; reflMix *= inv; extras *= inv; shade *= inv;
        irr = irr * inv * 1.5;   // decode the ×0.66 rgba8 packing (1.0 = neutral)
        vec3 n = (dot(nrm, nrm) > 1e-6) ? normalize(nrm) : vec3(0.0, 0.0, 1.0);

        // ── DE-LIGHT: divide the N64's baked lighting brightness back out ────
        // raster ≈ texture × shade. Divide by the shade's MAX COMPONENT: SM64
        // bakes MATERIAL COLORS into its lights, so per-channel bleaches hue
        // and LUMINANCE over-brightens saturated materials (a blue shade's
        // luma is tiny → blown out). Max-component removes brightness, keeps
        // hue, and can't exceed 1 for the dominant channel. Tight clamp.
        //
        // FADE GUARD: games animate fades by darkening the shade toward zero
        // (title-screen head pop-in). De-lighting a fading surface UNDOES the
        // fade and washes it gray — so the de-light itself fades out as the
        // shade gets very dark, leaving the game's intended dark look.
        float shadeM  = max(max(shade.r, shade.g), max(shade.b, 0.10));
        float dlAmt   = smoothstep(0.10, 0.30, shadeM);
        vec3  albedo  = clamp(mix(ras, ras / shadeM, dlAmt), 0.0, 1.2);

        // ── RELIGHT (RT64 model: albedo × (indirect + direct)) ───────────────
        float sunSize    = u.lightDir.w;
        bool  validLight = ((flags & RT_F_SHADOWS) != 0u)
                        && (sunSize > 1e-4)
                        && (dot(u.lightDir.xyz, u.lightDir.xyz) > 1e-6);
        float NdotLw = 1.0;
        vec3  sunCol = vec3(1.0);
        vec3  L      = vec3(0.0, 1.0, 0.0);
        if (validLight) {
            L = normalize(u.lightDir.xyz);
            // Wrap diffuse: soft terminator so low-poly models don't band.
            NdotLw = clamp((dot(n, L) + 0.4) / 1.4, 0.0, 1.0);
            sunCol = clamp(u.lightColor.rgb, 0.0, 1.0);
            if (dot(sunCol, sunCol) < 1e-4) sunCol = vec3(1.0);
        }

        // Light budget sums to ≤ 1 (then exposure scales it):
        //   indirect — one-bounce GI irradiance (sky + color bleeding from
        //   nearby surfaces) scaled by AO, 70% of the ambient share; eye fill
        //   (RT64's camera light) the other 30%;
        //   direct — the rest, warm sun tint, shadowed.
        const vec3 kSunTint = vec3(1.06, 1.00, 0.90);
        const vec3 kSkyTint = vec3(0.94, 0.98, 1.06);
        float eyeF     = max(dot(n, -rd), 0.0);
        vec3  indirect = ambientL * (0.7 * ao * irr + 0.3 * eyeF * kSkyTint);
        vec3  direct   = (1.0 - ambientL) * NdotLw * shadow * sunCol * kSunTint;
        vec3  lightTerm = (indirect + direct) * u.extra.y;
        dbgTerm = lightTerm;

        // Specular from the denoised shadow term, tinted like the diffuse.
        float spec = 0.0;
        if (validLight) {
            vec3 H = normalize(L - rd);
            spec = pow(max(dot(n, H), 0.0), 32.0) * 0.22
                 * max(dot(n, L), 0.0) * shadow;
        }

        outColor = albedo * lightTerm * (1.0 - reflMix)
                 + extras + spec * sunCol;

        if ((flags & RT_F_DEBUG) != 0u) outColor = n * 0.5 + 0.5;
    }

    // Term-only view (NOVA64_RT_DEBUG=2): shows exactly what the RT lighting
    // term contains spatially — misalignment/ghosting bugs are obvious here.
    if ((flags & RT_F_DEBUG_TERM) != 0u) outColor = dbgTerm;

    if ((flags & RT_F_DEBUG) != 0u &&
        ((pix.x & 31u) == 0u || (pix.y & 31u) == 0u)) outColor = vec3(1.0, 1.0, 0.0);

    imageStore(outImg, ip, vec4(outColor, 1.0));
}
)GLSL";

} // namespace Nova64::Video::RT
