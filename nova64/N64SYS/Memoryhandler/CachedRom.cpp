/**
 * @file CachedRom.cpp
 * @brief 64DD Disk Interface handler implementation
 * 
 * Implements hardware emulation for the Nintendo 64 Disk Drive (64DD) interface
 * including ASIC registers for disk control, status, and data transfer.
 */

#include "CachedRom.hpp"
#include "../MIPSY/Register.hpp"
#include "../N64Rom.hpp"
#include <cstring>

namespace DD64 {

// Register offsets from 0x05000000 (64DD register base)
namespace DDRegAddr {
constexpr uint32_t ASIC_DATA = 0x05000000;
constexpr uint32_t ASIC_MISC_REG = 0x05000004;
constexpr uint32_t ASIC_STATUS = 0x05000508;
constexpr uint32_t ASIC_CMD = 0x05000510;
constexpr uint32_t ASIC_CUR_TK = 0x05000518;
constexpr uint32_t ASIC_BM_STATUS = 0x05000520;
constexpr uint32_t ASIC_BM_CTL = 0x05000528;
constexpr uint32_t ASIC_ERR_SECTOR = 0x05000530;
constexpr uint32_t ASIC_SEQ_STATUS = 0x05000538;
constexpr uint32_t ASIC_SEQ_CTL = 0x05000540;
constexpr uint32_t ASIC_CUR_SECTOR = 0x05000548;
constexpr uint32_t ASIC_HARD_RESET = 0x05000550;
constexpr uint32_t ASIC_C1_S0 = 0x05000558;
constexpr uint32_t ASIC_HOST_SECBYTE = 0x05000560;
constexpr uint32_t ASIC_C1_S2 = 0x05000568;
constexpr uint32_t ASIC_SEC_BYTE = 0x05000570;
constexpr uint32_t ASIC_C1_S4 = 0x05000578;
constexpr uint32_t ASIC_C1_S6 = 0x05000580;
constexpr uint32_t ASIC_CUR_ADDR = 0x05000588;
constexpr uint32_t ASIC_ID_REG = 0x05000590;
constexpr uint32_t ASIC_TEST_REG = 0x05000598;
constexpr uint32_t ASIC_TEST_PIN_SEL = 0x050005A0;
} // namespace DDRegAddr

DiskInterfaceReg::DiskInterfaceReg(uint32_t* DiskInterface) :
    ASIC_DATA(DiskInterface[0]), ASIC_MISC_REG(DiskInterface[1]), ASIC_STATUS(DiskInterface[2]),
    ASIC_CMD(DiskInterface[3]), ASIC_CUR_TK(DiskInterface[4]), ASIC_BM_STATUS(DiskInterface[5]),
    ASIC_BM_CTL(DiskInterface[6]), ASIC_ERR_SECTOR(DiskInterface[7]), ASIC_SEQ_STATUS(DiskInterface[8]),
    ASIC_SEQ_CTL(DiskInterface[9]), ASIC_CUR_SECTOR(DiskInterface[10]), ASIC_HARD_RESET(DiskInterface[11]),
    ASIC_C1_S0(DiskInterface[12]), ASIC_HOST_SECBYTE(DiskInterface[13]), ASIC_C1_S2(DiskInterface[14]),
    ASIC_SEC_BYTE(DiskInterface[15]), ASIC_C1_S4(DiskInterface[16]), ASIC_C1_S6(DiskInterface[17]),
    ASIC_CUR_ADDR(DiskInterface[18]), ASIC_ID_REG(DiskInterface[19]), ASIC_TEST_REG(DiskInterface[20]),
    ASIC_TEST_PIN_SEL(DiskInterface[21]) {}

CachedRom::CachedRom(CRegisters& Reg, uint32_t* diskRegs) :
    DiskInterfaceReg(diskRegs), m_Reg(Reg), m_RomData(nullptr), m_RomSize(0), m_DiskData(nullptr), m_DiskSize(0),
    m_RDRAM(nullptr), m_RDRAMSize(0), m_DMAActive(false), m_DMALength(0), m_DMAAddress(0), m_CurrentSector(0),
    m_IsWrite(false), m_InterruptCallback(nullptr) {
    m_C2Buffer.fill(0);
}

bool CachedRom::Read32(uint32_t Address, uint32_t& Value) {
    switch (Address) {
    case DDRegAddr::ASIC_DATA: Value = ASIC_DATA; break;

    case DDRegAddr::ASIC_MISC_REG: Value = ASIC_MISC_REG; break;

    case DDRegAddr::ASIC_STATUS: Value = ASIC_STATUS; break;

    case DDRegAddr::ASIC_CMD: Value = ASIC_CMD; break;

    case DDRegAddr::ASIC_CUR_TK: Value = ASIC_CUR_TK; break;

    case DDRegAddr::ASIC_BM_STATUS: Value = ASIC_BM_STATUS; break;

    case DDRegAddr::ASIC_BM_CTL: Value = ASIC_BM_CTL; break;

    case DDRegAddr::ASIC_ERR_SECTOR: Value = ASIC_ERR_SECTOR; break;

    case DDRegAddr::ASIC_SEQ_STATUS: Value = ASIC_SEQ_STATUS; break;

    case DDRegAddr::ASIC_SEQ_CTL: Value = ASIC_SEQ_CTL; break;

    case DDRegAddr::ASIC_CUR_SECTOR: Value = ASIC_CUR_SECTOR; break;

    case DDRegAddr::ASIC_HARD_RESET: Value = ASIC_HARD_RESET; break;

    case DDRegAddr::ASIC_C1_S0: Value = ASIC_C1_S0; break;

    case DDRegAddr::ASIC_HOST_SECBYTE: Value = ASIC_HOST_SECBYTE; break;

    case DDRegAddr::ASIC_C1_S2: Value = ASIC_C1_S2; break;

    case DDRegAddr::ASIC_SEC_BYTE: Value = ASIC_SEC_BYTE; break;

    case DDRegAddr::ASIC_C1_S4: Value = ASIC_C1_S4; break;

    case DDRegAddr::ASIC_C1_S6: Value = ASIC_C1_S6; break;

    case DDRegAddr::ASIC_CUR_ADDR: Value = ASIC_CUR_ADDR; break;

    case DDRegAddr::ASIC_ID_REG: Value = ASIC_ID_REG; break;

    case DDRegAddr::ASIC_TEST_REG: Value = ASIC_TEST_REG; break;

    case DDRegAddr::ASIC_TEST_PIN_SEL: Value = ASIC_TEST_PIN_SEL; break;

    default: Value = 0; return false;
    }

    return true;
}

bool CachedRom::Write32(uint32_t Address, uint32_t Value, uint32_t Mask) {
    Value &= Mask;

    switch (Address) {
    case DDRegAddr::ASIC_DATA: ASIC_DATA = Value; break;

    case DDRegAddr::ASIC_MISC_REG: ASIC_MISC_REG = Value; break;

    case DDRegAddr::ASIC_CMD:
        ASIC_CMD = Value;
        ProcessCommand(Value);
        break;

    case DDRegAddr::ASIC_CUR_TK: ASIC_CUR_TK = Value; break;

    case DDRegAddr::ASIC_BM_CTL: ProcessBMControl(Value); break;

    case DDRegAddr::ASIC_SEQ_CTL: ASIC_SEQ_CTL = Value; break;

    case DDRegAddr::ASIC_HARD_RESET:
        if (Value & 0x80000000) {
            HardReset();
        }
        ASIC_HARD_RESET = Value;
        break;

    case DDRegAddr::ASIC_C1_S0: ASIC_C1_S0 = Value; break;

    case DDRegAddr::ASIC_HOST_SECBYTE: ASIC_HOST_SECBYTE = Value; break;

    case DDRegAddr::ASIC_C1_S2: ASIC_C1_S2 = Value; break;

    case DDRegAddr::ASIC_SEC_BYTE: ASIC_SEC_BYTE = Value; break;

    case DDRegAddr::ASIC_C1_S4: ASIC_C1_S4 = Value; break;

    case DDRegAddr::ASIC_C1_S6: ASIC_C1_S6 = Value; break;

    case DDRegAddr::ASIC_CUR_ADDR: ASIC_CUR_ADDR = Value; break;

    case DDRegAddr::ASIC_TEST_REG: ASIC_TEST_REG = Value; break;

    case DDRegAddr::ASIC_TEST_PIN_SEL: ASIC_TEST_PIN_SEL = Value; break;

    default: return false;
    }

    return true;
}

// Handles a command written to ASIC_CMD.
void CachedRom::ProcessCommand(uint32_t Command) {
    uint32_t cmdType = (Command >> 24) & 0xFF;

    switch (cmdType) {
    case 0x00: // No operation
        break;

    case 0x01: // Seek
        ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::BUSY_STATE);
        if (IsDiskPresent()) {
            ASIC_CUR_TK = (Command >> 16) & 0xFF;
            ASIC_CUR_SECTOR = Command & 0xFFFF;
            m_CurrentSector = CalculateSectorFromPosition();
        }
        break;

    case 0x02: // Read
        ASIC_STATUS |= static_cast<uint32_t>(DDStatus::BUSY_STATE);
        if (IsDiskPresent() && m_DiskData) {
            m_CurrentSector = CalculateSectorFromPosition();
            LoadSectorToC2Buffer(m_CurrentSector);
            StartDMARead();
        } else {
            ASIC_STATUS |= static_cast<uint32_t>(DDStatus::MECHANICAL_ERROR);
            ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::BUSY_STATE);
        }
        break;

