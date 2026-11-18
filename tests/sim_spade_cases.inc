// sim_selftest: Melange 0.16.0's carve (wum.sim.carve/carveLine), scripted kill (worm:kill, worm:aim) and melee gate
// (wum.sim.melee), driven through fakes the way the Grave Spade uses them. Included by tests/sim_selftest.cpp.
namespace sl = melange::simland;
namespace sm = melange::simmelee;
struct FakeLand final : sl::Backend {
    std::vector<std::string> calls;
    bool indestructible = false;
    bool Carve(const float pos[3], float radius, sl::CarveOut* out) override {
        if (indestructible) {
            out->why = "indestructible";
            return false;
        }
        char b[96];
        snprintf(b, sizeof b, "%g,%g,%g,%g", pos[0], pos[1], pos[2], radius);
        calls.push_back(b);
        out->removed = pos[1] < 0 ? 0 : 3;  // below y 0 is air
        return true;
    }
};
struct FakeMelee final : sm::Backend {
    int active = 1, weapon = 10, state = 0;
    bool inRange = false;
    int strikes = 0;
    int WeaponIndex(const char* name) override {
        if (!strcmp(name, "kWeaponBaseballBat")) return 10;
        if (!strcmp(name, "kWeaponProd")) return 11;
        return -1;
    }
    int ActiveSlot() override { return active; }
    int WeaponOf(int slot) override { return slot == active ? weapon : -1; }
    bool TargetInRange() override { return inRange; }
    int EntityState() override { return state; }
    const char* Strike() override {
        if (state) return "busy";
        ++strikes;
        state = 2;
        return nullptr;
    }
};

const char kSpadeMod[] = R"(
local s = wum.sim.storage
local function r(...) local t = {} for i = 1, arg.n do t[i] = tostring(arg[i]) end return table.concat(t, ",") end
s.claim = r(wum.sim.melee.claim("kWeaponBaseballBat", {dig = true}))
s.claimBad = r(wum.sim.melee.claim("kWeaponBazooka"))
s.mode = r(wum.sim.melee.mode("kWeaponBaseballBat"))
s.list = table.getn(wum.sim.melee.list()) .. ":" .. wum.sim.melee.list()[1].mode
s.carve = r(wum.sim.carve(1, 2, 3, 10))
s.carveBad = r(wum.sim.carve(1, 2, 3, 500))
s.raises = tostring(pcall(wum.sim.carve, "x", 1, 2, 3))
s.log = ""
local function add(t) s.log = s.log .. t .. ";" end
wum.sim.melee.on("press", "kWeaponBaseballBat", function(ev, w, tick, slot, mode) add("press:" .. slot .. ":" .. mode) end)
wum.sim.melee.on("release", "*", function(ev, w, tick, slot, held, mode) add("release:" .. slot .. ":" .. held .. ":" .. mode) end)
wum.sim.melee.on("strike", "*", function(ev, w, tick, slot) add("strike:" .. slot) end)
wum.sim.melee.on("done", "*", function(ev, w, tick, slot) add("done:" .. slot) end)
s.executed = false
wum.sim.melee.on("hit", "kWeaponBaseballBat", function(ev, w, tick, a, v, x, y, z, dmg, imp)
  add("hit:" .. a .. ">" .. v .. ":" .. dmg)
  if not s.executed then
    s.executed = true
    wum.sim.melee.result({cancel = true})
    s.anchorV = r(wum.sim.worm(v):anchor(true))
    s.inHitStrike = r(wum.sim.melee.strike("kWeaponBaseballBat"))
  else
    wum.sim.melee.result({damage = 10, impulse = 0.15})
  end
end)
wum.events.on("sim.wormKilled", function(ev, slot, tick) add("killed:" .. slot) end)
wum.events.on("tick", function(t)
  if t == 3 then s.held3 = r(wum.sim.melee.held("kWeaponBaseballBat")) end
  if t == 4 then s.line = r(wum.sim.carveLine(0, 0, 0, 40, 0, 0, 10, 10)) s.long = r(wum.sim.carveLine(0, 0, 0, 1000, 0, 0, 10)) end
  if t == 5 then s.reach = r(wum.sim.melee.reach("kWeaponBaseballBat")) end
  if t == 6 then s.strike = r(wum.sim.melee.strike("kWeaponBaseballBat")) end
  if t == 8 then s.kill = r(wum.sim.worm(2):kill({explode = false, grave = true, throes = 40}))
                 s.killAgain = r(wum.sim.worm(2):kill()) s.killDead = r(wum.sim.worm(3):kill())
                 s.killBad = tostring(pcall(function() return wum.sim.worm(0):kill({throes = 0}) end))
                 s.aim = r(wum.sim.worm(0):aim()) end
  if t == 9 then s.lateClaim = r(wum.sim.melee.claim("kWeaponProd")) s.notIn = r(wum.sim.melee.result({cancel = true})) end
end)
)";

