# The ray tracer

NovaStar64's ray tracer is a hybrid: the game is rasterised as usual, then a
ray-tracing pass adds shadows, ambient occlusion, colour bleeding and reflections
on top of the finished picture. It is switched on in Settings → Video →
*Ray-traced lighting* or with `NOVA64_RT=1`, and needs Vulkan and the high-level
graphics emulation (the low-level path never sees 3D geometry).

MoltenVK has no `VK_KHR_ray_tracing_pipeline`, so nothing here uses hardware ray
tracing. The BVH is built on the CPU and traversed in compute shaders, which also
means it runs on any Vulkan GPU. (`RTSceneManager::BuildAS` and `VKAccelStruct`
are a hardware path that was written for drivers that do have the extension; it
has never been run.)

## 1. Getting 3D out of an N64 frame

The RSP flattens a scene to 2D before the RDP sees it, so the ray tracer takes the
geometry earlier. `GBIHandler::DrawTriangle` already has each triangle's vertices
transformed into **eye space** (object space × the ModelView matrix, camera at the
origin), and after culling it hands a copy to the scene consumer
(`ISceneGeometryConsumer`, implemented by `RTSceneManager`) together with:

- the eye-space normals the RSP computed for lighting (zero when the triangle is
  drawn unlit) and the per-vertex shade the RSP produced,
- a material class derived from the RDP state: translucent (water), cutout
  (billboards, invisible to rays), reflective (environment-mapped metal), and
  whether the colour combiner multiplies by shade (`usesShadeInFinalColor`),
- the perspective projection and viewport in force.

Triangles drawn under an orthographic projection (HUD, menus, fades) are not
submitted: they live in a different space and would become invisible ghost planes
in front of the real scene.

## 2. Lights

N64 games light every object separately: SM64 gives each object its own light and
bakes its material colour into it, Zelda rewrites the sun every frame for the time
of day, and torches and fires are positional lights. So there is no "the light".
When `DrawTriangle` submits a triangle it also reports the lights in force
(`SceneLighting`: the first directional light, and the brightest point light when
positional lighting is on). `RTSceneManager` interns them into a per-frame **light
table** of up to 15 entries and stamps each triangle with the two indices.

The trace shader shadows every surface toward *its own* light, and the composite
relights it with the same one. Nothing is averaged or smoothed across frames, so a
light that moves moves its shadows on the same frame. Triangles drawn unlit (sky,
effects, vertex-coloured terrain) take the frame's dominant sun.

Light directions in the game are already eye-space, so they are used verbatim.
A point light's falloff is the RSP's own: distance weights z twice and
`D = 1 + (2·la·d + qa·d²/8) / 65536`.

## 3. The frame

Per frame, on the CPU: build the BVH (`RTBVH`: midpoint split on the longest axis,
32-byte nodes, at most two triangles per leaf; about 0.01–0.15 ms for 100–2000
triangles) and fill the uniform buffer, then upload both. On the GPU, three
compute passes read the finished raster image and write the result back onto the
swapchain:

1. **Trace** (half resolution). One primary ray per pixel through the inverse
   projection and the N64 viewport, then, at the hit: soft shadow rays toward the
   surface's sun (jittered across a small disc), a shadow ray toward its point
   light, and cosine-weighted hemisphere rays that give ambient occlusion *and*
   one bounce of colour (a hit contributes the hit surface's baked shade as bounce
   light, a miss contributes sky). Metal and water also get a reflection ray.
2. **Denoise** (half resolution). An edge-aware bilateral blur guided by normal and
   depth, then temporal accumulation (below).
3. **Composite** (full resolution). A joint-bilateral upsample of the half-res
   terms, then **de-light and relight**: the raster colour is divided by the shade
   the N64 baked in (the max component, so hue survives), giving an albedo, which
   is multiplied by the ray-traced lighting (indirect + sun + point light) and the
   reflections added. Anything the BVH can't see in front of the ray (billboards)
   is detected by comparing the raster depth buffer and left untouched.

## 4. Stable in motion

There are no motion vectors: triangles have no identity from one frame to the
next. Temporal history is therefore only trusted where the same surface still
covers the same pixel: the shading normal must agree and the eye distance match
within 0.6 % (`historyTrust` in both the trace and denoise shaders). Trust falls
off smoothly, so a slowly drifting view keeps a short history rather than a long
one, and history is clamped to the pixel's spatial neighbourhood (as in TAA) so an
edge cannot ghost. A pixel that fails the test restarts from the current frame.

The result: shadows, AO and colour bleeding converge to a smooth image while the
picture is still, and reset within a frame when something moves. History also
selects the *sample pattern*: a pixel with history rotates its pattern every frame
so the accumulation converges, while a pixel without it keeps one fixed pattern, so
noise stays put on screen instead of boiling.

## 5. Ray budget

A pixel with a long history has its noise averaged away, so the trace shader drops
its rays as history builds: from the full count (2 shadow, 4 AO/GI by default) for
a new or moving surface down to one each after eight frames. Static scenes cost
about 40 % less GPU time; moving ones about 10 % less (2 instead of 4 shadow
rays; going to one or two shadow rays changed the lighting term by under one grey
level against a 16-ray reference). Measured with `NOVA64_RT_STATS=1` on an M4 Pro,
averaged over a 6000-field SM64 run:

| Window | Before | After |
|---|---|---|
| 720p | 2.6 ms | 2.0 ms |
| 1440p | 9.8 ms | 7.5 ms |

Where the time goes at 1440p in motion, with the old 4 shadow rays: about 25 %
primary rays, 25 % shadow rays, 45 % AO/GI rays, the rest denoise, composite and
the two full-resolution copies. Cost follows the number of rays and pixels, not the
scene: these scenes have 100–2000 triangles, the BVH build takes well under a
millisecond, and a leaner triangle test or near-first traversal made no measurable
difference.

## 6. Limits

- One directional and one point light per triangle; the first directional light and
  the brightest point light are the ones used.
- Point lights have only been checked with a synthetic light, not a real game's.
- Rays ignore texture alpha (the BVH has none): cutouts are invisible to them and
  translucent surfaces never occlude.
- No motion vectors, so moving surfaces get single-frame quality.
- The N64's baked shade is divided out approximately, so surfaces whose combiner
  doesn't use shade are relit only by shadows and reflections.

## Tuning and debugging

See the `NOVA64_RT_*` rows of the environment-variable table in `CONTRIBUTING.md`.
`NOVA64_RT_DEBUG=2` shows only the lighting term, which is the quickest way to see
misalignment or ghosting; `NOVA64_RT_STATS=1` prints scene-update and GPU times.
