#ifndef E1_SDL_ENDIAN_H
#define E1_SDL_ENDIAN_H

#include <stdint.h>

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SDL_SwapLE16(x) ((uint16_t)(x))
#define SDL_SwapLE32(x) ((uint32_t)(x))
#define SDL_SwapBE16(x) __builtin_bswap16((uint16_t)(x))
#define SDL_SwapBE32(x) __builtin_bswap32((uint32_t)(x))
#else
#define SDL_SwapLE16(x) __builtin_bswap16((uint16_t)(x))
#define SDL_SwapLE32(x) __builtin_bswap32((uint32_t)(x))
#define SDL_SwapBE16(x) ((uint16_t)(x))
#define SDL_SwapBE32(x) ((uint32_t)(x))
#endif

#endif
