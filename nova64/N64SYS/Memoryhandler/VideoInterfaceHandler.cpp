#include "VideoInterfaceHandler.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Systiming.hpp"
#include "../../common/log.hpp"
#include "../Plugins/PluginManager.hpp"
#include "../MIPSY/Vmem.hpp" 

#include <cmath>
#include <iomanip>
#include <sstream>

// ===================================
// Helper Macros / Constants
// ===================================

#define VI_STATUS_REG       0x00
#define VI_ORIGIN_REG       0x04
#define VI_WIDTH_REG        0x08
#define VI_INTR_REG         0x0C
#define VI_CURRENT_REG      0x10
#define VI_BURST_REG        0x14
#define VI_V_SYNC_REG       0x18
#define VI_H_SYNC_REG       0x1C
#define VI_LEAP_REG         0x20
#define VI_H_START_REG      0x24
#define VI_V_START_REG      0x28
#define VI_V_BURST_REG      0x2C
#define VI_X_SCALE_REG      0x30
#define VI_Y_SCALE_REG      0x34

// ===================================
// Implementation
// ===================================

VideoInterfaceHandler::VideoInterfaceHandler(
    CN64System& system,
    Nova64::MipsMemoryVM& mmu,
    CRegisters& registers,
    const Config& config
) : 
    m_system(system),
    m_mmu(mmu),
    m_sys_regs(registers),
    m_system_timer(*system.GetSystemTimer()), 
    m_config(config),
    m_plugins(nullptr) // Initialize plugins later or via system
{
    reset();
}

VideoInterfaceHandler::~VideoInterfaceHandler() = default;

void VideoInterfaceHandler::reset() {
    m_registers.fill(0);
    // Set some defaults
    m_vi_regs.status() = 0;
    m_vi_regs.origin() = 0;
    m_vi_regs.width() = 320;
    m_vi_regs.v_intr() = 0x3FF; // Default interrupt line
    m_vi_regs.v_sync() = 0x20C; // NTSC default roughly
    m_vi_regs.h_sync() = 0; 
    
    // Reset state
    m_video_state = {};
    m_stats = {};
    
    Log::info("Video Interface Reset");
}

bool VideoInterfaceHandler::Read32(uint32_t address, uint32_t& value) {
    uint32_t offset = address & 0x3F; // Mask to 0-0x3F range
    
    switch (offset) {
        case VI_STATUS_REG:   value = m_vi_regs.status(); break;
        case VI_ORIGIN_REG:   value = m_vi_regs.origin(); break;
        case VI_WIDTH_REG:    value = m_vi_regs.width(); break;
        case VI_INTR_REG:     value = m_vi_regs.v_intr(); break;
        
        case VI_CURRENT_REG: {
            // Dynamically compute VI_CURRENT from the live NextTimer countdown.
            // m_NextTimer decrements by CountPerOp every instruction (live value).
            // When VI is the current timer (typical), NextTimer ≈ VI remaining.
            // half_line counts down from ~525 to 0 each frame (P64-style).
            int32_t next_timer = m_system.m_NextTimer;
            uint32_t count_per_op = m_system.CountPerOp();
            uint32_t vi_period = 781250u * count_per_op;
            uint32_t v_sync_val = m_vi_regs.v_sync();
            uint32_t num_half_lines = (v_sync_val > 0) ? (v_sync_val + 1) : 526u;
            uint32_t vi_refresh_rate = vi_period / num_half_lines;
            uint32_t half_line = 0;
            if (vi_refresh_rate > 0 && next_timer > 0) {
                half_line = (uint32_t)next_timer / vi_refresh_rate;
                if (half_line >= num_half_lines) half_line = num_half_lines - 1;
            }
            value = (half_line & ~1u) | m_video_state.current_field;
            break;
        }
            
        case VI_BURST_REG:    value = m_vi_regs.burst(); break;
        case VI_V_SYNC_REG:   value = m_vi_regs.v_sync(); break;
        case VI_H_SYNC_REG:   value = m_vi_regs.h_sync(); break;
        case VI_LEAP_REG:     value = m_vi_regs.leap(); break;
        case VI_H_START_REG:  value = m_vi_regs.h_start(); break;
        case VI_V_START_REG:  value = m_vi_regs.v_start(); break;
        case VI_V_BURST_REG:  value = m_vi_regs.v_burst(); break;
        case VI_X_SCALE_REG:  value = m_vi_regs.x_scale(); break;
        case VI_Y_SCALE_REG:  value = m_vi_regs.y_scale(); break;
        
        default:
            value = 0;
            return false;
    }
    return true;
}

