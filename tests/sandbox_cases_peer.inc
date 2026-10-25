// sandbox_selftest cases for wum.lobby / wum.peer (owner: C3). Included in the test file's anonymous namespace after
// the helpers; defines TestPeer(), run before the docs check. The bus is faked (sandbox_fakes_peer.inc): these cases
// cover the binding (generations, reloads, value conversion, limits, events); peerbus_selftest covers the bus.
namespace peer_cases {
using peerbus::wire::Value;
Value Str(const std::string& s) {
    Value v;
    v.type = Value::Type::Str;
    v.s = s;
    return v;
}
Value Int(int64_t i) {
    Value v;
    v.type = Value::Type::Int;
    v.i = i;
    return v;
}
Value Bool(bool b) {
    Value v;
    v.type = Value::Type::Bool;
    v.b = b;
    return v;
}
Value Table(std::vector<std::pair<Value, Value>> t) {
    Value v;
    v.type = Value::Type::Table;
    v.t = std::move(t);
    return v;
}
std::vector<uint8_t> Bytes(const Value& v) {
    std::vector<uint8_t> b;
    peerbus::wire::EncodeValue(v, &b, nullptr);
    return b;
}
const Value* Field(const Value& t, const std::string& k) {
    for (const auto& [key, v] : t.t)
        if (key.type == Value::Type::Str && key.s == k) return &v;
    return nullptr;
}
std::string ModA(int version) {
    return "CH = wum.peer.open{version = " + std::to_string(version) + "}\n"
           "GOT, UPS = {}, {}\n"
           "CH:on(function(from, t, meta) GOT[#GOT + 1] = {from = from, t = t, meta = meta} end)\n"
           "CH:onPeer(function(id, up) UPS[#UPS + 1] = id .. ':' .. tostring(up) end)\n";
}
std::string Res(const char* mod, const std::string& call) {  // "a:b" of a two-result call
    return Eval(mod, "local a, b = " + call + " return tostring(a) .. ':' .. tostring(b)");
}
}  // namespace peer_cases

