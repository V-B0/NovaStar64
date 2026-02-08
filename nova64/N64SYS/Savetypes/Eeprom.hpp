#pragma once
#include <cstdint>
#include "../../common/file.hpp"

class CEeprom
{
public:
    CEeprom(bool ReadOnly);
    ~CEeprom();

    void EepromCommand(uint8_t * Command);

private:
    CEeprom(void);
    CEeprom(const CEeprom &);
    CEeprom & operator=(const CEeprom &);

    void HandleStatusCommand(uint8_t* Command);
    void HandleReadCommand(uint8_t* Command);
    void HandleWriteCommand(uint8_t* Command);
    void HandleRTCStatusCommand(uint8_t* Command);
    void HandleRTCReadCommand(uint8_t* Command);
    void HandleRTCWriteCommand(uint8_t* Command);
    void ProcessingError(uint8_t * Command);
    void LoadEeprom();
    void ReadFrom(uint8_t * Buffer, int32_t line);
    void WriteTo(uint8_t * Buffer, int32_t line);

    uint8_t m_EEPROM[0x800];
    bool m_ReadOnly;
    FileSystem::File m_File;
};
