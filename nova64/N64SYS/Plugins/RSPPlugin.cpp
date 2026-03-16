/**
 * @file RSPPlugin.cpp
 * @brief Default (null) RSP plugin and DLL-backed RSP plugin implementation
 *
 * Provides a built-in fallback RSP plugin with no processing,
 * and a DLL loader for standard N64 RSP plugins (Zilmar RSP spec).
 *
 * Based on Project64's CRSP_Plugin architecture:
 * - Loads function pointers from a DLL (DoRspCycles, InitiateRSP)
 * - Passes RSP_INFO_DLL struct with register pointers to InitiateRSP
 * - Runs RSP tasks via DoRspCycles, returns cycle count
 */

#include "../../common/DynamicLibrary.hpp"
#include "RSPPlugin.hpp"
#include "PluginBase.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../../common/log.hpp"

#include <cstring>

namespace Plugins {

// ============================================================================
// DefaultRSPPlugin — Null/stub implementation
// ============================================================================

class DefaultRSPPlugin final : public RSPPlugin {
public:
    DefaultRSPPlugin() = default;
    ~DefaultRSPPlugin() override = default;

    // PluginBase interface
    PluginInfo GetPluginInfo() const override
    {
        PluginInfo info;
        info.Type = PluginType::RSP;
        info.Version = 0x0102;
        info.Name = "Nova64 Null RSP";
        info.Author = "Nova64";
        info.Description = "Built-in null RSP plugin (immediate task completion)";
        info.SupportsThreading = false;
        info.SupportsStateSync = false;
        info.RequiresAudioSync = false;
        info.SupportsConfigDialog = false;
        return info;
    }

    bool Initialize(CN64System* system) override
    {
        m_System = system;
        m_Initialized = true;
        Log::info("DefaultRSPPlugin: Initialized");
        return true;
    }

    void Shutdown() override
    {
        m_Initialized = false;
        m_System = nullptr;
        Log::info("DefaultRSPPlugin: Shutdown");
    }

    void Reset(bool /*coldReset*/) override
    {
        m_PC = 0;
        m_Running = false;
    }

    void Pause() override { m_Running = false; }
    void Resume() override {}
    bool IsInitialized() const override { return m_Initialized; }
    void ShowConfigDialog(void* /*hwndParent*/) override {}

    size_t SaveState(uint8_t* data, size_t size) override
    {
        if (data && size >= sizeof(uint32_t))
        {
            std::memcpy(data, &m_PC, sizeof(uint32_t));
            return sizeof(uint32_t);
        }
        return 0;
    }

    bool LoadState(const uint8_t* data, size_t size) override
    {
        if (data && size >= sizeof(uint32_t))
        {
            std::memcpy(&m_PC, data, sizeof(uint32_t));
            return true;
        }
        return false;
    }

    // RSPPlugin interface
    bool InitializeRSP(const RSPInfo& /*info*/) override { return true; }
    void SetCPUType(bool /*useLLE*/) override {}

    uint32_t DoRSPCycles(RSPTaskType /*taskType*/) override
    {
        // Null RSP: indicate immediate completion (0 cycles consumed)
        return 0;
    }

    bool DoRSPStep() override { return false; }
    void BreakRSP() override { m_Running = false; }

    void ProcessSPDMARead() override {}
    void ProcessSPDMAWrite() override {}

    uint32_t GetPC() const override { return m_PC; }
    void SetPC(uint32_t pc) override { m_PC = pc & 0xFFC; }
    bool IsRSPRunning() const override { return m_Running; }
    uint32_t GetStatus() const override { return 0; }

private:
    bool m_Initialized = false;
    bool m_Running = false;
    uint32_t m_PC = 0;
};

// ============================================================================
// DynLibRSPPlugin — DLL-backed implementation (Project64 DLL compatibility)
// ============================================================================

#ifdef _WIN32
#define CALL __cdecl
#else
#define CALL
#endif

/**
 * RSP_INFO struct matching the N64 plugin spec.
 * Passed to the DLL's InitiateRSP function.
 */
struct RSP_INFO_DLL {
    void*     hInst;
    int32_t   MemoryBswaped;   // If memory is bswapped on the host

