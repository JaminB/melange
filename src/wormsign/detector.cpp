#include "wormsign/detector.h"

#include <windows.h>

#include <shlobj.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "melange/draw.h"
#include "melange/jlog.h"
#include "melange/lua.h"
#include "melange/mods.h"
#include "melange/overlay.h"
#include "melange/testcmd.h"
#include "mods/lobby.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "tools/sysinfo.h"
#include "version.h"
#include "wormsign/bundle.h"
#include "wormsign/divergence.h"
#include "wormsign/enginecheck.h"
#include "wormsign/exchange.h"
#include "wormsign/library.h"
#include "wormsign/session.h"

namespace melange::wormsign::detector {
namespace {
namespace lobby = handshake::lobby;

constexpr uint64_t kLobbyPollMs = 1000, kToastMs = 8000;
constexpr int kMaxPacketsPerFrame = 128;
constexpr size_t kMaxRecordingBytes = 256u << 20;

Options g_opt;
bool g_installed = false;
int g_tickHandle = 0, g_sessionHandle = 0, g_panel = 0;

ContribNamesFn g_contribNames = nullptr;
ContribHashesFn g_contribHashes = nullptr;
DetailFn g_detail = nullptr;
RecordingFn g_recording = nullptr;

uint64_t g_lobbyAdvertised = 0, g_lobbyPollMs = 0;
uint32_t g_serial = 0;
std::string g_correlation;
bool g_correlated = false;

std::mutex g_bundleMx;
std::wstring g_lastBundle;
std::vector<std::wstring> g_bundleDone;  // written paths ("" on failure), picked up on the main thread

std::string g_toast;
uint64_t g_toastMs = 0;

uint64_t NowMs() { return GetTickCount64(); }

std::string Hex(uint64_t v) {
    char b[20];
    snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

void Toast(const std::string& s) {
    if (g_opt.onDesync != OnDesync::Report) return;
    g_toast = s;
    g_toastMs = NowMs();
}

std::string FallbackDetail(uint32_t tick) {
    TickHash h{};
    if (!TickAt(tick, &h)) return {};
    jsonmini::Obj c;
    for (int i = 0; i < kEngineComps; ++i) c.Str(detect::CompName(i), Hex(h.c[i]));
    jsonmini::Obj o;
    // rng2 is left out, as DetailJson (detail::ToJson minus rng2) does for the normal detail path: it is never
    // simulation state and always differs between machines, so it would misname itself as the differing field.
    o.UInt("tick", h.tick).Str("engine", Hex(h.engine)).Str("mods", Hex(h.mods)).Raw("components", c.End())
        .UInt("rngLogic", h.rngLogic).UInt("fpucw", h.fpucw).UInt("inputs", h.inputs);
    return o.End();
}

std::wstring DocumentsMelange() {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path)) && path) out = std::wstring(path) + L"\\Melange";
    if (path) CoTaskMemFree(path);
    return out;
}

std::string ProfileName() {
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &path)) && path) {
        const std::wstring w = path;
        const size_t s = w.find_last_of(L"\\/");
        out = game::Narrow(s == std::wstring::npos ? w : w.substr(s + 1));
    }
    if (path) CoTaskMemFree(path);
    return out;
}

bool ReadFileTail(const std::wstring& path, uint64_t cap, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const uint64_t total = static_cast<uint64_t>(size.QuadPart), want = total > cap ? cap : total;
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(total - want);
    SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
    out->resize(static_cast<size_t>(want));
    DWORD got = 0;
    const bool ok = want == 0 || ReadFile(f, out->data(), static_cast<DWORD>(want), &got, nullptr);
    out->resize(got);
    CloseHandle(f);
    return ok;
}

