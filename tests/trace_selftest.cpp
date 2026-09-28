// Offline self-test for the GL trace decoder, texture decoder, PNG writers and the GPU compatibility report
// (src/render/mirage/trace_decode.cpp, compat_report.cpp).
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#pragma warning(push)
#pragma warning(disable : 4505)
#include "stb_image.h"
#pragma warning(pop)

#include "miniz.h"
#include "render/mirage/compat.h"
#include "render/mirage/trace_internal.h"

namespace t = melange::mirage::trace;
namespace c = melange::mirage::compat;

namespace {
int g_checks = 0, g_failures = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::cerr << "FAIL: " << what << "\n";
}

// Strict enough JSON validator for our own output.
struct Json {
    const std::string& s;
    size_t i = 0;
    void Ws() {
        while (i < s.size() && strchr(" \t\r\n", s[i])) ++i;
    }
    bool Lit(const char* l) {
        size_t n = strlen(l);
        if (s.compare(i, n, l) != 0) return false;
        i += n;
        return true;
    }
    bool Str() {
        if (s[i] != '"') return false;
        for (++i; i < s.size(); ++i) {
            if (s[i] == '"') {
                ++i;
                return true;
            }
            if (static_cast<unsigned char>(s[i]) < 0x20) return false;
            if (s[i] == '\\') ++i;
        }
        return false;
    }
    bool Num() {
        size_t b = i;
        if (s[i] == '-') ++i;
        while (i < s.size() && (isdigit(static_cast<unsigned char>(s[i])) || strchr(".eE+-", s[i]))) ++i;
        return i > b && isdigit(static_cast<unsigned char>(s[i - 1]));
    }
    bool Val() {
        Ws();
        if (i >= s.size()) return false;
        char ch = s[i];
        if (ch == '{' || ch == '[') {
            char close = ch == '{' ? '}' : ']';
            ++i;
            Ws();
            if (s[i] == close) return ++i, true;
            for (;;) {
                if (ch == '{') {
                    Ws();
                    if (!Str()) return false;
                    Ws();
                    if (s[i++] != ':') return false;
                }
                if (!Val()) return false;
                Ws();
                if (s[i] == ',') {
                    ++i;
                    continue;
                }
                return s[i++] == close;
            }
        }
        if (ch == '"') return Str();
        if (ch == 't') return Lit("true");
        if (ch == 'f') return Lit("false");
        if (ch == 'n') return Lit("null");
        return Num();
    }
};
bool ValidJson(const std::string& s) {
    Json j{s};
    if (!j.Val()) return false;
    j.Ws();
    return j.i == s.size();
}

void TestTables() {
    Check(t::SigCount() > 2500, "signature table has the GL registry");
    const char* names[] = {"glBindFramebuffer", "glDrawElements", "glNamedProgramLocalParameters4fvEXT", "glBindProgramARB",
                           "glLoadMatrixf", "wglGetProcAddress", "glGetError", "glTexImage2D"};
    for (const char* n : names) Check(t::FindSig(n) != nullptr, std::string("signature for ") + n);
    Check(t::FindSig("glNotAFunction") == nullptr, "unknown name");
    Check(t::FindSig(nullptr) == nullptr, "null name");
    Check(t::EnumName(0x8D40) && !strcmp(t::EnumName(0x8D40), "GL_FRAMEBUFFER"), "GL_FRAMEBUFFER");
    Check(t::EnumName(0x0DE1) && !strcmp(t::EnumName(0x0DE1), "GL_TEXTURE_2D"), "GL_TEXTURE_2D");
    Check(t::EnumName(0x8620) && !strcmp(t::EnumName(0x8620), "GL_VERTEX_PROGRAM_ARB"), "GL_VERTEX_PROGRAM_ARB");
    Check(t::EnumName(0xDEADBEEF) == nullptr, "unknown enum");
}

