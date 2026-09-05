#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "doomdef.h"
#include "doomstat.h"
#include "i_printf.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_fixed.h"
#include "r_draw.h"
#include "r_main.h"
#include "r_plane.h"
#include "st_stuff.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "e1-framebuffer.h"

extern volatile sig_atomic_t e1_exit_requested;

fixed_t fractionaltic;
boolean dynamic_resolution;
boolean uncapped;
boolean resetneeded;
boolean setrefreshneeded;
boolean toggle_fullscreen;
boolean toggle_exclusive_fullscreen;
boolean correct_aspect_ratio = true;
boolean screenvisible = true;
boolean drs_skip_frame;
int current_video_height = 200;
int fps;
int custom_fov = FOV_DEFAULT;
int gamma2 = 9;

static byte palette_rgb[256][3];
static uint64_t start_us;
static uint64_t next_report_us;
static uint64_t next_present_us;
static uint32_t previous_hash;
static unsigned long frame_count;
static unsigned long changed_hashes;
static unsigned int present_frame_rate = E1_STREAM_DEFAULT_FRAME_RATE;
static int run_seconds = 60;
static int framebuffer_fd = -1;
static struct e1_framebuffer *shared_framebuffer;
static char game_ready_path[512];
static boolean game_ready_signaled;

static void InitGameReadyMarker(void)
{
    const char *state = getenv("E1_DOOM_RUNTIME_STATE");

    if (state != NULL && state[0] == '/' &&
        snprintf(game_ready_path, sizeof(game_ready_path), "%s/game.ready",
                 state) > 0) {
        (void)unlink(game_ready_path);
    } else {
        game_ready_path[0] = '\0';
    }
}

static void SignalGameReady(void)
{
    char temporary[560];
    char contents[64];
    int descriptor;
    int content_length;

    if (game_ready_signaled || game_ready_path[0] == '\0' ||
        snprintf(temporary, sizeof(temporary), "%s.%ld", game_ready_path,
                 (long)getpid()) <= 0) {
        return;
    }
    content_length = snprintf(contents, sizeof(contents),
                              "pid=%ld frame=%u\n", (long)getpid(),
                              shared_framebuffer->frame_sequence);
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                                    O_NOFOLLOW, 0600);
    if (descriptor < 0 || content_length <= 0 ||
        write(descriptor, contents, (size_t)content_length) != content_length) {
        if (descriptor >= 0) (void)close(descriptor);
        (void)unlink(temporary);
        I_Error("unable to publish game-ready marker");
    }
    if (close(descriptor) != 0 || rename(temporary, game_ready_path) != 0) {
        (void)unlink(temporary);
        I_Error("unable to commit game-ready marker");
    }
    game_ready_signaled = true;
    I_Printf(VB_ALWAYS, "E1_GAME_READY pid=%ld frame=%u\n", (long)getpid(),
             shared_framebuffer->frame_sequence);
}

static void BeginFrameWrite(void)
{
    struct e1_frame_slot *slot;
    uint32_t sequence;

    if (!shared_framebuffer)
    {
        return;
    }
    slot = &shared_framebuffer->slot[0];
    sequence = (slot->sequence & ~1U) + 1U;
    slot->sequence = sequence;
    e1_frame_barrier();
}

static void InitFramePublisher(void)
{
    const char *path = getenv("E1_DOOM_FRAMEBUFFER");

    if (!path || !*path)
    {
        return;
    }
    framebuffer_fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC |
                                O_NOFOLLOW, 0600);
    if (framebuffer_fd < 0 ||
        ftruncate(framebuffer_fd, (off_t)sizeof(*shared_framebuffer)) != 0)
    {
        I_Error("unable to create shared framebuffer %s", path);
    }
    shared_framebuffer = mmap(NULL, sizeof(*shared_framebuffer),
                              PROT_READ | PROT_WRITE, MAP_SHARED,
                              framebuffer_fd, 0);
    if (shared_framebuffer == MAP_FAILED)
    {
        shared_framebuffer = NULL;
        I_Error("unable to map shared framebuffer %s", path);
    }
    memset(shared_framebuffer, 0, sizeof(*shared_framebuffer));
    shared_framebuffer->magic = E1_FRAME_MAGIC;
    shared_framebuffer->version = E1_FRAME_VERSION;
    shared_framebuffer->structure_size = sizeof(*shared_framebuffer);
    e1_frame_barrier();
}

