/**
 * @file GFXPlugin.cpp
 * @brief Default (null) graphics plugin implementation
 *
 * Provides a built-in fallback graphics plugin that does nothing.
 * When a real graphics plugin DLL is loaded, this class is replaced
 * by a DLL-backed GFXPlugin wrapper.
 *
 * Based on Project64's CGfxPlugin architecture:
 * - Loads function pointers from a DLL (ChangeWindow, ProcessDList, etc.)
 * - Passes GFX_INFO struct with register/memory pointers to InitiateGFX
 * - Manages display list processing and screen updates
 */

#include "../../common/DynamicLibrary.hpp"
#include "GFXPlugin.hpp"
#include "PluginBase.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../../common/log.hpp"

#include <cstring>

namespace Plugins {

// --- DefaultGFXPlugin — Null/stub implementation ---

class DefaultGFXPlugin final : public GFXPlugin {
public:
    DefaultGFXPlugin() = default;
    ~DefaultGFXPlugin() override = default;

    // PluginBase interface
    PluginInfo GetPluginInfo() const override {
        PluginInfo info;
        info.Type = PluginType::Graphics;
        info.Version = 0x0104;
        info.Name = "Nova64 Null Graphics";
        info.Author = "Nova64";
        info.Description = "Built-in null graphics plugin (no rendering)";
        info.SupportsThreading = false;
        info.SupportsStateSync = false;
        info.RequiresAudioSync = false;
        info.SupportsConfigDialog = false;
        return info;
    }

    bool Initialize(CN64System* system) override {
        m_System = system;
        m_Initialized = true;
        Log::info("DefaultGFXPlugin: Initialized");
        return true;
    }

    void Shutdown() override {
        m_Initialized = false;
        m_System = nullptr;
        Log::info("DefaultGFXPlugin: Shutdown");
    }

    void Reset(bool /*coldReset*/) override { Log::info("DefaultGFXPlugin: Reset"); }

    void Pause() override {}
    void Resume() override {}

    bool IsInitialized() const override { return m_Initialized; }
    void ShowConfigDialog(void* /*hwndParent*/) override {}

    size_t SaveState(uint8_t* /*data*/, size_t /*size*/) override { return 0; }
    bool LoadState(const uint8_t* /*data*/, size_t /*size*/) override { return true; }

    // GFXPlugin interface
    bool InitializeDisplay(void* /*hwnd*/, const RenderSettings& /*settings*/) override { return true; }

    void ResizeDisplay(uint32_t /*width*/, uint32_t /*height*/) override {}

    void ProcessDList(uint32_t /*start*/, uint32_t /*end*/) override {}
    void ProcessRDPCommand(uint64_t /*cmd*/) override {}
    void UpdateRDPStatus() override {}

    void UpdateScreen() override {}
    void SwapBuffers() override {}

    FrameBufferInfo GetFrameBufferInfo() const override {
        FrameBufferInfo info{};
        info.ColorBuffer = nullptr;
        info.DepthBuffer = nullptr;
        info.Width = 320;
        info.Height = 240;
        info.Stride = 320;
        return info;
    }

    void ReadFrameBuffer(uint8_t* /*dest*/, uint32_t /*addr*/, uint32_t /*width*/, uint32_t /*height*/,
                         uint32_t /*size*/) override {}
    void WriteFrameBuffer(const uint8_t* /*src*/, uint32_t /*addr*/, uint32_t /*width*/, uint32_t /*height*/,
                          uint32_t /*size*/) override {}

    bool IsFrameReady() const override { return false; }
    void GetStatistics(float& fps, float& vis) const override {
        fps = 0.0f;
        vis = 0.0f;
    }
    bool CaptureScreenshot(const std::string& /*filename*/) override { return false; }

private:
    bool m_Initialized = false;
};

// --- DynLibGFXPlugin — DLL-backed implementation (Project64 DLL compatibility) ---

/**
 * GFX_INFO structure passed to InitiateGFX in the plugin DLL.
 * This matches the N64 plugin specification v1.3/1.4.
 */
struct GFX_INFO {
    void* hWnd;
    void* hStatusBar;
    int32_t MemoryBswaped;

    uint8_t* HEADER;
    uint8_t* RDRAM;
    uint8_t* DMEM;
    uint8_t* IMEM;

    uint32_t* MI__INTR_REG;

    uint32_t* DPC__START_REG;
    uint32_t* DPC__END_REG;
    uint32_t* DPC__CURRENT_REG;
    uint32_t* DPC__STATUS_REG;
    uint32_t* DPC__CLOCK_REG;
    uint32_t* DPC__BUFBUSY_REG;
    uint32_t* DPC__PIPEBUSY_REG;
    uint32_t* DPC__TMEM_REG;