bool VideoInterfaceHandler::Write32(uint32_t address, uint32_t value, uint32_t mask) {
    uint32_t offset = address & 0x3F;
    
    switch (offset) {
        case VI_STATUS_REG:
            handle_status_register_write(value, mask);
            break;
            
        case VI_ORIGIN_REG:
            handle_origin_register_write(value, mask);
            break;
            
        case VI_WIDTH_REG:
            if (m_vi_regs.width() != value) {
                m_vi_regs.width() = value;
                notify_framebuffer_change(m_vi_regs.origin());
            }
            break;
            
        case VI_INTR_REG:
            handle_v_intr_register_write(value, mask);
            break;
            
        case VI_CURRENT_REG:
            // Write clears interrupt
            clear_vi_interrupt();
            break;
            
        case VI_BURST_REG:    m_vi_regs.burst() = value; break;
        case VI_V_SYNC_REG:   m_vi_regs.v_sync() = value; break;
        case VI_H_SYNC_REG:   m_vi_regs.h_sync() = value; break;
        case VI_LEAP_REG:     m_vi_regs.leap() = value; break;
        case VI_H_START_REG:  m_vi_regs.h_start() = value; break;
        case VI_V_START_REG:  m_vi_regs.v_start() = value; break;
        case VI_V_BURST_REG:  m_vi_regs.v_burst() = value; break;
        case VI_X_SCALE_REG:  m_vi_regs.x_scale() = value; break;
        case VI_Y_SCALE_REG:  m_vi_regs.y_scale() = value; break;
        
        default:
            return false;
    }
    return true;
}

void VideoInterfaceHandler::handle_status_register_write(uint32_t value, uint32_t /*mask*/) {
    uint32_t old_status = m_vi_regs.status();
    m_vi_regs.status() = value;
    
    // Check for mode changes (BPP etc)
    if ((old_status & 0x03) != (value & 0x03)) {
        // Depth changed
        detect_video_mode_change();
    }
    
    // Serration bit (Bit 6)
    bool serrate = (value & 0x40) != 0;
    update_field_serration(serrate ? 1 : 0);
}

void VideoInterfaceHandler::handle_origin_register_write(uint32_t value, uint32_t /*mask*/) {
    value &= 0xFFFFFF; // Mask to 24-bit RDRAM address space
    if (m_vi_regs.origin() != value) {
        m_vi_regs.origin() = value;
        notify_framebuffer_change(value);
    }
}

void VideoInterfaceHandler::handle_v_intr_register_write(uint32_t value, uint32_t /*mask*/) {
    m_vi_regs.v_intr() = value;
    check_vi_interrupt();
}

void VideoInterfaceHandler::update_half_line() {
    m_video_state.half_line++;
    
    // Wrap around based on V_SYNC
    // V_SYNC register holds total half-lines per frame
    // Note: V_SYNC is typically ~525 for NTSC.
    
    uint32_t v_sync = m_vi_regs.v_sync();
    if (v_sync == 0) v_sync = 0x20C; // Safety default
    
    if (m_video_state.half_line >= v_sync) {
        m_video_state.half_line = 0;
        end_frame(); // Frame end processing
        begin_frame();
    }
    
    // Check Interrupt
    check_vi_interrupt();
    
    // Update scanline number
    m_video_state.current_scanline = m_video_state.half_line >> 1;
}

void VideoInterfaceHandler::check_vi_interrupt() {
    if (m_vi_regs.v_intr() == m_video_state.half_line) {
        generate_vi_interrupt();
    }
}

void VideoInterfaceHandler::generate_vi_interrupt() {
    m_stats.vi_interrupts++;
    
    // Set VI interrupt bit in MI_INTR_REG (Index 2)
    // MI_INTR_VI = 0x08
    m_sys_regs.m_Mips_Interface[2] |= 0x08;
    m_sys_regs.CheckInterrupts();
    
    // If callback is set, call it
    if (m_interrupt_callback) {
        m_interrupt_callback();
    }
}