static void PublishFrame(void)
{
    struct e1_frame_slot *slot;
    uint32_t sequence;

    if (!shared_framebuffer)
    {
        return;
    }
    slot = &shared_framebuffer->slot[0];
    sequence = slot->sequence;
    if ((sequence & 1U) == 0U)
    {
        sequence = (sequence & ~1U) + 1U;
        slot->sequence = sequence;
        e1_frame_barrier();
    }

    slot->width = (uint32_t)video.width;
    slot->height = (uint32_t)video.height;
    slot->pitch = (uint32_t)video.pitch;
    memcpy(slot->palette, palette_rgb, sizeof(slot->palette));

    e1_frame_barrier();
    slot->sequence = sequence + 1U;
    e1_frame_barrier();
    shared_framebuffer->active_slot = 0;
    ++shared_framebuffer->frame_sequence;
    e1_frame_barrier();
    SignalGameReady();
}

static uint32_t FrameHash(void)
{
    uint32_t hash = UINT32_C(2166136261);
    for (int y = 0; y < video.height; ++y)
    {
        const byte *row = I_VideoBuffer + (size_t)y * video.pitch;
        /* This is a once-per-second liveness probe, not a content digest.
         * Sampling every eighth pixel preserves freeze/change detection while
         * avoiding millions of 64-bit multiplies on the 32-bit Cortex-A7. */
        for (int x = 0; x < video.width; x += 8)
        {
            hash ^= row[x];
            hash *= UINT32_C(16777619);
        }
    }
    return hash;
}

static void ConfigureResolution(int height)
{
    current_video_height = height;
    video.height = height;
    video.width = height * 32 / 15;
    /* Woof's 16:9 logical viewport is 426x200 (displayed as 426x240).
     * This is intentionally independent of the render scale.  Setting it to
     * the 640-pixel render width over-expands the FOV, mis-scales the status
     * bar, and leaves the weapon sprite floating above it. */
    video.unscaledw = 426;
    correct_aspect_ratio = true;
    video.pitch = (video.width + 3) & ~3;
    video.deltaw = (video.unscaledw - NONWIDEWIDTH) / 2;

    if (shared_framebuffer)
    {
        I_VideoBuffer = shared_framebuffer->slot[0].pixels;
        memset(I_VideoBuffer, 0, (size_t)video.pitch * (size_t)video.height);
        BeginFrameWrite();
    }
    else
    {
        free(I_VideoBuffer);
        I_VideoBuffer = calloc((size_t)video.pitch, (size_t)video.height);
    }
    if (!I_VideoBuffer)
    {
        I_Error("unable to allocate %dx%d indexed framebuffer", video.width,
                video.height);
    }

    V_Init();
    V_RestoreBuffer();
    R_InitVisplanesRes();
    R_SetFuzzColumnMode();
    R_InitAnyRes();
    ST_InitRes();
    setsizeneeded = true;
    drs_skip_frame = true;
}

void I_InitGraphics(void)
{
    const char *height_env = getenv("E1_DOOM_HEIGHT");
    const char *frame_rate_env = getenv("E1_DOOM_FPS");
    const char *seconds_env = getenv("E1_DOOM_RUN_SECONDS");
    int height = height_env ? atoi(height_env) : 300;

    if (height != 200 && height != 300)
    {
        I_Error("E1_DOOM_HEIGHT must be 200 or 300");
    }
    if (seconds_env)
    {
        run_seconds = atoi(seconds_env);
    }
    if (run_seconds < 0)
    {
        I_Error("E1_DOOM_RUN_SECONDS must be zero (unlimited) or positive");
    }
    if (frame_rate_env && *frame_rate_env)
    {
        int configured_frame_rate = atoi(frame_rate_env);

        if (configured_frame_rate >= 1 && configured_frame_rate <= 30)
        {
            present_frame_rate = (unsigned int)configured_frame_rate;
        }
    }

    InitGameReadyMarker();
    InitFramePublisher();
    ConfigureResolution(height);
    I_SetPalette(W_CacheLumpName("PLAYPAL", PU_CACHE));
    I_AtExit(I_ShutdownGraphics, true);
    start_us = I_GetTimeUS();
    next_report_us = start_us;
    next_present_us = start_us;
    I_Printf(VB_ALWAYS,
             "E1_HEADLESS_START width=%d height=%d pitch=%d fps=%u seconds=%d\n",
             video.width, video.height, video.pitch, present_frame_rate,
             run_seconds);
}

