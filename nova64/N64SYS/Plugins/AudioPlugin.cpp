/**
 * @file AudioPlugin.cpp
 * @brief Default (null) audio plugin and DLL-backed audio plugin implementation
 *
 * Provides a built-in fallback audio plugin that produces silence,
 * and a DLL loader for standard N64 audio plugins (Zilmar Audio spec).
 *
 * Based on Project64's CAudioPlugin architecture:
 * - Loads function pointers from a DLL (AiLenChanged, AiDacrateChanged, etc.)
 * - Passes AUDIO_INFO struct with register/memory pointers to InitiateAudio
 * - Manages audio DAC rate changes and audio list processing
 */

#include "../../common/DynamicLibrary.hpp"
#include "AudioPlugin.hpp"
#include "PluginBase.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../../common/log.hpp"

#include <cstring>

namespace Plugins {

// --- DefaultAudioPlugin — Null/stub implementation ---

class DefaultAudioPlugin final : public AudioPlugin {
public:
    DefaultAudioPlugin() = default;
    ~DefaultAudioPlugin() override = default;

    // PluginBase interface
    PluginInfo GetPluginInfo() const override {
        PluginInfo info;
        info.Type = PluginType::Audio;
        info.Version = 0x0102;
        info.Name = "Nova64 Null Audio";
        info.Author = "Nova64";
        info.Description = "Built-in null audio plugin (silence)";
        info.SupportsThreading = false;
        info.SupportsStateSync = false;
        info.RequiresAudioSync = false;
        info.SupportsConfigDialog = false;
        return info;
    }

    bool Initialize(CN64System* system) override {
        m_System = system;
        m_Initialized = true;
        Log::info("DefaultAudioPlugin: Initialized");
        return true;
    }

    void Shutdown() override {
        m_Initialized = false;
        m_System = nullptr;
        Log::info("DefaultAudioPlugin: Shutdown");
    }

    void Reset(bool /*coldReset*/) override {}
    void Pause() override { m_Playing = false; }
    void Resume() override { m_Playing = true; }

    bool IsInitialized() const override { return m_Initialized; }
    void ShowConfigDialog(void* /*hwndParent*/) override {}

    size_t SaveState(uint8_t* /*data*/, size_t /*size*/) override { return 0; }
    bool LoadState(const uint8_t* /*data*/, size_t /*size*/) override { return true; }

    // AudioPlugin interface
    bool InitializeAudio(const AudioSettings& /*settings*/) override { return true; }
    void SetAudioInfo(const AudioInfo& /*info*/) override {}

    void ProcessAICommand() override {}
    uint32_t GetAILength() override { return 0; }
    void SetDACRate(uint32_t /*frequency*/) override {}
    void SetBitRate(uint32_t /*bitrate*/) override {}

    void AddAudioSamples(const int16_t* /*buffer*/, uint32_t /*length*/) override {}
    bool IsPlaying() const override { return m_Playing; }
    uint32_t GetBufferLevel() const override { return 0; }

    void WaitForBuffer() override {}
    void SetSyncMode(bool /*sync*/) override {}

    void SetVolume(uint32_t /*volume*/) override {}
    void SetMute(bool /*mute*/) override {}

private:
    bool m_Initialized = false;
    bool m_Playing = false;
};

// --- DynLibAudioPlugin — DLL-backed implementation (Project64 DLL compatibility) ---

/**
 * AUDIO_INFO structure passed to InitiateAudio in the plugin DLL.
 * Matches the N64 audio plugin specification.
 */
struct AUDIO_INFO_DLL {
    void* hWnd;
    void* hinst;
    int32_t Reserved;

    uint8_t* HEADER;
    uint8_t* RDRAM;
    uint8_t* DMEM;
    uint8_t* IMEM;

    uint32_t* MI_INTR_REG;

    uint32_t* AI_DRAM_ADDR_REG;
    uint32_t* AI_LEN_REG;
    uint32_t* AI_CONTROL_REG;
    uint32_t* AI_STATUS_REG;
    uint32_t* AI_DACRATE_REG;
    uint32_t* AI_BITRATE_REG;

    void (*CheckInterrupts)(void);
};

#ifdef _WIN32
#define CALL __cdecl
#else
#define CALL
#endif

static void CALL DummyCheckInterrupts() {}

/**
 * DLL-backed audio plugin. Loads a standard N64 audio plugin DLL
 * and wraps the function pointers.
 */
class DynLibAudioPlugin final : public AudioPlugin {
public:
    explicit DynLibAudioPlugin(const std::string& path) : m_LibPath(path) {}

