#include "wormsign/recording.h"

#include <algorithm>

#include "tools/json_read.h"

namespace melange::wormsign {
namespace {
constexpr uint32_t kMaxTickSpan = 1u << 20;

std::string Str(const json::Value& o, const char* key) {
    const json::Value* v = o.Get(key);
    return v && v->IsString() ? v->string : "";
}

std::string KeyOf(const json::Value* list) {
    if (!list || !list->IsArray()) return "";
    std::vector<std::string> v;
    for (const json::Value& e : list->items) {
        if (e.IsString()) {
            v.push_back(e.string);
        } else if (e.IsObject()) {
            std::string s = Str(e, "name");
            const json::Value* ver = e.Get("version");
            if (ver && ver->IsInteger()) s += "@" + std::to_string(static_cast<long long>(ver->number));
            else if (ver && ver->IsString()) s += "@" + ver->string;
            v.push_back(std::move(s));
        }
    }
    std::sort(v.begin(), v.end());
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : ",") + s;
    return out;
}
}  // namespace

bool Recording::Tick(uint32_t tick, TickHash* out) const {
    if (ticks.empty() || tick < firstTick || tick > lastTick || !have[tick - firstTick]) return false;
    *out = ticks[tick - firstTick];
    return true;
}

bool Recording::ContribHashes(uint32_t tick, std::vector<uint64_t>* out) const {
    out->assign(contribNames.size(), 0);
    bool any = false;
    for (const rec::ContribChange& c : contribChanges) {
        if (c.tick > tick) break;
        if (c.index < out->size()) (*out)[c.index] = c.hash, any = true;
    }
    return any;
}

uint32_t Recording::TickCount() const {
    uint32_t n = 0;
    for (uint8_t h : have) n += h;
    return n;
}

std::string ContributorKey(const std::string& headJson) {
    json::Value v;
    json::Error e;
    if (!json::Parse(headJson, &v, &e) || !v.IsObject()) return "";
    return KeyOf(v.Get("contributors"));
}

bool LoadRecording(const wsr::Reader& r, Recording* out, std::string* err) {
    *out = Recording{};
    out->format = r.Format();
    out->engineHash = r.EngineHash();
    out->complete = r.Complete();
    json::Value head;
    json::Error je;
    if (!json::Parse(r.Header(), &head, &je) || !head.IsObject()) {
        *err = "the recording's header is unreadable";
        return false;
    }
    out->exeBuild = Str(head, "exeBuild");
    out->contentHash = Str(head, "contentHash");
    out->melange = Str(head, "melange");
    const json::Value* online = head.Get("online");
    out->online = online && online->IsBool() && online->boolean;
    out->contributors = KeyOf(head.Get("contributors"));
    if (const json::Value* cl = head.Get("contributors"); cl && cl->IsArray())
        for (const json::Value& e : cl->items)
            out->contribNames.push_back(e.IsString() ? e.string.substr(0, e.string.find('@')) : Str(e, "name"));
    const json::Value* tickMs = head.Get("tickMs");
    if (tickMs && (!tickMs->IsNumber() || tickMs->number != kTickMs)) {
        *err = "the recording uses a different tick length";
        return false;
    }

    const char* bad = nullptr;
    std::vector<TickHash> ticks;
    const bool decoded = r.ForEach(0, [&](const wsr::ChunkRef& c, const std::vector<uint8_t>& p) {
        if (bad) return;
        const uint8_t* d = p.data();
        if (c.type == wsr::kSEED) {
            if (!rec::DecodeSeeds(d, p.size(), &out->seeds)) bad = "SEED";
        } else if (c.type == wsr::kPDRW) {
            if (!rec::DecodeDraws(d, p.size(), &out->draws)) bad = "PDRW";
        } else if (c.type == wsr::kINPT) {
            if (!rec::DecodeInputs(d, p.size(), &out->inputs)) bad = "INPT";
        } else if (c.type == wsr::kTICK) {
            if (!rec::DecodeTicks(d, p.size(), [&](const TickHash& h) { ticks.push_back(h); })) bad = "TICK";
        } else if (c.type == wsr::kCTRB) {
            if (!rec::DecodeContribChanges(d, p.size(), &out->contribChanges)) bad = "CTRB";
        } else if (c.type == wsr::kSETP) {
            out->setup.assign(p.begin(), p.end());
        }
    });
    if (!decoded) {
        *err = "a chunk of the recording does not decode";
        return false;
    }
    if (bad) {
        *err = std::string("the recording's ") + bad + " records are malformed";
        return false;
    }
    std::stable_sort(out->inputs.begin(), out->inputs.end(),
                     [](const rec::Input& a, const rec::Input& b) { return a.callT < b.callT; });
    if (!ticks.empty()) {
        uint32_t lo = ticks.front().tick, hi = lo;
        for (const TickHash& h : ticks) lo = (std::min)(lo, h.tick), hi = (std::max)(hi, h.tick);
        if (hi - lo >= kMaxTickSpan) {
            *err = "the recording's tick range is implausible";
            return false;
        }
        out->firstTick = lo;
        out->lastTick = hi;
        out->ticks.assign(hi - lo + 1, TickHash{});
        out->have.assign(hi - lo + 1, 0);
        for (const TickHash& h : ticks) {
            out->ticks[h.tick - lo] = h;
            out->have[h.tick - lo] = 1;
        }
    }
    return true;
}

std::string ArmRefusal(const Recording& rec, const ArmEnv& env) {
    if (env.inLobby) return "leave the lobby first: replays run offline only";
    if (env.online) return "replays run offline only";
    if (env.inMatch) return "finish or quit the current match first";
    if (rec.online) return "online matches cannot be replayed yet";
    if (!rec.exeBuild.empty() && rec.exeBuild != env.exeBuild) return "recorded with game build " + rec.exeBuild;
    if (rec.contentHash.substr(0, 16) != env.contentHash.substr(0, 16) && !env.anyContent) {
        auto shortHash = [](const std::string& h) { return h.empty() ? std::string("vanilla") : h.substr(0, 16); };
        return "recorded with different mod content (" + shortHash(rec.contentHash) + ", here " +
               shortHash(env.contentHash) + "); [Wormsign] ReplayAnyContent=1 replays it anyway";
    }
    if (rec.seeds.empty()) return "the recording has no seeds";
    if (rec.ticks.empty()) return "the recording has no ticks";
    return "";
}
}  // namespace melange::wormsign
