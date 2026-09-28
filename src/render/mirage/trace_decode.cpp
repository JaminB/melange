#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "miniz.h"
#include "render/mirage/trace_internal.h"

namespace melange::mirage::trace {
namespace {
struct GlEnumName {
    uint32_t value;
    const char* name;
};
struct GlSmallEnum {
    int group;
    uint32_t value;
    const char* name;
};
#include "render/mirage/trace_sigs.inc"

void Append(std::string& s, const char* fmt, ...) {
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) s.append(b, std::min<size_t>(static_cast<size_t>(n), sizeof b - 1));
}

void AppendDouble(std::string& s, double v) {
    if (std::isfinite(v)) Append(s, "%.9g", v);
    else s += std::isnan(v) ? "\"nan\"" : (v > 0 ? "\"inf\"" : "\"-inf\"");
}

// Walks an argument code string: calls fn(code, group, dwordOffset) per parameter.
template <class F>
int ForEachParam(const char* args, F&& fn) {
    int off = 0, idx = 0;
    for (const char* p = args; *p; ++p) {
        char c = *p;
        int group = -1;
        if (p[1] == '{') {
            group = atoi(p + 2);
            p = strchr(p, '}');
            if (!p) break;
        }
        if (!fn(c, group, off, idx)) return idx;
        off += (c == 'd' || c == 'l' || c == 'L') ? 2 : 1;
        ++idx;
    }
    return idx;
}

int ParamOffset(const char* args, int param) {
    int found = -1;
    ForEachParam(args, [&](char, int, int off, int idx) {
        if (idx == param) {
            found = off;
            return false;
        }
        return true;
    });
    return found;
}
}  // namespace

const GlSig* FindSig(const char* name) {
    if (!name) return nullptr;
    auto it = std::lower_bound(std::begin(kGlSigs), std::end(kGlSigs), name,
                               [](const GlSig& s, const char* n) { return strcmp(s.name, n) < 0; });
    return it != std::end(kGlSigs) && strcmp(it->name, name) == 0 ? &*it : nullptr;
}

size_t SigCount() { return std::size(kGlSigs); }

const char* EnumName(uint32_t value, int group) {
    if (group >= 0 && value < 16)
        for (const GlSmallEnum& e : kGlSmallEnumGroups)
            if (e.group == group && e.value == value) return e.name;
    auto it = std::lower_bound(std::begin(kGlEnums), std::end(kGlEnums), value,
                               [](const GlEnumName& e, uint32_t v) { return e.value < v; });
    return it != std::end(kGlEnums) && it->value == value ? it->name : nullptr;
}

void DecodeArgs(const GlSig* sig, const uint32_t a[8], std::string* json, std::string* text, bool* raw, bool* truncated) {
    json->assign("[");
    text->clear();
    *raw = sig == nullptr;
    *truncated = false;
    if (!sig) {
        for (int i = 0; i < 8; ++i) Append(*json, i ? ",%u" : "%u", a[i]);
        *json += ']';
        return;
    }
    ForEachParam(sig->args, [&](char c, int group, int off, int idx) {
        int need = (c == 'd' || c == 'l' || c == 'L') ? 2 : 1;
        if (off + need > 8) {
            *truncated = true;
            return false;
        }
        if (idx) {
            *json += ',';
            *text += ", ";
        }
        uint32_t v = a[off];
        uint64_t v64 = v | (need == 2 ? static_cast<uint64_t>(a[off + 1]) << 32 : 0);
        switch (c) {
            case 'e': {
                Append(*json, "%u", v);
                const char* n = EnumName(v, group);
                if (n) *text += n;
                else Append(*text, "0x%X", v);
                break;
            }
            case 'b':
                Append(*json, "%u", v);
                Append(*text, "0x%X", v);
                break;
            case 'z':
                Append(*json, "%u", v);
                *text += v == 0 ? "GL_FALSE" : v == 1 ? "GL_TRUE" : std::to_string(v);
                break;
            case 'i':
            case 's':
                Append(*json, "%d", static_cast<int32_t>(v));
                Append(*text, "%d", static_cast<int32_t>(v));
                break;
            case 'f': {
                float f;
                memcpy(&f, &v, 4);
                AppendDouble(*json, f);
                Append(*text, "%g", f);
                break;
            }
            case 'd': {
                double d;
                memcpy(&d, &v64, 8);
                AppendDouble(*json, d);
                Append(*text, "%g", d);
                break;
            }
            case 'l':
                Append(*json, "%lld", static_cast<long long>(v64));
                Append(*text, "%lld", static_cast<long long>(v64));
                break;
            case 'L':
                Append(*json, "%llu", static_cast<unsigned long long>(v64));
                Append(*text, "%llu", static_cast<unsigned long long>(v64));
                break;
            case 'p':
                Append(*json, "\"0x%x\"", v);
                if (v) Append(*text, "0x%x", v);
                else *text += "NULL";
                break;
            default:
                Append(*json, "%u", v);
                Append(*text, "%u", v);
                break;
        }
        return true;
    });
    *json += ']';
}

