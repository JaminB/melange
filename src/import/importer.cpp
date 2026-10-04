#include "import/importer.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>

#include "assets/crcsafe.h"
#include "erg/names.h"
#include "import/archive.h"
#include "import/plan.h"
#include "import/preview.h"
#include "import/w4reader.h"
#include "levels/hidden.h"
#include "levels/manifest.h"
#include "levels/roots.h"
#include "mods/spice.h"
#include "store/fetch.h"
#include "store/install.h"
#include "store/zipcheck.h"
#include "tools/hash.h"

namespace melange::import {
namespace {
namespace inst = store::install;
constexpr uint64_t kMaxWritten = 128ull << 20;
constexpr uint64_t kWriteEstimate = 64ull << 20;
constexpr int kPreviewWidth = 256;

std::wstring W(std::string_view s) {
    std::wstring w;
    for (char c : s) w.push_back(static_cast<unsigned char>(c));
    return w;
}

std::string Lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

std::string BaseStem(const std::string& rel) {
    const size_t slash = rel.rfind('/');
    std::string b = slash == std::string::npos ? rel : rel.substr(slash + 1);
    const size_t dot = b.rfind('.');
    return Lower(dot == std::string::npos ? b : b.substr(0, dot));
}

bool MakeDirs(const std::wstring& root, const std::wstring& rel) {
    size_t p = 0;
    for (;;) {
        p = rel.find(L'\\', p);
        const std::wstring d = root + L"\\" + (p == std::wstring::npos ? rel : rel.substr(0, p));
        if (!CreateDirectoryW(d.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        if (p == std::wstring::npos) return true;
        ++p;
    }
}

bool ReadFileCapped(const std::wstring& path, uint64_t cap, std::vector<uint8_t>* out) {
    out->clear();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (out->size() + n > cap) {
            ok = false;
            break;
        }
        out->insert(out->end(), buf, buf + n);
    }
    fclose(f);
    return ok;
}

bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& b) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = b.empty() || (WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &w, nullptr) && w == b.size());
    CloseHandle(h);
    return ok;
}

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

bool SizeOrHash(const std::string& e) {
    return e.starts_with("Downloaded file does not match") || e.find("larger than the") != std::string::npos;
}

std::string HidePrefix(const std::string& plugin) { return erg::names::Prefix(plugin) + "_"; }

bool PluginStem(const std::string& plugin, const std::string& stem) {
    const std::string p = HidePrefix(plugin);
    return stem.size() > p.size() + 2 && stem.compare(0, p.size(), p) == 0 && stem[p.size()] >= '1' && stem[p.size()] <= '9' &&
           stem[p.size() + 1] == '_';
}

bool WriteHidden(const Paths& p, const std::string& plugin, const std::set<std::string>& stems) {
    std::set<std::string> h = levels::hidden::Load(p.game);
    for (auto it = h.begin(); it != h.end();) it = PluginStem(plugin, *it) ? h.erase(it) : std::next(it);
    h.insert(stems.begin(), stems.end());
    return levels::hidden::Save(p.game, h);
}

class Runner {
  public:
    explicit Runner(const RunSpec& s) : s_(s), r_(s.plugin.recipe), paths_(MakePaths(s.game, s.plugin.id)) {}

    RunResult Go() {
        t0_ = std::chrono::steady_clock::now();
        if (!Prepare() || !Acquire() || !Read() || !Build() || !Install()) {
            if (!stageRoot_.empty()) inst::DeleteTree(stageRoot_);
            if (!part_.empty()) DeleteFileW(part_.c_str());
            res_.ok = false;
            return res_;
        }
        inst::DeleteTree(stageRoot_);
        res_.ok = true;
        return res_;
    }

  private:
    bool Fail(const char* reason, const std::string& msg) {
        res_.reason = reason;
        res_.message = msg;
        return false;
    }
    bool Cancelled() { return s_.cancel && s_.cancel->load(); }
    bool CheckTime() {
        if (Cancelled()) return Fail("cancelled", "cancelled");
        if (std::chrono::steady_clock::now() - t0_ > std::chrono::milliseconds(s_.watchdogMs))
            return Fail("internal", "the import took longer than its time limit");
        return true;
    }
    void Report(const char* phase, uint64_t bytes = 0, uint64_t total = 0, int step = 0, int of = 0) {
        if (s_.progress) s_.progress(Progress{phase, bytes, total, step, of});
    }

