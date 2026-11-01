// sandbox_selftest fakes for wum.spotify (Melange 0.13): link-time stand-ins for spotify::host:: (spotify/host.h), so
// wum_spotify.cpp runs without the network, the browser or DPAPI. Included inside namespace melange, after the other
// fakes. The fake host keeps a small model the cases inspect (fake_spotify::*) and delivers answers and status events
// on request. The real host's logic is tested by spotify_selftest.
}  // namespace melange
#include "spotify/host.h"
namespace melange {
namespace fake_spotify {
struct Submitted {
    std::string mod;
    spotify::logic::Request req;
    uint32_t token = 0, rid = 0, gen = 0;
};
inline bool g_available = true;
inline std::string g_reason = "no-client-id";
inline spotify::host::Status g_status{"disconnected", "", std::nullopt, false, ""};
inline std::set<std::string> g_granted;
inline bool g_connectOk = true;
inline std::string g_connectReason = "busy";
inline std::vector<std::string> g_connects;     // mods that started a sign-in
inline std::vector<std::string> g_disconnects;  // mods that signed out
inline std::vector<Submitted> g_submitted;
inline std::vector<uint32_t> g_cancelled;  // rids dropped by CancelGen
inline std::vector<std::pair<std::string, uint32_t>> g_cancelGens;  // every CancelGen call
inline std::set<std::string> g_touched;
inline uint32_t g_nextRid = 1;
inline spotify::host::Sink* g_sink = nullptr;

inline json::Value Parse(const std::string& s) {
    json::Value v;
    json::Error e;
    json::Parse(s, &v, &e);
    return v;
}
inline const Submitted* Find(uint32_t rid) {
    for (const Submitted& s : g_submitted)
        if (s.rid == rid) return &s;
    return nullptr;
}
// Delivers a request's answer to Lua (the callback runs now, done = true). Kept, so a second answer can be tried.
inline void AnswerRequest(uint32_t rid, const std::string& argJson) {
    const Submitted* s = Find(rid);
    if (s && g_sink && s->token) g_sink->Callback(s->mod, s->token, Parse(argJson), true);
}
inline void EmitStatus(const std::string& mod, const std::string& json) {
    if (g_sink) g_sink->Event(mod, Parse(json));
}
inline bool Cancelled(uint32_t rid) { return std::find(g_cancelled.begin(), g_cancelled.end(), rid) != g_cancelled.end(); }
}  // namespace fake_spotify

namespace spotify::host {
bool Available(std::string* reason) {
    if (!fake_spotify::g_available) *reason = fake_spotify::g_reason;
    return fake_spotify::g_available;
}
void Touch(const std::string& mod) { fake_spotify::g_touched.insert(mod); }
Status GetStatus(const std::string& mod) {
    Status s = fake_spotify::g_status;
    if (!mod.empty() && s.state == "connected" && !fake_spotify::g_granted.count(mod)) {
        s.state = "disconnected";
        s.user.clear();
        s.premium.reset();
    }
    return s;
}
bool Granted(const std::string& mod) { return fake_spotify::g_granted.count(mod) != 0; }
bool Connect(const std::string& mod, std::string* reason) {
    if (!fake_spotify::g_connectOk) {
        *reason = fake_spotify::g_connectReason;
        return false;
    }
    fake_spotify::g_connects.push_back(mod);
    return true;
}
void Disconnect(const std::string& mod) {
    fake_spotify::g_disconnects.push_back(mod);
    fake_spotify::g_granted.clear();
}
uint32_t Submit(const std::string& mod, uint32_t gen, const logic::Request& r, uint32_t token, std::string* reason) {
    if (!fake_spotify::g_granted.count(mod)) {
        *reason = "auth";
        return 0;
    }
    const uint32_t rid = fake_spotify::g_nextRid++;
    fake_spotify::g_submitted.push_back({mod, r, token, rid, gen});
    return rid;
}
void CancelGen(const std::string& mod, uint32_t gen) {
    fake_spotify::g_cancelGens.emplace_back(mod, gen);
    for (const fake_spotify::Submitted& s : fake_spotify::g_submitted)
        if (s.mod == mod && s.gen == gen && !fake_spotify::Cancelled(s.rid)) fake_spotify::g_cancelled.push_back(s.rid);
}
void SetSink(Sink* sink) { fake_spotify::g_sink = sink; }
}  // namespace spotify::host
