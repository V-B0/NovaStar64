/**
 * @file InputPlugin.cpp
 * @brief Default (null) input plugin and DLL-backed input plugin implementation
 *
 * Provides a built-in fallback input plugin with no controller connected,
 * and a DLL loader for standard N64 controller plugins (Zilmar Input spec).
 *
 * Based on Project64's CControl_Plugin architecture:
 * - Loads function pointers from a DLL (GetKeys, ControllerCommand, etc.)
 * - Passes CONTROL_INFO struct with controller config to InitiateControllers
 * - Manages controller state and accessory communication
 */

#include "../../common/DynamicLibrary.hpp"
#include "InputPlugin.hpp"
#include "PluginBase.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../../common/log.hpp"

#include <cstring>
#include <cstdlib>
#include <cmath>
#include <array>

namespace Plugins {

// ============================================================================
// DefaultInputPlugin — Null/stub implementation
// ============================================================================

class DefaultInputPlugin final : public InputPlugin {
public:
    DefaultInputPlugin() {
        // Initialize all controllers as disconnected
        for (auto& state : m_Controllers) {
            std::memset(&state, 0, sizeof(state));
            state.Connected = false;
            state.Pak = ControllerState::PakType::None;
        }
        // Connect controller 1 by default
        m_Controllers[0].Connected = true;
    }

    ~DefaultInputPlugin() override = default;

    // PluginBase interface
    PluginInfo GetPluginInfo() const override {
        PluginInfo info;
        info.Type = PluginType::Input;
        info.Version = 0x0102;
        info.Name = "Nova64 Null Input";
        info.Author = "Nova64";
        info.Description = "Built-in null input plugin (no controller input)";
        info.SupportsThreading = false;
        info.SupportsStateSync = false;
        info.RequiresAudioSync = false;
        info.SupportsConfigDialog = false;
        return info;
    }

    bool Initialize(CN64System* system) override {
        m_System = system;
        m_Initialized = true;
        Log::info("DefaultInputPlugin: Initialized");
        return true;
    }

    void Shutdown() override {
        m_Initialized = false;
        m_System = nullptr;
        Log::info("DefaultInputPlugin: Shutdown");
    }

    void Reset(bool /*coldReset*/) override {
        for (auto& state : m_Controllers) {
            state.AnalogX = 0;
            state.AnalogY = 0;
            state.ButtonA = false;
            state.ButtonB = false;
            state.ButtonStart = false;
            state.DPadUp = false;
            state.DPadDown = false;
            state.DPadLeft = false;
            state.DPadRight = false;
            state.TriggerZ = false;
            state.TriggerL = false;
            state.TriggerR = false;
            state.CButtonUp = false;
            state.CButtonDown = false;
            state.CButtonLeft = false;
            state.CButtonRight = false;
        }
    }

    void Pause() override {}
    void Resume() override {}
    bool IsInitialized() const override { return m_Initialized; }
    void ShowConfigDialog(void* /*hwndParent*/) override {}

    size_t SaveState(uint8_t* /*data*/, size_t /*size*/) override { return 0; }
    bool LoadState(const uint8_t* /*data*/, size_t /*size*/) override { return true; }

    // InputPlugin interface
    bool InitializeInput(const InputInfo& /*info*/) override { return true; }
    void SetFocus(bool /*focus*/) override {}

    uint32_t GetControllerCount() const override { return 4; }

    bool IsControllerPresent(uint32_t controller) const override {
        if (controller >= 4) return false;
        return m_Controllers[controller].Connected;
    }

    ControllerState GetControllerState(uint32_t controller) override {
        if (controller >= 4) return {};
        return m_Controllers[controller];
    }

    void UpdateControllers() override {}

    void ProcessPIFCommand() override {}

    bool ReadController(uint32_t /*controller*/, uint8_t* /*command*/) override { return false; }

    void GetRawControllerData(uint32_t /*controller*/, uint8_t* /*data*/) override {}
    void SetControllerData(uint32_t /*controller*/, const uint8_t* /*data*/) override {}

    bool ReadPak(uint32_t /*controller*/, uint16_t /*address*/, uint8_t* data, uint32_t length) override {
        if (data && length > 0) std::memset(data, 0, length);
        return false;
    }

    bool WritePak(uint32_t /*controller*/, uint16_t /*address*/, const uint8_t* /*data*/,
                  uint32_t /*length*/) override {
        return false;
    }

