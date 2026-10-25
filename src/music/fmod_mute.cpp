// FMOD music mute. Every music cue of the game (the match themes, sudden death, victory, the frontend music, and the
// Melange/Kanly sudden-death bank, which is a different bank behind the same event) lives in the FEV category "music",
// so muting that one category silences all of it and nothing else (sound effects, speech and ambience are siblings).
// Only mute and volume are touched, never pause or stop, so the music timeline that game logic may wait on runs
// unchanged, and the saved music volume of the player is never written.
//
// The FMOD::EventSystem is captured by an inline hook on the exported EventSystem::update, which the game calls once
// per frame on its main thread; the category is applied from inside it, on the thread that makes every other FMOD call.
// The level (1 audible .. 0 silent) comes from audio::MusicLevel; this file only turns it into FMOD calls:
//  - a fade ramps the category volume from the volume it had (v0) to 0, then sets the mute flag and puts v0 back, so
//    a change of the volume by the user while it is muted is never overwritten (it is not touched while muted);
//  - fading back in clears the mute with the volume at 0 and ramps up to v0.
// As insurance the interactive MusicSystem is muted too, in case a cue plays through it instead of the event category.
#include "music/fmod_mute.h"

#include <safetyhook.hpp>
#include <windows.h>

#include "audio/audio.h"
#include "core/log.h"
#include "melange/jlog.h"

