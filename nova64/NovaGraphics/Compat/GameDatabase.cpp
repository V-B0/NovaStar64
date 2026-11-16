/**
 * @file GameDatabase.cpp
 * @brief Built-in profiles for over 100 N64 titles with per-game rendering overrides, and the
 *        profile lookup and custom-profile file I/O.
 */

#include "GameDatabase.hpp"
#include "../Common/Logger.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace Nova64::Video {

const char* CompatRatingName(CompatRating rating) {
    switch (rating) {
    case CompatRating::Perfect:  return "Perfect";
    case CompatRating::Playable: return "Playable";
    case CompatRating::InGame:   return "In-Game";
    case CompatRating::Menus:    return "Menus";
    case CompatRating::Intro:    return "Intro";
    case CompatRating::Nothing:  return "Nothing";
    default:                     return "Unknown";
    }
}

GameDatabase& GameDatabase::Instance() {
    static GameDatabase instance;
    return instance;
}

void GameDatabase::Init() {
    PopulateBuiltinDatabase();
    BuildIndices();
    LOG_INFO("GameDatabase initialized: {} profiles", m_profiles.size());
}

void GameDatabase::BuildIndices() {
    m_idIndex.clear();
    m_crcIndex.clear();
    for (int i = 0; i < static_cast<int>(m_profiles.size()); ++i) {
        const auto& p = m_profiles[i];
        if (!p.gameId.empty()) {
            m_idIndex[p.gameId] = i;
        }
        if (p.crc1 != 0 || p.crc2 != 0) {
            uint64_t key = static_cast<uint64_t>(p.crc1) | (static_cast<uint64_t>(p.crc2) << 32);
            m_crcIndex[key] = i;
        }
    }
}

const GameProfile* GameDatabase::FindByGameId(const std::string& gameId) const {
    auto it = m_idIndex.find(gameId);
    if (it != m_idIndex.end()) return &m_profiles[it->second];
    return nullptr;
}

const GameProfile* GameDatabase::FindByCRC(uint32_t crc1, uint32_t crc2) const {
    uint64_t key = static_cast<uint64_t>(crc1) | (static_cast<uint64_t>(crc2) << 32);
    auto it = m_crcIndex.find(key);
    if (it != m_crcIndex.end()) return &m_profiles[it->second];
    return nullptr;
}

void GameDatabase::AddProfile(const GameProfile& profile) {
    m_profiles.push_back(profile);
    int idx = static_cast<int>(m_profiles.size()) - 1;
    if (!profile.gameId.empty()) m_idIndex[profile.gameId] = idx;
    if (profile.crc1 || profile.crc2) {
        uint64_t key = static_cast<uint64_t>(profile.crc1) | (static_cast<uint64_t>(profile.crc2) << 32);
        m_crcIndex[key] = idx;
    }
}

void GameDatabase::UpdateProfile(const std::string& gameId, const GameProfile& profile) {
    auto it = m_idIndex.find(gameId);
    if (it != m_idIndex.end()) {
        m_profiles[it->second] = profile;
    } else {
        AddProfile(profile);
    }
}

// --- Custom Profile I/O ---

bool GameDatabase::LoadCustomProfiles(const std::string& path) {
    std::ifstream file(path);
    if (!file) return false;

    std::string line;
    GameProfile current;
    bool inProfile = false;

    while (std::getline(file, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line.empty() || line[0] == '#') continue;

        if (line[0] == '[') {
            // New profile section
            if (inProfile && !current.gameId.empty()) {
                UpdateProfile(current.gameId, current);
            }
            current = {};
            size_t end = line.find(']');
            if (end != std::string::npos) {
                current.gameId = line.substr(1, end - 1);
            }
            inProfile = true;
            continue;
        }

        if (!inProfile) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));

        if (key == "name") current.name = val;
        else if (key == "rating") {
            try {
                current.rating = static_cast<CompatRating>(std::stoi(val));
            } catch (...) {
                current.rating = CompatRating::Playable;
            }
        } else if (key == "crc1") {
            try {
                current.crc1 = static_cast<uint32_t>(std::stoul(val, nullptr, 16));
            } catch (...) {
                current.crc1 = 0;
            }
        } else if (key == "crc2") {
            try {
                current.crc2 = static_cast<uint32_t>(std::stoul(val, nullptr, 16));
            } catch (...) {
                current.crc2 = 0;
            }
        } else if (key == "flags") {
            try {
                current.flags = std::stoull(val, nullptr, 16);
            } catch (...) {
                current.flags = 0;
            }
        } else if (key == "microcode") current.forcedMicrocode = val;
        else if (key == "issues") current.knownIssues = val;
        else if (key == "depthBias") {
            try {
                current.depthBias = std::stoi(val);
            } catch (...) {
                current.depthBias = 0;
            }
        } else if (key == "texLodBias") {
            try {
                current.textureLodBias = std::stof(val);
            } catch (...) {
                current.textureLodBias = 0.0f;
            }
        } else if (key == "aspect") {
            try {
                current.aspectRatioOverride = std::stof(val);
            } catch (...) {
                current.aspectRatioOverride = 0.0f;
            }
        }
    }

    if (inProfile && !current.gameId.empty()) {
        UpdateProfile(current.gameId, current);
    }

    BuildIndices();
    LOG_INFO("Loaded custom profiles from {}", path);
    return true;
}

