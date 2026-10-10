// Mod mesh banks through the engine's XGraphicalResourceManager (GRM). Build #1077 addresses; see docs/meshes.md.
//
// How the engine does it (AppDataService::RegisterResources 0x4d4220 at startup, then on demand):
//   GRM->AddMeshDescriptors(table, count)        vtable slot 10 = 0x6ad510: one XMeshDescriptor *stub* per 20-byte
//                                                record {name, section u16, sceneBin u8, sourceFile, flags u16, 0},
//                                                inserted by name into the GRM's 7500-slot hash table (0x6ad270).
//   GRM->CreateResource(name, ...)               slot 35 = 0x6aeac0: FindResourceIndex (0x6adc60); if the stub is not
//                                                loaded, "Auto loading section N" -> LoadSection.
//   GRM->LoadSection(&section)                   slot 13 = 0x6aefd0 -> 0x6ae6a0: sprintf(GRM+0x7dd0 format string
//                                                "Bundl%.2d.xom", section), XomLoadObject (0x63e107), root must QI to
//                                                XGraphSet; for every entry the name must already resolve to a stub
//                                                of the same section and type ("Bundle contains unrecognised
//                                                resource" otherwise), then the loaded descriptor adopts the stub's
//                                                sceneBin/sourceFile (0x6b3500) and replaces it in the table.
// So a mod bank is: register stubs for its names (step 1), point the format string at the bank, LoadSection, and
// put the format string back (step 2). Nothing is hooked or patched; every call is a public GRM method.
#include "assets/meshbank.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "weapons/engine.h"