void VideoInterfaceHandler::on_frame_timer() {
    // Called once per frame by the system timer.
    // Toggle field serration each frame (P64: m_FieldSerration ^= 1)
    // Then mask by interlace bit: field = field_serration & (VI_STATUS bit 6)
    m_video_state.field_serration ^= 1;
    uint32_t interlace_bit = (m_vi_regs.status() >> 6) & 1;
    m_video_state.current_field = m_video_state.field_serration & interlace_bit;


    generate_vi_interrupt();

    // NOTE: UpdateScreen is called by CN64System::RefreshScreen() which is invoked
    // by Systiming.cpp immediately after on_frame_timer(). Calling it again here
    // would result in two SwapBuffers per VI tick: the first renders content, the
    // second renders with a reset m_hleRenderedThisFrame flag and shows black.
    // That alternation (content / black / content / black) is the black/white flicker.

    // Advance frame counters
    m_stats.total_frames++;
}

void VideoInterfaceHandler::clear_vi_interrupt() {
    m_sys_regs.m_Mips_Interface[2] &= ~0x08;
    m_sys_regs.CheckInterrupts();
}

void VideoInterfaceHandler::update_field_serration(uint32_t interlaced) {
    // The field_serration toggle is handled in on_frame_timer().
    // This function is called when VI_STATUS changes — update the interlace
    // masking for the current_field computation.
    m_video_state.current_field = m_video_state.field_serration & interlaced;
}

void VideoInterfaceHandler::notify_framebuffer_change(uint32_t new_address) {
    m_stats.framebuffer_changes++;
    if (m_frame_callback) {
        VideoMode mode = get_current_video_mode();
        m_frame_callback(mode, new_address);
    }
}

VideoInterfaceHandler::VideoMode VideoInterfaceHandler::get_current_video_mode() const {
    VideoMode mode;
    mode.width = m_vi_regs.width();
    float scale_x = (float)(m_vi_regs.x_scale() & 0xFFF) / 1024.0f;
    float scale_y = (float)(m_vi_regs.y_scale() & 0xFFF) / 1024.0f;
    mode.height = (uint32_t)((float)mode.width * (scale_y / (scale_x > 0 ? scale_x : 1.0f))); // Rough estimation
    mode.color_depth = m_vi_regs.get_color_depth();
    mode.interlaced = m_vi_regs.get_serrate() == VI::Serrate::ENABLE;
    mode.x_scale = scale_x;
    mode.y_scale = scale_y;
    return mode;
}

void VideoInterfaceHandler::begin_frame() {
    m_video_state.frame_start_time = 0; // Capture time
}

void VideoInterfaceHandler::end_frame() {
    m_stats.total_frames++;
    // Notify GFX plugin to update window
    // Plugin interface usage not fully visible, assuming standard calls
    // m_plugins->GFX()->UpdateScreen(); 
}

// ===================================
// Additional Helper Methods
// ===================================

void VideoInterfaceHandler::update_scanline() {
    uint32_t current_half = m_video_state.half_line;

    // Check V-Blank status
    uint32_t v_start = m_vi_regs.v_start() >> 16;
    uint32_t v_end = m_vi_regs.v_start() & 0xFFFF;
    
    // N64 V-Sync register contains number of half-lines
    // If current half-line is outside active video range, we are in V-Blank
    bool vblank = (current_half < v_start) || (current_half >= v_end);
    
    m_video_state.vblank_active = vblank;
    
    // H-Blank check (simplified)
    // Would require horizontal counter relative to H_SYNC
    // For now we assume H-Blank at start of line
    m_video_state.hblank_active = false; 
}

void VideoInterfaceHandler::wait_for_vblank() {
    // Spin until vblank is active
    // In emulation, this might pause the thread, but we should be careful about blocking
    // For now, simpler implementation:
    if (!m_video_state.vblank_active) {
        // Yield to let other threads process if we are multithreaded
        // Or just return if we can't block safely
    }
}

void VideoInterfaceHandler::detect_video_mode_change() {
    VideoMode new_mode = get_current_video_mode();
    
    if (new_mode.width != m_video_state.current_mode.width ||
        new_mode.height != m_video_state.current_mode.height ||
        new_mode.color_depth != m_video_state.current_mode.color_depth ||
        new_mode.interlaced != m_video_state.current_mode.interlaced) {
        
        apply_video_mode(new_mode);
    }
}

