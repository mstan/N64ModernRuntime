// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with upstream authors. See COPYING.
//
// Modified 2026 by Matthew Stanley:
//   - osSpTaskStartGo ring + sp/dp_complete counters for runner-side
//     diagnostics (commit 4e0c9fe).
//   - Submit-task counters surfaced via ultramodern_submit_*_count
//     (commit dd8137d).
//   - Framework-level libultra ring + RSP watchdog + boot fixes for
//     PokemonStadium boot sequencing.
//   - Publish HLE SP task-completion status for CPU-side SP_STATUS polling.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include <thread>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <variant>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <queue>
#include <cstring>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#include "ultramodern/rsp.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultra_trace.hpp"

static ultramodern::events::callbacks_t events_callbacks{};

void ultramodern::events::set_callbacks(const ultramodern::events::callbacks_t& callbacks) {
    events_callbacks = callbacks;
}

struct SpTaskAction {
    OSTask task;
};

struct ScreenUpdateAction {
    ultramodern::renderer::ViRegs regs;
};

struct UpdateConfigAction {
};

using Action = std::variant<SpTaskAction, ScreenUpdateAction, UpdateConfigAction>;

struct ViState {
    const OSViMode* mode;
    PTR(void) framebuffer;
    PTR(OSMesg) mq;
    OSMesg msg;
    uint32_t state;
    uint32_t control;
    int retrace_count = 1;
};

#define VI_STATE_BLACK 0x20
#define VI_STATE_REPEATLINE 0x40

static struct {
    struct {
        std::thread thread;
        int cur_state;
        int field;
        ViState states[2];
        ultramodern::renderer::ViRegs regs;
        ultramodern::renderer::ViRegs update_screen_regs;

        ViState* get_next_state() {
            return &states[cur_state ^ 1];
        }
        ViState* get_cur_state() {
            return &states[cur_state];
        }
        void update_vi() {
            ViState* next_state = get_next_state();
            const OSViMode* next_mode = next_state->mode;
            const OSViCommonRegs* common_regs = &next_mode->comRegs;
            const OSViFieldRegs* field_regs = &next_mode->fldRegs[field];
            PTR(void) framebuffer = osVirtualToPhysical(next_state->framebuffer);
            PTR(void) origin = framebuffer + field_regs->origin;

            // Process the VI state flags.
            uint32_t hStart = common_regs->hStart;
            if (next_state->state & VI_STATE_BLACK) {
                hStart = 0;
            }

            uint32_t yScale = field_regs->yScale;
            if (next_state->state & VI_STATE_REPEATLINE) {
                // libultra implements osViRepeatLine by writing a zero VI_Y_SCALE
                // and pointing VI_ORIGIN at the framebuffer start. RT64 treats a
                // zero y scale as a zero-height VI, so preserve the normal scale
                // until repeated-scanline presentation is implemented there.
                origin = framebuffer;
            }

            // TODO implement osViFade

            // Update VI registers.
            regs.VI_ORIGIN_REG = origin;
            regs.VI_WIDTH_REG = common_regs->width;
            regs.VI_TIMING_REG = common_regs->burst;
            regs.VI_V_SYNC_REG = common_regs->vSync;
            regs.VI_H_SYNC_REG = common_regs->hSync;
            regs.VI_LEAP_REG = common_regs->leap;
            regs.VI_H_START_REG = hStart;
            regs.VI_V_START_REG = field_regs->vStart; // TODO implement osViExtendVStart
            regs.VI_V_BURST_REG = field_regs->vBurst;
            regs.VI_INTR_REG = field_regs->vIntr;
            regs.VI_X_SCALE_REG = common_regs->xScale; // TODO implement osViSetXScale
            regs.VI_Y_SCALE_REG = yScale; // TODO implement osViSetYScale
            regs.VI_STATUS_REG = next_state->control;
            
            // Swap VI states.
            cur_state ^= 1;
            *get_next_state() = *get_cur_state();
        }
    } vi;
    struct {
        std::thread gfx_thread;
        std::thread task_thread;
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } sp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } dp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } ai;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } si;
    // The same message queue may be used for multiple events, so share a mutex for all of them
    std::mutex message_mutex;
    uint8_t* rdram;
    moodycamel::BlockingConcurrentQueue<Action> action_queue{};
    moodycamel::BlockingConcurrentQueue<OSTask*> sp_task_queue{};
    moodycamel::ConcurrentQueue<OSThread*> deleted_threads{};
} events_context{};

