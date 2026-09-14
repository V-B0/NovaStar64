/**
 * @file LuaEngine.hpp
 * @brief Lua 5.4 scripting engine for NovaStar64 modding.
 *
 * Exposed Lua API:
 *   mem.read8(addr)  mem.read16(addr)  mem.read32(addr)
 *   mem.write8(addr, val)  mem.write16(addr, val)  mem.write32(addr, val)
 *   emu.frame_count()
 *   emu.rom_name()
 *   emu.register_frame_callback(fn)   -- fn() called every VI frame
 *   input.get_buttons()               -- returns bitmask uint16
 *   input.override_buttons(mask)      -- overrides input this frame
 *   console.print(msg)                -- writes to scripts output log
 */
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

struct lua_State;

namespace NovaUI {

class LuaEngine {
public:
    LuaEngine();
    ~LuaEngine();

    /// (Re)initialize the Lua state and register the NovaStar64 API.
    bool Init(uint8_t* rdram, size_t rdramSize);

    /// Shutdown and release Lua state.
    void Shutdown();

    /// Load and execute a Lua script file. Returns true on success.
    bool LoadScript(const std::string& path);

    /// Unload all scripts.
    void UnloadAll();

    /// Called by the emulator every VI frame — fires registered frame callbacks.
    void OnFrame();

    /// Points the script API at the running machine's RDRAM. Safe to call every
    /// frame: it re-binds only when the pointer actually changes, which is what
    /// lets scripts keep working across a reset or a ROM swap.
    void BindMemory(uint8_t* rdram, size_t rdramSize);

    /// Returns the console output log (populated by console.print).
    const std::vector<std::string>& GetLog() const { return m_log; }
    void ClearLog() { m_log.clear(); }

    /// True if the engine was successfully initialized.
    bool IsReady() const { return m_L != nullptr; }

    // Public for Lua C API callbacks
    std::vector<std::string> m_log;
    uint8_t*  m_rdram     = nullptr;
    size_t    m_rdramSize = 0;

    // Per-frame input override (set by Lua input.override_buttons)
    bool     m_hasInputOverride = false;
    uint16_t m_inputOverride    = 0;

private:
    lua_State* m_L = nullptr;
    void RegisterAPI();
    static int lua_mem_read8  (lua_State* L);
    static int lua_mem_read16 (lua_State* L);
    static int lua_mem_read32 (lua_State* L);
    static int lua_mem_write8 (lua_State* L);
    static int lua_mem_write16(lua_State* L);
    static int lua_mem_write32(lua_State* L);
    static int lua_emu_frame_count(lua_State* L);
    static int lua_emu_rom_name(lua_State* L);
    static int lua_emu_register_frame_callback(lua_State* L);
    static int lua_input_get_buttons(lua_State* L);
    static int lua_input_override_buttons(lua_State* L);
    static int lua_console_print(lua_State* L);
};

/// Global singleton accessed by Lua C callbacks.
extern LuaEngine* g_luaEngine;

} // namespace NovaUI