    uint8_t*  RDRAM;
    uint8_t*  DMEM;
    uint8_t*  IMEM;

    uint32_t* MI_INTR_REG;

    uint32_t* SP_MEM_ADDR_REG;
    uint32_t* SP_DRAM_ADDR_REG;
    uint32_t* SP_RD_LEN_REG;
    uint32_t* SP_WR_LEN_REG;
    uint32_t* SP_STATUS_REG;
    uint32_t* SP_DMA_FULL_REG;
    uint32_t* SP_DMA_BUSY_REG;
    uint32_t* SP_SEMAPHORE_REG;
    uint32_t* SP_PC_REG;

    uint32_t* DPC_START_REG;
    uint32_t* DPC_END_REG;
    uint32_t* DPC_CURRENT_REG;
    uint32_t* DPC_STATUS_REG;
    uint32_t* DPC_CLOCK_REG;
    uint32_t* DPC_BUFBUSY_REG;
    uint32_t* DPC_PIPEBUSY_REG;
    uint32_t* DPC_TMEM_REG;

    void (CALL *CheckInterrupts)();
    void (CALL *ProcessDList)();
    void (CALL *ProcessAList)();
    void (CALL *ProcessRdpList)();
    void (CALL *ShowCFB)();
};

// RSP DLL function signatures
using RSPDoRspCycles   = uint32_t(CALL*)(uint32_t Cycles);
using RSPInitiateRSP   = void(CALL*)(RSP_INFO_DLL info, uint32_t* CycleCount);
using RSPRomClosed     = void(CALL*)();
using RSPCloseDLL      = void(CALL*)();
using RSPDllConfig     = void(CALL*)(void* hWnd);
using RSPRomOpen       = void(CALL*)();

// Plugin-to-plugin callback stubs
static void CALL StubCheckInterrupts() {}
static void CALL StubProcessDList()    {}
static void CALL StubProcessAList()    {}
static void CALL StubProcessRdpList()  {}
static void CALL StubShowCFB()         {}

class DynLibRSPPlugin final : public RSPPlugin {
public:
    explicit DynLibRSPPlugin(const std::string& path)
        : m_LibPath(path)
    {
    }

    ~DynLibRSPPlugin() override
    {
        Shutdown();
        UnloadDLL();
    }

    // PluginBase interface
    PluginInfo GetPluginInfo() const override { return m_Info; }

    bool Initialize(CN64System* system) override
    {
        m_System = system;
        if (!LoadDLL()) return false;
        if (!LoadFunctions()) return false;
        if (!Initiate()) return false;
        m_Initialized = true;
        Log::info("DynLibRSPPlugin: Initialized from " + m_LibPath);
        return true;
    }

    void Shutdown() override
    {
        if (m_Initialized && m_fnRomClosed)
            m_fnRomClosed();
        if (m_fnCloseDLL)
            m_fnCloseDLL();
        m_Initialized = false;
        m_System = nullptr;
        Log::info("DynLibRSPPlugin: Shutdown");
    }

    void Reset(bool /*coldReset*/) override
    {
        if (m_fnRomClosed) m_fnRomClosed();
        if (m_fnRomOpen) m_fnRomOpen();
    }

    void Pause() override {}
    void Resume() override {}
    bool IsInitialized() const override { return m_Initialized; }
    void ShowConfigDialog(void* hwndParent) override
    {
        if (m_fnDllConfig) m_fnDllConfig(hwndParent);
    }

    size_t SaveState(uint8_t* /*data*/, size_t /*size*/) override { return 0; }
    bool LoadState(const uint8_t* /*data*/, size_t /*size*/) override { return true; }

    // RSPPlugin interface
    bool InitializeRSP(const RSPInfo& /*info*/) override { return m_Initialized; }
    void SetCPUType(bool /*useLLE*/) override {}

