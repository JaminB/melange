// Offline self-test for the content-identity and lobby-handshake logic (E). No game, no Steam, no filesystem.
// Exit code 0 = all passed.
#include <cstdio>
#include <string>

#include "mods/handshake_internal.h"

using namespace melange::handshake;
using melange::mods::ContentId;
using melange::mods::PeerStatus;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what);
    }
}

ContentMod Mod(std::string id, std::string version, std::vector<ContentFile> files) {
    return ContentMod{std::move(id), std::move(version), std::move(files)};
}
ContentFile File(std::string rel, std::string hash) { return ContentFile{std::move(rel), std::move(hash)}; }

bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

CloneSpec Mega() {
    CloneSpec c;
    c.k = 0;
    c.vid = 0x100;
    c.name = "kWeaponMegaBazooka";
    c.base = "kWeaponBazooka";
    c.mod = "mega-bazooka";
    c.cell = 29;
    c.set = {{"WormDamageMagnitude", SetNumber(120)}, {"PayloadGraphicsResourceID", SetString("Cow.Payload")},
             {"LandDamageRadius", SetNumber(90)}};
    return c;
}

// The M4 text for the same mods and message count, as the v1 builder wrote it.
std::string V1Text(const std::vector<ContentMod>& mods, size_t messages) {
    std::string t = "melange-content/1\nsim=1\n";
    for (const auto& m : mods) {
        t += "mod=" + m.id + "@" + m.version + "\n";
        for (const auto& f : m.files) t += "file=" + f.relPath + " " + f.sha256Hex + "\n";
    }
    return t + "messages=" + std::to_string(messages) + "\n";
}

uint16_t g_fakeId = 0;
uint16_t FakeLookup(const char* name) { return std::string(name) == "Mod.Test.20" ? g_fakeId : 0x4d4; }

void ContentTextTests(const std::vector<ContentMod>& a) {
    {
        const std::string t = CanonicalText(a, {}, {});
        Expect(t.rfind("melange-content/2\n", 0) == 0, "v2: first line");
        std::string v1 = V1Text(a, 0);
        Expect(t.substr(t.find('\n')) == v1.substr(v1.find('\n')), "v2 without messages or clones: only the first line differs from v1");
    }
    {
        const std::vector<ModMessage> msgs = {{"Mod.Test.20", 0x4d3}, {"Mod.Test.1104", 0x4d4}};
        const std::string t = CanonicalText(a, msgs, {});
        std::string v1 = V1Text(a, 2);
        const std::string pairs = "message=Mod.Test.20=1235\nmessage=Mod.Test.1104=1236\n";
        Expect(Contains(t, pairs), "v2: messages as name=id in registration order");
        std::string stripped = t;
        stripped.erase(stripped.find(pairs), pairs.size());
        Expect(stripped.substr(stripped.find('\n')) == v1.substr(v1.find('\n')),
               "v2 without clones: differs from v1 only in the first line and the name=id pairs");
    }
    {
        // The colliding pair registered in opposite orders on two peers: the ids swap, so the hashes differ.
        const std::vector<ModMessage> peerA = {{"Mod.Test.20", 0x4d3}, {"Mod.Test.1104", 0x4d4}};
        const std::vector<ModMessage> peerB = {{"Mod.Test.1104", 0x4d3}, {"Mod.Test.20", 0x4d4}};
        const ContentId ca = BuildContentId(a, peerA, {}), cb = BuildContentId(a, peerB, {});
        Expect(std::string(ca.hash) != std::string(cb.hash), "message order: opposite registration orders hash differently");
        Expect(ca.modMessages == 2, "message count kept in the content id");
        const std::string d = DiffMsgValues(BuildMsgValue(peerA), BuildMsgValue(peerB));
        Expect(Contains(d, "message ids differ") && Contains(d, "Mod.Test.20 1235 vs 1236") &&
                   Contains(d, "Mod.Test.1104 1236 vs 1235"),
               "message diff names both messages of the pair");
        Expect(DiffMsgValues(BuildMsgValue(peerA), BuildMsgValue(peerA)).empty(), "message diff: same pairs");
        Expect(Contains(DiffMsgValues("A.b=1,C.d=2", "C.d=2,A.b=1"), "message order differs"), "message diff: order only");
        Expect(Contains(DiffMsgValues("A.b=1", ""), "missing A.b"), "message diff: missing");
        Expect(Contains(DiffMsgValues("", "A.b=1"), "extra A.b"), "message diff: extra");
        Expect(BuildMsgValue(peerA) == "Mod.Test.20=1235,Mod.Test.1104=1236", "mlg.msg value");
        Expect(BuildMsgValue({}).empty(), "mlg.msg is empty without mod messages");
        std::vector<ModMessage> many;
        for (int i = 0; i < 48; ++i) many.push_back({"Mod.Long.Name" + std::to_string(i) + std::string(40, 'x'), static_cast<uint16_t>(1300 + i)});
        Expect(BuildMsgValue(many).size() <= kModsValueMaxBytes, "mlg.msg respects the byte cap");
    }
    {
        const std::vector<ModMessage> hashed = {{"Mod.Test.20", 0x4d3}, {"Mod.Test.1104", 0x4d4}};
        g_fakeId = 0x4d3;
        Expect(ChangedMessageIds(hashed, &FakeLookup).empty(), "Init id check: unchanged ids pass");
        g_fakeId = 0x4d5;
        const auto changed = ChangedMessageIds(hashed, &FakeLookup);
        Expect(changed.size() == 1 && changed[0] == "Mod.Test.20", "Init id check: a moved id is named");
        g_fakeId = 0xffff;
        Expect(ChangedMessageIds(hashed, &FakeLookup).size() == 1, "Init id check: an unregistered name is named");
        Expect(ChangedMessageIds({}, &FakeLookup).empty(), "Init id check: nothing hashed, nothing to check");
    }
}

