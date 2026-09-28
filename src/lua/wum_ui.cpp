// wum.ui: overlay panels, menu items, hotkeys and a small ImGui widget subset.
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <map>

#include "lua/sandbox_core.h"
#include "melange/overlay.h"

namespace melange::sandbox {
namespace {
// The overlay cannot remove menu items or hotkeys: one slot per path/key lives for the process and is rebound.
struct MenuSlot {
    uint32_t cb = 0;
};
struct HotkeySlot {
    std::vector<uint32_t> cbs;
};
std::map<std::string, MenuSlot*> g_menus;
std::map<uint16_t, HotkeySlot*> g_hotkeys;
Callback* g_panel = nullptr;  // the panel whose function is running

void* IdPtr(uint32_t id) { return reinterpret_cast<void*>(static_cast<uintptr_t>(id)); }

void PanelTramp(void* user) {
    Callback* cb = FindCallback(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user)));
    if (!cb || cb->dead) return;
    if (cb->disabled) {
        ImGui::TextDisabled("This panel was disabled after 3 errors (see the log).");
        return;
    }
    ImGuiErrorRecoveryState st;
    ImGui::ErrorRecoveryStoreState(&st);
    Callback* prev = g_panel;
    g_panel = cb;
    const bool ok = Invoke(cb, {});
    g_panel = prev;
    if (!ok) ImGui::ErrorRecoveryTryToRecoverWindowState(&st);
}

void MenuTramp(void* user) {
    auto* slot = static_cast<MenuSlot*>(user);
    if (Callback* cb = FindCallback(slot->cb)) Invoke(cb, {});
}

void HotkeyTramp(void* user) {
    auto* slot = static_cast<HotkeySlot*>(user);
    const std::vector<uint32_t> ids = slot->cbs;
    for (uint32_t id : ids)
        if (Callback* cb = FindCallback(id)) Invoke(cb, {});
}

void RequirePanel(lua_State* L) {
    if (!g_panel || g_panel->gen->mod != Current()) luaL_error(L, "wum.ui widgets can only be used inside the mod's panel function");
}

int Panel(lua_State* L) {
    const std::string id = luaL_checkstring(L, 1);
    const std::string title = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    const bool open = lua_toboolean(L, 4) != 0;
    ModRec* m = Current();
    Gen* g = CurrentGen();
    if (!m || !g) return luaL_error(L, "wum.ui.panel needs a mod context");
    if (id.empty() || id.size() > 64) return luaL_argerror(L, 1, "id must be 1-64 characters");
    if (!g->panelIds.insert(id).second) return luaL_error(L, "wum.ui.panel: '%s' already exists", id.c_str());
    Callback* cb = NewCallback(L, 3, CbKind::Panel, id);
    const std::string fullId = "mod." + m->id + "." + id;
    const std::string fullTitle = m->name + "/" + title;
    const uint32_t cid = cb->id;
    auto handle = std::make_shared<int>(0);
    cb->attach = [fullId, fullTitle, cid, open, handle] {
        *handle = overlay::AddPanel(fullId.c_str(), fullTitle.c_str(), &PanelTramp, IdPtr(cid),
                                    open ? overlay::kPanelOpenByDefault : overlay::kPanelNone);
        return *handle != 0;
    };
    cb->revoke = [handle] { overlay::RemovePanel(*handle); };
    if (!Activate(cb)) return luaL_error(L, "wum.ui.panel: the overlay refused '%s'", id.c_str());
    lua_pushinteger(L, cid);
    return 1;
}

int Menu(lua_State* L) {
    const std::string path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    ModRec* m = Current();
    if (!m) return luaL_error(L, "wum.ui.menu needs a mod context");
    const std::string full = "Mods/" + m->name + "/" + path;
    Callback* cb = NewCallback(L, 2, CbKind::Menu, path);
    const uint32_t cid = cb->id;
    cb->attach = [full, cid] {
        MenuSlot*& slot = g_menus[full];
        if (!slot) {
            slot = new MenuSlot;
            if (!overlay::AddMenuItem(full.c_str(), &MenuTramp, slot)) {
                delete slot;
                slot = nullptr;
                g_menus.erase(full);
                return false;
            }
        }
        slot->cb = cid;
        return true;
    };
    cb->revoke = [full, cid] {
        auto it = g_menus.find(full);
        if (it != g_menus.end() && it->second->cb == cid) it->second->cb = 0;
    };
    if (!Activate(cb)) return luaL_error(L, "wum.ui.menu: bad path '%s'", path.c_str());
    lua_pushinteger(L, cid);
    return 1;
}

int Hotkey(lua_State* L) {
    const std::string text = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    uint8_t dik = 0, mods = 0;
    if (!overlay::ParseHotkey(text.c_str(), &dik, &mods)) return luaL_argerror(L, 1, "not a hotkey (e.g. \"Ctrl+Shift+H\")");
    const uint16_t key = static_cast<uint16_t>(dik | (mods << 8));
    Callback* cb = NewCallback(L, 2, CbKind::Hotkey, text);
    const uint32_t cid = cb->id;
    cb->attach = [key, dik, mods, cid] {
        HotkeySlot*& slot = g_hotkeys[key];
        if (!slot) {
            slot = new HotkeySlot;
            overlay::AddHotkey(dik, mods, &HotkeyTramp, slot);
        }
        slot->cbs.push_back(cid);
        return true;
    };
    cb->revoke = [key, cid] {
        auto it = g_hotkeys.find(key);
        if (it == g_hotkeys.end()) return;
        auto& v = it->second->cbs;
        v.erase(std::remove(v.begin(), v.end(), cid), v.end());
    };
    Activate(cb);
    lua_pushinteger(L, cid);
    return 1;
}

