// Engine data containers for wum.sim.getData/setData: the same lookup the engine's GetData binding (0x696b44) makes
// before it can fail, so that a pre-check never lets a bad data id halt the level script.
#include <mutex>

#include "core/log.h"
#include "core/mem.h"
#include "lua/sim/sim_internal.h"

namespace melange::simcore {
namespace {
constexpr uintptr_t kXomRoot = 0x966d98;  // the object 0x639b1d returns (created on demand; we never create it)
constexpr uintptr_t kContainerGuid = 0x888288;

bool Verified() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        // GetData: push 0x888288; ...; call [eax+0x54]; ...; call [eax+0x74]; ...; call [eax+0x10]
        ok = mem::Expect(0x696bfc, {0x68, 0x88, 0x82, 0x88, 0x00}) && mem::Expect(0x696c0f, {0xff, 0x50, 0x54}) &&
             mem::Expect(0x696c2e, {0xff, 0x50, 0x74}) && mem::Expect(0x696d0f, {0xff, 0x50, 0x10}) &&
             mem::Expect(0x639b1d, {0x83, 0x3d, 0x98, 0x6d, 0x96, 0x00, 0x00});
        if (!ok) LOG_WARN("[sim] GetData layout differs: wum.sim.getData/setData are off");
    });
    return ok;
}

using Fn1 = uintptr_t(__stdcall*)(uintptr_t self);
using Fn2 = uintptr_t(__stdcall*)(uintptr_t self, uintptr_t arg);
using Fn3 = int(__stdcall*)(uintptr_t self, const char** name, uintptr_t* out);

uintptr_t Slot(uintptr_t obj, size_t off) { return *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(obj) + off); }
}  // namespace

int DataType(const char* name) {
    if (!name || !Verified()) return -1;
    const uintptr_t root = *reinterpret_cast<uintptr_t*>(kXomRoot);
    if (!root) return -1;
    const uintptr_t svc = reinterpret_cast<Fn2>(Slot(root, 0x54))(root, kContainerGuid);
    if (!svc) return -1;
    uintptr_t obj = 0;
    const char* n = name;
    const int rc = reinterpret_cast<Fn3>(Slot(svc, 0x74))(svc, &n, &obj);
    int type = -1;
    if (rc >= 0 && obj) type = static_cast<int>(reinterpret_cast<Fn1>(Slot(obj, 0x10))(obj));
    if (obj) reinterpret_cast<Fn1>(Slot(obj, 0x8))(obj);
    return type;
}
}  // namespace melange::simcore
