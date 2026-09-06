#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "doomstat.h"
#include "i_printf.h"
#include "i_oalstream.h"
#include "i_sound.h"
#include "i_system.h"
#include "i_timer.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

#include "e1-audio-ring.h"
#include "e1-audio-transition.h"
#include "e1-sfx-mixer.h"
#include "opl.h"

#define DMX_HEADER_SIZE 8U
#define DMX_PAD_SIZE 16U
#define E1_AUDIO_THREAD_POLL_NS 32000000L
#define E1_AUDIO_FRAME_US \
    ((uint64_t)E1_AUDIO_SAMPLES_PER_FRAME * UINT64_C(1000000) / \
     E1_AUDIO_SAMPLE_RATE)
#define E1_OPL_MAX_SOURCE_FRAMES \
    ((OPL_SAMPLE_RATE * E1_AUDIO_SAMPLES_PER_FRAME + \
      E1_AUDIO_SAMPLE_RATE - 1U) / E1_AUDIO_SAMPLE_RATE + 1U)

boolean auto_gain;

static struct e1_sfx_voice voices[E1_SFX_MAX_VOICES];
static struct e1_audio_ring *audio_ring;
static int audio_fd = -1;
static pthread_t audio_thread;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool audio_thread_stop;
static boolean audio_thread_started;
static uint32_t frame_number;
static uint32_t mixed_frames;
/* Capture instrumentation: isolate one source so each can be recorded alone. */
static int audio_isolate; /* 0 both, 1 music only, 2 sfx only */
static int audio_isolate_voice = -1; /* >=0 selects a single sfx voice */
static uint32_t dropped_frames;
static uint32_t started_sfx;
static boolean sound_initialized;
static unsigned int test_sfx_remaining;
static uint64_t test_sfx_next_us;
static int16_t opl_stereo[E1_OPL_MAX_SOURCE_FRAMES * 2U];
static uint64_t music_source_remainder;
static int music_handle_token;
static int music_volume = 8;
static boolean music_initialized;
static boolean music_registered;
static boolean music_playing;
static boolean music_paused;
static boolean exit_audio_only;
static boolean audio_free_run;
static uint64_t audio_next_frame_us;

