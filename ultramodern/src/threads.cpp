#include <cstdio>
#include <thread>
#include <cassert>
#include <string>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "blockingconcurrentqueue.h"

#include "ultramodern/threads.hpp"
#include "ultramodern/scheduler_tick.hpp"

// Native APIs only used to set thread names for easier debugging
#ifdef _WIN32
#include <Windows.h>
#endif

static ultramodern::threads::callbacks_t threads_callbacks;

void ultramodern::threads::set_callbacks(const callbacks_t& callbacks) {
    threads_callbacks = callbacks;
}

std::string ultramodern::threads::get_game_thread_name(const OSThread* t) {
    if (threads_callbacks.get_game_thread_name == nullptr) {
        return "Game Thread " + std::to_string(t->id);
    }
    return threads_callbacks.get_game_thread_name(t);
}

extern "C" void bootproc();

thread_local bool is_main_thread = false;
// True for threads that belong to the emulated game: the initial start thread and any created via osCreateThread.
thread_local bool is_game_thread = false;
thread_local PTR(OSThread) thread_self = NULLPTR;

void ultramodern::set_main_thread() {
    ::is_game_thread = true;
    is_main_thread = true;
}

bool ultramodern::is_game_thread() {
    return ::is_game_thread;
}

#if 0
int main(int argc, char** argv) {
    ultramodern::set_main_thread();

    bootproc();
}
#endif

#if 1
void run_thread_function(uint8_t* rdram, uint64_t addr, uint64_t sp, uint64_t arg);
#else
#define run_thread_function(func, sp, arg) func(arg)
#endif

