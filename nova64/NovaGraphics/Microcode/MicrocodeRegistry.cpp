/**
 * @file MicrocodeRegistry.cpp
 * @brief Microcode detection and registry implementation
 */

#include "MicrocodeRegistry.hpp"
#include "../Common/Logger.hpp"
#include <algorithm>
#include <cstring>

namespace Nova64::Video {

// --- MicrocodeType Name Table ---

const char* MicrocodeTypeName(MicrocodeType type) {
    switch (type) {
    case MicrocodeType::F3D:           return "Fast3D";
    case MicrocodeType::F3DEX:         return "F3DEX";
    case MicrocodeType::F3DEX2:        return "F3DEX2";
    case MicrocodeType::F3DPD:         return "F3DPD (Perfect Dark)";
    case MicrocodeType::F3DCBFD:       return "F3DCBFD (Conker)";
    case MicrocodeType::F3DTEXA:       return "F3DTEXA (Yoshi)";
    case MicrocodeType::F3DLX:         return "F3DLX";
    case MicrocodeType::F3DLX2:        return "F3DLX2";
    case MicrocodeType::F3DZEX:        return "F3DZEX (Zelda)";
    case MicrocodeType::F3DFLX2:       return "F3DFLX2 (Star Fox)";
    case MicrocodeType::F3DGOLDEN:     return "F3DGOLDEN (GoldenEye)";
    case MicrocodeType::L3D:           return "L3D (Line3D)";
    case MicrocodeType::L3DEX:         return "L3DEX (Line3D EX)";
    case MicrocodeType::L3DEX2:        return "L3DEX2 (Line3D EX2)";
    case MicrocodeType::F3DSETA:       return "F3DSETA";
    case MicrocodeType::F3DEX2ACCLAIM: return "F3DEX2ACCLAIM (Turok)";
    case MicrocodeType::F3DAM:         return "F3DAM (Army Men)";
    case MicrocodeType::S2DEX:         return "S2DEX";
    case MicrocodeType::S2DEX2:        return "S2DEX2";
    case MicrocodeType::Turbo3D:       return "Turbo3D";
    case MicrocodeType::ZSort:         return "ZSort";
    case MicrocodeType::WaveRace:      return "Wave Race";
    case MicrocodeType::DiddyKong:     return "Diddy Kong Racing";
    case MicrocodeType::StarWars:      return "Star Wars: Rogue Squadron";
    case MicrocodeType::JetForce:      return "Jet Force Gemini";
    case MicrocodeType::ZSortBOSS:     return "ZSort BOSS";
    case MicrocodeType::F5Rogue:       return "F5 Rogue Squadron";
    case MicrocodeType::F5Indi_Naboo:  return "F5 Indi/Naboo";
    case MicrocodeType::F3DBETA:       return "F3D Beta";
    case MicrocodeType::F3DEX3:        return "F3DEX3";
    case MicrocodeType::F3DEX095:      return "F3DEX 0.95";
    case MicrocodeType::T3DUX:         return "Turbo3D UX";
    default:                           return "Unknown";
    }
}

// --- MicrocodeCapabilities ---

MicrocodeCapabilities MicrocodeCapabilities::ForType(MicrocodeType type) {
    MicrocodeCapabilities caps{};
    caps.type = type;

    switch (type) {
    // --- F3D (original) ---
    case MicrocodeType::F3D:
        caps.gbiVersion = GBIVersion::GBI1;
        caps.maxVertices = 16;
        caps.maxTriPerCmd = 1;
        caps.hasModifyVtx = false;
        caps.hasCullDL = false;
        caps.hasBranchZ = false;
        caps.hasNearClip = false;
        caps.hasLighting = true;
        caps.hasFog = true;
        caps.hasTextureGen = true;
        caps.hasQuadCommand = false;
        caps.hasLine3D = false;
        caps.hasSpriteCommands = false;
        caps.hasZSort = false;
        caps.hasDMAIO = false;
        caps.hasLoadUcode = false;
        caps.hasPointLight = false;
        break;

    // --- F3DEX ---
    case MicrocodeType::F3DEX:
    case MicrocodeType::F3DLX:
        caps.gbiVersion = GBIVersion::GBI1;
        caps.maxVertices = 32;
        caps.maxTriPerCmd = 2;
        caps.hasModifyVtx = true;
        caps.hasCullDL = true;
        caps.hasBranchZ = true;
        caps.hasNearClip = false;
        caps.hasLighting = true;
        caps.hasFog = true;
        caps.hasTextureGen = true;
        caps.hasQuadCommand = true;
        caps.hasLine3D = true;
        caps.hasSpriteCommands = false;
        caps.hasZSort = false;
        caps.hasDMAIO = false;
        caps.hasLoadUcode = true;
        caps.hasPointLight = false;
        break;

    // --- F3DEX2 (most common) ---
    case MicrocodeType::F3DEX2:
    case MicrocodeType::F3DLX2:
    case MicrocodeType::F3DFLX2:
        caps.gbiVersion = GBIVersion::GBI2;
        caps.maxVertices = 32;
        caps.maxTriPerCmd = 2;
        caps.hasModifyVtx = true;
        caps.hasCullDL = true;
        caps.hasBranchZ = true;
        caps.hasNearClip = true;
        caps.hasLighting = true;
        caps.hasFog = true;
        caps.hasTextureGen = true;
        caps.hasQuadCommand = true;
        caps.hasLine3D = true;
        caps.hasSpriteCommands = false;
        caps.hasZSort = false;
        caps.hasDMAIO = true;
        caps.hasLoadUcode = true;
        caps.hasPointLight = false;
        break;

    // --- F3DZEX (Zelda) ---
    case MicrocodeType::F3DZEX:
        caps.gbiVersion = GBIVersion::GBI2;
        caps.maxVertices = 32;
        caps.maxTriPerCmd = 2;
        caps.hasModifyVtx = true;
        caps.hasCullDL = true;
        caps.hasBranchZ = true;
        caps.hasNearClip = true;
        caps.hasLighting = true;
        caps.hasFog = true;
        caps.hasTextureGen = true;
        caps.hasQuadCommand = true;
        caps.hasLine3D = true;
        caps.hasSpriteCommands = false;
        caps.hasZSort = false;
        caps.hasDMAIO = true;
        caps.hasLoadUcode = true;
        caps.hasPointLight = true; // Zelda point lights
        break;

    // --- Perfect Dark ---
    case MicrocodeType::F3DPD:
        caps.gbiVersion = GBIVersion::GBI1;
        caps.maxVertices = 80;
        caps.maxTriPerCmd = 2;
        caps.hasModifyVtx = true;
        caps.hasCullDL = true;
        caps.hasBranchZ = true;
        caps.hasNearClip = false;
        caps.hasLighting = true;
        caps.hasFog = true;
        caps.hasTextureGen = true;
        caps.hasQuadCommand = false;
        caps.hasLine3D = false;
        caps.hasSpriteCommands = false;
        caps.hasZSort = false;
        caps.hasDMAIO = false;
        caps.hasLoadUcode = true;
        caps.hasPointLight = false;
        break;

    // --- Conker's Bad Fur Day ---
    case MicrocodeType::F3DCBFD:
        caps.gbiVersion = GBIVersion::GBI2;
        caps.maxVertices = 32;
        caps.maxTriPerCmd = 2;
        caps.hasModifyVtx = true;
        caps.hasCullDL = true;
        caps.hasBranchZ = true;
        caps.hasNearClip = true;
        caps.hasLighting = true;
        caps.hasFog = true;
        caps.hasTextureGen = true;
        caps.hasQuadCommand = true;
        caps.hasLine3D = true;
        caps.hasSpriteCommands = false;
        caps.hasZSort = false;
        caps.hasDMAIO = true;
        caps.hasLoadUcode = true;
        caps.hasPointLight = false;
        break;

    // --- GoldenEye ---
    case MicrocodeType::F3DGOLDEN:
        caps.gbiVersion = GBIVersion::GBI1;
        caps.maxVertices = 32;
        caps.maxTriPerCmd = 1;
        caps.hasModifyVtx = false;
        caps.hasCullDL = false;
        caps.hasBranchZ = false;
        caps.hasNearClip = false;
        caps.hasLighting = true;
        caps.hasFog = true;
        caps.hasTextureGen = true;
        caps.hasQuadCommand = false;
        caps.hasLine3D = false;
        caps.hasSpriteCommands = false;
        caps.hasZSort = false;
        caps.hasDMAIO = false;
        caps.hasLoadUcode = false;
        caps.hasPointLight = false;
        break;

    // --- Yoshi's Story ---
    case MicrocodeType::F3DTEXA:
        caps.gbiVersion = GBIVersion::GBI1;
        caps.maxVertices = 32;
        caps.maxTriPerCmd = 1;
        caps.hasModifyVtx = true;
        caps.hasCullDL = true;
        caps.hasBranchZ = true;
        caps.hasNearClip = false;
        caps.hasLighting = true;
        caps.hasFog = true;
        caps.hasTextureGen = true;
        caps.hasQuadCommand = true;
        caps.hasLine3D = true;
        caps.hasSpriteCommands = false;
        caps.hasZSort = false;
        caps.hasDMAIO = false;
        caps.hasLoadUcode = true;
        caps.hasPointLight = false;
        break;

    // --- S2DEX family ---
    case MicrocodeType::S2DEX:
        caps.gbiVersion = GBIVersion::GBI_S2DEX;
        caps.maxVertices = 4;
        caps.maxTriPerCmd = 1;
        caps.hasModifyVtx = false;
        caps.hasCullDL = false;
        caps.hasBranchZ = false;
        caps.hasNearClip = false;
        caps.hasLighting = false;
        caps.hasFog = false;
        caps.hasTextureGen = false;
        caps.hasQuadCommand = false;
        caps.hasLine3D = false;
        caps.hasSpriteCommands = true;
        caps.hasZSort = false;
        caps.hasDMAIO = false;
        caps.hasLoadUcode = true;
        caps.hasPointLight = false;
        break;

    case MicrocodeType::S2DEX2:
        caps.gbiVersion = GBIVersion::GBI_S2DEX2;
        caps.maxVertices = 4;
        caps.maxTriPerCmd = 1;
        caps.hasModifyVtx = false;
        caps.hasCullDL = false;
        caps.hasBranchZ = false;
        caps.hasNearClip = false;
        caps.hasLighting = false;
        caps.hasFog = false;
        caps.hasTextureGen = false;
        caps.hasQuadCommand = false;
        caps.hasLine3D = false;
        caps.hasSpriteCommands = true;
        caps.hasZSort = false;
        caps.hasDMAIO = true;
        caps.hasLoadUcode = true;
        caps.hasPointLight = false;
        break;

    // --- Turbo3D ---
    case MicrocodeType::Turbo3D:
        caps.gbiVersion = GBIVersion::GBI1;
        caps.maxVertices = 16;
        caps.maxTriPerCmd = 1;
        caps.hasModifyVtx = false;
        caps.hasCullDL = false;
        caps.hasBranchZ = false;
        caps.hasNearClip = false;
        caps.hasLighting = true;
        caps.hasFog = false;
        caps.hasTextureGen = false;
        caps.hasQuadCommand = false;
        caps.hasLine3D = false;
        caps.hasSpriteCommands = false;
        caps.hasZSort = false;
        caps.hasDMAIO = false;
        caps.hasLoadUcode = false;
        caps.hasPointLight = false;
        break;

    // --- ZSort ---
    case MicrocodeType::ZSort:
        caps.gbiVersion = GBIVersion::GBI_Custom;
        caps.maxVertices = 256;
        caps.maxTriPerCmd = 1;
        caps.hasModifyVtx = false;
        caps.hasCullDL = false;
        caps.hasBranchZ = false;
        caps.hasNearClip = false;
        caps.hasLighting = true;
        caps.hasFog = true;
        caps.hasTextureGen = false;
        caps.hasQuadCommand = false;
        caps.hasLine3D = false;
        caps.hasSpriteCommands = false;
        caps.hasZSort = true;
        caps.hasDMAIO = false;
        caps.hasLoadUcode = false;
        caps.hasPointLight = false;
        break;

    // --- Line3D family (wireframe variants) ---
    case MicrocodeType::L3D:
        caps = ForType(MicrocodeType::F3D);
        caps.type = MicrocodeType::L3D;
        caps.hasLine3D = true;
        break;

    case MicrocodeType::L3DEX:
        caps = ForType(MicrocodeType::F3DEX);
        caps.type = MicrocodeType::L3DEX;
        caps.hasLine3D = true;
        break;

    case MicrocodeType::L3DEX2:
        caps = ForType(MicrocodeType::F3DEX2);
        caps.type = MicrocodeType::L3DEX2;
        caps.hasLine3D = true;
        break;

    // --- F3DSETA (alternate MoveWord encoding) ---
    case MicrocodeType::F3DSETA:
        caps = ForType(MicrocodeType::F3D);
        caps.type = MicrocodeType::F3DSETA;
        break;

    // --- F3DEX2ACCLAIM (Turok 2/3, South Park) ---
    case MicrocodeType::F3DEX2ACCLAIM:
        caps = ForType(MicrocodeType::F3DEX2);
        caps.type = MicrocodeType::F3DEX2ACCLAIM;
        break;

    // --- F3DAM (Army Men) ---
    case MicrocodeType::F3DAM:
        caps = ForType(MicrocodeType::F3DEX2);
        caps.type = MicrocodeType::F3DAM;
        break;

    // --- ZSortBOSS ---
    case MicrocodeType::ZSortBOSS:
        caps = ForType(MicrocodeType::ZSort);
        caps.type = MicrocodeType::ZSortBOSS;
        break;

    // --- F3DBETA (Star Wars SoE, early Wave Race) ---
    case MicrocodeType::F3DBETA:
        caps = ForType(MicrocodeType::F3D);
        caps.type = MicrocodeType::F3DBETA;
        break;

    // --- F3DEX3 (F3DEX2 derivative) ---
    case MicrocodeType::F3DEX3:
        caps = ForType(MicrocodeType::F3DEX2);
        caps.type = MicrocodeType::F3DEX3;
        break;

    // --- F3DEX 0.95 (Mario Kart 64) ---
    case MicrocodeType::F3DEX095:
        caps = ForType(MicrocodeType::F3DEX);
        caps.type = MicrocodeType::F3DEX095;
        break;

    // --- Factor 5: Rogue Squadron ---
    case MicrocodeType::F5Rogue:
        caps.gbiVersion = GBIVersion::GBI_Custom;
        caps.maxVertices = 32;
        caps.maxTriPerCmd = 1;
        caps.hasModifyVtx = false;
        caps.hasCullDL = false;
        caps.hasBranchZ = false;
        break;

    // --- Factor 5: Indiana Jones / Battle for Naboo ---
    case MicrocodeType::F5Indi_Naboo:
        caps.gbiVersion = GBIVersion::GBI_Custom;
        caps.maxVertices = 32;
        caps.maxTriPerCmd = 2;
        caps.hasModifyVtx = false;
        caps.hasCullDL = false;
        caps.hasBranchZ = false;
        break;

    // --- Turbo3D UX (Toukon Road) ---
    case MicrocodeType::T3DUX:
        caps = ForType(MicrocodeType::Turbo3D);
        caps.type = MicrocodeType::T3DUX;
        break;

    default:
        // Default to F3DEX2 capabilities as safe fallback
        caps = ForType(MicrocodeType::F3DEX2);
        caps.type = type;
        break;
    }

    return caps;
}

// --- CRC32 Lookup Table ---

static constexpr uint32_t CRC32_TABLE[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3, 0x0EDB8832,
    0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
    0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7, 0x136C9856, 0x646BA8C0, 0xFD62F97A,
    0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3,
    0x45DF5C75, 0xDCD60DCF, 0xABD13D59, 0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
    0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB,
    0xB6662D3D, 0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01, 0x6B6B51F4,
    0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
    0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65, 0x4DB26158, 0x3AB551CE, 0xA3BC0074,
    0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525,
    0x206F85B3, 0xB966D409, 0xCE61E49F, 0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
    0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615,
    0x73DC1683, 0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7, 0xFED41B76,
    0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
    0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B, 0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6,
    0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7,
    0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D, 0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
    0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7,
    0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45, 0xA00AE278,
    0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
    0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9, 0xBDBDF21C, 0xCABAC28A, 0x53B39330,
    0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
};

// --- Constructor & Initialization ---

MicrocodeRegistry::MicrocodeRegistry() {
    InitializeDatabase();
    InitializeStringPatterns();
}

void MicrocodeRegistry::InitializeDatabase() {
    // Known microcode CRCs from N64 game library
    // These CRCs are computed from the microcode IMEM text segment

    m_crcDatabase = {
        // F3D (original Fast3D)
        {0xBC45382E, MicrocodeType::F3D, "RSP SW Version: 2.0D, 04-01-96"},
        {0x47A4F3DE, MicrocodeType::F3D, "RSP SW Version: 2.0D, 04-01-96 (alt)"},
        {0x6932365F, MicrocodeType::F3D, "F3D (Super Mario 64 US)"},

        // F3DEX
        {0x4AED54A0, MicrocodeType::F3DEX, "RSP Gfx ucode F3DEX         1.21"},
        {0x739C0126, MicrocodeType::F3DEX, "RSP Gfx ucode F3DEX         1.23"},
        {0x2EDEE7BE, MicrocodeType::F3DEX, "RSP Gfx ucode F3DEX  NoN    1.21"},
        {0x65CB4B95, MicrocodeType::F3DEX, "RSP Gfx ucode F3DEX  NoN    1.23"},
        {0xE500C017, MicrocodeType::S2DEX2, "RSP Gfx ucode S2DEX2 (MM sprites)"},

        // F3DEX2 (most common)
        {0x1A62DC2F, MicrocodeType::F3DEX2, "RSP Gfx ucode F3DEX2       2.04H"},
        {0xDA51CCDB, MicrocodeType::F3DEX2, "RSP Gfx ucode F3DEX2       2.05"},
        {0x1C4F7869, MicrocodeType::F3DEX2, "RSP Gfx ucode F3DEX2       2.07"},
        {0x0C8E5EC6, MicrocodeType::F3DEX2, "RSP Gfx ucode F3DEX2       2.08"},
        {0x98E21A9E, MicrocodeType::F3DEX2, "RSP Gfx ucode F3DEX2       2.08H"},
        {0x2F71D1B5, MicrocodeType::F3DEX2, "RSP Gfx ucode F3DEX2  NoN  2.04H"},
        {0xD20CDBEC, MicrocodeType::F3DEX2, "RSP Gfx ucode F3DEX2  NoN  2.05"},
        {0xD39A0D2F, MicrocodeType::F3DEX2, "RSP Gfx ucode F3DEX2  NoN  2.07"},
        {0x5D3099F1, MicrocodeType::F3DEX2, "RSP Gfx ucode F3DEX2  NoN  2.08"},
        {0x814F3F11, MicrocodeType::F3DEX2, "RSP Gfx ucode F3DEX2  NoN  2.08H"},

        // F3DLX
        {0x3A1CBAC3, MicrocodeType::F3DLX, "RSP Gfx ucode F3DLX         1.21"},
        {0xCB8C9B6C, MicrocodeType::F3DLX, "RSP Gfx ucode F3DLX         1.23"},

        // F3DLX2
        {0x5A72547E, MicrocodeType::F3DLX2, "RSP Gfx ucode F3DLX2       2.04"},
        {0x5414030D, MicrocodeType::F3DLX2, "RSP Gfx ucode F3DLX2       2.08"},

        // F3DZEX (Zelda OoT / MM)
        {0x31B444E4, MicrocodeType::F3DZEX, "RSP Gfx ucode F3DZEX       2.06H"},
        {0x32B0E35A, MicrocodeType::F3DZEX, "RSP Gfx ucode F3DZEX       2.08I"},
        {0xDE7D67D4, MicrocodeType::F3DZEX, "RSP Gfx ucode F3DZEX  NoN  2.06H"},
        {0x8805C26A, MicrocodeType::F3DZEX, "RSP Gfx ucode F3DZEX  NoN  2.08I"},
        {0x0D7BBE18, MicrocodeType::F3DZEX, "RSP Gfx ucode F3DZEX  NoN  2.08J"},
        {0xCB8391EB, MicrocodeType::F3DZEX, "RSP Gfx ucode F3DZEX (OOT 1.0-1.2)"},
        {0xC71C0011, MicrocodeType::F3DZEX, "RSP Gfx ucode F3DZEX (MM reload)"},
        {0xDF528A85, MicrocodeType::F3DZEX, "RSP Gfx ucode F3DZEX (MM/OOT reload)"},

        // F3DPD (Perfect Dark)
        {0x1ECFD18A, MicrocodeType::F3DPD, "RSP Gfx ucode F3DPD"},

        // F3DCBFD (Conker's Bad Fur Day)
        {0x1B4ACE88, MicrocodeType::F3DCBFD, "RSP Gfx ucode F3DCBFD"},

        // F3DTEXA (Yoshi's Story)
        {0x0D177A75, MicrocodeType::F3DTEXA, "RSP Gfx ucode F3DTEXA"},

        // F3DFLX2 (Star Fox 64)
        {0x5B5D36E3, MicrocodeType::F3DFLX2, "RSP Gfx ucode F3DFLX2 Rej  2.08H"},

        // F3DGOLDEN (GoldenEye 007)
        {0x64CC729D, MicrocodeType::F3DGOLDEN, "RSP Gfx ucode F3D (GoldenEye)"},

        // S2DEX
        {0x2C597B0F, MicrocodeType::S2DEX, "RSP Gfx ucode S2DEX  1.06"},

        // S2DEX2
        {0xA346A5CC, MicrocodeType::S2DEX2, "RSP Gfx ucode S2DEX2 2.05"},
        {0x6D8F8F8A, MicrocodeType::S2DEX2, "RSP Gfx ucode S2DEX2 2.08"},
        {0x02C399DD, MicrocodeType::S2DEX2, "RSP Gfx ucode S2DEX2 (MM/OOT)"},

        // Turbo3D
        {0xFC6529AA, MicrocodeType::Turbo3D, "Turbo3D"},

        // ZSort
        {0xB62F900F, MicrocodeType::ZSort, "ZSort"},

        // L3D (Line3D)
        {0x7D372819, MicrocodeType::L3D, "RSP SW Version: 2.0D, 04-01-96 (Line3D)"},

        // L3DEX
        {0x3271B8B5, MicrocodeType::L3DEX, "RSP Gfx ucode L3DEX         1.21"},
        {0x0C3E739F, MicrocodeType::L3DEX, "RSP Gfx ucode L3DEX         1.23"},

        // L3DEX2
        {0x29C2BFDA, MicrocodeType::L3DEX2, "RSP Gfx ucode L3DEX2       2.08"},

        // F3DSETA
        {0x7F2D0A2E, MicrocodeType::F3DSETA, "RSP SW Version: 2.0D, 04-01-96 (SetA)"},

        // F3DEX2ACCLAIM (Turok 2/3)
        {0x6E1D9B03, MicrocodeType::F3DEX2ACCLAIM, "RSP Gfx ucode F3DEX2 Acclaim"},

        // F3DAM (Army Men)
        {0x5B1F6C16, MicrocodeType::F3DAM, "RSP Gfx ucode F3DAM"},

        // ZSortBOSS
        {0x32CEC12D, MicrocodeType::ZSortBOSS, "ZSort BOSS"},

        // F3DBETA (Star Wars SoE, early Wave Race)
        {0x94C4C833, MicrocodeType::F3DBETA, "RSP SW Version: 2.0D, 04-01-96 (SoE)"},
        {0xD17906E2, MicrocodeType::F3DBETA, "RSP SW Version: 2.0D, 04-01-96 (Wave Race)"},

        // F3DEX 0.95 (Mario Kart 64)
        {0x0ACE4C3F, MicrocodeType::F3DEX095, "F3DEX 0.95 (Mario Kart 64)"},
        {0xF89248E1, MicrocodeType::F3DEX095, "F3DLX 0.95 (Mario Kart 64)"},

        // Factor 5: Rogue Squadron
        // CRC 0xDA51CCDB overlaps F3DEX2 2.05; detection is refined at a higher level via the game
        // database ForceMicrocode.

        // Factor 5: Indiana Jones / Battle for Naboo
        {0x23FEF05F, MicrocodeType::F5Indi_Naboo, "F5 Battle for Naboo"},
        {0x6859BF8E, MicrocodeType::F5Indi_Naboo, "F5 Indiana Jones"},

        // T3DUX (Toukon Road)
        {0xBAD437F2, MicrocodeType::T3DUX, "T3DUX 0.83 (Toukon Road)"},
        {0xD0A1AA3D, MicrocodeType::T3DUX, "T3DUX 0.85 (Toukon Road 2)"},
    };
}

void MicrocodeRegistry::InitializeStringPatterns() {
    // Text patterns found in DMEM microcode data segment for fallback detection.
    // These strings are embedded in the microcode by Nintendo's SDK.
    // Order matters — more specific patterns checked first.

    m_stringPatterns = {
        {"F3DZEX", MicrocodeType::F3DZEX, 0.95f},
        {"F3DCBFD", MicrocodeType::F3DCBFD, 0.95f},
        {"F3DPD", MicrocodeType::F3DPD, 0.95f},
        {"F3DTEXA", MicrocodeType::F3DTEXA, 0.95f},
        {"F3DFLX2", MicrocodeType::F3DFLX2, 0.95f},
        {"L3DEX2", MicrocodeType::L3DEX2, 0.95f},
        {"L3DEX", MicrocodeType::L3DEX, 0.90f},
        {"L3D", MicrocodeType::L3D, 0.80f},
        {"F3DLX2", MicrocodeType::F3DLX2, 0.90f},
        {"F3DLX", MicrocodeType::F3DLX, 0.90f},
        {"F3DEX2", MicrocodeType::F3DEX2, 0.90f},
        {"F3DEX", MicrocodeType::F3DEX, 0.85f},
        {"F3D", MicrocodeType::F3D, 0.70f},
        {"S2DEX2", MicrocodeType::S2DEX2, 0.90f},
        {"S2DEX", MicrocodeType::S2DEX, 0.85f},
        {"Turbo3D", MicrocodeType::Turbo3D, 0.90f},
        {"ZSort", MicrocodeType::ZSort, 0.90f},
        {"T3DUX", MicrocodeType::T3DUX, 0.90f},
        {"F3DEX3", MicrocodeType::F3DEX3, 0.95f},
        {"F5Indi", MicrocodeType::F5Indi_Naboo, 0.95f},
        {"F5Naboo", MicrocodeType::F5Indi_Naboo, 0.95f},
        {"F5Rogue", MicrocodeType::F5Rogue, 0.95f},
        {"RSP SW Version: 2.0D", MicrocodeType::F3DBETA, 0.90f},
    };
}

// --- CRC Calculation ---

uint32_t MicrocodeRegistry::CalculateCRC(const uint8_t* data, uint32_t size) const {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < size; ++i) {
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

// --- Detection Methods ---

DetectionResult MicrocodeRegistry::Detect(const uint8_t* imemData, uint32_t imemSize, const uint8_t* dmemData,
                                          uint32_t dmemSize) const {
    ++m_totalDetections;

    // Check forced override first
    if (m_forcedType != MicrocodeType::Unknown) {
        DetectionResult result;
        result.type = m_forcedType;
        result.method = DetectionMethod::ForcedByGame;
        result.confidence = 1.0f;
        result.matchDetails = "Forced override: " + std::string(MicrocodeTypeName(m_forcedType));
        return result;
    }

    // Calculate CRC of IMEM data
    uint32_t crc = CalculateCRC(imemData, imemSize);

    // Check cache first
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_cache.find(crc);
        if (it != m_cache.end()) {
            ++m_cacheHits;
            return it->second;
        }
    }

    // Try CRC match
    DetectionResult result = DetectByCRC(crc);
    if (result.type != MicrocodeType::Unknown) {
        CacheResult(crc, result);
        return result;
    }

    // Try string match on DMEM
    if (dmemData && dmemSize > 0) {
        result = DetectByString(dmemData, dmemSize);
        if (result.type != MicrocodeType::Unknown) {
            result.crc = crc;
            CacheResult(crc, result);
            return result;
        }
    }

    // Try heuristic
    result = DetectByHeuristic(imemData, imemSize);
    if (result.type != MicrocodeType::Unknown) {
        result.crc = crc;
        CacheResult(crc, result);
        return result;
    }

    // Default to F3DEX2 — most common microcode
    NOVA_LOG(LogLevel::Warning, LogSubsystem::Core, "Unknown microcode CRC 0x{:08X}, defaulting to F3DEX2", crc);
    result.type = MicrocodeType::F3DEX2;
    result.method = DetectionMethod::Default;
    result.crc = crc;
    result.confidence = 0.1f;
    result.matchDetails = "Unknown CRC, defaulting to F3DEX2";
    CacheResult(crc, result);
    return result;
}

DetectionResult MicrocodeRegistry::DetectByCRC(uint32_t crc) const {
    DetectionResult result;
    result.crc = crc;

    // Check custom CRCs first (user overrides take priority)
    for (const auto& entry : m_customCRCs) {
        if (entry.crc == crc) {
            result.type = entry.type;
            result.method = DetectionMethod::CRC_Match;
            result.confidence = 1.0f;
            result.matchDetails = std::string("Custom CRC match: ") + entry.name;
            ++m_crcHits;

            NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "Microcode detected by custom CRC: {} (0x{:08X})", entry.name,
                     crc);
            return result;
        }
    }

