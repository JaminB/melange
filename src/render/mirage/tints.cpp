// Module "MirageTints": per-copy scenery tints (tints.h). Each frame in a match it checks the level key
// (WXD.Level.Current); when it changes, it finds the level's .xan and .XOM on the engine's search roots (Data and the
// roots Melange added: map packs, the Erg Test workspace, the level cache), loads them as an Erg scene and keeps every
// detail whose name carries a tint tag, at its world position and scale. shaders_glsl.cpp asks ForModel() before each
// draw of a replacement that declares mg_tint. No tinted details, no cost beyond the per-frame key check.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "assets/searchpath.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "erg/install.h"
#include "erg/load.h"
#include "erg/scene.h"
#include "levels/engine.h"
#include "melange/render.h"
#include "melange/sim.h"
#include "melange/testcmd.h"
#include "render/mirage/tints.h"
#include "render/mirage/tints_logic.h"

namespace melange::mirage::tints {
namespace {
namespace lg = logic;
lg::Table g_table;
std::string g_key;             // the level key the table was built for ("" none)
uint64_t g_checked = 0;        // frame of the last key check
const float kWhite[4] = {1.f, 1.f, 1.f, 0.f};
struct Stats {
    uint64_t lookups = 0, hits = 0;
    std::string file;
    size_t details = 0;
} g_st;

bool ReadOne(const std::wstring& path, size_t max, std::vector<uint8_t>* out) {
    std::string e;
    return erg::install::Exists(path) && erg::install::ReadFile(path, max, out, &e);
}

// The level's files: the first search root (Melange's added roots first, then Data) holding Maps\<stem>.xan and <stem>.XOM.
bool FindLevel(const std::string& stem, erg::load::BaseFiles* f, std::wstring* where) {
    const std::wstring game = game::GameDir();
    std::vector<std::wstring> roots;
    for (const std::string& r : assets::searchpath::Added()) roots.push_back(game + L"\\" + erg::install::Widen(r));
    roots.push_back(erg::install::DataDir(game));
    const std::wstring w = erg::install::Widen(stem);
    for (const std::wstring& r : roots) {
        std::wstring root = r;
        for (auto& c : root)
            if (c == L'/') c = L'\\';
        if (!ReadOne(root + L"\\Maps\\" + w + L".xan", erg::load::kMaxXanBytes, &f->xan)) continue;
        if (!ReadOne(root + L"\\" + w + L".XOM", erg::load::kMaxXomBytes, &f->xom)) continue;
        *where = root;
        return true;
    }
    return false;
}

void Load(const std::string& key) {
    g_table.Set({});
    g_st = Stats{};
    levels::engine::Details d;
    if (!levels::engine::LevelDetails(key.c_str(), &d) || !erg::install::ValidFileStem(d.file)) return;
    erg::load::BaseFiles f;
    f.key = key;
    f.file = d.file;
    std::wstring where;
    if (!FindLevel(d.file, &f, &where)) return;
    erg::load::Loaded L;
    std::string err;
    if (!erg::load::LoadScene(f, &L, &err)) {
        LOG_WARN("[tints] %s: cannot read the level (%s)", key.c_str(), err.c_str());
        return;
    }
    std::vector<lg::Entry> out;
    for (const erg::Detail& det : L.scene.details) {
        lg::Entry e;
        if (!lg::ParseTag(det.name, e.rgba)) continue;
        erg::Mat3x4 m;
        if (!erg::FrameWorld(L.scene, det.frame, &m)) continue;
        const erg::Vec3 p = erg::Apply(m, det.pos);
        double s = 0;
        for (int k = 0; k < 3; ++k) {
            const double col = std::sqrt(m.m[0][k] * m.m[0][k] + m.m[1][k] * m.m[1][k] + m.m[2][k] * m.m[2][k]);
            s += col * std::fabs(det.scale[k]);
        }
        for (int k = 0; k < 3; ++k) e.pos[k] = static_cast<float>(p[k]);
        e.scale = static_cast<float>(s / 3);
        e.rgba[3] = 1.f;
        out.push_back(e);
    }
    g_st.details = L.scene.details.size();
    g_st.file = erg::install::Narrow(where) + "\\" + d.file;
    if (!out.empty()) LOG_INFO("[tints] %s: %zu tinted scenery details of %zu (%s)", key.c_str(), out.size(), g_st.details, g_st.file.c_str());
    g_table.Set(std::move(out));
}

void OnFrame() {
    const uint64_t f = events::FrameCount();
    if (f - g_checked < 15) return;
    g_checked = f;
    if (!sim::InMatch()) {
        if (!g_key.empty()) {
            g_key.clear();
            g_table.Set({});
        }
        return;
    }
    // An Erg Test or a map pack plays another level than WXD.Level.Current names: the level hook's override, when the
    // hook saw this set-up (its frontend key is the current one).
    const char* from = nullptr;
    const std::string cur = levels::engine::CurrentLevelKey(), loading = levels::engine::LoadingLevelKey(&from);
    const std::string key = !loading.empty() && from && cur == from ? loading : cur;
    if (key == g_key) return;
    g_key = key;
    if (!key.empty()) Load(key);
}

// tints.stats: the table and the draws matched since it was loaded; with an argument, every entry.
bool VerbStats(std::string_view args, void*) {
    LOG_INFO("[tints] level '%s' (%s): %zu tinted of %zu details; %llu lookups, %llu matched", g_key.c_str(), g_st.file.c_str(),
             g_table.Size(), g_st.details, static_cast<unsigned long long>(g_st.lookups), static_cast<unsigned long long>(g_st.hits));
    if (!args.empty())
        for (const lg::Entry& e : g_table.Entries())
            LOG_INFO("[tints]   at %.3f %.3f %.3f scale %.3f colour %.3f %.3f %.3f", e.pos[0], e.pos[1], e.pos[2], e.scale, e.rgba[0], e.rgba[1], e.rgba[2]);
    return true;
}

// tints.probe: logs the model matrices of the next draws through an mg_tint program (translation and scale), so a
// mismatch between the table and the engine's matrices can be read off the log.
int g_probe = 0;
bool VerbProbe(std::string_view args, void*) {
    g_probe = args.empty() ? 64 : std::atoi(std::string(args).c_str());
    return true;
}

class MirageTints final : public melange::Module {
  public:
    const char* Name() const override { return "MirageTints"; }
    const char* Description() const override { return "scenery tints: per-copy colour for GLSL replacements (mg_tint)"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 46; }
    bool Install() override {
        testcmd::Register("tints.stats", &VerbStats);
        testcmd::Register("tints.probe", &VerbProbe);
        events::Subscribe(events::Event::Frame, &OnFrame);
        return true;
    }
};
MELANGE_MODULE(MirageTints);


// Row-vector 4x4 product (engine layout: translation in [12..14]).
void Mul(const float* a, const float* b, float* out) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float v = 0.f;
            for (int k = 0; k < 4; ++k) v += a[i * 4 + k] * b[k * 4 + j];
            out[i * 4 + j] = v;
        }
}
}  // namespace

