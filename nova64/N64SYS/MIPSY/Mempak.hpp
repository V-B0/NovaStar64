/**
 * @file Mempak.hpp
 * @brief Memory Pak declarations: controller storage, note-table types, the CMempak wrapper and file helpers.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>
#include <memory>
#include <string>
#include <fstream>
#include <filesystem>

namespace N64Controller {

// Memory Pak constants
constexpr size_t MEMPAK_PAGE_SIZE = 256;                                  // Bytes per page
constexpr size_t MEMPAK_NUM_PAGES = 128;                                  // Pages per mempak
constexpr size_t MEMPAK_TOTAL_SIZE = MEMPAK_PAGE_SIZE * MEMPAK_NUM_PAGES; // 32KB
constexpr size_t MAX_CONTROLLERS = 4;                                     // N64 supports 4 controllers
constexpr size_t MAX_SAVE_FILES = 16;                                     // Maximum save files per mempak

// Memory pak command codes.
enum class MempakCommand : uint8_t {
    ReadPage = 0x00,
    WritePage = 0x01,
    ReadStatus = 0x02,
    WriteStatus = 0x03,
    ReadCRC = 0x04,
    Reset = 0xFF
};

// Save file structure (based on N64 memory pak format)
struct SaveFileInfo {
    std::string game_name; // Game name (max 32 characters)
    std::string file_name; // File name (max 32 characters)
    uint32_t start_page;   // Starting page in memory pak
    uint32_t page_count;   // Number of pages used
    uint32_t checksum;     // CRC32 checksum
    bool valid;            // Whether this slot is in use
    bool corrupted;        // Whether the save is corrupted
    uint64_t timestamp;    // Last modification timestamp
};

struct MempakStatus {
    bool formatted;          // Whether the mempak is formatted
    bool write_protected;    // Whether writing is disabled
    bool error;              // Whether an error occurred
    uint8_t manufacturer_id; // Manufacturer ID
    uint8_t device_id;       // Device ID
    uint32_t total_pages;    // Total pages available
    uint32_t free_pages;     // Free pages remaining

    // Convert to byte representation for reading
    std::array<uint8_t, 32> to_bytes() const {
        std::array<uint8_t, 32> data{};
        data[0] = formatted ? 0x01 : 0x00;
        data[1] = write_protected ? 0x01 : 0x00;
        data[2] = error ? 0x01 : 0x00;
        data[3] = manufacturer_id;
        data[4] = device_id;
        data[5] = (total_pages >> 8) & 0xFF;
        data[6] = total_pages & 0xFF;
        data[7] = (free_pages >> 8) & 0xFF;
        data[8] = free_pages & 0xFF;
        return data;
    }
};

class MemoryPakController {
public:
    MemoryPakController();
    ~MemoryPakController();

    MemoryPakController(const MemoryPakController&) = delete;
    MemoryPakController& operator=(const MemoryPakController&) = delete;

    MemoryPakController(MemoryPakController&&) noexcept = default;
    MemoryPakController& operator=(MemoryPakController&&) noexcept = default;

    // Core operations
    bool initialize(int controller_index, const std::string& save_directory = "");
    void shutdown();

    // Memory access
    std::vector<uint8_t> read(int controller_index, uint32_t address, size_t length);
    bool write(int controller_index, uint32_t address, const std::vector<uint8_t>& data);

    // Page operations (for direct access)
    std::array<uint8_t, MEMPAK_PAGE_SIZE> read_page(int controller_index, uint32_t page);
    bool write_page(int controller_index, uint32_t page, const std::array<uint8_t, MEMPAK_PAGE_SIZE>& data);

    // File management
    bool load_save_file(int controller_index, const std::string& game_id, const std::string& save_name);
    bool save_to_file(int controller_index, const std::string& game_id, const std::string& save_name);
    bool format(int controller_index);

    // Save file management
    std::vector<SaveFileInfo> list_save_files(int controller_index) const;
    bool delete_save_file(int controller_index, const std::string& game_id, const std::string& save_name);
    bool verify_save_file(int controller_index, const std::string& game_id, const std::string& save_name) const;

    // Status and info
    MempakStatus get_status(int controller_index) const;
    bool is_connected(int controller_index) const;
    bool is_formatted(int controller_index) const;
    size_t get_free_space(int controller_index) const;

    // CRC calculation (static for utility use)
    static uint8_t calculate_crc(const uint8_t* data, size_t length);
    static uint32_t calculate_crc32(const uint8_t* data, size_t length);

private:
    struct ControllerMemoryPak {
        std::array<uint8_t, MEMPAK_TOTAL_SIZE> memory{}; // Memory content
        MempakStatus status{};                           // Current status
        std::vector<SaveFileInfo> save_files;            // List of save files
        std::string file_path;                           // Disk file path
        bool is_initialized = false;                     // Whether initialized
        bool is_dirty = false;                           // Whether memory changed

        ControllerMemoryPak() {
            status.manufacturer_id = 0x80; // Nintendo manufacturer ID
            status.device_id = 0x01;       // Memory pak device ID
            status.total_pages = MEMPAK_NUM_PAGES;
            status.free_pages = MEMPAK_NUM_PAGES;
        }
    };

    std::array<ControllerMemoryPak, MAX_CONTROLLERS> m_mempaks;
    std::string m_save_directory;

    bool load_from_disk(int controller_index);
    bool save_to_disk(int controller_index);
    void create_default_filesystem(int controller_index);

    // Page management
    bool is_valid_address(int controller_index, uint32_t address, size_t length) const;
    bool is_valid_page(int controller_index, uint32_t page) const;

    // File system operations
    bool parse_save_files(int controller_index);
    bool update_save_file_table(int controller_index);
    uint32_t find_free_pages(int controller_index, uint32_t required_pages) const;
    bool allocate_save_file(int controller_index, const SaveFileInfo& file_info);

    // Low-level memory operations
    uint8_t* get_memory_pointer(int controller_index, uint32_t address);
    const uint8_t* get_memory_pointer(int controller_index, uint32_t address) const;
};

// Wrapper with the original CMempak API; delegates to MemoryPakController.
class CMempak {
public:
    CMempak();

    // Legacy interface methods
    void ReadFrom(int32_t controller_index, uint32_t address, uint8_t* data);
    void WriteTo(int32_t controller_index, uint32_t address, uint8_t* data);

    static uint8_t CalculateCrc(uint8_t* data_to_crc);

    bool Initialize(const std::string& base_path = "saves/");
    void Cleanup();
    bool IsFormatted(int32_t controller_index) const;

private:
    CMempak(const CMempak&) = delete;
    CMempak& operator=(const CMempak&) = delete;

    std::unique_ptr<MemoryPakController> m_controller;

    // Legacy internal methods
    void LoadMempak(int32_t controller_index, bool create_if_missing);
    void Format(int32_t controller_index);

    std::array<std::array<uint8_t, MEMPAK_TOTAL_SIZE>, MAX_CONTROLLERS> m_mempak_memory;
    std::array<bool, MAX_CONTROLLERS> m_formatted{};
    std::array<bool, MAX_CONTROLLERS> m_save_exists{};

    std::array<std::fstream, MAX_CONTROLLERS> m_file_handles;

    std::string get_mempak_filename(int32_t controller_index) const;
    bool load_legacy_mempak(int32_t controller_index);
    bool save_legacy_mempak(int32_t controller_index);
};

namespace MempakUtils {
// Validation functions
bool validate_mempak_data(const uint8_t* data, size_t size);
bool validate_save_structure(const SaveFileInfo& save_info);

// Conversion functions
std::string bytes_to_string(const uint8_t* data, size_t max_length);
std::vector<uint8_t> string_to_bytes(const std::string& str, size_t max_length);

// File system helpers
std::string generate_mempak_filename(int controller_index, const std::string& base_dir = "saves/");
std::string generate_save_filename(const std::string& game_id, const std::string& save_name);

// CRC variants
uint8_t crc8(const uint8_t* data, size_t length);
uint16_t crc16(const uint8_t* data, size_t length);
uint32_t crc32(const uint8_t* data, size_t length);

// Checksum validation
bool verify_checksum(const uint8_t* data, size_t length, uint32_t expected_checksum);

// Page operations
constexpr uint32_t address_to_page(uint32_t address) { return address / MEMPAK_PAGE_SIZE; }

constexpr uint32_t page_to_address(uint32_t page) { return page * MEMPAK_PAGE_SIZE; }

constexpr uint32_t get_page_offset(uint32_t address) { return address % MEMPAK_PAGE_SIZE; }
} // namespace MempakUtils

class MemoryPakFactory {
public:
    static std::unique_ptr<MemoryPakController> create_controller();

    static std::unique_ptr<CMempak> create_legacy_mempak();

    static bool detect_mempak_type(const uint8_t* data, size_t size);

    // Format a memory pak with standard Nintendo format
    static std::vector<uint8_t> create_formatted_mempak();

    static std::vector<uint8_t> create_mempak_from_save(const std::string& save_file_path);

    static std::vector<SaveFileInfo> extract_save_files(const uint8_t* mempak_data, size_t size);
};

} // namespace N64Controller

namespace GlobalMemoryPak {
N64Controller::MemoryPakController& get_controller();

bool initialize(const std::string& save_directory = "saves/");

// Shutdown and save all memory paks
void shutdown();

extern N64Controller::CMempak g_legacy_mempak;
} // namespace GlobalMemoryPak