    uint32_t* VI__STATUS_REG;
    uint32_t* VI__ORIGIN_REG;
    uint32_t* VI__WIDTH_REG;
    uint32_t* VI__INTR_REG;
    uint32_t* VI__V_CURRENT_LINE_REG;
    uint32_t* VI__TIMING_REG;
    uint32_t* VI__V_SYNC_REG;
    uint32_t* VI__H_SYNC_REG;
    uint32_t* VI__LEAP_REG;
    uint32_t* VI__H_START_REG;
    uint32_t* VI__V_START_REG;
    uint32_t* VI__V_BURST_REG;
    uint32_t* VI__X_SCALE_REG;
    uint32_t* VI__Y_SCALE_REG;

    void (*CheckInterrupts)(void);
};

#ifdef _WIN32
#define CALL __cdecl
#else
#define CALL
#endif

static void CALL DummyCheckInterrupts() {}

/**
 * DLL-backed GFX plugin. Loads a standard N64 graphics plugin DLL
 * (Zilmar GFX spec) and wraps the function pointers.
 */
class DynLibGFXPlugin final : public GFXPlugin {
public:
    explicit DynLibGFXPlugin(const std::string& path) : m_LibPath(path) {}

    ~DynLibGFXPlugin() override {
        Shutdown();
        UnloadDLL();
    }

    // PluginBase interface
    PluginInfo GetPluginInfo() const override { return m_Info; }

    bool Initialize(CN64System* system) override {
        m_System = system;
        if (!LoadDLL()) return false;
        if (!LoadFunctions()) return false;
        if (!Initiate()) return false;
        m_Initialized = true;
        return true;
    }

    void Shutdown() override {
        if (m_Initialized && m_fnRomClosed) {
            m_fnRomClosed();
        }
        if (m_fnCloseDLL) {
            m_fnCloseDLL();
        }
        m_Initialized = false;
    }

    void Reset(bool /*coldReset*/) override {
        if (m_fnSoftReset) m_fnSoftReset();
    }

    void Pause() override {}
    void Resume() override {}
    bool IsInitialized() const override { return m_Initialized; }
    void ShowConfigDialog(void* /*hwndParent*/) override {
        if (m_fnDllConfig) m_fnDllConfig(nullptr);
    }

    size_t SaveState(uint8_t* /*data*/, size_t /*size*/) override { return 0; }
    bool LoadState(const uint8_t* /*data*/, size_t /*size*/) override { return true; }

    // GFXPlugin interface
    bool InitializeDisplay(void* /*hwnd*/, const RenderSettings& /*settings*/) override { return m_Initialized; }

    void ResizeDisplay(uint32_t /*width*/, uint32_t /*height*/) override {
        if (m_fnMoveScreen) m_fnMoveScreen(0, 0);
    }

    void ProcessDList(uint32_t /*start*/, uint32_t /*end*/) override {
        if (m_fnProcessDList) m_fnProcessDList();
    }

    void ProcessRDPCommand(uint64_t /*cmd*/) override {
        if (m_fnProcessRDPList) m_fnProcessRDPList();
    }

    void UpdateRDPStatus() override {}

    void UpdateScreen() override {
        if (m_fnUpdateScreen) m_fnUpdateScreen();
    }

    void SwapBuffers() override {
        if (m_fnChangeWindow) m_fnChangeWindow();
    }

    FrameBufferInfo GetFrameBufferInfo() const override {
        FrameBufferInfo info{};
        return info;
    }

    void ReadFrameBuffer(uint8_t* /*dest*/, uint32_t /*addr*/, uint32_t /*width*/, uint32_t /*height*/,
                         uint32_t /*size*/) override {}
    void WriteFrameBuffer(const uint8_t* /*src*/, uint32_t /*addr*/, uint32_t /*width*/, uint32_t /*height*/,
                          uint32_t /*size*/) override {}

