#pragma once
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
void InstallLevels(); void InstallErgAssets();                                                    // Erg level service, previews
void InstallErgChannel();                                                                         // Test state and level starts
core::Auth* MakeAuth();                                                                           // launch token, Host, Origin
}  // namespace melange::oasis::providers
