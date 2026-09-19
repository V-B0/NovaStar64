# MoltenVK

`libMoltenVK.dylib` is a prebuilt copy of [MoltenVK](https://github.com/KhronosGroup/MoltenVK),
the Vulkan-on-Metal layer, as a universal binary (x86-64 and arm64). macOS has no
system Vulkan loader, so NovaStar64 loads this library directly (`main.cpp` preloads
it, and the build copies it next to the executable). It reports Vulkan 1.4.

MoltenVK is licensed under the Apache License 2.0; the text is in [LICENSE](LICENSE)
in this folder, as that license requires when the library is redistributed. It is used
here unmodified.

To use a different build, replace `libMoltenVK.dylib` with one from the
[Vulkan SDK](https://vulkan.lunarg.com/) or the MoltenVK releases and rebuild.