    ~DynLibAudioPlugin() override {
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

    // AudioPlugin interface
    bool InitializeAudio(const AudioSettings& /*settings*/) override { return m_Initialized; }
    void SetAudioInfo(const AudioInfo& /*info*/) override {}

    void ProcessAICommand() override {
        if (m_fnAiLenChanged) m_fnAiLenChanged();
    }

    uint32_t GetAILength() override {
        if (m_fnAiReadLength) return m_fnAiReadLength();
        return 0;
    }

    void SetDACRate(uint32_t /*frequency*/) override {
        if (m_fnAiDacrateChanged) {
            // NTSC = 1, PAL = 0
            m_fnAiDacrateChanged(1);
        }
    }

    void SetBitRate(uint32_t /*bitrate*/) override {}

    void AddAudioSamples(const int16_t* /*buffer*/, uint32_t /*length*/) override {}
    bool IsPlaying() const override { return m_Initialized; }
    uint32_t GetBufferLevel() const override { return 0; }
    void WaitForBuffer() override {}
    void SetSyncMode(bool /*sync*/) override {}
    void SetVolume(uint32_t /*volume*/) override {}
    void SetMute(bool /*mute*/) override {}

private:
    bool LoadDLL() {
        try {
            m_Lib.Load(m_LibPath);
        } catch (const std::exception& e) {
            Log::errorf("DynLibAudioPlugin: Failed to load %s: %s", m_LibPath.c_str(), e.what());
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
            m_Info.Type = PluginType::Audio;
            m_Info.Version = dllInfo.Version;
            m_Info.Name = dllInfo.Name;
        }
        return true;
    }

    bool LoadFunctions() {
        if (!m_Lib.IsLoaded()) return false;

        m_fnAiLenChanged = m_Lib.GetSymbol<void(CALL*)()>("AiLenChanged");
        m_fnAiReadLength = m_Lib.GetSymbol<uint32_t(CALL*)()>("AiReadLength");
        m_fnAiDacrateChanged = m_Lib.GetSymbol<void(CALL*)(int)>("AiDacrateChanged");
        m_fnAiUpdate = m_Lib.GetSymbol<void(CALL*)(int32_t)>("AiUpdate");
        m_fnProcessAList = m_Lib.GetSymbol<void(CALL*)()>("ProcessAList");
        m_fnRomClosed = m_Lib.GetSymbol<void(CALL*)()>("RomClosed");
        m_fnCloseDLL = m_Lib.GetSymbol<void(CALL*)()>("CloseDLL");
        m_fnDllConfig = m_Lib.GetSymbol<void(CALL*)(void*)>("DllConfig");

        if (!m_fnAiLenChanged || !m_fnAiDacrateChanged || !m_fnProcessAList) {
            Log::error("DynLibAudioPlugin: Missing required functions");
            return false;
        }
        return true;
    }

    bool Initiate() {
        using InitiateAudioFunc = int32_t(CALL*)(AUDIO_INFO_DLL);
        auto initiateAudio = m_Lib.GetSymbol<InitiateAudioFunc>("InitiateAudio");
        if (!initiateAudio) {
            Log::error("DynLibAudioPlugin: Missing InitiateAudio");
            return false;
        }

        AUDIO_INFO_DLL info{};
        info.hWnd = nullptr;
        info.hinst = nullptr;
        info.Reserved = 1;
        info.CheckInterrupts = DummyCheckInterrupts;

        if (m_System) {
            auto* regs = m_System->GetRegisters();
            auto* mem = m_System->GetMemory();

            if (regs && mem) {
                info.RDRAM = mem->Rdram();
                info.DMEM = mem->Dmem();
                info.IMEM = mem->Imem();

                info.MI_INTR_REG = &regs->m_AudioIntrReg;

                // AI registers (indices 0-5 in m_Audio_Interface)
                info.AI_DRAM_ADDR_REG = &regs->m_Audio_Interface[0];
                info.AI_LEN_REG = &regs->m_Audio_Interface[1];
                info.AI_CONTROL_REG = &regs->m_Audio_Interface[2];
                info.AI_STATUS_REG = &regs->m_Audio_Interface[3];
                info.AI_DACRATE_REG = &regs->m_Audio_Interface[4];
                info.AI_BITRATE_REG = &regs->m_Audio_Interface[5];
            }
        } else {
            static uint8_t dummyMem[100]{};
            static uint32_t dummyReg = 0;
            info.HEADER = dummyMem;
            info.RDRAM = dummyMem;
            info.DMEM = dummyMem;
            info.IMEM = dummyMem;
            info.MI_INTR_REG = &dummyReg;
            info.AI_DRAM_ADDR_REG = &dummyReg;
            info.AI_LEN_REG = &dummyReg;
            info.AI_CONTROL_REG = &dummyReg;
            info.AI_STATUS_REG = &dummyReg;
            info.AI_DACRATE_REG = &dummyReg;
            info.AI_BITRATE_REG = &dummyReg;
        }

        return initiateAudio(info) != 0;
    }

    void UnloadDLL() {
        m_fnAiLenChanged = nullptr;
        m_fnAiReadLength = nullptr;
        m_fnAiDacrateChanged = nullptr;
        m_fnAiUpdate = nullptr;
        m_fnProcessAList = nullptr;
        m_fnRomClosed = nullptr;
        m_fnCloseDLL = nullptr;
        m_fnDllConfig = nullptr;
        m_Lib.Unload();
    }

    std::string m_LibPath;
    DynamicLibrary m_Lib;
    PluginInfo m_Info;
    bool m_Initialized = false;

    // DLL function pointers
    void(CALL* m_fnAiLenChanged)() = nullptr;
    uint32_t(CALL* m_fnAiReadLength)() = nullptr;
    void(CALL* m_fnAiDacrateChanged)(int) = nullptr;
    void(CALL* m_fnAiUpdate)(int32_t) = nullptr;
    void(CALL* m_fnProcessAList)() = nullptr;
    void(CALL* m_fnRomClosed)() = nullptr;
    void(CALL* m_fnCloseDLL)() = nullptr;
    void(CALL* m_fnDllConfig)(void*) = nullptr;
};

// --- Factory functions ---

std::unique_ptr<AudioPlugin> CreateAudioPluginFromDLL(const std::string& path) {
    return std::make_unique<DynLibAudioPlugin>(path);
}

} // namespace Plugins
