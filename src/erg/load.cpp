#include "erg/load.h"

#include <unordered_set>

#include "erg/xomutil.h"
#include "tools/hash.h"

namespace melange::erg::load {
const char* const kDatabankKeys[5] = {"Databank.Theme", "Databank.TimeOfDay", "Databank.MaterialFile",
                                      "Heightmap.BaseTexture", "Heightmap.SecondTexture"};

namespace {
using xom::Type;
using xom::Value;

bool Fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}

bool ParseStrict(const std::vector<uint8_t>& bytes, size_t max, const char* what, xom::Document* doc, std::string* err) {
    if (bytes.size() > max) return Fail(err, std::string(what) + ": larger than " + std::to_string(max >> 20) + " MB");
    std::string perr;
    xom::ParseOptions opt;
    opt.strict = true;
    if (!xom::parse(bytes.data(), bytes.size(), *doc, &perr, opt)) return Fail(err, std::string(what) + ": " + perr);
    return true;
}

bool RefList(const xom::Object& o, const char* field, std::vector<uint32_t>* out) {
    out->clear();
    const Value* v = o.field(field);
    if (!v || v->type != Type::Ref || !v->array) return false;
    for (const auto& it : v->items) out->push_back(it.asRef());
    return true;
}

std::string Clip(std::string s, size_t n) {
    if (s.size() > n) s.resize(n);
    for (auto& c : s)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) c = '?';
    return s;
}

bool IsFolder(const Frame& f) {
    return f.size == std::array<int, 3>{1, 1, 1} && f.rot == Vec3{0, 0, 0} && f.scale == Vec3{1, 1, 1};
}
}  // namespace

std::string Sha256(const std::vector<uint8_t>& bytes) { return hashutil::Sha256Hex(bytes.data(), bytes.size()); }

