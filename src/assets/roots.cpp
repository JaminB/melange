#include "assets/roots.h"

#include <windows.h>

#include "assets/searchpath.h"
#include "core/game.h"
#include "core/log.h"

namespace melange::assets::roots {
namespace {
uint32_t g_count = 0;

// Direct children of dir, split into files and subdirectories. Returns false (not "nothing found") only if dir
// itself doesn't exist.
bool ListEntries(const std::wstring& dir, std::vector<std::string>* files, std::vector<std::string>* dirs) {
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    do {
        if (fd.cFileName == std::wstring(L".") || fd.cFileName == std::wstring(L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs->push_back(game::Narrow(fd.cFileName));
        else files->push_back(game::Narrow(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}
}  // namespace

bool Add(const std::string& modId, const std::wstring& absLooseDir, const std::string& gameRelLooseDir, std::string* err) {
    std::vector<std::string> files, dirs;
    if (!ListEntries(absLooseDir, &files, &dirs)) return true;  // no assets/loose/: nothing to add, not a refusal
    // The naming rule and the engine's bare-name resolution both assume a flat folder: a subdirectory is refused
    // outright rather than silently skipped, since files inside it are never checked against either rule.
    if (!dirs.empty()) {
        if (err) *err = "loose/" + dirs.front() + " is a subdirectory, which is not allowed";
        LOG_ERROR("[assets] %s: %s", modId.c_str(), err ? err->c_str() : "");
        return false;
    }
    if (files.empty()) return true;
    if (!crcsafe::Available()) {
        if (err) *err = "the CRC table could not be verified; refusing to add a search path";
        LOG_ERROR("[assets] %s: %s", modId.c_str(), err ? err->c_str() : "");
        return false;
    }
    if (!CheckNames(modId, files, crcsafe::Entries(), err)) {
        LOG_ERROR("[assets] %s: %s", modId.c_str(), err ? err->c_str() : "");
        return false;
    }
    const size_t before = searchpath::Added().size();
    if (!searchpath::Add(gameRelLooseDir.c_str())) {
        if (err) *err = "adding the search path failed";
        return false;
    }
    if (searchpath::Added().size() > before) ++g_count;
    return true;
}

uint32_t Count() { return g_count; }
}  // namespace melange::assets::roots
