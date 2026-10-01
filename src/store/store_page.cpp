// Overlay panel "Thumper/Store": the list on the left, the details on the right, confirms as modals.
#include <windows.h>

#include <shellapi.h>

#include <imgui.h>

#include <cstdio>
#include <string>
#include <vector>

#include "melange/draw.h"
#include "melange/mods.h"
#include "melange/overlay.h"
#include "mods/spice.h"
#include "mods/thumper_internal.h"
#include "render/internal.h"
#include "stb_image.h"
#include "store/store.h"

namespace melange::store {
namespace {
constexpr const char* kPrivacy =
    "The Store downloads the plugin list and the files you choose from GitHub, only when you open it or press a button. "
    "Nothing about you or your game is sent.";
constexpr const char* kDeepDesert =
    "This plugin asks for Deep Desert: raw access to the game's memory. It will run sandboxed until you allow it in the "
    "game's overlay. Only allow it if you trust the author.";
const char* const kCategories[] = {"graphics", "gameplay", "maps", "weapons", "audio", "interface", "tools", "libraries"};

const ImVec4 kWarn(1.f, 0.75f, 0.3f, 1.f), kBad(1.f, 0.45f, 0.35f, 1.f), kOk(0.55f, 0.9f, 0.55f, 1.f), kInfo(0.6f, 0.75f, 1.f, 1.f);

char g_search[128] = {};
std::string g_category, g_selected;
int g_filter = 0;

struct Tex { int n = 0; unsigned id = 0; int w = 0, h = 0; };
std::string g_texFor;
std::vector<Tex> g_tex;

enum class Ask { None, Install, Remove };
Ask g_ask = Ask::None;
bool g_openAsk = false;
std::string g_askId, g_askVersion;
bool g_askOlder = false, g_enableAfter = true, g_deleteData = false;
std::string g_actionError;

std::string Size(uint64_t n) {
    char buf[32];
    if (n >= (1u << 20)) snprintf(buf, sizeof buf, "%.1f MiB", n / 1048576.0);
    else snprintf(buf, sizeof buf, "%.0f KiB", n < 1024 ? 1.0 : n / 1024.0);
    return buf;
}

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + v[i];
    return s;
}

void FreeTextures() {
    for (const Tex& t : g_tex) draw::FreeTexture(t.id);
    g_tex.clear();
}

void Act(Outcome o) {
    g_actionError = o.code ? o.message : "";
}

void AskInstall(const Item& it, const std::string& version, bool older) {
    g_ask = Ask::Install;
    g_openAsk = true;
    g_askId = it.id;
    g_askVersion = version;
    g_askOlder = older;
    g_enableAfter = true;
}

void AskRemove(const Item& it) {
    g_ask = Ask::Remove;
    g_openAsk = true;
    g_askId = it.id;
    g_deleteData = false;
}

void Badge(const char* text, const ImVec4& c) {
    ImGui::SameLine();
    ImGui::TextColored(c, "[%s]", text);
}

void ActionButton(const Item& it, const std::string& gate, bool busy) {
    const char* label = it.action == Action::Install ? (it.installed ? "Replace" : "Install")
                        : it.action == Action::Update ? "Update" : it.action == Action::Remove ? "Remove" : nullptr;
    if (!label) return;
    const bool blocked = !gate.empty() || busy;
    ImGui::BeginDisabled(blocked);
    if (ImGui::SmallButton(label)) {
        if (it.action == Action::Remove) AskRemove(it);
        else AskInstall(it, it.compatible, false);
    }
    ImGui::EndDisabled();
    if (blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", gate.empty() ? "another install, update or remove is running" : gate.c_str());
    if (it.action == Action::Update && it.canRemove) {
        ImGui::SameLine();
        ImGui::BeginDisabled(blocked);
        if (ImGui::SmallButton("Remove")) AskRemove(it);
        ImGui::EndDisabled();
    }
}

void DrawConfirm() {
    if (g_openAsk) {
        ImGui::OpenPopup("Store: confirm");
        g_openAsk = false;
    }
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Store: confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    Details d;
    if (!GetDetails(g_askId, &d, false)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const Item& it = d.item;
    ImGui::PushTextWrapPos(460.f);
    if (g_ask == Ask::Install) {
        const bool update = it.installed && it.managed;
        ImGui::Text("%s %s %s", update ? "Update" : "Install", it.name.c_str(), g_askVersion.c_str());
        ImGui::TextDisabled("%s, %s", Size(it.size).c_str(), it.kind.c_str());
        if (it.installed && !it.managed)
            ImGui::TextColored(kWarn, "Replace the copy in Mods\\%s (version %s)?", it.id.c_str(), it.installedVersion.c_str());
        if (g_askOlder) ImGui::TextColored(kWarn, "This is older than what you have.");
        if (it.unsafe) {
            ImGui::TextColored(kWarn, "%s", kDeepDesert);
            if (it.installed) ImGui::TextColored(kWarn, "Updates ask for Deep Desert again.");
        }
        if (d.content) ImGui::TextUnformatted("Content: everyone in an online match needs the same version.");
        std::vector<std::string> also;
        for (const Step& s : d.plan)
            if (s.id != it.id) also.push_back(s.id + " " + s.version);
        if (!also.empty()) ImGui::Text("Also installs: %s", Join(also).c_str());
        if (!d.planError.empty()) ImGui::TextColored(kBad, "%s", d.planError.c_str());
        if (!d.conflictsEnabled.empty())
            ImGui::TextColored(kWarn, "Conflicts with %s: Thumper will block both while they are enabled.", Join(d.conflictsEnabled).c_str());
        if (!it.installed) ImGui::Checkbox("Enable after install", &g_enableAfter);
    } else {
        ImGui::Text("Remove %s %s?", it.name.c_str(), it.installedVersion.c_str());
        if (!d.dependants.empty()) ImGui::TextColored(kWarn, "%s need this; they will be blocked.", Join(d.dependants).c_str());
        ImGui::Checkbox("Also delete its settings and saved data", &g_deleteData);
    }
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    if (ImGui::Button(g_ask == Ask::Install ? "Install" : "Remove")) {
        if (g_ask == Ask::Install) Act(Install(it.id, g_askVersion, g_enableAfter, it.installed && !it.managed));
        else Act(Remove(it.id, g_deleteData));
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawShots(const Details& d) {
    if (g_texFor != d.item.id) {
        FreeTextures();
        g_texFor = d.item.id;
    }
    for (const ShotRow& r : d.screenshots) {
        const Tex* t = nullptr;
        for (const Tex& x : g_tex)
            if (x.n == r.n) t = &x;
        if (!t && r.ready && g_tex.size() < 6) {
            const std::wstring path = ShotPath(d.item.id, r.n);
            Tex x;
            x.n = r.n;
            if (FILE* f = _wfopen(path.c_str(), L"rb")) {
                int comp = 0;
                stbi_info_from_file(f, &x.w, &x.h, &comp);
                fclose(f);
            }
            x.id = path.empty() ? 0 : draw::LoadTexture(path.c_str());
            if (x.id) {
                g_tex.push_back(x);
                t = &g_tex.back();
            }
        }
        if (t && t->w > 0) {
            const float w = (std::min)(ImGui::GetContentRegionAvail().x, 480.f);
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(t->id)), ImVec2(w, w * t->h / t->w));
        } else {
            ImGui::TextDisabled("(screenshot %d loading)", r.n);
        }
        if (!r.caption.empty()) ImGui::TextDisabled("%s", r.caption.c_str());
    }
}

void DrawDetails(const std::string& gate, bool busy) {
    Details d;
    if (g_selected.empty() || !GetDetails(g_selected, &d, true)) {
        ImGui::TextDisabled("Select a plugin to see its details.");
        return;
    }
    const Item& it = d.item;
    ImGui::Text("%s", it.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", it.latest.c_str());
    ImGui::TextDisabled("%s  by %s", it.id.c_str(), Join(it.authors).c_str());
    ActionButton(it, gate, busy);
    if (it.installed && !it.managed) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Open folder"))
            ShellExecuteW(nullptr, L"open", (std::wstring(mods::ModsDir()) + L"\\" + std::wstring(it.id.begin(), it.id.end())).c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::PushTextWrapPos(0.f);
    if (!it.reason.empty()) ImGui::TextColored(it.state == "incompatible" ? kBad : kWarn, "%s", it.reason.c_str());
    if (!it.error.empty()) ImGui::TextColored(kBad, "%s", it.error.c_str());
    ImGui::Separator();
    ImGui::TextUnformatted(it.description.c_str());
    ImGui::TextDisabled("Licence: %s   Categories: %s", it.licence.c_str(), Join(it.categories).c_str());
    if (!d.homepage.empty()) {
        if (ImGui::SmallButton(d.homepage.c_str())) {
            std::string err;
            OpenHomepage(it.id, &err);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens in your browser");
    }
    std::string perms = it.unsafe ? "Deep Desert (raw access to the game's memory)" : "sandboxed Lua only";
    if (d.filesystem != "none") perms += ", files: " + d.filesystem;
    ImGui::Text("Permissions: %s", perms.c_str());
    if (d.content) ImGui::TextColored(kInfo, "Content: everyone in an online match needs the same version.");
    if (!d.dependencies.empty()) {
        std::vector<std::string> v;
        for (const Dep& x : d.dependencies) v.push_back(x.id + (x.range.empty() ? "" : " " + x.range));
        ImGui::Text("Needs: %s", Join(v).c_str());
    }
    if (!d.conflicts.empty()) {
        std::vector<std::string> v;
        for (const Dep& x : d.conflicts) v.push_back(x.id);
        ImGui::Text("Conflicts with: %s", Join(v).c_str());
    }
    ImGui::PopTextWrapPos();
    if (!d.screenshots.empty() && ImGui::CollapsingHeader("Screenshots", ImGuiTreeNodeFlags_DefaultOpen)) DrawShots(d);
    if (ImGui::CollapsingHeader("Versions")) {
        for (const VersionRow& v : d.versions) {
            ImGui::PushID(v.version.c_str());
            ImGui::Text("%s", v.version.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s  %s  Melange %s", v.released.c_str(), Size(v.size).c_str(), v.melange.empty() ? "any" : v.melange.c_str());
            if (v.yanked) Badge("withdrawn", kBad);
            else if (!v.compatible) Badge("incompatible", kBad);
            if (v.compatible && v.version != it.installedVersion && v.version != it.compatible) {
                ImGui::SameLine();
                ImGui::BeginDisabled(!gate.empty() || busy);
                const bool older = it.installed && spice::SemverCompare(v.version, it.installedVersion) < 0;
                if (ImGui::SmallButton("Install this version")) AskInstall(it, v.version, older);
                ImGui::EndDisabled();
            }
            if (!v.changelog.empty()) {
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextDisabled("%s", v.changelog.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::PopID();
        }
    }
}

void DrawPanel(void*) {
    EnsureFetched();
    const Status s = GetStatus();
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", kPrivacy);
    if (s.customIndex) ImGui::TextColored(ImVec4(1.f, 0.9f, 0.2f, 1.f), "Custom index: %s", s.indexUrl.c_str());
    if (s.fetching) ImGui::TextUnformatted("Fetching the list...");
    else if (s.offline && s.haveIndex) ImGui::TextColored(kWarn, "Offline: showing the list from %s (%s)", s.fetchedAt.c_str(), s.error.c_str());
    else if (!s.error.empty()) ImGui::TextColored(kBad, "%s", s.error.c_str());
    else if (s.haveIndex) ImGui::Text("Fetched %s, %zu plugins", s.fetchedAt.c_str(), s.plugins);
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(s.fetching);
    if (ImGui::Button("Refresh")) Refresh();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Mods")) overlay::OpenPanel("thumper.mods");
    ImGui::PushTextWrapPos(0.f);
    if (s.rollback) ImGui::TextColored(kBad, "%s", kRollbackText);
    if (!s.gate.empty()) ImGui::TextColored(kWarn, "Install, update and remove are paused: %s", s.gate.c_str());
    for (const std::string& n : s.notices) ImGui::TextColored(kInfo, "%s", n.c_str());
    if (!g_actionError.empty()) ImGui::TextColored(kBad, "%s", g_actionError.c_str());
    const Job& j = s.job;
    if (j.phase == "downloading" || j.phase == "verifying" || j.phase == "installing" || j.phase == "removing") {
        char overlay[96];
        snprintf(overlay, sizeof overlay, "%s %s %s", j.phase.c_str(), j.id.c_str(), j.total ? (Size(j.bytes) + " / " + Size(j.total)).c_str() : "");
        ImGui::ProgressBar(j.total ? static_cast<float>(j.bytes) / static_cast<float>(j.total) : 0.f, ImVec2(-90.f, 0), overlay);
        if (j.phase == "downloading") {
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) Cancel();
        }
    } else if (j.phase == "done") {
        ImGui::TextColored(kOk, "%s", j.message.c_str());
    } else if (j.phase == "error") {
        ImGui::TextColored(kBad, "%s %s: %s", j.id.c_str(), j.version.c_str(), j.message.c_str());
    }
    ImGui::PopTextWrapPos();

    ImGui::SetNextItemWidth(220.f);
    ImGui::InputTextWithHint("##search", "Search", g_search, sizeof g_search);
    ImGui::SameLine();
    ImGui::RadioButton("All", &g_filter, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Installed", &g_filter, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Updates", &g_filter, 2);
    if (ImGui::Selectable("any", g_category.empty(), 0, ImVec2(ImGui::CalcTextSize("any").x, 0))) g_category.clear();
    for (const char* c : kCategories) {
        ImGui::SameLine();
        if (ImGui::Selectable(c, g_category == c, 0, ImVec2(ImGui::CalcTextSize(c).x, 0))) g_category = g_category == c ? "" : c;
    }

    ListQuery q;
    q.query = g_search;
    q.category = g_category;
    q.filter = g_filter == 1 ? "installed" : g_filter == 2 ? "updates" : "all";
    const std::vector<Item> items = List(q);

    const float left = ImGui::GetContentRegionAvail().x * 0.45f;
    ImGui::BeginChild("store.list", ImVec2(left, 420), ImGuiChildFlags_Borders);
    if (!s.haveIndex) ImGui::TextDisabled(s.fetching ? "Loading..." : "No list yet.");
    else if (items.empty()) ImGui::TextDisabled("No plugin matches.");
    for (const Item& it : items) {
        ImGui::PushID(it.id.c_str());
        if (ImGui::Selectable(it.name.c_str(), g_selected == it.id)) g_selected = it.id;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", (it.installed ? it.installedVersion : it.latest).c_str());
        ImGui::TextDisabled("%s  %s", Join(it.authors).c_str(), Size(it.size).c_str());
        Badge(it.kind.c_str(), it.kind == "content" ? kInfo : ImVec4(0.7f, 0.7f, 0.7f, 1.f));
        if (it.unsafe) Badge("Deep Desert", kWarn);
        if (it.state == "update") ImGui::TextColored(kOk, "Update to %s", it.compatible.c_str());
        else if (it.state == "pending") ImGui::TextColored(kInfo, "Applies at next launch");
        else if (it.installed && it.state != "manual") ImGui::TextColored(kOk, "Installed %s", it.installedVersion.c_str());
        if (!it.reason.empty() && it.state != "pending") ImGui::TextColored(it.state == "incompatible" ? kBad : kWarn, "%s", it.reason.c_str());
        if (!it.error.empty()) ImGui::TextColored(kBad, "%s", it.error.c_str());
        ActionButton(it, s.gate, s.busy);
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("store.details", ImVec2(0, 420), ImGuiChildFlags_Borders);
    DrawDetails(s.gate, s.busy);
    ImGui::EndChild();
    DrawConfirm();
    thumper::DrawConsentModals();
}
}  // namespace

void RegisterPage() {
    const int h = overlay::AddPanel("thumper.store", "Thumper/Store", &DrawPanel, nullptr);
    render::SetPanelDefaultRect(h, 80.f, 40.f, 900.f, 640.f);
}
}  // namespace melange::store