void CloneTests(const std::vector<ContentMod>& a) {
    Expect(SetNumber(120) == "120" && SetNumber(123.75) == "123.75" && SetNumber(-2) == "-2", "set numbers: short forms");
    Expect(SetNumber(0.1) == "0.10000000000000001", "set numbers: round-trip precision");
    Expect(SetBool(true) == "true" && SetBool(false) == "false", "set booleans");
    Expect(SetString("weapons/SheepBaa") == "\"weapons/SheepBaa\"", "set strings: plain");
    Expect(SetString("a b=c\"%") == "\"a%20b%3Dc%22%25\"", "set strings: escaped");

    const CloneSpec mega = Mega();
    Expect(CloneLine(mega) == "clone 0 0x100 kWeaponMegaBazooka kWeaponBazooka 29 - LandDamageRadius=90 "
                              "PayloadGraphicsResourceID=\"Cow.Payload\" WormDamageMagnitude=120",
           "clone line: fields and sorted set");
    {
        CloneSpec bare = mega;
        bare.set.clear();
        bare.bankSha256 = "ab12";
        Expect(CloneLine(bare) == "clone 0 0x100 kWeaponMegaBazooka kWeaponBazooka 29 ab12 -", "clone line: bank and empty set");
    }
    const std::vector<ModMessage> none;
    const ContentId base = BuildContentId(a, none, {mega});
    {
        const std::string t = CanonicalText(a, none, {mega});
        Expect(Contains(t, "\n" + CloneLine(mega) + "\n"), "v2: the clone line is in the text");
        Expect(std::string(base.hash) != std::string(BuildContentId(a, none, {}).hash), "a clone changes the hash");
    }
    {
        CloneSpec shuffled = mega;
        std::swap(shuffled.set[0], shuffled.set[2]);
        Expect(std::string(BuildContentId(a, none, {shuffled}).hash) == base.hash, "set order in the manifest does not matter");
        Expect(CloneHash16({shuffled}) == CloneHash16({mega}), "clone hash: set order does not matter");
    }
    auto differs = [&](CloneSpec c, const char* what) {
        Expect(std::string(BuildContentId(a, none, {c}).hash) != base.hash, what);
        Expect(CloneHash16({c}) != CloneHash16({mega}), what);
    };
    {
        CloneSpec c = mega;
        c.set[0].second = SetNumber(121);
        differs(c, "a set value changes the hashes");
    }
    {
        CloneSpec c = mega;
        c.cell = 39;
        differs(c, "the cell changes the hashes");
    }
    {
        CloneSpec c = mega;
        c.bankSha256 = "00ff";
        differs(c, "the bank's bytes change the hashes");
    }
    {
        CloneSpec c = mega;
        c.base = "kWeaponGrenade";
        differs(c, "the base changes the hashes");
    }
    {
        CloneSpec second = mega;
        second.k = 1;
        second.vid = 0x101;
        second.name = "kWeaponMegaGrenade";
        second.cell = 39;
        Expect(CloneHash16({mega, second}) != CloneHash16({second, mega}), "clone hash: k order matters");
    }
    {
        // ExtraPerExplosion is a per-machine ini setting, not a mod file, but it changes clone sim behaviour, so a
        // peer with a different value must hash and gate differently once clones are declared.
        Expect(CloneHash16({mega}, 8) != CloneHash16({mega}, 2), "clone hash: local ExtraPerExplosion is covered");
        Expect(CloneHash16({}, 8) == CloneHash16({}, 2), "no clones: ExtraPerExplosion is irrelevant");
        Expect(std::string(BuildContentId(a, none, {mega}, 8).hash) != BuildContentId(a, none, {mega}, 2).hash,
               "content hash: local ExtraPerExplosion is covered once clones are declared");
        Expect(std::string(BuildContentId(a, none, {}, 8).hash) == BuildContentId(a, none, {}, 2).hash,
               "content hash without clones: ExtraPerExplosion does not affect unrelated mods");
    }

    const std::string h = CloneHash16({mega});
    Expect(h.size() == 16, "clone hash16 is 16 characters");
    Expect(CloneHash16({}).empty(), "no clones: no clone hash");
    Expect(BuildWpnValue({mega}) == "1;" + h + ";1", "mlg.wpn value");
    Expect(BuildWpnValue({}).empty(), "mlg.wpn is never written without clones");
    Expect(BuildWpnValue({mega}).size() <= 32, "mlg.wpn fits its 32-byte budget");
    {
        std::string hash;
        uint32_t n = 0;
        Expect(ParseWpnValue(BuildWpnValue({mega}), &hash, &n) && hash == h && n == 1, "mlg.wpn round-trips");
        Expect(!ParseWpnValue("2;" + h + ";1", nullptr, nullptr), "mlg.wpn: another protocol version is refused");
        Expect(!ParseWpnValue("1;xyz;1", nullptr, nullptr), "mlg.wpn: a bad hash is refused");
        Expect(!ParseWpnValue("1;" + h + ";", nullptr, nullptr), "mlg.wpn: a missing count is refused");
        Expect(!ParseWpnValue("", nullptr, nullptr), "mlg.wpn: empty is refused");
    }
    const std::string ours = Hash16(base);
    Expect(BuildReqValue(ours, true) == "wpn1;" + ours, "mlg.req value");
    Expect(BuildReqValue(ours, true).size() <= 24, "mlg.req fits its 24-byte budget");
    Expect(BuildReqValue(ours, false).empty(), "mlg.req is removed without clones");
    Expect(BuildReqValue("v", true).empty(), "mlg.req is never written for vanilla content");
    {
        std::string hash;
        Expect(ParseReqValue(BuildReqValue(ours, true), &hash) && hash == ours, "mlg.req round-trips");
        Expect(!ParseReqValue("wpn2;" + ours, nullptr), "mlg.req: another protocol version is refused");
        Expect(!ParseReqValue("wpn1;", nullptr), "mlg.req: an empty hash is refused");
    }
}

