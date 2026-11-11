#pragma once
#include <string>

// The Test probe (levels/probe.cpp): while a Test match plays, where every worm stood at the first turn and, for each
// worm that dies, where, when and what most likely killed it (drowned, a mine, a weapon's explosion, an oil drum, a
// fall), with the explosions that hit it. Each is a record on the session log (levels/test.spawns, levels/test.death)
// and level.test.probe answers them. Read-only: it reads the worm containers, the explosion messages and the entity
// list, and changes nothing the match reads.
namespace melange::levels::probe {
std::string ReportJson();   // {key, playing, turn, spawns, deaths, explosions}; key "" before any Test
}  // namespace melange::levels::probe
