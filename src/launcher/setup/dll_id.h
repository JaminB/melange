#pragma once
#include <cstdint>
#include <string>
#include <vector>

// What is this DLL? Read from disk only (version resource, export table, bytes); it is never loaded to run.
namespace melange::launcher::setup {
enum class DllKind { Ual, Microsoft, ReShade, SpecialK, Other };
const char* DllKindName(DllKind k);

struct DllInfo {
    std::wstring path;
    std::string file, sha256, product, company, description, version, internalName, originalName;
    uint64_t size = 0;
    DllKind kind = DllKind::Other;
    bool known = false;   // kind decided by a known hash or by the version resource
    bool ours = false;    // the exact loader Melange ships
    bool exportsDinput = false;
};
// The UAL builds recognised by hash (at least the one Melange ships).
const std::vector<std::string>& KnownUalHashes();
// `oursSha256`: the payload's dinput8.dll hash ("" when there is no payload).
bool IdentifyDll(const std::wstring& path, const std::string& oursSha256, DllInfo* out);
std::vector<std::string> Exports(const std::wstring& path);   // export names, from the file on disk
std::string DllInfoJson(const DllInfo& d);
// "ReShade 6.3.0", "Ultimate ASI Loader 9.7.4", or what can be told.
std::string Describe(const DllInfo& d);
// melange.asi's version from its version resource ("" when unreadable).
std::string FileProductVersion(const std::wstring& path);
}  // namespace melange::launcher::setup
