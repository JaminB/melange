#include "audio/mixer.h"

#include <windows.h>
#include <xaudio2.h>

#include <atomic>
#include <map>

#include "core/log.h"

namespace melange::audio {
namespace {
constexpr size_t kMaxPooledPerFormat = 8;  // idle source voices kept for reuse, per (channels, rate)
constexpr float kMaxPitch = 2.0f;          // the frequency ratio ceiling every source voice is created with
constexpr size_t kMaxVoicesTotal = 128;    // playing plus stopped-but-unreleased voices: twice the largest MaxVoices
constexpr unsigned kIdleTrimPolls = 1800;  // polls with nothing playing (about 30 s of frames) before idle voices are destroyed

// Only sets a flag: it runs on XAudio2's engine thread.
struct EngineCallback final : IXAudio2EngineCallback {
    std::atomic<bool> lost{false};
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT) override { lost = true; }
};

using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

uint64_t FormatKey(uint16_t channels, uint32_t rate) { return (static_cast<uint64_t>(rate) << 8) | channels; }
}  // namespace

struct Mixer::Impl {
    HMODULE dll = nullptr;
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    EngineCallback cb;
    float masterVolume = 1.0f;

    struct Playing {
        IXAudio2SourceVoice* voice = nullptr;
        uint64_t key = 0;
        std::shared_ptr<const Clip> clip;  // XAudio2 reads clip->pcm until the buffer is released
        bool stopping = false;             // Stop() was called: do not report it as ended
    };
    std::map<uint32_t, Playing> playing;
    std::map<uint64_t, std::vector<IXAudio2SourceVoice*>> idle;
    unsigned quiet = 0;  // consecutive polls with nothing playing

    // Nothing is queued on the voice any more: back to the pool, or destroyed if the pool is full.
    void Recycle(Playing& p) {
        auto& pool = idle[p.key];
        if (pool.size() < kMaxPooledPerFormat) {
            p.voice->Stop();
            pool.push_back(p.voice);
        } else {
            p.voice->DestroyVoice();
        }
        p.voice = nullptr;
        p.clip.reset();
    }

    void Teardown() {
        // DestroyVoice blocks until the engine thread is done with the voice, so the clips can go afterwards.
        for (auto& [id, p] : playing)
            if (p.voice) p.voice->DestroyVoice();
        playing.clear();
        for (auto& [k, v] : idle)
            for (IXAudio2SourceVoice* sv : v) sv->DestroyVoice();
        idle.clear();
        if (master) master->DestroyVoice();
        master = nullptr;
        if (engine) {
            engine->StopEngine();
            engine->Release();
        }
        engine = nullptr;
        quiet = 0;
        cb.lost = false;
    }
};

Mixer::Mixer() : d_(new Impl) {}
Mixer::~Mixer() { Shutdown(); }

bool Mixer::Ready() const { return d_->engine && d_->master; }

bool Mixer::Init(std::string* why) {
    Impl& d = *d_;
    if (Ready()) return true;
    Shutdown();
    // Dynamic on purpose: a Windows without xaudio2_9.dll (older than 10, or Server Core) just has no sound for mods.
    // System32 only: a bare name would also search the game folder, where a stray xaudio2_9.dll would be loaded instead.
    if (!d.dll) d.dll = LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d.dll) {
        if (why) *why = "xaudio2_9.dll not found";
        return false;
    }
    auto create = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(d.dll, "XAudio2Create")));
    if (!create) {
        if (why) *why = "XAudio2Create not found";
        return false;
    }
    HRESULT hr = create(&d.engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !d.engine) {
        d.engine = nullptr;
        if (why) *why = "XAudio2Create failed";
        return false;
    }
    d.engine->RegisterForCallbacks(&d.cb);
    hr = d.engine->CreateMasteringVoice(&d.master);
    if (FAILED(hr) || !d.master) {
        d.master = nullptr;
        d.engine->UnregisterForCallbacks(&d.cb);
        d.engine->Release();
        d.engine = nullptr;
        if (why) *why = "no audio output device";
        return false;
    }
    d.master->SetVolume(d.masterVolume);
    return true;
}

void Mixer::Shutdown() {
    Impl& d = *d_;
    if (d.engine) d.engine->UnregisterForCallbacks(&d.cb);
    d.Teardown();
}

void Mixer::SetMasterVolume(float v) {
    d_->masterVolume = v;
    if (d_->master) d_->master->SetVolume(v);
}

