/**
 * @file LuaEngine.cpp
 * @brief Lua 5.4 scripting engine implementation.
 */
#include "LuaEngine.hpp"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <cstring>
#include <filesystem>

namespace NovaUI {

LuaEngine* g_luaEngine = nullptr;

// ── Registry key for frame callback table ────────────────────────────────────
static const char* kFrameCBKey = "nova64_frame_callbacks";

// ── Constructor / Destructor ──────────────────────────────────────────────────
LuaEngine::LuaEngine()  { g_luaEngine = this; }
LuaEngine::~LuaEngine() { Shutdown(); if (g_luaEngine == this) g_luaEngine = nullptr; }

// ── Init ──────────────────────────────────────────────────────────────────────
bool LuaEngine::Init(uint8_t* rdram, size_t rdramSize) {
    if (m_L) Shutdown();
    m_rdram     = rdram;
    m_rdramSize = rdramSize;

    m_L = luaL_newstate();
    if (!m_L) return false;
    luaL_openlibs(m_L);

    // Create frame-callback table in the registry
    lua_newtable(m_L);
    lua_setfield(m_L, LUA_REGISTRYINDEX, kFrameCBKey);

    RegisterAPI();
    std::filesystem::create_directories("scripts");
    return true;
}

void LuaEngine::Shutdown() {
    if (!m_L) return;
    lua_close(m_L);
    m_L = nullptr;
    m_hasInputOverride = false;
}

bool LuaEngine::LoadScript(const std::string& path) {
    if (!m_L) return false;
    int err = luaL_dofile(m_L, path.c_str());
    if (err != LUA_OK) {
        std::string msg = lua_tostring(m_L, -1);
        lua_pop(m_L, 1);
        m_log.push_back("[ERROR] " + msg);
        return false;
    }
    m_log.push_back("[LOADED] " + path);
    return true;
}

void LuaEngine::UnloadAll() {
    Shutdown();
    Init(m_rdram, m_rdramSize);
}

void LuaEngine::OnFrame() {
    if (!m_L) return;
    m_hasInputOverride = false;   // reset per frame

    // Call each registered frame callback
    lua_getfield(m_L, LUA_REGISTRYINDEX, kFrameCBKey);
    int tbl = lua_gettop(m_L);
    lua_pushnil(m_L);
    while (lua_next(m_L, tbl)) {
        if (lua_isfunction(m_L, -1)) {
            int err = lua_pcall(m_L, 0, 0, 0);
            if (err != LUA_OK) {
                std::string msg = lua_tostring(m_L, -1);
                lua_pop(m_L, 1);
                m_log.push_back("[FRAME CB ERROR] " + msg);
            }
        } else {
            lua_pop(m_L, 1);
        }
    }
    lua_pop(m_L, 1); // pop table
}

// ── API Registration ──────────────────────────────────────────────────────────
void LuaEngine::RegisterAPI() {
    // -- mem table --
    static const luaL_Reg mem_lib[] = {
        { "read8",   lua_mem_read8  },
        { "read16",  lua_mem_read16 },
        { "read32",  lua_mem_read32 },
        { "write8",  lua_mem_write8  },
        { "write16", lua_mem_write16 },
        { "write32", lua_mem_write32 },
        { nullptr,   nullptr }
    };
    luaL_newlib(m_L, mem_lib);
    lua_setglobal(m_L, "mem");

    // -- emu table --
    static const luaL_Reg emu_lib[] = {
        { "frame_count",              lua_emu_frame_count },
        { "rom_name",                 lua_emu_rom_name },
        { "register_frame_callback",  lua_emu_register_frame_callback },
        { nullptr, nullptr }
    };
    luaL_newlib(m_L, emu_lib);
    lua_setglobal(m_L, "emu");

    // -- input table --
    static const luaL_Reg input_lib[] = {
        { "get_buttons",      lua_input_get_buttons },
        { "override_buttons", lua_input_override_buttons },
        { nullptr, nullptr }
    };
    luaL_newlib(m_L, input_lib);
    lua_setglobal(m_L, "input");

    // -- console table --
    static const luaL_Reg console_lib[] = {
        { "print", lua_console_print },
        { nullptr, nullptr }
    };
    luaL_newlib(m_L, console_lib);
    lua_setglobal(m_L, "console");
}

// ── Lua API implementations ───────────────────────────────────────────────────
static inline uint32_t luaAddr(lua_State* L, int idx) {
    return (uint32_t)luaL_checkinteger(L, idx) & 0x1FFFFFFF; // mask to physical
}

static LuaEngine* eng() { return g_luaEngine; }

int LuaEngine::lua_mem_read8(lua_State* L) {
    auto* e = eng(); if (!e || !e->m_rdram) { lua_pushinteger(L, 0); return 1; }
    uint32_t a = luaAddr(L, 1);
    if (a < e->m_rdramSize) lua_pushinteger(L, e->m_rdram[a]);
    else lua_pushinteger(L, 0);
    return 1;
}

int LuaEngine::lua_mem_read16(lua_State* L) {
    auto* e = eng(); if (!e || !e->m_rdram) { lua_pushinteger(L, 0); return 1; }
    uint32_t a = luaAddr(L, 1) & ~1u;
    if (a + 1 < e->m_rdramSize) {
        uint16_t v = (uint16_t)((e->m_rdram[a] << 8) | e->m_rdram[a+1]);
        lua_pushinteger(L, v);
    } else lua_pushinteger(L, 0);
    return 1;
}

int LuaEngine::lua_mem_read32(lua_State* L) {
    auto* e = eng(); if (!e || !e->m_rdram) { lua_pushinteger(L, 0); return 1; }
    uint32_t a = luaAddr(L, 1) & ~3u;
    if (a + 3 < e->m_rdramSize) {
        uint32_t v = ((uint32_t)e->m_rdram[a]   << 24) | ((uint32_t)e->m_rdram[a+1] << 16)
                   | ((uint32_t)e->m_rdram[a+2] << 8)  |  (uint32_t)e->m_rdram[a+3];
        lua_pushinteger(L, (lua_Integer)v);
    } else lua_pushinteger(L, 0);
    return 1;
}

int LuaEngine::lua_mem_write8(lua_State* L) {
    auto* e = eng(); if (!e || !e->m_rdram) return 0;
    uint32_t a = luaAddr(L, 1);
    uint8_t  v = (uint8_t)luaL_checkinteger(L, 2);
    if (a < e->m_rdramSize) e->m_rdram[a] = v;
    return 0;
}

int LuaEngine::lua_mem_write16(lua_State* L) {
    auto* e = eng(); if (!e || !e->m_rdram) return 0;
    uint32_t a = luaAddr(L, 1) & ~1u;
    uint16_t v = (uint16_t)luaL_checkinteger(L, 2);
    if (a + 1 < e->m_rdramSize) {
        e->m_rdram[a]   = (v >> 8) & 0xFF;
        e->m_rdram[a+1] = v & 0xFF;
    }
    return 0;
}

int LuaEngine::lua_mem_write32(lua_State* L) {
    auto* e = eng(); if (!e || !e->m_rdram) return 0;
    uint32_t a = luaAddr(L, 1) & ~3u;
    uint32_t v = (uint32_t)luaL_checkinteger(L, 2);
    if (a + 3 < e->m_rdramSize) {
        e->m_rdram[a]   = (v >> 24) & 0xFF;
        e->m_rdram[a+1] = (v >> 16) & 0xFF;
        e->m_rdram[a+2] = (v >>  8) & 0xFF;
        e->m_rdram[a+3] =  v        & 0xFF;
    }
    return 0;
}

int LuaEngine::lua_emu_frame_count(lua_State* L) {
    lua_pushinteger(L, 0); // TODO: wire real frame counter
    return 1;
}

int LuaEngine::lua_emu_rom_name(lua_State* L) {
    lua_pushstring(L, "unknown"); // TODO: wire ROM name
    return 1;
}

int LuaEngine::lua_emu_register_frame_callback(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, kFrameCBKey);
    int n = (int)luaL_len(L, -1) + 1;
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, n);
    lua_pop(L, 1);
    return 0;
}

int LuaEngine::lua_input_get_buttons(lua_State* L) {
    lua_pushinteger(L, 0); // TODO: wire real input
    return 1;
}

int LuaEngine::lua_input_override_buttons(lua_State* L) {
    auto* e = eng(); if (!e) return 0;
    e->m_hasInputOverride = true;
    e->m_inputOverride    = (uint16_t)luaL_checkinteger(L, 1);
    return 0;
}

int LuaEngine::lua_console_print(lua_State* L) {
    auto* e = eng(); if (!e) return 0;
    const char* s = luaL_checkstring(L, 1);
    e->m_log.push_back(s);
    return 0;
}

} // namespace NovaUI