int ElemBytes(char elem) {
    switch (elem) {
        case 'd': case 'l': case 'L': return 8;
        case 'f': case 'i': case 'u': return 4;
        case 'h': case 'H': return 2;
        default: return 1;
    }
}

bool PayloadOf(const GlSig& sig, const uint32_t* args, int nargs, PayloadSpec* out) {
    *out = {};
    if (sig.payKind == 0 || sig.payArg < 0) return false;
    int po = ParamOffset(sig.args, sig.payArg);
    if (po < 0 || po >= nargs || !args[po]) return false;
    out->ptr = args[po];
    uint64_t bytes = 0;
    if (sig.payKind == 1) {
        bytes = static_cast<uint64_t>(sig.n) * ElemBytes(sig.elem);
    } else {
        int co = ParamOffset(sig.args, sig.countArg);
        if (co < 0 || co >= nargs) return false;
        auto count = static_cast<int32_t>(args[co]);
        if (count <= 0) return false;
        bytes = sig.payKind == 2 ? static_cast<uint64_t>(count) * sig.n * ElemBytes(sig.elem) : static_cast<uint64_t>(count);
    }
    if (sig.payKind == 3) {
        out->hashOnly = true;
        out->bytes = static_cast<uint32_t>(std::min<uint64_t>(bytes, 0xFFFFFFFFu));
        return true;
    }
    if (bytes > kMaxPayload) {
        out->capped = true;
        bytes = kMaxPayload - kMaxPayload % ElemBytes(sig.elem);
    }
    out->bytes = static_cast<uint32_t>(bytes);
    return bytes > 0;
}

std::string PayloadJson(char elem, const uint8_t* data, uint32_t bytes) {
    std::string s = "[";
    int eb = ElemBytes(elem);
    for (uint32_t o = 0; o + eb <= bytes; o += eb) {
        if (o) s += ',';
        const uint8_t* p = data + o;
        switch (elem) {
            case 'f': { float v; memcpy(&v, p, 4); AppendDouble(s, v); break; }
            case 'd': { double v; memcpy(&v, p, 8); AppendDouble(s, v); break; }
            case 'i': { int32_t v; memcpy(&v, p, 4); Append(s, "%d", v); break; }
            case 'u': { uint32_t v; memcpy(&v, p, 4); Append(s, "%u", v); break; }
            case 'h': { int16_t v; memcpy(&v, p, 2); Append(s, "%d", v); break; }
            case 'H': { uint16_t v; memcpy(&v, p, 2); Append(s, "%u", v); break; }
            case 'c': Append(s, "%d", static_cast<int8_t>(*p)); break;
            case 'l': { int64_t v; memcpy(&v, p, 8); Append(s, "%lld", static_cast<long long>(v)); break; }
            case 'L': { uint64_t v; memcpy(&v, p, 8); Append(s, "%llu", static_cast<unsigned long long>(v)); break; }
            default: Append(s, "%u", *p); break;
        }
    }
    return s + "]";
}

