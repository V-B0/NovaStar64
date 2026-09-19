/**
 * @file SRam.hpp
 * @brief S Ram declarations and types.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */
#pragma once
#include <cstdint>
#include "../../common/file.hpp"

class CSram
{
public:
    CSram(bool ReadOnly);
    ~CSram();

    void DmaFromSram(uint8_t * dest, int32_t StartOffset, uint32_t len);
    void DmaToSram(uint8_t * Source, int32_t StartOffset, uint32_t len);

private:
    CSram(void);
    CSram(const CSram &);
    CSram & operator=(const CSram &);

    bool LoadSram();

    bool m_ReadOnly;
    FileSystem::File m_File;
};
