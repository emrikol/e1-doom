#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define BUILD_ID "e1-ptz-hook-owner-dummy-v1"

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

int main(void)
{
    struct sigaction action;

    action.sa_handler = request_stop;
    action.sa_flags = 0;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGHUP, &action, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("owner-ready build_id=%s pid=%ld\n", BUILD_ID, (long)getpid());
    while (!stop_requested) {
        struct timespec delay = {1, 0};

        while (nanosleep(&delay, &delay) != 0 && errno == EINTR &&
               !stop_requested) {
        }
    }
    return 0;
}