ultramodern::renderer::ViRegs* ultramodern::renderer::get_vi_regs() {
    return &events_context.vi.update_screen_regs;
}

extern "C" void osSetEventMesg(RDRAM_ARG OSEvent event_id, PTR(OSMesgQueue) mq_, OSMesg msg) {
    std::lock_guard lock{ events_context.message_mutex };

    switch (event_id) {
        case OS_EVENT_SP:
            events_context.sp.msg = msg;
            events_context.sp.mq = mq_;
            break;
        case OS_EVENT_DP:
            events_context.dp.msg = msg;
            events_context.dp.mq = mq_;
            break;
        case OS_EVENT_AI:
            events_context.ai.msg = msg;
            events_context.ai.mq = mq_;
            break;
        case OS_EVENT_SI:
            events_context.si.msg = msg;
            events_context.si.mq = mq_;
    }
}

extern "C" void osViSetEvent(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, u32 retrace_count) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mq = mq_;
    next_state->msg = msg;
    next_state->retrace_count = retrace_count;
}

uint64_t total_vis = 0;


extern std::atomic_bool exited;
extern moodycamel::LightweightSemaphore graphics_shutdown_ready;

void set_dummy_vi(bool odd);

void vi_thread_func() {
    ultramodern::set_native_thread_name("VI Thread");
    // This thread should be prioritized over every other thread in the application, as it's what allows
    // the game to generate new audio and gfx lists.
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Critical);
    using namespace std::chrono_literals;

    int remaining_retraces = 1;

    while (!exited) {
        // Determine the next VI time (more accurate than adding 16ms each VI interrupt)
        auto next = ultramodern::get_start() + (total_vis * 1000000us) / (60 * ultramodern::get_speed_multiplier());
        //if (next > std::chrono::high_resolution_clock::now()) {
        //    printf("Sleeping for %" PRIu64 " us to get from %" PRIu64 " us to %" PRIu64 " us \n",
        //        (next - std::chrono::high_resolution_clock::now()) / 1us,
        //        (std::chrono::high_resolution_clock::now() - events_context.start) / 1us,
        //        (next - events_context.start) / 1us);
        //} else {
        //    printf("No need to sleep\n");
        //}
        // Detect if there's more than a second to wait and wait a fixed amount instead for the next VI if so, as that usually means the system clock went back in time.
        if (std::chrono::floor<std::chrono::seconds>(next - std::chrono::high_resolution_clock::now()) > 1s) {
            // printf("Skipping the next VI wait\n");
            next = std::chrono::high_resolution_clock::now();
        }
        ultramodern::sleep_until(next);
        auto time_now = ultramodern::time_since_start();
        // Calculate how many VIs have passed
        uint64_t new_total_vis = (time_now * (60 * ultramodern::get_speed_multiplier()) / 1000ms) + 1;
        if (new_total_vis > total_vis + 1) {
            //printf("Skipped % " PRId64 " frames in VI interupt thread!\n", new_total_vis - total_vis - 1);
        }
        total_vis = new_total_vis;

        // If the game hasn't started yet, OR has been marked started but
        // hasn't yet called osViSetMode (mode pointer still null), install
        // a dummy VI mode. The original guard (only when !is_game_started)
        // was racy: a runner that sets game_status=Running BEFORE the
        // game thread reaches its first osViSetMode call would leave
        // next_state->mode==NULL and crash update_vi() at the
        // &next_state->mode->comRegs deref.
        {
            static bool odd = false;
            ViState* next_state = events_context.vi.get_next_state();
            if (!ultramodern::is_game_started() || next_state->mode == nullptr) {
                set_dummy_vi(odd);
                odd = !odd;
            }
        }

        // Queue a screen update for the graphics thread with the current VI register state.
        // Doing this before the VI update is equivalent to updating the screen after the previous frame's scanout finished.
        events_context.action_queue.enqueue(ScreenUpdateAction{ events_context.vi.regs });

        // Update VI registers and swap VI modes.
        events_context.vi.update_vi();

        // If the game has started, handle sending VI and AI events.
        if (ultramodern::is_game_started()) {
            remaining_retraces--;

            uint8_t* rdram = events_context.rdram;
            std::lock_guard lock{ events_context.message_mutex };
            ViState* cur_state = events_context.vi.get_cur_state();
            if (remaining_retraces == 0) {
                if (cur_state->mq != NULLPTR) {
                    s32 sent = osSendMesg(PASS_RDRAM cur_state->mq, cur_state->msg, OS_MESG_NOBLOCK);
                    // Always-on ring: runtime→guest VI retrace delivery
                    // (a2 = result; -1 means the game's queue was full and
                    // the retrace was dropped). See ultra_trace.hpp.
                    recomp_ultra_trace_record("~vi_retrace", 0,
                        (uint32_t)cur_state->mq, (uint32_t)cur_state->msg, (uint32_t)sent, 0);
                }
                remaining_retraces = cur_state->retrace_count;
            }
            if (events_context.ai.mq != NULLPTR) {
                s32 sent = osSendMesg(PASS_RDRAM events_context.ai.mq, events_context.ai.msg, OS_MESG_NOBLOCK);
                recomp_ultra_trace_record("~ai_event", 0,
                    (uint32_t)events_context.ai.mq, (uint32_t)events_context.ai.msg, (uint32_t)sent, 0);
            }
        }

        if (events_callbacks.vi_callback != nullptr) {
            events_callbacks.vi_callback();
        }
    }
}

