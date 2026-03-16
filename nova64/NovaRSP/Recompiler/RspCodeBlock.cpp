// SuperNova64 - N64 Emulator
// RSP Code Block Implementation
// Code analysis, branch target detection, and block construction

#include "RspCodeBlock.hpp"
#include "../CPU/RspSystem.hpp"
#include "../CPU/RSPOpcode.hpp"
#include <algorithm>
#include <cstring>

namespace Nova64::RSP::Recompiler {

// ──────────────────────────────────────────────
// Constructor: create and analyze a code block
// ──────────────────────────────────────────────

RspCodeBlock::RspCodeBlock(
    CRSPSystem& system,
    uint32_t startAddress,
    RspCodeType type,
    uint32_t dispatchAddress,
    RspCodeBlocks& functions)
    : m_Functions(functions)
    , m_DispatchAddress(dispatchAddress)
    , m_StartAddress(startAddress & 0xFFC)
    , m_CodeType(type)
    , m_System(system)
{
    Analyze();
}

// ──────────────────────────────────────────────
// Query: find function block by address
// ──────────────────────────────────────────────

const RspCodeBlock* RspCodeBlock::GetFunctionBlock(uint32_t address) const
{
    auto it = m_Functions.find(address & 0xFFC);
    if (it != m_Functions.end())
        return it->second.get();
    return nullptr;
}

// ──────────────────────────────────────────────
// Instruction index lookup (PC → vector index)
// ──────────────────────────────────────────────

size_t RspCodeBlock::InstructionIndex(uint32_t pc) const
{
    auto it = m_InstructionIndex.find(pc & 0xFFC);
    if (it != m_InstructionIndex.end())
        return it->second;
    return static_cast<size_t>(-1);
}

// ──────────────────────────────────────────────
// End-address detection (is this an exit point?)
// ──────────────────────────────────────────────

bool RspCodeBlock::IsEnd(uint32_t address) const
{
    return m_End.find(address & 0xFFC) != m_End.end();
}

// ──────────────────────────────────────────────
// Check if an address exists in our instruction list
// ──────────────────────────────────────────────

bool RspCodeBlock::IsAddressInInstructions(uint32_t address) const
{
    return m_InstructionIndex.find(address & 0xFFC) != m_InstructionIndex.end();
}

// ──────────────────────────────────────────────
// Build a fast PC → index map
// ──────────────────────────────────────────────

void RspCodeBlock::BuildInstructionIndex()
{
    m_InstructionIndex.clear();
    for (size_t i = 0; i < m_Instructions.size(); i++)
    {
        uint32_t addr = m_StartAddress + static_cast<uint32_t>(i) * 4;
        m_InstructionIndex[addr & 0xFFC] = i;
    }
}

// ──────────────────────────────────────────────
// Analyze: walk IMEM from startAddress, collecting
// instructions, branch targets, function calls
// ──────────────────────────────────────────────

void RspCodeBlock::Analyze()
{
    m_Valid = false;
    m_Instructions.clear();
    m_BranchTargets.clear();
    m_FunctionCalls.clear();
    m_End.clear();

    const uint8_t* imem = m_System.IMEM();
    if (imem == nullptr) return;

    // Walk opcodes starting from m_StartAddress
    constexpr uint32_t IMEM_SIZE = 0x1000;
    constexpr uint32_t MAX_INSTRUCTIONS = IMEM_SIZE / 4;

    uint32_t pc = m_StartAddress;
    bool inDelaySlot = false;
    bool reachedEnd = false;

    for (uint32_t count = 0; count < MAX_INSTRUCTIONS && !reachedEnd; count++)
    {
        uint32_t maskedPC = pc & 0xFFC;

        // Read instruction from IMEM (big-endian)
        uint32_t opcode =
            (static_cast<uint32_t>(imem[maskedPC + 0]) << 24) |
            (static_cast<uint32_t>(imem[maskedPC + 1]) << 16) |
            (static_cast<uint32_t>(imem[maskedPC + 2]) << 8)  |
            (static_cast<uint32_t>(imem[maskedPC + 3]));

        RSPInstruction inst(maskedPC, opcode);
        m_Instructions.push_back(inst);

        if (inDelaySlot)
        {
            reachedEnd = true;
        }

        // Check for branches and jumps
        if (inst.IsBranch())
        {
            uint32_t target = inst.ConditionalBranchTarget();
            m_BranchTargets.insert(target & 0xFFC);
            inDelaySlot = true;
        }
        else if (inst.IsJump())
        {
            uint32_t target = inst.JumpTarget();
            if (inst.IsStaticCall())
            {
                m_FunctionCalls.insert(target & 0xFFC);
            }
            inDelaySlot = true;
        }
        else if (inst.IsRegisterJump())
        {
            // JR = block end, JALR = function call return
            inDelaySlot = true;
            if (inst.IsJumpReturn())
            {
                m_End.insert(maskedPC);
            }
        }

        // BREAK instruction ends block (SPECIAL funct=13)
        RSPOpcode op(opcode);
        if (op.op == RSP_SPECIAL && op.funct == RSP_SPECIAL_BREAK)
        {
            m_End.insert(maskedPC);
            reachedEnd = true;
        }

        pc = (pc + 4) & 0xFFC;
    }

    BuildInstructionIndex();
    m_Valid = !m_Instructions.empty();
}

} // namespace Nova64::RSP::Recompiler