    // Check built-in database
    for (const auto& entry : m_crcDatabase) {
        if (entry.crc == crc) {
            result.type = entry.type;
            result.method = DetectionMethod::CRC_Match;
            result.confidence = 1.0f;
            result.matchDetails = std::string("CRC match: ") + entry.name;
            ++m_crcHits;

            NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "Microcode detected: {} (0x{:08X})", entry.name, crc);
            return result;
        }
    }

    return result; // type remains Unknown
}

DetectionResult MicrocodeRegistry::DetectByString(const uint8_t* dmemData, uint32_t dmemSize) const {
    DetectionResult result;

    // Search for ASCII strings in DMEM.
    // Microcode data segments typically contain version strings at a known offset.
    // We scan the entire DMEM for safety.

    for (const auto& pattern : m_stringPatterns) {
        uint32_t patLen = static_cast<uint32_t>(std::strlen(pattern.pattern));
        if (patLen == 0) continue;

        for (uint32_t offset = 0; offset + patLen <= dmemSize; ++offset) {
            if (std::memcmp(dmemData + offset, pattern.pattern, patLen) == 0) {
                result.type = pattern.type;
                result.method = DetectionMethod::StringMatch;
                result.confidence = pattern.confidence;

                // Extract surrounding text for debug info
                uint32_t textStart = offset;
                uint32_t textEnd = offset;
                // Find start of string (backtrack past printable chars)
                while (textStart > 0 && dmemData[textStart - 1] >= 0x20 && dmemData[textStart - 1] < 0x7F) {
                    --textStart;
                }
                // Find end of string
                while (textEnd < dmemSize && dmemData[textEnd] >= 0x20 && dmemData[textEnd] < 0x7F) {
                    ++textEnd;
                }

                std::string found(reinterpret_cast<const char*>(dmemData + textStart), textEnd - textStart);
                result.matchDetails = "String match: \"" + found + "\"";
                ++m_stringHits;

                NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "Microcode detected by string: {} -> {}", found,
                         MicrocodeTypeName(pattern.type));
                return result;
            }
        }
    }

    return result;
}

