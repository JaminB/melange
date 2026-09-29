#include "wormsign/bundle.h"

#include <cstdio>

#include "miniz.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
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
}  // namespace

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
            s += "# contributor " + c.name + " " + Hex(c.ours) + " -> " + Hex(c.theirs) + "\n";
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