    case 0x03: // Write
        ASIC_STATUS |= static_cast<uint32_t>(DDStatus::BUSY_STATE);
        if (IsDiskPresent() && m_DiskData) {
            if (ASIC_STATUS & static_cast<uint32_t>(DDStatus::WRITE_PROTECT)) {
                ASIC_STATUS |= static_cast<uint32_t>(DDStatus::BUFFER_ERROR);
                ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::BUSY_STATE);
            } else {
                m_CurrentSector = CalculateSectorFromPosition();
                StartDMAWrite();
            }
        } else {
            ASIC_STATUS |= static_cast<uint32_t>(DDStatus::MECHANICAL_ERROR);
            ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::BUSY_STATE);
        }
        break;

    case 0x08: // Clear disk change flag
        ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::DISK_CHANGED);
        break;

    case 0x09: // Clear reset flag
        ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::RESET_STATE);
        break;

    case 0x0A: // Motor on
        if (IsDiskPresent()) {
            ASIC_STATUS |= static_cast<uint32_t>(DDStatus::MOTOR_SPINNING);
            ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::HEAD_RETRACTED);
        }
        break;

    case 0x0B: // Motor off
        ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::MOTOR_SPINNING);
        ASIC_STATUS |= static_cast<uint32_t>(DDStatus::HEAD_RETRACTED);
        break;

    default: break;
    }
}