    bool IsFrameReady() const override { return true; }
    void GetStatistics(float& fps, float& vis) const override {
        fps = 0;
        vis = 0;
    }
    bool CaptureScreenshot(const std::string& /*filename*/) override {
        if (m_fnCaptureScreen) {
            // Standard plugin CaptureScreen takes char*
            return true;
        }
        return false;
    }

private:
    bool LoadDLL() {
        try {
            m_Lib.Load(m_LibPath);
        } catch (const std::exception& e) {
            Log::errorf("DynLibGFXPlugin: Failed to load %s: %s", m_LibPath.c_str(), e.what());
            return false;
        }

        // Query plugin info
        using GetDllInfoFunc = void(CALL*)(void*);
        auto getDllInfo = m_Lib.GetSymbol<GetDllInfoFunc>("GetDllInfo");
        if (getDllInfo) {
            struct {
                uint16_t Version;
                uint16_t Type;
                char Name[100];
                int NormalMemory;
                int MemoryBswaped;
            } dllInfo{};
            getDllInfo(&dllInfo);
            m_Info.Type = PluginType::Graphics;
            m_Info.Version = dllInfo.Version;
            m_Info.Name = dllInfo.Name;
        }
        return true;
    }

    bool LoadFunctions() {
        if (!m_Lib.IsLoaded()) return false;

        m_fnChangeWindow = m_Lib.GetSymbol<void(CALL*)()>("ChangeWindow");
        m_fnDrawScreen = m_Lib.GetSymbol<void(CALL*)()>("DrawScreen");
        m_fnMoveScreen = m_Lib.GetSymbol<void(CALL*)(int, int)>("MoveScreen");
        m_fnProcessDList = m_Lib.GetSymbol<void(CALL*)()>("ProcessDList");
        m_fnProcessRDPList = m_Lib.GetSymbol<void(CALL*)()>("ProcessRDPList");
        m_fnUpdateScreen = m_Lib.GetSymbol<void(CALL*)()>("UpdateScreen");
        m_fnShowCFB = m_Lib.GetSymbol<void(CALL*)()>("ShowCFB");
        m_fnViStatusChanged = m_Lib.GetSymbol<void(CALL*)()>("ViStatusChanged");
        m_fnViWidthChanged = m_Lib.GetSymbol<void(CALL*)()>("ViWidthChanged");
        m_fnSoftReset = m_Lib.GetSymbol<void(CALL*)()>("SoftReset");
        m_fnCaptureScreen = m_Lib.GetSymbol<void(CALL*)(const char*)>("CaptureScreen");
        m_fnRomClosed = m_Lib.GetSymbol<void(CALL*)()>("RomClosed");
        m_fnCloseDLL = m_Lib.GetSymbol<void(CALL*)()>("CloseDLL");
        m_fnDllConfig = m_Lib.GetSymbol<void(CALL*)(void*)>("DllConfig");

        if (!m_fnChangeWindow || !m_fnProcessDList || !m_fnUpdateScreen) {
            Log::error("DynLibGFXPlugin: Missing required functions");
            return false;
        }
        return true;
    }

