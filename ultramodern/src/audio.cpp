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
    // The submitted PAYLOAD, exactly as the host copy will read it
    // (XOR-3 host layout resolved to wire order). Captured because the
    // 2026-07-02 crackle isolation showed the bytes at submission time
    // can differ from what the RSP task wrote (one-slice corruption
    // windows); this ring makes submitted-vs-synthesized diffs direct.
    uint32_t data_len;    // min(byte_count, sizeof(data))
    uint32_t pad_;
    uint8_t  data[2208];
};
constexpr size_t AI_SUBMIT_RING_CAP = 8192;   // x ~2.2 KiB = ~18 MiB
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

// Dump all resident submissions (oldest -> newest) as raw AiSubmitEvent
// records, payloads included. Returns records written or -1.
extern "C" int64_t ultramodern_ai_submit_dump(const char* path) {
    std::lock_guard<std::mutex> lk(g_ai_submit_mtx);
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    const uint64_t s = g_ai_submit_seq.load(std::memory_order_relaxed);
    const size_t available =
        (s < AI_SUBMIT_RING_CAP) ? size_t(s) : AI_SUBMIT_RING_CAP;
    const size_t start = (s - available) % AI_SUBMIT_RING_CAP;
    int64_t written = 0;
    for (size_t i = 0; i < available; i++) {
        const AiSubmitEvent& e =
            g_ai_submit_ring[(start + i) % AI_SUBMIT_RING_CAP];
        if (fwrite(&e, sizeof(e), 1, f) == 1) written++;
    }
    fclose(f);
    return written;
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

// ── Virtual AI DAC (hardware-shaped AI_LEN) ─────────────────────────
// The hardware AI_LEN register drains sample-by-sample at the DAC rate
// between buffer submissions. Deriving the reported value from the host
// buffer level instead quantizes it in audio-callback chunks (~10-21 ms
// stair-steps) and adds host scheduling noise. Games that pace their
// audio-frame SIZES from this register (Stadium: samplesLeft >= 0x1A9
// picks a 368- vs 552-sample task) turn that noise into an unstable
// task cadence — and game code whose own producer/consumer margins are
// tuned to the hardware limit cycle (Stadium's CPU-streamed announcer/
// crowd rings in n_mainbus, ~2 frames deep) splices stale samples under
// the jitter: the battle static.
//
// Model the register directly instead: submissions add their byte count
// at latch time; a steady clock drains at exactly the DAC byte rate. A
// very slow trim (bounded at ±0.05%, inaudible) servos the virtual rate
// against the real host buffer level so the two clocks cannot drift
// apart over long sessions. N64MR_AI_VIRTUAL=0 restores the host-level
// readback for A/B.
namespace {
struct VirtualAiDac {
    std::mutex mtx;
    double remaining_bytes = 0.0;
    double rate_scale = 1.0;
    bool ever_submitted = false;
    std::chrono::steady_clock::time_point last =
        std::chrono::steady_clock::now();

    void drain_locked(uint32_t rate) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.5) dt = 0.5;   // host stall: don't teleport
        remaining_bytes -= dt * (double)rate * 4.0 * rate_scale;
        if (remaining_bytes < 0.0) remaining_bytes = 0.0;
    }
};
VirtualAiDac g_vai;

bool ai_virtual_enabled() {
    static const bool v = [] {
        const char* e = getenv("N64MR_AI_VIRTUAL");
        return !(e && *e == '0' && e[1] == '\0');   // default ON
    }();
    return v;
}
}  // namespace