// Handles a control value written to ASIC_BM_CTL.
void CachedRom::ProcessBMControl(uint32_t Control) {
    if (Control & static_cast<uint32_t>(DDBMControl::START)) {
        ASIC_BM_STATUS |= static_cast<uint32_t>(DDBMStatus::RUNNING);
        ASIC_BM_CTL = Control;

        if (Control & static_cast<uint32_t>(DDBMControl::BLOCK_TRANSFER)) {
            ASIC_STATUS |= static_cast<uint32_t>(DDStatus::C2_TRANSFER);
            ProcessDMA();
        }
    }

    if (Control & static_cast<uint32_t>(DDBMControl::RESET)) {
        ASIC_BM_STATUS = 0;
        ASIC_BM_CTL = 0;
        m_DMAActive = false;
        m_C2BufferOffset = 0;
        ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::C2_TRANSFER);
    }

    if (Control & static_cast<uint32_t>(DDBMControl::BLOCK_TRANSFER)) {
        ASIC_BM_STATUS |= static_cast<uint32_t>(DDBMStatus::BLOCK_MODE);
    } else {
        ASIC_BM_STATUS &= ~static_cast<uint32_t>(DDBMStatus::BLOCK_MODE);
    }

    if (Control & static_cast<uint32_t>(DDBMControl::MANAGER_MODE)) {
        ASIC_BM_STATUS |= static_cast<uint32_t>(DDBMStatus::MICRO_MODE);
    }
}

