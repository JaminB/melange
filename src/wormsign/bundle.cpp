#include "wormsign/bundle.h"

#include <cstdio>

#include "miniz.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"
#include "tools/redact.h"
#include "wormsign/detector_core.h"

namespace melange::wormsign::bundle {
namespace {
std::string Hex(uint64_t v) {
    char b[20];
    snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}
const char* SourceName(Source s) { return s == Source::Replay ? "replay" : "peer"; }

// A contributor value of 0 is "not computed on that machine at that tick" (contrib::HashesAt), not a hash.
bool Skipped(const Contributor& c, bool ours) { return ours ? c.haveOurs && c.ours == 0 : c.haveTheirs && c.theirs == 0; }
std::string ContribValue(const Contributor& c, bool ours) {
    if (Skipped(c, ours)) return std::string("(not hashed ") + (ours ? "here" : "on the peer") + " at this tick: demoted or faulted there)";
    return Hex(ours ? c.ours : c.theirs);
}

bool DetailRng(const std::string& json, uint32_t* out) {
    json::Value v;
    json::Error e;
    if (json.empty() || !json::Parse(json, &v, &e)) return false;
    const json::Value* r = v.Get("rng");
    if (!r || !r->IsString() || r->string.size() < 3) return false;
    char* end = nullptr;
    const unsigned long x = strtoul(r->string.c_str(), &end, 16);
    if (!end || *end) return false;
    *out = static_cast<uint32_t>(x);
    return true;
}
}  // namespace

int RngSteps(uint32_t from, uint32_t to, int max) {
    uint32_t x = from;
    for (int n = 0; n <= max; ++n) {
        if (x == to) return n;
        x = x * 1103515245u + 12345u;
    }
    return -1;
}

std::string RngLine(const Inputs& in) {
    uint32_t ours = 0, theirs = 0;
    if (!DetailRng(in.detailLocal, &ours) || !DetailRng(in.detailPeer, &theirs)) return "";
    char b[320];
    if (ours == theirs) {
        snprintf(b, sizeof b, "# rng: both 0x%08x", ours);
        return b;
    }
    const int behind = RngSteps(theirs, ours), ahead = RngSteps(ours, theirs);
    std::string at;
    for (const auto& [tick, v] : in.rngTrail)
        if (v == theirs) {
            char t[80];
            snprintf(t, sizeof t, " (= our value at the end of tick %u)", tick);
            at = t;
        }
    if (behind >= 0)
        snprintf(b, sizeof b, "# rng: ours 0x%08x, theirs 0x%08x%s: the peer is %d logic-RNG draw%s behind us", ours, theirs,
                 at.c_str(), behind, behind == 1 ? "" : "s");
    else if (ahead >= 0)
        snprintf(b, sizeof b, "# rng: ours 0x%08x, theirs 0x%08x%s: the peer is %d logic-RNG draw%s ahead of us", ours, theirs,
                 at.c_str(), ahead, ahead == 1 ? "" : "s");
    else
        snprintf(b, sizeof b, "# rng: ours 0x%08x, theirs 0x%08x%s: not within 65536 draws of each other (the RNG was reseeded, "
                 "or the states are unrelated)", ours, theirs, at.c_str());
    return b;
}

std::string PeerRef(const std::string& salt, uint64_t steamId) {
    return steamId ? "hash:" + hashutil::ShortSaltedHash(salt, std::to_string(steamId)) : std::string();
}

std::string ReportJson(const Inputs& in) {
    using jsonmini::Arr;
    using jsonmini::Obj;
    const Divergence& d = in.div;
    Arr comps;
    for (int i = 0; i < kEngineComps; ++i) {
        Obj c;
        c.Str("name", detect::CompName(i));
        if (in.haveOurs) c.Str("ours", Hex(in.ours[i]));
        if (in.haveTheirs) c.Str("theirs", Hex(in.theirs[i]));
        c.Bool("differs", (d.compMask >> i) & 1);
        comps.Raw(c.End());
    }
    Arr differing;
    for (int i = 0; i < kEngineComps; ++i)
        if ((d.compMask >> i) & 1) differing.Str(detect::CompName(i));
    Arr contribs;
    for (const Contributor& c : in.contributors) {
        Obj o;
        o.Str("name", c.name).UInt("version", c.version);
        if (c.haveOurs) o.Str("ours", Hex(c.ours));
        if (c.haveTheirs) o.Str("theirs", Hex(c.theirs));
        o.Bool("differs", c.haveOurs && c.haveTheirs && c.ours != c.theirs);
        // false: that side did not compute the contributor at this tick (demoted or faulted there); its value is 0
        if (c.haveOurs) o.Bool("oursComputed", !Skipped(c, true));
        if (c.haveTheirs) o.Bool("theirsComputed", !Skipped(c, false));
        contribs.Raw(o.End());
    }
    Obj engine, mods;
    engine.Str("ours", Hex(d.oursEngine)).Str("theirs", Hex(d.theirsEngine)).Bool("differs", d.oursEngine != d.theirsEngine);
    mods.Str("ours", Hex(d.oursMods)).Str("theirs", Hex(d.theirsMods)).Bool("differs", d.oursMods != d.theirsMods)
        .Bool("compared", in.contribListsMatch);
    Obj detail;
    detail.Bool("local", !in.detailLocal.empty()).Bool("peer", !in.detailPeer.empty());
    Obj root;
    root.Int("format", 1)
        .Str("source", SourceName(d.source))
        .UInt("serial", d.serial)
        .UInt("tick", d.tick)
        .UInt("timeMs", static_cast<unsigned long long>(d.tick) * kTickMs)
        .Str("peer", PeerRef(in.salt, d.peer))
        .Raw("engine", engine.End())
        .Raw("mods", mods.End())
        .UInt("compMask", d.compMask)
        .Raw("differing", differing.End())
        .Raw("components", comps.End())
        .Str("contrib", d.contrib)
        .Bool("contributorsMatch", in.contribListsMatch)
        .Raw("contributors", contribs.End())
        .Raw("peers", in.peersJson.empty() ? "[]" : in.peersJson)
        .Raw("engineChecks", in.engineJson.empty() ? "[]" : in.engineJson)
        .Str("engineCorrelation", in.correlation)
        .Raw("detail", detail.End())
        .Str("recording", in.recording.empty() ? "" : in.recordingName)
        .UInt("engineHash", kEngineHashVersion)
        .Str("melange", in.melangeVersion)
        .Str("exeBuild", in.exeBuild);
    return root.End();
}

std::string DiffText(const Inputs& in) {
    const Divergence& d = in.div;
    std::string s;
    char b[256];
    snprintf(b, sizeof b, "# desync at tick %u (t=%llu ms), match %u, %s %s\n", d.tick,
             static_cast<unsigned long long>(d.tick) * kTickMs, d.serial, SourceName(d.source),
             d.source == Source::Peer ? PeerRef(in.salt, d.peer).c_str() : "");
    s += b;
    s += "# engine " + Hex(d.oursEngine) + " -> " + Hex(d.theirsEngine) + "\n";
    s += "# mods " + Hex(d.oursMods) + " -> " + Hex(d.theirsMods) + (in.contribListsMatch ? "" : " (contributors differ)") + "\n";
    if (in.haveOurs && in.haveTheirs)
        for (int i = 0; i < kEngineComps; ++i)
            if (in.ours[i] != in.theirs[i])
                s += std::string("# component ") + detect::CompName(i) + " " + Hex(in.ours[i]) + " -> " + Hex(in.theirs[i]) + "\n";
    for (const Contributor& c : in.contributors)
        if (c.haveOurs && c.haveTheirs && c.ours != c.theirs)
            s += "# contributor " + c.name + " " + ContribValue(c, true) + " -> " + ContribValue(c, false) + "\n";
    if (const std::string rng = RngLine(in); !rng.empty()) s += rng + "\n";
    if (!in.correlation.empty()) s += "# " + in.correlation + "\n";
    if (in.detailLocal.empty() || in.detailPeer.empty()) {
        s += std::string("# no field diff: the ") + (in.detailLocal.empty() ? "local" : "peer") + " detail record is missing\n";
        return s;
    }
    std::string err;
    const std::vector<std::string> lines = detect::FieldDiff(in.detailLocal, in.detailPeer, &err);
    if (!err.empty()) {
        s += "# no field diff: " + err + "\n";
        return s;
    }
    s += "# fields: ours -> theirs (" + std::to_string(lines.size()) + ")\n";
    for (const std::string& l : lines) s += l + "\n";
    return s;
}

std::string Redact(const Inputs& in, std::string text) {
    text = redact::HashIdsAndIps(text, in.salt);
    if (!in.userName.empty()) text = redact::RedactUserName(text, in.userName);
    if (!in.profileName.empty()) text = redact::RedactUserName(text, in.profileName);
    if (!in.computerName.empty()) text = redact::ReplaceName(text, in.computerName, "%COMPUTERNAME%");
    return text;
}

bool BuildZip(const Inputs& in, std::string* zip, std::vector<std::string>* entries) {
    mz_zip_archive z{};
    if (!mz_zip_writer_init_heap(&z, 0, 256 * 1024)) return false;
    bool ok = true;
    auto add = [&](const char* name, const std::string& data, bool text) {
        if (!ok) return;
        const std::string body = text ? Redact(in, data) : data;
        ok = mz_zip_writer_add_mem(&z, name, body.data(), body.size(), 6) != 0;
        if (ok && entries) entries->push_back(name);
    };
    add("report.json", ReportJson(in), true);
    add("diff.txt", DiffText(in), true);
    if (!in.detailLocal.empty()) add("detail-local.json", in.detailLocal, true);
    if (!in.detailPeer.empty()) add("detail-peer.json", in.detailPeer, true);
    if (!in.melangeLog.empty()) add("logs/Melange.log", in.melangeLog, true);
    if (!in.jlog.empty()) add("logs/session.jsonl", in.jlog, true);
    add("mods.json", in.modsJson.empty() ? "[]" : in.modsJson, true);
    if (!in.sysinfo.empty()) add("sysinfo.txt", in.sysinfo, true);
    if (ok && !in.recording.empty()) {
        ok = mz_zip_writer_add_mem(&z, in.recordingName.c_str(), in.recording.data(), in.recording.size(), 6) != 0;
        if (ok && entries) entries->push_back(in.recordingName);
    }
    void* buf = nullptr;
    size_t n = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&z, &buf, &n) != 0;
    if (ok) zip->assign(static_cast<const char*>(buf), n);
    if (buf) mz_free(buf);
    mz_zip_writer_end(&z);
    return ok;
}

std::wstring FileName(uint32_t serial, uint32_t tick, int y, int mo, int d, int h, int mi, int s, uint32_t pid) {
    wchar_t b[112];
    swprintf(b, 112, L"desync-%04d%02d%02d-%02d%02d%02d-p%u-m%u-t%u.zip", y, mo, d, h, mi, s, pid, serial, tick);
    return b;
}
}  // namespace melange::wormsign::bundle
