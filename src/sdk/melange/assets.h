#pragma once
#include <cstddef>
#include <cstdint>
namespace melange::assets {
bool Enabled();                              // [Assets] Enabled and the upload/search-path checks passed
// <game>\Mods\<id>\assets\loose as an engine search path, once per launch. Refused if any file in it is not named
// "<id>.*" or collides with a CRC-listed path.
bool AddModRoot(const char* modId, char* err, size_t errLen);
// LoadBank of <mod>\assets\data\<rel>, in the current match scene. Returns the engine result (0 = ok) or -1.
int LoadModBank(const char* modId, const char* rel, char* err, size_t errLen);
// Panel icon: reserves a free atlas-3 sub-icon (9..15, first come in k order) and writes the PNG (64x64, alpha-blended
// over the atlas) into it at every upload of "Weapon Panel Icons3". Returns the iconCode (0x0903..0x0f03).
bool ReservePanelIcon(const char* modId, const char* relPng, uint32_t* iconCode, char* err, size_t errLen);
struct Stats { uint32_t roots, banks, icons, uploadsPatched; double msLastPatch; };
Stats GetStats();
}
