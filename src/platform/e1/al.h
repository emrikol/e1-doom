#ifndef E1_OPENAL_TYPE_SHIM_H
#define E1_OPENAL_TYPE_SHIM_H

/* The OPL stream module uses only these OpenAL-shaped scalar types and the
 * stereo-S16 format tag.  The camera backend never links or calls OpenAL. */
typedef int ALenum;
typedef int ALsizei;

#define AL_FORMAT_STEREO16 0x1103

#endif
