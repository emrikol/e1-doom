#ifndef E1_SDL_COMPAT_H
#define E1_SDL_COMPAT_H

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int SDL_JoystickID;
typedef struct SDL_GameController SDL_GameController;
typedef union SDL_Event { unsigned char padding[64]; } SDL_Event;

enum
{
    SDL_SCANCODE_1 = 30,
    SDL_SCANCODE_2 = 31,
    SDL_SCANCODE_3 = 32,
    SDL_SCANCODE_4 = 33,
};

#define SDL_INIT_VIDEO 0u
#define SDL_INIT_TIMER 0u
#define SDL_free free

static inline char *SDL_GetBasePath(void)
{
    return NULL;
}

static inline char *SDL_GetPrefPath(const char *org, const char *app)
{
    const char *state = getenv("E1_DOOM_STATE");
    char *result;
    (void)org;
    (void)app;
    if (!state)
    {
        return NULL;
    }
    result = malloc(strlen(state) + sizeof("/pref"));
    if (result)
    {
        strcpy(result, state);
        strcat(result, "/pref");
    }
    return result;
}

static inline void SDL_QuitSubSystem(unsigned int subsystem)
{
    (void)subsystem;
}

static inline void SDL_Quit(void)
{
}

#endif