void TestDecode() {
    std::string json, text;
    bool raw = false, trunc = false;
    uint32_t a[8] = {0x8D40, 1};
    t::DecodeArgs(t::FindSig("glBindFramebuffer"), a, &json, &text, &raw, &trunc);
    Check(json == "[36160,1]" && text == "GL_FRAMEBUFFER, 1" && !raw && !trunc, "glBindFramebuffer: " + json + " / " + text);

    uint32_t b[8] = {4};
    t::DecodeArgs(t::FindSig("glBegin"), b, &json, &text, &raw, &trunc);
    Check(text == "GL_TRIANGLES", "glBegin(4): " + text);
    b[0] = 0;
    t::DecodeArgs(t::FindSig("glBegin"), b, &json, &text, &raw, &trunc);
    Check(text == "GL_POINTS", "glBegin(0) uses the primitive group: " + text);

    float f[4] = {1.f, 0.5f, -2.f, 0.f};
    uint32_t col[8] = {};
    memcpy(col, f, sizeof f);
    t::DecodeArgs(t::FindSig("glColor4f"), col, &json, &text, &raw, &trunc);
    Check(json == "[1,0.5,-2,0]" && text == "1, 0.5, -2, 0", "glColor4f: " + json + " / " + text);

    uint32_t de[8] = {4, 36, 0x1403, 0};
    t::DecodeArgs(t::FindSig("glDrawElements"), de, &json, &text, &raw, &trunc);
    Check(json == "[4,36,5123,\"0x0\"]" && text == "GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, NULL", "glDrawElements: " + json + " / " + text);

    uint32_t ti[8] = {0x0DE1, 0, 0x1908, 64, 32, 0, 0x1908, 0x1401};
    t::DecodeArgs(t::FindSig("glTexImage2D"), ti, &json, &text, &raw, &trunc);
    Check(trunc, "glTexImage2D takes 9 dwords: truncated");

    double d = 0.25;
    uint32_t dr[8] = {};
    memcpy(dr, &d, 8);
    d = 1.0;
    memcpy(dr + 2, &d, 8);
    t::DecodeArgs(t::FindSig("glDepthRange"), dr, &json, &text, &raw, &trunc);
    Check(json == "[0.25,1]", "glDepthRange doubles: " + json);

    uint32_t z[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    t::DecodeArgs(nullptr, z, &json, &text, &raw, &trunc);
    Check(raw && json == "[1,2,3,4,5,6,7,8]", "unknown signature: raw dwords");
}

void TestCallJson() {
    melange::mirage::hub::Rec r{};
    r.frame = 11615;
    r.pass = 1;
    r.caller = 0x6f64be;
    r.a[0] = 36160;
    r.a[1] = 1;
    r.tsc = 123456;
    std::string line = t::CallJson(0, r, "glBindFramebuffer", melange::mirage::hub::Src::ExeProc, nullptr);
    Check(line == "{\"i\":0,\"f\":11615,\"fn\":\"glBindFramebuffer\",\"src\":\"exe-proc\",\"caller\":\"0x6f64be\",\"pass\":1,"
                  "\"args\":[36160,1],\"text\":\"GL_FRAMEBUFFER, 1\",\"payload\":null,\"tsc\":123456}",
          "calls.jsonl line: " + line);
    Check(ValidJson(line), "calls.jsonl line is JSON");

    melange::mirage::hub::Rec u{};
    std::string raw = t::CallJson(7, u, "glUnknownThing", melange::mirage::hub::Src::CgGLIat, nullptr);
    Check(ValidJson(raw) && raw.find("\"raw\":true") != std::string::npos && raw.find("cggl-import") != std::string::npos,
          "unknown call line: " + raw);

    melange::mirage::hub::Rec ti{};
    uint32_t a[8] = {0x0DE1, 0, 0x1908, 64, 32, 0, 0x1908, 0x1401};
    memcpy(ti.a, a, sizeof a);
    std::string pl = "[1,2]";
    std::string tl = t::CallJson(1, ti, "glTexImage2D", melange::mirage::hub::Src::ExeIat, &pl);
    Check(ValidJson(tl) && tl.find("\"bytes\":8192") != std::string::npos && tl.find("\"truncated\":true") != std::string::npos &&
              tl.find("\"payload\":[1,2]") != std::string::npos,
          "glTexImage2D line: " + tl);

    float nan = std::nanf("");
    melange::mirage::hub::Rec n{};
    memcpy(n.a, &nan, 4);
    std::string nl = t::CallJson(2, n, "glClearDepthf", melange::mirage::hub::Src::ExeProc, nullptr);
    Check(ValidJson(nl), "NaN argument stays valid JSON: " + nl);
}

void TestPayload() {
    t::PayloadSpec p;
    uint32_t lm[4] = {0x1000};
    Check(t::PayloadOf(*t::FindSig("glLoadMatrixf"), lm, 4, &p) && p.bytes == 64 && p.ptr == 0x1000 && !p.hashOnly,
          "glLoadMatrixf: 64 bytes");
    Check(!t::PayloadOf(*t::FindSig("glLoadMatrixf"), lm, 0, &p), "argument not readable");
    uint32_t zero[4] = {0};
    Check(!t::PayloadOf(*t::FindSig("glLoadMatrixf"), zero, 4, &p), "null pointer: no payload");
    uint32_t np[8] = {7, 0x8804, 0, 3, 0x2000};
    Check(t::PayloadOf(*t::FindSig("glNamedProgramLocalParameters4fvEXT"), np, 8, &p) && p.bytes == 48 && !p.capped,
          "Parameters4fvEXT: 16 x count");
    np[3] = 100;
    Check(t::PayloadOf(*t::FindSig("glNamedProgramLocalParameters4fvEXT"), np, 8, &p) && p.bytes == 1024 && p.capped,
          "Parameters4fvEXT: capped");
    uint32_t bd[8] = {0x8892, 4096, 0x3000, 0x88E4};
    Check(t::PayloadOf(*t::FindSig("glBufferData"), bd, 8, &p) && p.hashOnly && p.bytes == 4096, "glBufferData: size and hash only");
    Check(!t::PayloadOf(*t::FindSig("glBindTexture"), bd, 8, &p), "no pointer: no payload");

    float m[3] = {1.5f, -1.f, 0.f};
    Check(t::PayloadJson('f', reinterpret_cast<const uint8_t*>(m), 12) == "[1.5,-1,0]", "float payload");
    int16_t s[2] = {-3, 7};
    Check(t::PayloadJson('h', reinterpret_cast<const uint8_t*>(s), 4) == "[-3,7]", "short payload");
    Check(t::PayloadJson('C', reinterpret_cast<const uint8_t*>("\x01\xff"), 2) == "[1,255]", "ubyte payload");

    Check(t::PixelBytes(0x1908, 0x1401, 64, 32, 1) == 8192, "RGBA8 bytes");
    Check(t::PixelBytes(0x1907, 0x1401, 3, 3, 1) == 27, "RGB8 bytes");
    Check(t::PixelBytes(0x1908, 0x8366, 4, 4, 1) == 32, "1555 bytes");
    Check(t::PixelBytes(0x1908, 0x1406, 2, 2, 1) == 64, "float bytes");
    Check(t::PixelBytes(0x1234, 0x1401, 2, 2, 1) == 0, "unknown format");
}

void TestCategories() {
    Check(t::Categorize("glDrawElements") == t::kDraw, "draw");
    Check(t::Categorize("glDrawRangeElementsEXT") == t::kDraw, "draw range");
    Check(t::Categorize("glDrawBuffer") == 0, "glDrawBuffer is not a draw");
    Check(t::Categorize("glBindProgramARB") == t::kProgramSwitch, "program switch");
    Check(t::Categorize("glNamedProgramLocalParameters4fvEXT") == t::kParamFlush, "param flush");
    Check(t::Categorize("glBindFramebufferEXT") == t::kFboBind, "fbo bind");
    Check(t::Categorize("glBindMultiTextureEXT") == t::kTexBind, "tex bind");
    Check(t::Categorize("glTexSubImage2D") == t::kTexUpload && t::Categorize("glCompressedTexImage2DARB") == t::kTexUpload,
          "tex upload");
    Check(t::Categorize("glGetTexImage") == 0 && t::Categorize("glCopyTexImage2D") == 0, "not uploads");
    Check(t::Categorize("glGetError") == t::kGetError, "glGetError");
}

void TestTextures() {
    std::vector<uint8_t> out;
    int ch = 0;
    // 3x2 RGB, alignment 4: rows are 9 bytes padded to 12; the first row is the bottom one
    const uint8_t rgb[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0xEE, 0xEE, 0xEE, 10, 11, 12, 13, 14, 15, 16, 17, 18};
    Check(t::DecodeUpload(0x1907, 0x1401, 3, 2, 4, 0, rgb, &out, &ch) && ch == 4 && out.size() == 24, "RGB decode");
    const uint8_t want[] = {10, 11, 12, 255, 13, 14, 15, 255, 16, 17, 18, 255, 1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255};
    Check(out.size() == 24 && !memcmp(out.data(), want, 24), "RGB rows flipped, padding skipped, alpha 255");
    Check(t::DecodeUpload(0x1907, 0x1401, 3, 2, 1, 0, rgb, &out, &ch) && out[0] == 0xEE, "RGB alignment 1 is tight");

    const uint8_t bgra[] = {1, 2, 3, 4};
    Check(t::DecodeUpload(0x80E1, 0x1401, 1, 1, 4, 0, bgra, &out, &ch) && out[0] == 3 && out[1] == 2 && out[2] == 1 && out[3] == 4,
          "BGRA swizzle");
    // 1_5_5_5_REV with GL_RGBA: R in bits 0-4, G 5-9, B 10-14, A 15
    uint16_t px[2] = {static_cast<uint16_t>(31 | (0 << 5) | (16 << 10) | (1 << 15)), static_cast<uint16_t>(1 << 5)};
    Check(t::DecodeUpload(0x1908, 0x8366, 2, 1, 4, 0, reinterpret_cast<const uint8_t*>(px), &out, &ch) && ch == 4, "1555 decode");
    Check(out[0] == 255 && out[1] == 0 && out[2] == 132 && out[3] == 255, "1555 pixel 0 expanded");
    Check(out[4] == 0 && out[5] == 8 && out[6] == 0 && out[7] == 0, "1555 pixel 1 expanded");
    const uint8_t alpha[] = {0x80, 0x40};
    Check(t::DecodeUpload(0x1906, 0x1401, 2, 1, 1, 0, alpha, &out, &ch) && ch == 2 && out[0] == 255 && out[1] == 0x80 &&
              out[3] == 0x40,
          "ALPHA as grey + alpha");
    Check(!t::DecodeUpload(0x1908, 0x1406, 1, 1, 4, 0, alpha, &out, &ch), "float upload not decoded");
    Check(!t::DecodeUpload(0x1908, 0x1401, 0, 1, 4, 0, alpha, &out, &ch), "empty upload");
    const uint8_t rl[] = {1, 1, 1, 1, 9, 9, 9, 9, 2, 2, 2, 2, 9, 9, 9, 9};
    Check(t::DecodeUpload(0x1908, 0x1401, 1, 2, 4, 2, rl, &out, &ch) && out[0] == 2 && out[4] == 1, "row length");
}

void TestPng() {
    std::vector<uint8_t> px(5 * 3 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint8_t>(i * 7);
    std::string png = t::Png8(px.data(), 5, 3, 4);
    int w = 0, h = 0, n = 0;
    uint8_t* dec = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()), static_cast<int>(png.size()), &w, &h, &n, 4);
    Check(dec && w == 5 && h == 3 && !memcmp(dec, px.data(), px.size()), "RGBA PNG round trip (top-down rows)");
    stbi_image_free(dec);

    std::vector<uint16_t> g = {0, 1, 256, 65535, 12345, 40000};
    std::string p16 = t::PngGrey16(g.data(), 3, 2);
    Check(stbi_is_16_bit_from_memory(reinterpret_cast<const stbi_uc*>(p16.data()), static_cast<int>(p16.size())) != 0, "16-bit PNG");
    uint16_t* d16 = stbi_load_16_from_memory(reinterpret_cast<const stbi_uc*>(p16.data()), static_cast<int>(p16.size()), &w, &h, &n, 1);
    Check(d16 && w == 3 && h == 2 && n == 1 && !memcmp(d16, g.data(), g.size() * 2), "grey16 PNG round trip");
    stbi_image_free(d16);
    Check(t::SafeFileName("Textures\\Logo 1:a.tga") == "Textures_Logo_1_a.tga", "safe file name");
}

