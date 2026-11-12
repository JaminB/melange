#pragma once
#include <string>
#include <utility>
#include <vector>

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
void InstallErgChannel(); void InstallLevelTest();                                           // Test state, level starts, level.test
void InstallRenderShot(); void InstallLevelTestOps();                                      // render.camera/shot; level.test.explode/quit/fireFactory/probe
void InstallInputDrive();                                                                         // input.drive (the Controls module's drive)
bool BuildTestLevel(const std::string& project, const std::string& tod, uint64_t conn, std::string* stem, std::string* title,
                    std::string* err);
// The mesh banks a project carries for a Test: <projects>\<project>\meshes\*.xom (at most 8, by name), each with the
// mod id its mesh names start with (the file name up to its first dot: dustcrown.Rocks.xom -> dustcrown). An installed
// pack loads its banks from its spice.json at launch; a Test of the project loads these (level.test).
std::vector<std::pair<std::string, std::string>> ProjectMeshBanks(const std::string& project);   // server thread; tod "" keeps the project's time of day; leased to conn
core::Auth* MakeAuth();                                                                           // launch token, Host, Origin
}  // namespace melange::oasis::providers
