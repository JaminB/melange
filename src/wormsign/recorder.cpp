// Ties capture, rngtap and the public session/tick-end observers together into one streamed .wsr per match, then
// hands the closed file to the library for indexing and retention.
#include "wormsign/recorder.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "melange/bus.h"
#include "melange/gamestate.h"
#include "melange/mods.h"
#include "melange/wormsign.h"
#include "tools/json_mini.h"
#include "version.h"
#include "wormsign/capture_internal.h"
#include "wormsign/contrib.h"
#include "wormsign/detail.h"
#include "wormsign/enginecheck.h"
#include "wormsign/format.h"
#include "wormsign/fpu.h"
#include "wormsign/library.h"
#include "wormsign/records.h"
#include "wormsign/rngtap_internal.h"
#include "wormsign/setup.h"
#include "wormsign/writer.h"

namespace melange::wormsign::recorder {
namespace {
namespace wsr = melange::wormsign::wsr;
namespace rec = melange::wormsign::rec;

constexpr size_t kTickChunkTicks = 500;
constexpr size_t kFlushBytes = 64 * 1024;

std::mutex g_mu;
std::unique_ptr<writer::Writer> g_w = std::make_unique<writer::Writer>();
std::string g_contribNote = "{}";
std::atomic<bool> g_active{false}, g_recordEnabled{true}, g_detailEnabled{true};
std::wstring g_path;
uint32_t g_serial = 0;
uint64_t g_ticksSeen = 0, g_inputCount = 0, g_remoteCount = 0;
bool g_preIncomplete = false;
uint32_t g_recordingsWritten = 0;

std::vector<uint8_t> g_inptBuf, g_rmtiBuf, g_dispBuf, g_detlBuf, g_detlPrev, g_detlCur;
rec::TickChunk g_ticks;
std::vector<uint8_t> g_ctrbBuf;
std::vector<uint64_t> g_ctrbPrev;
std::vector<std::pair<uint8_t, uint64_t>> g_ctrbChanges;
contrib::Entry g_ctrbEntries[128];
uint32_t g_detlFrom = 0, g_detlTo = 0;

std::wstring TimestampedName(uint32_t serial) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[96];
    swprintf_s(buf, L"wsr-%04d%02d%02d-%02d%02d%02d-p%lu-m%u.wsr", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond, GetCurrentProcessId(), serial);
    return buf;
}

void FlushInpt() {
    if (g_inptBuf.empty()) return;
    g_w->Enqueue(wsr::kINPT, g_inptBuf.data(), g_inptBuf.size(), true);
    g_inptBuf.clear();
}
void FlushRmti() {
    if (g_rmtiBuf.empty()) return;
    g_w->Enqueue(wsr::kRMTI, g_rmtiBuf.data(), g_rmtiBuf.size(), true);
    g_rmtiBuf.clear();
}
void FlushDisp() {
    if (g_dispBuf.empty()) return;
    g_w->Enqueue(wsr::kDISP, g_dispBuf.data(), g_dispBuf.size(), true);
    g_dispBuf.clear();
}
void FlushTick() {
    if (g_ticks.Empty()) return;
    if (contrib::Count()) g_contribNote = contrib::NoteJson();
    const uint32_t from = g_ticks.FirstTick(), to = g_ticks.LastTick();
    const std::vector<uint8_t> b = g_ticks.Take();
    g_w->Enqueue(wsr::kTICK, b.data(), b.size(), true, from, to);
}
void FlushCtrb() {
    if (g_ctrbBuf.empty()) return;
    g_w->Enqueue(wsr::kCTRB, g_ctrbBuf.data(), g_ctrbBuf.size(), true);
    g_ctrbBuf.clear();
}
// The contributors whose hash changed this tick, so a replay can name the one that differs.
void RecordContribs(uint32_t tick) {
    const size_t n = contrib::HashesAt(tick, g_ctrbEntries, 128);
    const bool all = n != g_ctrbPrev.size();
    g_ctrbPrev.resize(n);
    g_ctrbChanges.clear();
    for (size_t i = 0; i < n; ++i)
        if (all || g_ctrbEntries[i].hash != g_ctrbPrev[i]) {
            g_ctrbChanges.emplace_back(static_cast<uint8_t>(i), g_ctrbEntries[i].hash);
            g_ctrbPrev[i] = g_ctrbEntries[i].hash;
        }
    if (!g_ctrbChanges.empty()) rec::AppendContribChanges(g_ctrbBuf, tick, g_ctrbChanges);
}
void FlushDetl() {
    if (g_detlBuf.empty()) return;
    g_w->Enqueue(wsr::kDETL, g_detlBuf.data(), g_detlBuf.size(), true, g_detlFrom, g_detlTo);
    g_detlBuf.clear();
    g_detlPrev.clear();
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
        .Bool("localNet", melange::config::GetBool("LocalNet", "Enabled", false))
        .Str("contentHash", ContentHash16())
        .Int("tickMs", static_cast<long long>(kTickMs))
        .Bool("record", g_recordEnabled)
        .Bool("recordDetail", g_detailEnabled);
    contrib::Info infos[128];
    const size_t n = contrib::List(infos, 128);
    jsonmini::Arr cs;
    for (size_t i = 0; i < n; ++i) {
        jsonmini::Obj c;
        c.Str("name", infos[i].name).Int("version", infos[i].version).Bool("replay", infos[i].inReplayCompare);
        cs.Raw(c.End());
    }
    o.Raw("contributors", cs.End());
    const std::string json = o.End();
    g_w->Enqueue(wsr::kHEAD, json.data(), json.size(), true);
}

void WriteSeedAndPreDraws() {
    std::vector<uint8_t> seedBuf;
    for (const auto& s : rngtap::TakeSessionSeeds())
        rec::AppendSeed(seedBuf, rec::Seed{static_cast<uint8_t>(s.kind), s.value, s.caller, s.t});
    g_w->Enqueue(wsr::kSEED, seedBuf.data(), seedBuf.size(), true);

    bool overflow = false;
    std::vector<uint8_t> preBuf;
    for (const auto& d : rngtap::PreMatchDraws(&overflow))
        rec::AppendDraw(preBuf, rec::Draw{static_cast<uint8_t>(d.rng), d.ret, d.stateAfter, d.bits});
    g_w->Enqueue(wsr::kPDRW, preBuf.data(), preBuf.size(), true);
    g_preIncomplete = overflow;
}

void WriteSetp() {
    setup::Data d;
    if (!setup::Capture(&d)) LOG_WARN("[wormsign] recorder: the match setup could not be read");
    const std::string json = setup::ToJson(d);
    g_w->Enqueue(wsr::kSETP, json.data(), json.size(), true);
}

void BeginRecording(uint32_t serial) {
    if (!g_recordEnabled) return;
    const std::wstring dir = library::ReplaysDir();
    if (dir.empty()) {
        LOG_ERROR("[wormsign] recorder: could not resolve the replays folder, recording is off this session");
        return;
    }
    const std::wstring path = dir + L"\\" + TimestampedName(serial);
    if (!g_w->Open(path)) {
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
        g_ticks.Take();
        g_detlBuf.clear();
        g_detlPrev.clear();
        g_contribNote = "{}";
        g_ctrbBuf.clear();
        g_ctrbPrev.clear();
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
    FlushDetl();
    FlushCtrb();
    const auto engv = enginecheck::Records(g_serial);
    if (!engv.empty()) {
        const std::string j = enginecheck::Json(engv);
        g_w->Enqueue(wsr::kENGV, j.data(), j.size(), true);
    }
    jsonmini::Obj note;
    note.Str("reason", reason)
        .Int("ticks", static_cast<long long>(g_ticksSeen))
        .Int("inputs", static_cast<long long>(g_inputCount))
        .Int("remoteInputs", static_cast<long long>(g_remoteCount))
        .Bool("preMatchDrawsIncomplete", g_preIncomplete)
        .Raw("contrib", g_contribNote)
        .Raw("fpu", fpu::NoteJson());
    const std::string json = note.End();
    g_w->Enqueue(wsr::kNOTE, json.data(), json.size(), true);
    // Closing deflates and writes what is still queued: off the main thread, with a fresh writer for the next match.
    std::thread([w = std::move(g_w), path = g_path, serial = g_serial, reason = std::string(reason),
                 ticks = g_ticksSeen, inputs = g_inputCount]() mutable {
        const bool ok = w->Close();
        LOG_INFO("[wormsign] recorder: session %u closed (%s), %llu ticks, %llu inputs, %s", serial, reason.c_str(),
                 static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(inputs),
                 ok ? "complete" : "FAILED to close");
        library::OnRecordingClosed(path, ok);
    }).detach();
    g_w = std::make_unique<writer::Writer>();
    ++g_recordingsWritten;
}

void OnSessionCb(bool begin, uint32_t serial, void*) {
    if (begin) BeginRecording(serial);
    else EndRecording("match-end");
}

void OnTickEndCb(const TickHash& h, void*) {
    if (!g_active) return;
    ++g_ticksSeen;
    if (h.tick == 1) WriteSetp();

    g_ticks.Add(h);
    RecordContribs(h.tick);

    if (g_detailEnabled && detail::GetPacked(h.tick, &g_detlCur)) {
        if (g_detlBuf.empty()) g_detlFrom = h.tick;
        g_detlTo = h.tick;
        detail::EncodeDelta(g_detlPrev.empty() ? nullptr : g_detlPrev.data(), g_detlPrev.size(), g_detlCur.data(),
                            g_detlCur.size(), &g_detlBuf);
        g_detlPrev.swap(g_detlCur);
        if (g_detlTo - g_detlFrom + 1 >= kTickChunkTicks) FlushDetl();
    }

    // Every 500 ticks (10 s) all streams go to the writer, so a crash loses at most that much of any of them.
    const bool chunkEnd = g_ticks.Count() >= kTickChunkTicks;
    if (chunkEnd) {
        FlushTick();
        FlushCtrb();
    }
    if (chunkEnd || g_inptBuf.size() >= kFlushBytes) FlushInpt();
    if (chunkEnd || g_rmtiBuf.size() >= kFlushBytes) FlushRmti();
    if (chunkEnd || g_dispBuf.size() >= kFlushBytes) FlushDisp();
}

void OnDivergenceCb(const Divergence& d, void*) {
    if (!g_active || d.serial != g_serial) return;
    char hex[17];
    auto h = [&hex](uint64_t v) {
        snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(v));
        return std::string(hex);
    };
    jsonmini::Obj o;
    o.Str("source", d.source == Source::Replay ? "replay" : "peer")
        .Int("serial", d.serial)
        .Int("tick", d.tick)
        .Str("oursEngine", h(d.oursEngine))
        .Str("theirsEngine", h(d.theirsEngine))
        .Str("oursMods", h(d.oursMods))
        .Str("theirsMods", h(d.theirsMods))
        .Int("compMask", d.compMask)
        .Str("contrib", d.contrib)
        .Str("peer", std::to_string(d.peer));
    detail::DetailRec r;
    if (detail::Get(d.tick, &r)) o.Raw("detailLocal", detail::ToJson(r));
    const std::string json = o.End();
    g_w->Enqueue(wsr::kDVRG, json.data(), json.size(), true, d.tick, d.tick);
}

void OnSendCb(const capture::SendEvent& e, void*) {
    if (!g_active) return;
    rec::AppendInput(g_inptBuf, rec::Input{static_cast<uint8_t>(e.type), e.id, e.a, e.b, e.time, e.callT, e.caller,
                                           e.str ? e.str : ""});
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
    melange::wormsign::OnDivergence(&OnDivergenceCb, nullptr);
    library::Rescan();
    return true;
}

void SetRecordEnabled(bool on) { g_recordEnabled = on; }
void SetDetailEnabled(bool on) { g_detailEnabled = on; }

bool RecordingPath(uint32_t serial, std::wstring* path) {
    if (serial != g_serial || g_path.empty()) return false;
    if (g_active) {
        FlushTick();
        FlushInpt();
        FlushRmti();
        FlushDisp();
        FlushDetl();
        FlushCtrb();
        g_w->RequestFlush();
    }
    *path = g_path;
    return true;
}

Stats GetStats() {
    return Stats{g_w->QueuedChunks(), g_w->DroppedChunks(), g_w->BytesWritten(), g_recordingsWritten};
}
}  // namespace melange::wormsign::recorder
