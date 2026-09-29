// CheckPath: path safety, the CRC-collision check and the size cap. Pure aside from the Win32 file-attribute stat,
// kept apart from banks_load.cpp (which calls engine::LoadBank) so an offline self-test can link this file alone.
#include "assets/banks.h"

#include <windows.h>

#include <cctype>
#include <cstring>

#include "xom/xom.h"

namespace melange::assets::banks {
namespace {
constexpr uint64_t kMaxBankBytes = 64ull << 20;

// Every field of XDataBank (src/xom/xom_schema.inc) that lists resource entries by name; each entry is a Ref to an
// XResourceDetails subclass (Name: String, Flags: U32, plus its own Value field), mirrored from tools/xom/xomtool.py.
constexpr const char* kResourceLists[] = {"IntResources",       "UintResources",       "StringResources",
                                           "FloatResources",     "VectorResources",     "ContainerResources",
                                           "StringTableResources", "ColorResources"};

// A small local UTF-8 -> UTF-16 helper (rather than core/game.h's Widen) so this pure-logic file has no dependency
// beyond the Win32 API; banks_load.cpp and assets.cpp already pull in the rest of the engine glue.
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool EndsWithI(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i]) return false;
    return true;
}

// No leading slash or drive letter, and no "." or ".." segment: rel can never point outside the folder it's
// joined to. Mirrors weapons/manifest.cpp's SafeRel (spice-time validation of the same field).
bool SafeRel(const std::string& p) {
    if (p.empty() || p.size() > 200 || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find_first_of("/\\", i);
        if (j == std::string::npos) j = p.size();
        const std::string seg = p.substr(i, j - i);
        if (seg.empty() || seg == "." || seg == "..") return false;
        i = j + 1;
    }
    return true;
}

std::wstring ToBackslash(std::wstring s) {
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    return s;
}
}  // namespace

bool BankResourceNames(const std::vector<uint8_t>& bytes, std::vector<std::string>* names, std::string* err) {
    xom::Document doc;
    if (!xom::parse(bytes.data(), bytes.size(), doc, err)) return false;
    const xom::Object* bank = nullptr;
    for (auto& o : doc.objects)
        if (o.type == "XDataBank" && !o.opaque && !o.inTail) { bank = &o; break; }
    if (!bank) {
        if (err) *err = "the file has no XDataBank";
        return false;
    }
    for (const char* list : kResourceLists) {
        const xom::Value* v = bank->field(list);
        if (!v) continue;
        for (size_t i = 0; i < v->size(); ++i) {
            const xom::Object* det = doc.object(v->at(i).asRef());
            const xom::Value* nm = det ? det->field("Name") : nullptr;
            if (nm && nm->type == xom::Type::String) names->push_back(nm->str);
        }
    }
    return true;
}

bool CheckPath(const std::wstring& absDataDir, const std::string& rel, const std::vector<crcsafe::Entry>& crcTable,
               std::string* err) {
    if (!SafeRel(rel) || !EndsWithI(rel, ".xom")) {
        if (err) *err = "must be a relative .xom path under assets/data/, with no '..'";
        return false;
    }
    if (crcsafe::Collides(crcTable, rel)) {
        if (err) *err = rel + " collides with a protected file";
        return false;
    }
    const std::wstring full = absDataDir + L"\\" + ToBackslash(Widen(rel));
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad) ||
        (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (err) *err = rel + " was not found";
        return false;
    }
    ULARGE_INTEGER sz;
    sz.HighPart = fad.nFileSizeHigh;
    sz.LowPart = fad.nFileSizeLow;
    if (sz.QuadPart > kMaxBankBytes) {
        if (err) *err = rel + " is larger than 64 MiB";
        return false;
    }
    return true;
}
}  // namespace melange::assets::banks
