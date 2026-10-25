// The media host: module [Media], players, the helper and the frame upload (media.h).
// Placeholder from Melange 0.12.0 C0: no player ever opens. C4b replaces this file.
#include "media/media.h"

#include "core/module.h"
#include "media/config_keys.h"

namespace melange::media {
void OnPresent() {}

bool FrameForDraw(const char*, uint32_t, DrawFrame*) { return false; }

namespace {
class Media final : public Module {
public:
    const char* Name() const override { return "Media"; }
    const char* Description() const override { return "Watch-party media: the media pack, the helper process, video frames"; }
    int Order() const override { return 60; }  // after the Sandbox (50)
    bool Install() override {
        cfg::ReadMedia();  // documents the [Media] keys in a fresh ini
        return true;
    }
};
}  // namespace
MELANGE_MODULE(Media);
}  // namespace melange::media
