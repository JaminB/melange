#pragma once
#include <string>

#include "oasis/core/server.h"

// The Oasis module calls one Install function per provider after the core is configured. Each owner replaces its
// stub file; the signatures are fixed.
namespace melange::oasis::providers {
void InstallLog(); void InstallBus(); void InstallNet(); void InstallLobby(); void InstallStats();  // streams
void InstallState(); void InstallEntities();                                                      // game state
void InstallLua(); void InstallMods(); void InstallIni();                                         // panels' RPCs
void InstallCapture();                                                                            // capture viewer
void InstallWebPanels();                                                                          // web panels
void InstallWormsign();                                                                           // replays and desync
void InstallLevels();                                                                              // Erg level service, previews
void InstallErgAssetRoute(const std::wstring& gameDir, int cacheMB);                          // both servers
void InstallErgChannel();                                                                         // Test state and level starts
core::Auth* MakeAuth();                                                                           // launch token, Host, Origin
}  // namespace melange::oasis::providers