void TestPeer() {
    using namespace peer_cases;
    fake_peer::Reset();
    ExpectEq(Eval(nullptr, "return tostring(wum.lobby.state())"), "nil", "no lobby outside a net session");
    ExpectEq(Eval(nullptr, "return tostring(wum.lobby.isHost())"), "false", "isHost is false outside a session");
    ExpectEq(Eval(nullptr, "local a, b = wum.peer.now(), wum.peer.now() return tostring(math.type(a) == 'float' and b > a)"), "true",
             "wum.peer.now() is a rising float");
    Expect(Eval(nullptr, "return pcall(wum.peer.open, {version = 1})").rfind("false", 0) == 0, "open needs a mod context");

    SetMod("peera", ModA(2));
    Expect(sandbox::LoadMod("peera"), "peera loads: " + StatusOf("peera").error);
    const peerbus::ChannelId ch = fake_peer::ChannelOf("peera");
    Expect(ch != 0 && fake_peer::g_chans[ch].version == 2, "open{version=2} opens the mod's channel");
    ExpectEq(Eval("peera", "return tostring(CH)"), "peer channel (open)", "a channel userdata");
    ExpectEq(Res("peera", "wum.peer.open{version = 2}"), "nil:already-open", "one channel per mod");
    Expect(Eval("peera", "return pcall(wum.peer.open, {version = 1.5})").rfind("false", 0) == 0, "a float version raises");
    Expect(Eval("peera", "return pcall(wum.peer.open, {version = 0})").rfind("false", 0) == 0, "version 0 raises");
    Expect(Eval("peera", "return pcall(wum.peer.open, 'x')").rfind("false", 0) == 0, "a non-table argument raises");
    ExpectEq(Eval("peera", "return tostring(getmetatable(CH))"), "false", "the channel metatable is protected");

    // send: value conversion, options and the bus's reasons
    ExpectEq(Eval("peera", "return tostring(CH:send('76561198000000001', {a = 1, b = 'x', c = {true, 2.5}}))"), "true", "send ok");
    Expect(fake_peer::g_sent.size() == 1 && fake_peer::g_sent[0].to == 76561198000000001ULL && fake_peer::g_sent[0].reliable,
           "send reaches the bus, reliable by default");
    if (!fake_peer::g_sent.empty()) {
        Value v;
        const auto& p = fake_peer::g_sent[0].payload;
        Expect(peerbus::wire::DecodeValue(p.data(), p.size(), &v) && v.type == Value::Type::Table && v.t.size() == 3,
               "the payload is a 3-key table");
        const Value* a = Field(v, "a");
        const Value* c = Field(v, "c");
        Expect(a && a->type == Value::Type::Int && a->i == 1, "integers stay integers");
        Expect(c && c->type == Value::Type::Table && c->t.size() == 2, "nested tables");
    }
    ExpectEq(Eval("peera", "return tostring(CH:send('5', {}, {reliable = false}))"), "true", "unreliable send");
    Expect(!fake_peer::g_sent.empty() && !fake_peer::g_sent.back().reliable, "reliable=false reaches the bus");
    ExpectEq(Res("peera", "CH:send('abc', {})"), "false:no-peer", "a bad id is no-peer");
    ExpectEq(Res("peera", "CH:send('', {})"), "false:no-peer", "an empty id is no-peer");
    fake_peer::g_status = peerbus::SendStatus::Rate;
    ExpectEq(Res("peera", "CH:send('5', {})"), "false:rate", "rate");
    fake_peer::g_status = peerbus::SendStatus::NoSession;
    ExpectEq(Res("peera", "CH:send('5', {})"), "false:no-session", "no-session");
    fake_peer::g_status = peerbus::SendStatus::Offline;
    ExpectEq(Res("peera", "CH:send('5', {})"), "false:offline", "offline");
    fake_peer::g_status = peerbus::SendStatus::Ok;
    ExpectEq(Res("peera", "CH:send('5', {s = string.rep('x', 1025)})"), "false:size", "strings over 1024 bytes");
    ExpectEq(Eval("peera", "return tostring(CH:send('5', {s = string.rep('x', 1024)}))"), "true", "a 1024-byte string");
    ExpectEq(Res("peera", "CH:send('5', {{{{{{{}}}}}}})"), "false:size", "tables 7 deep");
    ExpectEq(Eval("peera", "return tostring(CH:send('5', {{{{{{}}}}}}))"), "true", "tables 6 deep");
    ExpectEq(Res("peera", "(function() local t = {} for i = 1, 257 do t[i] = i end return CH:send('5', t) end)()"), "false:size",
             "257 keys");
    ExpectEq(Eval("peera", "local t = {} for i = 1, 256 do t[i] = i end return tostring(CH:send('5', t))"), "true", "256 keys");
    ExpectEq(Res("peera", "(function() local t = {} t.self = t return CH:send('5', t) end)()"), "false:size",
             "a table that contains itself");
    ExpectEq(Res("peera", "(function() local t = {} for i = 1, 9 do t[i] = string.rep('y', 1000) end return CH:send('5', t) end)()"),
             "false:size", "over 8 KB");
    ExpectEq(Res("peera", "CH:send('5', {s = string.rep('z', 950)}, {reliable = false})"), "false:size", "unreliable over 900 bytes");
    Expect(Eval("peera", "return pcall(CH.send, CH, '5', {f = print})").rfind("false", 0) == 0, "a function value raises");
    Expect(Eval("peera", "return pcall(CH.send, CH, '5', {[{}] = 1})").rfind("false", 0) == 0, "a table key raises");
    Expect(Eval("peera", "return pcall(CH.send, CH, '5', 'text')").rfind("false", 0) == 0, "a non-table message raises");
    Expect(Eval("peera", "return pcall(CH.send, CH, {}, {})").rfind("false", 0) == 0, "a non-string id raises");

    // broadcast, peers, clock
    fake_peer::g_peers = {11, 12};
    ExpectEq(Eval("peera", "return tostring(CH:broadcast({x = 1}))"), "2", "broadcast counts the members reached");
    ExpectEq(Res("peera", "CH:broadcast({s = string.rep('x', 2000)})"), "0:size", "broadcast refuses an oversized value");
    ExpectEq(Eval("peera", "return table.concat(CH:peers(), ',')"), "11,12", "peers()");
    fake_peer::g_clocks[11] = {12.5, 40.0, 3};
    ExpectEq(Eval("peera", "local c = CH:clock('11') return string.format('%g %g %d', c.offset, c.rtt, c.samples)"), "12.5 40 3",
             "clock()");
    ExpectEq(Eval("peera", "return tostring(CH:clock('99'))"), "nil", "no clock before the first sample");

    // delivery
    const Value msg = Table({{Str("msg"), Str("hi")}, {Str("n"), Int(3)}, {Str("nested"), Table({{Str("k"), Bool(true)}})},
                             {Int(1), Str("first")}});
    fake_peer::Deliver(ch, 11, Bytes(msg), true, 1234.5);
    ExpectEq(Eval("peera", "local g = GOT[1] return g.from .. ' ' .. g.t.msg .. ' ' .. g.t.n .. ' ' .. tostring(g.t.nested.k) .. ' ' "
                           ".. g.t[1] .. ' ' .. tostring(g.meta.reliable) .. ' ' .. g.meta.at"),
             "11 hi 3 true first true 1234.5", "on(from, tbl, meta)");
    fake_peer::Deliver(ch, 11, {0xff, 0x00}, false, 1.0);
    fake_peer::Deliver(ch + 100, 11, Bytes(msg), false, 1.0);
    ExpectEq(Eval("peera", "return #GOT"), "1", "a malformed payload or another channel's message is not delivered");
    fake_peer::PeerUp(ch, 11, true);
    fake_peer::PeerUp(ch, 12, false);
    ExpectEq(Eval("peera", "return table.concat(UPS, ' ')"), "11:true 12:false", "onPeer(id, up)");

    // presence
    ExpectEq(Eval("peera", "return tostring(CH:setPresence({ready = true}))"), "true", "setPresence");
    Expect(fake_peer::g_presenceSet[ch].find("\"ready\":true") != std::string::npos, "presence is JSON: " + fake_peer::g_presenceSet[ch]);
    ExpectEq(Res("peera", "CH:setPresence({s = string.rep('x', 300)})"), "false:size", "presence over 256 bytes");
    ExpectEq(Eval("peera", "return tostring(CH:setPresence(nil))"), "true", "setPresence(nil) clears");
    ExpectEq(fake_peer::g_presenceSet[ch], "", "cleared presence is empty");
    fake_peer::g_presenceOf[11] = "{\"pack\":\"ready\"}";
    fake_peer::g_presenceOf[12] = "[1]";
    fake_peer::g_presenceOf[13] = "not json";
    ExpectEq(Eval("peera", "return CH:presence('11').pack"), "ready", "presence(id)");
    ExpectEq(Eval("peera", "return tostring(CH:presence('12')) .. tostring(CH:presence('13')) .. tostring(CH:presence('14'))"),
             "nilnilnil", "presence that is not an object, not JSON, or missing");

    // wum.lobby
    fake_peer::g_inSession = true;
    fake_peer::g_view.lobby = 100;
    fake_peer::g_view.me = 10;
    fake_peer::g_view.owner = 10;
    fake_peer::g_view.phase = "lobby";
    fake_peer::g_view.members = {{10, "Me", "0.12.0", "1;peera@2", true, true},
                                 {11, "Bob", "0.12.0", "1;other@1,peera@2", false, false},
                                 {12, "Cy", "", "", false, false},
                                 {13, "Di", "0.12.0", "1;peera@1", false, false}};
    ExpectEq(Eval("peera", "local s = wum.lobby.state() local out = {s.lobby, s.me, s.owner, tostring(s.isHost), s.phase} "
                           "for _, m in ipairs(s.members) do out[#out + 1] = m.id .. '/' .. m.name .. '/' .. tostring(m.me) .. '/' "
                           ".. tostring(m.host) .. '/' .. tostring(m.melange) .. '/' .. tostring(m.peer) end return table.concat(out, ' ')"),
             "100 10 10 true lobby 10/Me/true/true/0.12.0/false 11/Bob/false/false/0.12.0/true 12/Cy/false/false/nil/false "
             "13/Di/false/false/0.12.0/false",
             "lobby.state(): peer is true only for the same mod and version");
    ExpectEq(Eval("peera", "return tostring(wum.lobby.isHost())"), "true", "isHost()");
    fake_peer::g_view.owner = 11;
    ExpectEq(Eval("peera", "return tostring(wum.lobby.isHost())"), "false", "isHost() after the lobby changed hands");

    Eval("peera", "EV = {} for _, n in ipairs{'melange.lobby.enter', 'melange.lobby.leave', 'melange.lobby.members', "
                  "'melange.lobby.owner', 'melange.lobby.phase'} do wum.events.on(n, function(p, name) "
                  "EV[#EV + 1] = name:sub(15) .. ':' .. tostring(p.isHost) .. ':' .. tostring(p.owner) .. ':' .. tostring(p.phase) end) end");
    fake_peer::Lobby(peerbus::LobbyEvent::Enter);
    fake_peer::Lobby(peerbus::LobbyEvent::Owner);
    fake_peer::g_view.phase = "loading";
    fake_peer::Lobby(peerbus::LobbyEvent::Phase);
    fake_peer::Lobby(peerbus::LobbyEvent::Members);
    fake_peer::Lobby(peerbus::LobbyEvent::Leave);
    Frames(1);
    ExpectEq(Eval("peera", "return table.concat(EV, ' ')"),
             "enter:false:nil:nil owner:false:11:nil phase:nil:nil:loading members:nil:nil:nil leave:nil:nil:nil",
             "melange.lobby.* events and payloads");

    // a reload of the same version keeps the channel; the new generation's callbacks get the messages
    const int opens = fake_peer::g_opens, closes = fake_peer::g_closes;
    Expect(sandbox::ReloadMod("peera"), "peera reloads: " + StatusOf("peera").error);
    Expect(fake_peer::ChannelOf("peera") == ch && fake_peer::g_opens == opens && fake_peer::g_closes == closes,
           "a same-version reload keeps the bus channel");
    fake_peer::Deliver(ch, 12, Bytes(Table({{Str("x"), Int(1)}})), true, 2.0);
    ExpectEq(Eval("peera", "return #GOT .. ':' .. GOT[1].from"), "1:12", "the reloaded generation receives");
    ExpectEq(Eval("peera", "return tostring(CH:send('5', {}))"), "true", "the reloaded generation sends");

    // a reload with another version reopens
    SetMod("peera", ModA(3));
    Expect(sandbox::ReloadMod("peera"), "peera reloads as version 3: " + StatusOf("peera").error);
    const peerbus::ChannelId ch3 = fake_peer::ChannelOf("peera");
    Expect(ch3 != 0 && ch3 != ch && fake_peer::g_chans[ch3].version == 3 && fake_peer::g_closes == closes + 1,
           "another version closes the old channel and opens a new one");

    // a failed reload leaves the running generation its channel
    SetMod("peera", "CH = wum.peer.open{version = 3}\nerror('boom')\n");
    Expect(!sandbox::ReloadMod("peera"), "a failing reload fails");
    Expect(fake_peer::ChannelOf("peera") == ch3, "a failed reload keeps the channel");
    ExpectEq(Eval("peera", "return tostring(CH) .. ' ' .. tostring(CH:send('5', {}))"), "peer channel (open) true",
             "the running generation still owns it");

    // close
    ExpectEq(Eval("peera", "return tostring(CH:close())"), "true", "close()");
    Expect(fake_peer::ChannelOf("peera") == 0, "close() closes the bus channel");
    ExpectEq(Eval("peera", "local a, b = CH:send('5', {}) return tostring(a) .. ':' .. b .. ' ' .. tostring(CH) .. ' ' .. "
                           "tostring(CH:close()) .. ' ' .. #CH:peers() .. ' ' .. select(2, CH:broadcast({}))"),
             "false:closed peer channel (closed) false 0 closed", "a closed channel");
    ExpectEq(Eval("peera", "C2 = wum.peer.open{version = 3} return tostring(C2)"), "peer channel (open)", "open again after close");
    sandbox::UnloadMod("peera");
    Expect(fake_peer::ChannelOf("peera") == 0, "unloading closes the channel");

    // [PeerBus] Enabled=0
    fake_peer::g_available = false;
    SetMod("peerc", "A, B = wum.peer.open{version = 1}\n");
    Expect(sandbox::LoadMod("peerc"), "peerc loads");
    ExpectEq(Eval("peerc", "return tostring(A) .. ':' .. B"), "nil:unavailable", "open() without the bus");
    sandbox::UnloadMod("peerc");
    fake_peer::Reset();
}
