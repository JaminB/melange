// Ties capture, rngtap and the public session/tick-end observers together into one streamed .wsr per match, then
// hands the closed file to the library for indexing and retention.
#include "wormsign/recorder.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "melange/bus.h"
#include "melange/gamestate.h"
#include "melange/mods.h"
#include "melange/wormsign.h"
#include "tools/json_mini.h"
#include "version.h"
#include "wormsign/capture_internal.h"
#include "wormsign/format.h"
#include "wormsign/library.h"
#include "wormsign/records.h"
#include "wormsign/rngtap_internal.h"
#include "wormsign/writer.h"

namespace melange::wormsign::recorder {
namespace {
namespace wsr = melange::wormsign::wsr;
namespace rec = melange::wormsign::records;

constexpr size_t kTickChunkTicks = 500;
constexpr size_t kFlushBytes = 64 * 1024;

std::mutex g_mu;
writer::Writer g_writer;
std::atomic<bool> g_active{false}, g_recordEnabled{true}, g_detailEnabled{true};
std::wstring g_path;
uint32_t g_serial = 0;
uint64_t g_ticksSeen = 0, g_inputCount = 0, g_remoteCount = 0;
bool g_preIncomplete = false;
uint32_t g_recordingsWritten = 0;

std::vector<uint8_t> g_inptBuf, g_rmtiBuf, g_dispBuf, g_tickBuf;
uint32_t g_tickChunkFrom = 0, g_tickChunkTo = 0;

std::wstring TimestampedName(uint32_t serial) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[96];
    swprintf_s(buf, L"wsr-%04d%02d%02d-%02d%02d%02d-m%u.wsr", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond, serial);
    return buf;
}

void FlushInpt() {
    if (g_inptBuf.empty()) return;
    g_writer.Enqueue(wsr::kINPT, g_inptBuf.data(), g_inptBuf.size(), true);
    g_inptBuf.clear();
}
void FlushRmti() {
    if (g_rmtiBuf.empty()) return;
    g_writer.Enqueue(wsr::kRMTI, g_rmtiBuf.data(), g_rmtiBuf.size(), true);
    g_rmtiBuf.clear();
}
void FlushDisp() {
    if (g_dispBuf.empty()) return;
    g_writer.Enqueue(wsr::kDISP, g_dispBuf.data(), g_dispBuf.size(), true);
    g_dispBuf.clear();
}
void FlushTick() {
    if (g_tickBuf.empty()) return;
    g_writer.Enqueue(wsr::kTICK, g_tickBuf.data(), g_tickBuf.size(), true, g_tickChunkFrom, g_tickChunkTo);
    g_tickBuf.clear();
}

std::string ContentHash16() {
    const mods::ContentId c = mods::LocalContent();
    if (c.vanilla || !c.hash[0]) return "";
    return std::string(c.hash, c.hash + (std::strlen(c.hash) < 16 ? std::strlen(c.hash) : 16));
}

void WriteHead() {
    bool online = false;
    gamestate::Snapshot snap{};
    if (gamestate::Latest(&snap) || gamestate::Read(&snap)) online = snap.match.online;

    jsonmini::Obj o;
    o.Int("format", wsr::kFormat)
        .Int("engineHash", static_cast<long long>(kEngineHashVersion))
        .Str("exeBuild", "1077")
        .Str("melange", MELANGE_VERSION)
        .Int("startUnix", static_cast<long long>(time(nullptr)))
        .Bool("online", online)
        .Bool("localNet", false)  // set true by LocalNet's own hook when it wraps a match; not detected here
        .Str("contentHash", ContentHash16())
        .Int("tickMs", static_cast<long long>(kTickMs))
        .Bool("record", g_recordEnabled)
        .Bool("recordDetail", g_detailEnabled);
    const std::string json = o.End();
    g_writer.Enqueue(wsr::kHEAD, json.data(), json.size(), true);
}

void WriteSeedAndPreDraws() {
    std::vector<uint8_t> seedBuf;
    for (const auto& s : rngtap::TakeSessionSeeds())
        rec::AppendSeed(seedBuf, static_cast<uint8_t>(s.kind), s.value, s.caller, s.t);
    g_writer.Enqueue(wsr::kSEED, seedBuf.data(), seedBuf.size(), true);

    bool overflow = false;
    std::vector<uint8_t> preBuf;
    for (const auto& d : rngtap::PreMatchDraws(&overflow)) rec::AppendPreDraw(preBuf, static_cast<uint8_t>(d.rng), d.ret, 0, d.bits);
    g_writer.Enqueue(wsr::kPDRW, preBuf.data(), preBuf.size(), true);
    g_preIncomplete = overflow;
}

// Best-effort setup fingerprint via the same named-variable path Land.* already uses in the overlay. GM.SchemeData
// and GM.GameInitData are read the same way if the engine's variable enumerator exposes them; when it does not,
// those two fields are left empty. Verify against a real session's variable dump before relying on this for
// anything but diagnostics -- the replay player is the actual consumer of this fingerprint.
void WriteSetp() {
    gamestate::Var v{};
    jsonmini::Obj o;
    auto field = [&](const char* name, const char* key) {
        if (gamestate::Var1(name, &v)) o.Str(key, v.value);
        else o.Str(key, "");
    };
    field("Land.File", "landFile");
    field("Land.Theme", "landTheme");
    field("LevelDetailsName", "levelDetailsName");
    field("WXD.Level.Current", "levelCurrent");
    field("GM.SchemeData", "schemeData");
    field("GM.GameInitData", "gameInitData");

    gamestate::Snapshot snap{};
    jsonmini::Arr teams;
    if (gamestate::Latest(&snap) || gamestate::Read(&snap)) {
        for (uint8_t i = 0; i < snap.teamCount; ++i) {
            const auto& t = snap.teams[i];
            jsonmini::Obj to;
            to.Str("name", t.name).Int("alliance", t.alliance).Bool("ai", t.ai);
            teams.Raw(to.End());
        }
        int wormCounts[4] = {};
        for (uint8_t i = 0; i < snap.wormCount; ++i)
            if (snap.worms[i].team < 4) ++wormCounts[snap.worms[i].team];
        jsonmini::Arr wc;
        for (int i = 0; i < 4 && i < snap.teamCount; ++i) wc.Raw(std::to_string(wormCounts[i]));
        o.Raw("wormCounts", wc.End());
    }
    o.Raw("teams", teams.End());
    const std::string json = o.End();
    g_writer.Enqueue(wsr::kSETP, json.data(), json.size(), true);
}

void BeginRecording(uint32_t serial) {
    if (!g_recordEnabled) return;
    const std::wstring dir = library::ReplaysDir();
    if (dir.empty()) {
        LOG_ERROR("[wormsign] recorder: could not resolve the replays folder, recording is off this session");
        return;
    }
    const std::wstring path = dir + L"\\" + TimestampedName(serial);
    if (!g_writer.Open(path)) {
        LOG_ERROR("[wormsign] recorder: could not open %ls for writing", path.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_path = path;
        g_serial = serial;
        g_ticksSeen = g_inputCount = g_remoteCount = 0;
        g_preIncomplete = false;
        g_inptBuf.clear();
        g_rmtiBuf.clear();
        g_dispBuf.clear();
        g_tickBuf.clear();
        g_tickChunkFrom = g_tickChunkTo = 0;
    }
    WriteHead();
    WriteSeedAndPreDraws();
    g_active = true;
    LOG_INFO("[wormsign] recorder: session %u -> %ls", serial, path.c_str());
}

void EndRecording(const char* reason) {
    if (!g_active.exchange(false)) return;
    FlushTick();
    FlushInpt();
    FlushRmti();
    FlushDisp();
    jsonmini::Obj note;
    note.Str("reason", reason)
        .Int("ticks", static_cast<long long>(g_ticksSeen))
        .Int("inputs", static_cast<long long>(g_inputCount))
        .Int("remoteInputs", static_cast<long long>(g_remoteCount))
        .Bool("preMatchDrawsIncomplete", g_preIncomplete);
    const std::string json = note.End();
    g_writer.Enqueue(wsr::kNOTE, json.data(), json.size(), true);
    const bool ok = g_writer.Close();
    LOG_INFO("[wormsign] recorder: session %u closed (%s), %llu ticks, %llu inputs, %s", g_serial, reason,
             static_cast<unsigned long long>(g_ticksSeen), static_cast<unsigned long long>(g_inputCount),
             ok ? "complete" : "FAILED to close");
    ++g_recordingsWritten;
    library::OnRecordingClosed(g_path, ok);
}

void OnSessionCb(bool begin, uint32_t serial, void*) {
    if (begin) BeginRecording(serial);
    else EndRecording("match-end");
}

void OnTickEndCb(const TickHash& h, void*) {
    if (!g_active) return;
    ++g_ticksSeen;
    if (h.tick == 1) WriteSetp();

    if (g_tickBuf.empty()) g_tickChunkFrom = h.tick;
    g_tickChunkTo = h.tick;
    const size_t base = g_tickBuf.size();
    g_tickBuf.resize(base + rec::kTickBytes);
    uint8_t* p = g_tickBuf.data() + base;
    memcpy(p, &h.engine, 8), p += 8;
    memcpy(p, &h.mods, 8), p += 8;
    memcpy(p, h.c, sizeof h.c), p += sizeof h.c;
    memcpy(p, &h.rngLogic, 4), p += 4;
    memcpy(p, &h.rng2, 4), p += 4;
    memcpy(p, &h.fpucw, 2), p += 2;
    memcpy(p, &h.inputs, 2);

    if (g_tickBuf.size() >= kTickChunkTicks * rec::kTickBytes) FlushTick();
    if (g_inptBuf.size() >= kFlushBytes) FlushInpt();
    if (g_rmtiBuf.size() >= kFlushBytes) FlushRmti();
    if (g_dispBuf.size() >= kFlushBytes) FlushDisp();
}

void OnSendCb(const capture::SendEvent& e, void*) {
    if (!g_active) return;
    rec::AppendInput(g_inptBuf, static_cast<uint8_t>(e.type), e.id, e.a, e.b, e.time, e.callT, e.caller, e.str);
    ++g_inputCount;
}
void OnInsertCb(const capture::InsertEvent& e, void*) {
    if (!g_active) return;
    rec::AppendRemoteInput(g_rmtiBuf, e.arrivedT, e.id, e.time, e.a);
    ++g_remoteCount;
}

constexpr uintptr_t kRmsLo1 = 0x53ef50, kRmsHi1 = 0x540000, kRmsLo2 = 0x542b80, kRmsHi2 = 0x542e40;
constexpr uintptr_t kImmLo = 0x5056b0, kImmHi = 0x505c70;
void OnDispatch(const melange::bus::MessageView& m, void*) {
    if (!g_active) return;
    const uintptr_t c = m.caller;
    const bool rms = (c >= kRmsLo1 && c < kRmsHi1) || (c >= kRmsLo2 && c < kRmsHi2);
    const bool imm = c >= kImmLo && c < kImmHi;
    if (!rms && !imm) return;
    rec::AppendDispatch(g_dispBuf, melange::wormsign::LogicTimeMs(), m.id);
}
}  // namespace

bool Install() {
    if (!capture::Install()) {
        LOG_ERROR("[wormsign] recorder: capture hooks failed, recording is off");
        return false;
    }
    if (!rngtap::Install()) {
        LOG_ERROR("[wormsign] recorder: rngtap hooks failed, recording is off");
        return false;
    }
    capture::SetSendSink(&OnSendCb, nullptr);
    capture::SetInsertSink(&OnInsertCb, nullptr);
    melange::bus::SubscribeAll(melange::bus::Path::Post, &OnDispatch);
    melange::wormsign::OnSession(&OnSessionCb, nullptr);
    melange::wormsign::OnTickEnd(&OnTickEndCb, nullptr, 10);
    library::Rescan();
    return true;
}

void SetRecordEnabled(bool on) { g_recordEnabled = on; }
void SetDetailEnabled(bool on) { g_detailEnabled = on; }

Stats GetStats() {
    return Stats{g_writer.QueuedChunks(), g_writer.DroppedChunks(), g_writer.BytesWritten(), g_recordingsWritten};
}
}  // namespace melange::wormsign::recorder