static int16_t clamp_s16(int32_t sample)
{
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static void mix_music_frame(int16_t output[E1_AUDIO_SAMPLES_PER_FRAME])
{
#if OPL_SAMPLE_RATE != E1_AUDIO_SAMPLE_RATE
    uint64_t requested;
#endif
    unsigned int source_frames;
    int filled;
    unsigned int sample;

    if (!music_playing || music_paused) {
        return;
    }
#if OPL_SAMPLE_RATE == E1_AUDIO_SAMPLE_RATE
    source_frames = E1_AUDIO_SAMPLES_PER_FRAME;
#else
    requested = music_source_remainder +
        (uint64_t)OPL_SAMPLE_RATE * E1_AUDIO_SAMPLES_PER_FRAME;
    source_frames = (unsigned int)(requested / E1_AUDIO_SAMPLE_RATE);
    music_source_remainder = requested % E1_AUDIO_SAMPLE_RATE;
#endif
    if (source_frames == 0U ||
        source_frames > E1_OPL_MAX_SOURCE_FRAMES) {
        return;
    }
    filled = stream_opl_module.I_FillStream((byte *)opl_stereo,
                                             (int)source_frames);
    if (filled <= 0) {
        return;
    }
    for (sample = 0; sample < E1_AUDIO_SAMPLES_PER_FRAME; ++sample) {
#if OPL_SAMPLE_RATE == E1_AUDIO_SAMPLE_RATE
        unsigned int source = sample;
#else
        unsigned int source = (unsigned int)(
            (uint64_t)sample * (unsigned int)filled /
            E1_AUDIO_SAMPLES_PER_FRAME);
#endif
        /* The E1 OPL backend duplicates mono into both output channels. */
        int32_t mono = opl_stereo[source * 2U];
        /* OPL is already mono here; selecting its left duplicate is not a
         * stereo sum and therefore does not need the former extra /2. */
        /* The camera/app listen path is substantially quieter than desktop
         * playback.  A 3x lift keeps measured OPL peaks below full scale while
         * making the score audible beside effects. */
        int32_t scaled = mono * music_volume / 5;

        output[sample] = clamp_s16((int32_t)output[sample] + scaled);
    }
}

static uint32_t read_u32_le(const byte *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void publish_frame(const int16_t *samples)
{
    struct e1_audio_slot *slot;
    uint32_t sequence;
    uint32_t target;
    uint32_t peak = 0;
    int nonzero = 0;
    size_t sample_index;

    if (audio_ring == NULL) {
        return;
    }
    ++frame_number;
    target = (frame_number - 1U) & (E1_AUDIO_RING_SLOTS - 1U);
    slot = &audio_ring->slot[target];
    sequence = (slot->sequence & ~1U) + 1U;
    slot->sequence = sequence;
    e1_audio_barrier();
    slot->frame_number = frame_number;
    memcpy(slot->samples, samples, E1_AUDIO_PCM_BYTES_PER_FRAME);
    e1_audio_barrier();
    slot->sequence = sequence + 1U;
    e1_audio_barrier();
    for (sample_index = 0; sample_index < E1_AUDIO_SAMPLES_PER_FRAME;
         ++sample_index) {
        int32_t value = samples[sample_index];
        uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);

        if (magnitude != 0U) {
            nonzero = 1;
        }
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    ++mixed_frames;
    audio_ring->mixed_frames = mixed_frames;
    if (nonzero) {
        ++audio_ring->nonzero_frames;
        audio_ring->last_nonzero_frame = frame_number;
    }
    if (peak > audio_ring->peak_abs) {
        audio_ring->peak_abs = peak;
    }
    audio_ring->published_frame = frame_number;
    e1_audio_barrier();
}

static void fill_audio_prebuffer(void)
{
    int16_t output[E1_AUDIO_SAMPLES_PER_FRAME];
    unsigned int generated = 0;
    uint64_t now_us;

    if (!sound_initialized || audio_ring == NULL) {
        return;
    }
    if (audio_free_run) {
        now_us = I_GetTimeUS();
        while ((frame_number < E1_AUDIO_PREBUFFER_FRAMES ||
                now_us >= audio_next_frame_us) &&
               generated < E1_AUDIO_RING_SLOTS) {
            if (audio_isolate == 1) {
                memset(output, 0, sizeof(output));
            } else if (audio_isolate_voice >= 0) {
                e1_sfx_mix_frame(&voices[audio_isolate_voice], 1U, output);
            } else {
                e1_sfx_mix_frame(voices, E1_SFX_MAX_VOICES, output);
            }
            if (audio_isolate != 2) {
                mix_music_frame(output);
            }
            publish_frame(output);
            audio_next_frame_us += E1_AUDIO_FRAME_US;
            ++generated;
        }
        return;
    }
    while (frame_number - audio_ring->consumed_frame <
               E1_AUDIO_PREBUFFER_FRAMES &&
           generated < E1_AUDIO_RING_SLOTS) {
        if (audio_isolate == 1) {
            memset(output, 0, sizeof(output));
        } else if (audio_isolate_voice >= 0) {
            e1_sfx_mix_frame(&voices[audio_isolate_voice], 1U, output);
        } else {
            e1_sfx_mix_frame(voices, E1_SFX_MAX_VOICES, output);
        }
        if (audio_isolate != 2) {
            mix_music_frame(output);
        }
        publish_frame(output);
        ++generated;
    }
}

static void *audio_mixer_thread(void *ignored)
{
    const struct timespec delay = {0, E1_AUDIO_THREAD_POLL_NS};

    (void)ignored;
#ifdef __APPLE__
    (void)pthread_setname_np("e1-audio");
#else
    (void)pthread_setname_np(pthread_self(), "e1-audio");
#endif
    while (!atomic_load_explicit(&audio_thread_stop, memory_order_acquire)) {
        (void)pthread_mutex_lock(&audio_mutex);
        fill_audio_prebuffer();
        (void)pthread_mutex_unlock(&audio_mutex);
        (void)nanosleep(&delay, NULL);
    }
    return NULL;
}

void I_InitSound(void)
{
    const char *path;
    const char *test_sfx;
    const char *test_delay;

    if (sound_initialized) {
        return;
    }
    /*
     * The desktop i_sound.c normally gives snd_channels its configured
     * default from I_BindSoundVariables().  The E1 backend replaces that
     * module, so leaving the zero-initialized core variable untouched makes
     * S_getChannel() reject every real game sound before I_StartSound().
     */
    if (snd_channels < 1 ||
        snd_channels > (int)E1_SFX_MAX_VOICES) {
        snd_channels = E1_SFX_MAX_VOICES;
        /* Keep effects below their unbound maximum so several simultaneous
         * voices leave headroom for the boosted camera music path. */
        snd_SfxVolume = 6;
        snd_MusicVolume = 15;
    }
    memset(voices, 0, sizeof(voices));
    path = getenv("E1_DOOM_AUDIO");
    if (path == NULL || *path == '\0') {
        I_Printf(VB_ALWAYS, "E1_AUDIO disabled (E1_DOOM_AUDIO unset)\n");
        return;
    }
    audio_fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                    0600);
    if (audio_fd < 0 ||
        ftruncate(audio_fd, (off_t)sizeof(struct e1_audio_ring)) != 0) {
        I_Error("unable to create shared audio ring %s: %s", path,
                strerror(errno));
    }
    audio_ring = mmap(NULL, sizeof(*audio_ring), PROT_READ | PROT_WRITE,
                      MAP_SHARED, audio_fd, 0);
    if (audio_ring == MAP_FAILED) {
        audio_ring = NULL;
        I_Error("unable to map shared audio ring %s: %s", path,
                strerror(errno));
    }
    memset(audio_ring, 0, sizeof(*audio_ring));
    audio_ring->magic = E1_AUDIO_RING_MAGIC;
    audio_ring->version = E1_AUDIO_RING_VERSION;
    audio_ring->structure_size = sizeof(*audio_ring);
    audio_ring->sample_rate = E1_AUDIO_SAMPLE_RATE;
    audio_ring->samples_per_frame = E1_AUDIO_SAMPLES_PER_FRAME;
    audio_ring->slot_count = E1_AUDIO_RING_SLOTS;
    e1_audio_barrier();
    sound_initialized = true;
    exit_audio_only = false;
    frame_number = 0;
    mixed_frames = 0;
    dropped_frames = 0;
    started_sfx = 0;
    audio_free_run = getenv("E1_DOOM_AUDIO_FREE_RUN") != NULL &&
                     strcmp(getenv("E1_DOOM_AUDIO_FREE_RUN"), "1") == 0;
    {
        const char *isolate = getenv("E1_DOOM_AUDIO_ISOLATE");
        audio_isolate = 0;
        if (isolate != NULL) {
            if (strcmp(isolate, "music") == 0) {
                audio_isolate = 1;
            } else if (strncmp(isolate, "sfx", 3) == 0) {
                audio_isolate = 2;
                if (isolate[3] == ':') {
                    int want = atoi(isolate + 4);
                    if (want >= 0 && want < (int)E1_SFX_MAX_VOICES) {
                        audio_isolate_voice = want;
                    }
                }
            }
        }
    }
    audio_next_frame_us = I_GetTimeUS();
    test_sfx = getenv("E1_DOOM_TEST_SFX");
    test_sfx_remaining = test_sfx == NULL ? 0U : (unsigned int)atoi(test_sfx);
    if (test_sfx != NULL && test_sfx_remaining == 0U) {
        test_sfx_remaining = 1U;
    }
    if (test_sfx_remaining > 16U) {
        test_sfx_remaining = 16U;
    }
    test_delay = getenv("E1_DOOM_TEST_SFX_DELAY_MS");
    test_sfx_next_us = I_GetTimeUS() +
        (uint64_t)(test_delay == NULL ? 0 :
                   atoi(test_delay) < 0 ? 0 : atoi(test_delay)) * 1000U;
    fill_audio_prebuffer();
    atomic_store_explicit(&audio_thread_stop, false, memory_order_release);
    if (pthread_create(&audio_thread, NULL, audio_mixer_thread, NULL) != 0) {
        I_Error("unable to start E1 audio mixer thread");
    }
    audio_thread_started = true;
    I_AtExit(I_ShutdownSound, true);
    I_Printf(VB_ALWAYS,
             "E1_AUDIO start rate=%u samples=%u slots=%u voices=%d "
             "sfx_volume=%d music_volume=%d\n",
             E1_AUDIO_SAMPLE_RATE, E1_AUDIO_SAMPLES_PER_FRAME,
             E1_AUDIO_RING_SLOTS, snd_channels, snd_SfxVolume,
             snd_MusicVolume);
}

void I_ShutdownSound(void)
{
    if (!sound_initialized && audio_ring == NULL && audio_fd < 0) {
        return;
    }
    if (audio_thread_started) {
        atomic_store_explicit(&audio_thread_stop, true, memory_order_release);
        (void)pthread_join(audio_thread, NULL);
        audio_thread_started = false;
    }
    sound_initialized = false;
    audio_free_run = false;
    (void)pthread_mutex_lock(&audio_mutex);
    memset(voices, 0, sizeof(voices));
    I_Printf(VB_ALWAYS, "E1_AUDIO done frames=%u drops=%u\n",
             mixed_frames, dropped_frames);
    if (audio_ring != NULL) {
        (void)munmap(audio_ring, sizeof(*audio_ring));
        audio_ring = NULL;
    }
    if (audio_fd >= 0) {
        (void)close(audio_fd);
        audio_fd = -1;
    }
    (void)pthread_mutex_unlock(&audio_mutex);
}

boolean I_AllowReinitSound(void) { return false; }
void I_SetSoundModule(void) {}

void I_SetChannels(void)
{
    (void)pthread_mutex_lock(&audio_mutex);
    memset(voices, 0, sizeof(voices));
    (void)pthread_mutex_unlock(&audio_mutex);
}

void I_SetSfxVolume(int volume)
{
    snd_SfxVolume = volume;
}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    if (sfx->lumpnum == -1) {
        char name[16] = "DS";
        (void)strncat(name, sfx->name, sizeof(name) - 3U);
        sfx->lumpnum = W_CheckNumForName(name);
    }
    return sfx->lumpnum;
}

