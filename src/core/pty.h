#pragma once

#include <stdbool.h>
#include <sys/types.h>

struct pty {
    int master; /* non-blocking, close-on-exec */
    pid_t pid;
};

/* Spawns argv (or $SHELL, or /bin/sh when argv is NULL) on a new pty. */
bool pty_spawn(struct pty *pty, char *const argv[], const char *term_env,
               int cols, int rows, int width_px, int height_px);
bool pty_resize(struct pty *pty, int cols, int rows, int width_px, int height_px);
void pty_close(struct pty *pty);
