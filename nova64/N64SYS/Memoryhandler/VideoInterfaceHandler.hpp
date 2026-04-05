/**
 * @file VideoInterfaceHandler.hpp
 * @brief Video Interface Handler declarations and types.
 */
#pragma once
#include "Memoryhandler.hpp"
#include "../../common/log.hpp"
#include "../../common/Span.hpp"
#include <cstdint>
#include <array>
#include <string>
#include <memory>
#include <functional>


// Forward declarations
namespace Nova64 { class MipsMemoryVM; }
class CPlugins;
class CRegisters;
namespace N64System { class SystemTimer; }
class CN64System;

namespace VI {
    // Video modes
    enum class ColorDepth : uint32_t {
        PIXEL_SIZE_16BIT = 0,  // 16-bit RGB (5-5-5-1)
        PIXEL_SIZE_32BIT = 1   // 32-bit RGBA (8-8-8-8)
    };
    
    enum class GammaDither : uint32_t {
        DISABLE = 0,
        ENABLE = 1
    };
    
    enum class Gamma : uint32_t {
        DISABLE = 0,
        ENABLE = 1
    };
    
    enum class Divot : uint32_t {
        DISABLE = 0,
        ENABLE = 1
    };
    
    enum class Serrate : uint32_t {
        DISABLE = 0,  // Progressive
        ENABLE = 1    // Interlaced
    };
    
    enum class AA_Mode : uint32_t {
        AA_RESAMPLE_ALWAYS = 0,
        AA_RESAMPLE_FETCH_NEEDED = 1,
        AA_RESAMPLE_ONLY = 2,
        AA_RESAMPLE_REPLICATE = 3
    };
    
    // Register offsets (from 0x04400000)
    enum class Register : uint32_t {
        STATUS = 0x00,
        CONTROL = 0x00,  // Alias for STATUS
        ORIGIN = 0x04,
        DRAM_ADDR = 0x04,  // Alias for ORIGIN
        WIDTH = 0x08,
        H_WIDTH = 0x08,  // Alias for WIDTH
        INTR = 0x0C,
        V_INTR = 0x0C,  // Alias for INTR
        CURRENT = 0x10,
        V_CURRENT_LINE = 0x10,  // Alias for CURRENT
        BURST = 0x14,
        TIMING = 0x18,
        V_SYNC = 0x1C,
        H_SYNC = 0x20,
        LEAP = 0x24,
        H_SYNC_LEAP = 0x28,
        H_START = 0x2C,
        H_VIDEO = 0x30,
        V_START = 0x34,
        V_VIDEO = 0x38,
        V_BURST = 0x3C,
        X_SCALE = 0x40,
        Y_SCALE = 0x44,
        REGISTER_COUNT = 22
    };
    
    // Video timing constants
    struct Timing {
        static constexpr uint32_t NTSC_WIDTH = 640;
        static constexpr uint32_t NTSC_HEIGHT = 480;
        static constexpr uint32_t NTSC_VBLANK_START = 480;
        static constexpr uint32_t NTSC_VBLANK_END = 525;
        static constexpr uint32_t NTSC_HBLANK_START = 640;
        static constexpr uint32_t NTSC_HBLANK_END = 720;
        
        static constexpr uint32_t PAL_WIDTH = 640;
        static constexpr uint32_t PAL_HEIGHT = 576;
        static constexpr uint32_t PAL_VBLANK_START = 576;
        static constexpr uint32_t PAL_VBLANK_END = 625;
        static constexpr uint32_t PAL_HBLANK_START = 640;
        static constexpr uint32_t PAL_HBLANK_END = 720;
    };
    
    // Helper functions
    constexpr ColorDepth get_color_depth(uint32_t status) {
        return static_cast<ColorDepth>((status >> 0) & 0x3);
    }
    
    constexpr GammaDither get_gamma_dither(uint32_t status) {
        return static_cast<GammaDither>((status >> 2) & 0x1);
    }
    
    constexpr Gamma get_gamma(uint32_t status) {
        return static_cast<Gamma>((status >> 3) & 0x1);
    }
    
    constexpr Divot get_divot(uint32_t status) {
        return static_cast<Divot>((status >> 4) & 0x1);
    }
    
    constexpr Serrate get_serrate(uint32_t status) {
        return static_cast<Serrate>((status >> 6) & 0x1);
    }
    