LobbyMember Member(std::string name, bool hasMlg, std::string hash, std::string diff = "") {
    LobbyMember m;
    m.name = std::move(name);
    m.hasMlg = hasMlg;
    m.hash16 = std::move(hash);
    m.diff = std::move(diff);
    return m;
}

void PolicyTests() {
    const std::string ours = "0123456789abcdef", other = "fedcba9876543210";
    {
        CloneLobbyInput in;
        in.haveClones = true;
        Expect(EvaluateCloneLobby(in).ok, "policy: offline is always ok");
    }
    CloneLobbyInput host;
    host.inLobby = true;
    host.weAreOwner = true;
    host.haveClones = true;
    host.ourHash16 = ours;
    host.lobbyReq = BuildReqValue(ours, true);
    {
        CloneLobbyInput in = host;
        in.members = {Member("Bob", true, ours), Member("Cy", true, ours)};
        const CloneVerdict v = EvaluateCloneLobby(in);
        Expect(v.ok && !v.hostHeld && v.members.empty(), "host with clones, every member matches: ok");
        in.members.clear();
        Expect(EvaluateCloneLobby(in).ok, "host alone: ok");
    }
    {
        CloneLobbyInput in = host;
        in.members = {Member("Bob", false, "")};
        const CloneVerdict v = EvaluateCloneLobby(in);
        Expect(!v.ok && v.hostHeld && !v.joinerMismatch, "host with clones, a vanilla member: held");
        Expect(v.members.size() == 1 && v.members[0] == "Bob: no Melange", "held: the banner names the vanilla member");
        Expect(Contains(v.why, "Bob"), "held: the reason names the member");
    }
    {
        CloneLobbyInput in = host;
        in.members = {Member("Bob", true, ours), Member("Cy", true, other, "mega-bazooka 1.0.0 vs 1.1.0")};
        const CloneVerdict v = EvaluateCloneLobby(in);
        Expect(!v.ok && v.hostHeld && v.members.size() == 1 && v.members[0] == "Cy: mega-bazooka 1.0.0 vs 1.1.0",
               "host with clones, a mismatched member: held, with what differs");
    }
    {
        CloneLobbyInput in = host;
        in.members = {Member("Dee", true, "v"), Member("Eve", true, other)};
        const CloneVerdict v = EvaluateCloneLobby(in);
        Expect(v.members.size() == 2 && v.members[0] == "Dee: no content mods" && v.members[1] == "Eve: different mod files",
               "held: Melange without content, and a mismatch with no known diff");
    }
    {
        CloneLobbyInput in = host;
        in.haveClones = false;
        in.lobbyReq.clear();
        in.members = {Member("Bob", false, ""), Member("Cy", true, other)};
        const CloneVerdict v = EvaluateCloneLobby(in);
        Expect(v.ok && !v.hostHeld, "host without clones: nothing is refused");
    }
    CloneLobbyInput joiner;
    joiner.inLobby = true;
    joiner.ourHash16 = ours;
    {
        CloneLobbyInput in = joiner;
        in.haveClones = true;
        in.lobbyReq = BuildReqValue(ours, true);
        Expect(EvaluateCloneLobby(in).ok, "joiner matching the clone lobby: ok");
    }
    {
        CloneLobbyInput in = joiner;
        in.haveClones = true;
        in.lobbyReq = BuildReqValue(other, true);
        in.hostMods = "mega-bazooka@1.1.0";
        in.diffToHost = "mega-bazooka 1.0.0 vs 1.1.0";
        const CloneVerdict v = EvaluateCloneLobby(in);
        Expect(!v.ok && v.joinerMismatch && !v.hostHeld, "joiner with other clones: the modal, never a refusal");
        Expect(v.why == "This lobby uses clone weapons from: mega-bazooka@1.1.0. You have: mega-bazooka 1.0.0 vs 1.1.0.",
               "modal text names the host's mods and the diff");
    }
    {
        CloneLobbyInput in = joiner;
        in.lobbyReq = BuildReqValue(other, true);
        in.hostMods = "mega-bazooka@1.1.0";
        const CloneVerdict v = EvaluateCloneLobby(in);
        Expect(v.joinerMismatch && Contains(v.why, "different mod files"), "joiner without mods in a clone lobby: the modal");
    }
    {
        CloneLobbyInput in = joiner;
        in.lobbyReq = "wpn2;" + other;
        const CloneVerdict v = EvaluateCloneLobby(in);
        Expect(!v.ok && v.joinerMismatch && Contains(v.why, "newer Melange"), "joiner in a lobby with a newer clone protocol");
    }
    {
        CloneLobbyInput in = joiner;
        in.haveClones = true;
        const CloneVerdict v = EvaluateCloneLobby(in);
        Expect(!v.ok && !v.joinerMismatch && !v.hostHeld, "joiner with clones, host without: clones off, no modal");
    }
    {
        CloneLobbyInput in = joiner;
        Expect(EvaluateCloneLobby(in).ok, "joiner and host both without clones: ok");
    }
}