    uint32_t DoRSPCycles(RSPTaskType taskType) override
    {
        if (!m_fnDoRspCycles) return 0;
        return m_fnDoRspCycles(static_cast<uint32_t>(taskType));
    }

    bool DoRSPStep() override
    {
        // Most RSP DLL plugins don't expose single-step
        if (!m_fnDoRspCycles) return false;
        m_fnDoRspCycles(1);
        return true;
    }

    void BreakRSP() override {}

    void ProcessSPDMARead() override {}
    void ProcessSPDMAWrite() override {}

    uint32_t GetPC() const override
    {
        if (m_pSP_PC_REG) return *m_pSP_PC_REG;
        return 0;
    }

    void SetPC(uint32_t pc) override
    {
        if (m_pSP_PC_REG) *m_pSP_PC_REG = pc & 0xFFC;
    }

    bool IsRSPRunning() const override
    {
        // SP_STATUS_REG bit 0 = Halt, when clear the RSP is running
        if (m_pSP_STATUS_REG)
            return (*m_pSP_STATUS_REG & 0x1) == 0;
        return false;
    }

    uint32_t GetStatus() const override
    {
        if (m_pSP_STATUS_REG) return *m_pSP_STATUS_REG;
        return 0;
    }

private:
    bool LoadDLL()
    {
        try
        {
            m_Lib.Load(m_LibPath);
        }
        catch (const std::exception& e)
        {
            Log::errorf("DynLibRSPPlugin: Failed to load %s: %s", m_LibPath.c_str(), e.what());
            return false;
        }

        using GetDllInfoFunc = void(CALL*)(void*);
        auto getDllInfo = m_Lib.GetSymbol<GetDllInfoFunc>("GetDllInfo");
        if (getDllInfo)
        {
            struct { uint16_t Version; uint16_t Type; char Name[100]; int NormalMemory; int MemoryBswaped; } dllInfo{};
            getDllInfo(&dllInfo);
            m_Info.Type = PluginType::RSP;
            m_Info.Version = dllInfo.Version;
            m_Info.Name = dllInfo.Name;
        }
        return true;
    }

    bool LoadFunctions()
    {
        if (!m_Lib.IsLoaded()) return false;

        m_fnDoRspCycles = m_Lib.GetSymbol<RSPDoRspCycles>("DoRspCycles");
        m_fnRomClosed   = m_Lib.GetSymbol<RSPRomClosed>("RomClosed");
        m_fnCloseDLL    = m_Lib.GetSymbol<RSPCloseDLL>("CloseDLL");
        m_fnDllConfig   = m_Lib.GetSymbol<RSPDllConfig>("DllConfig");
        m_fnRomOpen     = m_Lib.GetSymbol<RSPRomOpen>("RomOpen");

        if (!m_fnDoRspCycles)
        {
            Log::error("DynLibRSPPlugin: Missing DoRspCycles entry point");
            return false;
        }
        return true;
    }

