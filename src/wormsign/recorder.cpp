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
#include "core/debug.h"
#include "core/events.h"
#include "core/log.h"
#include "core/thread_guard.h"
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
#include "wormsign/session.h"
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

// One read of another module's state at a recording's start, fault-guarded on its own: a fault there costs the
// recording that one field, not the whole recording (and is logged with what faulted where).
template <class F>
bool GuardedStep(F& step, EXCEPTION_RECORD* rec) {
    __try {
        step();
        return true;
    } __except (melange::debug::CopyExceptionRecord(GetExceptionInformation(), rec)) {
        return false;
    }
}

template <class F>
void StartStep(const char* what, F&& step) {
    EXCEPTION_RECORD rec{};
    if (!GuardedStep(step, &rec))
        LOG_ERROR("[wormsign] recorder: reading %s at the session start faulted (%s); recording without it", what,
                  melange::debug::DescribeException(rec).c_str());
}

// The parts of a recording's start that read other modules' state, built before the file is opened.
struct StartData {
    std::string head;
    std::vector<uint8_t> seeds, preDraws;
    bool preOverflow = false;
};

std::string HeadJson() {
    bool online = false;
    StartStep("the game state", [&] {
        gamestate::Snapshot snap{};
        if (gamestate::Latest(&snap) || gamestate::Read(&snap)) online = snap.match.online;
    });
    std::string contentHash;
    StartStep("the mod content id", [&] { contentHash = ContentHash16(); });

    jsonmini::Obj o;
    o.Int("format", wsr::kFormat)
        .Int("engineHash", static_cast<long long>(kEngineHashVersion))
        .Str("exeBuild", "1077")
        .Str("melange", MELANGE_VERSION)
        .Int("startUnix", static_cast<long long>(time(nullptr)))
        .Bool("online", online)
        .Bool("localNet", melange::config::GetBool("LocalNet", "Enabled", false))
        .Str("contentHash", contentHash)
        .Int("tickMs", static_cast<long long>(kTickMs))
        .Bool("record", g_recordEnabled)
        .Bool("recordDetail", g_detailEnabled);
    static contrib::Info infos[128];  // main thread only; 13 KB off the game's stack
    size_t n = 0;
    StartStep("the hash contributors", [&] { n = contrib::List(infos, 128); });
    jsonmini::Arr cs;
    for (size_t i = 0; i < n; ++i) {
        jsonmini::Obj c;
        c.Str("name", infos[i].name).Int("version", infos[i].version).Bool("replay", infos[i].inReplayCompare);
        cs.Raw(c.End());
    }
    o.Raw("contributors", cs.End());
    return o.End();
}

StartData ReadStart() {
    StartData d;
    d.head = HeadJson();
    StartStep("the session's RNG seeds", [&] {
        for (const auto& s : rngtap::TakeSessionSeeds())
            rec::AppendSeed(d.seeds, rec::Seed{static_cast<uint8_t>(s.kind), s.value, s.caller, s.t});
    });
    StartStep("the pre-match RNG draws", [&] {
        for (const auto& p : rngtap::PreMatchDraws(&d.preOverflow))
            rec::AppendDraw(d.preDraws, rec::Draw{static_cast<uint8_t>(p.rng), p.ret, p.stateAfter, p.bits});
    });
    return d;
}

void WriteSetp() {
    setup::Data d;
    if (!setup::Capture(&d)) LOG_WARN("[wormsign] recorder: the match setup could not be read");
    const std::string json = setup::ToJson(d);
    g_w->Enqueue(wsr::kSETP, json.data(), json.size(), true);
}

// A writer still open while no recording is active: a session start that faulted after opening its file (before
// 0.4.1 the file was opened first). Closed as it is, so the next Open does not fail and the file is not held open.
void CloseStaleWriter(const char* when) {
    if (g_active || !g_w->IsOpen()) return;
    LOG_WARN("[wormsign] recorder: a recording was left open without a session (%s); closing %ls", when, g_path.c_str());
    std::thread([w = std::move(g_w), path = g_path]() mutable {
        GuardedThreadBody("wormsign-recorder", [&] {
            const bool ok = w->Close();
            library::OnRecordingClosed(path, ok);
        });
    }).detach();
    g_w = std::make_unique<writer::Writer>();
}

void BeginRecording(uint32_t serial) {
    if (!g_recordEnabled) return;
    CloseStaleWriter("at the next session start");
    const std::wstring dir = library::ReplaysDir();
    if (dir.empty()) {
        LOG_ERROR("[wormsign] recorder: could not resolve the replays folder, recording is off this session");
        return;
    }
    // Everything that reads other modules first, so a fault in it cannot leave an opened, empty file behind.
    const StartData start = ReadStart();
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
        g_preIncomplete = start.preOverflow;
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
    // The writer thread puts these on disk straight away: the file has its HEAD before the first tick.
    g_w->Enqueue(wsr::kHEAD, start.head.data(), start.head.size(), true);
    g_w->Enqueue(wsr::kSEED, start.seeds.data(), start.seeds.size(), true);
    g_w->Enqueue(wsr::kPDRW, start.preDraws.data(), start.preDraws.size(), true);
    g_active = true;
    LOG_INFO("[wormsign] recorder: session %u -> %ls", serial, path.c_str());
}

