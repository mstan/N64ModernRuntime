#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <mutex>

static uint32_t sample_rate = 48000;

// ── Always-on AI-submission ring ────────────────────────────────────
// Records every audio buffer the game submits (guest address + byte
// count) so played PCM can be attributed to the guest memory — and
// therefore to the RSP task outputs — that produced it. Without this,
// host-side PCM observations cannot be aligned with guest-side
// synthesis records (the audio pipeline synthesizes ahead and out of
// order relative to playback). Queried backward via the runner's debug
// server; never armed.
namespace {
struct AiSubmitEvent {
    uint64_t seq;
    uint64_t ms;          // steady-clock ms since ring init
    uint32_t guest_addr;  // address passed to queue_audio_buffer
    uint32_t byte_count;
};
constexpr size_t AI_SUBMIT_RING_CAP = 65536;
AiSubmitEvent g_ai_submit_ring[AI_SUBMIT_RING_CAP];
std::atomic<uint64_t> g_ai_submit_seq{0};
std::mutex g_ai_submit_mtx;
std::chrono::steady_clock::time_point g_ai_submit_t0 =
    std::chrono::steady_clock::now();
}  // namespace

extern "C" void ultramodern_ai_submit_recent_copy(
    void* out_void, size_t cap, size_t* n_written, uint64_t* next_seq_out)
{
    std::lock_guard<std::mutex> lk(g_ai_submit_mtx);
    const uint64_t s = g_ai_submit_seq.load(std::memory_order_relaxed);
    if (next_seq_out) *next_seq_out = s;
    if (cap == 0 || out_void == nullptr) {
        if (n_written) *n_written = 0;
        return;
    }
    const size_t available = (s < AI_SUBMIT_RING_CAP) ? size_t(s) : AI_SUBMIT_RING_CAP;
    const size_t want = (cap < available) ? cap : available;
    AiSubmitEvent* out = static_cast<AiSubmitEvent*>(out_void);
    const size_t start = (s - want) % AI_SUBMIT_RING_CAP;
    for (size_t i = 0; i < want; i++)
        out[i] = g_ai_submit_ring[(start + i) % AI_SUBMIT_RING_CAP];
    if (n_written) *n_written = want;
}

extern "C" size_t ultramodern_ai_submit_event_size(void) {
    return sizeof(AiSubmitEvent);
}

static ultramodern::audio_callbacks_t audio_callbacks;

void ultramodern::set_audio_callbacks(const ultramodern::audio_callbacks_t& callbacks) {
    audio_callbacks = callbacks;
}

void ultramodern::init_audio() {
    // Pick an initial dummy sample rate; this will be set by the game later to the true sample rate.
    set_audio_frequency(48000);
}

void ultramodern::set_audio_frequency(uint32_t freq) {
    if (audio_callbacks.set_frequency) {
        audio_callbacks.set_frequency(freq);
    }
    sample_rate = freq;
}

void ultramodern::queue_audio_buffer(RDRAM_ARG PTR(int16_t) audio_data_, uint32_t byte_count) {
    // Ensure that the byte count is an integer multiple of samples.
    assert((byte_count & 1) == 0);

    // Calculate the number of samples from the number of bytes.
    uint32_t sample_count = byte_count / sizeof(int16_t);

    // Record the submission (address + size) in the always-on ring
    // before handing the samples to the host layer.
    {
        std::lock_guard<std::mutex> lk(g_ai_submit_mtx);
        const uint64_t s = g_ai_submit_seq.load(std::memory_order_relaxed);
        AiSubmitEvent& e = g_ai_submit_ring[s % AI_SUBMIT_RING_CAP];
        e.seq = s;
        e.ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_ai_submit_t0).count();
        e.guest_addr = (uint32_t)audio_data_;
        e.byte_count = byte_count;
        g_ai_submit_seq.store(s + 1, std::memory_order_release);
    }

    // Queue the swapped audio data.
    if (sample_count > 0 && audio_callbacks.queue_samples) {
        audio_callbacks.queue_samples(TO_PTR(int16_t, audio_data_), sample_count);
    }
}

// How far ahead of the true host buffer level to report, in frames. Titles size
// their next batch of generated samples from this number: too high tends to pop,
// too low tends to lag, so a fractional-frame lead keeps a cushion against audio-
// thread jitter without overcommitting.
float buffer_offset_frames = 0.5f;

// First place to look if audio ever pops or lags (see the note on the lead above).
uint32_t ultramodern::get_remaining_audio_bytes() {
    // Bytes still queued in the host audio buffer — two int16 channels per frame.
    // When the host can't report a level, fall back to a small nonzero estimate.
    uint32_t buffered_byte_count = (audio_callbacks.get_frames_remaining != nullptr)
        ? audio_callbacks.get_frames_remaining() * 2 * sizeof(int16_t)
        : 100;

    // Take off the lead cushion (buffer_offset_frames worth of one VI's samples,
    // converted to bytes), clamping at zero so the count never underflows.
    uint32_t samples_per_vi = sample_rate / 60;
    uint32_t cushion_bytes = static_cast<uint32_t>(buffer_offset_frames * sizeof(int16_t) * samples_per_vi);
    buffered_byte_count = (buffered_byte_count > cushion_bytes)
        ? (buffered_byte_count - cushion_bytes)
        : 0;

    return buffered_byte_count;
}