// Counters at the renderer-thread side, paired with the submit_gfx
// counter at the recomp side. Stadium gates its gfx slot state
// machine on RDP-complete (unk_1E -> 2). If dp_complete_count <
// submit_gfx_count after a freeze, the renderer never finished a
// queued gfx task — i.e., RT64 stalled inside processDisplayLists.
static std::atomic<uint64_t> g_sp_complete_count{0};
static std::atomic<uint64_t> g_dp_complete_count{0};
extern "C" uint64_t ultramodern_sp_complete_count(void) { return g_sp_complete_count.load(); }
extern "C" uint64_t ultramodern_dp_complete_count(void) { return g_dp_complete_count.load(); }

// Gfx-DL in-flight tracker. Stamped before/after the BLOCKING call to
// renderer_context->send_dl() in the gfx event thread. If a DL hangs
// inside the renderer (RT64's processDisplayLists), g_current_dl_entry_seq
// > g_current_dl_exit_seq, and the stamped data_ptr/data_size identify
// exactly which DL is stuck — without needing to scrape the SP task ring
// (which can be overwhelmed by audio task submissions during a long hang).
//
// All atomics for lock-free read from the debug-server thread. data_ptr
// and data_size are stamped under the gfx-thread-only invariant that
// only ONE send_dl runs at a time, so there is no torn-read concern.
static std::atomic<uint64_t> g_current_dl_entry_seq{0};
static std::atomic<uint64_t> g_current_dl_exit_seq{0};
static std::atomic<uint64_t> g_current_dl_entry_ms{0};
static std::atomic<uint64_t> g_current_dl_exit_ms{0};
static std::atomic<uint32_t> g_current_dl_data_ptr{0};
static std::atomic<uint32_t> g_current_dl_data_size{0};
static std::atomic<uint32_t> g_current_dl_ucode_ptr{0};

extern "C" void recomp_rsp_mark_sp_task_complete(uint8_t* rdram, uint32_t task_type);

extern "C" void ultramodern_get_current_dl_state(
    uint64_t* entry_seq, uint64_t* exit_seq,
    uint64_t* entry_ms,  uint64_t* exit_ms,
    uint32_t* data_ptr,  uint32_t* data_size,
    uint32_t* ucode_ptr)
{
    if (entry_seq) *entry_seq = g_current_dl_entry_seq.load();
    if (exit_seq)  *exit_seq  = g_current_dl_exit_seq.load();
    if (entry_ms)  *entry_ms  = g_current_dl_entry_ms.load();
    if (exit_ms)   *exit_ms   = g_current_dl_exit_ms.load();
    if (data_ptr)  *data_ptr  = g_current_dl_data_ptr.load();
    if (data_size) *data_size = g_current_dl_data_size.load();
    if (ucode_ptr) *ucode_ptr = g_current_dl_ucode_ptr.load();
}