int I_StartSound(sfxinfo_t *sound, const sfxparams_t *params, int pitch)
{
    const byte *lump;
    const byte *samples;
    uint32_t sample_rate;
    uint32_t sample_count;
    int lump_number;
    int lump_length;
    unsigned int channel;

    if (!sound_initialized || params == NULL) {
        return -1;
    }
    (void)pthread_mutex_lock(&audio_mutex);
    if (exit_audio_only &&
        (sound == NULL || sound->name == NULL ||
         strcmp(sound->name, "pldeth") != 0)) {
        (void)pthread_mutex_unlock(&audio_mutex);
        return -1;
    }
    for (channel = 0; channel < E1_SFX_MAX_VOICES; ++channel) {
        if (!voices[channel].active) {
            break;
        }
    }
    if (channel == E1_SFX_MAX_VOICES) {
        (void)pthread_mutex_unlock(&audio_mutex);
        return -1;
    }
    lump_number = I_GetSfxLumpNum(sound);
    if (lump_number < 0) {
        (void)pthread_mutex_unlock(&audio_mutex);
        return -1;
    }
    lump = W_CacheLumpNum(lump_number, PU_STATIC);
    lump_length = W_LumpLength(lump_number);
    if (lump == NULL || lump_length <= (int)DMX_HEADER_SIZE ||
        lump[0] != 3 || lump[1] != 0) {
        (void)pthread_mutex_unlock(&audio_mutex);
        return -1;
    }
    sample_rate = (uint32_t)lump[2] | ((uint32_t)lump[3] << 8);
    sample_count = read_u32_le(lump + 4);
    if (sample_count > (uint32_t)lump_length - DMX_HEADER_SIZE ||
        sample_count <= DMX_PAD_SIZE * 2U) {
        (void)pthread_mutex_unlock(&audio_mutex);
        return -1;
    }
    samples = lump + DMX_HEADER_SIZE + DMX_PAD_SIZE;
    sample_count -= DMX_PAD_SIZE * 2U;
    if (e1_sfx_start_voice(&voices[channel], samples, sample_count,
                           sample_rate, (unsigned int)pitch,
                           (unsigned int)params->volume,
                           (unsigned int)params->separation) != 0) {
        (void)pthread_mutex_unlock(&audio_mutex);
        return -1;
    }
    sound->cached = true;
    ++started_sfx;
    I_Printf(VB_ALWAYS,
             "E1_AUDIO_SFX count=%u channel=%u name=%s rate=%u samples=%u\n",
             started_sfx, channel, sound->name, sample_rate, sample_count);
    (void)pthread_mutex_unlock(&audio_mutex);
    return (int)channel;
}

