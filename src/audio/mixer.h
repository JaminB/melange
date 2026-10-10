#pragma once
// XAudio2 2.9 backend, loaded at run time. Ready() is false when xaudio2_9.dll or an output device is missing, and the
// caller then reports "audio unavailable". Main thread only: the one thing that runs on XAudio2's own thread is the
// engine's critical-error callback, which only sets a flag.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/clips.h"

namespace melange::audio {
class Mixer {
public:
    Mixer();
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    bool Init(std::string* why);  // loads the DLL, creates the engine and the mastering voice
    void Shutdown();              // destroys every voice; safe to call twice
    bool Ready() const;
    void SetMasterVolume(float v);

    // Starts the clip as voice `id`. The mixer keeps a reference to the clip until the engine has let go of the buffer.
    bool Start(uint32_t id, std::shared_ptr<const Clip> clip, float volume, float pitch, bool loop, std::string* why);
    void SetVolume(uint32_t id, float volume);
    void Stop(uint32_t id);  // stops and flushes; the voice is recycled by a later Poll once the buffer is released

    // Recycles voices whose buffers have played out or been flushed. Ids that ended on their own (not through Stop) are
    // appended to *ended. If the output device was lost the engine is shut down and every voice counts as ended; Ready()
    // is then false until Init succeeds again.
    void Poll(std::vector<uint32_t>* ended);
    size_t Active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
}  // namespace melange::audio