int Remove(lua_State* L) {
    const lua_Integer h = luaL_checkinteger(L, 1);
    Callback* cb = h > 0 ? FindCallback(static_cast<uint32_t>(h)) : nullptr;
    const bool ok = cb && cb->gen->mod == Current() &&
                    (cb->kind == CbKind::Panel || cb->kind == CbKind::Menu || cb->kind == CbKind::Hotkey);
    if (ok) {
        if (cb->kind == CbKind::Panel) cb->gen->panelIds.erase(cb->label);
        KillCallback(cb->id);
    }
    lua_pushboolean(L, ok);
    return 1;
}

// ---------------------------------------------------------------- widgets
int Text(lua_State* L) {
    RequirePanel(L);
    ImGui::TextUnformatted(luaL_checkstring(L, 1));
    return 0;
}

int Button(lua_State* L) {
    RequirePanel(L);
    lua_pushboolean(L, ImGui::Button(luaL_checkstring(L, 1)));
    return 1;
}

int Checkbox(lua_State* L) {
    RequirePanel(L);
    bool v = lua_toboolean(L, 2) != 0;
    const bool changed = ImGui::Checkbox(luaL_checkstring(L, 1), &v);
    lua_pushboolean(L, v);
    lua_pushboolean(L, changed);
    return 2;
}

int SliderFloat(lua_State* L) {
    RequirePanel(L);
    float v = static_cast<float>(luaL_checknumber(L, 2));
    const bool changed = ImGui::SliderFloat(luaL_checkstring(L, 1), &v, static_cast<float>(luaL_checknumber(L, 3)),
                                            static_cast<float>(luaL_checknumber(L, 4)));
    lua_pushnumber(L, v);
    lua_pushboolean(L, changed);
    return 2;
}

int SliderInt(lua_State* L) {
    RequirePanel(L);
    int v = static_cast<int>(luaL_checkinteger(L, 2));
    const bool changed = ImGui::SliderInt(luaL_checkstring(L, 1), &v, static_cast<int>(luaL_checkinteger(L, 3)),
                                          static_cast<int>(luaL_checkinteger(L, 4)));
    lua_pushinteger(L, v);
    lua_pushboolean(L, changed);
    return 2;
}

int InputText(lua_State* L) {
    RequirePanel(L);
    size_t len;
    const char* s = luaL_optlstring(L, 2, "", &len);
    const lua_Integer cap = std::clamp<lua_Integer>(luaL_optinteger(L, 3, 256), 1, 4096);
    std::string buf(static_cast<size_t>(cap) + 1, '\0');
    memcpy(buf.data(), s, std::min(len, static_cast<size_t>(cap)));
    const bool changed = ImGui::InputText(luaL_checkstring(L, 1), buf.data(), buf.size());
    lua_pushstring(L, buf.c_str());
    lua_pushboolean(L, changed);
    return 2;
}

int Combo(lua_State* L) {
    RequirePanel(L);
    const char* label = luaL_checkstring(L, 1);
    int idx = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<std::string> items;
    const lua_Integer n = std::min<lua_Integer>(luaL_len(L, 3), 256);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, 3, i);
        items.emplace_back(luaL_tolstring(L, -1, nullptr));
        lua_pop(L, 2);
    }
    std::vector<const char*> ptrs;
    for (const std::string& s : items) ptrs.push_back(s.c_str());
    const bool changed = ImGui::Combo(label, &idx, ptrs.data(), static_cast<int>(ptrs.size()));
    lua_pushinteger(L, idx + 1);
    lua_pushboolean(L, changed);
    return 2;
}

int Separator(lua_State* L) {
    RequirePanel(L);
    ImGui::Separator();
    return 0;
}

int SameLine(lua_State* L) {
    RequirePanel(L);
    ImGui::SameLine();
    return 0;
}

int Spacing(lua_State* L) {
    RequirePanel(L);
    ImGui::Spacing();
    return 0;
}

int CollapsingHeader(lua_State* L) {
    RequirePanel(L);
    lua_pushboolean(L, ImGui::CollapsingHeader(luaL_checkstring(L, 1)));
    return 1;
}

int ProgressBar(lua_State* L) {
    RequirePanel(L);
    ImGui::ProgressBar(static_cast<float>(luaL_checknumber(L, 1)), ImVec2(-1, 0), luaL_optstring(L, 2, nullptr));
    return 0;
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kUi[] = {{"panel", Panel},         {"menu", Menu},
                                   {"hotkey", Hotkey},       {"remove", Remove},
                                   {"text", Text},           {"button", Button},
                                   {"checkbox", Checkbox},   {"sliderFloat", SliderFloat},
                                   {"sliderInt", SliderInt}, {"inputText", InputText},
                                   {"combo", Combo},         {"separator", Separator},
                                   {"sameLine", SameLine},   {"spacing", Spacing},
                                   {"collapsingHeader", CollapsingHeader},
                                   {"progressBar", ProgressBar},
                                   {nullptr, nullptr}};
    lua_newtable(L);
    RegisterFunctions(L, -1, kUi);
    lua_setfield(L, wum, "ui");
}

const LibRegistrar g_reg(&Shared, nullptr);
}  // namespace
}  // namespace melange::sandbox
