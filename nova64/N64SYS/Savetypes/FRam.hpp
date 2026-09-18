/**
 * @file FRam.hpp
 * @brief F Ram declarations and types.
 */
#pragma once
#include <cstdint>
#include "../../common/file.hpp"

namespace Nova64 { class StateArchive; }

class CFlashRam
{
public:
    enum Modes
    {
        FLASHRAM_MODE_NOPES = 0,
        FLASHRAM_MODE_ERASE = 1,
        FLASHRAM_MODE_WRITE = 2,
        FLASHRAM_MODE_READ = 3,
        FLASHRAM_MODE_STATUS = 4,
    };

public:
    CFlashRam(bool ReadOnly);
    ~CFlashRam();

    void DmaFromFlashram(uint8_t * dest, int32_t StartOffset, int32_t len);
    void DmaToFlashram(uint8_t * Source, int32_t StartOffset, int32_t len);
    uint32_t ReadFromFlashStatus(uint32_t PAddr);
    void WriteToFlashCommand(uint32_t Value);

    /// Save states and rewind (see StateArchive.hpp).
    void SerializeState(Nova64::StateArchive& ar);

private:
    CFlashRam(void);
    CFlashRam(const CFlashRam &);
    CFlashRam & operator=(const CFlashRam &);

    bool LoadFlashram();

    uint8_t * m_FlashRamPointer;
    Modes m_FlashFlag;
    uint64_t m_FlashStatus;
    uint32_t m_FlashRAM_Offset;
    bool m_ReadOnly;
    FileSystem::File m_File;
    uint8_t m_PageBuffer[128];
};