void CachedRom::HardReset() {
    ASIC_DATA = 0;
    ASIC_MISC_REG = 0;
    ASIC_STATUS = static_cast<uint32_t>(DDStatus::RESET_STATE);
    ASIC_CMD = 0;
    ASIC_CUR_TK = 0;
    ASIC_BM_STATUS = 0;
    ASIC_BM_CTL = 0;
    ASIC_ERR_SECTOR = 0;
    ASIC_SEQ_STATUS = 0;
    ASIC_SEQ_CTL = 0;
    ASIC_CUR_SECTOR = 0;
    ASIC_HARD_RESET = 0;
    ASIC_C1_S0 = 0;
    ASIC_HOST_SECBYTE = 0;
    ASIC_C1_S2 = 0;
    ASIC_SEC_BYTE = 0;
    ASIC_C1_S4 = 0;
    ASIC_C1_S6 = 0;
    ASIC_CUR_ADDR = 0;
    ASIC_ID_REG = 0x00030000;
    ASIC_TEST_REG = 0;
    ASIC_TEST_PIN_SEL = 0;

    ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::DISK_PRESENT);

    m_DMAActive = false;
    m_DMALength = 0;
    m_DMAAddress = 0;
    m_CurrentSector = 0;
    m_IsWrite = false;
    m_C2BufferOffset = 0;
    m_C2Buffer.fill(0);

    m_RomData = nullptr;
    m_RomSize = 0;
    m_DiskData = nullptr;
    m_DiskSize = 0;
}

void CachedRom::StartDMARead() {
    if (!m_RDRAM || !IsDiskPresent()) {
        ASIC_STATUS |= static_cast<uint32_t>(DDStatus::BUFFER_ERROR);
        ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::BUSY_STATE);
        return;
    }

    m_DMAAddress = ASIC_CUR_ADDR & 0x00FFFFFF;
    m_DMALength = ASIC_HOST_SECBYTE;
    m_IsWrite = false;

    if (!IsRDRAMAddressValid(m_DMAAddress, m_DMALength)) {
        ASIC_STATUS |= static_cast<uint32_t>(DDStatus::BUFFER_ERROR);
        ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::BUSY_STATE);
        return;
    }

    m_DMAActive = true;
    ASIC_STATUS |= static_cast<uint32_t>(DDStatus::DATA_REQUEST);
}

void CachedRom::StartDMAWrite() {
    if (!m_RDRAM || !IsDiskPresent()) {
        ASIC_STATUS |= static_cast<uint32_t>(DDStatus::BUFFER_ERROR);
        ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::BUSY_STATE);
        return;
    }

    m_DMAAddress = ASIC_CUR_ADDR & 0x00FFFFFF;
    m_DMALength = ASIC_HOST_SECBYTE;
    m_IsWrite = true;

    if (!IsRDRAMAddressValid(m_DMAAddress, m_DMALength)) {
        ASIC_STATUS |= static_cast<uint32_t>(DDStatus::BUFFER_ERROR);
        ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::BUSY_STATE);
        return;
    }

    m_DMAActive = true;
    ASIC_STATUS |= static_cast<uint32_t>(DDStatus::DATA_REQUEST);
}