void I_StopSound(int handle)
{
    (void)pthread_mutex_lock(&audio_mutex);
    if (handle >= 0 && handle < (int)E1_SFX_MAX_VOICES) {
        voices[handle].active = 0;
    }
    (void)pthread_mutex_unlock(&audio_mutex);
}

void I_E1SetExitAudioOnly(boolean enabled)
{
    (void)pthread_mutex_lock(&audio_mutex);
    exit_audio_only = enabled;
    (void)pthread_mutex_unlock(&audio_mutex);
    I_Printf(VB_ALWAYS, "E1_EXIT_AUDIO death-only=%d\n", enabled);
}

void I_PauseSound(int handle)
{
    (void)pthread_mutex_lock(&audio_mutex);
    if (handle >= 0 && handle < (int)E1_SFX_MAX_VOICES) {
        voices[handle].paused = 1;
    }
    (void)pthread_mutex_unlock(&audio_mutex);
}

void I_ResumeSound(int handle)
{
    (void)pthread_mutex_lock(&audio_mutex);
    if (handle >= 0 && handle < (int)E1_SFX_MAX_VOICES) {
        voices[handle].paused = 0;
    }
    (void)pthread_mutex_unlock(&audio_mutex);
}

boolean I_SoundIsPlaying(int handle)
{
    boolean playing;

    (void)pthread_mutex_lock(&audio_mutex);
    playing = handle >= 0 && handle < (int)E1_SFX_MAX_VOICES &&
              voices[handle].active;
    (void)pthread_mutex_unlock(&audio_mutex);
    return playing;
}

