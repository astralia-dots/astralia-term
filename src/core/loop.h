/*
 * Event loop: epoll fd registry, signalfd signals, timerfd timers, and
 * pre-poll hooks. Adapted from foot's fdm.c.
 *
 * Copyright (c) 2019 Daniel Eklöf (foot, MIT; see LICENSE)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct loop;

/* Return false to stop the loop (loop_poll() then returns false). */
typedef bool (*loop_fd_cb)(struct loop *loop, int fd, uint32_t events, void *data);
typedef bool (*loop_signal_cb)(struct loop *loop, int signo, void *data);
typedef void (*loop_hook_cb)(struct loop *loop, void *data);

enum loop_hook_priority {
    LOOP_HOOK_HIGH,
    LOOP_HOOK_NORMAL,
    LOOP_HOOK_LOW,
    LOOP_HOOK_COUNT,
};

struct loop *loop_new(void);
void loop_destroy(struct loop *loop);

bool loop_add(struct loop *loop, int fd, uint32_t events, loop_fd_cb cb, void *data);
bool loop_del(struct loop *loop, int fd); /* also closes fd */
bool loop_del_no_close(struct loop *loop, int fd);
bool loop_event_add(struct loop *loop, int fd, uint32_t events);
bool loop_event_del(struct loop *loop, int fd, uint32_t events);

bool loop_signal_add(struct loop *loop, int signo, loop_signal_cb cb, void *data);
bool loop_signal_del(struct loop *loop, int signo);

bool loop_hook_add(struct loop *loop, loop_hook_cb cb, void *data,
                   enum loop_hook_priority prio);
bool loop_hook_del(struct loop *loop, loop_hook_cb cb, enum loop_hook_priority prio);

/* Timers are timerfds registered as ordinary fds. The callback must call
 * loop_timer_ack() to drain the expiration count. Returns the fd, or -1. */
int loop_timer_add(struct loop *loop, loop_fd_cb cb, void *data);
bool loop_timer_set(int fd, uint64_t first_ns, uint64_t interval_ns); /* 0,0 disarms */
uint64_t loop_timer_ack(int fd);

/* Runs hooks, waits for events, dispatches. Returns false on error or
 * when a callback asked to stop. */
bool loop_poll(struct loop *loop);
