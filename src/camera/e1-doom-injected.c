#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "hdal.h"
#include "e1-audio-contract.h"
#include "e1-audio-ring.h"
#include "e1-device-functions.h"
#include "e1-device-signatures.h"
#include "e1-frame-convert.h"
#include "e1-framebuffer.h"
#include "e1-melt.h"
#include "e1-ptz-event.h"
#include "e1-runtime-control.h"

#define BUILD_ID "e1-doom-injected-v27"
#define STATE_DIR "/mnt/tmp/e1-doom/state"
#define DEVICE_TARGET "/mnt/app/device"
#define COMMAND_PATH STATE_DIR "/injected.command"
#define DASHBOARD_START_PATH STATE_DIR "/dashboard.start"
#define DASHBOARD_CAMERA_PATH STATE_DIR "/dashboard.camera"
#define GAME_READY_PATH STATE_DIR "/game.ready"
#define HEARTBEAT_PATH STATE_DIR "/heartbeat"
#define LOG_PATH STATE_DIR "/injected.log"
#define HEARTBEAT_TIMEOUT_MS UINT64_C(4000)
#define PTZ_RING_SIZE 64U
#define DEVICE_MANAGER_POINTER_ADDRESS UINT32_C(0x00359be0)
#define MANAGER_AENC_PATH_OFFSET UINT32_C(0x66c54)
#define MANAGER_AUDIO_HARDWARE_OFFSET UINT32_C(0x66cb0)
#define AUDIO_TONE_FRAMES 8U
#define STOCK_AUDIO_TONE_FRAMES 256U
#define AUDIO_RING_PATH "/mnt/tmp/e1-doom/audio.pcm"

enum audio_pcm_mode {
    AUDIO_PCM_PASS_THROUGH = 0,
    AUDIO_PCM_TONE = 1,
    AUDIO_PCM_DOOM = 2,
};

enum ptz_hook_mode {
    PTZ_HOOK_CAMERA = 0,
    PTZ_HOOK_DOOM = 1,
    PTZ_HOOK_OFF = 2,
};

struct ptz_ring_slot {
    atomic_uint committed;
    int32_t channel;
    int32_t command;
    int32_t speed;
};

extern void e1_ptz_hook_entry(void);
extern void e1_audio_hook_entry(void);

__attribute__((visibility("hidden"))) uintptr_t e1_ptz_continuation;
__attribute__((visibility("hidden"))) uintptr_t e1_audio_continuation;
static struct ptz_ring_slot ptz_ring[PTZ_RING_SIZE];
static atomic_uint ptz_write_ticket;
static atomic_uint ptz_active_calls;
static atomic_int ptz_mode;
static atomic_uint audio_pcm_active_calls;
static atomic_uint audio_pcm_frames;
static atomic_int audio_pcm_mode;
static atomic_int audio_pcm_readback_positive;
static atomic_int audio_pcm_readback_negative;
static _Atomic(struct e1_audio_ring *) audio_pcm_ring;
static atomic_uint audio_pcm_last_frame;
static atomic_uint audio_pcm_dropped_frames;
static atomic_uint audio_pcm_underflows;

/* Called from the stock audio thread immediately before its existing AAC
 * encoder. This path must remain bounded and lock-free. */
void e1_audio_pcm_hook_route(void *pcm, int bytes)
{
    int mode;

    (void)atomic_fetch_add_explicit(&audio_pcm_active_calls, 1U,
                                    memory_order_acquire);
    mode = atomic_load_explicit(&audio_pcm_mode, memory_order_acquire);
    if (pcm != NULL && bytes == (int)E1_AUDIO_PCM_BYTES_PER_FRAME &&
        mode == AUDIO_PCM_TONE) {
        unsigned int frame = atomic_fetch_add_explicit(
            &audio_pcm_frames, 1U, memory_order_relaxed);

        if (frame < STOCK_AUDIO_TONE_FRAMES) {
            e1_fill_tone_1khz_s16(pcm, E1_AUDIO_SAMPLES_PER_FRAME);
            atomic_store_explicit(&audio_pcm_readback_positive,
                                  ((const int16_t *)pcm)[4],
                                  memory_order_relaxed);
            atomic_store_explicit(&audio_pcm_readback_negative,
                                  ((const int16_t *)pcm)[12],
                                  memory_order_release);
        } else {
            atomic_store_explicit(&audio_pcm_mode, AUDIO_PCM_PASS_THROUGH,
                                  memory_order_release);
        }
    } else if (pcm != NULL && bytes == (int)E1_AUDIO_PCM_BYTES_PER_FRAME &&
               mode == AUDIO_PCM_DOOM) {
        struct e1_audio_ring *ring = atomic_load_explicit(
            &audio_pcm_ring, memory_order_acquire);
        uint32_t last = atomic_load_explicit(&audio_pcm_last_frame,
                                             memory_order_relaxed);
        uint32_t dropped = 0;
        int status = ring == NULL
                         ? -1
                         : e1_audio_ring_read_next(ring, &last, pcm,
                                                   &dropped);

        if (status == 1) {
            atomic_store_explicit(&audio_pcm_last_frame, last,
                                  memory_order_relaxed);
            (void)atomic_fetch_add_explicit(&audio_pcm_frames, 1U,
                                            memory_order_relaxed);
            (void)atomic_fetch_add_explicit(&audio_pcm_dropped_frames,
                                            dropped,
                                            memory_order_relaxed);
        } else {
            /* Doom mode owns the stream completely.  Never leak microphone
             * PCM merely because the game queue had no newer frame. */
            memset(pcm, 0, E1_AUDIO_PCM_BYTES_PER_FRAME);
            (void)atomic_fetch_add_explicit(&audio_pcm_underflows, 1U,
                                            memory_order_relaxed);
        }
    }
    (void)atomic_fetch_sub_explicit(&audio_pcm_active_calls, 1U,
                                    memory_order_release);
}

typedef HD_RESULT (*mem_alloc_fn)(CHAR *, UINT32 *, void **, UINT32,
                                  HD_COMMON_MEM_DDR_ID);
typedef HD_RESULT (*mem_free_fn)(UINT32, void *);
typedef HD_RESULT (*videoenc_open_fn)(HD_IN_ID, HD_OUT_ID, HD_PATH_ID *);
typedef HD_RESULT (*videoenc_start_fn)(HD_PATH_ID);
typedef HD_RESULT (*videoenc_stop_fn)(HD_PATH_ID);
typedef HD_RESULT (*videoenc_close_fn)(HD_PATH_ID);
typedef HD_RESULT (*videoenc_set_fn)(HD_PATH_ID, HD_VIDEOENC_PARAM_ID, void *);
typedef void *(*mem_mmap_fn)(HD_COMMON_MEM_MEM_TYPE, UINT32, UINT32);
typedef HD_RESULT (*mem_munmap_fn)(void *, unsigned int);
typedef HD_RESULT (*audioenc_open_fn)(HD_IN_ID, HD_OUT_ID, HD_PATH_ID *);
typedef HD_RESULT (*audioenc_start_fn)(HD_PATH_ID);
typedef HD_RESULT (*audioenc_stop_fn)(HD_PATH_ID);
typedef HD_RESULT (*audioenc_close_fn)(HD_PATH_ID);
typedef HD_RESULT (*audioenc_get_fn)(HD_PATH_ID, HD_AUDIOENC_PARAM_ID, void *);
typedef HD_RESULT (*audioenc_set_fn)(HD_PATH_ID, HD_AUDIOENC_PARAM_ID, void *);
typedef HD_RESULT (*audioenc_push_fn)(HD_PATH_ID, HD_AUDIO_FRAME *,
                                      HD_AUDIO_BS *, INT32);
typedef HD_RESULT (*audioenc_pull_fn)(HD_PATH_ID, HD_AUDIO_BS *, INT32);
typedef HD_RESULT (*audioenc_release_fn)(HD_PATH_ID, HD_AUDIO_BS *);

static int supported_ptz_command(int32_t command)
{
    if (command >= 0 && command <= 14) {
        return 1;
    }
    switch (command) {
    case 18:
    case 19:
    case 20:
    case 21:
    case 25:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
        return 1;
    default:
        return 0;
    }
}

/* Called from the assembly trampoline on an arbitrary device thread. Keep
 * this path to lock-free memory operations: the worker owns file and socket
 * I/O. Return one only when the original handler should be skipped. */
int e1_ptz_hook_route(int32_t channel, int32_t command, int32_t speed)
{
    int consume = 0;

    (void)atomic_fetch_add_explicit(&ptz_active_calls, 1U,
                                    memory_order_acquire);
    if (supported_ptz_command(command) &&
        atomic_load_explicit(&ptz_mode, memory_order_relaxed) != PTZ_HOOK_OFF) {
        unsigned int ticket = atomic_fetch_add_explicit(
            &ptz_write_ticket, 1U, memory_order_relaxed);
        struct ptz_ring_slot *slot = &ptz_ring[ticket % PTZ_RING_SIZE];

        slot->channel = channel;
        slot->command = command;
        slot->speed = speed;
        atomic_store_explicit(&slot->committed, ticket + 1U,
                              memory_order_release);
        if (atomic_load_explicit(&ptz_mode, memory_order_relaxed) ==
            PTZ_HOOK_DOOM) {
            consume = 1;
        }
    }
    (void)atomic_fetch_sub_explicit(&ptz_active_calls, 1U,
                                    memory_order_release);
    return consume;
}

