#pragma once
#include "melange/oasis.h"

// ini.get and ini.set on config::Path() (Melange.ini). No game dependency: the standalone server can register them too.
//   ini.get {}                     -> {path, encoding, text, keys: [{section, key, def, live, declared, current, line}]}
//   ini.set {section, key, value}  -> {live, restart, changed}
namespace melange::oasis::rpc {
void IniGet(const Call& c, Result& r, void* user);
void IniSet(const Call& c, Result& r, void* user);
}  // namespace melange::oasis::rpc