std::string ModsJson() {
    std::vector<mods::ModInfo> v(256);
    const int n = (std::min)(mods::List(v.data(), static_cast<int>(v.size())), static_cast<int>(v.size()));
    jsonmini::Arr a;
    for (int i = 0; i < n; ++i) {
        const mods::ModInfo& m = v[i];
        if (m.state != mods::State::Enabled) continue;
        jsonmini::Obj o;
        o.Str("id", m.id ? m.id : "").Str("version", m.version ? m.version : "")
            .Str("kind", m.kind == mods::Kind::Content ? "content" : "client-only").Bool("sim", m.hasSim)
            .Bool("client", m.hasClient);
        a.Raw(o.End());
    }
    return a.End();
}

// ---------------------------------------------------------------- the detector's environment
struct BundleJob {
    uint32_t serial = 0;
    std::shared_ptr<const bundle::Inputs> base;  // everything the main thread gathered
    std::wstring wsr, dir, path;
};
std::vector<BundleJob> g_jobs;                   // this match's bundles, rewritten once the engine's check fails
void WriteBundle(const BundleJob& job);

class GameEnv final : public detect::Env {
  public:
    uint64_t NowMs() override { return GetTickCount64(); }
    bool Send(uint64_t to, const std::vector<uint8_t>& packet, bool reliable) override {
        return exchange::Send(to, packet, reliable);
    }
    bool OurTick(uint32_t tick, TickHash* out) override { return TickAt(tick, out); }
    std::string Detail(uint32_t tick) override {
        std::string s;
        if (g_detail && g_detail(tick, &s) && !s.empty() && s.size() <= wire::kMaxDetailBytes) return s;
        return FallbackDetail(tick);
    }
    bool ContribHashes(uint32_t tick, std::vector<uint64_t>* out) override {
        out->clear();
        return g_contribHashes && g_contribHashes(tick, out);
    }
    void Diverged(const Divergence& d) override { Raise(d); }
    void Bundle(const detect::Report& r) override;
    void Note(bool warn, const std::string& text) override {
        if (warn) LOG_WARN("[wormsign] %s", text.c_str());
        else LOG_INFO("[wormsign] %s", text.c_str());
        jlog::Rec("wormsign", warn ? jlog::Level::Warn : jlog::Level::Info, "exchange").Str("note", text);
    }
};
GameEnv g_env;
std::unique_ptr<detect::Detector> g_det;

std::string PeersJson(const std::string& salt) {
    detect::PeerStatus v[detect::Detector::kMaxPeers];
    const int n = g_det ? g_det->Peers(v, static_cast<int>(detect::Detector::kMaxPeers)) : 0;
    jsonmini::Arr a;
    for (int i = 0; i < n; ++i) {
        jsonmini::Obj o;
        o.Str("peer", bundle::PeerRef(salt, v[i].steamId)).Str("state", detect::PeerStateName(v[i].state))
            .UInt("protocol", wire::kProtocol).Str("melange", v[i].melange).Str("content", v[i].content)
            .Bool("contributorsMatch", v[i].modsCompared).UInt("compared", v[i].compared)
            .UInt("lastCommonTick", v[i].lastCommonTick).UInt("theirTick", v[i].theirTick)
            .UInt("divergedTick", v[i].divergedTick);
        a.Raw(o.End());
    }
    return a.End();
}

