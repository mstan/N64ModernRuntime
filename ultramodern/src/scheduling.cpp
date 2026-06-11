#include "ultramodern/ultramodern.hpp"

void ultramodern::schedule_running_thread(RDRAM_ARG PTR(OSThread) t_) {
    debug_printf("[Scheduling] Adding thread %d to the running queue\n", TO_PTR(OSThread, t_)->id);
    ultramodern::scheduler_trace_mark(PASS_RDRAM 100, ultramodern::running_queue, t_);
    thread_queue_insert(PASS_RDRAM running_queue, t_);
    TO_PTR(OSThread, t_)->state = OSThreadState::QUEUED;
}

void swap_to_thread(RDRAM_ARG PTR(OSThread) to_) {
    OSThread* to = TO_PTR(OSThread, to_);
    debug_printf("[Scheduling] Thread %d giving execution to thread %d\n", TO_PTR(OSThread, ultramodern::this_thread())->id, to->id);
    ultramodern::scheduler_trace_mark(PASS_RDRAM 110, ultramodern::running_queue, to_);
    // Insert this thread in the running queue.
    ultramodern::thread_queue_insert(PASS_RDRAM ultramodern::running_queue, ultramodern::this_thread());
    ultramodern::scheduler_trace_mark(PASS_RDRAM 111, ultramodern::running_queue, ultramodern::this_thread());
    TO_PTR(OSThread, ultramodern::this_thread())->state = OSThreadState::QUEUED;
    // Unpause the target thread and wait for this one to be unpaused.
    ultramodern::scheduler_trace_mark(PASS_RDRAM 112, ultramodern::running_queue, to_);
    ultramodern::resume_thread_and_wait(PASS_RDRAM to);
    ultramodern::scheduler_trace_mark(PASS_RDRAM 113, ultramodern::running_queue, to_);
}

void ultramodern::check_running_queue(RDRAM_ARG1) {
    // Check if there are any threads in the running queue.
    if (thread_queue_empty(PASS_RDRAM running_queue)) {
        ultramodern::scheduler_trace_mark(PASS_RDRAM 101, running_queue, NULLPTR);
        return;
    }

    // Check if the highest priority thread in the queue is higher priority than the current thread.
    PTR(OSThread) next_ = ultramodern::thread_queue_peek(PASS_RDRAM running_queue);
    OSThread* next_thread = TO_PTR(OSThread, next_);
    OSThread* self = TO_PTR(OSThread, ultramodern::this_thread());
    ultramodern::scheduler_trace_mark(PASS_RDRAM 102, running_queue, next_);
    if (next_thread->priority > self->priority) {
        ultramodern::scheduler_trace_mark(PASS_RDRAM 104, running_queue, next_);
        ultramodern::thread_queue_pop(PASS_RDRAM running_queue);
        // Swap to the higher priority thread.
        swap_to_thread(PASS_RDRAM next_);
    } else {
        ultramodern::scheduler_trace_mark(PASS_RDRAM 103, running_queue, next_);
    }
}

extern "C" void pause_self(RDRAM_ARG1) {
    while (true) {
        // Wait until an external message arrives, then allow the next thread to run.
        ultramodern::wait_for_external_message(PASS_RDRAM1);
        ultramodern::check_running_queue(PASS_RDRAM1);
    }
}

extern "C" void yield_self(RDRAM_ARG1) {
    ultramodern::wait_for_external_message(PASS_RDRAM1);
    ultramodern::check_running_queue(PASS_RDRAM1);
}

extern "C" void yield_self_1ms(RDRAM_ARG1) {
    ultramodern::wait_for_external_message_timed(PASS_RDRAM1, 1);
    ultramodern::check_running_queue(PASS_RDRAM1);
}
