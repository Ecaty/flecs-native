#include "flecs_unity.h"
#define TSF_IMPLEMENTATION
#include "tsf.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

// Synthesizer voices (TinySoundFont plus a sine/noise voice). One renderer per emitter: the main thread queues note
// events, the audio thread renders. It does not touch the Flecs world.
namespace {

struct AudioEvent {
    int kind, note;
    float velocity;
    int program;
    uint64_t sample;
};

std::mutex font_gate;

struct SoundBank {
    tsf* font;
    explicit SoundBank(tsf* f) : font(f) {}
    ~SoundBank() { tsf_close(font); }
};
// Immutable sample data is shared between emitters that loaded the same bank bytes.
std::unordered_map<uint64_t, std::weak_ptr<SoundBank>> banks;

struct Audio {
    std::shared_ptr<SoundBank> bank;
    tsf* font = nullptr;
    int rate;
    uint64_t position = 0;
    uint32_t rng = 0x12345678;
    std::array<AudioEvent, 256> events{}, pending{};
    int pending_count = 0;
    std::atomic<uint32_t> write{0}, read{0};
    int oscillator = 0;
    float gain = 0, frequency = 440, phase = 0;
    uint64_t end = 0;
    Audio(std::shared_ptr<SoundBank> b, tsf* f, int r) : bank(std::move(b)), font(f), rate(r) {}
    ~Audio() {
        std::lock_guard<std::mutex> lock(font_gate);
        if (font) tsf_close(font);
        bank.reset();
    }
};

} // namespace

extern "C" {

FU_API void* fu_audio_create(const void* bank, int bytes, int rate) {
    if (!bank || bytes < 16 || rate < 8000 || rate > 192000) return nullptr;
    uint64_t key = 1469598103934665603ull;
    const auto* data = static_cast<const unsigned char*>(bank);
    for (int i = 0; i < bytes; ++i) key = (key ^ data[i]) * 1099511628211ull;
    std::lock_guard<std::mutex> lock(font_gate);
    auto shared = banks[key].lock();
    if (!shared) {
        tsf* source = tsf_load_memory(bank, bytes);
        if (!source) return nullptr;
        shared = std::make_shared<SoundBank>(source);
        banks[key] = shared;
    }
    // Each emitter keeps its own preallocated voices and channels.
    tsf* font = tsf_copy(shared->font);
    if (!font) return nullptr;
    tsf_set_output(font, TSF_MONO, rate, 0);
    tsf_set_max_voices(font, 64);
    tsf_channel_set_presetnumber(font, 0, 0, 0);
    return new Audio(shared, font, rate);
}

FU_API void fu_audio_destroy(void* renderer) { delete static_cast<Audio*>(renderer); }

// Single main-thread producer, one audio-thread consumer. Times are absolute renderer samples.
FU_API int fu_audio_note(void* renderer, int kind, int note, float velocity, int program, uint64_t sample) {
    auto* audio = static_cast<Audio*>(renderer);
    if (!audio || kind < 0 || kind > 4 || note < 0 || note > 127 || !std::isfinite(velocity)) return -1;
    const auto write = audio->write.load(std::memory_order_relaxed), read = audio->read.load(std::memory_order_acquire);
    if (write - read >= audio->events.size()) return -2;
    audio->events[write % audio->events.size()] = {kind, note, std::clamp(velocity, 0.f, 1.f), program, sample};
    audio->write.store(write + 1, std::memory_order_release);
    return 0;
}

FU_API void fu_audio_render(void* renderer, float* output, int count) {
    auto* audio = static_cast<Audio*>(renderer);
    if (!audio || !output || count < 0 || count > 1048576) return;
    for (int offset = 0; offset < count;) {
        // Move queued events into the time-ordered pending list.
        auto read = audio->read.load(std::memory_order_relaxed);
        const auto write = audio->write.load(std::memory_order_acquire);
        while (read != write && audio->pending_count < 256) {
            const auto event = audio->events[read % audio->events.size()];
            int slot = audio->pending_count++;
            while (slot > 0 && audio->pending[slot - 1].sample > event.sample) {
                audio->pending[slot] = audio->pending[slot - 1];
                --slot;
            }
            audio->pending[slot] = event;
            audio->read.store(++read, std::memory_order_release);
        }
        // Start every event that is due. kind: 0 note on, 1 note off, 2 all off, 3 sine, 4 noise.
        while (audio->pending_count && audio->pending[0].sample <= audio->position) {
            const auto event = audio->pending[0];
            for (int k = 1; k < audio->pending_count; ++k) audio->pending[k - 1] = audio->pending[k];
            --audio->pending_count;
            if (event.kind == 0) {
                tsf_channel_set_presetnumber(audio->font, 0, std::clamp(event.program, 0, 127), 0);
                tsf_channel_note_on(audio->font, 0, event.note, event.velocity);
            } else if (event.kind == 1) {
                tsf_channel_note_off(audio->font, 0, event.note);
            } else if (event.kind == 2) {
                tsf_note_off_all(audio->font);
                audio->gain = 0;
            } else {
                audio->oscillator = event.kind;
                audio->frequency = 440.f * std::pow(2.f, (event.note - 69) / 12.f);
                audio->gain = event.velocity * .25f;
                audio->end = audio->position + static_cast<uint64_t>(audio->rate * .3);
            }
        }
        // Render up to the next pending event, at most 64 samples at a time.
        int block = std::min(64, count - offset);
        if (audio->pending_count) {
            const auto next = audio->pending[0].sample;
            if (next > audio->position) block = static_cast<int>(std::min<uint64_t>(block, next - audio->position));
        }
        tsf_render_float(audio->font, output + offset, block, 0);
        for (int i = 0; i < block; ++i) {
            if (audio->position + static_cast<uint64_t>(i) < audio->end && audio->gain > 0) {
                audio->rng ^= audio->rng << 13;
                audio->rng ^= audio->rng >> 17;
                audio->rng ^= audio->rng << 5;
                const float wave = audio->oscillator == 4
                    ? static_cast<float>(static_cast<int32_t>(audio->rng)) / 2147483648.f : std::sin(audio->phase);
                const float envelope = static_cast<float>(audio->end - audio->position - i) / (audio->rate * .3f);
                output[offset + i] += wave * audio->gain * envelope;
                audio->phase += 6.283185307f * audio->frequency / audio->rate;
            }
            output[offset + i] = std::clamp(output[offset + i], -1.f, 1.f);
        }
        offset += block;
        audio->position += block;
    }
}

}