void GameEnv::Bundle(const detect::Report& r) {
    auto in = std::make_shared<bundle::Inputs>();
    in->salt = hashutil::RandomSalt();
    in->div = r.div;
    memcpy(in->ours, r.ours, sizeof in->ours);
    memcpy(in->theirs, r.theirs, sizeof in->theirs);
    in->haveOurs = r.haveOurs;
    in->haveTheirs = r.haveTheirs;
    in->contribListsMatch = r.contribListsMatch;
    for (size_t i = 0; i < r.contribNames.size(); ++i) {
        bundle::Contributor c;
        c.name = r.contribNames[i].name;
        c.version = r.contribNames[i].version;
        c.haveOurs = i < r.oursContrib.size();
        if (c.haveOurs) c.ours = r.oursContrib[i];
        c.haveTheirs = r.contribListsMatch && i < r.theirsContrib.size();
        if (c.haveTheirs) c.theirs = r.theirsContrib[i];
        in->contributors.push_back(c);
    }
    in->peerName = r.peerName;
    in->peersJson = PeersJson(in->salt);
    in->engineJson = enginecheck::Json(enginecheck::Records(r.div.serial));
    in->correlation = g_correlation;
    in->detailLocal = r.detailLocal;
    in->detailPeer = r.detailPeer;
    in->modsJson = ModsJson();
    in->melangeVersion = MELANGE_VERSION;
    in->exeBuild = game::Exe().build;
    BundleJob job;
    job.serial = r.div.serial;
    job.base = in;
    if (!g_recording || !g_recording(r.div.serial, &job.wsr)) job.wsr.clear();
    job.dir = ReplaysDir();
    SYSTEMTIME st;
    GetLocalTime(&st);
    job.path = job.dir + L"\\" + bundle::FileName(r.div.serial, r.div.tick, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                                                  st.wSecond, GetCurrentProcessId());
    WriteBundle(job);
    g_jobs.push_back(std::move(job));
}

void WriteBundle(const BundleJob& job) {
    auto in = std::make_shared<bundle::Inputs>(*job.base);
    std::thread([in, wsr = job.wsr, dir = job.dir, path = job.path] {
        wchar_t name[256];
        DWORD n = 256;
        if (GetUserNameW(name, &n)) in->userName = game::Narrow(name);
        n = 256;
        if (GetComputerNameW(name, &n)) in->computerName = game::Narrow(name);
        in->profileName = ProfileName();
        std::string log;
        if (ReadFileTail(game::DataDir() + L"\\Melange.log", 4u << 20, &log)) in->melangeLog = detect::LogTailSeconds(log, 120);
        jlog::Flush(1000);
        std::vector<jlog::Line> lines;
        jlog::Tail(0, lines, 20000);
        const double last = lines.empty() ? 0 : lines.back().t;
        for (const jlog::Line& l : lines)
            if (l.t >= last - 120) in->jlog += l.json + "\n";
        in->sysinfo = sysinfo::CollectJson();
        if (!wsr.empty()) {
            const std::wstring tmp = path + L".wsr.tmp";
            std::string bytes, err;
            if (library::ExportRedacted(wsr, tmp, &err, in->salt) && ReadFileTail(tmp, kMaxRecordingBytes, &bytes))
                in->recording.assign(bytes.begin(), bytes.end());
            DeleteFileW(tmp.c_str());
        }
        std::string zip;
        std::wstring result;
        if (bundle::BuildZip(*in, &zip)) {
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                const bool ok = WriteFile(f, zip.data(), static_cast<DWORD>(zip.size()), &w, nullptr) && w == zip.size();
                CloseHandle(f);
                if (ok) result = path;
                else DeleteFileW(path.c_str());
            }
        }
        if (!result.empty()) library::OnBundleWritten(result);
        std::lock_guard lk(g_bundleMx);
        g_bundleDone.push_back(result);
    }).detach();
}

// ---------------------------------------------------------------- per frame
void PickUpBundles() {
    std::vector<std::wstring> done;
    {
        std::lock_guard lk(g_bundleMx);
        done.swap(g_bundleDone);
        for (const auto& path : done)
            if (!path.empty()) g_lastBundle = path;
    }
    for (const auto& path : done) {
        if (path.empty()) {
            LOG_ERROR("[wormsign] writing the desync bundle failed");
            continue;
        }
        LOG_WARN("[wormsign] desync bundle saved: %s", game::Narrow(path).c_str());
        jlog::Rec("wormsign", jlog::Level::Warn, "desync bundle").Str("path", game::Narrow(path));
        if (!g_toast.empty() && NowMs() - g_toastMs < kToastMs && g_toast.find("bundle saved") == std::string::npos)
            Toast(g_toast + ", bundle saved");
    }
}

