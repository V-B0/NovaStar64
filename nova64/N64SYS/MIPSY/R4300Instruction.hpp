/**
 * @file R4300Instruction.hpp
 * @brief Decoded view of one MIPS R4300i instruction: disassembly, register usage and delay-slot hazards.
 */
#pragma once
#include <string>
#include <bitset>
#include "R4300Iops.hpp"

class R4300iInstruction {
public:
    R4300iInstruction(uint64_t address, uint32_t instruction);

    uint64_t GetAddress() const { return m_Address; }
    uint32_t GetInstruction() const { return m_Instruction; }

    const std::string& GetName() const {
        if (m_Name.empty()) DecodeName();
        return m_Name;
    }

    const std::string& GetParam() const {
        if (m_Param.empty()) DecodeParam();
        return m_Param;
    }

    std::string GetFullString() const { return GetName() + " " + GetParam(); }

    /// General-purpose registers read and written (0xFFFFFFFF = none) and whether HI/LO are touched.
    struct RegisterInfo {
        uint32_t read1 = 0xFFFFFFFF;
        uint32_t read2 = 0xFFFFFFFF;
        uint32_t write = 0xFFFFFFFF;
        bool readsHI = false;
        bool readsLO = false;
        bool writesHI = false;
        bool writesLO = false;
    };

    RegisterInfo GetRegisterInfo() const;

    /// True if the instruction in the delay slot writes a register (or HI/LO, or the FP condition) that this instruction reads.
    bool AffectedByDelaySlot(uint32_t delaySlotInstruction) const;

private:
    R4300iInstruction() = delete;
    R4300iInstruction(const R4300iInstruction&) = delete;
    R4300iInstruction& operator=(const R4300iInstruction&) = delete;

    void DecodeName() const;
    void DecodeParam() const;

    uint64_t m_Address;
    uint32_t m_Instruction;
    mutable std::string m_Name;
    mutable std::string m_Param;
};