// --- Built-in Database ---

// Macro for compact profile definition
#define GAME(id, title, rat, flg) {title, id, 0, 0, 0, CompatRating::rat, "", flg}

#define GAME_CRC(id, title, c1, c2, rat, flg) {title, id, c1, c2, 0, CompatRating::rat, "", flg}

#define GAME_HACK(id, title, rat, flg, ...)                                                                            \
    [&]() -> GameProfile {                                                                                             \
        GameProfile p = GAME(id, title, rat, flg);                                                                     \
        __VA_ARGS__;                                                                                                   \
        return p;                                                                                                      \
    }()

void GameDatabase::PopulateBuiltinDatabase() {
    using namespace VideoFlags;

    m_profiles = {
        // --- Nintendo First-Party ---
        GAME("NZLE", "The Legend of Zelda: Ocarina of Time (U)", Perfect, None),
        GAME("NZLP", "The Legend of Zelda: Ocarina of Time (E)", Perfect, None),
        GAME("NZLJ", "The Legend of Zelda: Ocarina of Time (J)", Perfect, None),
        GAME("NZSE", "The Legend of Zelda: Majora's Mask (U)", Perfect, CFBReadWrite),
        GAME("NSME", "Super Mario 64 (U)", Perfect, None),
        GAME("NMKE", "Mario Kart 64 (U)", Perfect, None),
        GAME("NLBE", "Mario Party (U)", Playable, None),
        GAME("NMWE", "Mario Party 2 (U)", Playable, None),
        GAME("NMVE", "Mario Party 3 (U)", Playable, None),
        GAME("NAYE", "Yoshi's Story (U)", Perfect, None),
        GAME("NNBE", "Kirby 64: The Crystal Shards (U)", Perfect, None),
        GAME("NPOE", "Pokemon Stadium (U)", Playable, CFBReadWrite),
        GAME("NP3E", "Pokemon Stadium 2 (U)", Playable, CFBReadWrite),
        GAME("NPNE", "Pokemon Snap (U)", Playable, CFBReadWrite),
        GAME("CPUE", "Pokemon Puzzle League (U)", Playable, None),
        GAME("NFXE", "Star Fox 64 (U)", Perfect, None),
        GAME("NFXP", "Lylat Wars (E)", Perfect, None),
        GAME("NK4E", "Kirby 64 (U)", Perfect, None),
        GAME("NADE", "Animal Crossing (J)", Playable, CFBReadWrite),
        GAME("NWRE", "Wave Race 64 (U)", Perfect, AccurateBlending),
        GAME("NMQE", "Paper Mario (U)", Perfect, AccurateBlending),
        GAME("NALE", "Super Smash Bros. (U)", Perfect, None),
        GAME("NSUE", "1080 Snowboarding (U)", Perfect, None),
        GAME("NCTE", "Mario Tennis (U)", Perfect, None),
        GAME("NMGE", "Mario Golf (U)", Perfect, None),
        GAME("NDYE", "Diddy Kong Racing (U)", Perfect, None),
        GAME("NDUE", "Duck Hunt / Duck Dodgers (U)", Playable, None),
        GAME("NFFE", "F-Zero X (U)", Perfect, None),
        GAME("NPDE", "Perfect Dark (U)", Perfect, CFBReadWrite),
        GAME("NERE", "Excitebike 64 (U)", Perfect, None),
        GAME("NPFE", "Pilotwings 64 (U)", Perfect, None),
        GAME("NBCE", "Blast Corps (U)", Perfect, None),

        // --- Rare ---
        GAME("NBKE", "Banjo-Kazooie (U)", Perfect, None),
        GAME("NBTE", "Banjo-Tooie (U)", Perfect, CFBReadWrite),
        GAME("NGEE", "GoldenEye 007 (U)", Perfect, None),
        GAME("NJFE", "Jet Force Gemini (U)", Playable, CFBReadWrite),
        GAME("NDKE", "Donkey Kong 64 (U)", Playable, CFBReadWrite),
        GAME("NFUE", "Conker's Bad Fur Day (U)", Playable, CFBReadWrite | AccurateBlending),

        // --- Third Party — Action/Adventure ---
        GAME("NDOE", "Doom 64 (U)", Perfect, None),
        GAME("NQKE", "Quake 64 (U)", Perfect, None),
        GAME("NQ2E", "Quake II (U)", Playable, None),
        GAME("NTUE", "Turok: Dinosaur Hunter (U)", Perfect, None),
        GAME("NT2E", "Turok 2: Seeds of Evil (U)", Perfect, AccurateBlending),
        GAME("NT3E", "Turok 3: Shadow of Oblivion (U)", Playable, AccurateBlending),
        GAME("NRZE", "Resident Evil 2 (U)", Playable, CFBReadWrite),
        GAME("NSWE", "Star Wars: Shadows of the Empire (U)", Perfect, None),
        GAME("NRSE", "Star Wars: Rogue Squadron (U)", Playable, CFBReadWrite),
        GAME("NSFE", "Star Wars Episode I: Racer (U)", Perfect, None),
        GAME("NSPE", "Superman 64 (U)", Playable, None),
        GAME("NGCE", "Castlevania (U)", Playable, None),
        GAME("NCVE", "Castlevania: Legacy of Darkness (U)", Playable, AccurateBlending),
        GAME("NMFE", "Megaman 64 (U)", Playable, None),
        GAME("NARE", "Army Men: Sarge's Heroes (U)", Playable, None),
        GAME("NBME", "Bomberman 64 (U)", Perfect, None),
        GAME("NBHE", "Bomberman Hero (U)", Perfect, None),
        GAME("NB2E", "Bomberman 64: The Second Attack (U)", Playable, None),
        GAME("NCLE", "Chameleon Twist (U)", Perfect, None),
        GAME("NGSE", "Glover (U)", Playable, None),
        GAME("NHYE", "Hybrid Heaven (U)", Playable, None),
        GAME("NMDE", "Mischief Makers (U)", Perfect, None),
        GAME("NROE", "Road Rash 64 (U)", Perfect, None),
        GAME("NSDE", "Shadow Man (U)", Playable, AccurateBlending),
        GAME("NAIE", "Rayman 2: The Great Escape (U)", Perfect, None),

        // --- Third Party — Racing ---
        GAME("NCRE", "Cruis'n USA (U)", Playable, None),
        GAME("NC2E", "Cruis'n World (U)", Playable, None),
        GAME("NC3E", "Cruis'n Exotica (U)", Playable, None),
        GAME("NASE", "All-Star Baseball 2000 (U)", Playable, None),
        GAME("NGVE", "GT 64 (U)", Perfect, None),
        GAME("NMOE", "Monaco GP (U)", Playable, None),
        GAME("NRCE", "Top Gear Rally (U)", Playable, None),
        GAME("NR2E", "Top Gear Rally 2 (U)", Playable, None),
        GAME("NSGE", "San Francisco Rush (U)", Perfect, None),
        GAME("NS2E", "San Francisco Rush 2049 (U)", Perfect, None),
        GAME("NWCE", "WCW vs. NWO World Tour (U)", Playable, None),
        GAME("NWIE", "V-Rally Edition 99 (U)", Playable, None),
        GAME("NXGE", "Extreme-G (U)", Perfect, None),
        GAME("NX2E", "Extreme-G XG2 (U)", Perfect, None),

        // --- Third Party — Sports ---
        GAME("NWXE", "WWF WrestleMania 2000 (U)", Playable, None),
        GAME("NW2E", "WWF No Mercy (U)", Playable, None),
        GAME("NTNE", "Tony Hawk's Pro Skater (U)", Perfect, None),
        GAME("NTHE", "Tony Hawk's Pro Skater 2 (U)", Perfect, None),
        GAME("NTK3", "Tony Hawk's Pro Skater 3 (U)", Playable, None),
        GAME("NMSE", "Madden NFL 2000 (U)", Playable, None),
        GAME("NFIE", "FIFA 99 (U)", Playable, None),
        GAME("NICE", "International Superstar Soccer 64 (U)", Playable, None),

        // --- Third Party — Platformer ---
        GAME("NGNE", "Gex 64 (U)", Playable, None),
        GAME("NJDE", "Earthworm Jim 3D (U)", Playable, None),
        GAME("NMXE", "Mystical Ninja Starring Goemon (U)", Perfect, None),
        GAME("NM2E", "Goemon's Great Adventure (U)", Perfect, None),
        GAME("NRAE", "Rocket: Robot on Wheels (U)", Perfect, None),
        GAME("NSLE", "Spider-Man (U)", Playable, None),
        GAME("NTLE", "Tonic Trouble (U)", Playable, None),

        // --- Third Party — RPG ---
        GAME("CLBE", "Ogre Battle 64 (U)", Perfect, None),
        GAME("CZLE", "Quest 64 (U)", Perfect, None),
        GAME("NHAE", "Harvest Moon 64 (U)", Perfect, None),

        // --- Third Party — Puzzle/Party/Other ---
        GAME("NTPE", "Tetrisphere (U)", Perfect, None),
        GAME("NPUE", "Puyo Puyo Sun 64 (J)", Perfect, None),
        GAME("NDRE", "Dr. Mario 64 (U)", Perfect, None),
        GAME("NWOE", "World Driver Championship (U)", Playable, AccurateBlending),
        GAME("NBVE", "Body Harvest (U)", Playable, AccurateBlending),
        GAME("NWLE", "WinBack: Covert Operations (U)", Playable, None),
        GAME("NSNE", "Sin and Punishment (J)", Playable, CFBReadWrite | AccurateBlending),
        GAME("NCUE", "Custom Robo (J)", Playable, None),
        GAME("NCWE", "Custom Robo V2 (J)", Playable, None),

        // --- Factor 5 (Advanced microcode) ---
        // Uses the primary NRSE entry above.
        GAME("NISE", "Indiana Jones and the Infernal Machine (U)", Playable, AccurateBlending),

        // --- Games with Special Microcode ---
        // Uses the primary NWRE entry above.
        GAME("NDPE", "Dinosaur Planet (Prototype)", InGame, CFBReadWrite | AccurateBlending),
    };

    // Per-game hack flags.
    for (auto& p : m_profiles) {
        // Zelda games: special handling
        if (p.gameId == "NZLE" || p.gameId == "NZLP" || p.gameId == "NZLJ" || p.gameId == "NZSE") {
            p.zelda64Hack = true;
            p.zeldaMonochrome = true;
            p.zeldaSubscreen = true;
        }
        // Paper Mario
        if (p.gameId == "NMQE") {
            p.paperMarioHack = true;
            p.paperMarioSubscreen = true;
        }
        // Perfect Dark
        if (p.gameId == "NPDE") {
            p.perfectDarkHack = true;
            p.perfectDarkDepthClear = true;
        }
        // Conker
        if (p.gameId == "NFUE") {
            p.conkerHack = true;
            p.conkerBlurPauseScreen = true;
        }
        // DK Racing
        if (p.gameId == "NDYE") {
            p.dkRacingHack = true;
        }
        // WinBack: skip TexRect (removes grey rectangle overlay)
        if (p.gameId == "NWLE") {
            p.winBackSkipTexrect = true;
        }
        // Tony Hawk: blend fix
        if (p.gameId == "NTQE") {
            p.tonyHawkBlendFix = true;
        }
        // Blast Corps: blend fix
        if (p.gameId == "NBCE") {
            p.blastCorpsBlendFix = true;
        }
        // StarCraft 64: background skip
        if (p.gameId == "NSCE") {
            p.starCraftBgSkip = true;
        }
        // Pokemon Snap: force aux copy
        if (p.gameId == "NPNE") {
            p.snapForceCopy = true;
        }
        // GoldenEye: depth clear alone
        if (p.gameId == "NGEE") {
            p.perfectDarkDepthClear = true;
        }
        // Quake, Gauntlet: don't reset otherMode
        if (p.gameId == "NQKE" || p.gameId == "NGAE") {
            p.doNotResetOtherMode = true;
        }
        // F1 Pole Position, Vigilante 8, Roadsters, Extreme G 2
        if (p.gameId == "NF1E" || p.gameId == "NV8E" || p.gameId == "NRDE" || p.gameId == "NGXE") {
            p.noDepthFrameBuffers = true;
        }
    }
}

#undef GAME
#undef GAME_CRC
#undef GAME_HACK

} // namespace Nova64::Video