// ── Deferred AI buffer consumption (hardware AI timing) ─────────────
// On real hardware osAiSetNextBuffer only latches address+length; the
// AI DMA reads the bytes gradually over the FOLLOWING ~17 ms. Games
// legally exploit that: Stadium submits the buffer while the RSP task
// filling it is still running (measured 2026-07-02: the task completes
// ~1 ms AFTER the submission in 2347/3706 frames). An instant snapshot
// copy therefore reads the buffer's previous-rotation content — and
// occasionally fresh or torn content when the race lands the other way.
// Every old<->new transition splices the waveform: that was the
// perpetual crackle.
//
// Fix: consume like the hardware — latch (addr, len) now, copy the
// bytes when the NEXT buffer is submitted (one AI frame later, by which
// time the writer is provably done). N64MR_AI_INSTANT_COPY=1 restores
// the old behavior for A/B.
namespace {
struct PendingAiBuffer {
    uint32_t addr = 0;
    uint32_t bytes = 0;
    bool valid = false;
};
PendingAiBuffer g_ai_pending;

bool ai_instant_copy() {
    static const bool v = [] {
        const char* e = getenv("N64MR_AI_INSTANT_COPY");
        return e && *e && *e != '0';
    }();
    return v;
}

void ai_play_buffer(uint8_t* rdram, uint32_t guest_addr, uint32_t byte_count) {
    uint32_t sample_count = byte_count / sizeof(int16_t);

    // Record the played buffer (address + size + payload) in the
    // always-on ring before handing the samples to the host layer.
    {
        std::lock_guard<std::mutex> lk(g_ai_submit_mtx);
        const uint64_t s = g_ai_submit_seq.load(std::memory_order_relaxed);
        AiSubmitEvent& e = g_ai_submit_ring[s % AI_SUBMIT_RING_CAP];
        e.seq = s;
        e.ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_ai_submit_t0).count();
        e.guest_addr = guest_addr;
        e.byte_count = byte_count;
        uint32_t want = byte_count;
        if (want > sizeof(e.data)) want = sizeof(e.data);
        const uint32_t paddr = guest_addr & 0xFFFFFFu;
        if ((uint64_t)paddr + want > 0x800000u) want = 0;
        for (uint32_t i = 0; i < want; i++) {
            e.data[i] = rdram[(paddr + i) ^ 3];   // host XOR-3 -> wire order
        }
        e.data_len = want;
        e.pad_ = 0;
        g_ai_submit_seq.store(s + 1, std::memory_order_release);
    }

    if (sample_count > 0 && audio_callbacks.queue_samples) {
        audio_callbacks.queue_samples(
            TO_PTR(int16_t, (PTR(int16_t))guest_addr), sample_count);
    }
}
}  // namespace

void ultramodern::queue_audio_buffer(RDRAM_ARG PTR(int16_t) audio_data_, uint32_t byte_count) {
    // Ensure that the byte count is an integer multiple of samples.
    assert((byte_count & 1) == 0);

    // Hardware latches (addr, len) into the AI at submission time — that
    // is when AI_LEN jumps up, regardless of when we copy the payload.
    if (ai_virtual_enabled() && byte_count > 0) {
        std::lock_guard<std::mutex> lk(g_vai.mtx);
        g_vai.drain_locked(sample_rate);
        g_vai.remaining_bytes += (double)byte_count;
        g_vai.ever_submitted = true;
    }

    if (ai_instant_copy()) {
        ai_play_buffer(rdram, (uint32_t)audio_data_, byte_count);
        return;
    }

    // Play the PREVIOUS submission's buffer now — its writer finished at
    // least one full AI frame ago — then latch this one.
    if (g_ai_pending.valid) {
        ai_play_buffer(rdram, g_ai_pending.addr, g_ai_pending.bytes);
    }
    g_ai_pending.addr = (uint32_t)audio_data_;
    g_ai_pending.bytes = byte_count;
    g_ai_pending.valid = byte_count > 0;
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

    if (ai_virtual_enabled()) {
        std::lock_guard<std::mutex> lk(g_vai.mtx);
        if (g_vai.ever_submitted) {
            // Host reporting empty means either fast-forward (the runner
            // deliberately reports 0 to unthrottle the game) or a genuinely
            // dry pipeline (boot, scene stall): mirror it so those modes
            // keep working.
            if (buffered_byte_count == 0) {
                g_vai.remaining_bytes = 0.0;
                return 0;
            }
            // Slow trim: hold the host level near its current cushion by
            // biasing the virtual drain rate a hair. err > 0 (host holding
            // more than ~15 ms) => drain slightly faster so the game
            // produces less, and vice versa.
            double err_bytes = (double)buffered_byte_count -
                               0.015 * (double)sample_rate * 4.0;
            double trim = err_bytes * 1e-8;
            if (trim > 5e-4) trim = 5e-4;
            if (trim < -5e-4) trim = -5e-4;
            g_vai.rate_scale = 1.0 + trim;
            g_vai.drain_locked(sample_rate);
            return (uint32_t)g_vai.remaining_bytes & ~1u;
        }
    }

    // Take off the lead cushion (buffer_offset_frames worth of one VI's samples,
    // converted to bytes), clamping at zero so the count never underflows.
    uint32_t samples_per_vi = sample_rate / 60;
    uint32_t cushion_bytes = static_cast<uint32_t>(buffer_offset_frames * sizeof(int16_t) * samples_per_vi);
    buffered_byte_count = (buffered_byte_count > cushion_bytes)
        ? (buffered_byte_count - cushion_bytes)
        : 0;

    return buffered_byte_count;
}
