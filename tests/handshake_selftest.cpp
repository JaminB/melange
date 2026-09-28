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
}  // namespace

int main() {
    {
        ContentId c = BuildContentId({}, 0);
        Expect(c.vanilla, "no mods, no messages: vanilla");
        Expect(c.contentMods == 0 && c.modMessages == 0, "vanilla: counts are zero");
        Expect(c.hash[0] == '\0', "vanilla: hash is empty");
        Expect(Hash16(c) == "v", "vanilla: hash16 is \"v\"");
    }
    {
        ContentId c = BuildContentId({}, 3);
        Expect(!c.vanilla, "messages with no mods is still non-vanilla");
        Expect(c.hash[0] != '\0', "messages with no mods: hash is non-empty");
    }

    std::vector<ContentMod> a = {Mod("alpha", "1.0.0", {File("assets/a.png", "aa"), File("spice.json", "11")}),
                                 Mod("bravo", "2.1.0", {File("spice.json", "22"), File("sim/init.lua", "bb")})};
    // Same mods, files given in a different order within each mod: the hash must not depend on directory-walk order.
    std::vector<ContentMod> aShuffled = {Mod("alpha", "1.0.0", {File("spice.json", "11"), File("assets/a.png", "aa")}),
                                         Mod("bravo", "2.1.0", {File("sim/init.lua", "bb"), File("spice.json", "22")})};
    ContentId ca = BuildContentId(a, 0);
    ContentId caShuffled = BuildContentId(aShuffled, 0);
    Expect(std::string(ca.hash) == std::string(caShuffled.hash), "file order within a mod does not affect the hash");
    Expect(ca.contentMods == 2, "content mod count");

    {
        // Same fixtures on two "peers": identical input gives an identical hash.
        ContentId peerB = BuildContentId(a, 0);
        Expect(std::string(ca.hash) == std::string(peerB.hash), "identical fixtures give an identical hash");
    }
    {
        // Touching an asset file (its hash changes) changes the content hash.
        std::vector<ContentMod> touched = a;
        touched[0].files[0].sha256Hex = "changed";
        ContentId c = BuildContentId(touched, 0);
        Expect(std::string(c.hash) != std::string(ca.hash), "touching an asset file changes the hash");
    }
    {
        // Mod load order matters: swapping two mods' order changes the canonical text and so the hash.
        std::vector<ContentMod> swapped = {a[1], a[0]};
        ContentId c = BuildContentId(swapped, 0);
        Expect(std::string(c.hash) != std::string(ca.hash), "mod load order changes the hash");
    }
    {
        ContentId c1 = BuildContentId(a, 0);
        ContentId c2 = BuildContentId(a, 1);
        Expect(std::string(c1.hash) != std::string(c2.hash), "a different registered-message count changes the hash");
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

    printf("handshake_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
