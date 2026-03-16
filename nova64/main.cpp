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