    void SetRumble(uint32_t /*controller*/, uint8_t /*strength*/) override {}
    void ConfigureController(uint32_t /*controller*/, void* /*hwndParent*/) override {}

private:
    bool m_Initialized = false;
    std::array<ControllerState, 4> m_Controllers;
};

// ============================================================================
// DynLibInputPlugin — DLL-backed implementation (Project64 DLL compatibility)
// ============================================================================

/**
 * Controller plugin data structures matching N64 plugin spec.
 */

#ifdef _WIN32
#define CALL __cdecl
#else
#define CALL
#endif

// Present flags
constexpr int32_t PRESENT_NONE = 0;

// Plugin type flags
constexpr int32_t PLUGIN_NONE = 1;

struct CONTROL {
    int32_t Present;
    int32_t RawData;
    int32_t Plugin;
};

struct CONTROL_INFO_DLL {
    void* hWnd;
    void* hinst;
    int32_t Reserved;
    CONTROL Controls[4];
};

struct BUTTONS {
    uint32_t Value;
};

class DynLibInputPlugin final : public InputPlugin {
public:
    explicit DynLibInputPlugin(const std::string& path) : m_LibPath(path) {
        std::memset(m_PluginControllers, 0, sizeof(m_PluginControllers));
    }

    ~DynLibInputPlugin() override {
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
        if (m_Initialized && m_fnRomClosed) m_fnRomClosed();
        if (m_fnCloseDLL) m_fnCloseDLL();
        m_Initialized = false;
    }

    void Reset(bool /*coldReset*/) override {}
    void Pause() override {}
    void Resume() override {}
    bool IsInitialized() const override { return m_Initialized; }
    void ShowConfigDialog(void* /*hwndParent*/) override {
        if (m_fnDllConfig) m_fnDllConfig(nullptr);
    }

    size_t SaveState(uint8_t* /*data*/, size_t /*size*/) override { return 0; }
    bool LoadState(const uint8_t* /*data*/, size_t /*size*/) override { return true; }

    // InputPlugin interface
    bool InitializeInput(const InputInfo& /*info*/) override { return m_Initialized; }
    void SetFocus(bool /*focus*/) override {}

    uint32_t GetControllerCount() const override { return 4; }

    bool IsControllerPresent(uint32_t controller) const override {
        if (controller >= 4) return false;
        return m_PluginControllers[controller].Present != PRESENT_NONE;
    }

    ControllerState GetControllerState(uint32_t controller) override {
        ControllerState state{};
        if (controller >= 4) return state;

        state.Connected = (m_PluginControllers[controller].Present != PRESENT_NONE);

        if (state.Connected && m_fnGetKeys) {
            BUTTONS buttons{};
            m_fnGetKeys(static_cast<int32_t>(controller), &buttons);

            // Decode N64 button bitmask
            state.ButtonA = (buttons.Value & (1 << 31)) != 0;
            state.ButtonB = (buttons.Value & (1 << 30)) != 0;
            state.TriggerZ = (buttons.Value & (1 << 29)) != 0;
            state.ButtonStart = (buttons.Value & (1 << 28)) != 0;
            state.DPadUp = (buttons.Value & (1 << 27)) != 0;
            state.DPadDown = (buttons.Value & (1 << 26)) != 0;
            state.DPadLeft = (buttons.Value & (1 << 25)) != 0;
            state.DPadRight = (buttons.Value & (1 << 24)) != 0;
            state.TriggerL = (buttons.Value & (1 << 21)) != 0;
            state.TriggerR = (buttons.Value & (1 << 20)) != 0;
            state.CButtonUp = (buttons.Value & (1 << 19)) != 0;
            state.CButtonDown = (buttons.Value & (1 << 18)) != 0;
            state.CButtonLeft = (buttons.Value & (1 << 17)) != 0;
            state.CButtonRight = (buttons.Value & (1 << 16)) != 0;
            state.AnalogX = static_cast<int8_t>((buttons.Value >> 8) & 0xFF);
            state.AnalogY = static_cast<int8_t>(buttons.Value & 0xFF);
        }

        return state;
    }

    void UpdateControllers() override {
        // GetKeys is called per-controller in GetControllerState
        if (m_fnReadController) m_fnReadController(-1, nullptr);
    }

    void ProcessPIFCommand() override {}

    bool ReadController(uint32_t controller, uint8_t* command) override {
        if (m_fnReadController && controller < 4) {
            m_fnReadController(static_cast<int32_t>(controller), command);
            return true;
        }
        return false;
    }

    void GetRawControllerData(uint32_t /*controller*/, uint8_t* /*data*/) override {}
    void SetControllerData(uint32_t /*controller*/, const uint8_t* /*data*/) override {}

    bool ReadPak(uint32_t /*controller*/, uint16_t /*address*/, uint8_t* /*data*/, uint32_t /*length*/) override {
        return false;
    }

    bool WritePak(uint32_t /*controller*/, uint16_t /*address*/, const uint8_t* /*data*/,
                  uint32_t /*length*/) override {
        return false;
    }

    void SetRumble(uint32_t controller, uint8_t strength) override {
        if (m_fnRumbleCommand && controller < 4) {
            // Rumble plugin command: send rumble on/off
            m_fnRumbleCommand(static_cast<int32_t>(controller), strength > 0 ? 1 : 0);
        }
    }

