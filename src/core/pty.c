#include "core/pty.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define LOG_MODULE "pty"
#include "core/util.h"

static const char *
default_shell(void) {
    const char *shell = getenv("SHELL");
    if (shell != NULL && shell[0] != '\0')
        return shell;

    struct passwd *pw = getpwuid(getuid());
    if (pw != NULL && pw->pw_shell != NULL && pw->pw_shell[0] != '\0')
        return pw->pw_shell;

    return "/bin/sh";
}

/* Drop vars by which a parent terminal or editor identifies itself */
static void
scrub_host_env(void) {
    static const char *const prefixes[] = {
        "TERM_PROGRAM",
        "TERMINAL_EMULATOR",
        "NVIM",
        "VIM",
        "KITTY_",
        "WEZTERM_",
        "GHOSTTY_",
        "ALACRITTY_",
        "ITERM_",
        "VTE_",
        "KONSOLE_",
        "WT_",
    };
    for (size_t i = 0; environ != NULL && environ[i] != NULL;) {
        bool match = false;
        for (size_t p = 0; p < ARRAY_LEN(prefixes) && !match; p++)
            match = strncmp(environ[i], prefixes[p], strlen(prefixes[p])) == 0;
        if (!match) {
            i++;
            continue;
        }
        char *eq = strchr(environ[i], '=');
        if (eq == NULL) {
            i++;
            continue;
        }
        size_t len = (size_t)(eq - environ[i]);
        char name[128];
        if (len >= sizeof(name)) {
            i++;
            continue;
        }
        memcpy(name, environ[i], len);
        name[len] = '\0';
        unsetenv(name);
    }
}

static _Noreturn void
child_exec(const char *slave_name, char *const argv[], const char *term_env) {
    /* The parent blocks signals it routes through signalfd; undo that and
     * any ignored dispositions so the shell starts clean. */
    sigset_t mask;
    sigemptyset(&mask);
    sigprocmask(SIG_SETMASK, &mask, NULL);
    for (int sig = 1; sig < NSIG; sig++)
        signal(sig, SIG_DFL);

    if (setsid() < 0)
        _exit(127);

    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0)
        _exit(127);
    if (ioctl(slave, TIOCSCTTY, 0) < 0)
        _exit(127);

    if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
        dup2(slave, STDERR_FILENO) < 0)
        _exit(127);
    if (slave > STDERR_FILENO)
        close(slave);

    setenv("TERM", term_env, 1);
    setenv("COLORTERM", "truecolor", 1);
    unsetenv("COLUMNS");
    unsetenv("LINES");
    unsetenv("TERMCAP");
    scrub_host_env();

    if (argv != NULL) {
        execvp(argv[0], argv);
    } else {
        const char *shell = default_shell();
        execl(shell, shell, (char *)NULL);
    }
    _exit(127);
}

bool pty_spawn(struct pty *pty, char *const argv[], const char *term_env,
               int cols, int rows, int width_px, int height_px) {
    pty->master = -1;
    pty->pid = -1;

    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        LOG_ERRNO("posix_openpt failed");
        return false;
    }
    if (grantpt(master) < 0 || unlockpt(master) < 0) {
        LOG_ERRNO("failed to unlock pty");
        close(master);
        return false;
    }

    char slave_name[64];
    if (ptsname_r(master, slave_name, sizeof(slave_name)) != 0) {
        LOG_ERRNO("ptsname failed");
        close(master);
        return false;
    }

    pty->master = master;
    pty_resize(pty, cols, rows, width_px, height_px);

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERRNO("fork failed");
        close(master);
        pty->master = -1;
        return false;
    }
    if (pid == 0)
        child_exec(slave_name, argv, term_env);

    int flags = fcntl(master, F_GETFL);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) < 0)
        LOG_ERRNO("failed to make pty non-blocking");

    pty->pid = pid;
    return true;
}

bool pty_resize(struct pty *pty, int cols, int rows, int width_px, int height_px) {
    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
        .ws_xpixel = (unsigned short)width_px,
        .ws_ypixel = (unsigned short)height_px,
    };
    if (ioctl(pty->master, TIOCSWINSZ, &ws) < 0) {
        LOG_ERRNO("TIOCSWINSZ failed");
        return false;
    }
    return true;
}

void pty_close(struct pty *pty) {
    if (pty->master >= 0)
        close(pty->master);
    pty->master = -1;
    if (pty->pid > 0)
        kill(pty->pid, SIGHUP);
}
