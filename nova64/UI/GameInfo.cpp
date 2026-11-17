#include "GameInfo.hpp"

#include <cctype>
#include <cstring>

namespace NovaUI {

namespace {

struct Entry {
    const char* key; ///< the name in capitals, without spaces or punctuation
    bool exact;      ///< the whole name must match (the key is also a prefix of other titles)
    GameInfo info;
};

// "Playable" is claimed only for the games listed as playable in the README; anything else is untested.
const Entry kEntries[] = {
    {"SUPERMARIO64",
     false,
     {"Super Mario 64", "Nintendo", 1996,
      "Mario leaps into paintings to hunt for Power Stars in Princess Peach's castle, in the 3D platformer that set "
      "the standard for the genre.",
      "Playable"}},
    {"THELEGENDOFZELDA",
     true,
     {"The Legend of Zelda: Ocarina of Time", "Nintendo", 1998,
      "Link must travel through time to stop Ganondorf and save Hyrule in this legendary adventure.", "Playable"}},
    {"ZELDAMAJORASMASK",
     false,
     {"The Legend of Zelda: Majora's Mask", "Nintendo", 2000,
      "Three days remain before the moon falls on Termina. Wear the masks, relive the days and save a world caught in "
      "a loop.",
      "Playable", true}},
    {"MARIOKART64",
     false,
     {"Mario Kart 64", "Nintendo", 1996,
      "Four-player kart racing with items, shortcuts and some of the series' best tracks.", "Untested"}},
    {"STARFOX64",
     false,
     {"Star Fox 64", "Nintendo", 1997, "Fox McCloud and his squadron fly through branching routes of the Lylat system.",
      "Untested"}},
    {"LYLATWARS",
     false,
     {"Star Fox 64", "Nintendo", 1997, "Fox McCloud and his squadron fly through branching routes of the Lylat system.",
      "Untested"}},
    {"DONKEYKONG64",
     false,
     {"Donkey Kong 64", "Rare", 1999,
      "Five Kongs, hundreds of collectables and a big island to take back from King K. Rool.", "Untested", true}},
    {"POKEMONSTADIUM",
     false,
     {"Pok\xC3\xA9mon Stadium", "Nintendo", 2000,
      "Pok\xC3\xA9mon battles in full 3D, with tournaments and a wealth of mini-games.", "Untested"}},
    {"PERFECTDARK",
     false,
     {"Perfect Dark", "Rare", 2000,
      "Joanna Dark takes on the dataDyne corporation in a stealth-and-shooting spy thriller.", "Untested", true}},
    {"GOLDENEYE",
     false,
     {"GoldenEye 007", "Rare", 1997,
      "The Bond film as a first-person shooter, remembered for its objectives and four-player split screen.",
      "Untested"}},
    {"SMASHBROTHERS",
     false,
     {"Super Smash Bros.", "HAL Laboratory", 1999,
      "Nintendo's mascots fight on floating stages in the first Smash Bros.", "Untested"}},
    {"SUPERSMASHBROS",
     false,
     {"Super Smash Bros.", "HAL Laboratory", 1999,
      "Nintendo's mascots fight on floating stages in the first Smash Bros.", "Untested"}},
    {"BANJOKAZOOIE",
     false,
     {"Banjo-Kazooie", "Rare", 1998,
      "A bear and a bird collect jiggies and notes across colourful worlds to stop a witch.", "Untested"}},
    {"BANJOTOOIE",
     false,
     {"Banjo-Tooie", "Rare", 2000, "A larger, more ambitious sequel with connected worlds and new moves.", "Untested"}},
    {"PAPERMARIO",
     false,
     {"Paper Mario", "Intelligent Systems", 2000, "A storybook role-playing game with timing-based battles.",
      "Untested"}},
    {"DIDDYKONGRACING",
     false,
     {"Diddy Kong Racing", "Rare", 1997, "Kart, hovercraft and plane racing with an adventure mode on a hub island.",
      "Untested"}},
    {"FZEROX",
     false,
     {"F-Zero X", "Nintendo", 1998, "Thirty racers at once on twisting, high-speed tracks.", "Untested"}},
    {"MARIOPARTY",
     false,
     {"Mario Party", "Hudson Soft", 1998, "Board-game rounds interrupted by mini-games, for up to four players.",
      "Untested"}},
    {"WAVERACE64",
     false,
     {"Wave Race 64", "Nintendo", 1996, "Jet-ski racing known for its water simulation.", "Untested"}},
    {"KIRBY64",
     false,
     {"Kirby 64: The Crystal Shards", "HAL Laboratory", 2000,
      "Kirby combines copy abilities in a side-scrolling adventure.", "Untested"}},
    {"CONKER",
     false,
     {"Conker's Bad Fur Day", "Rare", 2001, "A crude, comedic platformer starring a hung-over squirrel.", "Untested"}},
    {"YOSHISSTORY",
     false,
     {"Yoshi's Story", "Nintendo", 1997, "A storybook platformer where the goal is to eat fruit.", "Untested"}},
    {"BLASTCORPS",
     false,
     {"Blast Corps", "Rare", 1997, "Clear a path for a runaway missile carrier by demolishing everything in the way.",
      "Untested"}},
    {"PILOTWINGS64",
     false,
     {"Pilotwings 64", "Nintendo", 1996, "Hang gliders, rocket belts and gyrocopters over open islands.", "Untested"}},
    {"1080", false, {"1080\xC2\xB0 Snowboarding", "Nintendo", 1998, "Fast, trick-heavy snowboarding.", "Untested"}},
    {"EXCITEBIKE64",
     false,
     {"Excitebike 64", "Left Field Productions", 2000, "Dirt-bike racing and stunts.", "Untested"}},
    {"POKEMONSNAP",
     false,
     {"Pok\xC3\xA9mon Snap", "HAL Laboratory", 1999,
      "Photograph Pok\xC3\xA9mon as a rail carries you through their habitats.", "Untested"}},
    {"MARIOTENNIS", false, {"Mario Tennis", "Camelot", 2000, "Tennis with the Mario cast.", "Untested"}},
    {"MARIOGOLF", false, {"Mario Golf", "Camelot", 1999, "Golf with the Mario cast.", "Untested"}},
};

std::string Normalise(const std::string& s) {
    std::string out;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

const GameInfo* FindGameInfo(const std::string& libraryName) {
    const std::string name = Normalise(libraryName);
    if (name.empty()) return nullptr;
    for (const Entry& e : kEntries) {
        if (e.exact ? name == e.key : name.find(e.key) != std::string::npos) return &e.info;
    }
    return nullptr;
}

} // namespace NovaUI
