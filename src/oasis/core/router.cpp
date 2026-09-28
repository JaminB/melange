#include "oasis/core/router.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include "oasis/core/queue.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::oasis {
namespace core {
namespace {
constexpr int kMaxChannels = 256;
constexpr int kMaxQueuedCalls = 16;
constexpr int kMaxFaults = 3;
constexpr double kPumpBudgetMs = 0.5;

struct SubCb { SubscribeFn fn; void* user; };
struct ChannelSlot {
    std::atomic<uint32_t> id{0};
    std::atomic<int> subs{0};
    std::string name;
    ChannelOptions opt;
    std::vector<SubCb> cbs;
};
struct Method { int handle; std::string name; RpcFn fn; void* user; uint32_t flags; int faults; };
struct Panel { int handle; std::string id, title, entry; std::wstring dir; };

struct Client {
    int id = 0;
    HANDLE wake = nullptr;
    std::mutex mx;                 // box, closeCode, closeReason, gone
    Outbox box;
    uint16_t closeCode = 0;
    std::string closeReason;
    bool gone = false;
    bool refused = false;
    std::atomic<bool> hello{false};
    std::atomic<int> pending{0};
    std::vector<ChannelId> subs;   // g_reg
    explicit Client(Outbox::Limits l) : box(l) {}
};

struct PendingCall { int client; int64_t id; int method; std::string name, params; };
struct SubEvent { ChannelId ch; int client; std::string filter; bool on; SubCb cb; bool main; };

std::shared_mutex g_reg;  // everything below up to g_host
ChannelSlot g_ch[kMaxChannels];
uint32_t g_chGen = 0;
std::vector<Method> g_methods;
std::vector<Panel> g_panels;
std::map<int, std::shared_ptr<Client>> g_clients;
int g_nextClient = 1, g_nextHandle = 1;
Config g_cfg;
Host g_host;
std::string g_build = "dev";

std::mutex g_pumpMx;
std::deque<PendingCall> g_calls;
std::deque<SubEvent> g_subEvents;
std::atomic<bool> g_pumpWork{false};

struct Counters {
    std::atomic<uint64_t> framesOut{0}, bytesOut{0}, bytesIn{0}, dropped{0}, authFailures{0}, rpcCalls{0};
} g_ct;

bool ValidName(std::string_view n, size_t max, bool needDot) {
    if (n.empty() || n.size() > max) return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    return !needDot || (n.find('.') != std::string_view::npos && n.front() != '.' && n.back() != '.');
}

int Slot(ChannelId ch) { return ch ? static_cast<int>((ch - 1) % kMaxChannels) : -1; }

ChannelSlot* LiveSlot(ChannelId ch) {
    const int s = Slot(ch);
    return s >= 0 && g_ch[s].id.load(std::memory_order_acquire) == ch ? &g_ch[s] : nullptr;
}

std::shared_ptr<Client> FindClient(int id) {
    std::shared_lock lk(g_reg);
    auto it = g_clients.find(id);
    return it == g_clients.end() ? nullptr : it->second;
}

void Send(Client& c, std::string msg) {
    std::lock_guard lk(c.mx);
    if (c.gone) return;
    if (!c.box.PushControl(std::move(msg)) && !c.closeCode) {
        c.closeCode = kCloseQueue;
        c.closeReason = "connection queue over the limit";
    }
    SetEvent(c.wake);
}

void RequestClose(Client& c, uint16_t code, const char* reason) {
    std::lock_guard lk(c.mx);
    if (!c.closeCode) {
        c.closeCode = code;
        c.closeReason = reason ? reason : "";
    }
    SetEvent(c.wake);
}

void Json(std::string& o, const json::Value& v) {
    switch (v.type) {
        case json::Type::Null: o += "null"; break;
        case json::Type::Bool: o += v.boolean ? "true" : "false"; break;
        case json::Type::Number: {
            char b[32];
            if (v.IsInteger()) snprintf(b, sizeof b, "%lld", static_cast<long long>(v.number));
            else snprintf(b, sizeof b, "%.17g", v.number);
            o += b;
            break;
        }
        case json::Type::String: o += '"'; o += jsonmini::Escape(v.string); o += '"'; break;
        case json::Type::Array:
            o += '[';
            for (size_t i = 0; i < v.items.size(); ++i) {
                if (i) o += ',';
                Json(o, v.items[i]);
            }
            o += ']';
            break;
        case json::Type::Object:
            o += '{';
            for (size_t i = 0; i < v.members.size(); ++i) {
                if (i) o += ',';
                o += '"';
                o += jsonmini::Escape(v.members[i].first);
                o += "\":";
                Json(o, v.members[i].second);
            }
            o += '}';
            break;
    }
}

std::string ErrMsg(int64_t id, bool hasId, int code, std::string_view msg) {
    return jsonmini::Obj().Str("t", "err").Raw("id", hasId ? std::to_string(id) : "null").Int("code", code).Str("msg", msg).End();
}

std::string ResultMsg(int64_t id, const Result& r) {
    if (!r.ok) return ErrMsg(id, true, r.code ? r.code : kErrRefused, r.message);
    return jsonmini::Obj().Str("t", "res").Int("id", id).Raw("r", r.json.empty() ? "null" : r.json).End();
}

int InvokeCpp(RpcFn fn, const Call* c, Result* r, void* user) {
    try {
        fn(*c, *r, user);
        return 0;
    } catch (...) {
        return 1;
    }
}

// 0 = ran, else the exception code (1 for a C++ exception).
unsigned long InvokeSeh(RpcFn fn, const Call* c, Result* r, void* user) {
    __try {
        return static_cast<unsigned long>(InvokeCpp(fn, c, r, user));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

void SubCallSeh(SubscribeFn fn, ChannelId ch, int client, std::string_view* filter, bool on, void* user) {
    __try {
        fn(ch, client, *filter, on, user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

Result Run(const Method& m, int client, std::string_view params) {
    Result r;
    const Call call{m.name, params, client};
    const unsigned long code = InvokeSeh(m.fn, &call, &r, m.user);
    if (!code) return r;
    int faults = 0;
    {
        std::unique_lock lk(g_reg);
        for (auto& x : g_methods)
            if (x.handle == m.handle) faults = ++x.faults;
    }
    char b[96];
    snprintf(b, sizeof b, "handler faulted (0x%08lx)%s", code, faults >= kMaxFaults ? "; method disabled" : "");
    Result f;
    f.ok = false;
    f.code = kErrFault;
    f.message = b;
    return f;
}

void Dispatch(std::vector<SubEvent>& evs) {
    std::vector<SubEvent> main;
    for (auto& e : evs) {
        if (e.main) {
            main.push_back(std::move(e));
            continue;
        }
        std::string_view f = e.filter;
        SubCallSeh(e.cb.fn, e.ch, e.client, &f, e.on, e.cb.user);
    }
    if (main.empty()) return;
    std::lock_guard lk(g_pumpMx);
    for (auto& e : main) g_subEvents.push_back(std::move(e));
    g_pumpWork = true;
}

std::string Welcome() {
    std::shared_lock lk(g_reg);
    jsonmini::Arr ch, me, pa;
    for (const auto& s : g_ch)
        if (s.id.load()) ch.Str(s.name);
    for (const auto& m : g_methods)
        if (!(m.flags & kRpcGameOnly) || g_host.server == "game") me.Str(m.name);
    for (const auto& p : g_panels)
        pa.Raw(jsonmini::Obj().Str("id", p.id).Str("title", p.title).Str("url", "/ext/" + p.id + "/" + p.entry).End());
    jsonmini::Obj o;
    o.Str("t", "welcome").Int("proto", kProtocol).Str("build", g_build).Str("server", g_host.server);
    if (!g_host.gameJson.empty()) o.Raw("game", g_host.gameJson);
    o.Raw("channels", ch.End()).Raw("methods", me.End()).Raw("panels", pa.End());
    o.Raw("limits", jsonmini::Obj()
                        .Int("maxClients", g_cfg.maxClients)
                        .UInt("maxMessageKB", g_cfg.maxMessageKB)
                        .UInt("maxQueueKB", g_cfg.maxQueueKB)
                        .Int("maxQueuedCalls", kMaxQueuedCalls)
                        .Bool("readOnly", g_cfg.readOnly)
                        .End());
    return o.End();
}

void OnSub(Client& c, const json::Value& v, bool on) {
    const json::Value* chv = v.Get("ch");
    const json::Value* idv = v.Get("id");
    const bool hasId = idv && idv->IsInteger();
    const int64_t id = hasId ? static_cast<int64_t>(idv->number) : 0;
    if (!chv || !chv->IsString()) {
        if (hasId) Send(c, ErrMsg(id, true, kErrEnvelope, "ch expected"));
        return;
    }
    const json::Value* fv = v.Get("filter");
    if (on && fv && !fv->IsObject() && !fv->IsNull()) {
        if (hasId) Send(c, ErrMsg(id, true, kErrParams, "filter must be an object"));
        return;
    }
    std::string filter = "{}";
    if (on && fv && fv->IsObject()) {
        filter.clear();
        Json(filter, *fv);
    }
    std::vector<SubEvent> evs;
    bool found = false;
    {
        std::unique_lock lk(g_reg);
        for (auto& s : g_ch) {
            const ChannelId sid = s.id.load();
            if (!sid || s.name != chv->string) continue;
            found = true;
            bool was = false;
            for (auto x : c.subs) was |= x == sid;
            if (on) {
                {
                    std::lock_guard ck(c.mx);
                    c.box.Subscribe(sid, s.name, s.opt.overflow, static_cast<size_t>(s.opt.maxQueueKB) * 1024);
                }
                if (!was) {
                    c.subs.push_back(sid);
                    ++s.subs;
                }
            } else if (was) {
                {
                    std::lock_guard ck(c.mx);
                    c.box.Unsubscribe(sid);
                }
                std::erase(c.subs, sid);
                --s.subs;
            }
            if (on || was)
                for (const auto& cb : s.cbs) evs.push_back(SubEvent{sid, c.id, filter, on, cb, s.opt.mainThreadSubscribe});
            break;
        }
    }
    if (!found) {
        if (hasId) Send(c, ErrMsg(id, true, kErrParams, "unknown channel"));
        return;
    }
    if (hasId) Send(c, jsonmini::Obj().Str("t", "res").Int("id", id).Raw("r", "true").End());
    Dispatch(evs);
}

void OnCall(Client& c, const json::Value& v) {
    const json::Value* idv = v.Get("id");
    if (!idv || !idv->IsInteger()) return Send(c, ErrMsg(0, false, kErrEnvelope, "id must be an integer"));
    const int64_t id = static_cast<int64_t>(idv->number);
    const json::Value* mv = v.Get("m");
    if (!mv || !mv->IsString()) return Send(c, ErrMsg(id, true, kErrEnvelope, "m expected"));
    const json::Value* pv = v.Get("p");
    std::string params = "{}";
    if (pv && !pv->IsNull()) {
        if (!pv->IsObject()) return Send(c, ErrMsg(id, true, kErrParams, "p must be an object"));
        params.clear();
        Json(params, *pv);
    }
    Method m{};
    bool found = false, readOnly = false, game = true;
    {
        std::shared_lock lk(g_reg);
        for (const auto& x : g_methods)
            if (x.name == mv->string) {
                m = x;
                found = true;
            }
        readOnly = g_cfg.readOnly;
        game = g_host.server == "game";
    }
    if (!found) return Send(c, ErrMsg(id, true, kErrMethod, "unknown method"));
    if (m.faults >= kMaxFaults) return Send(c, ErrMsg(id, true, kErrFault, "method disabled after repeated faults"));
    if ((m.flags & kRpcMutating) && readOnly) return Send(c, ErrMsg(id, true, kErrReadOnly, "Oasis is read-only ([Oasis] ReadOnly=1)"));
    if ((m.flags & kRpcGameOnly) && !game) return Send(c, ErrMsg(id, true, kErrNotInMatch, "the game is not running"));
    ++g_ct.rpcCalls;
    if (m.flags & kRpcServerThread) return Send(c, ResultMsg(id, Run(m, c.id, params)));
    if (c.pending.load() >= kMaxQueuedCalls) return Send(c, ErrMsg(id, true, kErrBusy, "too many queued calls"));
    ++c.pending;
    std::lock_guard lk(g_pumpMx);
    g_calls.push_back(PendingCall{c.id, id, m.handle, m.name, std::move(params)});
    g_pumpWork = true;
}

void SysPing(const Call&, Result& r, void*) {
    uint64_t frame = 0;
    {
        std::shared_lock lk(g_reg);
        if (g_host.frame) frame = g_host.frame();
    }
    r.json = jsonmini::Obj().UInt("frame", frame).UInt("ms", GetTickCount64()).End();
}
}  // namespace

void SetHost(const Host& h) {
    std::unique_lock lk(g_reg);
    g_host = h;
}

void SetBuild(std::string b) {
    std::unique_lock lk(g_reg);
    g_build = std::move(b);
}

std::string Build() {
    std::shared_lock lk(g_reg);
    return g_build;
}

void Pump() {
    if (!g_pumpWork.load(std::memory_order_acquire)) return;
    LARGE_INTEGER t0, t, f;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&f);
    bool evalRan = false;
    for (;;) {
        PendingCall pc;
        {
            std::lock_guard lk(g_pumpMx);
            if (g_calls.empty()) break;
            if (g_calls.front().name == "lua.eval" && evalRan) break;
            pc = std::move(g_calls.front());
            g_calls.pop_front();
        }
        evalRan |= pc.name == "lua.eval";
        Method m{};
        bool found = false;
        {
            std::shared_lock lk(g_reg);
            for (const auto& x : g_methods)
                if (x.handle == pc.method) {
                    m = x;
                    found = true;
                }
        }
        Result r;
        if (!found) {
            r.ok = false;
            r.code = kErrMethod;
            r.message = "method removed";
        } else if (m.faults >= kMaxFaults) {
            r.ok = false;
            r.code = kErrFault;
            r.message = "method disabled after repeated faults";
        } else {
            r = Run(m, pc.client, pc.params);
        }
        if (auto c = FindClient(pc.client)) {
            --c->pending;
            Send(*c, ResultMsg(pc.id, r));
        }
        QueryPerformanceCounter(&t);
        if (static_cast<double>(t.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart) >= kPumpBudgetMs) break;
    }
    std::deque<SubEvent> evs;
    {
        std::lock_guard lk(g_pumpMx);
        evs.swap(g_subEvents);
    }
    for (auto& e : evs) {
        std::string_view fv = e.filter;
        SubCallSeh(e.cb.fn, e.ch, e.client, &fv, e.on, e.cb.user);
    }
    std::lock_guard lk(g_pumpMx);
    g_pumpWork = !g_calls.empty() || !g_subEvents.empty();
}

namespace router {
void Configure(const Config& c) {
    {
        std::unique_lock lk(g_reg);
        g_cfg = c;
    }
    static const int ping = AddMethod("sys.ping", &SysPing, nullptr, kRpcServerThread);
    (void)ping;
}

int Open(void* wake) {
    std::unique_lock lk(g_reg);
    Outbox::Limits l;
    l.connectionBytes = static_cast<size_t>(g_cfg.maxQueueKB) * 1024;
    auto c = std::make_shared<Client>(l);
    c->id = g_nextClient++;
    c->wake = static_cast<HANDLE>(wake);
    int open = 0;
    for (const auto& [id, x] : g_clients) open += !x->gone && !x->refused;
    if (open >= g_cfg.maxClients) {
        c->refused = true;
        c->closeCode = kCloseTooMany;
        c->closeReason = "too many clients";
        SetEvent(c->wake);
    }
    g_clients[c->id] = c;
    return c->id;
}

void Text(int id, std::string_view msg) {
    auto c = FindClient(id);
    if (!c || c->refused) return;
    json::Value v;
    json::Error e;
    const json::Value* t = nullptr;
    if (json::Parse(msg, &v, &e, static_cast<size_t>(g_cfg.maxMessageKB) * 1024) && v.IsObject()) t = v.Get("t");
    if (!t || !t->IsString()) {
        if (!c->hello) return RequestClose(*c, kCloseNoHello, "hello expected");
        return Send(*c, ErrMsg(0, false, kErrEnvelope, "a JSON object with \"t\" expected"));
    }
    if (!c->hello) {
        if (t->string != "hello") return RequestClose(*c, kCloseNoHello, "hello expected");
        const json::Value* p = v.Get("proto");
        if (!p || !p->IsNumber() || p->number != kProtocol) {
            Send(*c, jsonmini::Obj().Str("t", "bye").Str("reason", "protocol").Int("want", kProtocol).End());
            return RequestClose(*c, kCloseProtocol, "protocol version");
        }
        c->hello = true;
        return Send(*c, Welcome());
    }
    if (t->string == "sub") return OnSub(*c, v, true);
    if (t->string == "unsub") return OnSub(*c, v, false);
    if (t->string == "call") return OnCall(*c, v);
    if (t->string == "hello") return Send(*c, ErrMsg(0, false, kErrEnvelope, "hello already received"));
    Send(*c, ErrMsg(0, false, kErrEnvelope, "unknown message type"));
}

bool Take(int id, uint32_t now, std::vector<std::string>* out, uint32_t* wait, uint16_t* code, std::string* reason) {
    auto c = FindClient(id);
    if (!c) return false;
    std::lock_guard lk(c->mx);
    if (c->gone) return false;
    *wait = c->box.Take(now, out);
    *code = c->closeCode;
    if (c->closeCode) *reason = c->closeReason;
    return true;
}

void Close(int id, uint16_t code, const char* reason) {
    if (auto c = FindClient(id)) RequestClose(*c, code, reason);
}

void CloseAll(uint16_t code, const char* reason) {
    std::vector<std::shared_ptr<Client>> all;
    {
        std::shared_lock lk(g_reg);
        for (const auto& kv : g_clients) all.push_back(kv.second);
    }
    for (auto& c : all) RequestClose(*c, code, reason);
}

void Gone(int id) {
    std::vector<SubEvent> evs;
    {
        std::unique_lock lk(g_reg);
        auto it = g_clients.find(id);
        if (it == g_clients.end()) return;
        Client& c = *it->second;
        for (ChannelId ch : c.subs) {
            ChannelSlot* s = LiveSlot(ch);
            if (!s) continue;
            --s->subs;
            for (const auto& cb : s->cbs) evs.push_back(SubEvent{ch, id, "{}", false, cb, s->opt.mainThreadSubscribe});
        }
        c.subs.clear();
        std::lock_guard ck(c.mx);
        c.gone = true;
        SetEvent(c.wake);
    }
    Dispatch(evs);
}

void Release(int id) {
    std::unique_lock lk(g_reg);
    g_clients.erase(id);
}

void CountIo(uint64_t frames, uint64_t out, uint64_t in) {
    if (frames) g_ct.framesOut += frames;
    if (out) g_ct.bytesOut += out;
    if (in) g_ct.bytesIn += in;
}

void CountAuthFailure() { ++g_ct.authFailures; }

int OpenClients() {
    std::shared_lock lk(g_reg);
    int n = 0;
    for (const auto& [id, c] : g_clients) n += !c->gone && !c->refused && c->hello;
    return n;
}
}  // namespace router
}  // namespace core

int Clients() { return core::router::OpenClients(); }

ChannelId AddChannel(const char* name, const ChannelOptions& opt) {
    using namespace core;
    if (!name || !ValidName(name, 48, false)) return 0;
    std::unique_lock lk(g_reg);
    int freeSlot = -1;
    for (int i = 0; i < kMaxChannels; ++i) {
        if (!g_ch[i].id.load()) {
            if (freeSlot < 0) freeSlot = i;
        } else if (g_ch[i].name == name) {
            return 0;
        }
    }
    if (freeSlot < 0) return 0;
    ChannelSlot& s = g_ch[freeSlot];
    s.name = name;
    s.opt = opt;
    if (s.opt.maxQueueKB == 0) s.opt.maxQueueKB = 1;
    s.cbs.clear();
    s.subs = 0;
    const ChannelId id = ++g_chGen * kMaxChannels + static_cast<uint32_t>(freeSlot) + 1;
    s.id.store(id, std::memory_order_release);
    return id;
}

void RemoveChannel(ChannelId ch) {
    using namespace core;
    std::unique_lock lk(g_reg);
    ChannelSlot* s = LiveSlot(ch);
    if (!s) return;
    for (auto& [id, c] : g_clients) {
        std::erase(c->subs, ch);
        std::lock_guard ck(c->mx);
        c->box.Unsubscribe(ch);
    }
    s->id.store(0, std::memory_order_release);
    s->subs = 0;
    s->cbs.clear();
    s->name.clear();
}

bool HasSubscribers(ChannelId ch) {
    const core::ChannelSlot* s = core::LiveSlot(ch);
    return s && s->subs.load(std::memory_order_relaxed) > 0;
}

namespace {
bool PublishImpl(ChannelId ch, int only, std::string_view json) {
    using namespace core;
    if (!HasSubscribers(ch)) return false;
    bool any = false;
    std::shared_lock lk(g_reg);
    if (!LiveSlot(ch)) return false;
    for (const auto& [id, c] : g_clients) {
        if (only && id != only) continue;
        std::lock_guard ck(c->mx);
        if (c->gone) continue;
        uint64_t lost = 0;
        if (!c->box.Publish(ch, json, &lost)) continue;
        any = true;
        if (lost) g_ct.dropped += lost;
        SetEvent(c->wake);
    }
    return any;
}
}  // namespace

bool Publish(ChannelId ch, std::string_view json) { return PublishImpl(ch, 0, json); }
bool PublishTo(ChannelId ch, int client, std::string_view json) { return client > 0 && PublishImpl(ch, client, json); }

int OnSubscribe(ChannelId ch, SubscribeFn fn, void* user) {
    using namespace core;
    if (!fn) return 0;
    std::unique_lock lk(g_reg);
    ChannelSlot* s = LiveSlot(ch);
    if (!s) return 0;
    s->cbs.push_back(SubCb{fn, user});
    return g_nextHandle++;
}

int AddMethod(const char* name, RpcFn fn, void* user, uint32_t flags) {
    using namespace core;
    if (!name || !fn || !ValidName(name, 64, true)) return 0;
    std::unique_lock lk(g_reg);
    for (const auto& m : g_methods)
        if (m.name == name) return 0;
    const int h = g_nextHandle++;
    g_methods.push_back(Method{h, name, fn, user, flags, 0});
    return h;
}

void RemoveMethod(int handle) {
    std::unique_lock lk(core::g_reg);
    std::erase_if(core::g_methods, [&](const core::Method& m) { return m.handle == handle; });
}

int AddWebPanel(const char* id, const char* title, const wchar_t* dir, const char* entry) {
    using namespace core;
    if (!id || !title || !dir || !ValidName(id, 48, false) || !entry || !SafePath(entry)) return 0;
    std::unique_lock lk(g_reg);
    for (const auto& p : g_panels)
        if (p.id == id) return 0;
    const int h = g_nextHandle++;
    g_panels.push_back(Panel{h, id, title, entry, dir});
    return h;
}

void RemoveWebPanel(int handle) {
    std::unique_lock lk(core::g_reg);
    std::erase_if(core::g_panels, [&](const core::Panel& p) { return p.handle == handle; });
}

Stats GetStats() {
    using namespace core;
    Stats s{};
    {
        std::shared_lock lk(g_reg);
        for (const auto& c : g_ch) s.channels += c.id.load() != 0;
        s.methods = static_cast<uint32_t>(g_methods.size());
    }
    s.clients = static_cast<uint32_t>(Clients());
    s.framesOut = g_ct.framesOut;
    s.bytesOut = g_ct.bytesOut;
    s.bytesIn = g_ct.bytesIn;
    s.dropped = g_ct.dropped;
    s.authFailures = g_ct.authFailures;
    s.rpcCalls = g_ct.rpcCalls;
    return s;
}
}  // namespace melange::oasis