    bool Initiate()
    {
        using InitFunc = void(CALL*)(RSP_INFO_DLL, uint32_t*);
        auto initiate = m_Lib.GetSymbol<InitFunc>("InitiateRSP");
        if (!initiate)
        {
            Log::error("DynLibRSPPlugin: Missing InitiateRSP entry point");
            return false;
        }

        RSP_INFO_DLL info{};
        info.hInst = nullptr;
        info.MemoryBswaped = 1;

        if (m_System)
        {
            auto* mem = m_System->GetMemory();
            auto* regs = m_System->GetRegisters();

            if (mem)
            {
                info.RDRAM = mem->Rdram();
                info.DMEM  = mem->Dmem();
                info.IMEM  = mem->Imem();
            }

            if (regs)
            {
                // MI register: interrupt register
                info.MI_INTR_REG = &regs->m_Mips_Interface[2]; // MI_INTR_REG index = 2

                // SP registers: m_SigProcessor_Interface[0..9]
                // Index mapping:
                //   0: SP_MEM_ADDR, 1: SP_DRAM_ADDR, 2: SP_RD_LEN, 3: SP_WR_LEN
                //   4: SP_STATUS, 5: SP_DMA_FULL, 6: SP_DMA_BUSY, 7: SP_SEMAPHORE
                //   8: SP_PC, 9: SP_IBIST
                info.SP_MEM_ADDR_REG  = &regs->m_SigProcessor_Interface[0];
                info.SP_DRAM_ADDR_REG = &regs->m_SigProcessor_Interface[1];
                info.SP_RD_LEN_REG    = &regs->m_SigProcessor_Interface[2];
                info.SP_WR_LEN_REG    = &regs->m_SigProcessor_Interface[3];
                info.SP_STATUS_REG    = &regs->m_SigProcessor_Interface[4];
                info.SP_DMA_FULL_REG  = &regs->m_SigProcessor_Interface[5];
                info.SP_DMA_BUSY_REG  = &regs->m_SigProcessor_Interface[6];
                info.SP_SEMAPHORE_REG = &regs->m_SigProcessor_Interface[7];
                info.SP_PC_REG        = &regs->m_SigProcessor_Interface[8];

                // DPC registers: m_Display_ControlReg[0..7]
                info.DPC_START_REG    = &regs->m_Display_ControlReg[0];
                info.DPC_END_REG      = &regs->m_Display_ControlReg[1];
                info.DPC_CURRENT_REG  = &regs->m_Display_ControlReg[2];
                info.DPC_STATUS_REG   = &regs->m_Display_ControlReg[3];
                info.DPC_CLOCK_REG    = &regs->m_Display_ControlReg[4];
                info.DPC_BUFBUSY_REG  = &regs->m_Display_ControlReg[5];
                info.DPC_PIPEBUSY_REG = &regs->m_Display_ControlReg[6];
                info.DPC_TMEM_REG     = &regs->m_Display_ControlReg[7];

                // Cache status/PC pointers for runtime queries
                m_pSP_STATUS_REG = info.SP_STATUS_REG;
                m_pSP_PC_REG     = info.SP_PC_REG;
            }
        }

        // Plugin cross-callbacks (stubs until full plugin system wiring)
        info.CheckInterrupts = StubCheckInterrupts;
        info.ProcessDList    = StubProcessDList;
        info.ProcessAList    = StubProcessAList;
        info.ProcessRdpList  = StubProcessRdpList;
        info.ShowCFB         = StubShowCFB;

        uint32_t cycleCount = 0;
        initiate(info, &cycleCount);

        if (m_fnRomOpen)
            m_fnRomOpen();

        return true;
    }

    void UnloadDLL()
    {
        m_fnDoRspCycles = nullptr;
        m_fnRomClosed = nullptr;
        m_fnCloseDLL = nullptr;
        m_fnDllConfig = nullptr;
        m_fnRomOpen = nullptr;
        m_Lib.Unload();
    }

    std::string m_LibPath;
    DynamicLibrary m_Lib;
    PluginInfo m_Info;
    bool m_Initialized = false;

    // Cached register pointers from initialization
    uint32_t* m_pSP_STATUS_REG = nullptr;
    uint32_t* m_pSP_PC_REG     = nullptr;

    // DLL function pointers
    RSPDoRspCycles m_fnDoRspCycles = nullptr;
    RSPRomClosed   m_fnRomClosed   = nullptr;
    RSPCloseDLL    m_fnCloseDLL    = nullptr;
    RSPDllConfig   m_fnDllConfig   = nullptr;
    RSPRomOpen     m_fnRomOpen     = nullptr;
};

// ============================================================================
// Factory functions
// ============================================================================

std::unique_ptr<RSPPlugin> CreateDefaultRSPPlugin()
{
    return std::make_unique<DefaultRSPPlugin>();
}

std::unique_ptr<RSPPlugin> CreateRSPPluginFromDLL(const std::string& path)
{
    return std::make_unique<DynLibRSPPlugin>(path);
}

} // namespace Plugins