bool LoadScene(const BaseFiles& in, Loaded* out, std::string* err) {
    Loaded L;
    if (!ParseStrict(in.xan, kMaxXanBytes, ".xan", &L.xan, err) || !ParseStrict(in.xom, kMaxXomBytes, "level .XOM", &L.xom, err))
        return false;
    if (in.hmp && in.hmp->size() != kHmpBytes) return Fail(err, ".hmp: " + std::to_string(in.hmp->size()) + " bytes, expected 50000");
    L.hmp = in.hmp;

    const xom::Document& x = L.xan;
    const size_t nobj = x.objects.size();
    const xom::Object* root = x.object(x.root);
    if (!root || root->type != "LandFrameStore") return Fail(err, ".xan: the root is not a LandFrameStore");
    size_t nFrames = 0, nDetails = 0;
    for (const auto& o : x.objects) {
        if (o.type == "LandFrameStore") ++nFrames;
        else if (o.type == "DetailEntityStore") ++nDetails;
        else return Fail(err, ".xan: unexpected object type " + o.type);
    }
    if (nFrames > kMaxFrames) return Fail(err, ".xan: more than 4096 frames");
    if (nDetails > kMaxDetails) return Fail(err, ".xan: more than 65536 details");

    Scene& s = L.scene;
    s.stem = in.file;
    s.title = PrintableAscii(in.title, 1, 40) ? in.title : Clip(in.key, 40);
    s.base.key = in.key;
    s.base.source = in.source;
    s.base.file = in.file;
    s.base.sha256 = {Sha256(in.xan), Sha256(in.xom), in.hmp ? Sha256(*in.hmp) : std::string()};
    s.registry = in.registry;
    s.hmp = in.hmp ? HmpMode::Copy : HmpMode::None;

    std::vector<std::string> db(5);
    for (const auto& o : L.xom.objects) {
        if (o.type != "XStringResourceDetails") continue;
        const std::string name = xomutil::Str(o, "Name");
        for (int k = 0; k < 5; ++k)
            if (name == kDatabankKeys[k]) db[k] = xomutil::Str(o, "Value");
    }
    s.databank = {db[0], db[1], db[2], db[3], db[4]};

    std::vector<char> seenFrame(nobj + 1, 0), seenDetail(nobj + 1, 0);
    std::vector<std::pair<uint32_t, int64_t>> stack{{x.root, -1}};
    std::vector<uint32_t> kids, dets;
    int64_t nextRef = 1;
    while (!stack.empty()) {
        const auto [idx, parent] = stack.back();
        stack.pop_back();
        if (idx == 0 || idx > nobj) return Fail(err, ".xan: a Children reference is out of range");
        const xom::Object& o = x.objects[idx - 1];
        if (o.type != "LandFrameStore") return Fail(err, ".xan: object #" + std::to_string(idx) + " in Children is not a frame");
        if (seenFrame[idx]++) return Fail(err, ".xan: frame #" + std::to_string(idx) + " is reachable twice");
        Frame f;
        f.id = idx;
        f.parent = parent;
        f.name = Clip(xomutil::Str(o, "Name"), 127);
        if (!xomutil::GetVec(o, "Position", &f.pos) || !xomutil::GetVec(o, "Orientation", &f.rot) ||
            !xomutil::GetVec(o, "Scale", &f.scale))
            return Fail(err, ".xan: frame #" + std::to_string(idx) + " lacks its transform");
        f.size = {static_cast<int>(xomutil::Int(o, "XSize", -1)), static_cast<int>(xomutil::Int(o, "YSize", -1)),
                  static_cast<int>(xomutil::Int(o, "ZSize", -1))};
        for (int v : f.size)
            if (v < 0 || v > 255) return Fail(err, ".xan: frame #" + std::to_string(idx) + " has a bad size");
        const uint64_t cells = static_cast<uint64_t>(f.size[0]) * f.size[1] * f.size[2];
        if (cells > kMaxFrameVoxels) return Fail(err, ".xan: frame #" + std::to_string(idx) + " has more than 262144 voxels");
        const Value* vox = o.field("Voxels");
        const Value* hm = o.field("HeightMap");
        if (!vox || vox->type != Type::U32 || !vox->array || !hm || hm->type != Type::F32 || !hm->array)
            return Fail(err, ".xan: frame #" + std::to_string(idx) + " lacks Voxels or HeightMap");
        const uint64_t corners = cells ? static_cast<uint64_t>(f.size[0] + 1) * (f.size[2] + 1) : 0;
        if (vox->size() != 0 && vox->size() != cells)
            return Fail(err, ".xan: frame #" + std::to_string(idx) + ": Voxels holds " + std::to_string(vox->size()) + " of " +
                                 std::to_string(cells) + " voxels");
        if (hm->size() != 0 && hm->size() != corners)
            return Fail(err, ".xan: frame #" + std::to_string(idx) + ": HeightMap holds " + std::to_string(hm->size()) + " of " +
                                 std::to_string(corners) + " corners");
        if (cells && vox->size() == cells) {
            f.voxels = nextRef++;
            s.blobs.push_back({f.voxels, "voxels", f.id, cells * 4});
            L.blobs[f.voxels] = vox->raw;
        }
        if (corners && hm->size() == corners) {
            f.heightMap = nextRef++;
            s.blobs.push_back({f.heightMap, "heightMap", f.id, corners * 4});
            L.blobs[f.heightMap] = hm->raw;
        }
        f.folder = IsFolder(f);
        if (!RefList(o, "Details", &dets) || !RefList(o, "Children", &kids))
            return Fail(err, ".xan: frame #" + std::to_string(idx) + " lacks Details or Children");
        for (uint32_t d : dets) {
            if (d == 0 || d > nobj || x.objects[d - 1].type != "DetailEntityStore")
                return Fail(err, ".xan: frame #" + std::to_string(idx) + " lists a detail that is not one");
            if (seenDetail[d]++) return Fail(err, ".xan: detail #" + std::to_string(d) + " belongs to two frames");
            const xom::Object& dobj = x.objects[d - 1];
            Detail det;
            det.id = d;
            det.src = d;
            det.frame = idx;
            det.name = Clip(xomutil::Str(dobj, "Name"), 127);
            det.resource = Clip(xomutil::Str(dobj, "ResourceName"), 127);
            if (!xomutil::GetVec(dobj, "Position", &det.pos) || !xomutil::GetVec(dobj, "Orientation", &det.rot) ||
                !xomutil::GetVec(dobj, "Scale", &det.scale) || !xomutil::GetVec(dobj, "VoxelPos", &det.voxelPos))
                return Fail(err, ".xan: detail #" + std::to_string(d) + " lacks its transform");
            det.role = DeriveRole(det.name, det.resource);
            s.details.push_back(std::move(det));
        }
        s.frames.push_back(std::move(f));
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back({*it, static_cast<int64_t>(idx)});
        if (s.frames.size() > kMaxFrames) return Fail(err, ".xan: more than 4096 frames");
    }
    std::string why;
    if (!ValidateScene(s, &why)) return Fail(err, "the base does not form a valid scene: " + why);
    *out = std::move(L);
    return true;
}
}  // namespace melange::erg::load
