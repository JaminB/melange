#include "launcher/setup/dll_id.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "launcher/util.h"
#include "tools/hash.h"
#include "tools/json_mini.h"

namespace melange::launcher::setup {
namespace {
struct Strings {
    std::string product, company, description, fileVersion, productVersion, internalName, originalName;
};

bool ReadVersionStrings(const std::wstring& path, Strings* s) {
    DWORD dummy = 0;
    const DWORD n = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &dummy);
    if (!n) return false;
    std::vector<char> buf(n);
    if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), 0, n, buf.data())) return false;
    std::vector<std::wstring> langs;
    struct Lang { WORD lang, cp; };
    Lang* tr = nullptr;
    UINT trLen = 0;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &trLen) && tr)
        for (UINT i = 0; i < trLen / sizeof(Lang); ++i) {
            wchar_t b[16];
            swprintf(b, 16, L"%04x%04x", tr[i].lang, tr[i].cp);
            langs.push_back(b);
        }
    langs.push_back(L"040904b0");
    langs.push_back(L"040904e4");
    langs.push_back(L"000004b0");
    auto get = [&](const wchar_t* name) {
        for (const auto& l : langs) {
            wchar_t* v = nullptr;
            UINT len = 0;
            const std::wstring q = L"\\StringFileInfo\\" + l + L"\\" + name;
            if (VerQueryValueW(buf.data(), q.c_str(), reinterpret_cast<void**>(&v), &len) && v && len) {
                std::wstring w(v, wcsnlen(v, len));
                while (!w.empty() && (w.back() == L' ')) w.pop_back();
                if (!w.empty()) return Narrow(w);
            }
        }
        return std::string();
    };
    s->product = get(L"ProductName");
    s->company = get(L"CompanyName");
    s->description = get(L"FileDescription");
    s->fileVersion = get(L"FileVersion");
    s->productVersion = get(L"ProductVersion");
    s->internalName = get(L"InternalName");
    s->originalName = get(L"OriginalFilename");
    return true;
}

bool NamesUal(const std::string& s) { return IContains(s, "Ultimate ASI Loader") || IContains(s, "Ultimate-ASI-Loader"); }

template <class T>
bool At(const std::string& f, size_t off, T* out) {
    if (off > f.size() || f.size() - off < sizeof(T)) return false;
    memcpy(out, f.data() + off, sizeof(T));
    return true;
}
}  // namespace

const char* DllKindName(DllKind k) {
    switch (k) {
        case DllKind::Ual: return "ual";
        case DllKind::Microsoft: return "microsoft";
        case DllKind::ReShade: return "reshade";
        case DllKind::SpecialK: return "specialk";
        case DllKind::Other: return "other";
    }
    return "other";
}

const std::vector<std::string>& KnownUalHashes() {
    static const std::vector<std::string> h = {
        "ec2f4824eca58dd40f425756a4a7cec77b8e381f21d11d7c846ec4b339b617ab",   // v9.7.4 Win32, shipped with Melange
    };
    return h;
}

