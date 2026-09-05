#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "i_printf.h"
#include "m_argv.h"
#include "version.h"

void D_DoomMain(void);

volatile sig_atomic_t e1_exit_requested;

static void RequestExit(int sig)
{
    (void)sig;
    e1_exit_requested = 1;
}

static int HasArg(int argc, char **argv, const char *needle)
{
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], needle))
        {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *iwad = getenv("E1_DOOM_IWAD");
    char **camera_argv = calloc((size_t)argc + 16, sizeof(*camera_argv));
    int camera_argc = 0;

    if (!camera_argv)
    {
        return 2;
    }

    signal(SIGINT, RequestExit);
    signal(SIGTERM, RequestExit);

    for (int i = 0; i < argc; ++i)
    {
        camera_argv[camera_argc++] = argv[i];
    }

    if (!HasArg(argc, argv, "-iwad"))
    {
        camera_argv[camera_argc++] = "-iwad";
        camera_argv[camera_argc++] = (char *)(iwad ? iwad : "/share/doom/doom1.wad");
    }
    camera_argv[camera_argc++] = "-nogui";
    camera_argv[camera_argc] = NULL;

    myargc = camera_argc;
    myargv = camera_argv;
    I_Printf(VB_ALWAYS, "%s (built on %s; e1 headless)\n", PROJECT_STRING,
             version_date);
    D_DoomMain();
    free(camera_argv);
    return 0;
}
