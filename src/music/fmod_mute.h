#pragma once
// Silences the game's FMOD music category for wum.audio.music (Melange 0.12). By exports only: no game address is
// used, so this is not tied to a build. See fmod_mute.cpp.
namespace melange::fmodmute {
// Called from the Audio module's Install. Resolves fmod_event.dll's exports and hooks EventSystem::update. Returns
// false when that is not possible yet; a DLL that is not mapped yet is retried from OnFrame, and Failed() is true
// once it is known to be impossible.
bool Install();
void OnFrame();  // Frame event: the retry above
bool Live();     // the music category was found, so a request has an effect
bool Failed();   // FMOD cannot be driven in this process
}  // namespace melange::fmodmute
