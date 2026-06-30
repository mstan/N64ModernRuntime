#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include <cassert>

static uint32_t sample_rate = 48000;

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