struct media_state {
    UINT32 media_pa;
    void *media_va;
    UINT32 media_size;
    HD_PATH_ID path;
    uint16_t *pixels;
    uint16_t *doom_pixels;
    unsigned int width;
    unsigned int height;
    unsigned int encoder;
    unsigned int phase;
    int allocated;
    int path_open;
    int path_started;
    int checkerboard;
    int doom_frames;
    int entry_melt;
    int frame_fd;
    const struct e1_framebuffer *framebuffer;
    uint32_t last_frame_sequence;
    uint32_t video_source_frames;
    uint32_t video_source_drops;
    uint32_t video_published_frames;
    uint32_t video_publish_errors;
    uint32_t video_retries;
    uint64_t video_stats_started_ms;
    unsigned int source_width;
    unsigned int source_height;
    uint16_t x_map[E1_OSG_WIDTH];
    uint16_t y_map[E1_OSG_HEIGHT];
    struct e1_melt_state melt;
    uint64_t melt_last_ms;
    uint64_t melt_tick_remainder;
};

struct audio_state {
    UINT32 pcm_pa;
    void *pcm_va;
    UINT32 pcm_size;
    HD_PATH_ID path;
    UINT32 output_pa;
    UINT32 output_size;
    void *output_va;
    uint64_t first_timestamp;
    uint64_t last_timestamp;
    uint32_t first_size;
    uint32_t last_size;
    uint32_t rolling_hash;
    unsigned int frames;
    int pcm_allocated;
    int path_open;
    int path_started;
    int output_mapped;
    int stock_hook_active;
    int stock_hook_used;
    enum audio_pcm_mode stock_hook_mode;
    int ring_fd;
    struct e1_audio_ring *ring;
};

static mem_alloc_fn mem_alloc_call;
static mem_free_fn mem_free_call;
static videoenc_open_fn videoenc_open_call;
static videoenc_start_fn videoenc_start_call;
static videoenc_stop_fn videoenc_stop_call;
static videoenc_close_fn videoenc_close_call;
static videoenc_set_fn videoenc_set_call;
static mem_mmap_fn mem_mmap_call;
static mem_munmap_fn mem_munmap_call;
static audioenc_open_fn audioenc_open_call;
static audioenc_start_fn audioenc_start_call;
static audioenc_stop_fn audioenc_stop_call;
static audioenc_close_fn audioenc_close_call;
static audioenc_get_fn audioenc_get_call;
static audioenc_set_fn audioenc_set_call;
static audioenc_push_fn audioenc_push_call;
static audioenc_pull_fn audioenc_pull_call;
static audioenc_release_fn audioenc_release_call;
/* The normal runtime substitutes PCM in the stock AAC path and does not need
 * the private device-manager layout. Keep the older native-AAC diagnostic
 * command fail-closed until that object layout is independently resolved. */
static int native_audio_manager_abi_valid;

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static unsigned int read_stream_frame_rate(void)
{
    FILE *stream = fopen(STATE_DIR "/video.fps", "r");
    unsigned int frame_rate = E1_STREAM_DEFAULT_FRAME_RATE;

    if (stream != NULL) {
        unsigned int configured = 0;

        if (fscanf(stream, "%u", &configured) == 1 &&
            configured >= 1U && configured <= 30U) {
            frame_rate = configured;
        }
        (void)fclose(stream);
    }
    return frame_rate;
}