void Correlate() {
    uint32_t flagged = 0;
    if (g_correlated || !g_det || !g_det->Flagged(&flagged)) return;
    enginecheck::Record r{};
    if (!enginecheck::FirstFailure(g_serial, &r)) return;
    g_correlated = true;
    // Every failed reason, not only the first: a camera failure (7..10) can come with worm ones (11, 13).
    const std::string reasons = enginecheck::DescribeReasons(r.reasons);
    char b[160];
    if (!reasons.empty())
        snprintf(b, sizeof b, "Wormsign flagged tick %u; the engine's turn-end check failed at tick %u (", flagged, r.tick);
    else
        snprintf(b, sizeof b, "Wormsign flagged tick %u; the engine aborted the match at tick %u (%s)", flagged, r.tick,
                 r.error);
    g_correlation = reasons.empty() ? std::string(b) : std::string(b) + reasons + ")";
    LOG_WARN("[wormsign] %s", g_correlation.c_str());
    for (BundleJob& job : g_jobs) {
        if (job.serial != g_serial) continue;
        auto in = std::make_shared<bundle::Inputs>(*job.base);
        in->correlation = g_correlation;
        in->engineJson = enginecheck::Json(enginecheck::Records(job.serial));
        job.base = in;
        WriteBundle(job);
    }
    jlog::Rec("wormsign", jlog::Level::Warn, "engine correlation")
        .Uint("flaggedTick", flagged).Uint("engineTick", r.tick)
        .Int("lagTicks", static_cast<int64_t>(r.tick) - static_cast<int64_t>(flagged))
        .Uint("reason", enginecheck::FirstReason(r.reasons))  // the first, for readers of the single code
        .Uint("reasons", r.reasons).Str("reasonList", enginecheck::ReasonNumbers(r.reasons))
        .Str("reasonText", enginecheck::ReasonTexts(r.reasons)).Str("error", r.error);
}

void PollLobby() {
    const uint64_t now = NowMs();
    if (now - g_lobbyPollMs < kLobbyPollMs) return;
    g_lobbyPollMs = now;
    const uint64_t l = lobby::Current();
    if (l && l != g_lobbyAdvertised) {
        lobby::SetMyData(wire::kLobbyKey, wire::kLobbyValue);
        g_lobbyAdvertised = l;
        jlog::Rec("wormsign", jlog::Level::Info, "exchange advertised").Uint("protocol", wire::kProtocol);
    }
    if (!l) g_lobbyAdvertised = 0;
    std::vector<detect::PeerInfo> peers;
    if (l)
        for (uint64_t id : lobby::Members()) peers.push_back({id, lobby::Name(id), lobby::MemberData(id, wire::kLobbyKey)});
    g_det->SetPeers(peers);
}

void OnPacket(uint64_t from, const uint8_t* p, size_t n, void*) { g_det->OnPacket(from, p, n); }

void DrawToast() {
    if (g_toast.empty() || NowMs() - g_toastMs > kToastMs) return;
    const overlay::GlInfo gl = overlay::Gl();
    const float w = gl.viewportW > 0 ? static_cast<float>(gl.viewportW) : 1280.f;
    const float tw = 10.f * static_cast<float>(g_toast.size()) + 24.f;
    const float x0 = (w - tw) * 0.5f, y0 = 48.f;
    draw::HudRect(x0, y0, x0 + tw, y0 + 32.f, 0xd8101018, true);
    draw::HudRect(x0, y0, x0 + tw, y0 + 32.f, 0xff3080ff, false, 2.f);
    draw::HudText(x0 + 12.f, y0 + 7.f, g_toast.c_str(), 0xff70c0ff, 18.f);
}

void Frame() {
    PickUpBundles();
    DrawToast();
    if (!g_opt.exchange) return;
    PollLobby();
    exchange::Drain(&OnPacket, nullptr, kMaxPacketsPerFrame);
    g_det->Pump();
    Correlate();
}

void OnTick(const TickHash& h, void*) { g_det->OnTick(h); }

