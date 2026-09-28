// wum.mod, wum.log, wum.events, wum.timers, wum.config, wum.storage, wum.game.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/config.h"
#include "core/game.h"
#include "lua/sandbox_core.h"
#include "tools/json_mini.h"

namespace melange::sandbox {
namespace {
constexpr size_t kStorageCap = 1u << 20;
constexpr size_t kReadFileCap = 8u << 20;
constexpr int kMaxJsonDepth = 32;

ModRec* NeedMod(lua_State* L) {
    ModRec* m = Current();
    if (!m) luaL_error(L, "this function needs a mod context (not available in the console environment)");
    return m;
}

Callback* OwnedCallback(lua_State* L, int idx, CbKind kind) {
    const lua_Integer h = luaL_checkinteger(L, idx);
    Callback* cb = h > 0 ? FindCallback(static_cast<uint32_t>(h)) : nullptr;
    if (!cb || cb->kind != kind || cb->gen->mod != Current()) return nullptr;
    return cb;
}

// Strict UTF-8 (no overlongs, no surrogates), as the JSON reader expects when storage is read back.
bool ValidUtf8(const char* s, size_t n) {
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        size_t k;
        unsigned char lo = 0x80, hi = 0xbf;
        if (c < 0x80) k = 0;
        else if (c >= 0xc2 && c <= 0xdf) k = 1;
        else if (c >= 0xe0 && c <= 0xef) k = 2, lo = c == 0xe0 ? 0xa0 : 0x80, hi = c == 0xed ? 0x9f : 0xbf;
        else if (c >= 0xf0 && c <= 0xf4) k = 3, lo = c == 0xf0 ? 0x90 : 0x80, hi = c == 0xf4 ? 0x8f : 0xbf;
        else return false;
        if (k && i + k >= n) return false;
        for (size_t j = 1; j <= k; ++j) {
            const unsigned char b = p[i + j];
            if (j == 1 ? (b < lo || b > hi) : (b < 0x80 || b > 0xbf)) return false;
        }
        i += k + 1;
    }
    return true;
}

std::string Join(lua_State* L, int from) {
    std::string s;
    const int n = lua_gettop(L);
    for (int i = from; i <= n; ++i) {
        size_t len;
        const char* p = luaL_tolstring(L, i, &len);
        if (i > from) s += '\t';
        s.append(p, len);
        lua_pop(L, 1);
    }
    return s;
}

// ---------------------------------------------------------------- wum.mod
int ModReadFile(lua_State* L) {
    ModRec* m = NeedMod(L);
    const std::string rel = luaL_checkstring(L, 1);
    if (m->filesystem == "none") return luaL_error(L, "wum.mod.readFile needs permissions.filesystem \"own-folder\" in spice.json");
    if (!SafeRelPath(rel)) return luaL_error(L, "wum.mod.readFile: '%s' is not a relative path inside the mod folder", rel.c_str());
    std::wstring w = Widen(rel);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    std::string data, err;
    if (!ReadWhole(m->dir + L"\\" + w, kReadFileCap, &data, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

// ---------------------------------------------------------------- wum.log
int LogAt(lua_State* L, int level) {
    ModLog(Current(), level, Join(L, 1));
    return 0;
}
int LogDebug(lua_State* L) { return LogAt(L, 0); }
int LogInfo(lua_State* L) { return LogAt(L, 1); }
int LogWarn(lua_State* L) { return LogAt(L, 2); }
int LogError(lua_State* L) { return LogAt(L, 3); }

// ---------------------------------------------------------------- wum.events
int EventsOn(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (name.empty()) return luaL_error(L, "wum.events.on: empty event name");
    Callback* cb = NewCallback(L, 2, CbKind::Event, name);
    AddListener(name, cb->id);
    Activate(cb);
    lua_pushinteger(L, cb->id);
    return 1;
}

int EventsOff(lua_State* L) {
    Callback* cb = OwnedCallback(L, 1, CbKind::Event);
    if (cb) KillCallback(cb->id);
    lua_pushboolean(L, cb != nullptr);
    return 1;
}

int EventsEmit(lua_State* L) {
    ModRec* m = NeedMod(L);
    std::string name = luaL_checkstring(L, 1);
    const std::string own = "mod." + m->id + ".";
    if (name.rfind("mod.", 0) != 0) name = own + name;
    else if (name.rfind(own, 0) != 0) return luaL_error(L, "wum.events.emit: a mod can only emit mod.%s.* events", m->id.c_str());
    json::Value v;
    v.type = json::Type::Object;
    std::string err;
    if (!lua_isnoneornil(L, 2) && !ToJson(L, 2, &v, &err)) return luaL_error(L, "wum.events.emit: %s", err.c_str());
    QueueEvent(std::move(name), std::move(v));
    return 0;
}

// ---------------------------------------------------------------- wum.timers
int TimerAdd(lua_State* L, bool repeat) {
    const double sec = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!std::isfinite(sec) || sec < 0) return luaL_error(L, "wum.timers: seconds must be >= 0");
    char label[48];
    snprintf(label, sizeof(label), "%s %.3gs", repeat ? "every" : "after", sec);
    Callback* cb = NewCallback(L, 2, CbKind::Timer, label);
    AddTimer(cb->id, sec, repeat ? sec : -1.0);
    Activate(cb);
    lua_pushinteger(L, cb->id);
    return 1;
}
int TimersAfter(lua_State* L) { return TimerAdd(L, false); }
int TimersEvery(lua_State* L) { return TimerAdd(L, true); }
int TimersCancel(lua_State* L) {
    Callback* cb = OwnedCallback(L, 1, CbKind::Timer);
    if (cb) KillCallback(cb->id);
    lua_pushboolean(L, cb != nullptr);
    return 1;
}

// ---------------------------------------------------------------- wum.config
const SettingDecl* FindSetting(lua_State* L, ModRec* m, const std::string& key) {
    for (const SettingDecl& d : m->settings)
        if (d.key == key) return &d;
    luaL_error(L, "wum.config: '%s' is not declared in the settings of spice.json", key.c_str());
    return nullptr;
}

int ConfigGet(lua_State* L) {
    ModRec* m = NeedMod(L);
    const std::string key = luaL_checkstring(L, 1);
    const SettingDecl* d = FindSetting(L, m, key);
    const std::string v = config::GetString(("Mod." + m->id).c_str(), key.c_str(), d->def.c_str());
    if (d->type == "bool") lua_pushboolean(L, v == "1" || v == "true");
    else if (d->type == "int") lua_pushinteger(L, static_cast<lua_Integer>(strtoll(v.c_str(), nullptr, 10)));
    else if (d->type == "float") lua_pushnumber(L, strtod(v.c_str(), nullptr));
    else lua_pushstring(L, v.c_str());
    return 1;
}

int ConfigSet(lua_State* L) {
    ModRec* m = NeedMod(L);
    const std::string key = luaL_checkstring(L, 1);
    const SettingDecl* d = FindSetting(L, m, key);
    std::string text;
    char buf[64];
    if (d->type == "bool") {
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        text = lua_toboolean(L, 2) ? "true" : "false";
    } else if (d->type == "int" || d->type == "float") {
        const double v = d->type == "int" ? static_cast<double>(luaL_checkinteger(L, 2)) : luaL_checknumber(L, 2);
        if (!std::isfinite(v) || (d->hasMin && v < d->min) || (d->hasMax && v > d->max))
            return luaL_error(L, "wum.config: %s is out of range", key.c_str());
        if (d->type == "int") snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        else snprintf(buf, sizeof(buf), "%.9g", v);
        text = buf;
    } else if (d->type == "enum") {
        text = luaL_checkstring(L, 2);
        if (std::find(d->options.begin(), d->options.end(), text) == d->options.end())
            return luaL_error(L, "wum.config: '%s' is not an option of %s", text.c_str(), key.c_str());
    } else {
        text = luaL_checkstring(L, 2);
        if (text.find_first_of("\r\n") != std::string::npos) return luaL_error(L, "wum.config: line breaks are not allowed");
    }
    config::SetString(("Mod." + m->id).c_str(), key.c_str(), text.c_str());
    lua_pushboolean(L, 1);
    return 1;
}

// ---------------------------------------------------------------- wum.storage
void StorageLoad(ModRec* m) {
    if (m->storageLoaded) return;
    m->storageLoaded = true;
    m->storage.clear();
    m->storageBytes = 2;
    json::Value doc;
    json::Error e;
    const std::wstring path = StoragePath(m);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (!json::ParseFile(path, &doc, &e) || !doc.IsObject()) {
        SysLog(2, "wum.storage: unreadable file ignored", m->id, e.text);
        return;
    }
    for (auto& [k, v] : doc.members) {
        std::string s = WriteJson(v);
        m->storageBytes += k.size() + s.size() + 4;
        m->storage.emplace(k, std::move(s));
    }
}

int StorageGet(lua_State* L) {
    ModRec* m = NeedMod(L);
    StorageLoad(m);
    auto it = m->storage.find(luaL_checkstring(L, 1));
    if (it == m->storage.end()) return 0;
    json::Value v;
    json::Error e;
    if (!json::Parse(it->second, &v, &e)) return 0;
    PushJson(L, v);
    return 1;
}

void StorageErase(ModRec* m, const std::string& key) {
    auto it = m->storage.find(key);
    if (it == m->storage.end()) return;
    m->storageBytes -= it->first.size() + it->second.size() + 4;
    m->storage.erase(it);
    m->storageDirty = true;
}

int StorageSet(lua_State* L) {
    ModRec* m = NeedMod(L);
    StorageLoad(m);
    const std::string key = luaL_checkstring(L, 1);
    if (lua_isnoneornil(L, 2)) {
        StorageErase(m, key);
        return 0;
    }
    json::Value v;
    std::string err;
    if (!ToJson(L, 2, &v, &err)) return luaL_error(L, "wum.storage.set: %s", err.c_str());
    std::string s = WriteJson(v);
    size_t total = m->storageBytes + key.size() + s.size() + 4;
    auto it = m->storage.find(key);
    if (it != m->storage.end()) total -= it->first.size() + it->second.size() + 4;
    if (total > kStorageCap) return luaL_error(L, "wum.storage is full (1 MB per mod)");
    if (it != m->storage.end()) {
        if (it->second == s) return 0;
        it->second = std::move(s);
    } else {
        m->storage.emplace(key, std::move(s));
    }
    m->storageBytes = total;
    m->storageDirty = true;
    return 0;
}

int StorageRemove(lua_State* L) {
    ModRec* m = NeedMod(L);
    StorageLoad(m);
    StorageErase(m, luaL_checkstring(L, 1));
    return 0;
}

int StorageKeys(lua_State* L) {
    ModRec* m = NeedMod(L);
    StorageLoad(m);
    lua_createtable(L, static_cast<int>(m->storage.size()), 0);
    int i = 0;
    for (auto& [k, v] : m->storage) {
        lua_pushlstring(L, k.data(), k.size());
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// ---------------------------------------------------------------- wum.game
int GameScene(lua_State* L) {
    lua_pushstring(L, Game().scene.c_str());
    return 1;
}
int GameInMatch(lua_State* L) {
    lua_pushboolean(L, Game().inMatch);
    return 1;
}
int GameOnline(lua_State* L) {
    lua_pushboolean(L, Game().online);
    return 1;
}
int GameTurn(lua_State* L) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, Game().turn);
    lua_setfield(L, -2, "index");
    if (Game().team >= 0) {
        lua_pushinteger(L, Game().team);
        lua_setfield(L, -2, "team");
    }
    return 1;
}
int GameTick(lua_State* L) {
    lua_pushinteger(L, Game().tick);
    return 1;
}
int GameWorms(lua_State* L) {
    lua_pushnil(L);
    lua_pushliteral(L, "unavailable");
    return 2;
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kLog[] = {{"debug", LogDebug}, {"info", LogInfo}, {"warn", LogWarn}, {"error", LogError}, {nullptr, nullptr}};
    static const luaL_Reg kEvents[] = {{"on", EventsOn}, {"off", EventsOff}, {"emit", EventsEmit}, {nullptr, nullptr}};
    static const luaL_Reg kTimers[] = {{"after", TimersAfter}, {"every", TimersEvery}, {"cancel", TimersCancel}, {nullptr, nullptr}};
    static const luaL_Reg kConfig[] = {{"get", ConfigGet}, {"set", ConfigSet}, {nullptr, nullptr}};
    static const luaL_Reg kStorage[] = {{"get", StorageGet}, {"set", StorageSet}, {"remove", StorageRemove}, {"keys", StorageKeys}, {nullptr, nullptr}};
    static const luaL_Reg kGame[] = {{"scene", GameScene}, {"inMatch", GameInMatch}, {"online", GameOnline},
                                     {"turn", GameTurn}, {"tick", GameTick}, {"worms", GameWorms}, {nullptr, nullptr}};
    struct Ns {
        const char* name;
        const luaL_Reg* fns;
    };
    for (const Ns& ns : {Ns{"log", kLog}, Ns{"events", kEvents}, Ns{"timers", kTimers}, Ns{"config", kConfig},
                         Ns{"storage", kStorage}, Ns{"game", kGame}}) {
        lua_newtable(L);
        RegisterFunctions(L, -1, ns.fns);
        lua_setfield(L, wum, ns.name);
    }
}

void PerEnv(lua_State* L, int wum, ModRec* m) {
    if (!m) return;
    lua_createtable(L, 0, 6);
    lua_pushstring(L, m->id.c_str());
    lua_setfield(L, -2, "id");
    lua_pushstring(L, m->name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushstring(L, m->version.c_str());
    lua_setfield(L, -2, "version");
    lua_pushstring(L, game::Narrow(m->dir).c_str());
    lua_setfield(L, -2, "dir");
    lua_pushcfunction(L, ModReadFile);
    lua_setfield(L, -2, "readFile");
    lua_newtable(L);
    lua_setfield(L, -2, "keep");
    lua_setfield(L, wum, "mod");
}

const LibRegistrar g_reg(&Shared, &PerEnv);
}  // namespace

// ---------------------------------------------------------------- JSON <-> Lua
bool ToJson(lua_State* L, int idx, json::Value* out, std::string* err, int depth) {
    idx = lua_absindex(L, idx);
    if (depth > kMaxJsonDepth) {
        *err = "nested too deeply (or a cycle)";
        return false;
    }
    *out = {};
    switch (lua_type(L, idx)) {
        case LUA_TNIL: out->type = json::Type::Null; return true;
        case LUA_TBOOLEAN:
            out->type = json::Type::Bool;
            out->boolean = lua_toboolean(L, idx) != 0;
            return true;
        case LUA_TNUMBER: {
            const double d = static_cast<double>(lua_tonumber(L, idx));
            if (!std::isfinite(d)) {
                *err = "numbers must be finite";
                return false;
            }
            out->type = json::Type::Number;
            out->number = d;
            return true;
        }
        case LUA_TSTRING: {
            size_t n;
            const char* s = lua_tolstring(L, idx, &n);
            if (!ValidUtf8(s, n)) {
                *err = "strings must be valid UTF-8";
                return false;
            }
            out->type = json::Type::String;
            out->string.assign(s, n);
            return true;
        }
        case LUA_TTABLE: {
            const lua_Unsigned len = lua_rawlen(L, idx);
            size_t count = 0;
            bool array = len > 0;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                ++count;
                if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || static_cast<lua_Unsigned>(lua_tointeger(L, -2)) > len) array = false;
                lua_pop(L, 1);
            }
            array = array && count == len;
            out->type = array ? json::Type::Array : json::Type::Object;
            if (array) {
                for (lua_Unsigned i = 1; i <= len; ++i) {
                    lua_rawgeti(L, idx, static_cast<lua_Integer>(i));
                    json::Value v;
                    const bool ok = ToJson(L, -1, &v, err, depth + 1);
                    lua_pop(L, 1);
                    if (!ok) return false;
                    out->items.push_back(std::move(v));
                }
                return true;
            }
            std::vector<std::pair<std::string, json::Value>> members;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                if (lua_type(L, -2) != LUA_TSTRING) {
                    lua_pop(L, 2);
                    *err = "table keys must be strings (or a sequence 1..n)";
                    return false;
                }
                json::Value v;
                const bool ok = ToJson(L, -1, &v, err, depth + 1);
                std::string k = lua_tostring(L, -2);
                lua_pop(L, 1);
                if (!ok) {
                    lua_pop(L, 1);
                    return false;
                }
                members.emplace_back(std::move(k), std::move(v));
            }
            std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            out->members = std::move(members);
            return true;
        }
        default: *err = std::string("cannot store a ") + luaL_typename(L, idx) + " value"; return false;
    }
}