bool Any() { return g_table.Size() > 0; }

const float* ForDraw(const float* model, const float* view) {
    if ((!model && !view) || g_table.Size() == 0) return kWhite;
    static uint64_t camFrame = ~0ull;
    static render::Camera cam{};
    const uint64_t f = events::FrameCount();
    if (f != camFrame) {
        camFrame = f;
        if (!render::GetCamera(&cam)) cam.valid = false;
    }
    float w[16];
    if (view && cam.valid) {
        float mv[16];
        if (model) Mul(model, view, mv);
        else std::copy(view, view + 16, mv);
        Mul(mv, cam.invView, w);
    } else if (model) {
        std::copy(model, model + 16, w);
    } else {
        return kWhite;
    }
    ++g_st.lookups;
    const lg::Entry* e = g_table.Find(w);
    if (g_probe > 0) {
        // distinct non-origin draws only, with the nearest table entry (map units: world / 20 and as is)
        static float last[3] = {};
        if ((std::fabs(w[12]) + std::fabs(w[13]) + std::fabs(w[14]) > 1e-3f) && (w[12] != last[0] || w[13] != last[1] || w[14] != last[2])) {
            std::copy(w + 12, w + 15, last);
            --g_probe;
            float best = 1e30f, bestK = 0.f, bestScale = 0.f;
            for (float k : {1.f, 20.f})
                for (const lg::Entry& en : g_table.Entries()) {
                    const float d0 = w[12] / k - en.pos[0], d1 = w[13] / k - en.pos[1], d2 = w[14] / k - en.pos[2];
                    const float d = std::sqrt(d0 * d0 + d1 * d1 + d2 * d2);
                    if (d < best) best = d, bestK = k, bestScale = en.scale;
                }
            LOG_INFO("[tints] probe: world t %.3f %.3f %.3f scale %.3f -> %s; nearest entry %.3f map units away (units /%g, its scale %.3f)", w[12], w[13], w[14],
                     lg::RowScale(w), e ? "tinted" : "-", best, bestK, bestScale);
        }
    }
    if (!e) return kWhite;
    ++g_st.hits;
    return e->rgba;
}
}  // namespace melange::mirage::tints