    bool Prepare() {
        if (s_.game.empty()) return Fail("internal", "no game folder");
        for (const Source& src : r_.sources)
            if (src.id == s_.sourceId || s_.kind == RunSpec::Kind::File) {
                src_ = &src;
                break;
            }
        if (!src_) return Fail("internal", "no source '" + s_.sourceId + "' in the recipe");
        if (!Recover(paths_, s_.plugin.id, s_.move)) return Fail("write", "cannot restore the previous packs from Mods\\.import");
        std::vector<std::string> foreign;
        ExistingPacks(paths_, s_.plugin.id, &foreign);
        if (!foreign.empty()) return Fail("occupied", foreign.front());
        if (!EnsureWork(paths_)) return Fail("write", "cannot create Mods\\.import");
        hadState_ = LoadState(paths_, &prev_);
        zip_ = paths_.Dl() + L"\\" + W(src_->fileName);
        return true;
    }

    bool SpaceFor(uint64_t download) {
        ULARGE_INTEGER freeBytes{};
        if (!GetDiskFreeSpaceExW(paths_.mods.c_str(), &freeBytes, nullptr, nullptr)) return true;
        const uint64_t need = download + 2 * kWriteEstimate + (16ull << 20);
        if (freeBytes.QuadPart >= need) return true;
        return Fail("space", std::to_string((need + (1 << 20) - 1) >> 20) + " MB");
    }

