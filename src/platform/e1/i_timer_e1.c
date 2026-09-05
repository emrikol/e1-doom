#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <time.h>

#include "doomdef.h"
#include "doomtype.h"
#include "i_system.h"
#include "i_timer.h"
#include "m_fixed.h"

static uint64_t base_us;
static uint64_t scaled_base_us;
static int fasttic;

int time_scale = 100;

static uint64_t MonotonicUS(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        I_Error("clock_gettime(CLOCK_MONOTONIC) failed");
    }
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

uint64_t I_GetTimeUS(void)
{
    const uint64_t now = MonotonicUS();
    if (!base_us)
    {
        base_us = now;
    }
    return now - base_us;
}

int I_GetTimeMS(void)
{
    return (int)(I_GetTimeUS() / 1000ull);
}

static uint64_t ScaledUS(void)
{
    const uint64_t now = MonotonicUS() * (uint64_t)time_scale / 100ull;
    if (!scaled_base_us)
    {
        scaled_base_us = now;
    }
    return now - scaled_base_us;
}

int I_GetTime_RealTime(void)
{
    return (int)(I_GetTimeUS() * TICRATE / 1000000ull);
}

static int GetTimeScaled(void)
{
    return (int)(ScaledUS() * TICRATE / 1000000ull);
}

static int GetFracTimeScaled(void)
{
    return (int)((ScaledUS() * TICRATE % 1000000ull) * FRACUNIT / 1000000ull);
}

static int GetTimeFast(void)
{
    return fasttic++;
}

static int GetFracTimeFast(void)
{
    return 0;
}

int (*I_GetTime)(void) = GetTimeScaled;
int (*I_GetFracTime)(void) = GetFracTimeScaled;

void I_InitTimer(void)
{
    base_us = MonotonicUS();
    scaled_base_us = base_us;
    I_GetTime = GetTimeScaled;
    I_GetFracTime = GetFracTimeScaled;
}

void I_SetTimeScale(int scale)
{
    const uint64_t elapsed = ScaledUS();
    time_scale = scale;
    scaled_base_us = MonotonicUS() * (uint64_t)time_scale / 100ull - elapsed;
}

void I_SetFastdemoTimer(boolean on)
{
    if (on)
    {
        fasttic = GetTimeScaled();
        I_GetTime = GetTimeFast;
        I_GetFracTime = GetFracTimeFast;
    }
    else
    {
        I_GetTime = GetTimeScaled;
        I_GetFracTime = GetFracTimeScaled;
    }
}

void I_EnableWarp(boolean warp)
{
    (void)warp;
}

void I_SleepUS(uint64_t us)
{
    struct timespec req = {(time_t)(us / 1000000ull),
                           (long)(us % 1000000ull) * 1000L};
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
    {
    }
}

void I_Sleep(int ms)
{
    if (ms > 0)
    {
        I_SleepUS((uint64_t)ms * 1000ull);
    }
}

void I_WaitVBL(int count)
{
    I_Sleep((count * 500) / TICRATE);
}