void VideoInterfaceHandler::apply_video_mode(const VideoMode& mode) {
    if (m_mode_change_callback) {
        m_mode_change_callback(m_video_state.current_mode, mode);
    }
    
    m_video_state.previous_mode = m_video_state.current_mode;
    m_video_state.current_mode = mode;
    m_stats.mode_changes++;
    
    log_video_mode_change(m_video_state.previous_mode, mode);
}

void VideoInterfaceHandler::limit_frame_rate() {
    // If not using sync, return
    // Logic depends on system timer being accurate
    // We defer to SystemTimer sync for now
    m_system_timer.sync_to_real_time();
}

void VideoInterfaceHandler::update_frame_timing() {
    // Calculate FPS and stats
    // This should be called at end_frame
}

std::string VideoInterfaceHandler::get_video_info() const {
    VideoMode mode = get_current_video_mode();
    return mode.to_string();
}

std::string VideoInterfaceHandler::get_register_dump() const {
    std::stringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');
    ss << "VI_STATUS:  " << std::setw(8) << m_vi_regs.status() << "\n";
    ss << "VI_ORIGIN:  " << std::setw(8) << m_vi_regs.origin() << "\n";
    ss << "VI_WIDTH:   " << std::setw(8) << m_vi_regs.width() << "\n";
    ss << "VI_INTR:    " << std::setw(8) << m_vi_regs.v_intr() << "\n";
    ss << "VI_CURRENT: " << std::setw(8) << m_video_state.half_line << "\n";
    ss << "VI_BURST:   " << std::setw(8) << m_vi_regs.burst() << "\n";
    ss << "VI_V_SYNC:  " << std::setw(8) << m_vi_regs.v_sync() << "\n";
    ss << "VI_H_SYNC:  " << std::setw(8) << m_vi_regs.h_sync();
    return ss.str();
}

void VideoInterfaceHandler::save_state(std::ostream& os) const {
    // Save registers
    os.write(reinterpret_cast<const char*>(m_registers.data()), 
             m_registers.size() * sizeof(uint32_t));
             
    // Save state
    os.write(reinterpret_cast<const char*>(&m_video_state), sizeof(VideoState));
    os.write(reinterpret_cast<const char*>(&m_stats), sizeof(Statistics));
}

void VideoInterfaceHandler::load_state(std::istream& is) {
    // Load registers
    is.read(reinterpret_cast<char*>(m_registers.data()), 
            m_registers.size() * sizeof(uint32_t));
            
    // Load state
    is.read(reinterpret_cast<char*>(&m_video_state), sizeof(VideoState));
    is.read(reinterpret_cast<char*>(&m_stats), sizeof(Statistics));
}

uint32_t VideoInterfaceHandler::calculate_scanline_time() const {
    // N64 Clock / (H_SYNC period usually)
    // Simplified:
    return is_ntsc_mode() ? 1524 : 1568; // Roughly cycles per scanline
}

uint32_t VideoInterfaceHandler::calculate_vblank_start() const {
    return (m_vi_regs.v_start() & 0xFFFF);
}

uint32_t VideoInterfaceHandler::calculate_vblank_end() const {
    return (m_vi_regs.v_start() >> 16);
}

bool VideoInterfaceHandler::is_ntsc_mode() const {
    // Usually determined by V_SYNC count
    // NTSC ~525, PAL ~625
    return m_vi_regs.v_sync() < 600;
}

bool VideoInterfaceHandler::is_pal_mode() const {
    return !is_ntsc_mode();
}

bool VideoInterfaceHandler::validate_framebuffer_address(uint32_t address) const {
    // Basic RDRAM check
    return address < 0x00800000; // 8MB RDRAM
}

void VideoInterfaceHandler::log_video_mode_change(const VideoMode& old_mode, const VideoMode& new_mode) const {
    Log::infof("VI Mode Change: %s -> %s", old_mode.to_string().c_str(), new_mode.to_string().c_str());
}

void VideoInterfaceHandler::log_register_access(uint32_t address, uint32_t value, bool is_write) const {
    Log::debugf("VI Reg %s [0x%X] = 0x%X", (is_write ? "Write" : "Read"), address, value);
}

void VideoInterfaceHandler::log_frame_statistics() const {
    // Log once per second typically
}

void VideoInterfaceHandler::on_system_reset() {
    reset();
}

void VideoInterfaceHandler::on_game_state_loaded() {
    // Refresh derived state
    detect_video_mode_change();
}