namespace melange::fmodmute {
namespace {
using Result = int;  // FMOD_RESULT, 0 = FMOD_OK
using GetCategoryFn = Result(__stdcall*)(void* es, const char* name, void** cat);
using GetMusicSystemFn = Result(__stdcall*)(void* es, void** ms);
using SetMuteFn = Result(__stdcall*)(void* obj, int mute);
using GetMuteFn = Result(__stdcall*)(void* obj, int* mute);
using SetVolumeFn = Result(__stdcall*)(void* obj, float v);
using GetVolumeFn = Result(__stdcall*)(void* obj, float* v);

constexpr int kCheckEvery = 60;  // updates between GetMute checks while muted

GetCategoryFn pGetCategory;
GetMusicSystemFn pGetMusicSystem;
SetMuteFn pCatSetMute, pMsSetMute;
GetMuteFn pCatGetMute;
SetVolumeFn pCatSetVolume;
GetVolumeFn pCatGetVolume;

SafetyHookInline g_hook;
bool g_installed = false, g_failed = false, g_live = false;

// What has been done to FMOD, so that an idle game costs nothing.
bool g_haveV0 = false, g_volDirty = false, g_muteSet = false, g_msMuted = false;
float g_v0 = 1.0f;
int g_sinceCheck = 0;

template <class T>
T Export(HMODULE m, const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

bool Resolve(HMODULE m) {
    pGetCategory = Export<GetCategoryFn>(m, "_FMOD_EventSystem_GetCategory@12");
    pGetMusicSystem = Export<GetMusicSystemFn>(m, "_FMOD_EventSystem_GetMusicSystem@8");
    pCatSetMute = Export<SetMuteFn>(m, "_FMOD_EventCategory_SetMute@8");
    pCatGetMute = Export<GetMuteFn>(m, "_FMOD_EventCategory_GetMute@8");
    pCatSetVolume = Export<SetVolumeFn>(m, "_FMOD_EventCategory_SetVolume@8");
    pCatGetVolume = Export<GetVolumeFn>(m, "_FMOD_EventCategory_GetVolume@8");
    pMsSetMute = Export<SetMuteFn>(m, "_FMOD_MusicSystem_SetMute@8");  // optional
    return pGetCategory && pCatSetMute && pCatGetMute && pCatSetVolume && pCatGetVolume;
}

// The category by the name the game itself asks for ("Music", 0x6f9740), then the FEV's own spelling. Fetched on every
// use, so a project reload (which frees the categories) can never leave a dangling pointer.
void* Category(void* es) {
    void* cat = nullptr;
    if (pGetCategory(es, "Music", &cat) == 0 && cat) return cat;
    cat = nullptr;
    if (pGetCategory(es, "music", &cat) == 0 && cat) return cat;
    return nullptr;
}

// One update's worth of work. Plain code in an SEH frame (no C++ objects): a bad pointer from FMOD must not take the
// game down; a fault turns the feature off for the session.
void Apply(void* es, float level, bool* live) {
    void* cat = Category(es);
    *live = cat != nullptr;
    const bool engaged = g_haveV0 || g_muteSet || g_msMuted;
    if (!cat || (level >= 1.0f && !engaged)) return;  // idle: nothing was touched, nothing to do

    if (level < 1.0f && !g_haveV0) {
        float v = 1.0f;
        if (pCatGetVolume(cat, &v) != 0 || !(v >= 0.0f && v <= 8.0f)) v = 1.0f;
        g_v0 = v;
        g_haveV0 = true;
    }
    if (level >= 1.0f) {  // audible again: put everything back
        if (g_volDirty) pCatSetVolume(cat, g_v0);
        if (g_muteSet) pCatSetMute(cat, 0);
        g_volDirty = g_muteSet = g_haveV0 = false;
    } else if (level > 0.0f) {  // fading: audible at a scaled volume
        pCatSetVolume(cat, g_v0 * level);
        g_volDirty = true;
        if (g_muteSet) {
            pCatSetMute(cat, 0);
            g_muteSet = false;
        }
    } else {  // silent: the mute flag, with the volume back where it was
        if (!g_muteSet) {
            pCatSetMute(cat, 1);
            g_muteSet = true;
            g_sinceCheck = 0;
        }
        if (g_volDirty) {
            pCatSetVolume(cat, g_v0);
            g_volDirty = false;
        }
        g_haveV0 = false;  // while muted the volume is the user's: the next fade reads it afresh
        if (++g_sinceCheck >= kCheckEvery) {  // something cleared it (a project reload recreates the categories)
            g_sinceCheck = 0;
            int m = 0;
            if (pCatGetMute(cat, &m) == 0 && !m) pCatSetMute(cat, 1);
        }
    }
    if (pMsSetMute && pGetMusicSystem) {
        const bool wantMs = level <= 0.0f;
        if (wantMs != g_msMuted) {
            void* ms = nullptr;
            if (pGetMusicSystem(es, &ms) == 0 && ms) {
                pMsSetMute(ms, wantMs ? 1 : 0);
                g_msMuted = wantMs;
            }
        }
    }
}

bool SafeApply(void* es, float level, bool* live) {
    __try {
        Apply(es, level, live);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

Result __stdcall HkUpdate(void* es) {
    if (!g_failed && es) {
        bool live = false;
        if (!SafeApply(es, audio::MusicLevel(), &live)) {
            g_failed = true;
            g_live = false;
            LOG_ERROR("[audio] the FMOD music mute faulted; wum.audio.music is off for this session");
            jlog::Rec("audio", jlog::Level::Error, "music_mute_fault").Emit();
        } else {
            g_live = live;
        }
    }
    return g_hook.stdcall<Result>(es);
}

bool TryHook() {
    HMODULE m = GetModuleHandleW(L"fmod_event.dll");
    if (!m) return false;
    if (!Resolve(m)) {
        g_failed = true;
        LOG_WARN("[audio] fmod_event.dll lacks the category exports; wum.audio.music is unavailable");
        return false;
    }
    void* upd = reinterpret_cast<void*>(GetProcAddress(m, "?update@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@XZ"));
    if (!upd) {
        g_failed = true;
        LOG_WARN("[audio] FMOD::EventSystem::update is not exported; wum.audio.music is unavailable");
        return false;
    }
    g_hook = safetyhook::create_inline(upd, reinterpret_cast<void*>(&HkUpdate));
    if (!g_hook) {
        g_failed = true;
        LOG_WARN("[audio] hooking FMOD::EventSystem::update failed; wum.audio.music is unavailable");
        return false;
    }
    g_installed = true;
    LOG_INFO("[audio] FMOD music mute hooked (EventSystem::update at %p)", upd);
    return true;
}
}  // namespace

bool Install() {
    if (g_installed || g_failed) return g_installed;
    return TryHook();  // not mapped yet: OnFrame tries again
}

void OnFrame() {
    if (!g_installed && !g_failed) TryHook();
}

bool Live() { return g_installed && !g_failed && g_live; }
bool Failed() { return g_failed; }
}  // namespace melange::fmodmute