    constexpr AA_Mode get_aa_mode(uint32_t status) {
        return static_cast<AA_Mode>((status >> 8) & 0x3);
    }
}

// Modern register interface
class VideoInterfaceReg {
public:
    // Accessor methods
    uint32_t& status() { return m_registers[static_cast<size_t>(VI::Register::STATUS) >> 2]; }
    uint32_t& origin() { return m_registers[static_cast<size_t>(VI::Register::ORIGIN) >> 2]; }
    uint32_t& width() { return m_registers[static_cast<size_t>(VI::Register::WIDTH) >> 2]; }
    uint32_t& v_intr() { return m_registers[static_cast<size_t>(VI::Register::INTR) >> 2]; }
    uint32_t& current() { return m_registers[static_cast<size_t>(VI::Register::CURRENT) >> 2]; }
    uint32_t& burst() { return m_registers[static_cast<size_t>(VI::Register::BURST) >> 2]; }
    uint32_t& v_sync() { return m_registers[static_cast<size_t>(VI::Register::V_SYNC) >> 2]; }
    uint32_t& h_sync() { return m_registers[static_cast<size_t>(VI::Register::H_SYNC) >> 2]; }
    uint32_t& leap() { return m_registers[static_cast<size_t>(VI::Register::LEAP) >> 2]; }
    uint32_t& h_sync_leap() { return m_registers[static_cast<size_t>(VI::Register::H_SYNC_LEAP) >> 2]; }
    uint32_t& h_start() { return m_registers[static_cast<size_t>(VI::Register::H_START) >> 2]; }
    uint32_t& h_video() { return m_registers[static_cast<size_t>(VI::Register::H_VIDEO) >> 2]; }
    uint32_t& v_start() { return m_registers[static_cast<size_t>(VI::Register::V_START) >> 2]; }
    uint32_t& v_video() { return m_registers[static_cast<size_t>(VI::Register::V_VIDEO) >> 2]; }
    uint32_t& v_burst() { return m_registers[static_cast<size_t>(VI::Register::V_BURST) >> 2]; }
    uint32_t& x_scale() { return m_registers[static_cast<size_t>(VI::Register::X_SCALE) >> 2]; }
    uint32_t& y_scale() { return m_registers[static_cast<size_t>(VI::Register::Y_SCALE) >> 2]; }
    
    // Const accessors
    uint32_t status() const { return m_registers[static_cast<size_t>(VI::Register::STATUS) >> 2]; }
    uint32_t origin() const { return m_registers[static_cast<size_t>(VI::Register::ORIGIN) >> 2]; }
    uint32_t width() const { return m_registers[static_cast<size_t>(VI::Register::WIDTH) >> 2]; }
    uint32_t v_intr() const { return m_registers[static_cast<size_t>(VI::Register::INTR) >> 2]; }
    uint32_t current() const { return m_registers[static_cast<size_t>(VI::Register::CURRENT) >> 2]; }
    uint32_t burst() const { return m_registers[static_cast<size_t>(VI::Register::BURST) >> 2]; }
    uint32_t v_sync() const { return m_registers[static_cast<size_t>(VI::Register::V_SYNC) >> 2]; }
    uint32_t h_sync() const { return m_registers[static_cast<size_t>(VI::Register::H_SYNC) >> 2]; }
    uint32_t leap() const { return m_registers[static_cast<size_t>(VI::Register::LEAP) >> 2]; }
    uint32_t h_sync_leap() const { return m_registers[static_cast<size_t>(VI::Register::H_SYNC_LEAP) >> 2]; }
    uint32_t h_start() const { return m_registers[static_cast<size_t>(VI::Register::H_START) >> 2]; }
    uint32_t h_video() const { return m_registers[static_cast<size_t>(VI::Register::H_VIDEO) >> 2]; }
    uint32_t v_start() const { return m_registers[static_cast<size_t>(VI::Register::V_START) >> 2]; }
    uint32_t v_video() const { return m_registers[static_cast<size_t>(VI::Register::V_VIDEO) >> 2]; }
    uint32_t v_burst() const { return m_registers[static_cast<size_t>(VI::Register::V_BURST) >> 2]; }
    uint32_t x_scale() const { return m_registers[static_cast<size_t>(VI::Register::X_SCALE) >> 2]; }
    uint32_t y_scale() const { return m_registers[static_cast<size_t>(VI::Register::Y_SCALE) >> 2]; }
    
    // Helper methods
    VI::ColorDepth get_color_depth() const {
        return VI::get_color_depth(status());
    }
    