void PushJson(lua_State* L, const json::Value& v) {
    luaL_checkstack(L, 4, "PushJson");
    switch (v.type) {
        case json::Type::Null: lua_pushnil(L); return;
        case json::Type::Bool: lua_pushboolean(L, v.boolean); return;
        case json::Type::Number:
            if (v.IsInteger()) lua_pushinteger(L, static_cast<lua_Integer>(v.number));
            else lua_pushnumber(L, v.number);
            return;
        case json::Type::String: lua_pushlstring(L, v.string.data(), v.string.size()); return;
        case json::Type::Array:
            lua_createtable(L, static_cast<int>(v.items.size()), 0);
            for (size_t i = 0; i < v.items.size(); ++i) {
                PushJson(L, v.items[i]);
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
            return;
        case json::Type::Object:
            lua_createtable(L, 0, static_cast<int>(v.members.size()));
            for (const auto& [k, x] : v.members) {
                lua_pushlstring(L, k.data(), k.size());
                PushJson(L, x);
                lua_rawset(L, -3);
            }
            return;
    }
}

std::string WriteJson(const json::Value& v) {
    char buf[40];
    switch (v.type) {
        case json::Type::Null: return "null";
        case json::Type::Bool: return v.boolean ? "true" : "false";
        case json::Type::Number:
            if (v.IsInteger()) snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.number));
            else snprintf(buf, sizeof(buf), "%.17g", v.number);
            return buf;
        case json::Type::String: return "\"" + jsonmini::Escape(v.string) + "\"";
        case json::Type::Array: {
            std::string s = "[";
            for (size_t i = 0; i < v.items.size(); ++i) s += (i ? "," : "") + WriteJson(v.items[i]);
            return s + "]";
        }
        case json::Type::Object: {
            std::string s = "{";
            bool first = true;
            for (const auto& [k, x] : v.members) {
                if (!first) s += ",";
                first = false;
                s += "\"" + jsonmini::Escape(k) + "\":" + WriteJson(x);
            }
            return s + "}";
        }
    }
    return "null";
}

std::wstring StoragePath(const ModRec* m) { return game::DataDir() + L"\\mods\\" + Widen(m->id) + L"\\storage.json"; }

void StorageFlush(ModRec* m, bool force) {
    if (!m->storageDirty) return;
    const double now = NowSeconds();
    if (!force && now - m->storageWritten < 2.0) return;
    m->storageWritten = now;
    m->storageDirty = false;
    std::string text = "{";
    bool first = true;
    for (auto& [k, v] : m->storage) {
        if (!first) text += ",\n";
        first = false;
        text += "\"" + jsonmini::Escape(k) + "\":" + v;
    }
    text += "}\n";
    const std::wstring path = StoragePath(m);
    CreateDirectoryW((game::DataDir() + L"\\mods").c_str(), nullptr);
    CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    const bool ok = f && fwrite(text.data(), 1, text.size(), f) == text.size();
    if (f) fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        SysLog(2, "wum.storage: write failed", m->id, game::Narrow(path));
        m->storageDirty = true;
    }
}
}  // namespace melange::sandbox
