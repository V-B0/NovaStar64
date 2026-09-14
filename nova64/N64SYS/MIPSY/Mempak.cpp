/**
 * @file Mempak.cpp
 * @brief Mempak implementation.
 */
#include "Mempak.hpp"

#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <filesystem>
#include <iomanip>

namespace fs = std::filesystem;

namespace N64Controller {

    // ==========================================================
    // MempakUtils Implementation
    // ==========================================================

    namespace MempakUtils {
        
        bool validate_mempak_data(const uint8_t* data, size_t size) {
            return data != nullptr && size == MEMPAK_TOTAL_SIZE;
        }

        bool validate_save_structure(const SaveFileInfo& save_info) {
            // Basic validation
            return !save_info.game_name.empty() && save_info.page_count > 0;
        }

        std::string bytes_to_string(const uint8_t* data, size_t max_length) {
            // N64 strings are often fixed length and null padded or space padded
            std::string result;
            for (size_t i = 0; i < max_length; ++i) {
                if (data[i] == 0) break;
                result += static_cast<char>(data[i]);
            }
            return result;
        }

        std::vector<uint8_t> string_to_bytes(const std::string& str, size_t max_length) {
            std::vector<uint8_t> result(max_length, 0);
            size_t copy_len = std::min(str.length(), max_length);
            std::memcpy(result.data(), str.data(), copy_len);
            return result;
        }

        std::string generate_mempak_filename(int controller_index, const std::string& base_dir) {
            fs::path path(base_dir);
            path.append("mempak" + std::to_string(controller_index + 1) + ".mpk");
            return path.string();
        }

        std::string generate_save_filename(const std::string& game_id, const std::string& save_name) {
            return game_id + "_" + save_name + ".save";
        }

        // CRC-8 (N64 Joybus Polynomial 0x85)
        uint8_t crc8(const uint8_t* data, size_t length) {
            uint8_t crc = 0;
            for (size_t i = 0; i < length; ++i) {
                crc ^= data[i];
                for (int j = 0; j < 8; ++j) {
                    if (crc & 0x80) {
                        crc = (crc << 1) ^ 0x85;
                    } else {
                        crc <<= 1;
                    }
                }
            }
            return crc;
        }

        uint16_t crc16(const uint8_t* data, size_t length) {
            uint16_t crc = 0;
            for (size_t i = 0; i < length; ++i) {
                crc ^= (uint16_t)data[i] << 8;
                for (int j = 0; j < 8; ++j) {
                    if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
                    else crc <<= 1;
                }
            }
            return crc;
        }

        uint32_t crc32(const uint8_t* data, size_t length) {
            uint32_t crc = 0xFFFFFFFF;
            for (size_t i = 0; i < length; ++i) {
                crc ^= data[i];
                for (int j = 0; j < 8; ++j) {
                    crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
                }
            }
            return ~crc;
        }

        bool verify_checksum(const uint8_t* data, size_t length, uint32_t expected_checksum) {
            // This is generic; specific implementation might depend on context
            // Assuming CRC32 for general verification unless specified otherwise
            return crc32(data, length) == expected_checksum;
        }
    }

    // ==========================================================
    // MemoryPakController Implementation
    // ==========================================================

    MemoryPakController::MemoryPakController() {
        // Initialize default states
        for (auto& mp : m_mempaks) {
            mp.status.manufacturer_id = 0x01; // Nintendo
            mp.status.device_id = 0x01;       // Mempak
            mp.status.total_pages = MEMPAK_NUM_PAGES;
            mp.status.free_pages = MEMPAK_NUM_PAGES;
            mp.status.formatted = false;
        }
    }

    MemoryPakController::~MemoryPakController() {
        shutdown();
    }

