/*
 * Adapted from foot's fdm.c.
 * Copyright (c) 2019 Daniel Eklöf (foot, MIT; see LICENSE)
 */
#include "core/loop.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define LOG_MODULE "loop"
#include "core/util.h"

#define MAX_EVENTS 32

struct fd_handler {
    int fd;
    uint32_t events;
    loop_fd_cb cb;
    void *data;
    bool deleted;
};

struct sig_handler {
    loop_signal_cb cb;
    void *data;
};

struct hook {
    loop_hook_cb cb;
    void *data;
};

struct hook_list {
    struct hook *items;
    size_t count, cap;
};

struct loop {
    int epoll_fd;
    bool is_polling;

    struct fd_handler **fds;
    size_t fd_count, fd_cap;

    /* Handlers removed while dispatching; freed after the dispatch pass */
    struct fd_handler **deferred;
    size_t deferred_count, deferred_cap;

    int signal_fd;
    sigset_t sigmask; /* signals routed through signal_fd */
    sigset_t orig_sigmask;
    struct sig_handler signals[65];

    struct hook_list hooks[LOOP_HOOK_COUNT];
};

static bool signal_fd_cb(struct loop *loop, int fd, uint32_t events, void *data);

struct loop *
loop_new(void) {
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        LOG_ERRNO("failed to create epoll fd");
        return NULL;
    }

    struct loop *loop = xcalloc(1, sizeof(*loop));
    loop->epoll_fd = epoll_fd;
    loop->signal_fd = -1;
    sigemptyset(&loop->sigmask);
    sigprocmask(0, NULL, &loop->orig_sigmask);
    return loop;
}

void loop_destroy(struct loop *loop) {
    if (loop == NULL)
        return;

    if (loop->signal_fd >= 0)
        loop_del(loop, loop->signal_fd);

    if (loop->fd_count > 0)
        LOG_WARN("%zu fds still registered", loop->fd_count);
    for (size_t i = 0; i < loop->fd_count; i++)
        free(loop->fds[i]);

    sigprocmask(SIG_SETMASK, &loop->orig_sigmask, NULL);

    for (int i = 0; i < LOOP_HOOK_COUNT; i++)
        free(loop->hooks[i].items);
    free(loop->fds);
    free(loop->deferred);
    close(loop->epoll_fd);
    free(loop);
}

static struct fd_handler **
find_fd(struct loop *loop, int fd) {
    for (size_t i = 0; i < loop->fd_count; i++) {
        if (loop->fds[i]->fd == fd)
            return &loop->fds[i];
    }
    return NULL;
}

bool loop_add(struct loop *loop, int fd, uint32_t events, loop_fd_cb cb, void *data) {
    if (find_fd(loop, fd) != NULL) {
        LOG_ERR("fd %d already registered", fd);
        return false;
    }

    struct fd_handler *h = xmalloc(sizeof(*h));
    *h = (struct fd_handler){.fd = fd, .events = events, .cb = cb, .data = data};

    struct epoll_event ev = {.events = events, .data.ptr = h};
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERRNO("failed to register fd %d with epoll", fd);
        free(h);
        return false;
    }

    if (loop->fd_count == loop->fd_cap) {
        loop->fd_cap = loop->fd_cap ? loop->fd_cap * 2 : 8;
        loop->fds = xrealloc(loop->fds, loop->fd_cap * sizeof(loop->fds[0]));
    }
    loop->fds[loop->fd_count++] = h;
    return true;
}

static bool
del_internal(struct loop *loop, int fd, bool close_fd) {
    if (fd < 0)
        return true;

    struct fd_handler **slot = find_fd(loop, fd);
    if (slot == NULL) {
        LOG_ERR("fd %d not registered", fd);
        if (close_fd)
            close(fd);
        return false;
    }

    struct fd_handler *h = *slot;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0)
        LOG_ERRNO("failed to unregister fd %d from epoll", fd);
    if (close_fd)
        close(fd);

    h->deleted = true;
    if (loop->is_polling) {
        if (loop->deferred_count == loop->deferred_cap) {
            loop->deferred_cap = loop->deferred_cap ? loop->deferred_cap * 2 : 4;
            loop->deferred = xrealloc(
                loop->deferred, loop->deferred_cap * sizeof(loop->deferred[0]));
        }
        loop->deferred[loop->deferred_count++] = h;
    } else
        free(h);

    *slot = loop->fds[--loop->fd_count];
    return true;
}

bool loop_del(struct loop *loop, int fd) {
    return del_internal(loop, fd, true);
}

bool loop_del_no_close(struct loop *loop, int fd) {
    return del_internal(loop, fd, false);
}

static bool
event_modify(struct loop *loop, int fd, uint32_t add, uint32_t del) {
    struct fd_handler **slot = find_fd(loop, fd);
    if (slot == NULL) {
        LOG_ERR("fd %d not registered", fd);
        return false;
    }

    struct fd_handler *h = *slot;
    uint32_t events = (h->events | add) & ~del;
    if (events == h->events)
        return true;

    struct epoll_event ev = {.events = events, .data.ptr = h};
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        LOG_ERRNO("failed to modify fd %d", fd);
        return false;
    }
    h->events = events;
    return true;
}

bool loop_event_add(struct loop *loop, int fd, uint32_t events) {
    return event_modify(loop, fd, events, 0);
}