static uint64_t monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static void log_line(const char *message, long value)
{
    int descriptor = open(LOG_PATH,
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    char buffer[192];
    int length;

    if (descriptor < 0) {
        return;
    }
    length = snprintf(buffer, sizeof(buffer), "%llu %s %ld\n",
                      (unsigned long long)monotonic_ms(), message, value);
    if (length > 0 && (size_t)length < sizeof(buffer)) {
        (void)write(descriptor, buffer, (size_t)length);
    }
    (void)close(descriptor);
}

static void log_ptz_event(const struct e1_ptz_event *event)
{
    int descriptor = open(STATE_DIR "/ptz-events.log",
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    char buffer[192];
    int length;

    if (descriptor < 0) {
        return;
    }
    length = snprintf(buffer, sizeof(buffer),
                      "%llu sequence=%u channel=%d command=%d speed=%d\n",
                      (unsigned long long)event->monotonic_ms,
                      event->sequence, event->channel, event->command,
                      event->speed);
    if (length > 0 && (size_t)length < sizeof(buffer)) {
        (void)write(descriptor, buffer, (size_t)length);
    }
    (void)close(descriptor);
}

static void log_ptz_runtime(const struct e1_ptz_event *event,
                            const struct e1_runtime_control *control,
                            unsigned int action)
{
    int descriptor = open(STATE_DIR "/ptz-runtime.log",
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    char buffer[192];
    int length;

    if (descriptor < 0) {
        return;
    }
    length = snprintf(buffer, sizeof(buffer),
                      "sequence=%u command=%d mode=%d index=%u down=%d "
                      "complete=%d stop_only=%u raw_direction=%d action=%#x\n",
                      event->sequence, event->command, (int)control->mode,
                      control->secret_index, control->camera_control_down,
                      control->secret_complete, control->stop_only_index,
                      control->raw_direction_since_stop, action);
    if (length > 0 && (size_t)length < sizeof(buffer)) {
        (void)write(descriptor, buffer, (size_t)length);
    }
    (void)close(descriptor);
}

static int open_ptz_sender(void)
{
    return socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
}

static void drain_ptz_events(unsigned int *read_ticket, int sender,
                             struct e1_runtime_control *control,
                             unsigned int *actions)
{
    unsigned int drained;

    for (drained = 0; drained < PTZ_RING_SIZE; ++drained) {
        unsigned int expected = *read_ticket + 1U;
        struct ptz_ring_slot *slot =
            &ptz_ring[*read_ticket % PTZ_RING_SIZE];
        unsigned int committed = atomic_load_explicit(
            &slot->committed, memory_order_acquire);
        struct e1_ptz_event event;

        if (committed == 0U || committed < expected) {
            break;
        }
        if (committed != expected) {
            log_line("ptz-ring-overrun", (long)(committed - expected));
            *read_ticket = committed - 1U;
            continue;
        }
        memset(&event, 0, sizeof(event));
        event.magic = E1_PTZ_EVENT_MAGIC;
        event.version = E1_PTZ_EVENT_VERSION;
        event.sequence = committed;
        event.channel = slot->channel;
        event.command = slot->command;
        event.speed = slot->speed;
        event.monotonic_ms = monotonic_ms();
        log_ptz_event(&event);
        {
            enum e1_runtime_mode mode_before = control->mode;
            unsigned int action = e1_runtime_observe_raw_ptz(
                control, event.command, event.monotonic_ms);

            action |= e1_runtime_apply_ptz(
                control, event.command, event.monotonic_ms);

            /* Treat each decoded non-stop dispatcher call as a complete tap
             * only for camera-mode lifecycle recognition. Doom sees the raw
             * press and its input adapter releases it with the 350 ms deadman;
             * synthesizing a runtime stop there would break hold-to-exit. */
            if (mode_before == E1_RUNTIME_CAMERA_ARMED &&
                event.command != 0) {
                action |= e1_runtime_apply_ptz(
                    control, 0, event.monotonic_ms + 1U);
            }
            *actions |= action;
            log_ptz_runtime(&event, control, action);
            if ((action & E1_RUNTIME_ACTION_ENTER) != 0U) {
                log_line("secret-code-complete", (long)event.sequence);
            }
        }
        if (sender >= 0 &&
            (control->mode == E1_RUNTIME_DOOM ||
             (*actions & E1_RUNTIME_ACTION_FORWARD) != 0U)) {
            struct sockaddr_un destination;

            memset(&destination, 0, sizeof(destination));
            destination.sun_family = AF_UNIX;
            (void)snprintf(destination.sun_path,
                           sizeof(destination.sun_path), "%s",
                           E1_PTZ_SOCKET_PATH);
            (void)sendto(sender, &event, sizeof(event), MSG_DONTWAIT,
                         (const struct sockaddr *)(const void *)&destination,
                         sizeof(destination));
        }
        *read_ticket = committed;
    }
}

static int write_marker(const char *name, const char *value)
{
    char path[160];
    int descriptor;
    size_t length = strlen(value);

    if (snprintf(path, sizeof(path), "%s/%s", STATE_DIR, name) >=
        (int)sizeof(path)) {
        return -1;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return -1;
    }
    if (write(descriptor, value, length) != (ssize_t)length ||
        write(descriptor, "\n", 1) != 1 || close(descriptor) != 0) {
        (void)close(descriptor);
        return -1;
    }
    return 0;
}

static void remove_marker(const char *name)
{
    char path[160];

    if (snprintf(path, sizeof(path), "%s/%s", STATE_DIR, name) <
        (int)sizeof(path)) {
        (void)unlink(path);
    }
}

static int resolve_self_device_sites(void)
{
    static const unsigned char ptz_prologue[8] = {
        0x2d, 0xe9, 0xf0, 0x4f, 0x04, 0x46, 0x15, 0x46
    };
    static const unsigned char audio_prologue[8] = {
        0x10, 0xb5, 0x00, 0xf5, 0xcc, 0x20, 0x00, 0x2a
    };
    char line[256];
    struct e1_device_site ptz_site = {0};
    struct e1_device_site audio_site = {0};
    struct e1_device_functions functions = {0};
    FILE *maps;
    int ptz_found = 0;
    int audio_found = 0;
    int functions_found = 0;

    maps = fopen("/proc/self/maps", "r");
    if (maps == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long start;
        unsigned long end;
        char permissions[5] = {0};
        struct e1_device_site candidate;
        struct e1_device_functions function_candidate;
        int found;

        if (sscanf(line, "%lx-%lx %4s", &start, &end, permissions) != 3 ||
            permissions[0] != 'r' || permissions[2] != 'x' ||
            strstr(line, DEVICE_TARGET) == NULL || end <= start ||
            end - start > UINT32_C(32 * 1024 * 1024)) {
            continue;
        }
        found = e1_find_device_site((const unsigned char *)(uintptr_t)start,
                                    (size_t)(end - start), (uintptr_t)start,
                                    E1_DEVICE_SITE_PTZ, &candidate);
        if (found < 0 || (found == 1 && ptz_found != 0)) {
            (void)fclose(maps);
            return -1;
        }
        if (found == 1) {
            ptz_site = candidate;
            ptz_found = 1;
        }
        found = e1_find_device_site((const unsigned char *)(uintptr_t)start,
                                    (size_t)(end - start), (uintptr_t)start,
                                    E1_DEVICE_SITE_AUDIO, &candidate);
        if (found < 0 || (found == 1 && audio_found != 0)) {
            (void)fclose(maps);
            return -1;
        }
        if (found == 1) {
            audio_site = candidate;
            audio_found = 1;
        }
        found = e1_resolve_device_functions(
            (const unsigned char *)(uintptr_t)start, (size_t)(end - start),
            (uintptr_t)start, &function_candidate);
        if (found == 1) {
            if (functions_found != 0) {
                (void)fclose(maps);
                return -1;
            }
            functions = function_candidate;
            functions_found = 1;
        }
    }
    (void)fclose(maps);
    if (ptz_found != 1 || audio_found != 1 || functions_found != 1 ||
        memcmp((const void *)ptz_site.address, ptz_prologue,
               sizeof(ptz_prologue)) != 0 ||
        memcmp((const void *)audio_site.address, audio_prologue,
               sizeof(audio_prologue)) != 0) {
        return -1;
    }
    e1_ptz_continuation = (ptz_site.address + 8U) | 1U;
    e1_audio_continuation = (audio_site.address + 8U) | 1U;
    {
        uintptr_t address;

#define E1_ASSIGN_THUMB_FUNCTION(destination, kind)                         \
    do {                                                                    \
        address = functions.address[(kind)] | (uintptr_t)1U;                \
        memcpy(&(destination), &address, sizeof(destination));              \
    } while (0)
        E1_ASSIGN_THUMB_FUNCTION(mem_alloc_call,
                                 E1_DEVICE_FUNCTION_MEM_ALLOC);
        E1_ASSIGN_THUMB_FUNCTION(mem_free_call, E1_DEVICE_FUNCTION_MEM_FREE);
        E1_ASSIGN_THUMB_FUNCTION(videoenc_open_call,
                                 E1_DEVICE_FUNCTION_VIDEOENC_OPEN);
        E1_ASSIGN_THUMB_FUNCTION(videoenc_start_call,
                                 E1_DEVICE_FUNCTION_VIDEOENC_START);
        E1_ASSIGN_THUMB_FUNCTION(videoenc_stop_call,
                                 E1_DEVICE_FUNCTION_VIDEOENC_STOP);
        E1_ASSIGN_THUMB_FUNCTION(videoenc_close_call,
                                 E1_DEVICE_FUNCTION_VIDEOENC_CLOSE);
        E1_ASSIGN_THUMB_FUNCTION(videoenc_set_call,
                                 E1_DEVICE_FUNCTION_VIDEOENC_SET);
        E1_ASSIGN_THUMB_FUNCTION(mem_mmap_call, E1_DEVICE_FUNCTION_MEM_MMAP);
        E1_ASSIGN_THUMB_FUNCTION(mem_munmap_call,
                                 E1_DEVICE_FUNCTION_MEM_MUNMAP);
        E1_ASSIGN_THUMB_FUNCTION(audioenc_open_call,
                                 E1_DEVICE_FUNCTION_AUDIOENC_OPEN);
        E1_ASSIGN_THUMB_FUNCTION(audioenc_start_call,
                                 E1_DEVICE_FUNCTION_AUDIOENC_START);
        E1_ASSIGN_THUMB_FUNCTION(audioenc_stop_call,
                                 E1_DEVICE_FUNCTION_AUDIOENC_STOP);
        E1_ASSIGN_THUMB_FUNCTION(audioenc_close_call,
                                 E1_DEVICE_FUNCTION_AUDIOENC_CLOSE);
        E1_ASSIGN_THUMB_FUNCTION(audioenc_get_call,
                                 E1_DEVICE_FUNCTION_AUDIOENC_GET);
        E1_ASSIGN_THUMB_FUNCTION(audioenc_set_call,
                                 E1_DEVICE_FUNCTION_AUDIOENC_SET);
        E1_ASSIGN_THUMB_FUNCTION(audioenc_push_call,
                                 E1_DEVICE_FUNCTION_AUDIOENC_PUSH);
        E1_ASSIGN_THUMB_FUNCTION(audioenc_pull_call,
                                 E1_DEVICE_FUNCTION_AUDIOENC_PULL);
        E1_ASSIGN_THUMB_FUNCTION(audioenc_release_call,
                                 E1_DEVICE_FUNCTION_AUDIOENC_RELEASE);
#undef E1_ASSIGN_THUMB_FUNCTION
    }
    log_line("ptz-site", (long)ptz_site.address);
    log_line("audio-site", (long)audio_site.address);
    log_line("mem-functions", (long)functions.address[E1_DEVICE_FUNCTION_MEM_MMAP]);
    log_line("video-functions",
             (long)functions.address[E1_DEVICE_FUNCTION_VIDEOENC_OPEN]);
    log_line("audio-functions",
             (long)functions.address[E1_DEVICE_FUNCTION_AUDIOENC_OPEN]);
    return 0;
}

static int validate_exact_code(void)
{
    if (resolve_self_device_sites() != 0) {
        log_line("device-site-resolution-failed", 0);
        return -1;
    }
    return 0;
}

static HD_IN_ID encoder_input(unsigned int encoder)
{
    switch (encoder) {
    case 0:
        return HD_VIDEOENC_0_IN_0;
    case 1:
        return HD_VIDEOENC_0_IN_1;
    case 2:
        return HD_VIDEOENC_0_IN_2;
    case 3:
        return HD_VIDEOENC_0_IN_3;
    default:
        return HD_VIDEOENC_0_IN_0;
    }
}

static UINT32 required_buffer_size(unsigned int width, unsigned int height)
{
    uint64_t line = ((uint64_t)width * 2U + 63U) & ~UINT64_C(63);
    uint64_t total = (line * height * 2U + 127U) & ~UINT64_C(127);

    if (total == 0 || total > UINT32_MAX) {
        return 0;
    }
    return (UINT32)total;
}

static void fill_pixels(struct media_state *state, int transparent)
{
    unsigned int x;
    unsigned int y;

    for (y = 0; y < state->height; ++y) {
        for (x = 0; x < state->width; ++x) {
            uint16_t value;
            if (transparent) {
                value = UINT16_C(0x0000);
            } else if ((((x / 32U) + (y / 32U) + state->phase) & 1U) == 0U) {
                value = UINT16_C(0xffff);
            } else {
                value = UINT16_C(0xf05f);
            }
            state->pixels[(size_t)y * state->width + x] = value;
        }
    }
}

static void close_frame_source(struct media_state *state)
{
    if (state->framebuffer != NULL) {
        (void)munmap((void *)state->framebuffer, sizeof(*state->framebuffer));
        state->framebuffer = NULL;
    }
    if (state->frame_fd >= 0) {
        (void)close(state->frame_fd);
        state->frame_fd = -1;
    }
    state->doom_frames = 0;
    state->last_frame_sequence = 0;
    state->source_width = 0;
    state->source_height = 0;
}

static int open_frame_source(struct media_state *state)
{
    struct stat metadata;
    const struct e1_framebuffer *framebuffer;
    struct timespec delay = {0, 5000000L};
    unsigned int attempt;
    uint32_t slot_index;
    const struct e1_frame_slot *slot;

    state->frame_fd = open(E1_FRAME_PATH,
                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (state->frame_fd < 0 || fstat(state->frame_fd, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) ||
        metadata.st_size != (off_t)sizeof(*framebuffer)) {
        close_frame_source(state);
        return -1;
    }
    framebuffer = mmap(NULL, sizeof(*framebuffer), PROT_READ, MAP_SHARED,
                       state->frame_fd, 0);
    if (framebuffer == MAP_FAILED) {
        close_frame_source(state);
        return -1;
    }
    state->framebuffer = framebuffer;
    e1_frame_barrier();
    if (framebuffer->magic != E1_FRAME_MAGIC ||
        framebuffer->version != E1_FRAME_VERSION ||
        framebuffer->structure_size != sizeof(*framebuffer) ||
        framebuffer->frame_sequence == 0) {
        close_frame_source(state);
        return -1;
    }
    slot_index = framebuffer->active_slot & 1U;
    slot = &framebuffer->slot[slot_index];
    for (attempt = 0; attempt < 40U && (slot->sequence & 1U) != 0U;
         ++attempt) {
        (void)nanosleep(&delay, NULL);
        slot_index = framebuffer->active_slot & 1U;
        slot = &framebuffer->slot[slot_index];
    }
    if ((slot->sequence & 1U) != 0U ||
        e1_build_scale_maps(slot->width, slot->height,
                            state->x_map, state->y_map) != 0 ||
        slot->pitch < slot->width ||
        (uint64_t)slot->pitch * slot->height > E1_FRAME_MAX_PIXELS) {
        close_frame_source(state);
        return -1;
    }
    state->source_width = slot->width;
    state->source_height = slot->height;
    state->doom_frames = 1;
    return 0;
}

/* Return zero for a stable new frame, one when there is no new stable frame,
 * and minus one for an invalid source. */
static int convert_doom_frame(struct media_state *state, uint16_t *output)
{
    const struct e1_framebuffer *framebuffer = state->framebuffer;
    const struct e1_frame_slot *slot;
    uint32_t frame_sequence;
    uint32_t slot_index;
    uint32_t slot_sequence;
    int status;

    if (framebuffer == NULL || output == NULL) {
        return -1;
    }
    frame_sequence = framebuffer->frame_sequence;
    if (frame_sequence == 0 || frame_sequence == state->last_frame_sequence) {
        ++state->video_retries;
        return 1;
    }
    slot_index = framebuffer->active_slot & 1U;
    slot = &framebuffer->slot[slot_index];
    slot_sequence = slot->sequence;
    e1_frame_barrier();
    if ((slot_sequence & 1U) != 0U) {
        ++state->video_retries;
        return 1;
    }
    if (slot->width != state->source_width ||
        slot->height != state->source_height || slot->pitch < slot->width ||
        (uint64_t)slot->pitch * slot->height > E1_FRAME_MAX_PIXELS) {
        return -1;
    }
    status = e1_convert_indexed_argb4444(
        output, slot->pixels, slot->width, slot->height, slot->pitch,
        slot->palette, state->x_map, state->y_map);
    e1_frame_barrier();
    if (status != 0 || slot->sequence != slot_sequence ||
        framebuffer->active_slot != slot_index ||
        framebuffer->frame_sequence != frame_sequence) {
        if (status == 0) {
            ++state->video_retries;
        }
        return status != 0 ? -1 : 1;
    }
    state->video_source_drops += e1_frame_sequence_drops(
        state->last_frame_sequence, frame_sequence);
    state->last_frame_sequence = frame_sequence;
    ++state->video_source_frames;
    return 0;
}

static int wait_for_stable_doom_frame(struct media_state *state,
                                      uint16_t *output)
{
    struct timespec delay = {0, 5000000L};
    unsigned int attempt;

    for (attempt = 0; attempt < 40U; ++attempt) {
        int status = convert_doom_frame(state, output);

        if (status == 0) {
            return 0;
        }
        (void)nanosleep(&delay, NULL);
    }
    log_line("stable-frame-timeout", 40);
    return -1;
}

static int publish_image(struct media_state *state)
{
    HD_OSG_STAMP_IMG image;
    HD_RESULT status;

    memset(&image, 0, sizeof(image));
    image.fmt = HD_VIDEO_PXLFMT_ARGB4444;
    image.dim.w = state->width;
    image.dim.h = state->height;
    image.p_addr = (UINT32)(uintptr_t)state->pixels;
    status = videoenc_set_call(state->path,
                               HD_VIDEOENC_PARAM_IN_STAMP_IMG, &image);
    log_line("set-image", (long)status);
    if (state->doom_frames) {
        if (status == HD_OK) {
            ++state->video_published_frames;
        } else {
            ++state->video_publish_errors;
        }
    }
    return status == HD_OK ? 0 : -1;
}

static int write_video_stats(const struct media_state *state)
{
    char marker[224];
    uint64_t now = monotonic_ms();
    uint64_t elapsed = state->video_stats_started_ms == 0U
                           ? 0U
                           : now - state->video_stats_started_ms;
    int length = snprintf(
        marker, sizeof(marker),
        "elapsed_ms=%llu source=%u published=%u source_drops=%u "
        "publish_errors=%u retries=%u last_sequence=%u",
        (unsigned long long)elapsed,
        state->video_source_frames, state->video_published_frames,
        state->video_source_drops, state->video_publish_errors,
        state->video_retries, state->last_frame_sequence);

    if (length < 0 || (size_t)length >= sizeof(marker)) {
        return -1;
    }
    return write_marker("video.stats", marker);
}

static void reset_video_stats(struct media_state *state, uint64_t now)
{
    state->video_source_frames = 0;
    state->video_source_drops = 0;
    state->video_published_frames = 0;
    state->video_publish_errors = 0;
    state->video_retries = 0;
    state->video_stats_started_ms = now;
    if (state->framebuffer != NULL) {
        state->last_frame_sequence = state->framebuffer->frame_sequence;
    }
    remove_marker("video.stats");
}

static int allocate_media(struct media_state *state, unsigned int width,
                          unsigned int height)
{
    char name[] = "e1-doom-osg";
    HD_RESULT status;

    state->media_size = required_buffer_size(width, height);
    if (state->media_size == 0) {
        return -1;
    }
    status = mem_alloc_call(name, &state->media_pa, &state->media_va,
                            state->media_size, DDR_ID0);
    log_line("mem-alloc", (long)status);
    if (status != HD_OK) {
        return -1;
    }
    state->allocated = 1;
    if ((state->media_pa & UINT32_C(0x7f)) != 0U) {
        return -1;
    }
    return 0;
}

static int audio_has_resources(const struct audio_state *state)
{
    return state->pcm_allocated || state->path_open || state->path_started ||
           state->output_mapped || state->stock_hook_active ||
           state->ring_fd >= 0 || state->ring != NULL;
}

static int stock_audioenc_is_idle(void)
{
    uintptr_t manager =
        *(volatile const uintptr_t *)(uintptr_t)DEVICE_MANAGER_POINTER_ADDRESS;

    if (manager < UINT32_C(0x10000) || (manager & 3U) != 0U) {
        log_line("audio-manager-invalid", (long)manager);
        return 0;
    }
    if (*(volatile const int32_t *)(manager + MANAGER_AENC_PATH_OFFSET) != -1 ||
        *(volatile const int32_t *)(manager + MANAGER_AUDIO_HARDWARE_OFFSET) !=
            0) {
        log_line("stock-audioenc-not-idle", 0);
        return 0;
    }
    return 1;
}

static int start_stock_audio_tone(struct audio_state *state)
{
    if (audio_has_resources(state)) {
        return -1;
    }
    remove_marker("audio.encoded");
    remove_marker("audio.source.restored");
    remove_marker("audio.hook.quiesced");
    atomic_store_explicit(&audio_pcm_frames, 0U, memory_order_relaxed);
    atomic_store_explicit(&audio_pcm_readback_positive, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&audio_pcm_readback_negative, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&audio_pcm_mode, AUDIO_PCM_TONE,
                          memory_order_release);
    state->stock_hook_active = 1;
    state->stock_hook_used = 1;
    state->stock_hook_mode = AUDIO_PCM_TONE;
    log_line("stock-aac-pcm-tone-start", 0);
    if (write_marker("audio.active", "stock-aac-pcm-tone") != 0) {
        atomic_store_explicit(&audio_pcm_mode, AUDIO_PCM_PASS_THROUGH,
                              memory_order_release);
        state->stock_hook_active = 0;
        state->stock_hook_mode = AUDIO_PCM_PASS_THROUGH;
        return -1;
    }
    return 0;
}

static int start_stock_audio_doom(struct audio_state *state)
{
    struct stat metadata;
    struct e1_audio_ring *ring;

    if (audio_has_resources(state)) {
        return -1;
    }
    state->ring_fd = open(AUDIO_RING_PATH,
                          O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (state->ring_fd < 0 || fstat(state->ring_fd, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) ||
        metadata.st_size != (off_t)sizeof(struct e1_audio_ring)) {
        log_line("doom-audio-ring-open", errno == 0 ? -1 : -errno);
        if (state->ring_fd >= 0) {
            (void)close(state->ring_fd);
            state->ring_fd = -1;
        }
        return -1;
    }
    ring = mmap(NULL, sizeof(*ring), PROT_READ | PROT_WRITE, MAP_SHARED,
                state->ring_fd, 0);
    if (ring == MAP_FAILED) {
        log_line("doom-audio-ring-mmap", -errno);
        (void)close(state->ring_fd);
        state->ring_fd = -1;
        return -1;
    }
    if (!e1_audio_ring_valid(ring)) {
        log_line("doom-audio-ring-invalid", -1);
        (void)munmap((void *)ring, sizeof(*ring));
        (void)close(state->ring_fd);
        state->ring_fd = -1;
        return -1;
    }
    remove_marker("audio.encoded");
    remove_marker("audio.source.restored");
    remove_marker("audio.hook.quiesced");
    atomic_store_explicit(&audio_pcm_frames, 0U, memory_order_relaxed);
    atomic_store_explicit(&audio_pcm_last_frame, 0U, memory_order_relaxed);
    atomic_store_explicit(&audio_pcm_dropped_frames, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&audio_pcm_underflows, 0U, memory_order_relaxed);
    state->ring = ring;
    atomic_store_explicit(&audio_pcm_ring, ring, memory_order_release);
    atomic_store_explicit(&audio_pcm_mode, AUDIO_PCM_DOOM,
                          memory_order_release);
    state->stock_hook_active = 1;
    state->stock_hook_used = 1;
    state->stock_hook_mode = AUDIO_PCM_DOOM;
    log_line("stock-aac-pcm-doom-start", 0);
    if (write_marker("audio.active", "stock-aac-pcm-doom") != 0) {
        atomic_store_explicit(&audio_pcm_mode, AUDIO_PCM_PASS_THROUGH,
                              memory_order_release);
        atomic_store_explicit(&audio_pcm_ring, NULL, memory_order_release);
        state->stock_hook_active = 0;
        state->stock_hook_mode = AUDIO_PCM_PASS_THROUGH;
        (void)munmap((void *)state->ring, sizeof(*state->ring));
        state->ring = NULL;
        (void)close(state->ring_fd);
        state->ring_fd = -1;
        return -1;
    }
    return 0;
}

static void update_stock_audio(struct audio_state *state)
{
    unsigned int frames;
    char marker[224];

    if (!state->stock_hook_active) {
        return;
    }
    frames = atomic_load_explicit(&audio_pcm_frames, memory_order_acquire);
    if (state->stock_hook_mode == AUDIO_PCM_TONE &&
        frames > STOCK_AUDIO_TONE_FRAMES) {
        frames = STOCK_AUDIO_TONE_FRAMES;
    }
    if (frames != 0U) {
        int length;

        if (state->stock_hook_mode == AUDIO_PCM_TONE) {
            length = snprintf(
                marker, sizeof(marker),
                "mode=tone frames=%u samples_per_frame=%u sample_rate=%u "
                "readback_positive=%d readback_negative=%d",
                frames, E1_AUDIO_SAMPLES_PER_FRAME, E1_AUDIO_SAMPLE_RATE,
                atomic_load_explicit(&audio_pcm_readback_positive,
                                     memory_order_acquire),
                atomic_load_explicit(&audio_pcm_readback_negative,
                                     memory_order_acquire));
        } else {
            length = snprintf(
                marker, sizeof(marker),
                "mode=doom frames=%u last_frame=%u dropped=%u "
                "underflows=%u producer_drops=%u produced=%u nonzero=%u "
                "peak=%u sample_rate=%u",
                frames,
                atomic_load_explicit(&audio_pcm_last_frame,
                                     memory_order_acquire),
                atomic_load_explicit(&audio_pcm_dropped_frames,
                                     memory_order_acquire),
                atomic_load_explicit(&audio_pcm_underflows,
                                     memory_order_acquire),
                state->ring == NULL ? 0U : state->ring->producer_drops,
                state->ring == NULL ? 0U : state->ring->mixed_frames,
                state->ring == NULL ? 0U : state->ring->nonzero_frames,
                state->ring == NULL ? 0U : state->ring->peak_abs,
                E1_AUDIO_SAMPLE_RATE);
        }
        if (length >= 0 && length < (int)sizeof(marker)) {
            (void)write_marker("audio.encoded", marker);
        }
    }
    if (state->stock_hook_mode == AUDIO_PCM_TONE &&
        atomic_load_explicit(&audio_pcm_mode, memory_order_acquire) ==
            AUDIO_PCM_PASS_THROUGH) {
        state->stock_hook_active = 0;
        state->stock_hook_mode = AUDIO_PCM_PASS_THROUGH;
        remove_marker("audio.active");
        (void)write_marker("audio.source.restored", "microphone-pass-through");
        log_line("stock-aac-pcm-tone-bounded-stop", (long)frames);
    }
}

static int cleanup_stock_audio_hook(struct audio_state *state)
{
    struct timespec delay = {0, 2000000L};
    unsigned int attempt;

    atomic_store_explicit(&audio_pcm_mode, AUDIO_PCM_PASS_THROUGH,
                          memory_order_release);
    state->stock_hook_active = 0;
    for (attempt = 0; attempt < 100U; ++attempt) {
        if (atomic_load_explicit(&audio_pcm_active_calls,
                                 memory_order_acquire) == 0U) {
            atomic_store_explicit(&audio_pcm_ring, NULL,
                                  memory_order_release);
            if (state->ring != NULL) {
                (void)munmap((void *)state->ring, sizeof(*state->ring));
                state->ring = NULL;
            }
            if (state->ring_fd >= 0) {
                (void)close(state->ring_fd);
                state->ring_fd = -1;
            }
            state->stock_hook_mode = AUDIO_PCM_PASS_THROUGH;
            remove_marker("audio.active");
            (void)write_marker("audio.source.restored",
                               "microphone-pass-through");
            log_line("stock-aac-pcm-source-restored", 0);
            return 0;
        }
        (void)nanosleep(&delay, NULL);
    }
    log_line("stock-aac-pcm-quiesce-timeout", -1);
    return -1;
}

static uint32_t fnv1a_update(uint32_t hash, const uint8_t *bytes, size_t size)
{
    size_t index;

    for (index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static int encode_tone_frames(struct audio_state *state)
{
    struct timespec cadence = {0, 64000000L};
    unsigned int index;

    state->rolling_hash = UINT32_C(2166136261);
    for (index = 0; index < AUDIO_TONE_FRAMES; ++index) {
        HD_AUDIO_FRAME frame;
        HD_AUDIO_BS bitstream;
        struct e1_adts_contract contract;
        uint8_t *payload;
        uint64_t output_end;
        HD_RESULT status;
        int validation = 0;

        memset(&frame, 0, sizeof(frame));
        frame.sign = MAKEFOURCC('A', 'F', 'R', 'M');
        frame.ddr_id = DDR_ID0;
        frame.size = state->pcm_size;
        frame.phy_addr[0] = state->pcm_pa;
        frame.bit_width = HD_AUDIO_BIT_WIDTH_16;
        frame.sound_mode = HD_AUDIO_SOUND_MODE_MONO;
        frame.sample_rate = HD_AUDIO_SR_16000;
        frame.count = index;
        frame.timestamp = monotonic_us();

        status = audioenc_push_call(state->path, &frame, NULL, 0);
        log_line("audioenc-push", (long)status);
        if (status != HD_OK) {
            return -1;
        }
        memset(&bitstream, 0, sizeof(bitstream));
        status = audioenc_pull_call(state->path, &bitstream, 1000);
        log_line("audioenc-pull", (long)status);
        if (status != HD_OK) {
            return -1;
        }
        output_end = (uint64_t)state->output_pa + state->output_size;
        if (bitstream.sign != MAKEFOURCC('A', 'S', 'T', 'M') ||
            bitstream.acodec_format != HD_AUDIO_CODEC_AAC ||
            bitstream.phy_addr < state->output_pa || bitstream.size == 0U ||
            (uint64_t)bitstream.phy_addr + bitstream.size > output_end) {
            validation = -1;
        }
        payload = NULL;
        if (validation == 0) {
            payload = (uint8_t *)state->output_va +
                      (bitstream.phy_addr - state->output_pa);
            if (e1_parse_stock_adts(payload, bitstream.size, &contract) != 0 ||
                contract.frame_size != bitstream.size) {
                validation = -1;
            }
        }
        if (validation == 0) {
            if (state->frames == 0U) {
                state->first_timestamp = bitstream.timestamp;
                state->first_size = bitstream.size;
            } else if (bitstream.timestamp < state->last_timestamp) {
                validation = -1;
            }
            state->last_timestamp = bitstream.timestamp;
            state->last_size = bitstream.size;
            state->rolling_hash = fnv1a_update(
                state->rolling_hash, payload, bitstream.size);
        }
        status = audioenc_release_call(state->path, &bitstream);
        log_line("audioenc-release", (long)status);
        if (status != HD_OK || validation != 0) {
            return -1;
        }
        ++state->frames;
        if (index + 1U < AUDIO_TONE_FRAMES) {
            (void)nanosleep(&cadence, NULL);
        }
    }
    return 0;
}

static int start_audio_tone(struct audio_state *state)
{
    char name[] = "e1-doom-pcm";
    HD_AUDIOENC_PATH_CONFIG path_config;
    HD_AUDIOENC_IN input;
    HD_AUDIOENC_OUT output;
    HD_AUDIOENC_BUFINFO buffer_info;
    HD_RESULT status;
    char marker[192];

    if (!native_audio_manager_abi_valid) {
        errno = ENOTSUP;
        log_line("native-audio-manager-abi-unresolved", 0);
        return -1;
    }
    if (audio_has_resources(state) || !stock_audioenc_is_idle()) {
        return -1;
    }
    state->pcm_size = E1_AUDIO_PCM_BYTES_PER_FRAME;
    status = mem_alloc_call(name, &state->pcm_pa, &state->pcm_va,
                            state->pcm_size, DDR_ID0);
    log_line("audio-pcm-alloc", (long)status);
    if (status != HD_OK || state->pcm_va == NULL || state->pcm_pa == 0U) {
        return -1;
    }
    state->pcm_allocated = 1;
    e1_fill_tone_1khz_s16(state->pcm_va, E1_AUDIO_SAMPLES_PER_FRAME);

    status = audioenc_open_call(HD_AUDIOENC_0_IN_0, HD_AUDIOENC_0_OUT_0,
                                &state->path);
    log_line("audioenc-open", (long)status);
    if (status != HD_OK) {
        return -1;
    }
    state->path_open = 1;

    memset(&path_config, 0, sizeof(path_config));
    path_config.max_mem.codec_type = HD_AUDIO_CODEC_AAC;
    path_config.max_mem.sample_rate = HD_AUDIO_SR_16000;
    path_config.max_mem.sample_bit = HD_AUDIO_BIT_WIDTH_16;
    path_config.max_mem.mode = HD_AUDIO_SOUND_MODE_MONO;
    status = audioenc_set_call(state->path, HD_AUDIOENC_PARAM_PATH_CONFIG,
                               &path_config);
    log_line("audioenc-set-path", (long)status);
    if (status != HD_OK) {
        return -1;
    }

    memset(&input, 0, sizeof(input));
    input.sample_rate = HD_AUDIO_SR_16000;
    input.sample_bit = HD_AUDIO_BIT_WIDTH_16;
    input.mode = HD_AUDIO_SOUND_MODE_MONO;
    status = audioenc_set_call(state->path, HD_AUDIOENC_PARAM_IN, &input);
    log_line("audioenc-set-in", (long)status);
    if (status != HD_OK) {
        return -1;
    }

    memset(&output, 0, sizeof(output));
    output.codec_type = HD_AUDIO_CODEC_AAC;
    output.aac_adts = 1;
    status = audioenc_set_call(state->path, HD_AUDIOENC_PARAM_OUT, &output);
    log_line("audioenc-set-out", (long)status);
    if (status != HD_OK) {
        return -1;
    }

    status = audioenc_start_call(state->path);
    log_line("audioenc-start", (long)status);
    if (status != HD_OK) {
        return -1;
    }
    state->path_started = 1;

    memset(&buffer_info, 0, sizeof(buffer_info));
    status = audioenc_get_call(state->path, HD_AUDIOENC_PARAM_BUFINFO,
                               &buffer_info);
    log_line("audioenc-get-buffer", (long)status);
    if (status != HD_OK || buffer_info.buf_info.phy_addr == 0U ||
        buffer_info.buf_info.buf_size == 0U) {
        return -1;
    }
    state->output_pa = buffer_info.buf_info.phy_addr;
    state->output_size = buffer_info.buf_info.buf_size;
    state->output_va = mem_mmap_call(HD_COMMON_MEM_MEM_TYPE_CACHE,
                                     state->output_pa, state->output_size);
    if (state->output_va == NULL) {
        log_line("audio-output-mmap", -1);
        return -1;
    }
    state->output_mapped = 1;
    log_line("audio-output-mmap", 0);
    if (write_marker("audio.active", "native-aac-tone") != 0 ||
        encode_tone_frames(state) != 0) {
        return -1;
    }
    if (snprintf(marker, sizeof(marker),
                 "frames=%u first_size=%u last_size=%u first_ts=%llu "
                 "last_ts=%llu fnv1a=%08x",
                 state->frames, state->first_size, state->last_size,
                 (unsigned long long)state->first_timestamp,
                 (unsigned long long)state->last_timestamp,
                 state->rolling_hash) >= (int)sizeof(marker) ||
        write_marker("audio.encoded", marker) != 0) {
        return -1;
    }
    return 0;
}

static HD_RESULT retry_audio_path_call(HD_RESULT (*function)(HD_PATH_ID),
                                       HD_PATH_ID path)
{
    struct timespec delay = {0, 20000000L};
    HD_RESULT status = HD_ERR_NG;
    unsigned int attempt;

    for (attempt = 0; attempt < 3U; ++attempt) {
        status = function(path);
        if (status == HD_OK) {
            break;
        }
        (void)nanosleep(&delay, NULL);
    }
    return status;
}

static int cleanup_audio(struct audio_state *state)
{
    int result = 0;

    if (state->stock_hook_active || state->stock_hook_used) {
        if (cleanup_stock_audio_hook(state) != 0) {
            result = -1;
        }
    }

    if (state->path_started) {
        HD_RESULT status = retry_audio_path_call(audioenc_stop_call,
                                                 state->path);
        log_line("audioenc-stop", (long)status);
        if (status == HD_OK) {
            state->path_started = 0;
        } else {
            result = -1;
        }
    }
    if (!state->path_started && state->output_mapped) {
        HD_RESULT status = mem_munmap_call(state->output_va,
                                           state->output_size);
        log_line("audio-output-munmap", (long)status);
        if (status == HD_OK) {
            state->output_mapped = 0;
            state->output_va = NULL;
            state->output_pa = 0;
            state->output_size = 0;
        } else {
            result = -1;
        }
    }
    if (!state->path_started && !state->output_mapped && state->path_open) {
        HD_RESULT status = retry_audio_path_call(audioenc_close_call,
                                                 state->path);
        log_line("audioenc-close", (long)status);
        if (status == HD_OK) {
            state->path_open = 0;
            state->path = 0;
        } else {
            result = -1;
        }
    }
    if (!state->path_open && !state->path_started && state->pcm_allocated) {
        HD_RESULT status = mem_free_call(state->pcm_pa, state->pcm_va);
        log_line("audio-pcm-free", (long)status);
        if (status == HD_OK) {
            state->pcm_allocated = 0;
            state->pcm_pa = 0;
            state->pcm_va = NULL;
            state->pcm_size = 0;
        } else {
            result = -1;
        }
    }
    if (result == 0 && !audio_has_resources(state)) {
        remove_marker("audio.active");
        if (!state->stock_hook_used ||
            access(STATE_DIR "/audio.patch.used", F_OK) != 0) {
            (void)write_marker("audio.restored", "complete");
        }
    } else {
        (void)write_marker("injected.error", "audio-cleanup-failed");
        result = -1;
    }
    return result;
}

static int start_media(struct media_state *state, unsigned int encoder,
                       unsigned int width, unsigned int height,
                       int transparent, int allocation_only)
{
    HD_OSG_STAMP_BUF buffer;
    HD_OSG_STAMP_ATTR attribute;
    HD_RESULT status;

    if (state->allocated || state->path_open || state->path_started ||
        encoder > 3 || width < 8 || width > 640 || height < 8 || height > 512) {
        return -1;
    }
    state->encoder = encoder;
    state->width = width;
    state->height = height;
    state->checkerboard = !transparent;

    if (allocate_media(state, width, height) != 0) {
        return -1;
    }
    if (allocation_only) {
        if (write_marker("media.active", "allocation-only") != 0) {
            return -1;
        }
        return 0;
    }

    state->pixels = malloc((size_t)width * height * sizeof(*state->pixels));
    if (state->pixels == NULL) {
        return -1;
    }
    fill_pixels(state, transparent);
    if (state->doom_frames) {
        state->doom_pixels =
            malloc((size_t)width * height * sizeof(*state->doom_pixels));
        if (state->doom_pixels == NULL) {
            log_line("doom-pixels-allocation-failed", (long)(width * height));
            return -1;
        }
        if (wait_for_stable_doom_frame(state, state->doom_pixels) != 0) {
            return -1;
        }
        if (!state->entry_melt) {
            memcpy(state->pixels, state->doom_pixels,
                   (size_t)width * height * sizeof(*state->pixels));
        }
    }

    status = videoenc_open_call(encoder_input(encoder),
                                (HD_OUT_ID)HD_STAMP_2, &state->path);
    log_line("videoenc-open", (long)status);
    if (status != HD_OK) {
        return -1;
    }
    state->path_open = 1;

    memset(&buffer, 0, sizeof(buffer));
    buffer.type = HD_OSG_BUF_TYPE_PING_PONG;
    buffer.size = state->media_size;
    buffer.p_addr = state->media_pa;
    buffer.ddr_id = DDR_ID0;
    status = videoenc_set_call(state->path,
                               HD_VIDEOENC_PARAM_IN_STAMP_BUF, &buffer);
    log_line("set-buffer", (long)status);
    if (status != HD_OK || publish_image(state) != 0) {
        return -1;
    }

    memset(&attribute, 0, sizeof(attribute));
    attribute.align_type = HD_OSG_ALIGN_TYPE_TOP_LEFT;
    attribute.alpha = 255;
    attribute.position.x = 0;
    attribute.position.y = 0;
    /* Match the vendor videoenc OSG examples. This firmware accepted layer 2 /
     * region 2 but did not compose it into either 640x360 bitstream. */
    attribute.layer = 0;
    attribute.region = 0;
    status = videoenc_set_call(state->path,
                               HD_VIDEOENC_PARAM_IN_STAMP_ATTR, &attribute);
    log_line("set-attribute", (long)status);
    if (status != HD_OK) {
        return -1;
    }

    status = videoenc_start_call(state->path);
    log_line("videoenc-start", (long)status);
    if (status != HD_OK) {
        return -1;
    }
    state->path_started = 1;
    /* Prime the alternate ping-pong buffer after start. One pre-start image
     * leaves the second buffer's format unset on this exact firmware. */
    if (publish_image(state) != 0) {
        return -1;
    }
    if (write_marker("media.active",
                     state->doom_frames ? "doom-osg" :
                     (transparent ? "transparent-osg" : "checkerboard-osg")) !=
        0) {
        return -1;
    }
    return 0;
}

static int cleanup_media(struct media_state *state)
{
    int result = 0;

    if (state->path_started) {
        HD_RESULT status = videoenc_stop_call(state->path);
        log_line("videoenc-stop", (long)status);
        if (status != HD_OK) {
            result = -1;
        } else {
            state->path_started = 0;
        }
    }
    if (state->path_open) {
        HD_RESULT status = videoenc_close_call(state->path);
        log_line("videoenc-close", (long)status);
        if (status != HD_OK) {
            result = -1;
        } else {
            state->path_open = 0;
            state->path_started = 0;
        }
    }
    if (state->allocated && !state->path_open && !state->path_started) {
        HD_RESULT status = mem_free_call(state->media_pa, state->media_va);
        log_line("mem-free", (long)status);
        if (status != HD_OK) {
            result = -1;
        } else {
            state->allocated = 0;
            state->media_pa = 0;
            state->media_va = NULL;
            state->media_size = 0;
        }
    } else if (state->allocated) {
        log_line("mem-free-deferred-path-owned", 0);
        result = -1;
    }
    if (result == 0) {
        free(state->pixels);
        state->pixels = NULL;
        free(state->doom_pixels);
        state->doom_pixels = NULL;
        state->checkerboard = 0;
        state->entry_melt = 0;
        state->melt_last_ms = 0;
        state->melt_tick_remainder = 0;
        close_frame_source(state);
        remove_marker("media.active");
        (void)write_marker("media.restored", "complete");
    } else {
        close_frame_source(state);
        (void)write_marker("injected.error", "cleanup-failed");
    }
    return result;
}

static void set_ptz_hook_mode(enum ptz_hook_mode mode, const char *name)
{
    atomic_store_explicit(&ptz_mode, mode, memory_order_release);
    (void)write_marker("ptz.hook.mode", name);
}

static unsigned int melt_ticks(struct media_state *state, uint64_t now)
{
    return e1_melt_clock_ticks(&state->melt_last_ms,
                               &state->melt_tick_remainder, now);
}

static int begin_entry_melt(struct media_state *state,
                            struct e1_runtime_control *control)
{
    uint64_t now;

    reset_video_stats(state, monotonic_ms());
    set_ptz_hook_mode(PTZ_HOOK_DOOM, "doom");
    (void)write_marker("runtime.mode", "entry-melt");
    state->entry_melt = 1;
    if (open_frame_source(state) != 0 ||
        start_media(state, 2, E1_OSG_WIDTH, E1_OSG_HEIGHT, 1, 0) != 0) {
        (void)cleanup_media(state);
        control->mode = E1_RUNTIME_CAMERA_ARMED;
        set_ptz_hook_mode(PTZ_HOOK_CAMERA, "camera");
        (void)write_marker("runtime.mode", "camera-armed");
        return -1;
    }
    now = monotonic_ms();
    e1_melt_init(&state->melt, (uint32_t)now ^ UINT32_C(0x4531444d));
    state->melt_last_ms = now;
    state->melt_tick_remainder = 0;
    log_line("entry-melt-start", 0);
    return 0;
}

static int update_entry_melt(struct media_state *state,
                             struct e1_runtime_control *control,
                             uint64_t now)
{
    int frame_status = convert_doom_frame(state, state->doom_pixels);
    unsigned int ticks = melt_ticks(state, now);
    int done;

    if (frame_status < 0) {
        return -1;
    }
    if (ticks == 0U) {
        return 0;
    }
    done = e1_melt_advance(&state->melt, ticks);
    e1_melt_compose_entry(state->pixels, state->doom_pixels,
                          state->width, state->height, &state->melt);
    if (publish_image(state) != 0) {
        return -1;
    }
    if (done) {
        state->entry_melt = 0;
        if (e1_runtime_entry_complete(control) != 0 ||
            write_marker("runtime.mode", "doom") != 0) {
            return -1;
        }
        /* Entry animation timing is deliberately independent from gameplay.
         * Begin the sustained-run counters at the first ordinary Doom frame. */
        reset_video_stats(state, now);
        log_line("entry-melt-complete", 0);
    }
    return 0;
}

static int begin_exit_melt(struct media_state *state)
{
    uint64_t now;

    if (!state->path_started || !state->doom_frames ||
        state->pixels == NULL || state->doom_pixels == NULL) {
        return -1;
    }
    memcpy(state->doom_pixels, state->pixels,
           (size_t)state->width * state->height * sizeof(*state->pixels));
    now = monotonic_ms();
    e1_melt_init(&state->melt, (uint32_t)now ^ UINT32_C(0x5849544d));
    state->melt_last_ms = now;
    state->melt_tick_remainder = 0;
    (void)write_marker("runtime.mode", "exit-melt");
    log_line("exit-melt-start", 0);
    return 0;
}

static int update_exit_melt(struct media_state *state,
                            struct e1_runtime_control *control,
                            uint64_t now)
{
    unsigned int ticks = melt_ticks(state, now);
    int done;

    if (ticks == 0U) {
        return 0;
    }
    done = e1_melt_advance(&state->melt, ticks);
    e1_melt_compose_exit(state->pixels, state->doom_pixels,
                         state->width, state->height, &state->melt);
    if (publish_image(state) != 0) {
        return -1;
    }
    if (done) {
        if (cleanup_media(state) != 0 ||
            e1_runtime_exit_complete(control) != 0) {
            return -1;
        }
        set_ptz_hook_mode(PTZ_HOOK_CAMERA, "camera");
        if (write_marker("runtime.mode",
                         access(GAME_READY_PATH, F_OK) == 0
                             ? "stopping" : "camera-armed") != 0) {
            return -1;
        }
        log_line("exit-melt-complete", 0);
    }
    return 0;
}

static int read_command(char *buffer, size_t capacity)
{
    int descriptor = open(COMMAND_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ssize_t length;

    if (descriptor < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    length = read(descriptor, buffer, capacity - 1U);
    (void)close(descriptor);
    (void)unlink(COMMAND_PATH);
    if (length <= 0 || (size_t)length >= capacity) {
        return -1;
    }
    buffer[length] = '\0';
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 1;
}

static void *worker(void *ignored)
{
    struct media_state state;
    struct audio_state audio;
    struct e1_runtime_control control;
    struct stat heartbeat_metadata;
    struct timespec delay = {0, 100000000L};
    time_t heartbeat_mtime = 0;
    uint64_t heartbeat_seen = monotonic_ms();
    uint64_t last_animation = monotonic_ms();
    uint64_t last_video_stats = 0;
    uint64_t frame_interval_ms;
    unsigned int frame_rate;
    unsigned int ptz_read_ticket = 0;
    int ptz_sender = -1;
    int quit = 0;

    (void)ignored;
    (void)pthread_setname_np(pthread_self(), "e1-doom-osg");
    frame_rate = read_stream_frame_rate();
    frame_interval_ms = (UINT64_C(1000) + frame_rate - 1U) / frame_rate;
    delay.tv_nsec = (long)(frame_interval_ms * UINT64_C(500000));
    memset(&state, 0, sizeof(state));
    memset(&audio, 0, sizeof(audio));
    memset(&control, 0, sizeof(control));
    control.mode = E1_RUNTIME_CAMERA_ARMED;
    state.frame_fd = -1;
    audio.ring_fd = -1;
    remove_marker("injected.error");
    remove_marker("media.active");
    remove_marker("media.restored");
    remove_marker("video.stats");
    remove_marker("video.stats.reset");
    remove_marker("audio.active");
    remove_marker("audio.encoded");
    remove_marker("audio.restored");
    remove_marker("audio.source.restored");
    remove_marker("audio.hook.address");
    remove_marker("audio.hook.quiesced");
    remove_marker("audio.hook.ready");
    remove_marker("thread.exited");
    remove_marker("ptz.hook.address");
    remove_marker("ptz.hook.quiesced");
    remove_marker("ptz.hook.ready");
    remove_marker("runtime.mode");
    remove_marker("dashboard.start");
    remove_marker("dashboard.camera");
    remove_marker("runtime.start");

    if (validate_exact_code() != 0) {
        (void)write_marker("injected.error", "exact-code-mismatch");
        (void)write_marker("thread.exited", "failed");
        return NULL;
    }
    log_line(BUILD_ID, 0);
    {
        char hook_address[32];
        uintptr_t address = (uintptr_t)&e1_ptz_hook_entry;

        if ((address & 1U) == 0U ||
            snprintf(hook_address, sizeof(hook_address), "%#lx",
                     (unsigned long)address) < 0 ||
            write_marker("ptz.hook.address", hook_address) != 0 ||
            write_marker("ptz.hook.ready", "camera-pass-through") != 0 ||
            write_marker("ptz.hook.mode", "camera") != 0 ||
            write_marker("runtime.mode", "camera-armed") != 0) {
            (void)write_marker("injected.error", "ptz-hook-init-failed");
            (void)write_marker("thread.exited", "failed");
            return NULL;
        }
    }
    {
        char hook_address[32];
        uintptr_t address = (uintptr_t)&e1_audio_hook_entry;

        if ((address & 1U) == 0U ||
            snprintf(hook_address, sizeof(hook_address), "%#lx",
                     (unsigned long)address) < 0 ||
            write_marker("audio.hook.address", hook_address) != 0 ||
            write_marker("audio.hook.ready", "microphone-pass-through") != 0) {
            (void)write_marker("injected.error", "audio-hook-init-failed");
            (void)write_marker("thread.exited", "failed");
            return NULL;
        }
    }
    atomic_store_explicit(&ptz_mode, PTZ_HOOK_CAMERA, memory_order_release);
    atomic_store_explicit(&audio_pcm_ring, NULL, memory_order_release);
    atomic_store_explicit(&audio_pcm_mode, AUDIO_PCM_PASS_THROUGH,
                          memory_order_release);
    ptz_sender = open_ptz_sender();
    if (write_marker("injected.ready", BUILD_ID) != 0) {
        (void)write_marker("thread.exited", "failed");
        return NULL;
    }

    while (!quit) {
        char command[128];
        int command_status;
        uint64_t now = monotonic_ms();

        if (stat(HEARTBEAT_PATH, &heartbeat_metadata) == 0 &&
            heartbeat_metadata.st_mtime != heartbeat_mtime) {
            heartbeat_mtime = heartbeat_metadata.st_mtime;
            heartbeat_seen = now;
        }
        if ((state.allocated || audio_has_resources(&audio)) &&
            now - heartbeat_seen >= HEARTBEAT_TIMEOUT_MS) {
            log_line("heartbeat-deadman", 0);
            (void)write_marker("deadman.fired", "heartbeat-stale");
            (void)cleanup_audio(&audio);
            (void)cleanup_media(&state);
        }

        command_status = read_command(command, sizeof(command));
        if (command_status < 0) {
            (void)write_marker("injected.error", "command-read-failed");
        } else if (command_status > 0) {
            unsigned int encoder;
            unsigned int width;
            unsigned int height;

            remove_marker("media.restored");
            /* The external exact-byte supervisor owns audio.restored once a
             * stock PCM hook has been used.  A final quit must not erase that
             * proof between hook restoration and dlclose. */
            if (strcmp(command, "quit") != 0) {
                remove_marker("audio.restored");
            }
            remove_marker("deadman.fired");
            if (strcmp(command, "stop") == 0) {
                (void)cleanup_audio(&audio);
                (void)cleanup_media(&state);
            } else if (strcmp(command, "quit") == 0) {
                int audio_status = cleanup_audio(&audio);
                int media_status = cleanup_media(&state);

                if (audio_status == 0 && media_status == 0) {
                    quit = 1;
                }
            } else if (strcmp(command, "ptz-camera") == 0) {
                memset(&control, 0, sizeof(control));
                control.mode = E1_RUNTIME_CAMERA_ARMED;
                set_ptz_hook_mode(PTZ_HOOK_CAMERA, "camera");
                (void)write_marker("runtime.mode", "camera-armed");
            } else if (strcmp(command, "ptz-doom") == 0) {
                memset(&control, 0, sizeof(control));
                control.mode = E1_RUNTIME_DOOM;
                set_ptz_hook_mode(PTZ_HOOK_DOOM, "doom");
                (void)write_marker("runtime.mode", "doom");
            } else if (strcmp(command, "ptz-off") == 0) {
                atomic_store_explicit(&ptz_mode, PTZ_HOOK_OFF,
                                      memory_order_release);
                (void)write_marker("ptz.hook.mode", "off");
            } else if (sscanf(command, "alloc %u %u", &width, &height) == 2) {
                heartbeat_seen = now;
                if (start_media(&state, 3, width, height, 1, 1) != 0) {
                    (void)write_marker("injected.error", "alloc-failed");
                    (void)cleanup_media(&state);
                }
            } else if (sscanf(command, "transparent %u %u %u", &encoder,
                              &width, &height) == 3) {
                heartbeat_seen = now;
                if (start_media(&state, encoder, width, height, 1, 0) != 0) {
                    (void)write_marker("injected.error", "osg-start-failed");
                    (void)cleanup_media(&state);
                }
            } else if (sscanf(command, "checkerboard %u %u %u", &encoder,
                              &width, &height) == 3) {
                heartbeat_seen = now;
                if (start_media(&state, encoder, width, height, 0, 0) != 0) {
                    (void)write_marker("injected.error", "osg-start-failed");
                    (void)cleanup_media(&state);
                }
            } else if (sscanf(command, "doom %u", &encoder) == 1) {
                heartbeat_seen = now;
                state.entry_melt = 0;
                if (encoder > 3 || open_frame_source(&state) != 0 ||
                    start_media(&state, encoder, E1_OSG_WIDTH, E1_OSG_HEIGHT,
                                1, 0) != 0) {
                    (void)write_marker("injected.error", "doom-start-failed");
                    (void)cleanup_media(&state);
                }
            } else if (strcmp(command, "audio-tone") == 0) {
                heartbeat_seen = now;
                remove_marker("audio.encoded");
                if (start_audio_tone(&audio) != 0) {
                    (void)write_marker("injected.error",
                                       "audio-tone-start-failed");
                    (void)cleanup_audio(&audio);
                }
            } else if (strcmp(command, "audio-stock-tone") == 0) {
                heartbeat_seen = now;
                if (start_stock_audio_tone(&audio) != 0) {
                    (void)write_marker("injected.error",
                                       "audio-stock-tone-start-failed");
                    (void)cleanup_audio(&audio);
                }
            } else if (strcmp(command, "audio-stock-doom") == 0) {
                heartbeat_seen = now;
                if (start_stock_audio_doom(&audio) != 0) {
                    (void)write_marker("injected.error",
                                       "audio-stock-doom-start-failed");
                    (void)cleanup_audio(&audio);
                }
            } else if (strcmp(command, "audio-stop") == 0) {
                if (cleanup_audio(&audio) != 0) {
                    (void)write_marker("injected.error",
                                       "audio-stock-stop-failed");
                }
            } else {
                (void)write_marker("injected.error", "bad-command");
            }
        }

        {
            unsigned int actions = 0;
            uint64_t control_now;

            if (unlink(DASHBOARD_START_PATH) == 0) {
                unsigned int requested = e1_runtime_request_enter(&control);

                actions |= requested;
                log_line(requested != 0U ? "dashboard-start" :
                         "dashboard-start-ignored", (long)control.mode);
            }
            if (unlink(DASHBOARD_CAMERA_PATH) == 0) {
                unsigned int requested = e1_runtime_request_exit(&control);

                actions |= requested;
                log_line(requested != 0U ? "dashboard-camera" :
                         "dashboard-camera-ignored", (long)control.mode);
            }

            drain_ptz_events(&ptz_read_ticket, ptz_sender, &control,
                             &actions);
            if ((actions & E1_RUNTIME_ACTION_ENTER) != 0U &&
                access(GAME_READY_PATH, F_OK) != 0) {
                memset(&control, 0, sizeof(control));
                control.mode = E1_RUNTIME_CAMERA_ARMED;
                actions &= ~E1_RUNTIME_ACTION_ENTER;
                if (write_marker("runtime.start", "ptz") != 0) {
                    (void)write_marker("injected.error",
                                       "start-request-failed");
                }
                log_line("entry-deferred-game-not-ready", 0);
            }
            control_now = monotonic_ms();
            actions |= e1_runtime_tick(&control, control_now);
            if ((actions & E1_RUNTIME_ACTION_ENTER) != 0U &&
                begin_entry_melt(&state, &control) != 0) {
                (void)write_marker("injected.error", "entry-melt-failed");
            }
            if ((actions & E1_RUNTIME_ACTION_EXIT) != 0U &&
                begin_exit_melt(&state) != 0) {
                (void)write_marker("injected.error", "exit-melt-failed");
                (void)cleanup_media(&state);
                memset(&control, 0, sizeof(control));
                control.mode = E1_RUNTIME_CAMERA_ARMED;
                set_ptz_hook_mode(PTZ_HOOK_CAMERA, "camera");
                (void)write_marker("runtime.mode", "camera-armed");
            }
        }
        now = monotonic_ms();
        update_stock_audio(&audio);
        if (access(STATE_DIR "/ptz.patch.restored", F_OK) == 0 &&
            atomic_load_explicit(&ptz_active_calls, memory_order_acquire) == 0U) {
            (void)write_marker("ptz.hook.quiesced", "complete");
        }
        if (access(STATE_DIR "/audio.patch.restored", F_OK) == 0 &&
            atomic_load_explicit(&audio_pcm_active_calls,
                                 memory_order_acquire) == 0U) {
            (void)write_marker("audio.hook.quiesced", "complete");
        }

        if (state.path_started && state.checkerboard &&
            now - last_animation >= frame_interval_ms) {
            ++state.phase;
            fill_pixels(&state, 0);
            if (publish_image(&state) != 0) {
                (void)write_marker("injected.error", "image-update-failed");
                (void)cleanup_media(&state);
            }
            last_animation = now;
        }
        if (state.path_started && state.doom_frames &&
            (control.mode == E1_RUNTIME_DOOM ||
             now - last_animation >= frame_interval_ms)) {
            int frame_status;
            int presentation_attempted = 1;

            if (control.mode == E1_RUNTIME_ENTRY_MELT) {
                frame_status = update_entry_melt(&state, &control, now);
            } else if (control.mode == E1_RUNTIME_EXIT_MELT) {
                frame_status = update_exit_melt(&state, &control, now);
            } else {
                frame_status = convert_doom_frame(&state, state.pixels);
                if (frame_status == 0) {
                    frame_status = publish_image(&state);
                } else if (frame_status == 1) {
                    /* The producer marks its sole zero-copy slot odd only
                     * while drawing.  Do not advance the 10 Hz presentation
                     * deadline on that benign observation: the 50 ms worker
                     * poll will retry while the frame is stable instead of
                     * phase-locking into one skipped frame every cycle. */
                    presentation_attempted = 0;
                }
            }
            if (frame_status < 0) {
                (void)write_marker("injected.error", "doom-update-failed");
                (void)cleanup_media(&state);
                memset(&control, 0, sizeof(control));
                control.mode = E1_RUNTIME_CAMERA_ARMED;
                set_ptz_hook_mode(PTZ_HOOK_CAMERA, "camera");
                (void)write_marker("runtime.mode", "camera-armed");
            }
            if (presentation_attempted) {
                last_animation = now;
            }
        }
        if (control.mode == E1_RUNTIME_DOOM &&
            access(STATE_DIR "/video.stats.reset", F_OK) == 0) {
            remove_marker("video.stats.reset");
            reset_video_stats(&state, now);
            last_video_stats = 0;
        }
        if (state.path_started && state.doom_frames &&
            (last_video_stats == 0U || now - last_video_stats >= 5000U)) {
            if (write_video_stats(&state) != 0) {
                (void)write_marker("injected.error", "video-stats-failed");
            }
            last_video_stats = now;
        }
        (void)nanosleep(&delay, NULL);
    }

    (void)cleanup_audio(&audio);
    (void)cleanup_media(&state);
    atomic_store_explicit(&ptz_mode, PTZ_HOOK_OFF, memory_order_release);
    if (ptz_sender >= 0) {
        (void)close(ptz_sender);
    }
    remove_marker("ptz.hook.address");
    remove_marker("ptz.hook.mode");
    remove_marker("ptz.hook.ready");
    remove_marker("audio.hook.ready");
    remove_marker("runtime.mode");
    remove_marker("dashboard.start");
    remove_marker("dashboard.camera");
    remove_marker("runtime.start");
    remove_marker("injected.ready");
    (void)write_marker("thread.exited", "complete");
    log_line("thread-exited", 0);
    return NULL;
}

__attribute__((constructor)) static void start_worker(void)
{
    pthread_t thread;
    int status = pthread_create(&thread, NULL, worker, NULL);

    if (status == 0) {
        (void)pthread_detach(thread);
    } else {
        (void)write_marker("injected.error", "pthread-create-failed");
        log_line("pthread-create-failed", status);
    }
}
