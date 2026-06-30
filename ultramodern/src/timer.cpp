#include <thread>
#include <variant>
#include <set>
#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/ultra_trace.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include "Windows.h"
#endif

// Reference point captured at load; every reported time is measured relative to it.
static std::chrono::high_resolution_clock::time_point start_time = std::chrono::high_resolution_clock::now();
// Bias applied to osGetTime so that osSetTime can move the apparent clock.
static int64_t ostime_offset = 0;
// Game speed multiplier (1 means no speedup)
constexpr uint32_t speed_multiplier = 1;
// N64 CPU counter ticks per millisecond
constexpr uint32_t counter_per_ms = 46'875 * speed_multiplier;

struct OSTimer {
    PTR(OSTimer) unused1;
    PTR(OSTimer) unused2;
    OSTime interval;
    OSTime timestamp;
    PTR(OSMesgQueue) mq;
    OSMesg msg;
};

// Commands handed to the timer thread to mutate its active set.
struct TimerInsert {
    PTR(OSTimer) timer;
};

struct TimerCancel {
    PTR(OSTimer) timer;
};

using TimerCommand = std::variant<TimerInsert, TimerCancel>;

struct {
    std::thread thread;
    moodycamel::BlockingConcurrentQueue<TimerCommand> command_queue{};
} g_timer;

// Convert a wall-clock duration into N64 counter ticks. Integer math keeps full
// precision; at 46,875 ticks/ms the running product does not overflow a 64-bit
// value until well over a decade of uptime.
uint64_t duration_to_ticks(std::chrono::high_resolution_clock::duration duration) {
    uint64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    // (micros * ticks/ms) / (micros/ms) = ticks
    return (micros * counter_per_ms) / 1000;
}

std::chrono::microseconds ticks_to_duration(uint64_t ticks) {
    using namespace std::chrono_literals;
    return ticks * 1000us / counter_per_ms;
}

std::chrono::high_resolution_clock::time_point ticks_to_timepoint(uint64_t ticks) {
    return start_time + ticks_to_duration(ticks);
}

uint64_t time_now() {
    return duration_to_ticks(std::chrono::high_resolution_clock::now() - start_time);
}

