// Module "MirageDraw": world and HUD drawing API (stub).
#include "core/module.h"
#include "melange/draw.h"

namespace {
class MirageDraw final : public melange::Module {
public:
    const char* Name() const override { return "MirageDraw"; }
    const char* Description() const override { return "draw API: world and HUD primitives, text"; }
    int Order() const override { return 44; }
    bool Install() override { return true; }
};
}  // namespace

MELANGE_MODULE(MirageDraw);

namespace melange::draw {
void Line(const float[3], const float[3], Rgba, float, uint32_t, int) {}
void Box(const float[3], const float[3], Rgba, float, uint32_t, int) {}
void Sphere(const float[3], float, Rgba, float, uint32_t, int) {}
void Axes(const float[3], float, float, uint32_t, int) {}
void Quad(const float[4][3], Rgba, uint32_t, int) {}
void Text(const float[3], const char*, Rgba, float, uint32_t, int) {}
void Mesh(const Vertex*, uint32_t, const uint16_t*, uint32_t, const float*, unsigned, uint32_t, int) {}
void HudLine(float, float, float, float, Rgba, float, int) {}
void HudRect(float, float, float, float, Rgba, bool, float, int) {}
void HudText(float, float, const char*, Rgba, float, int) {}
void HudImage(float, float, float, float, unsigned, Rgba, int) {}
unsigned LoadTexture(const wchar_t*) { return 0; }
void FreeTexture(unsigned) {}
int AddDrawCallback(render::Stage, DrawFn, void*, int) { return 0; }
void RemoveDrawCallback(int) {}
Stats GetStats() { return {}; }
}  // namespace melange::draw