void EndRecording(const char* reason) {
    if (!g_active.exchange(false)) return;
    LARGE_INTEGER q0, qf;
    QueryPerformanceCounter(&q0);
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
    LARGE_INTEGER q1;
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    const long long handoverUs = (q1.QuadPart - q0.QuadPart) * 1000000 / qf.QuadPart;
    // Closing deflates and writes what is still queued: off the main thread, with a fresh writer for the next match.
    std::thread([w = std::move(g_w), path = g_path, serial = g_serial, reason = std::string(reason),
                 ticks = g_ticksSeen, inputs = g_inputCount, handoverUs]() mutable {
        GuardedThreadBody("wormsign-recorder", [&] {
            const bool ok = w->Close();
            LOG_INFO("[wormsign] recorder: session %u closed (%s), %llu ticks, %llu inputs, %s; main thread %lld us",
                     serial, reason.c_str(), static_cast<unsigned long long>(ticks),
                     static_cast<unsigned long long>(inputs), ok ? "complete" : "FAILED to close", handoverUs);
            library::OnRecordingClosed(path, ok);
        });
    }).detach();
    g_w = std::make_unique<writer::Writer>();
    ++g_recordingsWritten;
}

void OnSessionCb(bool begin, uint32_t serial, void*) {
    if (begin) {
        BeginRecording(serial);
    } else {
        EndRecording(session::EndReason());
        CloseStaleWriter("at the session end");
    }
}

// Diagnostics' crash hook, on its dump thread: what the recording has buffered goes to the disk. The file keeps no
// INDX (the library and the reader recover such a file and mark it incomplete). The main thread's buffers are only
// read when the main thread is the one that crashed, since it then waits in the crash filter; otherwise it may be
// writing them right now, and only what the writer already holds is flushed.
void CrashFlush(DWORD crashingThread) {
    if (!g_active || !g_w) return;
    size_t handed = 0;
    if (crashingThread == melange::events::MainThreadId()) {
        if (!g_ticks.Empty()) {
            const uint32_t from = g_ticks.FirstTick(), to = g_ticks.LastTick();
            const std::vector<uint8_t> b = g_ticks.Take();
            handed += g_w->TryEnqueue(wsr::kTICK, b.data(), b.size(), from, to);
        }
        for (std::vector<uint8_t>* buf : {&g_inptBuf, &g_rmtiBuf, &g_dispBuf, &g_ctrbBuf}) {
            if (buf->empty()) continue;
            const uint32_t type = buf == &g_inptBuf ? wsr::kINPT : buf == &g_rmtiBuf ? wsr::kRMTI
                                : buf == &g_dispBuf ? wsr::kDISP : wsr::kCTRB;
            handed += g_w->TryEnqueue(type, buf->data(), buf->size());
            buf->clear();
        }
        if (!g_detlBuf.empty()) handed += g_w->TryEnqueue(wsr::kDETL, g_detlBuf.data(), g_detlBuf.size(), g_detlFrom, g_detlTo);
    }
    char note[160];
    snprintf(note, sizeof note, "{\"reason\":\"crash\",\"ticks\":%llu,\"inputs\":%llu,\"remoteInputs\":%llu}",
             static_cast<unsigned long long>(g_ticksSeen), static_cast<unsigned long long>(g_inputCount),
             static_cast<unsigned long long>(g_remoteCount));
    handed += g_w->TryEnqueue(wsr::kNOTE, note, strlen(note));
    const bool flushed = g_w->FlushWithin(3000);
    LOG_ERROR("     recording: session %u, %llu ticks, %zu buffered chunk(s) handed over, %s: %ls", g_serial,
              static_cast<unsigned long long>(g_ticksSeen), handed,
              flushed ? "on disk" : "NOT confirmed on disk within 3 s", g_path.c_str());
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
    session::OnSessionNamed(&OnSessionCb, nullptr, "wormsign recorder");
    session::OnTickEndNamed(&OnTickEndCb, nullptr, 10, "wormsign recorder");
    melange::wormsign::OnDivergence(&OnDivergenceCb, nullptr);
    melange::debug::AddCrashHook(&CrashFlush, "wormsign recorder");
    // Off the main thread: a large or crafted replays folder can mean gigabytes of inflate work (up to 64 MB per
    // chunk, every chunk of every file), and the main thread never waits on disk (the same rule the writer and
    // detector follow). Library() simply returns nothing for this session's matches until the scan finishes.
    std::thread([] { GuardedThreadBody("wormsign-library", &library::Rescan); }).detach();
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