void TestSpade() {
    FakeWorms worms;
    worms.Set(0, 0, 0, 0, 0, 100, sw::kAmbulatory, "Digger");
    worms.Set(1, 10, 0, 0, 0, 100, sw::kAmbulatory, "Attacker");
    worms.Set(2, 20, 0, 0, 1, 80, sw::kAmbulatory, "Victim");
    worms.Set(3, 30, 0, 0, 1, 0, sw::kDeathThroes, "Gone");
    sw::SetBackend(&worms);
    FakeLand land;
    sl::SetBackend(&land);
    sl::Limits lim;
    lim.perTick = 5;
    lim.perTurn = 6;
    sl::SetLimits(lim);
    sl::ResetMatch();
    FakeMelee melee;
    sm::SetBackend(&melee);
    sm::ResetMatch();
    sw::ResetMatch();

    g_modLog.clear();
    fake::runState = 1;
    fake::halts.clear();
    fake::L = fake::NewMatchVM();
    core::SetSources({{"spade", "1.0.0", "@spade/sim.lua", kSpadeMod}});
    core::ContextCreated(fake::L);
    Expect(core::Init({"GameLogic.Turn.Ended"}, true), "spade mod loaded");
    const int th = melange::sim::AddTickHook([](uint32_t, void*) {
        sm::Tick();
        sw::PollKills();
    }, nullptr, 12);

    Expect(Storage("spade", "claim") == "true" && Storage("spade", "claimBad") == "nil,not melee",
           "claim: a melee weapon only: " + Storage("spade", "claimBad"));
    Expect(Storage("spade", "mode") == "auto" && Storage("spade", "list") == "1:auto", "mode and list");
    Expect(Storage("spade", "carve") == "3" && land.calls.size() == 1 && land.calls[0] == "1,2,3,10",
           "carve passes its arguments and answers the removed count: " + Storage("spade", "carve"));
    Expect(Storage("spade", "carveBad") == "nil,bad arguments" && Storage("spade", "raises") == "false",
           "carve refuses a radius over 200 and raises on a non-number");

    melee.inRange = false;
    core::Update();  // tick 1
    Expect(sm::OnFire(1, true), "auto with no target in range: the press is swallowed (dig)");
    Expect(sm::OnFire(1, true), "a repeat of the held press is swallowed too");
    Expect(!sm::OnFire(0, true), "another worm's press passes");
    core::Update();  // tick 2
    core::Update();  // tick 3
    Expect(Storage("spade", "held3").rfind("1,", 0) == 0 && Storage("spade", "held3").find(",dig") != std::string::npos,
           "held(): slot, ticks, press tick, mode: " + Storage("spade", "held3"));
    Expect(sm::OnFire(1, false), "the dig's release is swallowed");
    core::Update();  // tick 4: carveLine
    Expect(Storage("spade", "line") == "15,5" && land.calls.size() == 6,
           "carveLine: 5 spheres every radius/2: " + Storage("spade", "line"));
    Expect(Storage("spade", "long") == "nil,too long", "carveLine refuses a stroke over 16 spheres");
    const char* why = nullptr;
    Expect(sl::Carve(0, 0, 0, 5, &why) == -1 && std::string(why) == "budget", "the turn budget (6) is spent");
    melee.inRange = true;
    core::Update();  // tick 5
    Expect(Storage("spade", "reach") == "true", "reach: the active worm wields it and a target is in range");
    Expect(!sm::OnFire(1, true), "auto with a target in range: the press goes to the vanilla strike");
    Expect(!sm::OnFire(1, false), "and so does its release");
    core::Update();  // tick 6: strike() from the sim
    Expect(Storage("spade", "strike") == "true" && melee.strikes == 1, "strike() starts the engine swing");
    sm::OnStrike(1);
    const float at[3] = {20, 0, 0};
    const sm::HitResult h1 = sm::OnHit(1, 2, at, 30, 0.12f);
    Expect(h1.cancel && !h1.setDamage, "the first hit is cancelled (the execution)");
    Expect(Storage("spade", "anchorV") == "true" && sw::Anchored(2), "the hit handler anchors the victim");
    Expect(Storage("spade", "inHitStrike") == "nil,in hit", "strike() is refused inside a hit");
    const sm::HitResult h2 = sm::OnHit(1, 2, at, 30, 0.12f);
    Expect(!h2.cancel && h2.setDamage && h2.damage == 10 && h2.setImpulse && std::fabs(h2.impulse - 0.15f) < 1e-6f,
           "later hits get the Shiv's numbers");
    melee.weapon = 5;
    const sm::HitResult h3 = sm::OnHit(1, 2, at, 30, 0.12f);
    Expect(!h3.cancel && !h3.setDamage && sm::HitCount() == 2, "an unclaimed weapon's hit is the engine's own");
    melee.weapon = 10;
    sm::OnDone(1);
    core::Update();  // tick 7
    core::Update();  // tick 8: the kill
    Expect(Storage("spade", "kill") == "true" && worms.kills == std::vector<int>({2}),
           "kill posts the engine's death once: " + Storage("spade", "kill"));
    Expect(Storage("spade", "killAgain") == "nil,dead" && Storage("spade", "killDead") == "nil,dead",
           "a second kill and a dying worm: dead");
    Expect(Storage("spade", "killBad") == "false", "throes 0 raises");
    Expect(Storage("spade", "aim") == "0,0,0,0,1",
           "aim(): pitch, yaw and (sin yaw cos pitch, sin pitch, cos yaw cos pitch): " + Storage("spade", "aim"));
    sw::KillOpts o;
    Expect(sw::KillPending(2, &o) && !o.explode && o.grave && o.throes == 40, "the kill options are kept for the engine hooks");
    uint8_t kh[sw::kKillHashBytes];
    Expect(sw::KillHashBytes(kh, sizeof kh) == sw::kKillHashBytes && kh[20] == 1 && kh[21] == 2, "kill hash: stage and options");
    worms.slots[2].w.active = false;  // the throes ended: the engine unspawned it
    core::Update();  // tick 9
    Expect(!sw::KillPending(2, nullptr) && !sw::AnyKillPending(), "the kill completed");
    Expect(Storage("spade", "lateClaim") == "nil,too late" && Storage("spade", "notIn") == "nil,not in hit",
           "a claim after the chunk and a result outside a hit are refused");
    core::Update();
    const std::string log = Storage("spade", "log");
    Expect(log == "press:1:dig;release:1:2:dig;press:1:strike;release:1:0:strike;hit:1>2:30;hit:1>2:30;strike:1;done:1;killed:2;",
           "event order: " + log);
    uint8_t mh[sm::kHashBytes], mh2[sm::kHashBytes];
    Expect(sm::HashBytes(mh, sizeof mh) == sm::kHashBytes && sm::HashBytes(mh2, sizeof mh2) == sm::kHashBytes &&
               memcmp(mh, mh2, sizeof mh) == 0 && mh[0] == 10,
           "melee hash: the claim and the hits, deterministic");
    uint8_t lh[sl::kHashBytes];
    Expect(sl::HashBytes(lh, sizeof lh) == sl::kHashBytes && sl::GetStats().carves == 6, "land hash: six carves folded");
    land.indestructible = true;
    sl::ResetMatch();
    why = nullptr;
    Expect(sl::Carve(0, 0, 0, 5, &why) == -1 && std::string(why) == "indestructible", "an indestructible level answers so");

    melange::sim::RemoveTickHook(th);
    Expect(lua_gettop(fake::L) == 0 && fake::halts.empty(), "stack balanced");
    core::ContextClosing(fake::L);
    lua_close(fake::L);
    fake::L = nullptr;
    sm::ResetMatch();
    sm::SetBackend(nullptr);
    sl::SetBackend(nullptr);
    sw::ResetMatch();
    sw::SetBackend(nullptr);
}