void TestCompat() {
    using melange::compat::Kind;
    using melange::compat::Status;
    melange::compat::Report(Kind::Effect, "mirage-samples/bloom", Status::Failed, "blur.frag(12): syntax error", "mirage-samples");
    melange::compat::Report(Kind::Feature, "gldebug", Status::Skipped, "disabled");
    melange::compat::Report(Kind::Shader, "PostProcess.cg:CopyFxaa", Status::Loaded, "FXAA fix applied", "builtin");
    melange::compat::Report(Kind::Feature, "gldebug", Status::Loaded, "debug context 4.6");
    melange::compat::Report(Kind::Feature, nullptr, Status::Loaded);
    melange::compat::Entry e[8];
    size_t n = melange::compat::List(e, 8);
    Check(n == 3, "three distinct reports");
    Check(n >= 2 && e[1].status == Status::Loaded && !strcmp(e[1].reason, "debug context 4.6") && e[1].owner[0] == 0,
          "a later report replaces the earlier one in place");
    Check(melange::compat::List(e, 1) == 1 && melange::compat::List(nullptr, 8) == 0, "List bounds");
    melange::compat::Forget(Kind::Feature, "gldebug");
    Check(melange::compat::List(e, 8) == 2, "Forget");
    std::string longId(300, 'x');
    melange::compat::Report(Kind::Pass, longId.c_str(), Status::Skipped, longId.c_str(), longId.c_str());
    Check(melange::compat::List(e, 8) == 3 && strlen(e[2].id) == sizeof e[2].id - 1, "long strings truncated");

    c::Snapshot s;
    s.gpu.valid = true;
    strcpy(s.gpu.vendor, "ATI \"Technologies\"");
    strcpy(s.gpu.renderer, "Radeon");
    strcpy(s.gpu.cgVertex, "arbvp1");
    strcpy(s.gpu.cgFragment, "arbfp1");
    s.extensions = {"GL_ARB_fragment_program", "GL_EXT_framebuffer_object"};
    s.cgProfiles = {{"arbfp1", true}, {"fp40", false}};
    s.programsLoaded = true;
    c::ProgramRow ok, bad, fx;
    ok.file = "Landscape.cg", ok.entry = "LandscapeFragmentMain", ok.stage = 1;
    bad.file = "PostProcess.cg", bad.entry = "CopyFxaaSepia", bad.stage = 1, bad.failed = true;
    bad.reason = "Fxaa3_9.h(1346): error C3004: function \"tex2D\" not supported in this profile";
    fx.file = "PostProcess.cg", fx.entry = "CopyFxaa", fx.stage = 1;
    s.programs = {ok, bad, fx};
    s.effects.push_back({"mirage-samples/bloom", "Bloom", "PostWorld", true, true});
    s.effects.push_back({"mirage-samples/ssao", "SSAO", "PostWorld", false, false});
    s.stages.push_back({"World", 44, false, true, 10});
    c::SetSnapshot(s);
    std::string json = c::Json(), text = c::Text();
    Check(ValidJson(json), "compat.json is JSON");
    Check(json.find("\"format\":\"melange-gpu-compat\"") != std::string::npos, "compat.json format tag");
    Check(json.find("\"file\":\"PostProcess.cg\",\"entry\":\"CopyFxaaSepia\",\"stage\":\"fragment\",\"status\":\"failed\"") !=
              std::string::npos,
          "failed shader in compat.json");
    Check(json.find("\"entry\":\"CopyFxaa\",\"stage\":\"fragment\",\"status\":\"loaded\",\"reason\":\"FXAA fix applied\","
                    "\"owner\":\"builtin\"") != std::string::npos,
          "explicit shader report merged into the program list");
    Check(json.find("\"id\":\"mirage-samples/ssao\",\"title\":\"SSAO\",\"stage\":\"PostWorld\",\"status\":\"skipped\","
                    "\"reason\":\"disabled\"") != std::string::npos,
          "disabled effect is skipped");
    Check(json.find("\"reason\":\"blur.frag(12): syntax error\"") != std::string::npos, "effect failure reason");
    Check(text.find("FAILED   fp PostProcess.cg:CopyFxaaSepia - Fxaa3_9.h(1346)") != std::string::npos, "text report lists the failure");
    Check(text.find("Cg profiles supported: arbfp1") != std::string::npos && text.find("not supported: fp40") != std::string::npos,
          "text report lists Cg profiles");
    melange::compat::Gpu g = melange::compat::GetGpu();
    Check(g.valid && !strcmp(g.cgFragment, "arbfp1"), "GetGpu copies the snapshot");
    Check(ValidJson(c::BuildJson(c::Snapshot{}, {})), "empty report is JSON");
}
std::string ZipFile(mz_zip_archive& z, const char* name) {
    size_t n = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&z, name, &n, 0);
    if (!p) return {};
    std::string s(static_cast<const char*>(p), n);
    mz_free(p);
    return s;
}