    bool CopyHashed(const std::wstring& from) {
        HANDLE in = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (in == INVALID_HANDLE_VALUE) return Fail("hash", "cannot open the zip you picked");
        HANDLE out = CreateFileW(part_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) {
            CloseHandle(in);
            return Fail("write", "cannot write to Mods\\.import");
        }
        hashutil::Sha256 sha;
        std::vector<uint8_t> buf(1 << 20);
        uint64_t n = 0;
        bool ok = true, tooBig = false;
        for (;;) {
            if (Cancelled()) {
                ok = false;
                break;
            }
            DWORD got = 0;
            if (!ReadFile(in, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
                ok = false;
                break;
            }
            if (!got) break;
            if (n + got > src_->size) {
                tooBig = true;
                break;
            }
            DWORD w = 0;
            if (!WriteFile(out, buf.data(), got, &w, nullptr) || w != got) {
                CloseHandle(in);
                CloseHandle(out);
                return Fail("write", "could not copy the zip (disk full?)");
            }
            sha.Update(buf.data(), got);
            n += got;
            Report("copying", n, src_->size);
        }
        CloseHandle(in);
        CloseHandle(out);
        if (Cancelled()) return Fail("cancelled", "cancelled");
        if (!ok) return Fail("hash", "could not read the zip you picked");
        Report("verifying", n, src_->size);
        if (tooBig || n != src_->size || sha.FinishHex() != src_->sha256)
            return Fail("hash", "size or checksum does not match");
        return true;
    }

    bool Acquire() {
        if (inst::Exists(zip_)) {
            Report("verifying", 0, src_->size);
            std::string e;
            if (inst::VerifyFile(zip_, src_->size, src_->sha256, &e)) return true;
            DeleteFileW(zip_.c_str());
        }
        if (!SpaceFor(src_->size)) return false;
        part_ = zip_ + L".part";
        if (s_.kind == RunSpec::Kind::File) {
            if (!CopyHashed(s_.file)) return false;
        } else {
            const std::vector<std::string>& urls = s_.urls.empty() ? src_->urls : s_.urls;
            inst::Expect ex;
            ex.size = src_->size;
            ex.sha256 = src_->sha256;
            store::fetch::Options o;
            o.cancel = s_.cancel;
            o.userAgent = s_.userAgent;
            o.progress = [this](uint64_t got, uint64_t) { Report("downloading", got, src_->size); };
            bool ok = false;
            std::string e;
            for (const auto& u : urls) {
                if (s_.urls.empty() && !AllowedHost(HostOf(u))) continue;
                Report("downloading", 0, src_->size);
                e.clear();
                ok = inst::FetchVerified(u, part_, ex, src_->size, o, &e);
                if (ok || Cancelled() || SizeOrHash(e)) break;
            }
            if (Cancelled()) return Fail("cancelled", "cancelled");
            if (!ok) return Fail(SizeOrHash(e) ? "hash" : "network", e.empty() ? "no usable URL" : e);
            Report("verifying", src_->size, src_->size);
        }
        if (!MoveFileExW(part_.c_str(), zip_.c_str(), MOVEFILE_REPLACE_EXISTING)) return Fail("write", "cannot keep the verified zip");
        part_.clear();
        return true;
    }

    bool ReadMember(const Member& m, std::vector<uint8_t>* out) {
        std::string e;
        if (archive_.Read(m, out, &e, s_.cancel)) return true;
        return Fail(e == "cancelled" ? "cancelled" : "zip", e);
    }

    bool Read() {
        t0_ = std::chrono::steady_clock::now();   // the watchdog covers generation, not a slow download
        Report("reading");
        std::string e;
        if (!archive_.Open(zip_, r_.reader, &e)) return Fail("zip", e);
        const Member* reg = archive_.Find(r_.reader.registry);
        if (!reg) return Fail("recipe", "the zip has no " + r_.reader.root + "/" + r_.reader.registry);
        std::vector<uint8_t> b;
        Found f;
        if (!ReadMember(*reg, &b)) return false;
        if (!ReadRegistry(b, &f.registry, &e)) return Fail("zip", e);
        for (const auto& t : r_.reader.titles) {
            const Member* m = archive_.Find(t);
            if (!m) continue;
            std::map<std::string, std::string> strings;
            if (!ReadMember(*m, &b)) return false;
            if (!ReadStrings(b, &strings, &e)) return Fail("zip", e);
            titles_.push_back(std::move(strings));
        }
        for (const auto& [_, m] : archive_.Members()) {
            const std::string stem = BaseStem(m.rel);
            switch (m.kind) {
                case Kind::Descriptor: f.descriptors.insert(stem), desc_[stem] = &m; break;
                case Kind::Xan: f.xans.insert(stem), xan_[stem] = &m; break;
                case Kind::Txt: txt_[stem] = &m; break;
                case Kind::Hmp: hmp_[stem] = &m; break;
                case Kind::Preview: preview_[Lower(m.rel.substr(m.rel.rfind('/') + 1))] = &m; break;
                default: break;
            }
        }
        Selection sel;
        if (!SelectMaps(r_, f, &sel, &e)) return Fail("recipe", e);
        if (!PlanPacks(r_, sel, &plan_, &e)) return Fail("recipe", e);
        res_.skipped = sel.skipped;
        for (const Planned& p : plan_) {
            if (!p.sel.fromGame) continue;
            for (const auto& v : r_.select.vanilla) {
                if (Lower(v.file) != Lower(p.sel.entry.fileName)) continue;
                for (const auto& [rel, sha] : v.sha256) {
                    std::wstring path = s_.game + L"\\Data\\" + W(rel);
                    std::replace(path.begin(), path.end(), L'/', L'\\');
                    if (hashutil::Sha256HexFile(path) != sha) return Fail("vanilla", "Data/" + rel);
                }
            }
        }
        return CheckTime();
    }

    const VanillaPin* Pin(const std::string& file) const {
        for (const auto& v : r_.select.vanilla)
            if (Lower(v.file) == Lower(file)) return &v;
        return nullptr;
    }

    bool GameFile(const VanillaPin& pin, const char* suffix, uint64_t cap, std::vector<uint8_t>* out, bool* present) {
        *present = false;
        for (const auto& [rel, sha] : pin.sha256) {
            if (!Lower(rel).ends_with(suffix)) continue;
            std::wstring path = s_.game + L"\\Data\\" + W(rel);
            std::replace(path.begin(), path.end(), L'/', L'\\');
            if (!ReadFileCapped(path, cap, out) || hashutil::Sha256Hex(out->data(), out->size()) != sha)
                return Fail("vanilla", "Data/" + rel);
            if (store::zipcheck::ExecutableMagic(out->data(), out->size())) return Fail("vanilla", "Data/" + rel + ": executable content");
            *present = true;
            return true;
        }
        return true;
    }

    bool Put(const std::string& packId, const std::string& rel, const std::vector<uint8_t>& bytes) {
        written_ += bytes.size();
        if (written_ > kMaxWritten) return Fail("write", "the packs would be larger than 128 MiB");
        std::wstring wrel = W(rel);
        std::replace(wrel.begin(), wrel.end(), L'/', L'\\');
        const std::wstring dir = stageRoot_ + L"\\" + W(packId);
        const size_t slash = wrel.rfind(L'\\');
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return Fail("write", "cannot create " + packId);
        if (slash != std::wstring::npos && !MakeDirs(dir, wrel.substr(0, slash))) return Fail("write", "cannot create " + packId);
        if (!WriteBytes(dir + L"\\" + wrel, bytes)) return Fail("write", "cannot write " + packId + "/" + rel);
        hashes_.emplace_back(packId + "/" + rel, hashutil::Sha256Hex(bytes.data(), bytes.size()));
        return true;
    }

    std::string Title(const RegistryEntry& e, const std::string& slug) const {
        const size_t max = static_cast<size_t>(r_.transform.titleMax);
        for (const auto& bank : titles_)
            if (auto it = bank.find(e.frontendName); !e.frontendName.empty() && it != bank.end()) {
                std::string t = CleanTitle(it->second, max);
                if (!t.empty()) return t;
            }
        std::string t = CleanTitle(e.fileName, max);
        return t.empty() ? slug : t;
    }

    bool Build() {
        stageRoot_ = paths_.Stage() + L"\\" + W(hashutil::RandomSalt().substr(0, 8));
        if (!CreateDirectoryW(stageRoot_.c_str(), nullptr)) return Fail("write", "cannot create a staging folder");
        const std::string descExt = r_.reader.descriptors.substr(1);
        const int n = static_cast<int>(plan_.size());
        for (int i = 0; i < n; ++i) {
            if (!CheckTime()) return false;
            Report("building", 0, 0, i + 1, n);
            const Planned& p = plan_[static_cast<size_t>(i)];
            const RegistryEntry& e = p.sel.entry;
            const std::string key = Lower(e.fileName);
            std::vector<uint8_t> descBytes, xan, hmp, txt;
            bool hasHmp = false, hasTxt = false;
            std::string err;
            Descriptor d;
            if (p.sel.fromGame) {
                const VanillaPin* pin = Pin(e.fileName);
                bool present = false;
                if (!pin || !GameFile(*pin, ".xom", KindCap(Kind::Descriptor), &descBytes, &present)) return false;
                if (!GameFile(*pin, ".xan", KindCap(Kind::Xan), &xan, &present)) return false;
                if (!GameFile(*pin, ".hmp", KindCap(Kind::Hmp), &hmp, &hasHmp)) return false;
                if (!ReadDescriptor(descBytes, r_.transform.author, &d, &err)) return Fail("vanilla", e.fileName + ": " + err);
            } else {
                const auto di = desc_.find(key), xi = xan_.find(key);
                if (di == desc_.end() || xi == xan_.end()) return Fail("recipe", e.fileName + ": no descriptor or .xan in the zip");
                if (!ReadMember(*di->second, &descBytes) || !ReadMember(*xi->second, &xan)) return false;
                if (auto it = hmp_.find(key); it != hmp_.end()) {
                    if (!ReadMember(*it->second, &hmp)) return false;
                    hasHmp = true;
                }
                if (!ReadDescriptor(descBytes, r_.transform.author, &d, &err)) return Fail("zip", e.fileName + descExt + ": " + err);
            }
            DescriptorOut out;
            out.theme = d.theme;
            out.timeOfDay = NormalTimeOfDay(r_, d.timeOfDay);
            out.materialFile = d.materialFile;
            out.heightmapBase = d.heightmapBase;
            out.heightmapSecond = d.heightmapSecond;
            out.customTextureBank = d.customTextureBank;
            out.customDetailBank = d.customDetailBank;
            if (!p.sel.fromGame && Lower(d.materialFile).starts_with("maps\\") && d.materialFile.find_first_of("\\/", 5) == std::string::npos) {
                const std::string t = BaseStem(d.materialFile.substr(5));
                if (auto it = txt_.find(t); it != txt_.end() && Lower(d.materialFile).ends_with(".txt")) {
                    if (!ReadMember(*it->second, &txt)) return false;
                    hasTxt = true;
                    out.materialFile = "Maps\\" + p.stem + ".txt";
                }
            }
            std::vector<uint8_t> xom;
            if (!BuildDescriptor(out, &xom, &err)) return Fail("internal", err);
            if (!Put(p.packId, "assets/levels/" + p.stem + ".XOM", xom) || !Put(p.packId, "assets/levels/Maps/" + p.stem + ".xan", xan))
                return false;
            if (hasHmp && !Put(p.packId, "assets/levels/Maps/" + p.stem + ".hmp", hmp)) return false;
            if (hasTxt && !Put(p.packId, "assets/levels/Maps/" + p.stem + ".txt", txt)) return false;

            MapInfo mi;
            mi.file = e.fileName;
            mi.stem = p.stem;
            mi.pack = p.packId;
            mi.title = Title(e, p.slug);
            mi.author = CleanTitle(d.author, 80);
            const Group& g = r_.groups[static_cast<size_t>(p.group)];
            const Category& c = r_.categories[static_cast<size_t>(p.category)];
            mi.group = g.id, mi.groupLabel = g.label, mi.category = c.id, mi.categoryLabel = c.label;
            mi.mode = p.mode;
            mi.theme = CleanTitle(d.theme, 24);
            mi.timeOfDay = out.timeOfDay;
            mi.survivor = r_.transform.survivor;
            if (r_.transform.previews && !e.frontendImage.empty()) {
                if (auto it = preview_.find(Lower(e.frontendImage)); it != preview_.end()) {
                    std::vector<uint8_t> tga, png;
                    if (!ReadMember(*it->second, &tga)) return false;
                    if (TgaToPng(tga, kPreviewWidth, &png)) {
                        const std::wstring dir = stageRoot_ + L"\\.previews";
                        CreateDirectoryW(dir.c_str(), nullptr);
                        mi.preview = WriteBytes(dir + L"\\" + W(p.stem) + L".png", png);
                    }
                }
            }
            maps_.push_back(std::move(mi));
            res_.counts[c.id]++;
        }
        for (const Category& c : r_.categories) res_.counts.try_emplace(c.id, 0);
        return WriteManifests();
    }

    bool WriteManifests() {
        std::vector<assets::crcsafe::Entry> crc;
        assets::crcsafe::ReadFromExe(s_.game + L"\\WormsMayhem.exe", &crc);
        for (size_t i = 0; i < plan_.size();) {
            PackSpec ps;
            ps.id = plan_[i].packId;
            const int n = plan_[i].pack;
            ps.version = r_.output.version;
            ps.name = r_.output.packName;
            ps.name.replace(ps.name.find("{n}"), 3, std::to_string(n));
            ps.description = r_.output.packDescription;
            ps.author = s_.plugin.name;
            ps.melangeRange = s_.plugin.melangeRange;
            ps.generatedBy = s_.plugin.id;
            ps.recipe = r_.id;
            PackRecord rec;
            rec.id = ps.id;
            for (; i < plan_.size() && plan_[i].pack == n; ++i) {
                ps.levels.push_back({plan_[i].slug, maps_[i].title, r_.transform.survivor});
                const std::string& cat = r_.categories[static_cast<size_t>(plan_[i].category)].id;
                if (std::find(rec.categories.begin(), rec.categories.end(), cat) == rec.categories.end()) rec.categories.push_back(cat);
            }
            rec.levels = static_cast<int>(ps.levels.size());
            const std::string json = SpiceJson(ps);
            if (!Put(ps.id, "spice.json", std::vector<uint8_t>(json.begin(), json.end()))) return false;
            const std::wstring dir = stageRoot_ + L"\\" + W(ps.id);
            spice::Manifest m;
            std::vector<spice::Error> errs;
            std::vector<levels::manifest::Error> lerrs;
            if (!spice::Parse(dir, &m, &errs) || m.implicit || m.generatedBy != s_.plugin.id)
                return Fail("internal", ps.id + ": generated spice.json does not parse" + (errs.empty() ? "" : ": " + errs.front().text));
            const auto decls = levels::manifest::Parse(m, &lerrs);
            if (decls.size() != ps.levels.size()) return Fail("internal", ps.id + ": " + (lerrs.empty() ? "levels refused" : lerrs.front().text));
            const auto listing = levels::roots::ListLevelRoot(std::filesystem::path(dir) / L"assets" / L"levels");
            std::string why;
            if (!levels::roots::CheckLevelRoot(erg::names::Prefix(ps.id), listing, crc, &why) || !levels::roots::CheckBuilt(decls, listing, &why))
                return Fail("internal", ps.id + ": " + why);
            res_.packs.push_back(ps.id);
            state_.packs.push_back(std::move(rec));
        }
        return CheckTime();
    }

    bool Install() {
        Report("placing");
        std::string e;
        if (!Place(paths_, s_.plugin.id, stageRoot_, res_.packs, &e, s_.move)) {
            if (e.starts_with("occupied:")) return Fail("occupied", e.substr(9));
            return Fail("write", e);
        }
        inst::DeleteTree(paths_.Previews());
        if (inst::Exists(stageRoot_ + L"\\.previews")) MoveFileExW((stageRoot_ + L"\\.previews").c_str(), paths_.Previews().c_str(), 0);
        bool saved = inst::WriteFileAtomic(paths_.Catalogue(), CatalogueJson(maps_));

        std::vector<std::string> hiddenFiles;
        if (hadState_) {
            hiddenFiles = prev_.hiddenByFile;
        } else {
            for (const MapInfo& m : maps_)
                for (const Category& c : r_.categories)
                    if (c.hidden && c.id == m.category) hiddenFiles.push_back(m.file);
        }
        std::set<std::string> stems;
        for (const MapInfo& m : maps_)
            if (std::find(hiddenFiles.begin(), hiddenFiles.end(), m.file) != hiddenFiles.end()) stems.insert(m.stem);
        saved &= WriteHidden(paths_, s_.plugin.id, stems);

        res_.fingerprint = Fingerprint(hashes_);
        res_.maps = static_cast<int>(maps_.size());
        res_.bytes = written_;
        state_.recipe = r_.id;
        state_.recipeVersion = r_.output.version;
        state_.format = kFormat;
        state_.sourceSha256 = src_->sha256;
        state_.fingerprint = res_.fingerprint;
        state_.importedAt = inst::NowIso();
        state_.maps = res_.maps;
        state_.skipped = res_.skipped;
        state_.counts = res_.counts;
        state_.bytes = written_;
        state_.zipKept = s_.keepZip;
        state_.hiddenByFile = hiddenFiles;
        saved &= SaveState(paths_, state_);
        Finish(paths_);
        if (!s_.keepZip) DeleteFileW(zip_.c_str());
        return saved || Fail("write", "the packs are in place but the import record could not be saved");
    }

    const RunSpec& s_;
    const Recipe& r_;
    Paths paths_;
    RunResult res_;
    const Source* src_ = nullptr;
    std::chrono::steady_clock::time_point t0_;
    std::wstring zip_, part_, stageRoot_;
    bool hadState_ = false;
    State prev_, state_;
    Archive archive_;
    std::vector<std::map<std::string, std::string>> titles_;
    std::map<std::string, const Member*> desc_, xan_, txt_, hmp_, preview_;
    std::vector<Planned> plan_;
    std::vector<MapInfo> maps_;
    std::vector<std::pair<std::string, std::string>> hashes_;
    uint64_t written_ = 0;
};
}  // namespace