    bool MemoryPakController::initialize(int controller_index, const std::string& save_directory) {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return false;

        m_save_directory = save_directory;
        if (m_save_directory.empty()) {
            m_save_directory = "saves/";
        }

        // Create directory if it doesn't exist
        try {
            if (!fs::exists(m_save_directory)) {
                fs::create_directories(m_save_directory);
            }
        } catch (...) {
            return false;
        }

        m_mempaks[controller_index].file_path = MempakUtils::generate_mempak_filename(controller_index, m_save_directory);
        
        if (!load_from_disk(controller_index)) {
             // If load fails (e.g. file doesn't exist), format/create new
             format(controller_index);
             save_to_disk(controller_index);
        }

        m_mempaks[controller_index].is_initialized = true;
        return true;
    }

    void MemoryPakController::shutdown() {
        for (int i = 0; i < MAX_CONTROLLERS; ++i) {
            if (m_mempaks[i].is_initialized && m_mempaks[i].is_dirty) {
                save_to_disk(i);
            }
        }
    }

    std::vector<uint8_t> MemoryPakController::read(int controller_index, uint32_t address, size_t length) {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return {};
        const auto& mp = m_mempaks[controller_index];
        
        if (!mp.is_initialized) return {};
        if (address >= MEMPAK_TOTAL_SIZE) return {}; // Out of bounds

        size_t actual_length = length;
        if (address + length > MEMPAK_TOTAL_SIZE) {
            actual_length = MEMPAK_TOTAL_SIZE - address;
        }

        std::vector<uint8_t> result(actual_length);
        std::memcpy(result.data(), mp.memory.data() + address, actual_length);
        return result;
    }

    bool MemoryPakController::write(int controller_index, uint32_t address, const std::vector<uint8_t>& data) {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return false;
        auto& mp = m_mempaks[controller_index];
        
        if (!mp.is_initialized) return false;
        if (address >= MEMPAK_TOTAL_SIZE) return false;

        size_t length = data.size();
        if (address + length > MEMPAK_TOTAL_SIZE) {
            length = MEMPAK_TOTAL_SIZE - address;
        }

        std::memcpy(mp.memory.data() + address, data.data(), length);
        mp.is_dirty = true;
        return true;
    }

    std::array<uint8_t, MEMPAK_PAGE_SIZE> MemoryPakController::read_page(int controller_index, uint32_t page) {
        std::array<uint8_t, MEMPAK_PAGE_SIZE> result{};
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return result;
        
        if (page >= MEMPAK_NUM_PAGES) return result;

        uint32_t address = page * MEMPAK_PAGE_SIZE;
        std::memcpy(result.data(), m_mempaks[controller_index].memory.data() + address, MEMPAK_PAGE_SIZE);
        return result;
    }

    bool MemoryPakController::write_page(int controller_index, uint32_t page, const std::array<uint8_t, MEMPAK_PAGE_SIZE>& data) {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return false;
        if (page >= MEMPAK_NUM_PAGES) return false;

        uint32_t address = page * MEMPAK_PAGE_SIZE;
        std::memcpy(m_mempaks[controller_index].memory.data() + address, data.data(), MEMPAK_PAGE_SIZE);
        m_mempaks[controller_index].is_dirty = true;
        return true;
    }

    bool MemoryPakController::format(int controller_index) {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return false;
        
        // Zero out memory
        m_mempaks[controller_index].memory.fill(0);
        
        // Setup basic header (Page 0) - This is a rough approximation of a formatted pak
        // Real N64 Mempak format involves ID area, Label area, Inode table.
        // For standard emulator compatibility, 0x00 or 0xFF fill is often sufficient, 
        // but creating a valid filesystem structure allows games to recognize it immediately.
        
        // NOTE: A full filesystem implementation is complex. 
        // We will stick to zero-filled for now as many emulators do, 
        // letting games format it if they detect it's invalid.
        // However, we mark it as dirty.
        
        m_mempaks[controller_index].is_dirty = true;
        m_mempaks[controller_index].status.formatted = true;
        m_mempaks[controller_index].status.free_pages = MEMPAK_NUM_PAGES - 5; // 5 reserved pages

        // Minimal Header Logic (Optional):
        // 0-32 bytes: Label
        // ...
        
        return true;
    }