void OnSessionFn(bool begin, uint32_t serial, void*) {
    g_serial = serial;
    g_correlated = false;
    g_correlation.clear();
    if (!begin) {
        g_det->End();
        enginecheck::SetActive(false);
        return;
    }
    g_jobs.clear();
    enginecheck::SetActive(lobby::Current() != 0);
    std::vector<wire::ContribName> contribs;
    if (g_contribNames) g_contribNames(&contribs);
    const mods::ContentId c = mods::LocalContent();
    g_det->SetIdentity(MELANGE_VERSION, c.vanilla || !c.hash[0] ? "v" : std::string(c.hash, 16));
    g_det->Begin(serial, lobby::Current() ? wire::MatchKey(lobby::Owner(), lobby::Current()) : 0, std::move(contribs));
}

// ---------------------------------------------------------------- overlay and test verb
void DrawPanel(void*) {
    detect::PeerStatus v[detect::Detector::kMaxPeers];
    const int n = Peers(v, static_cast<int>(detect::Detector::kMaxPeers));
    ImGui::Text("Exchange: %s   match: %s   tick %u", g_opt.exchange ? "on" : "off", InMatch() ? "yes" : "no", Tick());
    if (!lobby::Current()) {
        ImGui::TextDisabled("Not in a lobby: no exchange.");
    } else if (!n) {
        ImGui::TextDisabled("No other lobby members.");
    } else if (ImGui::BeginTable("ws.peers", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Player");
        ImGui::TableSetupColumn("Exchange");
        ImGui::TableSetupColumn("Last common tick");
        ImGui::TableSetupColumn("Lag (ticks)");
        ImGui::TableSetupColumn("Compared");
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; ++i) {
            const detect::PeerStatus& s = v[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.name);
            ImGui::TableNextColumn();
            const ImVec4 col = s.state == PeerState::Diverged     ? ImVec4(1.f, .45f, .3f, 1.f)
                               : s.state == PeerState::Exchanging ? ImVec4(.4f, .9f, .4f, 1.f)
                                                                  : ImVec4(.7f, .7f, .7f, 1.f);
            if (s.state == PeerState::Diverged)
                ImGui::TextColored(col, "diverged at %u (apart %u ticks)", s.divergedTick, s.apartTicks);
            else
                ImGui::TextColored(col, "%s%s", PeerStateName(s.state),
                                   s.state == PeerState::Exchanging && !s.modsCompared ? " (mod contributors differ)" : "");
            ImGui::TableNextColumn();
            ImGui::Text("%u", s.lastCommonTick);
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.lagTicks);
            ImGui::TableNextColumn();
            ImGui::Text("%u", s.compared);
        }
        ImGui::EndTable();
    }
    if (!g_correlation.empty()) ImGui::TextWrapped("%s", g_correlation.c_str());
    const std::wstring b = LastBundle();
    if (!b.empty()) ImGui::TextWrapped("Last bundle: %s", game::Narrow(b).c_str());
}

bool VerbPeers(std::string_view, void*) {
    if (!g_det) return false;
    detect::PeerStatus v[detect::Detector::kMaxPeers];
    const int n = Peers(v, static_cast<int>(detect::Detector::kMaxPeers));
    const exchange::Counters c = exchange::Stats();
    LOG_INFO("[wormsign] peers: exchange=%d inMatch=%d key=%016llx tick=%u peers=%d bad=%u | sent=%llu (%llu B, %llu "
             "failed) received=%llu (%llu B)",
             g_opt.exchange, InMatch(), g_det->MatchKey(), Tick(), n, g_det->BadPackets(), c.sent, c.bytesSent,
             c.sendFailed, c.received, c.bytesReceived);
    for (int i = 0; i < n; ++i)
        LOG_INFO("[wormsign]   %s: %s mods=%d compared=%u lastCommon=%u theirs=%u lag=%d diverged=%u apart=%u", v[i].name,
                 PeerStateName(v[i].state), v[i].modsCompared, v[i].compared, v[i].lastCommonTick, v[i].theirTick,
                 v[i].lagTicks, v[i].divergedTick, v[i].apartTicks);
    return true;
}
}  // namespace