extern "C" void ultramodern_get_vi_debug_state(
    uint32_t* cur_flags, uint32_t* next_flags,
    uint32_t* cur_fb,    uint32_t* next_fb,
    uint32_t* origin,    uint32_t* h_start,
    uint32_t* y_scale,   uint32_t* status)
{
    std::lock_guard lock{ events_context.message_mutex };
    ViState* cur = events_context.vi.get_cur_state();
    ViState* next = events_context.vi.get_next_state();
    if (cur_flags)  *cur_flags = cur->state;
    if (next_flags) *next_flags = next->state;
    if (cur_fb)     *cur_fb = cur->framebuffer;
    if (next_fb)    *next_fb = next->framebuffer;
    if (origin)     *origin = events_context.vi.regs.VI_ORIGIN_REG;
    if (h_start)    *h_start = events_context.vi.regs.VI_H_START_REG;
    if (y_scale)    *y_scale = events_context.vi.regs.VI_Y_SCALE_REG;
    if (status)     *status = events_context.vi.regs.VI_STATUS_REG;
}

void sp_complete(uint32_t task_type) {
    uint8_t* rdram = events_context.rdram;
    recomp_rsp_mark_sp_task_complete(rdram, task_type);

    std::lock_guard lock{ events_context.message_mutex };
    s32 sent = osSendMesg(PASS_RDRAM events_context.sp.mq, events_context.sp.msg, OS_MESG_NOBLOCK);
    recomp_ultra_trace_record("~sp_done", 0,
        (uint32_t)events_context.sp.mq, (uint32_t)events_context.sp.msg, (uint32_t)sent, task_type);
    g_sp_complete_count.fetch_add(1, std::memory_order_relaxed);
}

void dp_complete(uint32_t task_type) {
    uint8_t* rdram = events_context.rdram;
    recomp_rsp_mark_sp_task_complete(rdram, task_type);

    std::lock_guard lock{ events_context.message_mutex };
    s32 sent = osSendMesg(PASS_RDRAM events_context.dp.mq, events_context.dp.msg, OS_MESG_NOBLOCK);
    recomp_ultra_trace_record("~dp_done", 0,
        (uint32_t)events_context.dp.mq, (uint32_t)events_context.dp.msg, (uint32_t)sent, task_type);
    g_dp_complete_count.fetch_add(1, std::memory_order_relaxed);
}

void task_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready) {
    ultramodern::set_native_thread_name("SP Task Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (true) {
        // Wait until an RSP task has been sent
        OSTask* task;
        events_context.sp_task_queue.wait_dequeue(task);

        if (task == nullptr) {
            return;
        }

        if (!ultramodern::rsp::run_task(PASS_RDRAM task)) {
            fprintf(stderr, "Failed to execute task type: %" PRIu32 "\n", task->t.type);
            ULTRAMODERN_QUICK_EXIT();
        }

        // Tell the game that the RSP has completed
        sp_complete(task->t.type);
    }
}

std::atomic_uint32_t display_refresh_rate = 60;
std::atomic<float> resolution_scale = 1.0f;

uint32_t ultramodern::get_target_framerate(uint32_t original) {
    auto& config = ultramodern::renderer::get_graphics_config();

    switch (config.rr_option) {
        case ultramodern::renderer::RefreshRate::Original:
        default:
            return original;
        case ultramodern::renderer::RefreshRate::Manual:
            return config.rr_manual_value;
        case ultramodern::renderer::RefreshRate::Display:
            return display_refresh_rate.load();
    }
}

uint32_t ultramodern::get_display_refresh_rate() {
    return display_refresh_rate.load();
}

float ultramodern::get_resolution_scale() {
    return resolution_scale.load();
}

void ultramodern::trigger_config_action() {
    events_context.action_queue.enqueue(UpdateConfigAction{});
}

std::atomic<ultramodern::renderer::SetupResult> renderer_setup_result = ultramodern::renderer::SetupResult::Success;
std::atomic<ultramodern::renderer::GraphicsApi> renderer_chosen_api = ultramodern::renderer::GraphicsApi::Auto;