    // Storage I/O
    bool MemoryPakController::load_from_disk(int controller_index) {
        auto& mp = m_mempaks[controller_index];
        std::ifstream file(mp.file_path, std::ios::binary);
        if (!file.is_open()) return false;

        file.read(reinterpret_cast<char*>(mp.memory.data()), MEMPAK_TOTAL_SIZE);
        if (file.gcount() != MEMPAK_TOTAL_SIZE) {
            // File might be smaller or empty, handle gracefully?
            // For now, if read fails partial, we keep what we have (rest 0)
        }
        
        // Check if actually formatted (heuristic)
        // If all zeros, might be unformatted? 
        // We assume loaded successfully.
        
        mp.is_dirty = false;
        return true;
    }

    bool MemoryPakController::save_to_disk(int controller_index) {
        auto& mp = m_mempaks[controller_index];
        std::ofstream file(mp.file_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;

        file.write(reinterpret_cast<const char*>(mp.memory.data()), MEMPAK_TOTAL_SIZE);
        mp.is_dirty = false;
        return true;
    }

    // Save File Management
    // The N64 memory pak uses a custom filesystem with a note table (iNode) at pages 3-4.
    // Each "note" (save file) has a 32-byte entry containing:
    //   - Game code (4 bytes)
    //   - Publisher code (2 bytes)  
    //   - Start page (2 bytes)
    //   - Status (1 byte)
    //   - Reserved (3 bytes)
    //   - File name (16 bytes, Shift-JIS encoded)
    //   - Extension (4 bytes)
    // The iNode table (pages 5-6) tracks page allocation chains.
    
    bool MemoryPakController::load_save_file(int controller_index, const std::string& game_id, const std::string& save_name) {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return false;
        auto& mp = m_mempaks[controller_index];
        if (!mp.is_initialized) return false;
        
        // Search through note table for matching game_id
        // Note table starts at offset 0x300 (page 3, 32 bytes each, up to 16 entries)
        constexpr uint32_t NOTE_TABLE_OFFSET = 0x300;
        constexpr uint32_t NOTE_ENTRY_SIZE = 32;
        constexpr uint32_t MAX_NOTES = 16;
        
        for (uint32_t i = 0; i < MAX_NOTES; i++) {
            uint32_t offset = NOTE_TABLE_OFFSET + (i * NOTE_ENTRY_SIZE);
            
            // Read game code (first 4 bytes)
            char gameCode[5] = {};
            std::memcpy(gameCode, &mp.memory[offset], 4);
            
            if (game_id == gameCode) {
                // Found matching entry
                return true;
            }
        }
        
        return false;
    }

    bool MemoryPakController::save_to_file(int controller_index, const std::string& game_id, const std::string& save_name) {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return false;
        auto& mp = m_mempaks[controller_index];
        if (!mp.is_initialized) return false;
        
        // Flush the entire mempak to disk - the N64 filesystem is stored inline
        return save_to_disk(controller_index);
    }

    std::vector<SaveFileInfo> MemoryPakController::list_save_files(int controller_index) const {
        std::vector<SaveFileInfo> result;
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return result;
        
        const auto& mp = m_mempaks[controller_index];
        if (!mp.is_initialized || !mp.status.formatted) return result;
        
        // Parse note table at offset 0x300
        constexpr uint32_t NOTE_TABLE_OFFSET = 0x300;
        constexpr uint32_t NOTE_ENTRY_SIZE = 32;
        constexpr uint32_t MAX_NOTES = 16;
        
        for (uint32_t i = 0; i < MAX_NOTES; i++) {
            uint32_t offset = NOTE_TABLE_OFFSET + (i * NOTE_ENTRY_SIZE);
            
            // Check if entry is valid (game code is non-zero)
            if (mp.memory[offset] == 0 && mp.memory[offset + 1] == 0 &&
                mp.memory[offset + 2] == 0 && mp.memory[offset + 3] == 0) {
                continue;
            }
            
            SaveFileInfo info;
            // Game code (bytes 0-3)
            char gameCode[5] = {};
            std::memcpy(gameCode, &mp.memory[offset], 4);
            info.game_name = gameCode;
            
            // Start page (bytes 6-7, big-endian)
            info.start_page = (mp.memory[offset + 6] << 8) | mp.memory[offset + 7];
            
            // Status byte (byte 8) 
            info.valid = (mp.memory[offset + 8] == 0x02); // 0x02 = occupied
            
            // File name (bytes 16-31, up to 16 chars)
            char fileName[17] = {};
            std::memcpy(fileName, &mp.memory[offset + 16], 16);
            info.file_name = fileName;
            
            // Count pages by following iNode chain
            // iNode table starts at page 5 (offset 0x500), each entry is 2 bytes
            constexpr uint32_t INODE_TABLE_OFFSET = 0x500;
            info.page_count = 0;
            uint16_t currentPage = static_cast<uint16_t>(info.start_page);
            while (currentPage >= 5 && currentPage < MEMPAK_NUM_PAGES && info.page_count < MEMPAK_NUM_PAGES) {
                info.page_count++;
                uint16_t nextPage = (mp.memory[INODE_TABLE_OFFSET + currentPage * 2] << 8) | 
                                     mp.memory[INODE_TABLE_OFFSET + currentPage * 2 + 1];
                if (nextPage == 0x0001 || nextPage == 0x0003) break; // End of chain
                currentPage = nextPage;
            }
            
            info.corrupted = false;
            info.checksum = 0;
            info.timestamp = 0;
            
            result.push_back(std::move(info));
        }
        
        return result;
    }

    bool MemoryPakController::delete_save_file(int controller_index, const std::string& game_id, const std::string& save_name) {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return false;
        auto& mp = m_mempaks[controller_index];
        if (!mp.is_initialized || !mp.status.formatted) return false;
        
        constexpr uint32_t NOTE_TABLE_OFFSET = 0x300;
        constexpr uint32_t NOTE_ENTRY_SIZE = 32;
        constexpr uint32_t MAX_NOTES = 16;
        constexpr uint32_t INODE_TABLE_OFFSET = 0x500;
        
        for (uint32_t i = 0; i < MAX_NOTES; i++) {
            uint32_t offset = NOTE_TABLE_OFFSET + (i * NOTE_ENTRY_SIZE);
            
            char gameCode[5] = {};
            std::memcpy(gameCode, &mp.memory[offset], 4);
            
            if (game_id == gameCode) {
                // Get start page and free the iNode chain
                uint16_t startPage = (mp.memory[offset + 6] << 8) | mp.memory[offset + 7];
                uint16_t currentPage = startPage;
                
                // Follow and clear the iNode chain
                while (currentPage >= 5 && currentPage < MEMPAK_NUM_PAGES) {
                    size_t inodeOff = INODE_TABLE_OFFSET + currentPage * 2;
                    uint16_t nextPage = (mp.memory[inodeOff] << 8) | mp.memory[inodeOff + 1];
                    // Mark page as free
                    mp.memory[inodeOff] = 0x00;
                    mp.memory[inodeOff + 1] = 0x03; // 0x0003 = free page
                    mp.status.free_pages++;
                    if (nextPage == 0x0001 || nextPage == 0x0003) break;
                    currentPage = nextPage;
                }
                
                // Clear the note entry
                std::memset(&mp.memory[offset], 0, NOTE_ENTRY_SIZE);
                mp.is_dirty = true;
                
                return true;
            }
        }
        
        return false;
    }

    bool MemoryPakController::verify_save_file(int controller_index, const std::string& game_id, const std::string& save_name) const {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return false;
        const auto& mp = m_mempaks[controller_index];
        if (!mp.is_initialized || !mp.status.formatted) return false;
        
        constexpr uint32_t NOTE_TABLE_OFFSET = 0x300;
        constexpr uint32_t NOTE_ENTRY_SIZE = 32;
        constexpr uint32_t MAX_NOTES = 16;
        constexpr uint32_t INODE_TABLE_OFFSET = 0x500;
        
        for (uint32_t i = 0; i < MAX_NOTES; i++) {
            uint32_t offset = NOTE_TABLE_OFFSET + (i * NOTE_ENTRY_SIZE);
            
            char gameCode[5] = {};
            std::memcpy(gameCode, &mp.memory[offset], 4);
            
            if (game_id == gameCode) {
                // Verify the iNode chain is valid (no loops, in-range pages)
                uint16_t startPage = (mp.memory[offset + 6] << 8) | mp.memory[offset + 7];
                uint16_t currentPage = startPage;
                uint32_t pageCount = 0;
                
                while (currentPage >= 5 && currentPage < MEMPAK_NUM_PAGES) {
                    pageCount++;
                    if (pageCount > MEMPAK_NUM_PAGES) return false; // Loop detected
                    
                    size_t inodeOff = INODE_TABLE_OFFSET + currentPage * 2;
                    uint16_t nextPage = (mp.memory[inodeOff] << 8) | mp.memory[inodeOff + 1];
                    if (nextPage == 0x0001) break; // Valid end of chain
                    if (nextPage == 0x0003) return false; // Hit a free page
                    currentPage = nextPage;
                }
                
                return pageCount > 0;
            }
        }
        
        return false;
    }

    // Status
    MempakStatus MemoryPakController::get_status(int controller_index) const {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return {};
        return m_mempaks[controller_index].status;
    }

    bool MemoryPakController::is_connected(int controller_index) const {
        // In this emulation, we assume paks are always "connectable" if initialized
        return controller_index >= 0 && controller_index < MAX_CONTROLLERS;
    }

    bool MemoryPakController::is_formatted(int controller_index) const {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return false;
        return m_mempaks[controller_index].status.formatted;
    }

    size_t MemoryPakController::get_free_space(int controller_index) const {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return 0;
        return m_mempaks[controller_index].status.free_pages * MEMPAK_PAGE_SIZE;
    }

    // Static CRC
    uint8_t MemoryPakController::calculate_crc(const uint8_t* data, size_t length) {
        return MempakUtils::crc8(data, length);
    }
    
    uint32_t MemoryPakController::calculate_crc32(const uint8_t* data, size_t length) {
        return MempakUtils::crc32(data, length);
    }

    // Private helpers
    bool MemoryPakController::is_valid_address(int controller_index, uint32_t address, size_t length) const {
        return address + length <= MEMPAK_TOTAL_SIZE;
    }

    bool MemoryPakController::is_valid_page(int controller_index, uint32_t page) const {
        return page < MEMPAK_NUM_PAGES;
    }

    bool MemoryPakController::parse_save_files(int controller_index) { return true; }
    bool MemoryPakController::update_save_file_table(int controller_index) { return true; }
    uint32_t MemoryPakController::find_free_pages(int controller_index, uint32_t required_pages) const { return 0; }
    bool MemoryPakController::allocate_save_file(int controller_index, const SaveFileInfo& file_info) { return false; }
    
    uint8_t* MemoryPakController::get_memory_pointer(int controller_index, uint32_t address) {
        return m_mempaks[controller_index].memory.data() + address;
    }
    const uint8_t* MemoryPakController::get_memory_pointer(int controller_index, uint32_t address) const {
        return m_mempaks[controller_index].memory.data() + address;
    }

    // ==========================================================
    // CMempak (Legacy) Implementation
    // ==========================================================
    
    CMempak::CMempak() {
        m_controller = std::make_unique<MemoryPakController>();
        // Initialize memory arrays to 0
        for (auto& arr : m_mempak_memory) {
            arr.fill(0);
        }
        m_formatted.fill(false);
        m_save_exists.fill(false);
    }

    bool CMempak::Initialize(const std::string& base_path) {
        bool result = true;
        // Initialize all controllers
        for (int i = 0; i < MAX_CONTROLLERS; ++i) {
            if (!m_controller->initialize(i, base_path)) {
                // If init fails, we just continue, maybe that controller has no pak
                // result = false; 
            }
            
            // Sync legacy memory
            // This is inefficient but necessary if we maintain two memory structures
            // Ideally, remove m_mempak_memory and use m_controller->read()
            auto page0 = m_controller->read(i, 0, MEMPAK_TOTAL_SIZE);
            if (page0.size() == MEMPAK_TOTAL_SIZE) {
                std::memcpy(m_mempak_memory[i].data(), page0.data(), MEMPAK_TOTAL_SIZE);
            }
        }
        return result;
    }

    void CMempak::Cleanup() {
        m_controller->shutdown();
    }

    void CMempak::ReadFrom(int32_t controller_index, uint32_t address, uint8_t* data) {
         // Legacy reads 32 bytes usually? The signature is uint8_t* data, but size is unknown.
         // Assuming Joybus 32-byte chunks or single byte? 
         // Most N64 emulators use 32 byte block reads.
         // However, the function signature `ReadFrom(..., uint8_t* data)` is ambiguous without length.
         // Standard Joybus read is 32 bytes (0x20).
         
         // Let's assume the callers know what they are doing and standard 32 bytes.
         // OR, check if there's a convention.
         
         // Access the controller
         std::vector<uint8_t> read_data = m_controller->read(controller_index, address, 32);
         if (!read_data.empty()) {
             std::memcpy(data, read_data.data(), read_data.size());
             
             // Also update legacy mirror
             std::memcpy(m_mempak_memory[controller_index].data() + address, read_data.data(), read_data.size());
         }
    }

    void CMempak::WriteTo(int32_t controller_index, uint32_t address, uint8_t* data) {
        // Assume 32 bytes
        std::vector<uint8_t> vec_data(data, data + 32);
        m_controller->write(controller_index, address, vec_data);
        
        // Update legacy mirror
        std::memcpy(m_mempak_memory[controller_index].data() + address, data, 32);
    }

    uint8_t CMempak::CalculateCrc(uint8_t* data_to_crc) {
        return MempakUtils::crc8(data_to_crc, 32); 
    }

    bool CMempak::IsFormatted(int32_t controller_index) const {
        return m_controller->is_formatted(controller_index);
    }

    void CMempak::Format(int32_t controller_index) {
        m_controller->format(controller_index);
        m_mempak_memory[controller_index].fill(0);
    }
    
    // Internal legacy methods - delegates to MemoryPakController
    void CMempak::LoadMempak(int32_t controller_index, bool create_if_missing) {
        if (m_controller && controller_index >= 0 && controller_index < MAX_CONTROLLERS) {
            m_controller->initialize(controller_index, "");
            if (create_if_missing && !m_controller->is_formatted(controller_index)) {
                m_controller->format(controller_index);
            }
            // Sync to legacy memory
            auto data = m_controller->read(controller_index, 0, MEMPAK_TOTAL_SIZE);
            if (data.size() == MEMPAK_TOTAL_SIZE) {
                std::memcpy(m_mempak_memory[controller_index].data(), data.data(), MEMPAK_TOTAL_SIZE);
            }
            m_formatted[controller_index] = m_controller->is_formatted(controller_index);
            m_save_exists[controller_index] = true;
        }
    }
    
    std::string CMempak::get_mempak_filename(int32_t controller_index) const {
        // Return the expected filename for this controller's mempak
        return std::string("Mempak") + std::to_string(controller_index) + ".mpk";
    }
    
    bool CMempak::load_legacy_mempak(int32_t controller_index) {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return false;
        std::string path = get_mempak_filename(controller_index);
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;
        
        file.read(reinterpret_cast<char*>(m_mempak_memory[controller_index].data()), MEMPAK_TOTAL_SIZE);
        m_save_exists[controller_index] = file.good();
        m_formatted[controller_index] = true;
        return file.good();
    }
    
    bool CMempak::save_legacy_mempak(int32_t controller_index) {
        if (controller_index < 0 || controller_index >= MAX_CONTROLLERS) return false;
        std::string path = get_mempak_filename(controller_index);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        
        file.write(reinterpret_cast<const char*>(m_mempak_memory[controller_index].data()), MEMPAK_TOTAL_SIZE);
        return file.good();
    }


    // ==========================================================
    // MemoryPakFactory Implementation
    // ==========================================================

    std::unique_ptr<MemoryPakController> MemoryPakFactory::create_controller() {
        return std::make_unique<MemoryPakController>();
    }

    std::unique_ptr<CMempak> MemoryPakFactory::create_legacy_mempak() {
        return std::make_unique<CMempak>();
    }

    bool MemoryPakFactory::detect_mempak_type(const uint8_t* data, size_t size) {
        return size == MEMPAK_TOTAL_SIZE;
    }

    std::vector<uint8_t> MemoryPakFactory::create_formatted_mempak() {
        std::vector<uint8_t> data(MEMPAK_TOTAL_SIZE, 0);
        
        // Initialize N64 mempak filesystem header
        // Page 0: ID area - repeated 4 times (32 bytes each = 128 bytes per repetition)
        // Contains manufacturer ID, device ID, etc.
        for (int rep = 0; rep < 4; rep++) {
            uint32_t base = rep * 32;
            data[base + 0] = 0x81; // Manufacturer (Nintendo)
            data[base + 1] = 0x01; // Device ID (Memory Pak)
        }
        
        // Pages 1-2: iNode table backup (initialized to free)
        // Pages 3-4: Note table (16 entries, 32 bytes each, initialized to 0)
        // Pages 5+: iNode table (2 bytes per page entry)
        // Mark all data pages as free (0x0003)
        constexpr uint32_t INODE_TABLE_OFFSET = 0x500; // Page 5
        for (uint32_t page = 5; page < MEMPAK_NUM_PAGES; page++) {
            data[INODE_TABLE_OFFSET + page * 2] = 0x00;
            data[INODE_TABLE_OFFSET + page * 2 + 1] = 0x03; // Free page marker
        }
        
        return data;
    }

    std::vector<uint8_t> MemoryPakFactory::create_mempak_from_save(const std::string& save_file_path) {
        // Load raw mempak data from a .mpk file
        std::ifstream file(save_file_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return create_formatted_mempak();
        }
        
        auto size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        if (static_cast<size_t>(size) != MEMPAK_TOTAL_SIZE) {
            // Wrong size - return a formatted empty pak
            return create_formatted_mempak();
        }
        
        std::vector<uint8_t> data(MEMPAK_TOTAL_SIZE);
        file.read(reinterpret_cast<char*>(data.data()), MEMPAK_TOTAL_SIZE);
        
        if (!file.good()) {
            return create_formatted_mempak();
        }
        
        return data;
    }

    std::vector<SaveFileInfo> MemoryPakFactory::extract_save_files(const uint8_t* mempak_data, size_t size) {
        return {};
    }

} // namespace N64Controller

namespace GlobalMemoryPak {
    N64Controller::CMempak g_legacy_mempak;
    static std::unique_ptr<N64Controller::MemoryPakController> g_controller;

    N64Controller::MemoryPakController& get_controller() {
        if (!g_controller) {
            g_controller = std::make_unique<N64Controller::MemoryPakController>();
        }
        return *g_controller;
    }

    bool initialize(const std::string& save_directory) {
        bool res = get_controller().initialize(0, save_directory);
        res &= get_controller().initialize(1, save_directory);
        res &= get_controller().initialize(2, save_directory);
        res &= get_controller().initialize(3, save_directory);
        
        g_legacy_mempak.Initialize(save_directory);
        return res;
    }

    void shutdown() {
        if (g_controller) {
            g_controller->shutdown();
        }
        g_legacy_mempak.Cleanup();
    }
}