void I_ShutdownGraphics(void)
{
    const uint64_t elapsed = start_us ? I_GetTimeUS() - start_us : 0;
    I_Printf(VB_ALWAYS,
             "E1_HEADLESS_DONE frames=%lu changed_hashes=%lu elapsed_ms=%llu\n",
             frame_count, changed_hashes,
             (unsigned long long)(elapsed / 1000ull));
    if (!shared_framebuffer)
    {
        free(I_VideoBuffer);
    }
    I_VideoBuffer = NULL;
    if (shared_framebuffer)
    {
        (void)munmap(shared_framebuffer, sizeof(*shared_framebuffer));
        shared_framebuffer = NULL;
    }
    if (framebuffer_fd >= 0)
    {
        (void)close(framebuffer_fd);
        framebuffer_fd = -1;
    }
    if (game_ready_signaled && game_ready_path[0] != '\0')
    {
        (void)unlink(game_ready_path);
        game_ready_signaled = false;
    }
}

void I_FinishUpdate(void)
{
    uint64_t now = I_GetTimeUS();
    uint64_t deadline;
    ++frame_count;

    if (now > next_present_us)
    {
        next_present_us = now;
    }
    /* The selected 640x360 H.264 substream is natively 10 fps. Rendering
     * additional software frames only burns CPU; game tics still advance from
     * the monotonic timer and may process more than one tic per presentation. */
    PublishFrame();

    if (now >= next_report_us)
    {
        const uint32_t hash = FrameHash();
        if (previous_hash && previous_hash != hash)
        {
            ++changed_hashes;
        }
        previous_hash = hash;
        I_Printf(VB_ALWAYS,
                 "E1_FRAME t_ms=%llu frame=%lu gametic=%d hash=%08x\n",
                 (unsigned long long)((now - start_us) / 1000ull), frame_count,
                 gametic, hash);
        next_report_us = now + 1000000ull;
    }

    if (e1_exit_requested ||
        (run_seconds > 0 &&
         now - start_us >= (uint64_t)run_seconds * 1000000ull))
    {
        I_SafeExit(0);
    }

    deadline = next_present_us + 1000000ull / present_frame_rate;
    next_present_us = deadline;
    now = I_GetTimeUS();
    if (now < deadline)
    {
        I_SleepUS(deadline - now);
    }
    BeginFrameWrite();
}

void I_SetPalette(byte *palette)
{
    for (int i = 0; i < 256; ++i)
    {
        palette_rgb[i][0] = palette[i * 3];
        palette_rgb[i][1] = palette[i * 3 + 1];
        palette_rgb[i][2] = palette[i * 3 + 2];
    }
}

byte I_GetNearestColor(byte *palette, int r, int g, int b)
{
    unsigned int best_distance = ~0u;
    byte best = 0;
    for (int i = 0; i < 256; ++i)
    {
        const int dr = r - palette[i * 3];
        const int dg = g - palette[i * 3 + 1];
        const int db = b - palette[i * 3 + 2];
        const unsigned int distance = (unsigned int)(dr * dr + dg * dg + db * db);
        if (distance < best_distance)
        {
            best_distance = distance;
            best = (byte)i;
        }
    }
    return best;
}

void I_ReadScreen(byte *dst)
{
    V_GetBlock(0, 0, video.width, video.height, dst);
}

void I_ResetScreen(void)
{
    resetneeded = false;
    ConfigureResolution(current_video_height);
}

void I_GetResolutionScaling(resolution_scaling_t *rs)
{
    rs->max = 300;
    rs->step = 100;
}

void I_ToggleVsync(void) {}
void I_DynamicResolution(void) {}
void I_InitWindowIcon(void) {}
void I_ShowMouseCursor(boolean toggle) { (void)toggle; }
void I_ResetRelativeMouseState(void) {}
void I_UpdatePriority(boolean active) { (void)active; }
void I_BindVideoVariables(void) {}
void I_BeginRead(unsigned int bytes) { (void)bytes; }
void I_EndRead(void) {}
boolean I_WritePNGfile(char *filename) { (void)filename; return false; }
void *I_GetSDLWindow(void) { return NULL; }
void *I_GetSDLRenderer(void) { return NULL; }
