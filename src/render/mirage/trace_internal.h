#pragma once
// Shared pieces of the MirageTrace module (trace.cpp, trace_capture.cpp, trace_panel.cpp, texdump.cpp).
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "melange/gltrace.h"
#include "render/mirage/hub.h"

namespace melange::mirage::trace {
// ---------------------------------------------------------------- trace_decode.cpp (no GL, no game; self-tested)
struct GlSig {
    const char* name;
    const char* args;  // one code per parameter, see scripts/gen_gl_sigs.py
    int8_t payArg;     // pointer argument copied while capturing, -1 = none
    uint8_t payKind;   // 1 fixed count, 2 count from argument, 3 byte size from argument (hash only)
    int8_t countArg;
    uint16_t n;
    char elem;
};
const GlSig* FindSig(const char* name);
size_t SigCount();
const char* EnumName(uint32_t value, int group = -1);  // nullptr if unknown

// JSON array of the decoded arguments and a readable "GL_FRAMEBUFFER, 1" form. Without a signature the 8 raw
// dwords are returned and *raw is set. *truncated: the function takes more than the 8 recorded dwords.
void DecodeArgs(const GlSig* sig, const uint32_t a[8], std::string* json, std::string* text, bool* raw, bool* truncated);

constexpr uint32_t kMaxPayload = 1024;
struct PayloadSpec {
    uintptr_t ptr = 0;
    uint32_t bytes = 0;     // bytes to copy (kind 1/2, capped) or the size the call names (kind 3)
    bool hashOnly = false;  // kind 3: record the size and a hash of the first 64 bytes, never the contents
    bool capped = false;
};
// `args` are the call's stack arguments; `nargs` how many dwords are readable.
bool PayloadOf(const GlSig& sig, const uint32_t* args, int nargs, PayloadSpec* out);
std::string PayloadJson(char elem, const uint8_t* data, uint32_t bytes);  // JSON array of the elements
int ElemBytes(char elem);
// Client-memory size of a glTexImage*/glTexSubImage* pixel upload (tight rows), 0 if unknown.
uint64_t PixelBytes(uint32_t format, uint32_t type, int64_t w, int64_t h, int64_t d);

const char* SrcName(hub::Src s);  // "exe-import", "exe-proc", "cggl-import", "cggl-proc"
uint8_t SrcBit(hub::Src s);       // gltrace::Source bit

enum Category : uint32_t {
    kDraw = 1, kProgramSwitch = 2, kParamFlush = 4, kFboBind = 8, kTexBind = 16, kTexUpload = 32, kGetError = 64,
};
uint32_t Categorize(const char* glName);

// One calls.jsonl line (no trailing newline).
std::string CallJson(uint64_t i, const hub::Rec& r, const char* fn, hub::Src src, const std::string* payload);

// Level 0 of an engine upload decoded to top-down 8-bit pixels: 4 channels (RGBA), or 2 (grey + alpha) for
// GL_ALPHA / GL_LUMINANCE_ALPHA. Honours GL_UNPACK_ALIGNMENT / GL_UNPACK_ROW_LENGTH. False for unsupported formats.
bool DecodeUpload(uint32_t format, uint32_t type, int w, int h, int alignment, int rowLength, const uint8_t* src,
                  std::vector<uint8_t>* out, int* channels);
// PNG encoders; input rows are top-down.
std::string Png8(const uint8_t* px, int w, int h, int channels);
std::string PngGrey16(const uint16_t* px, int w, int h);
std::string SafeFileName(const std::string& s);

// ---------------------------------------------------------------- trace_mcap.cpp (no GL, no game; self-tested)
struct McapPayload {
    const GlSig* sig;
    uint32_t off, len, full;  // arena slice; full = the size the call named
    bool hashOnly, capped;
    std::string hash;
};
struct McapMarker {
    uint32_t ringPos;  // the event is placed before the first record at or after this ring position
    std::string json;  // object members after "at"
};
struct McapTexture {
    uint32_t gl = 0;
    int w = 0, h = 0, ifmt = 0, levels = 0;
    bool depth = false;
    std::string name, skipped;
    std::vector<uint8_t> px8;    // RGBA, GL row order (bottom-up)
    std::vector<uint16_t> px16;  // depth
};
struct McapProgram {
    std::string file, entry, source, asmText, owner;
    int stage = 0;
    uint32_t cgProgram = 0, arbName = 0, binds = 0;
    bool failed = false, overridden = false, glsl = false;
};
struct McapData {
    gltrace::CaptureOptions opt;
    std::vector<hub::Rec> recs;
    std::vector<uint32_t> ringPos;  // per record
    std::vector<std::string> fnName;
    std::vector<hub::Src> fnSrc;
    std::unordered_map<uint32_t, McapPayload> pay;  // by ring position
    std::string arena;
    std::vector<McapMarker> markers;
    std::vector<std::string> notes;
    std::string begin, end;       // state/*.json
    std::vector<uint8_t> frame;   // RGBA, top-down
    int frameW = 0, frameH = 0;
    std::vector<McapTexture> tex;
    std::vector<McapProgram> progs;
    uint64_t firstFrame = 0, lastFrame = 0, dropped = 0, droppedPayloads = 0;
    std::string scene, glVendor, glRenderer, glVersion, melangeVersion, exeSha256, exeBuild;
    int winW = 0, winH = 0;
    double readbackMs = 0;  // main-thread time of the last frame's read-backs (the capture hitch)
};
struct McapResult {
    uint64_t calls = 0, programs = 0, draws = 0, textures = 0, programsWithAsm = 0, payloads = 0, events = 0;
};
// Writes <path>.tmp and renames it. Frees texture pixels as it goes.
bool WriteMcap(McapData& d, const std::wstring& path, McapResult* res, std::string* err);

// ---------------------------------------------------------------- trace_tap.cpp
// A pre-call hook on a GL entry point that sees the raw stack: f[0] is the caller's return address, f[1..] the
// arguments. Works for any signature (it never touches the arguments). Install time or main thread; permanent.
using TapFn = void (*)(int slot, const uint32_t* f, void* user);
int AddTap(const char* glName, TapFn fn, void* user);  // -1 on failure

// ---------------------------------------------------------------- trace.cpp
void RunAsync(std::function<void()> job);  // one background worker for PNG and capture writing
std::wstring MelangeDocsDir();             // Documents\Melange
bool EnsureDir(const std::wstring& dir);
bool WriteFileBytes(const std::wstring& path, const std::string& data);
std::wstring Widen(const std::string& utf8);
std::string Scene();
struct History { std::vector<float> calls, draws, busyMs; };
History GetHistory();  // oldest first, up to 240 frames
uint64_t CaptureCount();

// ---------------------------------------------------------------- trace_capture.cpp
bool CaptureRequest(const gltrace::CaptureOptions& opt, std::string* error);
void CaptureOnFrame();
bool CaptureBusy();
gltrace::CaptureState CaptureState(std::wstring* path, std::string* error);
std::wstring CaptureDir();

// ---------------------------------------------------------------- texdump.cpp
void TexdumpInstall(uint32_t atStart, const std::string& filter);
void TexdumpOnFrame();
std::wstring TexdumpDir();
std::string TextureName(uint32_t glName);  // the engine image name of an upload seen by the dumper, or ""

// ---------------------------------------------------------------- trace_panel.cpp
void RegisterPanel();
}  // namespace melange::mirage::trace
