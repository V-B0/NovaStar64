/**
 * @file main.cpp
 * @brief SuperNova64 - Nintendo 64 Emulator Entry Point
 */

#include "common/log.hpp"
#include "common/path.hpp"
#include "N64SYS/MainSYS.hpp"
#include "N64SYS/N64Rom.hpp"
#include "N64SYS/SysGlobals.hpp"
#include "UI/NovaLauncher.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>

#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>

// macOS has no system Vulkan loader. The Vulkan backend reaches Vulkan through
// MoltenVK (Vulkan-on-Metal), which volk loads via dlopen("libMoltenVK.dylib").
// We ship libMoltenVK.dylib next to the executable; preload it by absolute path
// here (RTLD_GLOBAL) so volk's later bare-name dlopen resolves to this image —
// no DYLD_LIBRARY_PATH needed. Harmless if the dylib is absent (GL/other paths).
static void PreloadMoltenVK()
{
    char exePath[4096];
    uint32_t sz = sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &sz) != 0) return;
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path dylib = dir / "libMoltenVK.dylib";
    if (std::filesystem::exists(dylib, ec)) {
        if (!dlopen(dylib.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
            Log::infof("PreloadMoltenVK: dlopen failed: %s", dlerror());
        }
    }
}
#endif

static void PrintUsage(const char* programName)
{
    std::printf("SuperNova64 - Nintendo 64 Emulator\n");
    std::printf("Usage: %s [options] [rom_file]\n\n", programName);
    std::printf("Options:\n");
    std::printf("  --help, -h       Show this help message\n");
    std::printf("  --debug          Enable debug logging\n");
    std::printf("  --log <file>     Write log to file\n");
    std::printf("  --config <file>  Use specified config file\n");
    std::printf("  --frames <N>     Run for N frames (0 = unlimited, default)\n");
    std::printf("  --fps <N>        Target FPS (0=unlimited, 30, 60; default: 60)\n");
    std::printf("  --resolution <R> Window resolution: 480p, 720p, 960p, 1080p, 1440p, 4k (default: 960p)\n");
    std::printf("  --fullscreen     Run in exclusive fullscreen mode\n");
    std::printf("  --windowed       Run in windowed mode (default: borderless fullscreen)\n");
    std::printf("  --interpreter    Force interpreter mode (slower, but better for debugging)\n");
    std::printf("  --recompiler     Force the dynamic recompiler (JIT) instead of the interpreter\n");
    std::printf("  --headless       Run without opening a window (useful with --pipe)\n");
    std::printf("  --pipe <name>    Connect to a named pipe for IPC commands/framebuffer\n");
}