#if defined(_WIN32)
void ultramodern::set_native_thread_name(const std::string& name) {
    // Win32 expects a wide string for the debugger-visible thread description.
    std::wstring wide_name(name.begin(), name.end());
    SetThreadDescription(GetCurrentThread(), wide_name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    // Translate the engine's priority enum into a Win32 thread-priority level. The
    // actual SetThreadPriority call is intentionally left disabled below; the mapping
    // is retained so it can be re-enabled without rework.
    int win32_priority;
    switch (pri) {
        case ThreadPriority::Low:      win32_priority = THREAD_PRIORITY_BELOW_NORMAL;  break;
        case ThreadPriority::Normal:   win32_priority = THREAD_PRIORITY_NORMAL;        break;
        case ThreadPriority::High:     win32_priority = THREAD_PRIORITY_ABOVE_NORMAL;  break;
        case ThreadPriority::VeryHigh: win32_priority = THREAD_PRIORITY_HIGHEST;       break;
        case ThreadPriority::Critical: win32_priority = THREAD_PRIORITY_TIME_CRITICAL; break;
        default:
            throw std::runtime_error("Invalid thread priority!");
    }
    (void)win32_priority;
    // SetThreadPriority(GetCurrentThread(), win32_priority);
}
#elif defined(__linux__)
#include <sys/prctl.h>

void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Linux only accepts up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    prctl(PR_SET_NAME, name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    // TODO linux thread priority
}
#elif defined(__APPLE__)
void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Macs seem to only accept up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    pthread_setname_np(name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {}
#endif

void wait_for_resumed(RDRAM_ARG UltraThreadContext* thread_context) {
    ultramodern::scheduler_trace_mark(PASS_RDRAM 120, NULLPTR, ultramodern::this_thread());
    thread_context->running.wait();
    ultramodern::scheduler_trace_mark(PASS_RDRAM 121, NULLPTR, ultramodern::this_thread());
    // Once woken, the running OSThread may no longer own the context we blocked on: a
    // replacement or a destroy can swap it out while we sleep. In that case re-enter the
    // destroy path from our own context so the cleanup actually runs.
    if (TO_PTR(OSThread, ultramodern::this_thread())->context != thread_context) {
        osDestroyThread(PASS_RDRAM NULLPTR);
    }
    // Stamp the "last context switch into a game thread" timestamp.
    // The scheduler_tick monitor compares now() against this to decide
    // whether the currently-running game thread is hogging the CPU.
    ultramodern::record_context_switch();
}

void resume_thread(RDRAM_ARG OSThread* t) {
    debug_printf("[Thread] Waking thread %d\n", t->id);
    ultramodern::scheduler_trace_mark(PASS_RDRAM 122, NULLPTR, NULLPTR);
    t->context->running.signal();
}

void run_next_thread(RDRAM_ARG1) {
    ultramodern::scheduler_trace_mark(PASS_RDRAM 123, ultramodern::running_queue, NULLPTR);
    if (ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
        ultramodern::scheduler_trace_mark(PASS_RDRAM 124, ultramodern::running_queue, NULLPTR);
        throw std::runtime_error("No runnable threads remain!\n");
    }

    PTR(OSThread) to_run_ = ultramodern::thread_queue_pop(PASS_RDRAM ultramodern::running_queue);
    OSThread* to_run = TO_PTR(OSThread, to_run_);
    ultramodern::scheduler_trace_mark(PASS_RDRAM 125, ultramodern::running_queue, to_run_);
    debug_printf("[Scheduling] Handing control to thread %d\n", to_run->id);
    to_run->context->running.signal();
}

void ultramodern::run_next_thread_and_wait(RDRAM_ARG1) {
    UltraThreadContext* caller_ctx = TO_PTR(OSThread, thread_self)->context;
    ultramodern::scheduler_trace_mark(PASS_RDRAM 126, ultramodern::running_queue, ultramodern::this_thread());
    run_next_thread(PASS_RDRAM1);
    wait_for_resumed(PASS_RDRAM caller_ctx);
    ultramodern::scheduler_trace_mark(PASS_RDRAM 127, ultramodern::running_queue, ultramodern::this_thread());
}

void ultramodern::resume_thread_and_wait(RDRAM_ARG OSThread *t) {
    UltraThreadContext* caller_ctx = TO_PTR(OSThread, thread_self)->context;
    ultramodern::scheduler_trace_mark(PASS_RDRAM 112, ultramodern::running_queue, ultramodern::this_thread());
    resume_thread(PASS_RDRAM t);
    wait_for_resumed(PASS_RDRAM caller_ctx);
    ultramodern::scheduler_trace_mark(PASS_RDRAM 113, ultramodern::running_queue, ultramodern::this_thread());
}

static void run_game_thread(RDRAM_ARG PTR(OSThread) self_, PTR(thread_func_t) entrypoint, PTR(void) arg, UltraThreadContext* ctx) {
    OSThread* self = TO_PTR(OSThread, self_);
    debug_printf("[Thread] Thread created: %d\n", self->id);

    // Publish our identity into thread-local state before anything the scheduler can observe.
    thread_self = self_;
    is_game_thread = true;

    // Set the thread name
    ultramodern::set_native_thread_name(ultramodern::threads::get_game_thread_name(self));
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::High);

    // Report that setup is done so osCreateThread can return to its caller.
    ctx->initialized.signal();

    debug_printf("[Thread] Thread waiting to be started: %d\n", self->id);

    // Park here until this context is signalled (by osStartThread, or by a destroy).
    try {
        wait_for_resumed(PASS_RDRAM ctx);
    } catch (ultramodern::thread_terminated&) {
    }

    // Only run the body if we're still the live context for this OSThread; a destroy that
    // arrived while we were parked clears it.
    if (self->context == ctx) {
        debug_printf("[Thread] Thread started: %d\n", self->id);
        try {
            // Hand off to the recompiled entrypoint with the supplied argument.
            run_thread_function(PASS_RDRAM entrypoint, self->sp, arg);
        } catch (ultramodern::thread_terminated&) {
        }
    }
    else {
        debug_printf("[Thread] Thread destroyed before being started: %d\n", self->id);
    }

    // If the body returned (or terminated) without the context having been swapped out
    // from under us, this thread ended on its own: drop our context and let the scheduler
    // advance to the next runnable thread.
    if (self->context == ctx) {
        self->context = nullptr;
        run_next_thread(PASS_RDRAM1);
    }

    // Hand the context to the cleaner thread for join + delete.
    ultramodern::cleanup_thread(ctx);
}

extern "C" void osStartThread(RDRAM_ARG PTR(OSThread) t_) {
    OSThread* t = TO_PTR(OSThread, t_);
    debug_printf("[os] Start Thread %d\n", t->id);

    if (thread_self) {
        // Invoked from inside a game thread: enqueue and let the scheduler decide.
        ultramodern::schedule_running_thread(PASS_RDRAM t_);
        ultramodern::check_running_queue(PASS_RDRAM1);
    }
    else {
        // Invoked from outside the game (the initial start): wake the thread directly.
        t->state = OSThreadState::QUEUED;
        resume_thread(PASS_RDRAM t);
        //throw ultramodern::thread_terminated{};
    }
}

extern "C" void osCreateThread(RDRAM_ARG PTR(OSThread) t_, OSId id, PTR(thread_func_t) entrypoint, PTR(void) arg, PTR(void) sp, OSPri pri) {
    debug_printf("[os] Create Thread %d\n", id);
    OSThread *t = TO_PTR(OSThread, t_);

    t->next = NULLPTR;
    t->queue = NULLPTR;
    t->priority = pri;
    t->id = id;
    t->state = OSThreadState::STOPPED;
    t->sp = sp - 0x10; // Set up the first stack frame

    // Launch the host thread; it parks itself immediately (see run_game_thread) and waits
    // to be started. The context is passed in as an argument so a later clear of t->context
    // can't race the thread reading its own context.
    UltraThreadContext* ctx = new UltraThreadContext{};
    t->context = ctx;
    ctx->host_thread = std::thread{run_game_thread, PASS_RDRAM t_, entrypoint, arg, t->context};

    // Block until the host thread reports it has initialized.
    ctx->initialized.wait();
    debug_printf("[os] Thread %d is ready to be started\n", t->id);
}

extern "C" void osStopThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    // The only supported case is a thread stopping itself (arg null or thread_self).
    if (t_ == thread_self) {
        ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
    }
    else {
        assert(false);
    }
}