namespace {
int Components(uint32_t format) {
    switch (format) {
        case 0x1902: case 0x1903: case 0x1904: case 0x1905: case 0x1906: case 0x1909: case 0x1900: case 0x1901:
        case 0x84F9:
            return 1;
        case 0x190A: case 0x8227: return 2;
        case 0x1907: case 0x80E0: return 3;
        case 0x1908: case 0x80E1: return 4;
        default: return 0;
    }
}

// Bytes per pixel for packed types (whole pixel), or per component (negated) for plain ones.
int TypeSize(uint32_t type) {
    switch (type) {
        case 0x1400: case 0x1401: return -1;
        case 0x1402: case 0x1403: case 0x140B: return -2;
        case 0x1404: case 0x1405: case 0x1406: return -4;
        case 0x8032: case 0x8362: return 1;
        case 0x8033: case 0x8034: case 0x8363: case 0x8364: case 0x8365: case 0x8366: return 2;
        case 0x8035: case 0x8036: case 0x8367: case 0x8368: case 0x84FA: case 0x8C3B: case 0x8C3E: return 4;
        default: return 0;
    }
}
}  // namespace

uint64_t PixelBytes(uint32_t format, uint32_t type, int64_t w, int64_t h, int64_t d) {
    int comps = Components(format), ts = TypeSize(type);
    if (!comps || !ts || w <= 0 || h <= 0 || d <= 0) return 0;
    uint64_t bpp = ts > 0 ? static_cast<uint64_t>(ts) : static_cast<uint64_t>(comps) * static_cast<uint64_t>(-ts);
    return bpp * static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * static_cast<uint64_t>(d);
}

const char* SrcName(hub::Src s) {
    switch (s) {
        case hub::Src::ExeIat: return "exe-import";
        case hub::Src::ExeProc: return "exe-proc";
        case hub::Src::CgGLIat: return "cggl-import";
        default: return "cggl-proc";
    }
}

uint8_t SrcBit(hub::Src s) {
    switch (s) {
        case hub::Src::ExeIat: return gltrace::kExeImport;
        case hub::Src::ExeProc: return gltrace::kExeProc;
        case hub::Src::CgGLIat: return gltrace::kCgGLImport;
        default: return gltrace::kCgGLProc;
    }
}

uint32_t Categorize(const char* n) {
    auto starts = [n](const char* p) { return strncmp(n, p, strlen(p)) == 0; };
    auto is = [n](const char* p) { return strcmp(n, p) == 0; };
    uint32_t c = 0;
    if (starts("glDrawArrays") || starts("glDrawElements") || starts("glDrawRangeElements") ||
        starts("glMultiDrawArrays") || starts("glMultiDrawElements"))
        c |= kDraw;
    if (is("glBindProgramARB") || is("glBindProgramNV") || is("glUseProgram") || is("glUseProgramObjectARB"))
        c |= kProgramSwitch;
    if (starts("glNamedProgramLocalParameter") || starts("glProgramLocalParameter") || starts("glProgramEnvParameter") ||
        starts("glNamedProgramEnvParameter"))
        c |= kParamFlush;
    if (is("glBindFramebuffer") || is("glBindFramebufferEXT")) c |= kFboBind;
    if (is("glBindTexture") || is("glBindTextureEXT") || is("glBindMultiTextureEXT") || is("glBindTextures")) c |= kTexBind;
    if ((strstr(n, "TexImage") || strstr(n, "TextureImage") || strstr(n, "TexSubImage") || strstr(n, "TextureSubImage")) &&
        !starts("glGet") && !strstr(n, "CopyTex") && !strstr(n, "CopyTexture") && !strstr(n, "CopyMultiTex"))
        c |= kTexUpload;
    if (is("glGetError")) c |= kGetError;
    return c;
}