bool Mixer::Start(uint32_t id, std::shared_ptr<const Clip> clip, float volume, float pitch, bool loop, std::string* why) {
    Impl& d = *d_;
    if (!Ready() || !clip) {
        if (why) *why = "audio unavailable";
        return false;
    }
    // Stopped voices do not count towards Active(), so a mod that stops and replays every frame is bounded here.
    if (d.playing.size() >= kMaxVoicesTotal) {
        if (why) *why = "too many voices";
        return false;
    }
    const uint64_t key = FormatKey(clip->channels, clip->rate);
    IXAudio2SourceVoice* v = nullptr;
    auto& pool = d.idle[key];
    if (!pool.empty()) {
        v = pool.back();
        pool.pop_back();
    } else {
        WAVEFORMATEX f = {};
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = clip->channels;
        f.nSamplesPerSec = clip->rate;
        f.wBitsPerSample = 16;
        f.nBlockAlign = static_cast<WORD>(f.nChannels * 2);
        f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
        if (FAILED(d.engine->CreateSourceVoice(&v, &f, 0, kMaxPitch)) || !v) {
            if (why) *why = "cannot create a voice";
            return false;
        }
    }
    v->SetVolume(volume);
    v->SetFrequencyRatio(pitch);
    XAUDIO2_BUFFER b = {};
    b.Flags = XAUDIO2_END_OF_STREAM;
    b.AudioBytes = static_cast<UINT32>(clip->pcm.size());
    b.pAudioData = clip->pcm.data();
    b.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
    if (FAILED(v->SubmitSourceBuffer(&b)) || FAILED(v->Start(0))) {
        v->DestroyVoice();
        if (why) *why = "cannot start the voice";
        return false;
    }
    Impl::Playing p;
    p.voice = v;
    p.key = key;
    p.clip = std::move(clip);
    d.playing[id] = std::move(p);
    return true;
}

void Mixer::SetVolume(uint32_t id, float volume) {
    auto it = d_->playing.find(id);
    if (it != d_->playing.end() && it->second.voice && !it->second.stopping) it->second.voice->SetVolume(volume);
}

void Mixer::Stop(uint32_t id) {
    auto it = d_->playing.find(id);
    if (it == d_->playing.end() || it->second.stopping) return;
    it->second.stopping = true;
    // Stop first so the buffer being played is flushed too; Poll recycles the voice once nothing is queued on it.
    it->second.voice->Stop();
    it->second.voice->FlushSourceBuffers();
    // A stopped voice usually has nothing queued after the flush: release it now, so that stop() then play() in the same
    // frame finds the slot free. If the engine thread still holds the buffer, Poll releases it later.
    XAUDIO2_VOICE_STATE st = {};
    it->second.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (st.BuffersQueued == 0) {
        d_->Recycle(it->second);
        d_->playing.erase(it);
    }
}

// Polled from the Frame event instead of using IXAudio2VoiceCallback::OnStreamEnd: that callback runs on XAudio2's
// own thread, which would then touch the voice pool and drop the clip while the main thread may be starting a voice.
// Polling keeps every voice and buffer operation on the main thread.
void Mixer::Poll(std::vector<uint32_t>* ended) {
    Impl& d = *d_;
    if (!d.engine) return;
    if (d.cb.lost) {
        if (ended)
            for (auto& [id, p] : d.playing)
                if (!p.stopping) ended->push_back(id);
        LOG_WARN("[audio] the output device was lost; sound is off until a mod plays again");
        Shutdown();
        return;
    }
    for (auto it = d.playing.begin(); it != d.playing.end();) {
        XAUDIO2_VOICE_STATE st = {};
        it->second.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued != 0) {
            ++it;
            continue;
        }
        if (!it->second.stopping && ended) ended->push_back(it->first);
        d.Recycle(it->second);
        it = d.playing.erase(it);
    }
    // Pooled voices are for bursts of sounds; after a long silence they are only memory, in the engine and in XAudio2.
    if (!d.playing.empty()) {
        d.quiet = 0;
    } else if (++d.quiet >= kIdleTrimPolls) {
        d.quiet = 0;
        for (auto& [k, v] : d.idle)
            for (IXAudio2SourceVoice* sv : v) sv->DestroyVoice();
        d.idle.clear();
    }
}

// Stopped voices that are not released yet do not count: they are no longer the mod's, and kMaxVoicesTotal bounds them.
size_t Mixer::Active() const {
    size_t n = 0;
    for (const auto& [id, p] : d_->playing)
        if (!p.stopping) ++n;
    return n;
}
}  // namespace melange::audio
