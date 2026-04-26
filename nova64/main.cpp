/**
 * @file main.cpp
 * @brief SuperNova64 - Nintendo 64 Emulator Entry Point
 */

#include "common/log.hpp"
#include "common/path.hpp"
#include "N64SYS/MainSYS.hpp"
#include "N64SYS/N64Rom.hpp"
#include "N64SYS/SysGlobals.hpp"

#include <cstdio>
#include <cstring>
#include <string>

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
}

int main(int argc, char* argv[])
{
    // Initialize logging
    LoggingConfig::set_min_level(Logging::Level::Info);
    LoggingConfig::enable_console_output(true);

    Log::info("SuperNova64 starting...");

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

    if (!romPath.empty())
    {
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
    else
    {
        Log::info("No ROM specified. Use --help for usage information.");
    }

    // Cleanup
    Log::info("SuperNova64 shutting down...");
    CN64System::CloseSystem();
    Logging::stop_logging();

    return 0;
}
