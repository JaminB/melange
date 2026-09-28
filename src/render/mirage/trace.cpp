// Module "MirageTrace": GL trace, stats, frame capture and texture dumper (stub).
#include "core/module.h"
#include "melange/gltrace.h"

namespace {
class MirageTrace final : public melange::Module {
public:
    const char* Name() const override { return "MirageTrace"; }
    const char* Description() const override { return "GL trace, stats panel, frame capture, texture dumper"; }
    int Order() const override { return 41; }
    bool Install() override { return true; }
};
}  // namespace

MELANGE_MODULE(MirageTrace);

namespace melange::gltrace {
bool Installed() { return false; }
Mode GetMode() { return Mode::Off; }
void SetMode(Mode) {}
FrameStats Last() { return {}; }
FrameStats Average(uint32_t) { return {}; }
size_t Top(FnStat*, size_t) { return 0; }
uint32_t ProcsHandedOut() { return 0; }
bool RequestCapture(const CaptureOptions&) { return false; }
CaptureState CaptureStatus(std::wstring*, std::string*) { return CaptureState::Idle; }
bool StartTextureDump(uint32_t, const char*) { return false; }
void StopTextureDump() {}
uint32_t TexturesDumped() { return 0; }
}  // namespace melange::gltrace
