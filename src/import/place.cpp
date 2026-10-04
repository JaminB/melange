#include "import/place.h"

#include <windows.h>

#include <algorithm>

#include "import/recipe.h"
#include "mods/spice.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::import {
namespace {
namespace inst = store::install;

std::wstring W(const std::string& s) {
    std::wstring w;
    for (char c : s) w.push_back(static_cast<unsigned char>(c));
    return w;
}

bool IsDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

struct Journal {
    std::vector<std::string> old, fresh;
    bool committed = false;
};

bool SaveJournal(const Paths& p, const Journal& j) {
    jsonmini::Arr o, n;
    for (const auto& s : j.old) o.Str(s);
    for (const auto& s : j.fresh) n.Str(s);
    return inst::WriteFileAtomic(p.Journal(), jsonmini::Obj().Raw("old", o.End()).Raw("new", n.End()).Bool("committed", j.committed).End() + "\n");
}

bool LoadJournal(const Paths& p, const std::string& plugin, Journal* j) {
    json::Value v;
    json::Error e;
    if (!json::ParseFile(p.Journal(), &v, &e) || !v.IsObject()) return false;
    auto list = [&](const char* k, std::vector<std::string>* out) {
        if (const json::Value* x = v.Get(k); x && x->IsArray())
            for (const auto& it : x->items)
                if (it.IsString() && it.string.size() == plugin.size() + 2 && it.string.starts_with(plugin + "-") &&
                    it.string.back() >= '1' && it.string.back() <= '9')
                    out->push_back(it.string);
    };
    list("old", &j->old);
    list("new", &j->fresh);
    if (const json::Value* x = v.Get("committed"); x && x->IsBool()) j->committed = x->boolean;
    return true;
}

bool Has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
}  // namespace

bool IsGeneratedBy(const std::wstring& dir, const std::string& plugin) {
    if (!IsDir(dir)) return false;
    spice::Manifest m;
    std::vector<spice::Error> errs;
    return spice::Parse(dir, &m, &errs) && !m.implicit && m.generatedBy == plugin;
}

std::vector<std::string> ExistingPacks(const Paths& p, const std::string& plugin, std::vector<std::string>* foreign) {
    std::vector<std::string> out;
    for (size_t n = 1; n <= kMaxPacks; ++n) {
        const std::string id = plugin + "-" + std::to_string(n);
        const std::wstring dir = p.mods + L"\\" + W(id);
        if (!inst::Exists(dir)) continue;
        if (IsGeneratedBy(dir, plugin)) out.push_back(id);
        else if (foreign) foreign->push_back(id);
    }
    return out;
}

bool Place(const Paths& p, const std::string& plugin, const std::wstring& stageRoot, const std::vector<std::string>& staged,
           std::string* err, const MoveFn& mv) {
    std::vector<std::string> foreign;
    const std::vector<std::string> existing = ExistingPacks(p, plugin, &foreign);
    if (!foreign.empty()) {
        *err = "occupied:" + foreign.front();
        return false;
    }
    inst::DeleteTree(p.Old());
    if (!CreateDirectoryW(p.Old().c_str(), nullptr) && !IsDir(p.Old())) {
        *err = "cannot create the importer's old folder";
        return false;
    }
    Journal j{existing, staged, false};
    if (!SaveJournal(p, j)) {
        *err = "cannot write the placement journal";
        return false;
    }
    std::vector<std::string> movedOld, placed;
    // A failed move back keeps the journal, so Recover finishes the rollback instead of deleting the old packs.
    auto rollback = [&] {
        bool clean = true;
        for (auto it = placed.rbegin(); it != placed.rend(); ++it) clean &= !mv(p.mods + L"\\" + W(*it), stageRoot + L"\\" + W(*it));
        for (auto it = movedOld.rbegin(); it != movedOld.rend(); ++it) clean &= !mv(p.Old() + L"\\" + W(*it), p.mods + L"\\" + W(*it));
        if (clean) DeleteFileW(p.Journal().c_str());
    };
    for (const auto& id : existing) {
        if (const unsigned long e = mv(p.mods + L"\\" + W(id), p.Old() + L"\\" + W(id))) {
            *err = "cannot move Mods\\" + id + " aside (error " + std::to_string(e) + ")";
            rollback();
            return false;
        }
        movedOld.push_back(id);
    }
    for (const auto& id : staged) {
        if (const unsigned long e = mv(stageRoot + L"\\" + W(id), p.mods + L"\\" + W(id))) {
            *err = "cannot move " + id + " into Mods (error " + std::to_string(e) + ")";
            rollback();
            return false;
        }
        placed.push_back(id);
    }
    j.committed = true;
    SaveJournal(p, j);
    return true;
}

void Finish(const Paths& p) {
    inst::DeleteTree(p.Old());
    DeleteFileW(p.Journal().c_str());
}

bool Recover(const Paths& p, const std::string& plugin, const MoveFn& mv) {
    Journal j;
    if (!LoadJournal(p, plugin, &j)) {
        DeleteFileW(p.Journal().c_str());
        inst::DeleteTree(p.Old());
        return true;
    }
    if (!j.committed) {
        bool restored = true;
        for (const auto& id : j.old) {
            const std::wstring back = p.Old() + L"\\" + W(id), target = p.mods + L"\\" + W(id);
            if (!IsDir(back)) continue;
            if (inst::Exists(target)) {
                if (!IsGeneratedBy(target, plugin)) continue;
                inst::DeleteTree(target);
            }
            restored &= !mv(back, target);
        }
        if (!restored) return false;   // keep the journal and the old packs for the next try
        for (const auto& id : j.fresh) {
            const std::wstring target = p.mods + L"\\" + W(id);
            if (!Has(j.old, id) && IsGeneratedBy(target, plugin)) inst::DeleteTree(target);
        }
    }
    Finish(p);
    inst::DeleteTree(p.Stage());
    return true;
}

bool RemovePacks(const Paths& p, const std::string& plugin, std::vector<std::string>* removed, std::string* err, const MoveFn& mv) {
    removed->clear();
    const std::vector<std::string> existing = ExistingPacks(p, plugin, nullptr);
    if (existing.empty()) return true;
    inst::DeleteTree(p.Old());
    if (!CreateDirectoryW(p.Old().c_str(), nullptr) && !IsDir(p.Old())) {
        *err = "cannot create the importer's old folder";
        return false;
    }
    for (const auto& id : existing) {
        if (const unsigned long e = mv(p.mods + L"\\" + W(id), p.Old() + L"\\" + W(id))) {
            *err = "cannot remove Mods\\" + id + " (error " + std::to_string(e) + ")";
            for (const auto& back : *removed) mv(p.Old() + L"\\" + W(back), p.mods + L"\\" + W(back));
            removed->clear();
            return false;
        }
        removed->push_back(id);
    }
    inst::DeleteTree(p.Old());
    return true;
}
}  // namespace melange::import
