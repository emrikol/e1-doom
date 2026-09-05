#ifndef E1_I_OALSOUND_H
#define E1_I_OALSOUND_H

#include "doomtype.h"

typedef float ALfloat;
struct sfxinfo_s;

extern boolean oal_use_doppler;
void I_OAL_SetResampler(void);
const char **I_OAL_GetResamplerStrings(void);

#endif