void gfx_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready, ultramodern::renderer::WindowHandle window_handle) {
    bool enabled_instant_present = false;
    using namespace std::chrono_literals;

    ultramodern::set_native_thread_name("Gfx Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    auto old_config = ultramodern::renderer::get_graphics_config();

    auto renderer_context = ultramodern::renderer::create_render_context(rdram, window_handle, ultramodern::renderer::get_graphics_config().developer_mode);

    renderer_chosen_api.store(renderer_context->get_chosen_api());
    if (!renderer_context->valid()) {
        renderer_setup_result.store(renderer_context->get_setup_result());
        // Notify the caller thread that this thread is ready.
        thread_ready->signal();
        return;
    }

    if (events_callbacks.gfx_init_callback != nullptr) {
        events_callbacks.gfx_init_callback();
    }

    ultramodern::rsp::init();

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (!exited) {
        // Try to pull an action from the queue
        Action action;
        if (events_context.action_queue.wait_dequeue_timed(action, 1ms)) {
            // Determine the action type and act on it
            if (const auto* task_action = std::get_if<SpTaskAction>(&action)) {
                // Turn on instant present if the game has been started and it hasn't been turned on yet.
                if (ultramodern::is_game_started() && !enabled_instant_present) {
                    renderer_context->enable_instant_present();
                    enabled_instant_present = true;
                }
                // Tell the game that the RSP completed instantly. This will allow it to queue other task types, but it won't
                // start another graphics task until the RDP is also complete. Games usually preserve the RSP inputs until the RDP
                // is finished as well, so sending this early shouldn't be an issue in most cases.
                // If this causes issues then the logic can be replaced with responding to yield requests.
                sp_complete(task_action->task.t.type);
                ultramodern::measure_input_latency();

                [[maybe_unused]] auto renderer_start = std::chrono::high_resolution_clock::now();
                // Stamp the in-flight DL tracker before entering the
                // BLOCKING send_dl. If RT64 hangs inside, debug-server's
                // current_dl cmd can read these atomics to identify the
                // exact DL that's stuck (data_ptr + data_size locate the
                // raw bytes in rdram for offline analysis).
                {
                    using namespace std::chrono;
                    uint64_t ms = duration_cast<milliseconds>(
                        steady_clock::now().time_since_epoch()).count();
                    g_current_dl_entry_ms.store(ms, std::memory_order_relaxed);
                    g_current_dl_data_ptr.store(
                        (uint32_t)(uintptr_t)task_action->task.t.data_ptr,
                        std::memory_order_relaxed);
                    g_current_dl_data_size.store(
                        task_action->task.t.data_size,
                        std::memory_order_relaxed);
                    g_current_dl_ucode_ptr.store(
                        (uint32_t)(uintptr_t)task_action->task.t.ucode,
                        std::memory_order_relaxed);
                    g_current_dl_entry_seq.fetch_add(1, std::memory_order_release);
                }
                renderer_context->send_dl(&task_action->task);
                {
                    using namespace std::chrono;
                    uint64_t ms = duration_cast<milliseconds>(
                        steady_clock::now().time_since_epoch()).count();
                    g_current_dl_exit_ms.store(ms, std::memory_order_relaxed);
                    g_current_dl_exit_seq.fetch_add(1, std::memory_order_release);
                }
                [[maybe_unused]] auto renderer_end = std::chrono::high_resolution_clock::now();
                dp_complete(task_action->task.t.type);
                // printf("Renderer ProcessDList time: %d us\n", static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(renderer_end - renderer_start).count()));
            }
            else if (const auto* screen_update_action = std::get_if<ScreenUpdateAction>(&action)) {
                events_context.vi.update_screen_regs = screen_update_action->regs;
                renderer_context->update_screen();
                display_refresh_rate = renderer_context->get_display_framerate();
                resolution_scale = renderer_context->get_resolution_scale();
            }
            else if (const auto* config_action = std::get_if<UpdateConfigAction>(&action)) {
                (void)config_action;
                auto new_config = ultramodern::renderer::get_graphics_config();
                if (renderer_context->update_config(old_config, new_config)) {
                    old_config = new_config;
                }
            }
        }
    }

    graphics_shutdown_ready.wait();
    renderer_context->shutdown();
}

#define VI_CTRL_TYPE_16             0x00002
#define VI_CTRL_TYPE_32             0x00003
#define VI_CTRL_GAMMA_DITHER_ON     0x00004
#define VI_CTRL_GAMMA_ON            0x00008
#define VI_CTRL_DIVOT_ON            0x00010
#define VI_CTRL_SERRATE_ON          0x00040
#define VI_CTRL_ANTIALIAS_MASK      0x00300
#define VI_CTRL_ANTIALIAS_MODE_1    0x00100
#define VI_CTRL_ANTIALIAS_MODE_2    0x00200
#define VI_CTRL_ANTIALIAS_MODE_3    0x00300
#define VI_CTRL_PIXEL_ADV_MASK      0x01000
#define VI_CTRL_PIXEL_ADV_1         0x01000
#define VI_CTRL_PIXEL_ADV_2         0x02000
#define VI_CTRL_PIXEL_ADV_3         0x03000
#define VI_CTRL_DITHER_FILTER_ON    0x10000

static const OSViMode dummy_mode = []() {
    OSViMode ret{};

    ret.type = 2;
    ret.comRegs.ctrl = VI_CTRL_TYPE_16 | VI_CTRL_GAMMA_DITHER_ON | VI_CTRL_GAMMA_ON | VI_CTRL_DIVOT_ON | VI_CTRL_ANTIALIAS_MODE_1 | VI_CTRL_PIXEL_ADV_3;
    ret.comRegs.width = 0x140;
    ret.comRegs.burst = 0x03E52239;
    ret.comRegs.vSync = 0x20D;
    ret.comRegs.hSync = 0xC15;
    ret.comRegs.leap = 0x0C150C15;
    ret.comRegs.hStart = 0x006C02EC;
    ret.comRegs.xScale = 0x200;
    ret.comRegs.vCurrent = 0x0;

    for (int field = 0; field < 2; field++) {
        ret.fldRegs[field].origin = 0x280;
        ret.fldRegs[field].yScale = 0x400;
        ret.fldRegs[field].vStart = 0x2501FF;
        ret.fldRegs[field].vBurst = 0xE0204;
        ret.fldRegs[field].vIntr = 0x2;
    }

    return ret;
}();

void set_dummy_vi(bool odd) {
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mode = &dummy_mode;
    // Set up a dummy framebuffer.
    next_state->framebuffer = 0x80700000;
    if (odd) {
        next_state->framebuffer += 0x25800;
    }
}

extern "C" void osViSwapBuffer(RDRAM_ARG PTR(void) frameBufPtr) {
    std::lock_guard lock{ events_context.message_mutex };
    events_context.vi.get_next_state()->framebuffer = frameBufPtr;
}

extern "C" void osViSetMode(RDRAM_ARG PTR(OSViMode) mode_) {
    std::lock_guard lock{ events_context.message_mutex };
    OSViMode* mode = TO_PTR(OSViMode, mode_);
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mode = mode;
    next_state->control = next_state->mode->comRegs.ctrl;
}

#define OS_VI_GAMMA_ON          0x0001
#define OS_VI_GAMMA_OFF         0x0002
#define OS_VI_GAMMA_DITHER_ON   0x0004
#define OS_VI_GAMMA_DITHER_OFF  0x0008
#define OS_VI_DIVOT_ON          0x0010
#define OS_VI_DIVOT_OFF         0x0020
#define OS_VI_DITHER_FILTER_ON  0x0040
#define OS_VI_DITHER_FILTER_OFF 0x0080

extern "C" void osViSetSpecialFeatures(uint32_t func) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* control_out = &next_state->control;
    if ((func & OS_VI_GAMMA_ON) != 0) {
        *control_out |= VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_OFF) != 0) {
        *control_out &= ~VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_ON) != 0) {
        *control_out |= VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_OFF) != 0) {
        *control_out &= ~VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_DIVOT_ON) != 0) {
        *control_out |= VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DIVOT_OFF) != 0) {
        *control_out &= ~VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DITHER_FILTER_ON) != 0) {
        *control_out |= VI_CTRL_DITHER_FILTER_ON;
        *control_out &= ~VI_CTRL_ANTIALIAS_MASK;
    }

    if ((func & OS_VI_DITHER_FILTER_OFF) != 0) {
        *control_out &= ~VI_CTRL_DITHER_FILTER_ON;
        *control_out |= next_state->mode->comRegs.ctrl & VI_CTRL_ANTIALIAS_MASK;
    }
}