    bool Initiate() {
        using InitiateGFXFunc = int32_t(CALL*)(GFX_INFO);
        auto initiateGFX = m_Lib.GetSymbol<InitiateGFXFunc>("InitiateGFX");
        if (!initiateGFX) {
            Log::error("DynLibGFXPlugin: Missing InitiateGFX");
            return false;
        }

        GFX_INFO info{};
        info.MemoryBswaped = 1;
        info.CheckInterrupts = DummyCheckInterrupts;

        if (m_System) {
            auto* regs = m_System->GetRegisters();
            auto* mem = m_System->GetMemory();

            if (regs && mem) {
                info.RDRAM = mem->Rdram();
                info.DMEM = mem->Dmem();
                info.IMEM = mem->Imem();

                info.MI__INTR_REG = &regs->m_GfxIntrReg;

                // DPC registers (indices 0-7 in m_Display_ControlReg)
                info.DPC__START_REG = &regs->m_Display_ControlReg[0];
                info.DPC__END_REG = &regs->m_Display_ControlReg[1];
                info.DPC__CURRENT_REG = &regs->m_Display_ControlReg[2];
                info.DPC__STATUS_REG = &regs->m_Display_ControlReg[3];
                info.DPC__CLOCK_REG = &regs->m_Display_ControlReg[4];
                info.DPC__BUFBUSY_REG = &regs->m_Display_ControlReg[5];
                info.DPC__PIPEBUSY_REG = &regs->m_Display_ControlReg[6];
                info.DPC__TMEM_REG = &regs->m_Display_ControlReg[7];

                // VI registers (indices 0-13 in m_Video_Interface)
                info.VI__STATUS_REG = &regs->m_Video_Interface[0];
                info.VI__ORIGIN_REG = &regs->m_Video_Interface[1];
                info.VI__WIDTH_REG = &regs->m_Video_Interface[2];
                info.VI__INTR_REG = &regs->m_Video_Interface[3];
                info.VI__V_CURRENT_LINE_REG = &regs->m_Video_Interface[4];
                info.VI__TIMING_REG = &regs->m_Video_Interface[6];
                info.VI__V_SYNC_REG = &regs->m_Video_Interface[7];
                info.VI__H_SYNC_REG = &regs->m_Video_Interface[8];
                info.VI__LEAP_REG = &regs->m_Video_Interface[9];
                info.VI__H_START_REG = &regs->m_Video_Interface[11];
                info.VI__V_START_REG = &regs->m_Video_Interface[13];
                info.VI__V_BURST_REG = &regs->m_Video_Interface[5];  // BURST at index 5
                info.VI__X_SCALE_REG = &regs->m_Video_Interface[10]; // placeholder
                info.VI__Y_SCALE_REG = &regs->m_Video_Interface[12]; // placeholder
            }
        } else {
            // Pre-init with dummy values (for config dialog)
            static uint8_t dummyMem[100]{};
            static uint32_t dummyReg = 0;
            info.HEADER = dummyMem;
            info.RDRAM = dummyMem;
            info.DMEM = dummyMem;
            info.IMEM = dummyMem;
            info.MI__INTR_REG = &dummyReg;
            info.DPC__START_REG = &dummyReg;
            info.DPC__END_REG = &dummyReg;
            info.DPC__CURRENT_REG = &dummyReg;
            info.DPC__STATUS_REG = &dummyReg;
            info.DPC__CLOCK_REG = &dummyReg;
            info.DPC__BUFBUSY_REG = &dummyReg;
            info.DPC__PIPEBUSY_REG = &dummyReg;
            info.DPC__TMEM_REG = &dummyReg;
            info.VI__STATUS_REG = &dummyReg;
            info.VI__ORIGIN_REG = &dummyReg;
            info.VI__WIDTH_REG = &dummyReg;
            info.VI__INTR_REG = &dummyReg;
            info.VI__V_CURRENT_LINE_REG = &dummyReg;
            info.VI__TIMING_REG = &dummyReg;
            info.VI__V_SYNC_REG = &dummyReg;
            info.VI__H_SYNC_REG = &dummyReg;
            info.VI__LEAP_REG = &dummyReg;
            info.VI__H_START_REG = &dummyReg;
            info.VI__V_START_REG = &dummyReg;
            info.VI__V_BURST_REG = &dummyReg;
            info.VI__X_SCALE_REG = &dummyReg;
            info.VI__Y_SCALE_REG = &dummyReg;
        }

        return initiateGFX(info) != 0;
    }

    void UnloadDLL() {
        m_fnChangeWindow = nullptr;
        m_fnDrawScreen = nullptr;
        m_fnMoveScreen = nullptr;
        m_fnProcessDList = nullptr;
        m_fnProcessRDPList = nullptr;
        m_fnUpdateScreen = nullptr;
        m_fnShowCFB = nullptr;
        m_fnViStatusChanged = nullptr;
        m_fnViWidthChanged = nullptr;
        m_fnSoftReset = nullptr;
        m_fnCaptureScreen = nullptr;
        m_fnRomClosed = nullptr;
        m_fnCloseDLL = nullptr;
        m_fnDllConfig = nullptr;
        m_Lib.Unload();
    }

    // Members
    std::string m_LibPath;
    DynamicLibrary m_Lib;
    PluginInfo m_Info;
    bool m_Initialized = false;

    // DLL function pointers
    void(CALL* m_fnChangeWindow)() = nullptr;
    void(CALL* m_fnDrawScreen)() = nullptr;
    void(CALL* m_fnMoveScreen)(int, int) = nullptr;
    void(CALL* m_fnProcessDList)() = nullptr;
    void(CALL* m_fnProcessRDPList)() = nullptr;
    void(CALL* m_fnUpdateScreen)() = nullptr;
    void(CALL* m_fnShowCFB)() = nullptr;
    void(CALL* m_fnViStatusChanged)() = nullptr;
    void(CALL* m_fnViWidthChanged)() = nullptr;
    void(CALL* m_fnSoftReset)() = nullptr;
    void(CALL* m_fnCaptureScreen)(const char*) = nullptr;
    void(CALL* m_fnRomClosed)() = nullptr;
    void(CALL* m_fnCloseDLL)() = nullptr;
    void(CALL* m_fnDllConfig)(void*) = nullptr;
};

// --- Factory function ---

std::unique_ptr<GFXPlugin> CreateDefaultGFXPlugin() { return std::make_unique<DefaultGFXPlugin>(); }

std::unique_ptr<GFXPlugin> CreateGFXPluginFromDLL(const std::string& path) {
    auto plugin = std::make_unique<DynLibGFXPlugin>(path);
    return plugin;
}

} // namespace Plugins