void TestMcap() {
    using melange::mirage::hub::Rec;
    using melange::mirage::hub::Src;
    t::McapData d;
    d.opt.frames = 2;
    d.fnName = {"glBindFramebuffer", "glLoadMatrixf", "glBindProgramARB", "glDrawElements", "glDrawArrays", "glBufferData"};
    d.fnSrc = {Src::ExeProc, Src::ExeIat, Src::CgGLProc, Src::ExeIat, Src::ExeIat, Src::ExeProc};
    auto rec = [&](uint16_t fn, uint32_t frame, uint8_t pass, std::initializer_list<uint32_t> args) {
        Rec r{};
        r.fn = fn;
        r.frame = frame;
        r.pass = pass;
        r.caller = 0x6f64be;
        int k = 0;
        for (uint32_t a : args) r.a[k++] = a;
        d.recs.push_back(r);
        d.ringPos.push_back(1000 + static_cast<uint32_t>(d.recs.size()) - 1);
    };
    rec(0, 100, 1, {0x8D40, 1});
    rec(1, 100, 3, {0x1234});
    rec(2, 100, 3, {0x8804, 5});
    rec(3, 100, 3, {4, 36, 0x1403, 0});
    rec(5, 101, 3, {0x8892, 4096, 0x5000, 0x88E4});
    rec(4, 101, 3, {4, 0, 3});
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1};
    d.arena.assign(reinterpret_cast<const char*>(m), sizeof m);
    d.pay[1001] = {t::FindSig("glLoadMatrixf"), 0, 64, 64, false, false, {}};
    d.pay[1004] = {t::FindSig("glBufferData"), 0, 0, 4096, true, false, "ab12"};
    d.pay[1003] = {t::FindSig("glLoadMatrixf"), 0, 64, 64, false, false, {}};  // wrong function: must be ignored
    d.markers.push_back({1003, "\"type\":\"stage\",\"stage\":\"World\",\"frame\":100"});
    d.notes.push_back("synthetic \"capture\"");
    d.begin = "{\"format\":\"melange-capture\",\"version\":1,\"which\":\"begin\",\"values\":{}}";
    d.end = "{\"format\":\"melange-capture\",\"version\":1,\"which\":\"end\",\"values\":{}}";
    d.frameW = 3, d.frameH = 2;
    for (int i = 0; i < 24; ++i) d.frame.push_back(static_cast<uint8_t>(i * 10));
    t::McapProgram p;
    p.file = "Landscape.cg", p.entry = "LandscapeFragmentMain", p.stage = 1, p.arbName = 5, p.source = "CG/Landscape.cg";
    p.asmText = "!!ARBfp1.0\nEND\n";
    d.progs.push_back(p);
    t::McapTexture tx;
    tx.gl = 7, tx.w = 2, tx.h = 2, tx.ifmt = 0x8058, tx.levels = 1;
    tx.px8 = {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4};
    d.tex.push_back(tx);
    t::McapTexture dt;
    dt.gl = 9, dt.w = 2, dt.h = 1, dt.ifmt = 0x88F0, dt.depth = true, dt.levels = 1;
    dt.px16 = {0, 65535};
    d.tex.push_back(dt);
    t::McapTexture sk;
    sk.gl = 11, sk.skipped = "maxTextureMB reached";
    d.tex.push_back(sk);
    d.firstFrame = 100, d.lastFrame = 101, d.scene = "match", d.melangeVersion = "0.0-test";

    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    std::wstring path = std::wstring(tmpDir) + L"melange_trace_selftest.mcap";
    t::McapResult res;
    std::string err;
    Check(t::WriteMcap(d, path, &res, &err), "WriteMcap: " + err);
    Check(res.calls == 6 && res.draws == 2 && res.textures == 2 && res.payloads == 2 && res.programsWithAsm == 1,
          "WriteMcap result counts");
    Check(d.tex[0].px8.empty(), "texture pixels freed after encoding");

    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, narrow, MAX_PATH, nullptr, nullptr);
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, narrow, 0)) {
        Check(false, "capture opens as a zip");
        return;
    }
    std::string man = ZipFile(z, "manifest.json");
    Check(ValidJson(man) && man.find("\"format\":\"melange-capture\",\"version\":1") != std::string::npos, "manifest.json");
    Check(man.find("\"calls\":6,\"draws\":2,\"textures\":2") != std::string::npos, "manifest counts: " + man.substr(0, 400));
    Check(man.find("\"frames\":[100,101]") != std::string::npos, "manifest frames");
    for (const char* f : {"calls.jsonl", "events.jsonl", "state/begin.json", "state/end.json", "programs/index.json",
                          "programs/0.asm", "textures/index.json", "textures/7.png", "textures/9.png", "frame.png"})
        Check(man.find(std::string("\"") + f + "\"") != std::string::npos && mz_zip_reader_locate_file(&z, f, nullptr, 0) >= 0,
              std::string("file listed and present: ") + f);
    std::string calls = ZipFile(z, "calls.jsonl");
    size_t lines = 0, pos = 0;
    bool allJson = true;
    for (size_t e; (e = calls.find('\n', pos)) != std::string::npos; pos = e + 1, ++lines) allJson &= ValidJson(calls.substr(pos, e - pos));
    Check(lines == 6 && allJson, "calls.jsonl: one JSON line per record");
    Check(calls.find("\"fn\":\"glLoadMatrixf\",\"src\":\"exe-import\",\"caller\":\"0x6f64be\",\"pass\":3,\"args\":[\"0x1234\"],"
                     "\"text\":\"0x1234\",\"payload\":[1,0,0,0,0,1,0,0,0,0,1,0,2,3,4,1]") != std::string::npos,
          "matrix payload");
    Check(calls.find("\"payload\":{\"bytes\":4096,\"hash\":\"ab12\"}") != std::string::npos, "buffer payload is size and hash");
    Check(calls.find("\"fn\":\"glDrawElements\",\"src\":\"exe-import\",\"caller\":\"0x6f64be\",\"pass\":3,\"args\":[4,36,5123,\"0x0\"],"
                     "\"text\":\"GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, NULL\",\"payload\":null") != std::string::npos,
          "payload of another function is not attached");
    std::string ev = ZipFile(z, "events.jsonl");
    Check(ev.find("{\"at\":0,\"type\":\"frame-begin\",\"frame\":100}") != std::string::npos, "frame-begin event");
    Check(ev.find("{\"at\":2,\"type\":\"cg-bind\",\"target\":34820,\"arb\":5,\"program\":\"Landscape.cg:LandscapeFragmentMain\"") !=
              std::string::npos,
          "cg-bind event names the program");
    Check(ev.find("{\"at\":3,\"type\":\"stage\",\"stage\":\"World\"") != std::string::npos, "stage marker placed before record 3");
    Check(ev.find("{\"at\":4,\"type\":\"frame-end\",\"frame\":100}") != std::string::npos &&
              ev.find("{\"at\":6,\"type\":\"frame-end\",\"frame\":101}") != std::string::npos,
          "frame-end events");
    Check(ev.find("synthetic \\\"capture\\\"") != std::string::npos, "note escaped");
    pos = 0;
    allJson = true;
    for (size_t e; (e = ev.find('\n', pos)) != std::string::npos; pos = e + 1) allJson &= ValidJson(ev.substr(pos, e - pos));
    Check(allJson, "events.jsonl lines are JSON");
    std::string pi = ZipFile(z, "programs/index.json"), ti = ZipFile(z, "textures/index.json");
    Check(ValidJson(pi) && pi.find("\"arbName\":5") != std::string::npos && pi.find("\"asm\":\"programs/0.asm\"") != std::string::npos,
          "programs/index.json");
    Check(ZipFile(z, "programs/0.asm") == "!!ARBfp1.0\nEND\n", "program text");
    Check(ValidJson(ti) && ti.find("\"skipped\":\"maxTextureMB reached\"") != std::string::npos &&
              ti.find("\"gl\":9,\"w\":2,\"h\":1,\"internalFormat\":35056,\"levels\":1,\"depth\":true") != std::string::npos,
          "textures/index.json");
    std::string png = ZipFile(z, "textures/7.png");
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()), static_cast<int>(png.size()), &w, &h, &n, 4);
    Check(px && w == 2 && h == 2 && px[0] == 3 && px[8] == 1, "texture PNG is flipped to top-down");
    stbi_image_free(px);
    std::string dpng = ZipFile(z, "textures/9.png");
    uint16_t* d16 = stbi_load_16_from_memory(reinterpret_cast<const stbi_uc*>(dpng.data()), static_cast<int>(dpng.size()), &w, &h, &n, 1);
    Check(d16 && w == 2 && h == 1 && d16[0] == 0 && d16[1] == 65535, "depth texture as 16-bit grey");
    stbi_image_free(d16);
    std::string fpng = ZipFile(z, "frame.png");
    px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(fpng.data()), static_cast<int>(fpng.size()), &w, &h, &n, 4);
    Check(px && w == 3 && h == 2 && !memcmp(px, d.frame.data(), 24), "frame.png as given (already top-down)");
    stbi_image_free(px);
    mz_zip_reader_end(&z);
    std::cout << "trace_selftest: sample capture left at " << narrow << "\n";
}
}  // namespace

int main() {
    TestTables();
    TestDecode();
    TestCallJson();
    TestPayload();
    TestCategories();
    TestTextures();
    TestPng();
    TestCompat();
    TestMcap();
    std::cout << "trace_selftest: " << g_checks - g_failures << "/" << g_checks << " checks passed\n";
    return g_failures ? 1 : 0;
}