boolean I_SoundIsPaused(int handle)
{
    boolean paused;

    (void)pthread_mutex_lock(&audio_mutex);
    paused = handle >= 0 && handle < (int)E1_SFX_MAX_VOICES &&
             voices[handle].active && voices[handle].paused;
    (void)pthread_mutex_unlock(&audio_mutex);
    return paused;
}

boolean I_AdjustSoundParams(const struct mobj_s *listener,
                            const struct mobj_s *source,
                            struct sfxparams_s *params)
{
    (void)listener;
    (void)source;
    if (!sound_initialized || params == NULL) {
        return false;
    }
    params->volume = snd_SfxVolume * params->volume_scale / 15;
    if (params->volume > 127) {
        params->volume = 127;
    }
    params->separation = NORM_SEP;
    return params->volume > 0;
}

void I_UpdateSoundParams(int handle, const struct sfxparams_s *params)
{
    (void)pthread_mutex_lock(&audio_mutex);
    if (handle >= 0 && handle < (int)E1_SFX_MAX_VOICES && params != NULL) {
        voices[handle].volume = (uint16_t)(params->volume > 127
                                               ? 127
                                               : params->volume < 0
                                                     ? 0
                                                     : params->volume);
        voices[handle].separation =
            (uint16_t)(params->separation > 255
                           ? 255
                           : params->separation < 0 ? 0 : params->separation);
    }
    (void)pthread_mutex_unlock(&audio_mutex);
}

void I_UpdateListenerParams(const struct mobj_s *listener) { (void)listener; }
void I_DeferSoundUpdates(void) {}
void I_ProcessSoundUpdates(void)
{
    uint64_t now = I_GetTimeUS();

    if (test_sfx_remaining != 0U && now >= test_sfx_next_us) {
        sfxparams_t params;
        int handle;

        memset(&params, 0, sizeof(params));
        params.volume_scale = 127;
        params.volume = 127;
        params.separation = NORM_SEP;
        params.priority = S_sfx[sfx_pistol].priority;
        handle = I_StartSound(&S_sfx[sfx_pistol], &params, NORM_PITCH);
        --test_sfx_remaining;
        test_sfx_next_us = now + UINT64_C(750000);
        I_Printf(VB_ALWAYS,
                 "E1_AUDIO_TEST_SFX pistol handle=%d remaining=%u\n",
                 handle, test_sfx_remaining);
    }
}