void CachedRom::ProcessDMA() {
    if (!m_DMAActive) {
        return;
    }

    if (m_IsWrite) {
        uint32_t transferSize = (m_DMALength < C2_BUFFER_SIZE) ? m_DMALength : C2_BUFFER_SIZE;

        if (IsRDRAMAddressValid(m_DMAAddress, transferSize)) {
            std::memcpy(m_C2Buffer.data() + m_C2BufferOffset, m_RDRAM + m_DMAAddress, transferSize);

            m_C2BufferOffset += transferSize;
            m_DMAAddress += transferSize;
            m_DMALength -= transferSize;

            if (m_DMALength == 0) {
                FlushC2BufferToDisk(m_CurrentSector);
                CompleteDMA();
            }
        } else {
            ASIC_STATUS |= static_cast<uint32_t>(DDStatus::BUFFER_ERROR);
            CompleteDMA();
        }
    } else {
        uint32_t transferSize =
            (m_DMALength < (C2_BUFFER_SIZE - m_C2BufferOffset)) ? m_DMALength : (C2_BUFFER_SIZE - m_C2BufferOffset);

        if (IsRDRAMAddressValid(m_DMAAddress, transferSize)) {
            std::memcpy(m_RDRAM + m_DMAAddress, m_C2Buffer.data() + m_C2BufferOffset, transferSize);

            m_C2BufferOffset += transferSize;
            m_DMAAddress += transferSize;
            m_DMALength -= transferSize;

            if (m_DMALength == 0) {
                CompleteDMA();
            }
        } else {
            ASIC_STATUS |= static_cast<uint32_t>(DDStatus::BUFFER_ERROR);
            CompleteDMA();
        }
    }
}

void CachedRom::CompleteDMA() {
    m_DMAActive = false;
    m_C2BufferOffset = 0;

    ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::BUSY_STATE);
    ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::DATA_REQUEST);
    ASIC_STATUS &= ~static_cast<uint32_t>(DDStatus::C2_TRANSFER);

    ASIC_BM_STATUS &= ~static_cast<uint32_t>(DDBMStatus::RUNNING);

    if (!(ASIC_BM_CTL & static_cast<uint32_t>(DDBMControl::INT_MASK))) {
        ASIC_STATUS |= static_cast<uint32_t>(DDStatus::BUFFER_INTERRUPT);
        TriggerInterrupt();
    }
}

void CachedRom::LoadSectorToC2Buffer(uint32_t sector) {
    if (!m_DiskData) {
        ASIC_STATUS |= static_cast<uint32_t>(DDStatus::MECHANICAL_ERROR);
        return;
    }

    m_C2Buffer.fill(0);
    m_C2BufferOffset = 0;

    uint32_t sectorSize = 512;
    uint32_t offset = sector * sectorSize;

    if (offset < m_DiskSize) {
        uint32_t bytesToRead = ((offset + sectorSize) > m_DiskSize) ? (m_DiskSize - offset) : sectorSize;
        std::memcpy(m_C2Buffer.data(), m_DiskData + offset, bytesToRead);
    } else {
        ASIC_ERR_SECTOR = sector;
        ASIC_STATUS |= static_cast<uint32_t>(DDStatus::MECHANICAL_ERROR);
    }
}

void CachedRom::FlushC2BufferToDisk(uint32_t sector) { (void)sector; }

void CachedRom::TriggerInterrupt() {
    if (m_InterruptCallback) {
        m_InterruptCallback();
    }
}

bool CachedRom::IsRDRAMAddressValid(uint32_t address, uint32_t length) const {
    if (!m_RDRAM || m_RDRAMSize == 0) {
        return false;
    }

    if (address > m_RDRAMSize || length > m_RDRAMSize) {
        return false;
    }

    if ((address + length) > m_RDRAMSize) {
        return false;
    }

    return true;
}

uint32_t CachedRom::CalculateSectorFromPosition() const { return (ASIC_CUR_TK * 85) + ASIC_CUR_SECTOR; }

} // namespace DD64