void VanillaTests() {
    const ContentId v = BuildContentId({}, {}, {});
    Expect(v.vanilla && BuildMlgValue("0.5.0", v) == "1;0.5.0;v;0", "vanilla: the mlg value is unchanged");
    Expect(BuildReqValue(Hash16(v), false).empty() && BuildWpnValue({}).empty() && BuildMsgValue({}).empty(),
           "vanilla: no clone or message keys");
}

void WeaponTests(const std::vector<ContentMod>& a) {
    ContentTextTests(a);
    CloneTests(a);
    PolicyTests();
    VanillaTests();
}
}  // namespace

int main() {
    {
        ContentId c = BuildContentId({}, {}, {});
        Expect(c.vanilla, "no mods, no messages: vanilla");
        Expect(c.contentMods == 0 && c.modMessages == 0, "vanilla: counts are zero");
        Expect(c.hash[0] == '\0', "vanilla: hash is empty");
        Expect(Hash16(c) == "v", "vanilla: hash16 is \"v\"");
    }
    {
        ContentId c = BuildContentId({}, {{"Mod.A.b", 1300}}, {});
        Expect(!c.vanilla, "messages with no mods is still non-vanilla");
        Expect(c.hash[0] != '\0', "messages with no mods: hash is non-empty");
    }

    std::vector<ContentMod> a = {Mod("alpha", "1.0.0", {File("assets/a.png", "aa"), File("spice.json", "11")}),
                                 Mod("bravo", "2.1.0", {File("spice.json", "22"), File("sim/init.lua", "bb")})};
    // Same mods, files given in a different order within each mod: the hash must not depend on directory-walk order.
    std::vector<ContentMod> aShuffled = {Mod("alpha", "1.0.0", {File("spice.json", "11"), File("assets/a.png", "aa")}),
                                         Mod("bravo", "2.1.0", {File("sim/init.lua", "bb"), File("spice.json", "22")})};
    ContentId ca = BuildContentId(a, {}, {});
    ContentId caShuffled = BuildContentId(aShuffled, {}, {});
    Expect(std::string(ca.hash) == std::string(caShuffled.hash), "file order within a mod does not affect the hash");
    Expect(ca.contentMods == 2, "content mod count");

    {
        ContentId peerB = BuildContentId(a, {}, {});
        Expect(std::string(ca.hash) == std::string(peerB.hash), "identical fixtures give an identical hash");
    }
    {
        std::vector<ContentMod> touched = a;
        touched[0].files[0].sha256Hex = "changed";
        ContentId c = BuildContentId(touched, {}, {});
        Expect(std::string(c.hash) != std::string(ca.hash), "touching an asset file changes the hash");
    }
    {
        std::vector<ContentMod> swapped = {a[1], a[0]};
        ContentId c = BuildContentId(swapped, {}, {});
        Expect(std::string(c.hash) != std::string(ca.hash), "mod load order changes the hash");
    }
    {
        ContentId c1 = BuildContentId(a, {}, {});
        ContentId c2 = BuildContentId(a, {{"Mod.A.b", 1300}}, {});
        Expect(std::string(c1.hash) != std::string(c2.hash), "a registered mod message changes the hash");
    }
    Expect(Hash16(ca) == std::string(ca.hash).substr(0, 16), "hash16 is the first 16 hex characters");
    Expect(Hash16(ca).size() == 16, "hash16 is 16 characters");

    {
        std::string v = BuildMlgValue("0.2.0", ca);
        std::string version, hash16;
        uint32_t contentMods = 0;
        Expect(ParseMlgValue(v, &version, &hash16, &contentMods), "mlg value round-trips");
        Expect(version == "0.2.0", "mlg round-trip: version");
        Expect(hash16 == Hash16(ca), "mlg round-trip: hash16");
        Expect(contentMods == 2, "mlg round-trip: content mod count");
    }
    {
        ContentId vanilla{};
        vanilla.vanilla = true;
        std::string v = BuildMlgValue("0.2.0", vanilla);
        Expect(v == "1;0.2.0;v;0", "mlg value for a vanilla peer");
    }
    Expect(!ParseMlgValue("2;0.2.0;abcd;1", nullptr, nullptr, nullptr), "mlg: wrong protocol version rejected");
    Expect(!ParseMlgValue("1;0.2.0;abcd", nullptr, nullptr, nullptr), "mlg: wrong field count rejected");
    {
        uint32_t n = 99;
        Expect(ParseMlgValue("1;0.2.0;v;notanumber", nullptr, nullptr, &n), "mlg: non-numeric count still parses (protocol ok)");
        Expect(n == 0, "mlg: non-numeric count defaults to 0");
    }

    {
        std::vector<ContentMod> few = {Mod("a", "1.0.0", {}), Mod("b", "2.0.0", {})};
        Expect(BuildModsValue(few) == "a@1.0.0,b@2.0.0", "mods value: small list is not truncated");
    }
    {
        std::vector<ContentMod> many;
        for (int i = 0; i < 400; ++i) many.push_back(Mod("mod-" + std::to_string(i), "1.0.0", {}));
        std::string v = BuildModsValue(many, 200);
        Expect(v.size() <= 200, "mods value: truncated result respects the byte cap");
        Expect(v.size() >= 3 && v.substr(v.size() - 3) == "\xE2\x80\xA6", "mods value: truncated result ends with an ellipsis");
        Expect(v.find(",,") == std::string::npos, "mods value: truncation lands on an entry boundary");
    }

    Expect(DiffModsValues("a@1.0.0,b@2.0.0", "b@2.0.0,a@1.0.0").empty(), "mods diff: same set in another order");
    Expect(DiffModsValues("a@1.0.0,b@2.0.0", "a@1.0.1,c@3.0.0") == "missing b@2.0.0, extra c@3.0.0, a 1.0.0 vs 1.0.1",
           "mods diff: missing, extra and another version");
    Expect(DiffModsValues("a@1.0.0", "a@1.0.0,\xE2\x80\xA6").empty(), "mods diff: a truncated list's ellipsis is ignored");

    Expect(ClassifyPeer(false, "", "abcd") == PeerStatus::Vanilla, "classify: no mlg key is Vanilla");
    Expect(ClassifyPeer(true, "v", "abcd") == PeerStatus::MelangeVanilla, "classify: peer hash16 v is MelangeVanilla");
    Expect(ClassifyPeer(true, "abcd", "abcd") == PeerStatus::Match, "classify: equal hash16 is Match");
    Expect(ClassifyPeer(true, "abcd", "efgh") == PeerStatus::Mismatch, "classify: different hash16 is Mismatch");
    Expect(ClassifyPeer(true, "abcd", "v") == PeerStatus::Mismatch, "classify: we are vanilla, they are not: Mismatch");

    Expect(BuildMlgSim("abcd", true, {"abcd", "abcd"}).empty(), "mlg.sim: a vanilla host writes nothing");
    Expect(BuildMlgSim("abcd", false, {}) == "abcd", "mlg.sim: solo lobby matches trivially");
    Expect(BuildMlgSim("abcd", false, {"abcd", "abcd"}) == "abcd", "mlg.sim: every member matches");
    Expect(BuildMlgSim("abcd", false, {"abcd", "efgh"}) == "off", "mlg.sim: one member differs");

    Expect(GateAllowsSim(false, "v", "off"), "gate: offline is always allowed, even with no content");
    Expect(GateAllowsSim(true, "abcd", "abcd"), "gate: online, hash matches the lobby");
    Expect(!GateAllowsSim(true, "abcd", "off"), "gate: online, lobby suspended");
    Expect(!GateAllowsSim(true, "abcd", "efgh"), "gate: online, lobby matches someone else");
    Expect(!GateAllowsSim(true, "v", "v"), "gate: online, we are vanilla ourselves");

    WeaponTests(a);

    printf("handshake_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
