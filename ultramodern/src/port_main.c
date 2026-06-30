#if 0

#include <stdio.h>
#include <stdlib.h>
#include "ultra64.h"

#define THREAD_STACK_SIZE 0x1000

u8 idle_stack[THREAD_STACK_SIZE] ALIGNED(16);
u8 main_stack[THREAD_STACK_SIZE] ALIGNED(16);
u8 thread3_stack[THREAD_STACK_SIZE] ALIGNED(16);
u8 thread4_stack[THREAD_STACK_SIZE] ALIGNED(16);

OSThread idle_thread;
OSThread main_thread;
OSThread thread3;
OSThread thread4;

OSMesgQueue msg_queue;
OSMesg msg_queue_storage[1];

void thread3_func(UNUSED void *arg) {
    OSMesg received;
    printf("thread 3: waiting on message\n");
    fflush(stdout);
    osRecvMesg(&msg_queue, &received, OS_MESG_BLOCK);
    printf("thread 3: got %d\n", (int)(intptr_t)received);
    fflush(stdout);
}

void thread4_func(void *arg) {
    printf("thread 4: posting %d\n", (int)(intptr_t)arg);
    fflush(stdout);
    osSendMesg(&msg_queue, arg, OS_MESG_BLOCK);
    printf("thread 4: done\n");
    fflush(stdout);
}

void main_thread_func(UNUSED void* arg) {
    osCreateMesgQueue(&msg_queue, msg_queue_storage, sizeof(msg_queue_storage) / sizeof(msg_queue_storage[0]));

    printf("main: spawning thread 3\n");
    osCreateThread(&thread3, 3, thread3_func, NULL, &thread3_stack[THREAD_STACK_SIZE], 14);
    printf("main: launching thread 3\n");
    osStartThread(&thread3);

    printf("main: spawning thread 4\n");
    osCreateThread(&thread4, 4, thread4_func, (void*)10, &thread4_stack[THREAD_STACK_SIZE], 13);
    printf("main: launching thread 4\n");
    osStartThread(&thread4);

    while (1) {
        printf("main: tick\n");
        sleep(1);
    }
}

void idle_thread_func(UNUSED void* arg) {
    printf("idle: entered\n");
    printf("idle: spawning main thread\n");
    osCreateThread(&main_thread, 2, main_thread_func, NULL, &main_stack[THREAD_STACK_SIZE], 11);
    printf("idle: launching main thread\n");
    osStartThread(&main_thread);

    // Drop this thread to priority 0 so it serves as the idle thread.
    osSetThreadPri(NULL, 0);

    // Remain here as the idle loop.
    while (1) {
        printf("idle: tick\n");
        sleep(1);
    }
}

void bootproc(void) {
    osInitialize();

    osCreateThread(&idle_thread, 1, idle_thread_func, NULL, &idle_stack[THREAD_STACK_SIZE], 127);
    printf("booting idle thread\n");
    osStartThread(&idle_thread);
}

#endif