int main(int argc, char* argv[])
{
    // Initialize logging
    LoggingConfig::set_min_level(Logging::Level::Info);
    LoggingConfig::enable_console_output(true);

    Log::info("SuperNova64 starting...");

#ifdef __APPLE__
    PreloadMoltenVK();
#endif

    std::string romPath;
    std::string configPath;
    uint64_t frameLimit = 0; // 0 = unlimited
    int targetFPS = 60;      // Default: 60 FPS (NTSC)
    int windowWidth = 1280;   // Default: 16:9 HD (1280x720)
    int windowHeight = 720;
    // Sentinel 0 means "don't override config". A non-zero value comes from
    // an explicit --scale CLI flag and overrides NovaGraphics.ini.  Resolution
    // presets only set window size now — they used to also force scale, which
    // silently overwrote the ini every run.
    int internalResScale = 0;
    int fullscreenMode = 1;  // 0 = windowed, 1 = borderless fullscreen (default), 2 = exclusive fullscreen

    // Parse command line arguments
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            PrintUsage(argv[0]);
            return 0;
        }
        else if (std::strcmp(argv[i], "--debug") == 0)
        {
            LoggingConfig::set_min_level(Logging::Level::Debug);
        }
        else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc)
        {
            LoggingConfig::open_log_file(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc)
        {
            configPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
        {
            frameLimit = std::strtoull(argv[++i], nullptr, 10);
            Log::infof("Frame limit set to: %llu", (unsigned long long)frameLimit);
        }
        else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
        {
            targetFPS = std::atoi(argv[++i]);
            Log::infof("Target FPS set to: %d", targetFPS);
        }
        else if (std::strcmp(argv[i], "--resolution") == 0 && i + 1 < argc)
        {
            const char* res = argv[++i];
            // Window size only — internal scale is governed by NovaGraphics.ini
            // (or by an explicit --scale flag below).
            if (std::strcmp(res, "480p") == 0)       { windowWidth = 854;  windowHeight = 480;  }
            else if (std::strcmp(res, "720p") == 0)  { windowWidth = 1280; windowHeight = 720;  }
            else if (std::strcmp(res, "960p") == 0)  { windowWidth = 1706; windowHeight = 960;  }
            else if (std::strcmp(res, "1080p") == 0) { windowWidth = 1920; windowHeight = 1080; }
            else if (std::strcmp(res, "1440p") == 0) { windowWidth = 2560; windowHeight = 1440; }
            else if (std::strcmp(res, "4k") == 0)    { windowWidth = 3840; windowHeight = 2160; }
            else {
                std::fprintf(stderr, "Unknown resolution: %s (use 480p, 720p, 960p, 1080p, 1440p, 4k)\n", res);
                return 1;
            }
            Log::infof("Resolution set to: %dx%d", windowWidth, windowHeight);
        }
        else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
        {
            internalResScale = std::atoi(argv[++i]);
            if (internalResScale < 1 || internalResScale > 8) {
                std::fprintf(stderr, "--scale must be 1..8\n");
                return 1;
            }
            Log::infof("Internal resolution scale: %dx (overrides ini)", internalResScale);
        }
        else if (std::strcmp(argv[i], "--fullscreen") == 0)
        {
            fullscreenMode = 2;
            Log::info("Exclusive fullscreen mode enabled");
        }
        else if (std::strcmp(argv[i], "--windowed") == 0)
        {
            fullscreenMode = 0;
            Log::info("Windowed mode enabled");
        }
        else if (std::strcmp(argv[i], "--interpreter") == 0)
        {
            CN64System::s_UseInterpreter = true;
            Log::info("Interpreter mode forced via CLI");
        }
        else if (std::strcmp(argv[i], "--recompiler") == 0)
        {
            CN64System::s_UseInterpreter = false;
            Log::info("Dynamic recompiler forced via CLI");
        }
        else if (std::strcmp(argv[i], "--headless") == 0)
        {
            CN64System::s_HeadlessMode = true;
            Log::info("Headless mode enabled");
        }
        else if (std::strcmp(argv[i], "--pipe") == 0 && i + 1 < argc)
        {
            CN64System::s_PipeName = argv[++i];
            Log::infof("IPC Pipe name set to: %s", CN64System::s_PipeName.c_str());
        }
        else if (argv[i][0] != '-')
        {
            romPath = argv[i];
        }
        else
        {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            PrintUsage(argv[0]);
            return 1;
        }
    }

    Log::info("SuperNova64 initialized successfully");

    if (romPath.empty() && !CN64System::s_HeadlessMode)
    {
        Log::info("No ROM specified. Launching Nova UI...");
        NovaUI::NovaLauncher launcher;
        launcher.Run();
        // The launcher manages all emulator instances as child processes.
        // When it exits, we're done.
        Log::info("Launcher closed. SuperNova64 exiting.");
        Logging::stop_logging();
        return 0;
    }

    if (!romPath.empty())
    {
        std::filesystem::path sourcePath(romPath);
        std::filesystem::path romsDir("roms");
        if (!std::filesystem::exists(romsDir)) {
            std::filesystem::create_directory(romsDir);
        }
        
        std::filesystem::path destPath = romsDir / sourcePath.filename();
        
        std::error_code ec;
        if (!std::filesystem::exists(sourcePath) || !std::filesystem::equivalent(sourcePath.parent_path(), std::filesystem::absolute(romsDir), ec)) {
            if (std::filesystem::exists(sourcePath)) {
                Log::infof("Copying ROM to roms directory: %s", destPath.string().c_str());
                std::filesystem::copy_file(sourcePath, destPath, std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    Log::errorf("Failed to copy ROM: %s", ec.message().c_str());
                } else {
                    romPath = destPath.string();
                }
            }
        }
        
        Log::infof("Loading ROM: %s", romPath.c_str());
        CN64System::s_TargetFPS = targetFPS;
        CN64System::s_WindowWidth = windowWidth;
        CN64System::s_WindowHeight = windowHeight;
        CN64System::s_InternalResScale = internalResScale;
        CN64System::s_FullscreenMode = fullscreenMode;
        if (!CN64System::RunFileImage(romPath.c_str(), frameLimit))
        {
            Log::error("Failed to load ROM. Exiting.");
            Logging::stop_logging();
            return 1;
        }
        // RunFileImage runs the CPU on the current thread (blocking).
        // When it returns, emulation has ended.
    }

    // Cleanup
    Log::info("SuperNova64 shutting down...");
    CN64System::CloseSystem();
    Logging::stop_logging();

    return 0;
}