extern "C" void osViBlack(uint8_t active) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* state_out = &next_state->state;
    if (active) {
        *state_out |= VI_STATE_BLACK;
    } else {
        *state_out &= ~VI_STATE_BLACK;
    }
}

extern "C" void osViRepeatLine(uint8_t active) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* state_out = &next_state->state;
    if (active) {
        *state_out |= VI_STATE_REPEATLINE;
    } else {
        *state_out &= ~VI_STATE_REPEATLINE;
    }
}

extern "C" void osViSetXScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" void osViSetYScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" PTR(void) osViGetNextFramebuffer() {
    return events_context.vi.get_next_state()->framebuffer;
}

extern "C" PTR(void) osViGetCurrentFramebuffer() {
    return events_context.vi.get_cur_state()->framebuffer;
}

// Counters: total submit_rsp_task calls by task type. Surfaced via
// status command so a frozen game can be characterized as
// "gfx tasks not being submitted by game thread" vs "submitted but
// stuck somewhere downstream of action_queue".
static std::atomic<uint64_t> g_submit_gfx{0};
static std::atomic<uint64_t> g_submit_audio{0};
static std::atomic<uint64_t> g_submit_other{0};

extern "C" uint64_t ultramodern_submit_gfx_count(void) { return g_submit_gfx.load(); }
extern "C" uint64_t ultramodern_submit_audio_count(void) { return g_submit_audio.load(); }
extern "C" uint64_t ultramodern_submit_other_count(void) { return g_submit_other.load(); }
extern "C" uint64_t ultramodern_sp_complete_count(void);
extern "C" uint64_t ultramodern_dp_complete_count(void);

