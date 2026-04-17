/*
 * watch - periodically run a command and display its output.
 * Solaris 8 port, v1 (core flags: -n SECONDS, -t).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#ifndef TIOCGWINSZ
#include <sys/termios.h>
#endif

static void usage(FILE *out) {
    fputs("Usage: watch [-n SECONDS] [-t] COMMAND [ARGS...]\n", out);
}

/* Join argv[start..argc) with single spaces into a malloc'd string. */
static char *join_argv(int argc, char **argv, int start) {
    size_t total = 1; /* trailing NUL */
    for (int i = start; i < argc; i++) {
        total += strlen(argv[i]);
        if (i + 1 < argc) total += 1; /* space separator */
    }
    char *buf = (char *)malloc(total);
    if (!buf) return NULL;
    buf[0] = '\0';
    for (int i = start; i < argc; i++) {
        strcat(buf, argv[i]);
        if (i + 1 < argc) strcat(buf, " ");
    }
    return buf;
}

static volatile sig_atomic_t stop_flag = 0;

static void on_signal(int sig) {
    (void)sig;
    stop_flag = 1;
}

static void sleep_fractional(double seconds) {
    struct timespec ts;
    ts.tv_sec  = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0) ts.tv_nsec = 0;
    if (ts.tv_nsec >= 1000000000L) ts.tv_nsec = 999999999L;

    /* Resume across EINTR unless a signal asked us to stop. */
    struct timespec rem;
    while (nanosleep(&ts, &rem) != 0) {
        if (errno != EINTR) break;
        if (stop_flag) return;
        ts = rem;
    }
}

static int term_cols(void) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return (int)ws.ws_col;
    }
    return 80;
}

static void print_header(double interval, const char *cmd) {
    int cols = term_cols();
    if (cols < 20) cols = 20;

    char host[256];
    if (gethostname(host, sizeof(host)) != 0) {
        strcpy(host, "localhost");
    }
    host[sizeof(host) - 1] = '\0';

    time_t now = time(NULL);
    struct tm tmv;
    char tbuf[64];
    if (localtime_r(&now, &tmv) != NULL) {
        strftime(tbuf, sizeof(tbuf), "%a %b %e %H:%M:%S %Y", &tmv);
    } else {
        strcpy(tbuf, "----");
    }

    /* Left: "Every N.Ns: <cmd>".  Right: "<host>: <time>". */
    char left[1024];
    snprintf(left, sizeof(left), "Every %.1fs: %s", interval, cmd);

    char right[512];
    snprintf(right, sizeof(right), "%s: %s", host, tbuf);

    int left_len  = (int)strlen(left);
    int right_len = (int)strlen(right);

    /* If combined exceeds width, truncate the left (command) with "...". */
    if (left_len + 1 + right_len > cols) {
        int max_left = cols - right_len - 4; /* space + "..." */
        if (max_left < 5) max_left = 5;
        if (left_len > max_left) {
            left[max_left - 3] = '.';
            left[max_left - 2] = '.';
            left[max_left - 1] = '.';
            left[max_left]     = '\0';
            left_len = max_left;
        }
    }

    int pad = cols - left_len - right_len;
    if (pad < 1) pad = 1;

    fputs(left, stdout);
    for (int i = 0; i < pad; i++) fputc(' ', stdout);
    fputs(right, stdout);
    fputs("\n\n", stdout);
}

int main(int argc, char **argv) {
    double interval = 2.0;
    int no_title = 0;
    int i = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Flag parsing loop. */
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) { usage(stderr); return 1; }
            char *end = NULL;
            double v = strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || v <= 0.0) {
                fprintf(stderr, "watch: invalid interval '%s'\n", argv[i]);
                return 1;
            }
            interval = v;
        } else if (strcmp(argv[i], "-t") == 0) {
            no_title = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "watch: unknown option '%s'\n", argv[i]);
            usage(stderr);
            return 1;
        } else {
            break; /* start of command */
        }
    }

    if (i >= argc) { usage(stderr); return 1; }

    char *cmd = join_argv(argc, argv, i);
    if (!cmd) {
        fprintf(stderr, "watch: out of memory\n");
        return 1;
    }

    for (;;) {
        /* Clear screen + home cursor. */
        fputs("\033[2J\033[H", stdout);
        if (!no_title) {
            print_header(interval, cmd);
        }
        fflush(stdout);

        FILE *pipe = popen(cmd, "r");
        if (!pipe) {
            fprintf(stderr, "watch: popen failed: %s\n", cmd);
            free(cmd);
            return 2;
        }

        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) {
            fwrite(buf, 1, n, stdout);
        }
        pclose(pipe);
        fflush(stdout);

        if (stop_flag) break;
        sleep_fractional(interval);
        if (stop_flag) break;
    }

    fputc('\n', stdout);
    fflush(stdout);
    free(cmd);
    return 0;
}