void timer_thread(RDRAM_ARG1) {
    ultramodern::set_native_thread_name("Timer Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::VeryHigh);

    // Keep the active timers sorted so the soonest deadline is always at begin().
    // Ties on timestamp are broken by guest address to give the set a total order.
    auto by_deadline = [PASS_RDRAM1](PTR(OSTimer) lhs_, PTR(OSTimer) rhs_) {
        OSTimer* lhs = TO_PTR(OSTimer, lhs_);
        OSTimer* rhs = TO_PTR(OSTimer, rhs_);

        if (lhs->timestamp != rhs->timestamp) {
            return lhs->timestamp < rhs->timestamp;
        }
        return lhs < rhs;
    };

    std::set<PTR(OSTimer), decltype(by_deadline)> pending{by_deadline};

    // Apply one queued command to the active set.
    auto apply_command = [&](const TimerCommand& command) {
        if (const auto* insert = std::get_if<TimerInsert>(&command)) {
            pending.insert(insert->timer);
        } else if (const auto* cancel = std::get_if<TimerCancel>(&command)) {
            pending.erase(cancel->timer);
        }
    };

    TimerCommand command;
    while (true) {
        // Drain whatever commands are already waiting without blocking.
        while (g_timer.command_queue.try_dequeue(command)) {
            apply_command(command);
        }

        // With nothing scheduled, block until a command gives us a timer to track.
        while (pending.empty()) {
            g_timer.command_queue.wait_dequeue(command);
            apply_command(command);
        }

        // Pull out the timer with the nearest deadline. It is removed up front and
        // re-inserted below if the wait gets interrupted before it elapses.
        PTR(OSTimer) next_ = *pending.begin();
        OSTimer* next = TO_PTR(OSTimer, next_);
        pending.erase(next_);

        // How long until this timer's deadline relative to right now.
        auto remaining = ticks_to_timepoint(next->timestamp) - std::chrono::high_resolution_clock::now();

        // Sleep until the deadline, but wake early if a command arrives first. A
        // non-positive remaining means the deadline already passed, so skip the
        // wait and fire immediately.
        if (remaining.count() >= 0 && g_timer.command_queue.wait_dequeue_timed(command, remaining)) {
            // A command preempted the wait. Re-add this timer (first, so a cancel
            // command targeting it still takes effect) then apply the command.
            pending.insert(next_);
            apply_command(command);
        }
        else {
            // The deadline elapsed: deliver the timer's message to its queue.
            s32 sent = osSendMesg(PASS_RDRAM next->mq, next->msg, OS_MESG_NOBLOCK);
            // Always-on ring: runtime→guest timer delivery (see ultra_trace.hpp).
            recomp_ultra_trace_record("~timer_fire", 0,
                (uint32_t)next->mq, (uint32_t)next->msg, (uint32_t)sent, (uint32_t)next_);
            // Periodic timers (non-zero interval) re-arm from the current time.
            if (next->interval != 0) {
                next->timestamp = next->interval + time_now();
                pending.insert(next_);
            }
        }
    }
}

void ultramodern::init_timers(RDRAM_ARG1) {
    g_timer.thread = std::thread{ timer_thread, PASS_RDRAM1 };
    g_timer.thread.detach();
}

uint32_t ultramodern::get_speed_multiplier() {
    return speed_multiplier;
}

std::chrono::high_resolution_clock::time_point ultramodern::get_start() {
    return start_time;
}

std::chrono::high_resolution_clock::duration ultramodern::time_since_start() {
    return std::chrono::high_resolution_clock::now() - start_time;
}

extern "C" u32 osGetCount() {
    // osGetCount is allowed to wrap, so truncating the 64-bit tick count is fine.
    return (uint32_t)time_now();
}

extern "C" void osSetCount(u32 count) {
    assert(false);
}

extern "C" OSTime osGetTime() {
    return time_now() - ostime_offset;
}

extern "C" void osSetTime(OSTime t) {
    ostime_offset = time_now() - t;
}

extern "C" int osSetTimer(RDRAM_ARG PTR(OSTimer) t_, OSTime countdown, OSTime interval, PTR(OSMesgQueue) mq, OSMesg msg) {
    OSTimer* t = TO_PTR(OSTimer, t_);

    // A zero countdown means the first fire is one interval away; otherwise it is
    // the countdown away.
    t->timestamp = (countdown == 0 ? interval : countdown) + time_now();
    t->interval = interval;
    t->mq = mq;
    t->msg = msg;

    g_timer.command_queue.enqueue(TimerInsert{ t_ });

    return 0;
}

extern "C" int osStopTimer(RDRAM_ARG PTR(OSTimer) t_) {
    g_timer.command_queue.enqueue(TimerCancel{ t_ });

    // TODO don't blindly return 0 here; requires some response from the timer thread to know what the returned value was
    return 0;
}

#ifdef _WIN32

// Older Microsoft STL builds let a backwards system-clock change disrupt
// std::chrono sleep_until/sleep_for. That was fixed in Visual Studio 2022 17.9,
// but ultramodern calls the Win32 Sleep API directly to stay safe regardless.
void ultramodern::sleep_milliseconds(uint32_t millis) {
    Sleep(millis);
}

void ultramodern::sleep_until(const std::chrono::high_resolution_clock::time_point& time_point) {
    auto now = std::chrono::high_resolution_clock::now();
    if (time_point > now) {
        long long delta_ms = std::chrono::ceil<std::chrono::milliseconds>(time_point - now).count();
        Sleep(delta_ms);
    }
}

#else

void ultramodern::sleep_milliseconds(uint32_t millis) {
    std::this_thread::sleep_for(std::chrono::milliseconds{millis});
}

void ultramodern::sleep_until(const std::chrono::high_resolution_clock::time_point& time_point) {
    std::this_thread::sleep_until(time_point);
}

#endif