    void ConfigureController(uint32_t /*controller*/, void* /*hwndParent*/) override {
        if (m_fnDllConfig) m_fnDllConfig(nullptr);
    }

private:
    bool LoadDLL() {
        try {
            m_Lib.Load(m_LibPath);
        } catch (const std::exception& e) {
            Log::errorf("DynLibInputPlugin: Failed to load %s: %s", m_LibPath.c_str(), e.what());
            return false;
        }

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
            m_Info.Type = PluginType::Input;
            m_Info.Version = dllInfo.Version;
            m_Info.Name = dllInfo.Name;
        }
        return true;
    }

    bool LoadFunctions() {
        if (!m_Lib.IsLoaded()) return false;

        m_fnGetKeys = m_Lib.GetSymbol<void(CALL*)(int32_t, BUTTONS*)>("GetKeys");
        m_fnControllerCommand = m_Lib.GetSymbol<void(CALL*)(int32_t, uint8_t*)>("ControllerCommand");
        m_fnReadController = m_Lib.GetSymbol<void(CALL*)(int32_t, uint8_t*)>("ReadController");
        m_fnRumbleCommand = m_Lib.GetSymbol<void(CALL*)(int32_t, int32_t)>("RumbleCommand");
        m_fnWMKeyDown = m_Lib.GetSymbol<void(CALL*)(uint32_t, int32_t)>("WM_KeyDown");
        m_fnWMKeyUp = m_Lib.GetSymbol<void(CALL*)(uint32_t, int32_t)>("WM_KeyUp");
        m_fnRomClosed = m_Lib.GetSymbol<void(CALL*)()>("RomClosed");
        m_fnCloseDLL = m_Lib.GetSymbol<void(CALL*)()>("CloseDLL");
        m_fnDllConfig = m_Lib.GetSymbol<void(CALL*)(void*)>("DllConfig");

        // GetKeys is the minimum requirement
        // (InitiateControllers is handled in Initiate)
        return true;
    }

    bool Initiate() {
        // Try InitiateControllers v1.2 (pointer arg)
        using InitFunc_1_2 = void(CALL*)(CONTROL_INFO_DLL*);
        auto initiate = m_Lib.GetSymbol<InitFunc_1_2>("InitiateControllers");
        if (!initiate) {
            Log::error("DynLibInputPlugin: Missing InitiateControllers");
            return false;
        }

        CONTROL_INFO_DLL info{};
        info.hWnd = nullptr;
        info.hinst = nullptr;
        info.Reserved = 1;

        for (int i = 0; i < 4; i++) {
            info.Controls[i].Present = PRESENT_NONE;
            info.Controls[i].RawData = 0;
            info.Controls[i].Plugin = PLUGIN_NONE;
        }

        initiate(&info);

        // Copy controller config back
        std::memcpy(m_PluginControllers, info.Controls, sizeof(m_PluginControllers));

        return true;
    }

    void UnloadDLL() {
        m_fnGetKeys = nullptr;
        m_fnControllerCommand = nullptr;
        m_fnReadController = nullptr;
        m_fnRumbleCommand = nullptr;
        m_fnWMKeyDown = nullptr;
        m_fnWMKeyUp = nullptr;
        m_fnRomClosed = nullptr;
        m_fnCloseDLL = nullptr;
        m_fnDllConfig = nullptr;
        m_Lib.Unload();
    }

    std::string m_LibPath;
    DynamicLibrary m_Lib;
    PluginInfo m_Info;
    bool m_Initialized = false;
    CONTROL m_PluginControllers[4]{};

    // DLL function pointers
    void(CALL* m_fnGetKeys)(int32_t, BUTTONS*) = nullptr;
    void(CALL* m_fnControllerCommand)(int32_t, uint8_t*) = nullptr;
    void(CALL* m_fnReadController)(int32_t, uint8_t*) = nullptr;
    void(CALL* m_fnRumbleCommand)(int32_t, int32_t) = nullptr;
    void(CALL* m_fnWMKeyDown)(uint32_t, int32_t) = nullptr;
    void(CALL* m_fnWMKeyUp)(uint32_t, int32_t) = nullptr;
    void(CALL* m_fnRomClosed)() = nullptr;
    void(CALL* m_fnCloseDLL)() = nullptr;
    void(CALL* m_fnDllConfig)(void*) = nullptr;
};

// ============================================================================
// Factory functions
// ============================================================================

std::unique_ptr<InputPlugin> CreateDefaultInputPlugin() { return std::make_unique<DefaultInputPlugin>(); }

std::unique_ptr<InputPlugin> CreateInputPluginFromDLL(const std::string& path) {
    return std::make_unique<DynLibInputPlugin>(path);
}

} // namespace Plugins