RunResult Run(const RunSpec& spec) { return Runner(spec).Go(); }

bool LoadPlugin(const std::wstring& game, const std::string& id, Plugin* out, std::string* err, bool* unsupported) {
    *out = Plugin{};
    if (unsupported) *unsupported = false;
    if (!spice::ValidModId(id)) {
        *err = "bad plugin id";
        return false;
    }
    const std::wstring dir = game + L"\\Mods\\" + W(id);
    spice::Manifest m;
    std::vector<spice::Error> errs;
    if (!spice::Parse(dir, &m, &errs) || m.implicit || m.id != id) {
        *err = id + " is not installed";
        return false;
    }
    if (m.importerRecipe.empty() || !m.generatedBy.empty()) {
        *err = id + " has no importer";
        return false;
    }
    std::wstring rel = W(m.importerRecipe);
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    std::vector<uint8_t> text;
    if (!ReadFileCapped(dir + L"\\" + rel, 64 * 1024, &text)) {
        *err = "cannot read " + m.importerRecipe;
        return false;
    }
    out->id = id;
    out->name = m.name;
    out->melangeRange = m.melangeRange;
    return ParseRecipe(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), id, &out->recipe, err, unsupported);
}

std::vector<std::string> ImporterPlugins(const std::wstring& game) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((game + L"\\Mods\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        spice::Manifest m;
        std::vector<spice::Error> errs;
        if (spice::Parse(game + L"\\Mods\\" + fd.cFileName, &m, &errs) && !m.implicit && !m.importerRecipe.empty() && m.generatedBy.empty())
            out.push_back(m.id);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::string Status(const Paths& p, const Plugin& pl, const State* st, std::string* reason) {
    reason->clear();
    if (!st) return "none";
    for (const PackRecord& r : st->packs) {
        const std::wstring dir = p.mods + L"\\" + W(r.id);
        spice::Manifest m;
        std::vector<spice::Error> errs;
        if (!spice::Parse(dir, &m, &errs) || m.implicit || m.generatedBy != pl.id || m.version != st->recipeVersion) {
            *reason = r.id + " is missing or changed";
            return "damaged";
        }
    }
    if (st->recipe != pl.recipe.id || st->recipeVersion != pl.recipe.output.version || st->format != kFormat) {
        *reason = "imported with " + st->recipe + " " + st->recipeVersion;
        return "stale";
    }
    return "imported";
}

bool Uninstall(const Paths& p, const std::string& plugin, bool deleteZip, std::vector<std::string>* removed, std::string* err,
               const MoveFn& mv) {
    if (!Recover(p, plugin, mv)) {
        *err = "cannot restore the previous packs from Mods\\.import";
        return false;
    }
    if (!RemovePacks(p, plugin, removed, err, mv)) return false;
    WriteHidden(p, plugin, {});
    DeleteFileW(p.Catalogue().c_str());
    DeleteFileW(p.StateFile().c_str());
    inst::DeleteTree(p.Previews());
    inst::DeleteTree(p.Stage());
    if (deleteZip) inst::DeleteTree(p.Dl());
    RemoveDirectoryW(p.Work().c_str());
    return true;
}

int SetHidden(const Paths& p, const std::vector<std::string>& files, bool hidden, std::string* err) {
    State st;
    std::vector<MapInfo> maps;
    std::vector<uint8_t> text;
    if (!LoadState(p, &st) || !ReadFileCapped(p.Catalogue(), 4u << 20, &text) ||
        !ParseCatalogue(std::string(text.begin(), text.end()), &maps)) {
        *err = "notImported";
        return -1;
    }
    std::set<std::string> h(st.hiddenByFile.begin(), st.hiddenByFile.end());
    for (const auto& f : files) {
        if (hidden) h.insert(f);
        else h.erase(f);
    }
    st.hiddenByFile.assign(h.begin(), h.end());
    std::set<std::string> stems;
    for (const MapInfo& m : maps)
        if (h.count(m.file)) stems.insert(m.stem);
    std::string plugin;
    for (wchar_t c : p.plugin) plugin.push_back(static_cast<char>(c));
    if (!SaveState(p, st) || !WriteHidden(p, plugin, stems)) {
        *err = "write";
        return -1;
    }
    return static_cast<int>(stems.size());
}

bool CachedZip(const Paths& p, const Source& s, uint64_t* bytes, bool verify) {
    const std::wstring zip = p.Dl() + L"\\" + W(s.fileName);
    *bytes = inst::Exists(zip) ? FileSize(zip) : 0;
    if (!*bytes) return false;
    std::string e;
    return !verify || inst::VerifyFile(zip, s.size, s.sha256, &e);
}
}  // namespace melange::import