DetectionResult MicrocodeRegistry::DetectByHeuristic(const uint8_t* imemData, uint32_t imemSize) const {
    DetectionResult result;

    if (!imemData || imemSize < 256) return result;

    // Heuristic: examine command dispatch patterns in the microcode.
    // GBI1 microcodes have a command jump table that maps opcodes 0x00-0x0F.
    // GBI2 microcodes have a dual-table structure (0x00-0x0F + 0xD0-0xFF).
    // We look for characteristic patterns:
    //   - GBI2 has vertex commands at low opcodes (0x01)
    //   - GBI1 has vertex commands at 0x04

    // Count references to known opcode patterns
    int gbi1Score = 0;
    int gbi2Score = 0;
    int s2dexScore = 0;

    for (uint32_t i = 0; i + 4 <= imemSize; i += 4) {
        // Read big-endian word (RSP is big-endian)
        uint32_t word = (static_cast<uint32_t>(imemData[i]) << 24) | (static_cast<uint32_t>(imemData[i + 1]) << 16) |
                        (static_cast<uint32_t>(imemData[i + 2]) << 8) | static_cast<uint32_t>(imemData[i + 3]);

        // The high byte of each word is tested against known GBI opcodes
        uint32_t hi = (word >> 24) & 0xFF;
        if (hi == 0x04) gbi1Score += 2;                        // G_VTX in GBI1 (also G_SELECT_DL in S2DEX2)
        if (hi == 0x01 && (word & 0x00F00000)) gbi2Score += 2; // G_VTX in GBI2
        if (hi == 0xBF) gbi1Score += 2;                        // G_TRI1 in GBI1 (also G_OBJ_RENDERMODE S2DEX2)
        if (hi == 0x05) gbi2Score += 2;                        // G_TRI1 in GBI2
        if (hi == 0xB1) gbi1Score += 1;                        // G_TRI2 in GBI1
        if (hi == 0x06) gbi2Score += 1;                        // G_TRI2 in GBI2
        // S2DEX2-specific sprite opcodes (GBI2 base with sprite command set)
        if (hi == 0xBE) s2dexScore += 3; // G_OBJ_RECTANGLE (S2DEX2 only)
        if (hi == 0xBD) s2dexScore += 3; // G_OBJ_LOADTXTR (S2DEX2 only)
        if (hi == 0xBC) s2dexScore += 3; // G_OBJ_SPRITE (S2DEX2 only)
        if (hi == 0xBB) s2dexScore += 2; // G_OBJ_SCALEBG (S2DEX2 only)
        if (hi == 0xBA) s2dexScore += 2; // G_OBJ_MOVEMEM (S2DEX2)
        if (hi == 0xB0) s2dexScore += 2; // G_SELECT_DL (S2DEX1) / G_OBJ_LDTX_RECT (S2DEX2)
    }

    if (s2dexScore > 5) {
        result.type = MicrocodeType::S2DEX2; // S2DEX2 pattern detected
        result.method = DetectionMethod::Heuristic;
        result.confidence = 0.6f;
        result.matchDetails = "Heuristic: S2DEX2 sprite pattern (score " + std::to_string(s2dexScore) + ")";
        ++m_heuristicHits;

        NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "Microcode heuristic: S2DEX2 (sprite), score {}", s2dexScore);
    } else if (gbi1Score > gbi2Score && gbi1Score > 10) {
        result.type = MicrocodeType::F3DEX; // Default GBI1 guess
        result.method = DetectionMethod::Heuristic;
        result.confidence = 0.4f;
        result.matchDetails =
            "Heuristic: GBI1 pattern (score " + std::to_string(gbi1Score) + " vs " + std::to_string(gbi2Score) + ")";
        ++m_heuristicHits;

        NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "Microcode heuristic: GBI1 (F3DEX), score {}:{}", gbi1Score,
                 gbi2Score);
    } else if (gbi2Score > 10) {
        result.type = MicrocodeType::F3DEX2; // Default GBI2 guess
        result.method = DetectionMethod::Heuristic;
        result.confidence = 0.5f;
        result.matchDetails =
            "Heuristic: GBI2 pattern (score " + std::to_string(gbi2Score) + " vs " + std::to_string(gbi1Score) + ")";
        ++m_heuristicHits;

        NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "Microcode heuristic: GBI2 (F3DEX2), score {}:{}", gbi2Score,
                 gbi1Score);
    }

    return result;
}