namespace melange::assets::meshes {
namespace {
constexpr uintptr_t kXomGetApp = 0x639b1d, kGrmIid = 0x888e8c, kGrmVtable = 0x88948c;
constexpr uintptr_t kAddMeshDescriptors = 0x6ad510, kLoadSection = 0x6aefd0, kFindResource = 0x6aec90;
constexpr uintptr_t kBundleLoad = 0x6ae6a0, kFindIndex = 0x6adc60, kInitFromRecord = 0x6b2550;
constexpr int kSlotQI = 0x54 / 4, kSlotAddMesh = 10, kSlotLoadSection = 13, kSlotFind = 36;
constexpr uintptr_t kFormatString = 0x7dd0;                 // GRM+0x7dd0: XString, the bundle file-name format
constexpr uintptr_t kSectionLoaded = 0x96f368;               // byte[520], 1 = section loaded (or load attempted)
constexpr uintptr_t kSectionInstances = 0x96f570;            // int[520], live mesh instances per section
constexpr size_t kMaxBankBytes = 64u << 20;

// XMeshDescriptor stub record (0x14 bytes), the layout XMeshDescriptor::InitFromRecord (0x6b2550) reads:
// +0 name -> XString at desc+0x14; +4 -> desc+0x18; +6 -> desc+0x1a; +8 -> desc+0x20 (raw pointer, kept);
// +0xc -> desc+0x24; +0x10 -> desc+0x2c.
#pragma pack(push, 1)
struct Record {
    const char* name;
    uint16_t section;
    uint8_t sceneBin;
    uint8_t pad;
    const char* sourceFile;
    uint16_t flags;
    uint16_t pad2;
    uint32_t extra;
};
#pragma pack(pop)
static_assert(sizeof(Record) == 0x14, "GRM mesh record is 20 bytes");

struct Site {
    uintptr_t addr;
    std::initializer_list<int> bytes;
};
// First bytes of every function this file calls or relies on, from build #1077.
const Site kSites[] = {
    {kXomGetApp, {0x83, 0x3d, 0x98, 0x6d, 0x96, 0x00, 0x00}},
    {kAddMeshDescriptors, {0x51, 0x8b, 0x44, 0x24, 0x10, 0x53, 0x8b, 0x5c, 0x24, 0x0c}},
    {kLoadSection, {0x56, 0x8b, 0x74, 0x24, 0x0c, 0x0f, 0xb7, 0x0e, 0x80, 0xb9}},
    {kFindResource, {0x8b, 0x4c, 0x24, 0x08, 0x8b, 0x11, 0x56, 0x8b, 0x74, 0x24}},
    {kBundleLoad, {0x83, 0xec, 0x38, 0x53, 0x55, 0x56, 0x57, 0x33, 0xed, 0x80}},
    {kFindIndex, {0x83, 0xec, 0x08, 0x53, 0x8b, 0x5c, 0x24, 0x10, 0x8b, 0xc3}},
    {kInitFromRecord, {0x56, 0x8b, 0x74, 0x24, 0x08, 0x57, 0x8b, 0x7c, 0x24, 0x10}},
};

// Names and source-file strings handed to the engine live for the rest of the process: the stub keeps the
// sourceFile pointer raw (desc+0x20) and the loaded descriptor copies it over.
std::deque<std::string> g_strings;
uint32_t g_banks = 0, g_stubs = 0;
std::map<std::string, std::string> g_nodesMissing;  // loaded mesh -> VehicleNodes() it lacks, "" if none

template <class T>
T Rd(uintptr_t a) {
    T v{};
    mem::SafeRead(a, &v, sizeof(T));
    return v;
}
uintptr_t Slot(uintptr_t obj, int slot) { return Rd<uintptr_t>(Rd<uintptr_t>(obj) + slot * 4); }

bool SitesOk() {
    if (!game::IsKnownBuild()) return false;
    for (auto& s : kSites)
        if (!mem::Expect(s.addr, s.bytes)) return false;
    return true;
}

uintptr_t RawGrm() {
    __try {
        const uintptr_t app = reinterpret_cast<uintptr_t(__cdecl*)()>(kXomGetApp)();
        if (!app) return 0;
        auto qi = reinterpret_cast<uintptr_t(__stdcall*)(uintptr_t, uintptr_t)>(Slot(app, kSlotQI));
        return qi ? qi(app, kGrmIid) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The GRM, or 0 unless its vtable is XGraphicalResourceManager's and the three slots we call are the functions
// whose bytes were just verified.
uintptr_t Grm() {
    if (!SitesOk()) return 0;
    const uintptr_t grm = RawGrm();
    if (!grm || Rd<uintptr_t>(grm) != kGrmVtable) return 0;
    if (Slot(grm, kSlotAddMesh) != kAddMeshDescriptors || Slot(grm, kSlotLoadSection) != kLoadSection ||
        Slot(grm, kSlotFind) != kFindResource)
        return 0;
    return grm;
}

uintptr_t RawFind(uintptr_t grm, const char* name) {
    __try {
        return reinterpret_cast<uintptr_t(__stdcall*)(uintptr_t, const char**)>(kFindResource)(grm, &name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int RawAddMeshes(uintptr_t grm, const Record* recs, uint32_t count) {
    __try {
        return reinterpret_cast<int(__stdcall*)(uintptr_t, const Record*, uint32_t)>(kAddMeshDescriptors)(grm, recs, count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int RawLoadSection(uintptr_t grm, uint16_t section) {
    __try {
        return reinterpret_cast<int(__stdcall*)(uintptr_t, uint16_t*)>(kLoadSection)(grm, &section);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool ReadAll(const std::wstring& path, std::vector<uint8_t>* out, std::string* err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        if (err) *err = "cannot read " + game::Narrow(path);
        return false;
    }
    const std::streamoff n = f.tellg();
    if (n <= 0 || static_cast<uint64_t>(n) > kMaxBankBytes) {
        if (err) *err = "the bank is empty or larger than 64 MiB";
        return false;
    }
    f.seekg(0);
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// First section in the mod range the engine has not loaded (or tried to load); 0 when none is left.
uint16_t FirstFreeSection() {
    for (uint16_t s = kSectionMin; s <= kSectionMax; ++s)
        if (Rd<uint8_t>(kSectionLoaded + s) == 0) return s;
    return 0;
}

bool IEquals(wchar_t a, wchar_t b) { return towlower(a) == towlower(b); }

// "<game>\Mods\x\y.xom" -> "Mods/x/y.xom" (what the engine's search-path file open takes, as LoadBank already
// proves); any other absolute path is passed through unchanged.
std::string EnginePath(const std::wstring& abs) {
    std::wstring game = game::GameDir();
    while (!game.empty() && (game.back() == L'\\' || game.back() == L'/')) game.pop_back();
    std::wstring rel;
    if (abs.size() > game.size() + 1 && (abs[game.size()] == L'\\' || abs[game.size()] == L'/') &&
        std::equal(game.begin(), game.end(), abs.begin(), IEquals))
        rel = abs.substr(game.size() + 1);
    else
        rel = abs;
    for (auto& c : rel)
        if (c == L'\\') c = L'/';
    return game::Narrow(rel);
}

bool Fail(std::string* err, const std::string& msg) {
    if (err) *err = msg;
    LOG_ERROR("[meshes] %s", msg.c_str());
    return false;
}
}  // namespace

bool Available() { return Grm() != 0; }

bool Resolves(const char* resourceName) {
    if (!resourceName || !*resourceName) return false;
    const uintptr_t grm = Grm();
    return grm && RawFind(grm, resourceName) != 0;
}

bool Describe(const char* resourceName, Info* out) {
    if (!resourceName || !*resourceName) return false;
    const uintptr_t grm = Grm();
    const uintptr_t d = grm ? RawFind(grm, resourceName) : 0;
    if (!d) return false;
    if (out) {
        out->descriptor = d;
        out->section = Rd<uint16_t>(d + 0x18);
        out->sceneBin = Rd<uint8_t>(d + 0x1a);
        out->loaded = (Rd<uint8_t>(d + 0x1b) & 1) != 0;
        out->graphSet = Rd<uintptr_t>(d + 0x28);
        out->sourceFile = weapons::engine::ReadCString(Rd<uintptr_t>(d + 0x20), 128);
    }
    return true;
}

bool LoadModBank(const std::wstring& absPath, const std::string& modId, std::string* err, uint8_t sceneBin, uint16_t* sectionUsed,
                 bool relocate) {
    const uintptr_t grm = Grm();
    if (!grm) return Fail(err, "the graphical resource manager's code is not build #1077's (or the game is not up): refused");

    std::vector<uint8_t> bytes;
    std::vector<BankEntry> entries;
    uint16_t section = 0;
    std::string e;
    std::wstring loadPath = absPath;
    if (!ReadAll(absPath, &bytes, &e) || !InspectBank(bytes, &entries, &section, &e))
        return Fail(err, modId + ": " + game::Narrow(absPath) + ": " + e);
    if (relocate && (section < kSectionMin || section > kSectionMax || Rd<uint8_t>(kSectionLoaded + section) != 0)) {
        const uint16_t free = FirstFreeSection();
        std::vector<uint8_t> moved;
        if (!free)
            return Fail(err, modId + ": no free mod section left, all " + std::to_string(kSectionMax - kSectionMin + 1) + " (" + std::to_string(kSectionMin) + ".." + std::to_string(kSectionMax) + ") are in use");
        if (!RelocateBank(bytes, free, &moved, &e)) return Fail(err, modId + ": " + game::Narrow(absPath) + ": " + e);
        const std::filesystem::path dir = std::filesystem::path(game::GameDir()) / L"Melange" / L"cache" / L"meshes";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path cached = dir / (game::Widen(modId) + L"." + std::to_wstring(free) + L".xom");
        std::ofstream of(cached, std::ios::binary | std::ios::trunc);
        of.write(reinterpret_cast<const char*>(moved.data()), static_cast<std::streamsize>(moved.size()));
        of.close();
        if (!of) return Fail(err, modId + ": " + game::Narrow(cached.wstring()) + " could not be written");
        LOG_INFO("[meshes] %s: section %u is %s; the bank is loaded as section %u from %s", modId.c_str(), section,
                 section < kSectionMin || section > kSectionMax ? "outside the mod range" : "taken", free,
                 game::Narrow(cached.wstring()).c_str());
        bytes = std::move(moved);
        loadPath = cached.wstring();
        section = free;
    }
    if (!CheckEntries(modId, entries, section, &e)) return Fail(err, modId + ": " + game::Narrow(absPath) + ": " + e);
    if (sectionUsed) *sectionUsed = section;
    if (Rd<uint8_t>(kSectionLoaded + section) != 0)
        return Fail(err, modId + ": section " + std::to_string(section) + " was already loaded this session (another bank, or an earlier attempt); pick another --section");
    for (auto& en : entries)
        if (RawFind(grm, en.name.c_str()))
            return Fail(err, modId + ": \"" + en.name + "\" already exists in the graphical resource manager");
    // The engine's insert has no way out of a full name table (see kMaxStubsTotal), so refuse before registering anything.
    if (g_stubs + entries.size() > kMaxStubsTotal)
        return Fail(err, modId + ": " + std::to_string(entries.size()) + " more mesh stubs would pass the limit of " + std::to_string(kMaxStubsTotal) +
                             " for all mods (" + std::to_string(g_stubs) + " registered)");
    // The string the engine's sprintf sees is this one (game-relative when the file is under the game folder), not the
    // absolute path: a '%' in it becomes a format directive. A game folder with a '%' in its name is fine.
    const std::string path = EnginePath(loadPath);
    if (path.find('%') != std::string::npos) return Fail(err, modId + ": the bank path '" + path + "' may not contain '%' (it becomes a format string)");

    // Step 1: stubs. Strings must outlive this call (see g_strings).
    std::vector<Record> recs;
    for (auto& en : entries) {
        g_strings.push_back(en.name);
        const char* name = g_strings.back().c_str();
        g_strings.push_back(en.name + ".xom");
        const char* src = g_strings.back().c_str();
        recs.push_back({name, section, sceneBin, 0, src, en.flags, 0, 0});
    }
    const int added = RawAddMeshes(grm, recs.data(), static_cast<uint32_t>(recs.size()));
    if (added != 0) return Fail(err, modId + ": registering the mesh stubs failed (" + std::to_string(added) + ")");
    g_stubs += static_cast<uint32_t>(recs.size());
    for (auto& en : entries)
        if (!RawFind(grm, en.name.c_str()))
            return Fail(err, modId + ": \"" + en.name + "\" did not register");

    // Step 2: load the file as this section's bundle. The format string is swapped for the bank's path (no '%'
    // in it, so the section number the engine passes to sprintf is ignored) and restored right after, whatever
    // happens.
    const uintptr_t fmtField = grm + kFormatString;
    const std::string oldFmt = weapons::engine::XStringValue(fmtField);
    if (oldFmt.empty()) return Fail(err, "the bundle format string could not be read; refused");
    if (!weapons::engine::AssignXString(fmtField, path.c_str())) return Fail(err, "could not set the bundle path");
    const int rc = RawLoadSection(grm, section);
    const bool restored = weapons::engine::AssignXString(fmtField, oldFmt.c_str());
    if (!restored) LOG_ERROR("[meshes] the bundle format string could not be restored to '%s'; vanilla section loads may fail", oldFmt.c_str());
    if (rc != 0)
        return Fail(err, modId + ": the engine refused the bundle (" + std::to_string(rc) + ", path '" + path +
                             "'); see WormsMayhem's own log for 'Bundle contains unrecognised resource' / 'XomLoadObject'");
    for (auto& en : entries) {
        Info i;
        if (!Describe(en.name.c_str(), &i) || !i.loaded || !i.graphSet)
            return Fail(err, modId + ": \"" + en.name + "\" has no graph after the load (loaded=" + std::to_string(i.loaded) + ")");
        LOG_INFO("[meshes] %s: \"%s\" ready: section %u bin %u graphSet %08x", modId.c_str(), en.name.c_str(), i.section, i.sceneBin,
                 static_cast<unsigned>(i.graphSet));
    }
    // Which of the Airstrike helicopter's nodes each mesh lacks, for the "vehicleMeshes" gate (the nodes are the same after
    // a relocation, so the bytes loaded are the ones inspected). A failed read records nothing: such a mesh is not blocked.
    for (auto& en : entries) {
        std::vector<std::string> missing;
        std::string ne;
        if (!MissingNodes(bytes, en.name, VehicleNodes(), &missing, &ne)) continue;
        std::string list;
        for (auto& m : missing) list += (list.empty() ? "" : ", ") + m;
        g_nodesMissing[en.name] = list;
    }
    ++g_banks;
    LOG_INFO("[meshes] %s: loaded %zu mesh(es) from %s as section %u (instances now %d)", modId.c_str(), entries.size(), path.c_str(),
             section, Rd<int>(kSectionInstances + section * 4));
    return true;
}

std::string VehicleNodesMissing(const char* resourceName) {
    if (!resourceName) return "";
    const auto it = g_nodesMissing.find(resourceName);
    return it == g_nodesMissing.end() ? std::string() : it->second;
}

uint32_t FreeSections() {
    if (!Grm()) return 0;
    uint32_t n = 0;
    for (uint16_t s = kSectionMin; s <= kSectionMax; ++s) n += Rd<uint8_t>(kSectionLoaded + s) == 0;
    return n;
}
uint32_t Count() { return g_banks; }
uint32_t Registered() { return g_stubs; }
}  // namespace melange::assets::meshes
