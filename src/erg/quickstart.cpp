#include "erg/quickstart.h"

#include <windows.h>

#include <cstring>

#include "core/game.h"
#include "core/mem.h"
#include "levels/engine.h"
#include "melange/bus.h"

// The Quick Game popup posts WXMsg.StartGame("QuickStartHvC") as a StringMessage. The message is
// allocated by the frontend's message factory and posted on the same event bus melange/bus.h observes.
namespace melange::erg::quickstart {
namespace {
constexpr uintptr_t kMsgFactory = 0x96d14c, kMsgAlloc = 0x691705, kStringMsgInit = 0x69151a, kPost = 0x6910e4;
constexpr const char* kStartGame = "WXMsg.StartGame";
constexpr const char* kQuickStart = "QuickStartHvC";

bool BytesOk() {
    return game::IsKnownBuild() && mem::Expect(kMsgAlloc, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x10}) &&
           mem::Expect(kStringMsgInit, {0x55, 0x8b, 0xec, 0x51, 0x89, 0x4d, 0xfc}) &&
           (bus::Installed() || mem::Expect(kPost, {0x55, 0x8b, 0xec, 0x51, 0x51, 0x83, 0x3d, 0x90, 0xd0, 0x96, 0x00, 0x00}));
}

// The text pointer is stored by the message, not copied, so it must outlive the post; static storage does.
char g_text[32] = {};

bool RawPost(uint16_t id, const char* text) {
    __try {
        const uintptr_t factory = *reinterpret_cast<uintptr_t*>(kMsgFactory);
        if (!factory) return false;
        const uintptr_t msg = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, uint32_t)>(kMsgAlloc)(factory, 0xc);
        if (!msg) return false;
        reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, uint32_t, const char*)>(kStringMsgInit)(msg, id, text);
        reinterpret_cast<int(__cdecl*)(uintptr_t)>(kPost)(msg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool Available() { return BytesOk() && bus::RegistryReady() && levels::engine::AtFrontend(); }

bool PostQuickGame() {
    if (!Available()) return false;
    const bus::MsgId id = bus::IdOf(kStartGame);
    if (id == bus::kInvalidId) return false;
    strncpy_s(g_text, kQuickStart, _TRUNCATE);
    return RawPost(id, g_text);
}
}  // namespace melange::erg::quickstart
