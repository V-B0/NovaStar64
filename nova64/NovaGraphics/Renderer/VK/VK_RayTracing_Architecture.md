# Vulkan Ray Tracing Architecture (MLE)

To implement hardware ray tracing (`VK_KHR_ray_tracing_pipeline`) in the SuperNova64 Vulkan backend (`VKBackend.cpp`), the following architectural changes must be made to support the MLE (Low-Level RSP) pipeline:

## 1. The Challenge of MLE Ray Tracing
In a pure MLE pipeline, the RSP executes native microcode and outputs raw RDP commands. The RDP commands contain 2D screen-space triangles (`scissored`, `projected`, and `flattened`), not 3D scene data. Hardware ray tracing requires 3D vertices, BVH (Bounding Volume Hierarchy) structures, and material properties.

## 2. The Hybrid LLE/HLE Approach
To support Ray Tracing, the emulator must intercept the 3D data *before* the RSP flattens it.

### Step 1: RDRAM Vertex Interception
The `MicrocodeDispatcher` (or the core LLE RSP) must track when the game loads vertices from RDRAM into RSP DMEM (e.g., via `gSPVertex`). 
- NovaGraphics must read the untransformed 3D vertex positions and normals directly from RDRAM.

### Step 2: Matrix Stack Interception
NovaGraphics must track the RSP's ModelView matrix stack to transform the intercepted RDRAM vertices into World Space. 
- *Note: This requires hooking into the RSP matrix multiplication instructions if running pure LLE, or parsing the `gSPMatrix` commands if using HLE microcodes.*

### Step 3: BVH Construction (Vulkan)
Once the 3D triangles are extracted:
1. Build Bottom-Level Acceleration Structures (BLAS) for each model/object in `VKBackend`.
2. Build a Top-Level Acceleration Structure (TLAS) for the entire scene per frame.
3. Update `VKPipeline.cpp` to initialize the Ray Tracing pipeline (`vkCreateRayTracingPipelinesKHR`) alongside the traditional rasterization pipeline.

### Step 4: Material Mapping (RDP Color Combiner to Ray Tracer)
The N64 Color Combiner logic (currently in `ShaderGenerator.cpp`) must be ported to a Vulkan Closest Hit Shader (`.rchit`). 
- The Raygen shader (`.rgen`) will cast rays from the camera.
- The Closest Hit shader will sample the High-Res Texture Pack textures (from `HiResTextures.hpp`) and apply the N64 lighting/combiner math to compute the final pixel color.

## Conclusion
Ray Tracing in MLE is entirely possible via Vulkan, but it fundamentally requires side-channeling 3D data out of the RSP *before* it becomes 2D RDP commands. You will need to implement a "Scene Extractor" layer in NovaGraphics before writing the Vulkan RT shaders.