    VI::GammaDither get_gamma_dither() const {
        return VI::get_gamma_dither(status());
    }
    
    VI::Gamma get_gamma() const {
        return VI::get_gamma(status());
    }
    
    VI::Divot get_divot() const {
        return VI::get_divot(status());
    }
    
    VI::Serrate get_serrate() const {
        return VI::get_serrate(status());
    }
    
    VI::AA_Mode get_aa_mode() const {
        return VI::get_aa_mode(status());
    }
    
    uint32_t get_framebuffer_address() const {
        return origin() & 0xFFFFFF;  // 24-bit address
    }
    
    uint32_t get_framebuffer_width() const {
        return width() & 0xFFF;  // 12-bit width
    }
    
    float get_x_scale_factor() const {
        return static_cast<float>(x_scale() & 0xFFF) / 1024.0f;
    }
    
    float get_y_scale_factor() const {
        return static_cast<float>(y_scale() & 0xFFF) / 1024.0f;
    }
    
    uint32_t get_active_width() const {
        uint32_t h_video_val = h_video() & 0xFFF;
        uint32_t h_start_val = h_start() & 0xFFF;
        return (h_video_val - h_start_val) / 2;  // In pixels
    }
    
    uint32_t get_active_height() const {
        uint32_t v_video_val = v_video() & 0xFFF;
        uint32_t v_start_val = v_start() & 0xFFF;
        return (v_video_val - v_start_val) / 2;  // In scanlines
    }

    explicit VideoInterfaceReg(Common::Span<uint32_t, static_cast<size_t>(VI::Register::REGISTER_COUNT)> registers)
        : m_registers(registers) {}

protected:

private:
    Common::Span<uint32_t, static_cast<size_t>(VI::Register::REGISTER_COUNT)> m_registers;
    
    // Prevent copying
    VideoInterfaceReg() = delete;
    VideoInterfaceReg(const VideoInterfaceReg&) = delete;
    VideoInterfaceReg& operator=(const VideoInterfaceReg&) = delete;
};

// Main handler class
class VideoInterfaceHandler final : public MemoryHandler {
public:
    struct Config {
        bool enable_interlacing = true;
        bool enable_vi_interrupt = true;
        bool enable_frame_limiter = true;
        bool enable_vsync = true;
        bool enable_debug_overlay = false;
        bool enable_statistics = true;
        uint32_t target_fps = 60;
        std::string video_plugin_name;
    };
    
    struct VideoMode {
        uint32_t width;
        uint32_t height;
        VI::ColorDepth color_depth;
        bool interlaced;
        float x_scale;
        float y_scale;
        
        std::string to_string() const {
            return std::to_string(width) + "x" + std::to_string(height) + 
                   (color_depth == VI::ColorDepth::PIXEL_SIZE_16BIT ? " 16-bit" : " 32-bit") +
                   (interlaced ? " interlaced" : " progressive");
        }
    };
    
    explicit VideoInterfaceHandler(
        CN64System& system,
        Nova64::MipsMemoryVM& mmu,
        CRegisters& registers,
        const Config& config = {}
    );
    
    ~VideoInterfaceHandler() override;
    
    // MemoryHandler interface
    [[nodiscard]] bool Read32(uint32_t address, uint32_t& value) override;
    [[nodiscard]] bool Write32(uint32_t address, uint32_t value, uint32_t mask) override;
    
    // Video control
    void update_field_serration(uint32_t interlaced);
    void update_half_line();
    void update_scanline();
    void generate_vi_interrupt();
    void on_frame_timer();
    
    // State management
    void reset();
    void save_state(std::ostream& os) const;
    void load_state(std::istream& is);
    
    // Video information
    [[nodiscard]] VideoMode get_current_video_mode() const;
    [[nodiscard]] std::string get_video_info() const;
    [[nodiscard]] std::string get_register_dump() const;
    [[nodiscard]] uint32_t get_current_scanline() const;
    [[nodiscard]] uint32_t get_framebuffer_address() const;
    [[nodiscard]] uint32_t get_framebuffer_width() const;

    /// Get a mutable pointer to a specific VI register (for plugin wiring)
    [[nodiscard]] uint32_t* GetRegisterPtr(VI::Register reg) noexcept {
        return &m_registers[static_cast<size_t>(reg) >> 2];
    }
    
