# NovaStar64's own source files, grouped by component. Listed by hand rather than
# globbed so a new file is a deliberate, reviewable change; add yours to the group it
# belongs to. Included from CMakeLists.txt.

set(NOVA64_SOURCES
    # Common utilities
    common/DateTime.cpp
    common/DynamicLibrary.cpp
    common/File.cpp
    common/IniFile.cpp
    common/LockGuard.cpp
    common/log.cpp
    common/md5.cpp
    common/MemoryManagment.cpp
    common/Thread.cpp
    common/TimeStamp.cpp
    common/trace.cpp

    # N64 System files
    N64SYS/SysGlobals.cpp
    N64SYS/N64DD.cpp
    N64SYS/N64Rom.cpp
    N64SYS/MainSYS.cpp
    N64SYS/MachineState.cpp
    N64SYS/SaveState.cpp
    N64SYS/StateTrace.cpp        # cross-engine divergence tracing
    N64SYS/Interpreter/InterpreterOps.cpp
    N64SYS/MIPSY/TLB.cpp
    N64SYS/MIPSY/Systiming.cpp
    N64SYS/MIPSY/R4300Instruction.cpp
    N64SYS/MIPSY/Register.cpp
    N64SYS/MIPSY/Vmem.cpp

    # Plugins
    N64SYS/Plugins/PluginManager.cpp
    N64SYS/Plugins/GFXPlugin.cpp
    N64SYS/Plugins/AudioPlugin.cpp
    N64SYS/Plugins/InputPlugin.cpp
    N64SYS/Plugins/RSPPlugin.cpp

    # Memory Handlers
    N64SYS/Memoryhandler/AudioInterfaceHandler.cpp
    N64SYS/Memoryhandler/CachedRom.cpp
    N64SYS/Memoryhandler/CachedSysMemory.cpp
    N64SYS/Memoryhandler/DisplayCtrlReg.cpp
    N64SYS/Memoryhandler/MIPSYInterfaceHandler.cpp
    N64SYS/Memoryhandler/PifInterface.cpp
    N64SYS/Memoryhandler/PIInterFace.cpp
    N64SYS/Memoryhandler/RDRAMInterFaceHandler.cpp
    N64SYS/Memoryhandler/RDRAMRegHandler.cpp
    N64SYS/Memoryhandler/RomMemHandler.cpp
    N64SYS/Memoryhandler/SIInterfaceHandler.cpp
    N64SYS/Memoryhandler/SPregistersHandler.cpp
    N64SYS/Memoryhandler/UnCachedRom.cpp
    N64SYS/Memoryhandler/UnCachedSysMemory.cpp
    N64SYS/Memoryhandler/VideoInterfaceHandler.cpp
    N64SYS/Savetypes/Eeprom.cpp
    N64SYS/Savetypes/FRam.cpp
    N64SYS/Savetypes/SRam.cpp
    N64SYS/Recomp/CodeBlock.cpp
    N64SYS/Recomp/CodeSection.cpp
    N64SYS/Recomp/ExitInfo.cpp
    N64SYS/Recomp/FunctionMap.cpp
    N64SYS/Recomp/LoopAnalysis.cpp
    N64SYS/Recomp/RegBase.cpp
    N64SYS/Recomp/Recompiler.cpp
    N64SYS/Recomp/RecompilerMemory.cpp
    N64SYS/Recomp/RegInfo.cpp
    N64SYS/Recomp/x64-86/x64RecompilerOps.cpp
    N64SYS/Recomp/x64-86/x64ops.cpp
    N64SYS/Recomp/x64-86/x64RegInfo.cpp
    # AArch64 (Apple Silicon / ARM64) recompiler backend.  Like the x64-86
    # sources above, these are listed unconditionally and self-gate with
    # `#if defined(__aarch64__) || defined(_M_ARM64)`, so they compile to empty
    # translation units on non-ARM64 hosts.
    N64SYS/Recomp/Arm64/arm64ops.cpp
    N64SYS/Recomp/Arm64/arm64RecompilerOps.cpp

    # NovaRSP - RSP plugin (CPU core)
    NovaRSP/CPU/RspClamp.cpp
    NovaRSP/CPU/RSPCpu.cpp
    NovaRSP/CPU/RSPInstruction.cpp
    NovaRSP/CPU/RSPInterpreterOps.cpp
    NovaRSP/CPU/RSPRegisterHandler.cpp
    NovaRSP/CPU/RspSystem.cpp

    # NovaRSP - RSP plugin (HLE audio / task / graphics)
    NovaRSP/HLE/alist.cpp
    NovaRSP/HLE/arithmetics.cpp
    NovaRSP/HLE/audio.cpp
    NovaRSP/HLE/AudioOutput.cpp
    NovaRSP/HLE/hle.cpp
    NovaRSP/HLE/mem.cpp
    NovaRSP/HLE/TaskDispatcher.cpp
    NovaRSP/HLE/ucodes.cpp

    # NovaRSP - RSP plugin (settings / misc)
    NovaRSP/Settings/RspSettings.cpp
    NovaRSP/RSPDebugger.cpp

    # NovaRSP - RSP recompiler (block-at-a-time; one code emitter per CPU)
    NovaRSP/Jit/RspJit.cpp
    NovaRSP/Jit/RspJitKernels.cpp
    NovaRSP/Jit/RspJitA64.cpp
    NovaRSP/Jit/RspJitX64.cpp
    NovaRSP/Jit/RspJitNoBackend.cpp
    NovaRSP/RSPInfo.cpp
    NovaRSP/Test/RspDiffTest.cpp

    # NovaInput - Keyboard and game controller input
    NovaInput/KeyboardInput.cpp
    NovaInput/KeyBindings.cpp
    NovaInput/GamepadInput.cpp

    # NovaAudio - Built-in audio plugin
    NovaAudio/NovaAudioPlugin.cpp

    # NovaGraphics - Graphics plugin (Common)
    NovaGraphics/Common/BitUtils.cpp
    NovaGraphics/Common/ClipMath.cpp
    NovaGraphics/Common/Logger.cpp
    NovaGraphics/Common/SIMDMath.cpp
    NovaGraphics/Common/VectorMath.cpp

    # NovaGraphics - Graphics plugin (Compat)
    NovaGraphics/Compat/GameDatabase.cpp

    # NovaGraphics - Graphics plugin (Core)
    NovaGraphics/Core/DepthBuffer.cpp
    NovaGraphics/Core/FramebufferManager.cpp
    NovaGraphics/Core/N64Math.cpp
    NovaGraphics/Core/RDPCommand.cpp
    NovaGraphics/Core/RDPProcessor.cpp
    NovaGraphics/Core/RDPProcessor_State.cpp
    NovaGraphics/Core/RDPProcessor_Texture.cpp
    NovaGraphics/Core/RDPProcessor_Draw.cpp
    NovaGraphics/Core/RDPProcessor_Batch.cpp

    # NovaGraphics - Graphics plugin (Debug — ImGuiDebugUI only; rest removed as dead)
    NovaGraphics/Debug/ImGuiDebugUI.cpp

    # NovaGraphics - Graphics plugin (Microcode)
    NovaGraphics/Microcode/GBIHandler.cpp
    NovaGraphics/Microcode/MicrocodeDispatcher.cpp
    NovaGraphics/Microcode/MicrocodeRegistry.cpp

    # NovaGraphics - Graphics plugin (Plugin)
    NovaGraphics/Plugin/FrameCallback.cpp
    NovaGraphics/Plugin/NovaGraphicsGFXPlugin.cpp
    NovaGraphics/Plugin/PluginConfig.cpp
    NovaGraphics/Plugin/VideoPlugin.cpp

    # NovaGraphics - Graphics plugin (Renderer)
    NovaGraphics/Renderer/GL/GLShaderCompiler.cpp
    NovaGraphics/Renderer/GL/GLShaderProgram.cpp
    NovaGraphics/Renderer/GLBackend.cpp
    NovaGraphics/Renderer/GLRenderer.cpp
    NovaGraphics/Renderer/PostProcessing.cpp
    NovaGraphics/Renderer/RendererFactory.cpp
    NovaGraphics/Renderer/ShaderGenerator.cpp
    NovaGraphics/Renderer/ShaderManager.cpp

    # NovaGraphics - Graphics plugin (Renderer/VK - real impl via volk loader)
    NovaGraphics/Renderer/VK/VKBackend.cpp
    NovaGraphics/Renderer/VK/VKInstance.cpp
    NovaGraphics/Renderer/VK/VKDevice.cpp
    NovaGraphics/Renderer/VK/VKSwapchain.cpp
    NovaGraphics/Renderer/VK/VKMemory.cpp
    NovaGraphics/Renderer/VK/VKBuffer.cpp
    NovaGraphics/Renderer/VK/VKTexture.cpp
    NovaGraphics/Renderer/VK/VKShader.cpp
    NovaGraphics/Renderer/VK/VKPipeline.cpp
    NovaGraphics/Renderer/VK/VKRenderTarget.cpp
    NovaGraphics/Renderer/VK/VKAccelStruct.cpp
    NovaGraphics/Renderer/VK/VKRTPipeline.cpp
    NovaGraphics/Renderer/VK/VKComputeRaytracer.cpp
    NovaGraphics/Renderer/VK/VKShaderCompiler.cpp

    # NovaGraphics - Graphics plugin (Ray tracing)
    NovaGraphics/RT/RTBVH.cpp

    # NovaGraphics - Graphics plugin (Texture)
    NovaGraphics/Texture/stb_image_impl.cpp
    NovaGraphics/Texture/HiResPack.cpp
    NovaGraphics/Texture/TextureCache.cpp

    # NovaGraphics - Graphics plugin (Window)
    NovaGraphics/Window/WindowManager.cpp

    # NovaLua - scripting
    NovaLua/LuaEngine.cpp

    # UI - launcher and tools
    UI/AppIcon.cpp
    UI/GameInfo.cpp
    UI/HexEditor.cpp
    UI/LauncherGames.cpp
    UI/LauncherPlay.cpp
    UI/LauncherSettings.cpp
    UI/LauncherSimulator.cpp
    UI/LauncherTools.cpp
    UI/NovaLauncher.cpp
    UI/UiKit.cpp

    main.cpp
)