extern "C" void osDestroyThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);
    // A thread destroying itself (arg null or thread_self) unwinds via the termination exception.
    if (t_ == thread_self) {
        throw ultramodern::thread_terminated{};
    }
    // Destroying another thread: detach it from whatever queue currently holds it.
    if (t->state != OSThreadState::STOPPED) {
        ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
    }
    // Clear the context (guarding against a double destroy) then wake the thread; it will
    // see the cleared context on resume and terminate itself instead of running.
    UltraThreadContext* target_ctx = t->context;
    if (target_ctx != nullptr) {
        t->context = nullptr;
        target_ctx->running.signal();
    }
}

extern "C" void osSetThreadPri(RDRAM_ARG PTR(OSThread) t_, OSPri pri) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);

    if (t->priority != pri) {
        t->priority = pri;

        // A thread that is queued somewhere must be re-inserted so the queue order tracks
        // its new priority. The running thread isn't in a queue, so it's skipped.
        if (t_ != ultramodern::this_thread() && t->state != OSThreadState::STOPPED) {
            ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
            ultramodern::thread_queue_insert(PASS_RDRAM t->queue, t_);
        }

        ultramodern::check_running_queue(PASS_RDRAM1);
    }
}

extern "C" OSPri osGetThreadPri(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return TO_PTR(OSThread, t)->priority;
}

extern "C" OSId osGetThreadId(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return TO_PTR(OSThread, t)->id;
}

PTR(OSThread) ultramodern::this_thread() {
    return thread_self;
}

static std::thread thread_cleaner_thread;
static moodycamel::BlockingConcurrentQueue<UltraThreadContext*> deleted_threads{};
extern std::atomic_bool exited;

void thread_cleaner_func() {
    using namespace std::chrono_literals;
    // A thread can't join itself, so completed contexts are funnelled here to be joined and
    // freed from a separate thread.
    while (!exited) {
        UltraThreadContext* to_delete;
        if (deleted_threads.wait_dequeue_timed(to_delete, 10ms)) {
            debug_printf("[Cleanup] Deleting thread context %p\n", to_delete);

            to_delete->host_thread.join();
            delete to_delete;
        }
    }
}

void ultramodern::init_thread_cleanup() {
    thread_cleaner_thread = std::thread{thread_cleaner_func};
}

void ultramodern::cleanup_thread(UltraThreadContext *cur_context) {
    deleted_threads.enqueue(cur_context);
}

void ultramodern::join_thread_cleaner_thread() {
    thread_cleaner_thread.join();
}
