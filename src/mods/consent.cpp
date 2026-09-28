// Deep Desert: the wum.unsafe consent modal, its persisted grant, and the always-visible HUD marker.
#include <imgui.h>

#include <cstdint>
#include <ctime>
#include <string>

#include "core/events.h"
#include "core/game.h"
#include "melange/draw.h"
#include "melange/jlog.h"
#include "melange/render.h"
#include "mods/thumper_internal.h"
#include "tools/hash.h"

namespace melange::thumper {
namespace {
std::string g_pendingConsentId;  // set by RequestConsent(); drained by DrawConsentModals()
bool g_openThisFrame = false;

std::string NowIso() {
    time_t t = time(nullptr);
    tm utc{};
    gmtime_s(&utc, &t);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}
}  // namespace

// grantHash: sha256(a per-install salt || permissions fields || authors || sha256(entry.client's bytes)). Any
// change to what code runs with Deep Desert, to the declared permissions, or to the authors, changes this and
// drops an existing grant. The salt (kept in Melange.ini, never under Mods\) stops a mod archive from shipping
// a pre-computed grant record for itself in a dropped thumper-state.json.
std::string GrantHash(const spice::Manifest& m) {
    std::string clientHash;
    if (!m.entryClient.empty()) {
        std::wstring path = m.dir + L"\\" + melange::game::Widen(m.entryClient);
        clientHash = hashutil::Sha256HexFile(path);
    }
    std::string authors;
    for (const std::string& a : m.authors) authors += (authors.empty() ? "" : ",") + a;
    std::string text = "salt=" + GrantSalt() + ";unsafe=" + std::string(m.unsafe ? "1" : "0") +
                        ";filesystem=" + m.filesystem + ";authors=" + authors + ";entryClient=" + clientHash;
    return hashutil::Sha256Hex(text.data(), text.size());
}

bool IsGranted(const Entry& e) {
    if (!e.manifest.unsafe) return true;  // the flag is meaningless for a mod that never asked
    auto it = Live().deepDesert.find(e.manifest.id);
    if (it == Live().deepDesert.end() || !it->second.granted) return false;
    return it->second.grantHash == GrantHash(e.manifest);
}

void RequestConsent(const std::string& id) {
    g_pendingConsentId = id;
    g_openThisFrame = true;
}

void DrawConsentModals() {
    // Both Thumper/Mods and Thumper/Deep Desert call this; only the first call in a given frame may
    // touch ImGui's popup state, or BeginPopupModal("Deep Desert") would run twice in one frame.
    static uint64_t lastFrame = ~0ull;
    uint64_t frame = events::FrameCount();
    if (frame == lastFrame) return;
    lastFrame = frame;

    if (g_openThisFrame) {
        ImGui::OpenPopup("Deep Desert");
        g_openThisFrame = false;
    }
    if (g_pendingConsentId.empty()) return;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Deep Desert", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    Entry e;
    if (!FindEntry(g_pendingConsentId, &e)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        g_pendingConsentId.clear();
        return;
    }
    ImGui::TextWrapped("%s wants Deep Desert access: it can read and write the game's memory directly and call "
                        "internal game functions. This can crash the game or corrupt your save, and Melange cannot "
                        "sandbox what it does with this access.",
                        e.manifest.name.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("%s  v%s  by %s", e.manifest.id.c_str(), e.manifest.version.c_str(),
                         e.manifest.authors.empty() ? "unknown" : e.manifest.authors.front().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Allow", ImVec2(120, 0))) {
        DeepDesertRecord r;
        r.granted = true;
        r.grantHash = GrantHash(e.manifest);
        r.author = e.manifest.authors.empty() ? "" : e.manifest.authors.front();
        r.at = NowIso();
        Live().deepDesert[e.manifest.id] = r;
        Save();
        jlog::Rec("thumper", jlog::Level::Info, "deep_desert_grant").Str("id", e.manifest.id);
        Rescan();
        ImGui::CloseCurrentPopup();
        g_pendingConsentId.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep sandboxed", ImVec2(160, 0))) {
        DeepDesertRecord r;
        r.granted = false;
        r.at = NowIso();
        Live().deepDesert[e.manifest.id] = r;
        Save();
        jlog::Rec("thumper", jlog::Level::Info, "deep_desert_declined").Str("id", e.manifest.id);
        ImGui::CloseCurrentPopup();
        g_pendingConsentId.clear();
    }
    ImGui::EndPopup();
}

void DrawDeepDesertMarker() {
    bool any = false;
    for (const Entry& e : Snapshot())
        if (e.manifest.unsafe && e.deepDesertGranted && e.sessionActive) {
            any = true;
            break;
        }
    if (!any) return;
    int w = 0, h = 0;
    render::WindowSize(&w, &h);
    draw::HudText(static_cast<float>(w) - 130.f, 6.f, "Deep Desert active", 0xff40c0ffu, 14.f, 1);
}
}  // namespace melange::thumper