std::vector<std::string> Exports(const std::wstring& path) {
    std::vector<std::string> out;
    std::string f;
    if (!ReadAll(path, &f, 64u << 20)) return out;
    IMAGE_DOS_HEADER dos{};
    if (!At(f, 0, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return out;
    const size_t nt = static_cast<size_t>(dos.e_lfanew);
    DWORD sig = 0;
    IMAGE_FILE_HEADER fh{};
    if (!At(f, nt, &sig) || sig != IMAGE_NT_SIGNATURE || !At(f, nt + 4, &fh)) return out;
    const size_t opt = nt + 4 + sizeof(IMAGE_FILE_HEADER);
    WORD magic = 0;
    if (!At(f, opt, &magic)) return out;
    IMAGE_DATA_DIRECTORY dir{};
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 oh{};
        if (!At(f, opt, &oh) || oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return out;
        dir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 oh{};
        if (!At(f, opt, &oh) || oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return out;
        dir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else {
        return out;
    }
    if (!dir.VirtualAddress) return out;
    const size_t secAt = opt + fh.SizeOfOptionalHeader;
    auto toOff = [&](DWORD rva, size_t* off) {
        for (WORD i = 0; i < fh.NumberOfSections && i < 96; ++i) {
            IMAGE_SECTION_HEADER s{};
            if (!At(f, secAt + i * sizeof s, &s)) return false;
            const DWORD span = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + span) {
                *off = static_cast<size_t>(rva - s.VirtualAddress) + s.PointerToRawData;
                return *off < f.size();
            }
        }
        return false;
    };
    size_t edOff = 0;
    IMAGE_EXPORT_DIRECTORY ed{};
    if (!toOff(dir.VirtualAddress, &edOff) || !At(f, edOff, &ed)) return out;
    size_t namesOff = 0;
    if (!ed.NumberOfNames || ed.NumberOfNames > 65536 || !toOff(ed.AddressOfNames, &namesOff)) return out;
    for (DWORD i = 0; i < ed.NumberOfNames; ++i) {
        DWORD rva = 0;
        size_t off = 0;
        if (!At(f, namesOff + i * 4, &rva) || !toOff(rva, &off)) break;
        const size_t end = f.find('\0', off);
        if (end == std::string::npos || end - off > 512) break;
        out.push_back(f.substr(off, end - off));
    }
    return out;
}

bool IdentifyDll(const std::wstring& path, const std::string& oursSha256, DllInfo* out) {
    *out = DllInfo{};
    out->path = path;
    out->file = Narrow(FileName(path));
    if (!FileExists(path)) return false;
    out->size = FileSize(path);
    out->sha256 = Sha256Cached(path);
    Strings s;
    const bool hasVer = ReadVersionStrings(path, &s);
    out->product = s.product;
    out->company = s.company;
    out->description = s.description;
    out->version = !s.fileVersion.empty() ? s.fileVersion : s.productVersion;
    out->internalName = s.internalName;
    out->originalName = s.originalName;
    const auto exports = Exports(path);
    out->exportsDinput = std::find(exports.begin(), exports.end(), "DirectInput8Create") != exports.end();
    out->ours = !oursSha256.empty() && out->sha256 == oursSha256;

    const auto& known = KnownUalHashes();
    if (out->ours || std::find(known.begin(), known.end(), out->sha256) != known.end()) {
        out->kind = DllKind::Ual;
        out->known = true;
    } else if (hasVer && (NamesUal(s.product) || NamesUal(s.description) || NamesUal(s.internalName) || NamesUal(s.originalName))) {
        out->kind = DllKind::Ual;
        out->known = true;
    } else if (hasVer && IContains(s.product, "ReShade")) {
        out->kind = DllKind::ReShade;
        out->known = true;
    } else if (hasVer && (IContains(s.product, "Special K") || IContains(s.description, "Special K"))) {
        out->kind = DllKind::SpecialK;
        out->known = true;
    } else if (hasVer && IEquals(s.company, "Microsoft Corporation")) {
        out->kind = DllKind::Microsoft;
        out->known = true;
    } else {
        std::string bytes;
        if (out->size <= (64u << 20) && ReadAll(path, &bytes, 64u << 20) &&
            (bytes.find("Ultimate ASI Loader") != std::string::npos || bytes.find("ThirteenAG/Ultimate-ASI-Loader") != std::string::npos) &&
            out->exportsDinput)
            out->kind = DllKind::Ual;
    }
    return true;
}

std::string Describe(const DllInfo& d) {
    std::string name;
    switch (d.kind) {
        case DllKind::Ual: name = "Ultimate ASI Loader"; break;
        case DllKind::ReShade: name = "ReShade"; break;
        case DllKind::SpecialK: name = "Special K"; break;
        case DllKind::Microsoft: name = "Windows DirectInput"; break;
        case DllKind::Other: name = !d.product.empty() ? d.product : !d.description.empty() ? d.description : ""; break;
    }
    std::string v = d.version;
    if (const size_t dash = v.find('-'); dash != std::string::npos) v.resize(dash);
    if (d.kind == DllKind::Ual && v.size() > 2 && v.compare(v.size() - 2, 2, ".0") == 0 && std::count(v.begin(), v.end(), '.') == 3)
        v.resize(v.size() - 2);
    if (name.empty()) return "";
    return v.empty() ? name : name + " " + v;
}

std::string FileProductVersion(const std::wstring& path) {
    Strings s;
    if (!ReadVersionStrings(path, &s)) return {};
    return !s.productVersion.empty() ? s.productVersion : s.fileVersion;
}

std::string DllInfoJson(const DllInfo& d) {
    jsonmini::Obj o;
    o.Str("file", d.file).Str("sha256", d.sha256).UInt("size", d.size);
    if (!d.product.empty()) o.Str("product", d.product);
    if (!d.company.empty()) o.Str("company", d.company);
    if (!d.description.empty()) o.Str("description", d.description);
    if (!d.version.empty()) o.Str("version", d.version);
    o.Str("kind", DllKindName(d.kind)).Bool("known", d.known).Bool("ours", d.ours);
    const std::string label = Describe(d);
    if (!label.empty()) o.Str("label", label);
    return o.End();
}
}  // namespace melange::launcher::setup