// --- Info Lookup ---

MicrocodeInfo MicrocodeRegistry::GetInfo(MicrocodeType type) const {
    MicrocodeInfo info;
    info.type = type;
    info.caps = MicrocodeCapabilities::ForType(type);
    info.gbiVersion = info.caps.gbiVersion;
    info.name = MicrocodeTypeName(type);
    return info;
}

MicrocodeInfo MicrocodeRegistry::GetInfo(MicrocodeType type, uint32_t textAddr, uint32_t textSize, uint32_t dataAddr,
                                         uint32_t dataSize) const {
    MicrocodeInfo info = GetInfo(type);
    info.textAddress = textAddr;
    info.textSize = textSize;
    info.dataAddress = dataAddr;
    info.dataSize = dataSize;
    return info;
}

// --- Custom Registration ---

void MicrocodeRegistry::RegisterCustomCRC(uint32_t crc, MicrocodeType type, const char* name) {
    m_customCRCs.push_back({crc, type, name});

    NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "Registered custom microcode CRC: 0x{:08X} -> {} ({})", crc,
             MicrocodeTypeName(type), name);
}

void MicrocodeRegistry::SetForced(MicrocodeType type) {
    m_forcedType = type;
    ClearCache();

    NOVA_LOG(LogLevel::Warning, LogSubsystem::Core, "Microcode type forced to: {}", MicrocodeTypeName(type));
}

void MicrocodeRegistry::ClearForced() {
    m_forcedType = MicrocodeType::Unknown;
    ClearCache();

    NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "Microcode force override cleared");
}

// --- Cache Management ---

bool MicrocodeRegistry::HasCachedResult(uint32_t crc) const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return m_cache.find(crc) != m_cache.end();
}

void MicrocodeRegistry::CacheResult(uint32_t crc, const DetectionResult& result) const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_cache[crc] = result;
}

void MicrocodeRegistry::ClearCache() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_cache.clear();
}

} // namespace Nova64::Video
