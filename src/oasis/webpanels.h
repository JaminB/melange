#pragma once
// Web panels from C++ modules and Lua mods: registers the protocol-level panel (melange/oasis.h) and serves its
// folder at /ext/<id>/ with a sandboxing CSP. Not a public contract: used by wum_web.cpp and native modules.
namespace melange::oasis::providers {
// `id`/`title`/`dir`/`entry` as melange::oasis::AddWebPanel. 0 on failure (bad id, duplicate, bad dir).
int AddPanel(const char* id, const char* title, const wchar_t* dir, const char* entry = "index.html");
void RemovePanel(int handle);
}  // namespace melange::oasis::providers
