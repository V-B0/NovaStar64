/**
 * @file GFXPlugin.hpp
 * @brief Graphics plugin interface for N64 RDP/VI emulation
 * 
 * Handles all graphics-related functionality:
 * - RDP (Reality Display Processor) command processing
 * - VI (Video Interface) frame rendering
 * - Frame buffer management
 * - Display output
 */

#pragma once

#include "PluginBase.hpp"
#include <cstdint>

namespace Plugins {

/**
 * @struct RenderSettings
 * @brief Graphics rendering configuration
 */
struct RenderSettings {
    uint32_t WindowWidth;           // Output window width
    uint32_t WindowHeight;          // Output window height
    uint32_t RefreshRate;           // Display refresh rate
    bool Fullscreen;                // Fullscreen mode
    bool VSync;                     // Vertical sync
    bool AntiAliasing;              // Anti-aliasing enabled
    uint32_t AnisotropicFiltering;  // Anisotropic filtering level
    bool Bilinear;                  // Bilinear filtering
};

/**
 * @struct FrameBufferInfo
 * @brief Frame buffer information structure
 */
struct FrameBufferInfo {
    uint32_t* ColorBuffer;          // Pointer to color buffer (RGBA8888)
    uint32_t* DepthBuffer;          // Pointer to depth buffer
    uint32_t Width;                 // Frame buffer width
    uint32_t Height;                // Frame buffer height
    uint32_t Stride;                // Row stride in pixels
};

/**
 * @class GFXPlugin
 * @brief Graphics plugin interface
 */
class GFXPlugin : public PluginBase {
public:
    virtual ~GFXPlugin() = default;
    
    // ========================================================================
    // Display Initialization
    // ========================================================================
    
    /**
     * @brief Initialize display output
     * @param hwnd Window handle for rendering
     * @param settings Render settings
     * @return true on success
     */
    virtual bool InitializeDisplay(void* hwnd, const RenderSettings& settings) = 0;
    
    /**
     * @brief Resize display output
     * @param width New width
     * @param height New height
     */
    virtual void ResizeDisplay(uint32_t width, uint32_t height) = 0;
    
    // ========================================================================
    // RDP Processing
    // ========================================================================
    
    /**
     * @brief Process RDP display list
     * @param start Start address in RDRAM
     * @param end End address in RDRAM
     */
    virtual void ProcessDList(uint32_t start, uint32_t end) = 0;
    
    /**
     * @brief Process single RDP command
     * @param cmd Command word
     */
    virtual void ProcessRDPCommand(uint64_t cmd) = 0;
    
    /**
     * @brief Update RDP status
     */
    virtual void UpdateRDPStatus() = 0;
    
    // ========================================================================
    // Video Interface
    // ========================================================================
    
    /**
     * @brief Update screen (called on VI interrupt)
     */
    virtual void UpdateScreen() = 0;
    
    /**
     * @brief Move rendered frame to display
     */
    virtual void SwapBuffers() = 0;
    
    /**
     * @brief Get current frame buffer info
     * @return Frame buffer information
     */
    virtual FrameBufferInfo GetFrameBufferInfo() const = 0;
    
    /**
     * @brief Read frame buffer to memory
     * @param dest Destination buffer
     * @param addr RDRAM address
     * @param width Width in pixels
     * @param height Height in pixels
     * @param size Size in bytes
     */
    virtual void ReadFrameBuffer(uint8_t* dest, uint32_t addr, uint32_t width, uint32_t height, uint32_t size) = 0;
    
    /**
     * @brief Write to frame buffer from memory
     * @param src Source buffer
     * @param addr RDRAM address
     * @param width Width in pixels
     * @param height Height in pixels
     * @param size Size in bytes
     */
    virtual void WriteFrameBuffer(const uint8_t* src, uint32_t addr, uint32_t width, uint32_t height, uint32_t size) = 0;
    
    // ========================================================================
    // Status and Debugging
    // ========================================================================
    
    /**
     * @brief Check if frame is ready for display
     * @return true if frame is complete
     */
    virtual bool IsFrameReady() const = 0;
    
    /**
     * @brief Get rendering statistics
     * @param fps Frames per second
     * @param vis VIs per second
     */
    virtual void GetStatistics(float& fps, float& vis) const = 0;
    
    /**
     * @brief Capture screenshot
     * @param filename Output filename
     * @return true on success
     */
    virtual bool CaptureScreenshot(const std::string& filename) = 0;
    
    /**
     * @brief Get last frame's swap time (SwapBuffers duration)
     * @return Swap time in milliseconds
     */
    virtual double GetLastSwapTimeMs() const { return 0.0; }
};

} // namespace Plugins