    // Frame timing
    void begin_frame();
    void end_frame();
    void wait_for_vblank();
    [[nodiscard]] bool is_vblank_active() const;
    [[nodiscard]] bool is_hblank_active() const;
    
    // Statistics
    struct Statistics {
        uint64_t total_frames = 0;
        uint64_t vi_interrupts = 0;
        uint64_t framebuffer_changes = 0;
        uint64_t mode_changes = 0;
        uint64_t vsync_misses = 0;
        
        // Timing
        uint64_t frame_time_us = 0;
        uint64_t max_frame_time_us = 0;
        uint64_t min_frame_time_us = UINT64_MAX;
        
        // Performance
        double average_fps = 0.0;
        double current_fps = 0.0;
        double frame_time_variance = 0.0;
    };
    
    [[nodiscard]] const Statistics& get_statistics() const { return m_stats; }
    void reset_statistics() { m_stats = {}; }
    
    // Callbacks
    using FrameCallback = std::function<void(const VideoMode& mode, uint32_t fb_address)>;
    using InterruptCallback = std::function<void()>;
    using ModeChangeCallback = std::function<void(const VideoMode& old_mode, const VideoMode& new_mode)>;
    
    void set_frame_callback(FrameCallback callback);
    void set_interrupt_callback(InterruptCallback callback);
    void set_mode_change_callback(ModeChangeCallback callback);

private:
    // Delete copy and move operations
    VideoInterfaceHandler() = delete;
    VideoInterfaceHandler(const VideoInterfaceHandler&) = delete;
    VideoInterfaceHandler(VideoInterfaceHandler&&) = delete;
    VideoInterfaceHandler& operator=(const VideoInterfaceHandler&) = delete;
    VideoInterfaceHandler& operator=(VideoInterfaceHandler&&) = delete;
    
    
    CN64System& m_system;
    Nova64::MipsMemoryVM& m_mmu;
    CRegisters& m_sys_regs;
    N64System::SystemTimer& m_system_timer;
    Config m_config;
    
    // VI registers
    std::array<uint32_t, static_cast<size_t>(VI::Register::REGISTER_COUNT)> m_registers{};
    VideoInterfaceReg m_vi_regs{m_registers};
    
    // Video state
    struct VideoState {
        uint32_t field_serration = 0;  // Interlace field
        uint32_t half_line = 0;        // Half-line for interlacing
        uint32_t half_line_check = 0;
        uint32_t current_scanline = 0;
        uint32_t current_field = 0;    // 0=even field, 1=odd field (for interlacing)
        bool vblank_active = false;
        bool hblank_active = false;
        uint64_t frame_start_time = 0;
        uint64_t frame_number = 0;
        
        VideoMode current_mode;
        VideoMode previous_mode;
    } m_video_state;
    
    // Plugins
    CPlugins* m_plugins;
    
    // Statistics
    Statistics m_stats;
    uint64_t m_last_stat_update = 0;
    
    // Callbacks
    FrameCallback m_frame_callback;
    InterruptCallback m_interrupt_callback;
    ModeChangeCallback m_mode_change_callback;
    
    // Internal methods
    void handle_status_register_write(uint32_t value, uint32_t mask);
    void handle_origin_register_write(uint32_t value, uint32_t mask);
    void handle_v_intr_register_write(uint32_t value, uint32_t mask);
    
    // Timing calculations
    [[nodiscard]] uint32_t calculate_scanline_time() const;
    [[nodiscard]] uint32_t calculate_vblank_start() const;
    [[nodiscard]] uint32_t calculate_vblank_end() const;
    [[nodiscard]] bool is_ntsc_mode() const;
    [[nodiscard]] bool is_pal_mode() const;
    
    // Framebuffer operations
    [[nodiscard]] bool validate_framebuffer_address(uint32_t address) const;
    void notify_framebuffer_change(uint32_t new_address);
    
    // Mode detection
    void detect_video_mode_change();
    void apply_video_mode(const VideoMode& mode);
    
    // Interrupt handling
    void check_vi_interrupt();
    void clear_vi_interrupt();
    
    // Timing control
    void update_frame_timing();
    void limit_frame_rate();
    
    // Logging
    void log_video_mode_change(const VideoMode& old_mode, const VideoMode& new_mode) const;
    void log_register_access(uint32_t address, uint32_t value, bool is_write) const;
    void log_frame_statistics() const;
    
    // System event handlers
    void on_system_reset();
    void on_game_state_loaded();
};
