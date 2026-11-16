/**
 * @file GameDatabase.hpp
 * @brief Game Compatibility Database.
 */
// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — Game Compatibility Database
// Per-game overrides, known bugs workarounds, microcode detection,
// compatibility ratings, and automatic settings adjustment.
// ══════════════════════════════════════════════════════════════════════════════

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <array>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════
//  Compatibility Rating
// ═══════════════════════════════════════════════════════════════

enum class CompatRating : uint8_t {
    Perfect = 5,  // No known issues
    Playable = 4, // Minor visual glitches, fully playable
    InGame = 3,   // Major visual issues but somewhat playable
    Menus = 2,    // Gets to menus but not in-game
    Intro = 1,    // Only shows intro/logos
    Nothing = 0   // Crashes or black screen
};

const char* CompatRatingName(CompatRating rating);

// ═══════════════════════════════════════════════════════════════
//  Video Override Flags
// ═══════════════════════════════════════════════════════════════

// Each bit controls a specific rendering behavior override
namespace VideoFlags {
constexpr uint64_t None = 0;

// Framebuffer
constexpr uint64_t ForceNativeRes = 1ULL << 0;
constexpr uint64_t CFBReadWrite = 1ULL << 1;
constexpr uint64_t NoFBEmulation = 1ULL << 2;
constexpr uint64_t HalfResVI = 1ULL << 3;
constexpr uint64_t NoDepthCompare = 1ULL << 4;

// Textures
constexpr uint64_t Force16BitTex = 1ULL << 8;
constexpr uint64_t NoMipmaps = 1ULL << 9;
constexpr uint64_t PointSampleTextures = 1ULL << 10;
constexpr uint64_t DisableHiResTex = 1ULL << 11;
constexpr uint64_t AlphaTestFixup = 1ULL << 12;

// Blending
constexpr uint64_t AccurateBlending = 1ULL << 16;
constexpr uint64_t NoAlphaTest = 1ULL << 17;
constexpr uint64_t ForceFogDisable = 1ULL << 18;
constexpr uint64_t LegacyBlendMath = 1ULL << 19;

// Geometry
constexpr uint64_t RejectBadTris = 1ULL << 24;
constexpr uint64_t ClipFix = 1ULL << 25;
constexpr uint64_t FixedW = 1ULL << 26;
constexpr uint64_t NearPlaneClip = 1ULL << 27;

// Microcode
constexpr uint64_t ForceMicrocode = 1ULL << 32;
constexpr uint64_t IgnoreOtherModeH = 1ULL << 33;
constexpr uint64_t LegacyCombiner = 1ULL << 34;

// Performance
constexpr uint64_t DisableAsync = 1ULL << 40;
constexpr uint64_t HalfSpeedDL = 1ULL << 41;
constexpr uint64_t NoRDPSkip = 1ULL << 42;

// Misc
constexpr uint64_t CorrectAspect43 = 1ULL << 48;
constexpr uint64_t ForcePALTiming = 1ULL << 49;
constexpr uint64_t PALSpeedFix = 1ULL << 50;
constexpr uint64_t SwapRB = 1ULL << 51;
} // namespace VideoFlags

// ═══════════════════════════════════════════════════════════════
//  Game Profile
// ═══════════════════════════════════════════════════════════════

struct GameProfile {
    // Identification
    std::string name;        // Game title
    std::string gameId;      // ROM internal ID (4 chars)
    uint32_t crc1 = 0;       // ROM CRC1
    uint32_t crc2 = 0;       // ROM CRC2
    uint8_t countryCode = 0; // N64 country code

    // Compatibility
    CompatRating rating = CompatRating::Playable;
    std::string knownIssues; // Description of known bugs

    // Override flags
    uint64_t flags = VideoFlags::None;

    // Forced microcode type (if ForceMicrocode flag set)
    std::string forcedMicrocode; // e.g., "F3DEX2", "S2DEX"

    // Custom parameters
    int32_t depthBias = 0;            // Depth bias adjustment
    float textureLodBias = 0.0f;      // Texture LOD bias
    float aspectRatioOverride = 0.0f; // 0 = auto
    uint32_t maxTextureSize = 0;      // 0 = no limit
    float fogAdjust = 1.0f;           // Fog multiplier adjustment
    int32_t viWidthOverride = 0;      // Force VI width
    int32_t viHeightOverride = 0;     // Force VI height

    // Texture filter override (-1 = default)
    int textureFilter = -1;

    // Specific hacks for individual games
    bool zelda64Hack = false;
    bool paperMarioHack = false;
    bool perfectDarkHack = false;
    bool conkerHack = false;
    bool dkRacingHack = false;

    // Extended game-specific hack flags
    bool winBackSkipTexrect = false;    // Skip TEXRECT (removes grey rects)
    bool zeldaMonochrome = false;       // Monochrome effect via CI8 over RGBA16
    bool zeldaSubscreen = false;        // Subscreen FB delay fix
    bool paperMarioSubscreen = false;   // Blue channel adjust on FB copy
    bool perfectDarkDepthClear = false; // Force clear depth alone
    bool conkerBlurPauseScreen = false; // FB→depth→pause blur effect
    bool starCraftBgSkip = false;       // Skip RGBA16 LUT FB textures
    bool snapForceCopy = false;         // Force aux buffer copy at fullsync
    bool tonyHawkBlendFix = false;      // Fix blend mode 0x0150
    bool blastCorpsBlendFix = false;    // Fix blend ZERO+ONE condition
    bool noDepthFrameBuffers = false;   // Don't use depth FBs as textures
    bool doNotResetOtherMode = false;   // Keep otherMode across DLists

    // Check a flag
    bool HasFlag(uint64_t flag) const { return (flags & flag) != 0; }
};

// ═══════════════════════════════════════════════════════════════
//  Game Database
// ═══════════════════════════════════════════════════════════════

class GameDatabase {
public:
    static GameDatabase& Instance();

    void Init();

    // Lookup by ROM header info
    const GameProfile* FindByGameId(const std::string& gameId) const;
    const GameProfile* FindByCRC(uint32_t crc1, uint32_t crc2) const;
    const GameProfile* FindByName(const std::string& name) const;

    // Add/update a profile
    void AddProfile(const GameProfile& profile);
    void UpdateProfile(const std::string& gameId, const GameProfile& profile);

    // Load/save custom overrides
    bool LoadCustomProfiles(const std::string& path);
    bool SaveCustomProfiles(const std::string& path) const;

    // Get all profiles
    const std::vector<GameProfile>& GetAllProfiles() const { return m_profiles; }
    int GetProfileCount() const { return static_cast<int>(m_profiles.size()); }

    // Stats
    struct DatabaseStats {
        int total = 0;
        int perfect = 0;
        int playable = 0;
        int inGame = 0;
        int menus = 0;
        int intro = 0;
        int nothing = 0;
    };
    DatabaseStats GetStats() const;

private:
    GameDatabase() = default;

    std::vector<GameProfile> m_profiles;
    std::unordered_map<std::string, int> m_idIndex; // gameId → index
    std::unordered_map<uint64_t, int> m_crcIndex;   // (crc1|crc2<<32) → index

    void BuildIndices();
    void PopulateBuiltinDatabase();
};

} // namespace Nova64::Video