std::string CallJson(uint64_t i, const hub::Rec& r, const char* fn, hub::Src src, const std::string* payload) {
    const GlSig* sig = FindSig(fn);
    std::string args, text;
    bool raw = false, truncated = false;
    DecodeArgs(sig, r.a, &args, &text, &raw, &truncated);
    std::string s;
    s.reserve(160 + args.size() + text.size());
    Append(s, "{\"i\":%llu,\"f\":%u,\"fn\":\"%s\",\"src\":\"%s\",\"caller\":\"0x%x\",\"pass\":%u,\"args\":",
           static_cast<unsigned long long>(i), r.frame, fn, SrcName(src), r.caller, r.pass);
    s += args;
    s += ",\"text\":\"";
    for (char c : text) {
        if (c == '"' || c == '\\') s += '\\';
        s += c;
    }
    s += "\",\"payload\":";
    s += payload && !payload->empty() ? *payload : std::string("null");
    if (raw) s += ",\"raw\":true";
    if (truncated) s += ",\"truncated\":true";
    uint64_t bytes = 0;
    if (!strcmp(fn, "glTexImage2D")) bytes = PixelBytes(r.a[6], r.a[7], static_cast<int32_t>(r.a[3]), static_cast<int32_t>(r.a[4]), 1);
    else if (!strcmp(fn, "glTexImage1D")) bytes = PixelBytes(r.a[5], r.a[6], static_cast<int32_t>(r.a[3]), 1, 1);
    else if (!strcmp(fn, "glTexSubImage2D")) bytes = PixelBytes(r.a[6], r.a[7], static_cast<int32_t>(r.a[4]), static_cast<int32_t>(r.a[5]), 1);
    else if (!strcmp(fn, "glTexSubImage1D")) bytes = PixelBytes(r.a[4], r.a[5], static_cast<int32_t>(r.a[3]), 1, 1);
    if (bytes) Append(s, ",\"bytes\":%llu", static_cast<unsigned long long>(bytes));
    Append(s, ",\"tsc\":%llu}", static_cast<unsigned long long>(r.tsc));
    return s;
}

