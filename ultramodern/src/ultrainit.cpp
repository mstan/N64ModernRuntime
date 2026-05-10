// Modifications in this file are part of N64ModernRuntime (GPL-3.0; see
// COPYING). Original file copyright remains with the upstream
// N64ModernRuntime authors.
//
// Modified 2026 by Matthew Stanley:
//   - Spawn ultramodern::init_scheduler_tick at end of preinit so the
//     no-arg ultramodern_scheduler_tick() called from generated code at
//     function entry can pass rdram through to ultramodern primitives.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

void ultramodern::set_callbacks(
    const rsp::callbacks_t& rsp_callbacks,
    const renderer::callbacks_t& renderer_callbacks,
    const audio_callbacks_t& audio_callbacks,
    const input::callbacks_t& input_callbacks,
    const gfx_callbacks_t& gfx_callbacks,
    const events::callbacks_t& events_callbacks,
    const error_handling::callbacks_t& error_handling_callbacks,
    const threads::callbacks_t& threads_callbacks
) {
    ultramodern::rsp::set_callbacks(rsp_callbacks);
    ultramodern::renderer::set_callbacks(renderer_callbacks);
    ultramodern::set_audio_callbacks(audio_callbacks);
    ultramodern::input::set_callbacks(input_callbacks);
    (void)gfx_callbacks; // nothing yet
    ultramodern::events::set_callbacks(events_callbacks);
    ultramodern::error_handling::set_callbacks(error_handling_callbacks);
    ultramodern::threads::set_callbacks(threads_callbacks);
}

void ultramodern::preinit(RDRAM_ARG ultramodern::renderer::WindowHandle window_handle) {
    ultramodern::set_main_thread();
    ultramodern::init_events(PASS_RDRAM window_handle);
    ultramodern::init_timers(PASS_RDRAM1);
    ultramodern::init_audio();
    ultramodern::init_thread_cleanup();
    ultramodern::init_scheduler_tick(PASS_RDRAM1);
}

extern "C" void osInitialize() {
}
