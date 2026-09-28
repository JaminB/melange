// wum.unsafe (Deep Desert): raw memory access and native calls for mods the user granted.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "core/game.h"
#include "lua/sandbox_core.h"
#include "melange/jlog.h"

namespace melange::sandbox {
namespace {
constexpr size_t kMaxBytes = 1u << 20;
constexpr lua_Integer kMaxCount = 4096;
constexpr int kMaxArgs = 8;

int g_grantedRef = LUA_NOREF, g_refusedRef = LUA_NOREF;

enum class Ty { U8, I8, U16, I16, U32, I32, F32, F64, Ptr, CStr, Bytes };
struct TyName {
    const char* name;
    Ty ty;
    size_t size;
};
constexpr TyName kTypes[] = {{"u8", Ty::U8, 1},   {"i8", Ty::I8, 1},   {"u16", Ty::U16, 2}, {"i16", Ty::I16, 2},
                             {"u32", Ty::U32, 4}, {"i32", Ty::I32, 4}, {"f32", Ty::F32, 4}, {"f64", Ty::F64, 8},
                             {"ptr", Ty::Ptr, 4}, {"cstr", Ty::CStr, 1}, {"bytes", Ty::Bytes, 1}};

bool SehCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copies up to max bytes stopping at NUL; returns false on a fault.
bool SehCStr(const char* src, char* dst, size_t max, size_t* len) {
    size_t n = 0;
    __try {
        while (n < max && src[n]) {
            dst[n] = src[n];
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    *len = n;
    return true;
}

enum class Conv { Cdecl, Stdcall, Thiscall };
enum class Ret { Int, F32, F64 };
struct CallReq {
    uintptr_t fn;
    Conv conv;
    Ret ret;
    int n;
    uint32_t a[kMaxArgs + 1];
    uint32_t eax;
    double fp;
};

template <size_t>
using U32 = uint32_t;

template <class R, size_t... I>
R Cdecl(uintptr_t f, const uint32_t* a, std::index_sequence<I...>) {
    return reinterpret_cast<R(__cdecl*)(U32<I>...)>(f)(a[I]...);
}
template <class R, size_t... I>
R Stdcall(uintptr_t f, const uint32_t* a, std::index_sequence<I...>) {
    return reinterpret_cast<R(__stdcall*)(U32<I>...)>(f)(a[I]...);
}
// __thiscall is not allowed on free function pointers; __fastcall with a dummy edx has the same ecx/stack layout.
template <class R, size_t... I>
R Thiscall(uintptr_t f, const uint32_t* a, std::index_sequence<I...>) {
    return reinterpret_cast<R(__fastcall*)(uint32_t, uint32_t, U32<I>...)>(f)(a[0], 0, a[I + 1]...);
}

template <class R, size_t N>
R Dispatch(const CallReq* r) {
    switch (r->conv) {
        case Conv::Cdecl: return Cdecl<R>(r->fn, r->a, std::make_index_sequence<N>{});
        case Conv::Stdcall: return Stdcall<R>(r->fn, r->a, std::make_index_sequence<N>{});
        case Conv::Thiscall:
            if constexpr (N >= 1) return Thiscall<R>(r->fn, r->a, std::make_index_sequence<N - 1>{});
            break;
    }
    return R{};
}

template <class R>
R ByCount(const CallReq* r) {
    switch (r->n) {
        case 0: return Dispatch<R, 0>(r);
        case 1: return Dispatch<R, 1>(r);
        case 2: return Dispatch<R, 2>(r);
        case 3: return Dispatch<R, 3>(r);
        case 4: return Dispatch<R, 4>(r);
        case 5: return Dispatch<R, 5>(r);
        case 6: return Dispatch<R, 6>(r);
        case 7: return Dispatch<R, 7>(r);
        default: return Dispatch<R, 8>(r);
    }
}

void DoCall(CallReq* r) {
    switch (r->ret) {
        case Ret::Int: r->eax = ByCount<uint32_t>(r); break;
        case Ret::F32: r->fp = ByCount<float>(r); break;
        case Ret::F64: r->fp = ByCount<double>(r); break;
    }
}

bool SehCall(CallReq* r, DWORD* code) {
    __try {
        DoCall(r);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Record(jlog::Level lvl, const char* msg, const char* fn, uintptr_t addr) {
    ModRec* m = Current();
    jlog::Rec r("thumper", lvl, msg);
    r.Str("mod", m ? m->id : "console").Str("fn", fn);
    if (addr) r.Hex("addr", addr);
    r.Emit();
}

ModRec* NeedGrant(lua_State* L, const char* fn) {
    ModRec* m = Current();
    if (!m || !m->granted) {
        Record(jlog::Level::Warn, "wum.unsafe refused", fn, 0);
        luaL_error(L, "wum.unsafe.%s: Deep Desert not granted", fn);
    }
    return m;
}

uintptr_t CheckAddr(lua_State* L, int idx) {
    const lua_Integer a = luaL_checkinteger(L, idx);
    if (a < 0 || a > 0xffffffffLL) luaL_argerror(L, idx, "address out of range");
    return static_cast<uintptr_t>(a);
}

const TyName& CheckType(lua_State* L, int idx) {
    const char* s = luaL_checkstring(L, idx);
    for (const TyName& t : kTypes)
        if (strcmp(t.name, s) == 0) return t;
    luaL_argerror(L, idx, "type must be u8 i8 u16 i16 u32 i32 f32 f64 ptr cstr bytes");
    return kTypes[0];
}

[[noreturn]] void Fault(lua_State* L, const char* fn, uintptr_t addr) {
    Record(jlog::Level::Error, "wum.unsafe fault", fn, addr ? addr : 1);
    char msg[96];
    snprintf(msg, sizeof(msg), "wum.unsafe.%s: access violation at 0x%08x", fn, static_cast<unsigned>(addr));
    luaL_error(L, "%s", msg);
    std::abort();
}

void PushScalar(lua_State* L, Ty t, const uint8_t* p) {
    switch (t) {
        case Ty::U8: lua_pushinteger(L, *p); break;
        case Ty::I8: lua_pushinteger(L, static_cast<int8_t>(*p)); break;
        case Ty::U16: { uint16_t v; memcpy(&v, p, 2); lua_pushinteger(L, v); break; }
        case Ty::I16: { int16_t v; memcpy(&v, p, 2); lua_pushinteger(L, v); break; }
        case Ty::U32:
        case Ty::Ptr: { uint32_t v; memcpy(&v, p, 4); lua_pushinteger(L, v); break; }
        case Ty::I32: { int32_t v; memcpy(&v, p, 4); lua_pushinteger(L, v); break; }
        case Ty::F32: { float v; memcpy(&v, p, 4); lua_pushnumber(L, v); break; }
        case Ty::F64: { double v; memcpy(&v, p, 8); lua_pushnumber(L, v); break; }
        default: lua_pushnil(L);
    }
}

int Read(lua_State* L) {
    NeedGrant(L, "read");
    const uintptr_t addr = CheckAddr(L, 1);
    const TyName& t = CheckType(L, 2);
    if (t.ty == Ty::Bytes) {
        const lua_Integer n = luaL_checkinteger(L, 3);
        if (n < 0 || n > static_cast<lua_Integer>(kMaxBytes)) return luaL_argerror(L, 3, "length out of range");
        std::string buf(static_cast<size_t>(n), '\0');
        if (!SehCopy(buf.data(), reinterpret_cast<const void*>(addr), buf.size())) Fault(L, "read", addr);
        lua_pushlstring(L, buf.data(), buf.size());
        return 1;
    }
    if (t.ty == Ty::CStr) {
        const lua_Integer n = luaL_optinteger(L, 3, 256);
        if (n < 0 || n > 65536) return luaL_argerror(L, 3, "length out of range");
        std::string buf(static_cast<size_t>(n), '\0');
        size_t len = 0;
        if (!SehCStr(reinterpret_cast<const char*>(addr), buf.data(), buf.size(), &len)) Fault(L, "read", addr);
        lua_pushlstring(L, buf.data(), len);
        return 1;
    }
    const lua_Integer count = luaL_optinteger(L, 3, 1);
    if (count < 1 || count > kMaxCount) return luaL_argerror(L, 3, "count out of range");
    std::string buf(t.size * static_cast<size_t>(count), '\0');
    if (!SehCopy(buf.data(), reinterpret_cast<const void*>(addr), buf.size())) Fault(L, "read", addr);
    const auto* p = reinterpret_cast<const uint8_t*>(buf.data());
    if (count == 1) {
        PushScalar(L, t.ty, p);
        return 1;
    }
    lua_createtable(L, static_cast<int>(count), 0);
    for (lua_Integer i = 0; i < count; ++i) {
        PushScalar(L, t.ty, p + static_cast<size_t>(i) * t.size);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int Write(lua_State* L) {
    NeedGrant(L, "write");
    const uintptr_t addr = CheckAddr(L, 1);
    const TyName& t = CheckType(L, 2);
    uint8_t scalar[8];
    const void* src = scalar;
    size_t n = t.size;
    std::string str;
    switch (t.ty) {
        case Ty::U8:
        case Ty::I8: { const auto v = static_cast<uint8_t>(luaL_checkinteger(L, 3)); memcpy(scalar, &v, 1); break; }
        case Ty::U16:
        case Ty::I16: { const auto v = static_cast<uint16_t>(luaL_checkinteger(L, 3)); memcpy(scalar, &v, 2); break; }
        case Ty::U32:
        case Ty::I32:
        case Ty::Ptr: { const auto v = static_cast<uint32_t>(luaL_checkinteger(L, 3)); memcpy(scalar, &v, 4); break; }
        case Ty::F32: { const auto v = static_cast<float>(luaL_checknumber(L, 3)); memcpy(scalar, &v, 4); break; }
        case Ty::F64: { const auto v = static_cast<double>(luaL_checknumber(L, 3)); memcpy(scalar, &v, 8); break; }
        case Ty::CStr:
        case Ty::Bytes: {
            size_t len;
            const char* s = luaL_checklstring(L, 3, &len);
            if (len > kMaxBytes) return luaL_argerror(L, 3, "too long");
            str.assign(s, len);
            if (t.ty == Ty::CStr) str.push_back('\0');
            src = str.data();
            n = str.size();
            break;
        }
    }
    if (!SehCopy(reinterpret_cast<void*>(addr), src, n)) Fault(L, "write", addr);
    lua_pushboolean(L, 1);
    return 1;
}

int Call(lua_State* L) {
    NeedGrant(L, "call");
    CallReq r{};
    r.fn = CheckAddr(L, 1);
    std::string conv = luaL_checkstring(L, 2);
    const size_t colon = conv.find(':');
    const std::string ret = colon == std::string::npos ? "" : conv.substr(colon + 1);
    conv = conv.substr(0, colon);
    if (conv == "cdecl") r.conv = Conv::Cdecl;
    else if (conv == "stdcall") r.conv = Conv::Stdcall;
    else if (conv == "thiscall") r.conv = Conv::Thiscall;
    else return luaL_argerror(L, 2, "convention must be cdecl, stdcall or thiscall (optionally :f32 or :f64)");
    if (ret.empty() || ret == "u32" || ret == "i32" || ret == "ptr") r.ret = Ret::Int;
    else if (ret == "f32") r.ret = Ret::F32;
    else if (ret == "f64") r.ret = Ret::F64;
    else return luaL_argerror(L, 2, "return type must be f32, f64 or omitted");
    r.n = lua_gettop(L) - 2;
    if (r.n > kMaxArgs) return luaL_error(L, "wum.unsafe.call: at most %d arguments", kMaxArgs);
    if (r.conv == Conv::Thiscall && r.n < 1) return luaL_error(L, "wum.unsafe.call: thiscall needs the object pointer first");
    for (int i = 0; i < r.n; ++i) {
        const int idx = i + 3;
        switch (lua_type(L, idx)) {
            case LUA_TNIL: r.a[i] = 0; break;
            case LUA_TBOOLEAN: r.a[i] = lua_toboolean(L, idx) ? 1u : 0u; break;
            case LUA_TSTRING: r.a[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lua_tostring(L, idx))); break;
            case LUA_TNUMBER:
                if (lua_isinteger(L, idx)) {
                    r.a[i] = static_cast<uint32_t>(lua_tointeger(L, idx));
                } else {
                    const float f = static_cast<float>(lua_tonumber(L, idx));
                    memcpy(&r.a[i], &f, 4);
                }
                break;
            default: return luaL_argerror(L, idx, "integer, float, boolean, string or nil expected");
        }
    }
    DWORD code = 0;
    if (!SehCall(&r, &code)) {
        Record(jlog::Level::Error, "wum.unsafe fault", "call", r.fn ? r.fn : 1);
        char msg[96];
        snprintf(msg, sizeof(msg), "wum.unsafe.call: exception 0x%08x in the call to 0x%08x", static_cast<unsigned>(code),
                 static_cast<unsigned>(r.fn));
        return luaL_error(L, "%s", msg);
    }
    if (r.ret == Ret::Int) lua_pushinteger(L, r.eax);
    else lua_pushnumber(L, r.fp);
    return 1;
}

int Base(lua_State* L) {
    NeedGrant(L, "base");
    lua_pushinteger(L, static_cast<lua_Integer>(game::Base()));
    return 1;
}

int Build(lua_State* L) {
    NeedGrant(L, "build");
    lua_pushstring(L, game::Exe().build);
    lua_pushboolean(L, game::Exe().known);
    return 2;
}

int Refused(lua_State* L) {
    const char* fn = lua_tostring(L, lua_upvalueindex(1));
    Record(jlog::Level::Warn, "wum.unsafe refused", fn, 0);
    return luaL_error(L, "wum.unsafe.%s: Deep Desert not granted", fn);
}

constexpr luaL_Reg kFns[] = {{"read", Read}, {"write", Write}, {"call", Call}, {"base", Base}, {"build", Build}, {nullptr, nullptr}};

void Shared(lua_State* L, int) {
    lua_newtable(L);
    RegisterFunctions(L, -1, kFns);
    PushFrozen(L, -1);
    g_grantedRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    lua_newtable(L);
    for (const luaL_Reg* f = kFns; f->name; ++f) {
        lua_pushstring(L, f->name);
        lua_pushcclosure(L, &Refused, 1);
        lua_setfield(L, -2, f->name);
    }
    PushFrozen(L, -1);
    g_refusedRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
}

void PerEnv(lua_State* L, int wum, ModRec* m) {
    if (!m || !m->unsafe) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, m->granted ? g_grantedRef : g_refusedRef);
    lua_setfield(L, wum, "unsafe");
}

const LibRegistrar g_reg(&Shared, &PerEnv);
}  // namespace
}  // namespace melange::sandbox