void ultramodern::submit_rsp_task(RDRAM_ARG PTR(OSTask) task_) {
    OSTask* task = TO_PTR(OSTask, task_);

    // Send gfx tasks to the graphics action queue
    if (task->t.type == M_GFXTASK) {
        g_submit_gfx.fetch_add(1, std::memory_order_relaxed);
        events_context.action_queue.enqueue(SpTaskAction{ *task });
    }
    // Set all other tasks as the RSP task
    else {
        if (task->t.type == M_AUDTASK) g_submit_audio.fetch_add(1, std::memory_order_relaxed);
        else g_submit_other.fetch_add(1, std::memory_order_relaxed);
        events_context.sp_task_queue.enqueue(task);
    }
}

void ultramodern::send_si_message(RDRAM_ARG1) {
    s32 sent = osSendMesg(PASS_RDRAM events_context.si.mq, events_context.si.msg, OS_MESG_NOBLOCK);
    recomp_ultra_trace_record("~si_done", 0,
        (uint32_t)events_context.si.mq, (uint32_t)events_context.si.msg, (uint32_t)sent, 0);
}

void ultramodern::init_events(RDRAM_ARG ultramodern::renderer::WindowHandle window_handle) {
    moodycamel::LightweightSemaphore gfx_thread_ready;
    moodycamel::LightweightSemaphore task_thread_ready;
    events_context.rdram = rdram;
    events_context.sp.gfx_thread = std::thread{ gfx_thread_func, rdram, &gfx_thread_ready, window_handle };
    events_context.sp.task_thread = std::thread{ task_thread_func, rdram, &task_thread_ready };

    // Wait for the two sp threads to be ready before continuing to prevent the game from
    // running before we're able to handle RSP tasks.
    gfx_thread_ready.wait();
    task_thread_ready.wait();

    ultramodern::renderer::SetupResult setup_result = renderer_setup_result.load();
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        auto show_renderer_error = [](const std::string& msg) {
            std::string error_msg = "An error has been encountered on startup: " + msg;

            ultramodern::error_handling::message_box(error_msg.c_str());
        };

        const std::string driver_os_suffix = "\nPlease make sure your GPU drivers and your OS are up to date.";
        switch (setup_result) {
            case ultramodern::renderer::SetupResult::Success:
                break;
            case ultramodern::renderer::SetupResult::DynamicLibrariesNotFound:
                show_renderer_error("Failed to load dynamic libraries. Make sure the DLLs are next to the recomp executable.");
                break;
            case ultramodern::renderer::SetupResult::InvalidGraphicsAPI:
                show_renderer_error(ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + " is not supported on this platform. Please select a different graphics API.");
                break;
            case ultramodern::renderer::SetupResult::GraphicsAPINotFound:
                show_renderer_error("Unable to initialize " + ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + "." + driver_os_suffix);
                break;
            case ultramodern::renderer::SetupResult::GraphicsDeviceNotFound:
                show_renderer_error("Unable to find compatible graphics device." + driver_os_suffix);
                break;
        }
        throw std::runtime_error("Failed to initialize the renderer");
    }

    events_context.vi.thread = std::thread{ vi_thread_func };
}

void ultramodern::join_event_threads() {
    events_context.sp.gfx_thread.join();
    events_context.vi.thread.join();

    // Send a null RSP task to indicate that the RSP task thread should exit.
    events_context.sp_task_queue.enqueue(nullptr);
    events_context.sp.task_thread.join();
}
