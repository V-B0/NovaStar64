/**
 * @file Eeprom.hpp
 * @brief EEPROM (4K/16K) and RTC save chip handler for PIF channel 4 commands.
 */
#pragma once
#include <cstdint>
#include <mutex>
#include "../../common/file.hpp"

class CEeprom {
public:
    CEeprom(void);
    CEeprom(bool ReadOnly);
    ~CEeprom();

    void EepromCommand(uint8_t* Command);

private:
    CEeprom(const CEeprom&);
    CEeprom& operator=(const CEeprom&);

    void HandleStatusCommand(uint8_t* Command);
    void HandleReadCommand(uint8_t* Command);
    void HandleWriteCommand(uint8_t* Command);
    void HandleRTCStatusCommand(uint8_t* Command);
    void HandleRTCReadCommand(uint8_t* Command);
    void HandleRTCWriteCommand(uint8_t* Command);
    void ProcessingError(uint8_t* Command);
    void LoadEeprom();
    void ReadFrom(uint8_t* Buffer, int32_t line);
    void WriteTo(uint8_t* Buffer, int32_t line);

    uint8_t m_EEPROM[0x800];
    bool m_ReadOnly;
    bool m_Is16K; // true = 16K EEPROM (2048 bytes), false = 4K (512 bytes)
    FileSystem::File m_File;
    mutable std::mutex m_FileMutex;
};