bool Install(const Options& opt) {
    if (g_installed) return true;
    g_opt = opt;
    g_det = std::make_unique<detect::Detector>(g_env);
    enginecheck::Install();
    g_tickHandle = session::OnTickEndNamed(&OnTick, nullptr, 100, "wormsign desync detector");
    g_sessionHandle = session::OnSessionNamed(&OnSessionFn, nullptr, "wormsign desync detector");
    static bool subscribed = false;
    if (!subscribed) {
        subscribed = true;
        events::Subscribe(events::Event::Frame, [] {
            if (g_installed) Frame();
        });
        testcmd::Register("wormsign.peers", &VerbPeers);
    }
    g_panel = overlay::AddPanel("wormsign.peers", "Wormsign/Peers", &DrawPanel, nullptr);
    g_installed = true;
    LOG_INFO("[wormsign] desync detector ready (exchange %s, on desync: %s)", opt.exchange ? "on" : "off",
             opt.onDesync == OnDesync::Report ? "report" : "bundle-only");
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    g_installed = false;
    RemoveOnTickEnd(g_tickHandle);
    RemoveOnSession(g_sessionHandle);
    overlay::RemovePanel(g_panel);
    enginecheck::Uninstall();
    g_det.reset();
}

void Raise(const Divergence& d) {
    const std::string comps = detect::CompList(d.compMask);
    std::string who = "the recording";
    if (d.source == Source::Peer) who = lobby::Name(d.peer);
    std::string what = comps;
    if (d.contrib[0]) what += (what.empty() ? "mods: " : ", mods: ") + std::string(d.contrib);
    else if (what.empty()) what = d.oursMods != d.theirsMods ? "mods" : "engine";
    LOG_WARN("[wormsign] DESYNC at tick %u (%s) with %s, match %u; reported at our tick %u", d.tick, what.c_str(), who.c_str(),
             d.serial, Tick());
    jlog::Rec("wormsign", jlog::Level::Warn, "divergence")
        .Str("source", d.source == Source::Peer ? "peer" : "replay").Uint("serial", d.serial).Uint("tick", d.tick)
        .Hex("oursEngine", d.oursEngine).Hex("theirsEngine", d.theirsEngine).Hex("oursMods", d.oursMods)
        .Hex("theirsMods", d.theirsMods).Uint("compMask", d.compMask).Str("comps", comps).Str("contrib", d.contrib);
    char b[200];
    if (d.source == Source::Peer) snprintf(b, sizeof b, "Desync at tick %u (%s) with %s", d.tick, what.c_str(), who.c_str());
    else snprintf(b, sizeof b, "Replay diverged at tick %u (%s)", d.tick, what.c_str());
    Toast(b);
    jsonmini::Arr ca;
    for (int i = 0; i < kEngineComps; ++i)
        if (d.compMask >> i & 1) ca.Str(detect::CompName(i));
    jsonmini::Obj ev;
    ev.Str("source", d.source == Source::Peer ? "peer" : "replay").UInt("tick", d.tick).UInt("serial", d.serial)
        .Raw("comps", ca.End()).Str("contrib", d.contrib).Str("peer", d.peer ? std::to_string(d.peer) : "");
    lua::PostEvent("wormsign.divergence", ev.End().c_str());
    divergence::Raise(d);
}

void SetContribSource(ContribNamesFn names, ContribHashesFn hashes) {
    g_contribNames = names;
    g_contribHashes = hashes;
}
void SetDetailSource(DetailFn fn) { g_detail = fn; }
void SetRecordingSource(RecordingFn fn) { g_recording = fn; }

int Peers(PeerStatus* out, int max) { return g_det ? g_det->Peers(out, max) : 0; }

std::wstring LastBundle() {
    std::lock_guard lk(g_bundleMx);
    return g_lastBundle;
}

std::wstring ReplaysDir() {
    const std::wstring d = DocumentsMelange();
    return (d.empty() ? game::DataDir() : d) + L"\\replays";
}
}  // namespace melange::wormsign::detector