bool loop_event_del(struct loop *loop, int fd, uint32_t events) {
    return event_modify(loop, fd, 0, events);
}

static bool
update_signal_fd(struct loop *loop) {
    int fd = signalfd(loop->signal_fd, &loop->sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) {
        LOG_ERRNO("failed to create signalfd");
        return false;
    }

    if (loop->signal_fd < 0) {
        if (!loop_add(loop, fd, EPOLLIN, &signal_fd_cb, NULL)) {
            close(fd);
            return false;
        }
        loop->signal_fd = fd;
    }
    return true;
}

bool loop_signal_add(struct loop *loop, int signo, loop_signal_cb cb, void *data) {
    if (signo <= 0 || signo >= (int)ARRAY_LEN(loop->signals)) {
        LOG_ERR("invalid signal %d", signo);
        return false;
    }
    if (loop->signals[signo].cb != NULL) {
        LOG_ERR("signal %d already has a handler", signo);
        return false;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signo);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        LOG_ERRNO("failed to block signal %d", signo);
        return false;
    }

    sigaddset(&loop->sigmask, signo);
    if (!update_signal_fd(loop)) {
        sigdelset(&loop->sigmask, signo);
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
        return false;
    }

    loop->signals[signo] = (struct sig_handler){.cb = cb, .data = data};
    return true;
}

bool loop_signal_del(struct loop *loop, int signo) {
    if (signo <= 0 || signo >= (int)ARRAY_LEN(loop->signals) ||
        loop->signals[signo].cb == NULL)
        return false;

    loop->signals[signo] = (struct sig_handler){0};
    sigdelset(&loop->sigmask, signo);
    update_signal_fd(loop);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signo);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    return true;
}

static bool
signal_fd_cb(struct loop *loop, int fd, uint32_t events, void *data) {
    struct signalfd_siginfo info;
    for (;;) {
        ssize_t n = read(fd, &info, sizeof(info));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return true;
            LOG_ERRNO("failed to read signalfd");
            return false;
        }
        if (n != sizeof(info))
            return true;

        int signo = (int)info.ssi_signo;
        if (signo < (int)ARRAY_LEN(loop->signals) && loop->signals[signo].cb != NULL) {
            if (!loop->signals[signo].cb(loop, signo, loop->signals[signo].data))
                return false;
        }
    }
}

bool loop_hook_add(struct loop *loop, loop_hook_cb cb, void *data,
                   enum loop_hook_priority prio) {
    struct hook_list *list = &loop->hooks[prio];
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].cb == cb) {
            LOG_ERR("hook already registered");
            return false;
        }
    }

    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 4;
        list->items = xrealloc(list->items, list->cap * sizeof(list->items[0]));
    }
    list->items[list->count++] = (struct hook){.cb = cb, .data = data};
    return true;
}

bool loop_hook_del(struct loop *loop, loop_hook_cb cb, enum loop_hook_priority prio) {
    struct hook_list *list = &loop->hooks[prio];
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].cb == cb) {
            memmove(&list->items[i], &list->items[i + 1],
                    (list->count - i - 1) * sizeof(list->items[0]));
            list->count--;
            return true;
        }
    }
    return false;
}

int loop_timer_add(struct loop *loop, loop_fd_cb cb, void *data) {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        LOG_ERRNO("failed to create timerfd");
        return -1;
    }
    if (!loop_add(loop, fd, EPOLLIN, cb, data)) {
        close(fd);
        return -1;
    }
    return fd;
}

bool loop_timer_set(int fd, uint64_t first_ns, uint64_t interval_ns) {
    struct itimerspec spec = {
        .it_value = {.tv_sec = first_ns / 1000000000, .tv_nsec = first_ns % 1000000000},
        .it_interval = {.tv_sec = interval_ns / 1000000000,
                        .tv_nsec = interval_ns % 1000000000},
    };
    if (timerfd_settime(fd, 0, &spec, NULL) < 0) {
        LOG_ERRNO("failed to arm timerfd");
        return false;
    }
    return true;
}

uint64_t
loop_timer_ack(int fd) {
    uint64_t expirations = 0;
    if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations))
        return 0;
    return expirations;
}

bool loop_poll(struct loop *loop) {
    if (loop->is_polling) {
        LOG_ERR("nested loop_poll() not allowed");
        return false;
    }

    for (int p = 0; p < LOOP_HOOK_COUNT; p++) {
        struct hook_list *list = &loop->hooks[p];
        for (size_t i = 0; i < list->count; i++)
            list->items[i].cb(loop, list->items[i].data);
    }

    struct epoll_event events[MAX_EVENTS];
    int r = epoll_wait(loop->epoll_fd, events, MAX_EVENTS, -1);
    if (r < 0) {
        if (errno == EINTR)
            return true;
        LOG_ERRNO("epoll_wait failed");
        return false;
    }

    bool ret = true;
    loop->is_polling = true;
    for (int i = 0; i < r; i++) {
        struct fd_handler *h = events[i].data.ptr;
        if (h->deleted)
            continue;
        if (!h->cb(loop, h->fd, events[i].events, h->data)) {
            ret = false;
            break;
        }
    }
    loop->is_polling = false;

    for (size_t i = 0; i < loop->deferred_count; i++)
        free(loop->deferred[i]);
    loop->deferred_count = 0;

    return ret;
}