midiplayertype_t I_MidiPlayerType(void) { return midiplayer_opl; }
void I_SetMidiPlayer(void) {}
boolean I_InitMusic(void)
{
    (void)pthread_mutex_lock(&audio_mutex);
    if (!music_initialized) {
        music_initialized = stream_opl_module.I_InitStream(0);
        I_Printf(VB_ALWAYS, "E1_MUSIC init=%d backend=opl2 rate=%u\n",
                 music_initialized, OPL_SAMPLE_RATE);
    }
    (void)pthread_mutex_unlock(&audio_mutex);
    return music_initialized;
}
void I_ShutdownMusic(void)
{
    (void)pthread_mutex_lock(&audio_mutex);
    if (music_registered) {
        stream_opl_module.I_CloseStream();
    }
    if (music_initialized) {
        stream_opl_module.I_ShutdownStream();
    }
    music_initialized = false;
    music_registered = false;
    music_playing = false;
    music_paused = false;
    (void)pthread_mutex_unlock(&audio_mutex);
}
void I_SetMusicVolume(int volume)
{
    (void)pthread_mutex_lock(&audio_mutex);
    music_volume = volume < 0 ? 0 : volume > 15 ? 15 : volume;
    (void)pthread_mutex_unlock(&audio_mutex);
}
void I_PauseSong(void *handle)
{
    (void)pthread_mutex_lock(&audio_mutex);
    if (handle == &music_handle_token) {
        music_paused = true;
    }
    (void)pthread_mutex_unlock(&audio_mutex);
}
void I_ResumeSong(void *handle)
{
    (void)pthread_mutex_lock(&audio_mutex);
    if (!exit_audio_only && handle == &music_handle_token) {
        music_paused = false;
    }
    (void)pthread_mutex_unlock(&audio_mutex);
}
void *I_RegisterSong(void *data, int size)
{
    ALenum format = 0;
    ALsizei frequency = 0;
    ALsizei frame_size = 0;

    if (!music_initialized || data == NULL || size <= 0) {
        return NULL;
    }
    (void)pthread_mutex_lock(&audio_mutex);
    if (music_registered) {
        stream_opl_module.I_CloseStream();
        music_registered = false;
    }
    if (!stream_opl_module.I_OpenStream(data, size, &format, &frequency,
                                         &frame_size) ||
        format != AL_FORMAT_STEREO16 || frequency != OPL_SAMPLE_RATE ||
        frame_size != 4) {
        (void)pthread_mutex_unlock(&audio_mutex);
        return NULL;
    }
    music_registered = true;
    music_playing = false;
    music_paused = false;
    music_source_remainder = 0;
    I_Printf(VB_ALWAYS, "E1_MUSIC registered format=%s\n",
             stream_opl_module.I_MusicFormat());
    (void)pthread_mutex_unlock(&audio_mutex);
    return &music_handle_token;
}
void I_PlaySong(void *handle, boolean looping)
{
    (void)pthread_mutex_lock(&audio_mutex);
    if (!exit_audio_only && music_registered && handle == &music_handle_token) {
        stream_opl_module.I_PlayStream(looping);
        music_source_remainder = 0;
        music_playing = true;
        music_paused = false;
        I_Printf(VB_ALWAYS, "E1_MUSIC playing loop=%d\n", looping);
    }
    (void)pthread_mutex_unlock(&audio_mutex);
}
void I_StopSong(void *handle)
{
    (void)pthread_mutex_lock(&audio_mutex);
    if (music_registered && handle == &music_handle_token) {
        stream_opl_module.I_CloseStream();
        music_registered = false;
        music_playing = false;
        music_paused = false;
    }
    (void)pthread_mutex_unlock(&audio_mutex);
}
void I_UnRegisterSong(void *handle)
{
    I_StopSong(handle);
}
const char **I_DeviceList(void)
{
    return stream_opl_module.I_DeviceList();
}
boolean IsMid(byte *mem, int len)
{ return len >= 4 && !memcmp(mem, "MThd", 4); }
boolean IsMus(byte *mem, int len)
{ return len >= 4 && !memcmp(mem, "MUS\x1a", 4); }
const char *I_MusicFormat(void)
{
    const char *format;

    (void)pthread_mutex_lock(&audio_mutex);
    format = music_initialized ? stream_opl_module.I_MusicFormat() : "disabled";
    (void)pthread_mutex_unlock(&audio_mutex);
    return format;
}
void I_BindSoundVariables(void) {}