namespace {
uint8_t X5(uint32_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
uint8_t X4(uint32_t v) { return static_cast<uint8_t>(v * 17); }
uint8_t X6(uint32_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }
}  // namespace

bool DecodeUpload(uint32_t format, uint32_t type, int w, int h, int alignment, int rowLength, const uint8_t* src,
                  std::vector<uint8_t>* out, int* channels) {
    if (!src || w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    constexpr uint32_t kUByte = 0x1401, kRGB = 0x1907, kRGBA = 0x1908, kBGR = 0x80E0, kBGRA = 0x80E1, kAlpha = 0x1906,
                       kLum = 0x1909, kLumA = 0x190A, kRed = 0x1903, k1555Rev = 0x8366, k5551 = 0x8034, k565 = 0x8363,
                       k4444 = 0x8033, k4444Rev = 0x8365;
    int bpp = 0, comp = 1;
    if (type == kUByte) {
        bpp = Components(format);
        if (format != kRGB && format != kRGBA && format != kBGR && format != kBGRA && format != kAlpha && format != kLum &&
            format != kLumA && format != kRed)
            return false;
    } else if ((type == k1555Rev || type == k5551 || type == k4444 || type == k4444Rev) && (format == kRGBA || format == kBGRA)) {
        bpp = comp = 2;
    } else if (type == k565 && format == kRGB) {
        bpp = comp = 2;
    } else {
        return false;
    }
    size_t row = static_cast<size_t>(rowLength > 0 ? rowLength : w) * bpp;
    if (alignment > comp) row = (row + alignment - 1) / alignment * alignment;
    int ch = (format == kAlpha || format == kLumA) ? 2 : 4;
    *channels = ch;
    out->assign(static_cast<size_t>(w) * h * ch, 0);
    bool bgr = format == kBGR || format == kBGRA;
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + row * y;
        uint8_t* d = out->data() + static_cast<size_t>(h - 1 - y) * w * ch;
        for (int x = 0; x < w; ++x, d += ch) {
            if (type == kUByte) {
                const uint8_t* p = s + static_cast<size_t>(x) * bpp;
                switch (format) {
                    case kAlpha: d[0] = 255, d[1] = p[0]; break;
                    case kLumA: d[0] = p[0], d[1] = p[1]; break;
                    case kLum: d[0] = d[1] = d[2] = p[0], d[3] = 255; break;
                    case kRed: d[0] = p[0], d[1] = d[2] = 0, d[3] = 255; break;
                    default:
                        d[0] = p[bgr ? 2 : 0], d[1] = p[1], d[2] = p[bgr ? 0 : 2];
                        d[3] = bpp == 4 ? p[3] : 255;
                }
                continue;
            }
            uint32_t v = static_cast<uint32_t>(s[x * 2]) | static_cast<uint32_t>(s[x * 2 + 1]) << 8;
            uint8_t c0, c1, c2, al;
            if (type == k1555Rev) c0 = X5(v & 31), c1 = X5((v >> 5) & 31), c2 = X5((v >> 10) & 31), al = (v >> 15) ? 255 : 0;
            else if (type == k5551) c0 = X5(v >> 11), c1 = X5((v >> 6) & 31), c2 = X5((v >> 1) & 31), al = (v & 1) ? 255 : 0;
            else if (type == k4444) c0 = X4(v >> 12), c1 = X4((v >> 8) & 15), c2 = X4((v >> 4) & 15), al = X4(v & 15);
            else if (type == k4444Rev) c0 = X4(v & 15), c1 = X4((v >> 4) & 15), c2 = X4((v >> 8) & 15), al = X4(v >> 12);
            else c0 = X5(v >> 11), c1 = X6((v >> 5) & 63), c2 = X5(v & 31), al = 255;
            d[0] = bgr ? c2 : c0, d[1] = c1, d[2] = bgr ? c0 : c2, d[3] = al;
        }
    }
    return true;
}

std::string Png8(const uint8_t* px, int w, int h, int channels) {
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(px, w, h, channels, &len, 6, MZ_FALSE);
    if (!png) return {};
    std::string s(static_cast<const char*>(png), len);
    mz_free(png);
    return s;
}

namespace {
void Be32(std::string& s, uint32_t v) {
    for (int k = 3; k >= 0; --k) s += static_cast<char>((v >> (k * 8)) & 0xFF);
}
void Chunk(std::string& png, const char* type, const std::string& data) {
    Be32(png, static_cast<uint32_t>(data.size()));
    std::string body = std::string(type, 4) + data;
    png += body;
    Be32(png, static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const uint8_t*>(body.data()), body.size())));
}
}  // namespace

std::string PngGrey16(const uint16_t* px, int w, int h) {
    if (w <= 0 || h <= 0) return {};
    std::string rows;
    rows.reserve(static_cast<size_t>(h) * (1 + w * 2));
    for (int y = 0; y < h; ++y) {
        rows += '\0';
        for (int x = 0; x < w; ++x) {
            uint16_t v = px[static_cast<size_t>(y) * w + x];
            rows += static_cast<char>(v >> 8);
            rows += static_cast<char>(v & 0xFF);
        }
    }
    mz_ulong zlen = mz_compressBound(static_cast<mz_ulong>(rows.size()));
    std::string z(zlen, '\0');
    if (mz_compress2(reinterpret_cast<unsigned char*>(z.data()), &zlen, reinterpret_cast<const unsigned char*>(rows.data()),
                     static_cast<mz_ulong>(rows.size()), 6) != MZ_OK)
        return {};
    z.resize(zlen);
    std::string png("\x89PNG\r\n\x1a\n", 8), ihdr;
    Be32(ihdr, static_cast<uint32_t>(w));
    Be32(ihdr, static_cast<uint32_t>(h));
    ihdr += std::string("\x10\x00\x00\x00\x00", 5);
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    return png;
}

std::string SafeFileName(const std::string& s) {
    std::string o;
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        o += ok ? c : '_';
        if (o.size() >= 64) break;
    }
    return o;
}
}  // namespace melange::mirage::trace
