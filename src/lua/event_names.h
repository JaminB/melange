#pragma once
// Client event names added in Melange 0.12.0 (wum.events.on). Every emitter uses these constants so a name is spelled
// once; docs/lua-api.md lists each one with its payload. Owner-only events go through sandbox::QueueEventFor or
// melange::lua::PostEventTo; the others reach every listener.
namespace melange::sandbox::ev {
// wum.media (owner only). Payload {type=...}: "loaded" {title, duration} | "ended" {reason} | "error" {code, message}
// | "buffering" {on} | "tracks" {} | "helper" {state} | "pack" {state, progress} (at most 4 Hz) | "torrent" {infohash, files?}
inline constexpr const char kMedia[] = "melange.media";

// wum.lobby (every listener)
inline constexpr const char kLobbyEnter[] = "melange.lobby.enter";      // {isHost}
inline constexpr const char kLobbyLeave[] = "melange.lobby.leave";      // {}
inline constexpr const char kLobbyMembers[] = "melange.lobby.members";  // {}
inline constexpr const char kLobbyOwner[] = "melange.lobby.owner";      // {owner, isHost}
inline constexpr const char kLobbyPhase[] = "melange.lobby.phase";      // {phase}

// wum.input (owner only)
inline constexpr const char kInputMouse[] = "melange.input.mouse";  // {region, button, down, x, y, double}
inline constexpr const char kInputWheel[] = "melange.input.wheel";  // {region, delta, x, y}
inline constexpr const char kInputKey[] = "melange.input.key";      // {key, down, repeat, ctrl, shift, alt} (focus only)
inline constexpr const char kInputChar[] = "melange.input.char";    // {text} (focus only)
inline constexpr const char kInputPaste[] = "melange.input.paste";  // {text} (focus only, Ctrl+V)

// wum.game (every listener)
inline constexpr const char kTurn[] = "melange.turn";  // {index, team, worm, mine, ai, online}
inline constexpr const char kUi[] = "melange.ui";      // {weaponPanel, pause, readyIntro}
}  // namespace melange::sandbox::ev
