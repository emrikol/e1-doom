#ifndef E1_DEVICE_FUNCTIONS_H
#define E1_DEVICE_FUNCTIONS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define E1_DEVICE_FUNCTION_SIGNATURE_BYTES 12U

enum e1_device_function_kind {
    E1_DEVICE_FUNCTION_MEM_ALLOC = 0,
    E1_DEVICE_FUNCTION_MEM_FREE,
    E1_DEVICE_FUNCTION_VIDEOENC_OPEN,
    E1_DEVICE_FUNCTION_VIDEOENC_START,
    E1_DEVICE_FUNCTION_VIDEOENC_STOP,
    E1_DEVICE_FUNCTION_VIDEOENC_CLOSE,
    E1_DEVICE_FUNCTION_VIDEOENC_SET,
    E1_DEVICE_FUNCTION_VIDEOPROC_PULL,
    E1_DEVICE_FUNCTION_VIDEOPROC_RELEASE,
    E1_DEVICE_FUNCTION_MEM_MMAP,
    E1_DEVICE_FUNCTION_MEM_MUNMAP,
    E1_DEVICE_FUNCTION_AUDIOENC_OPEN,
    E1_DEVICE_FUNCTION_AUDIOENC_START,
    E1_DEVICE_FUNCTION_AUDIOENC_STOP,
    E1_DEVICE_FUNCTION_AUDIOENC_CLOSE,
    E1_DEVICE_FUNCTION_AUDIOENC_GET,
    E1_DEVICE_FUNCTION_AUDIOENC_SET,
    E1_DEVICE_FUNCTION_AUDIOENC_PUSH,
    E1_DEVICE_FUNCTION_AUDIOENC_PULL,
    E1_DEVICE_FUNCTION_AUDIOENC_RELEASE,
    E1_DEVICE_FUNCTION_COUNT,
};

struct e1_device_function_signature {
    const char *name;
    unsigned char bytes[E1_DEVICE_FUNCTION_SIGNATURE_BYTES];
};

struct e1_device_functions {
    uintptr_t address[E1_DEVICE_FUNCTION_COUNT];
};

static const struct e1_device_function_signature
    e1_device_function_signatures[E1_DEVICE_FUNCTION_COUNT] = {
        {"mem-alloc",
         {0xc0, 0xef, 0x50, 0x00, 0x2d, 0xe9, 0xf0, 0x4f, 0x97, 0xb0, 0xdf,
          0xf8}},
        {"mem-free",
         {0x2d, 0xe9, 0xf0, 0x41, 0x04, 0x46, 0xdf, 0xf8, 0xdc, 0x80, 0x88,
          0xb0}},
        {"video-open",
         {0x2d, 0xe9, 0xf0, 0x4f, 0x8b, 0xb0, 0xdf, 0xf8, 0x84, 0x67, 0x17,
          0x46}},
        {"video-start",
         {0x2d, 0xe9, 0xf0, 0x4f, 0xc5, 0xb2, 0xdf, 0xf8, 0x04, 0x45, 0xc9,
          0xb0}},
        {"video-stop",
         {0x2d, 0xe9, 0xf0, 0x47, 0xc5, 0xb2, 0xdf, 0xf8, 0x00, 0x83, 0x86,
          0xb0}},
        {"video-close",
         {0x2d, 0xe9, 0xf0, 0x4f, 0x04, 0x46, 0xdf, 0xf8, 0x20, 0x54, 0x9f,
          0xb0}},
        {"video-set",
         {0xc0, 0xef, 0x50, 0x00, 0x2d, 0xe9, 0xf0, 0x4f, 0xf5, 0xb0, 0xdf,
          0xf8}},
        {"videoproc-pull",
         {0x2d, 0xe9, 0xf0, 0x4f, 0xbd, 0xb0, 0x52, 0x4e, 0x52, 0x4b, 0x7e,
          0x44}},
        {"videoproc-release",
         {0x2d, 0xe9, 0xf0, 0x47, 0xbc, 0xb0, 0x4a, 0x4e, 0x4a, 0x4b, 0x7e,
          0x44}},
        {"mem-mmap",
         {0x2d, 0xe9, 0xf0, 0x43, 0x85, 0xb0, 0x50, 0x4e, 0x50, 0x4b, 0x7e,
          0x44}},
        {"mem-munmap",
         {0x70, 0xb5, 0x84, 0xb0, 0x29, 0x4e, 0x2a, 0x4b, 0x7e, 0x44, 0xf3,
          0x58}},
        {"audio-open",
         {0x2d, 0xe9, 0xf0, 0x4f, 0x8d, 0xb0, 0xdf, 0xf8, 0x0c, 0x54, 0x0c,
          0x46}},
        {"audio-start",
         {0x2d, 0xe9, 0xf0, 0x42, 0x04, 0x46, 0x70, 0x4e, 0x88, 0xb0, 0x70,
          0x4b}},
        {"audio-stop",
         {0x2d, 0xe9, 0xf0, 0x41, 0x04, 0x46, 0x51, 0x4d, 0x86, 0xb0, 0x51,
          0x4b}},
        {"audio-close",
         {0xc0, 0xef, 0x50, 0x00, 0x2d, 0xe9, 0xf0, 0x4f, 0x99, 0xb0, 0xf3,
          0x4c}},
        {"audio-get",
         {0x2d, 0xe9, 0xf0, 0x47, 0x88, 0xb0, 0xdf, 0xf8, 0xa0, 0x83, 0x0c,
          0x46}},
        {"audio-set",
         {0xc0, 0xef, 0x50, 0x00, 0x2d, 0xe9, 0xf0, 0x4f, 0x95, 0xb0, 0xdf,
          0xf8}},
        {"audio-push",
         {0x2d, 0xe9, 0xf0, 0x4f, 0x0d, 0x46, 0x77, 0x4e, 0xbf, 0xb0, 0x77,
          0x4a}},
        {"audio-pull",
         {0x2d, 0xe9, 0xf0, 0x4e, 0x04, 0x46, 0x6c, 0x4e, 0xbe, 0xb0, 0x6c,
          0x4b}},
        {"audio-release",
         {0x2d, 0xe9, 0xf0, 0x4f, 0x0e, 0x46, 0x53, 0x4d, 0xbd, 0xb0, 0x53,
          0x4b}},
};

static int e1_device_function_range_ordered(
    const struct e1_device_functions *functions,
    enum e1_device_function_kind first, enum e1_device_function_kind last,
    const enum e1_device_function_kind *order, size_t order_count)
{
    size_t index;

    if (functions->address[last] <= functions->address[first] ||
        functions->address[last] - functions->address[first] >
            UINT32_C(0x20000)) {
        return 0;
    }
    for (index = 1; index < order_count; ++index) {
        if (functions->address[order[index]] <=
            functions->address[order[index - 1U]]) {
            return 0;
        }
    }
    return 1;
}

/* Resolve the statically linked HDAL wrappers from their function prologues.
 * Every signature must be unique except audio-stop: that prologue is shared,
 * so it is accepted only when exactly one candidate lies between the uniquely
 * resolved audio-start and audio-close functions. The independent ordering
 * checks make a coincidental set of byte matches fail closed. */
static int e1_resolve_device_functions(const unsigned char *mapping,
                                       size_t length,
                                       uintptr_t mapping_address,
                                       struct e1_device_functions *functions)
{
    static const enum e1_device_function_kind memory_order[] = {
        E1_DEVICE_FUNCTION_MEM_MMAP, E1_DEVICE_FUNCTION_MEM_MUNMAP,
        E1_DEVICE_FUNCTION_MEM_ALLOC, E1_DEVICE_FUNCTION_MEM_FREE,
    };
    static const enum e1_device_function_kind video_order[] = {
        E1_DEVICE_FUNCTION_VIDEOENC_OPEN, E1_DEVICE_FUNCTION_VIDEOENC_START,
        E1_DEVICE_FUNCTION_VIDEOENC_STOP, E1_DEVICE_FUNCTION_VIDEOENC_CLOSE,
        E1_DEVICE_FUNCTION_VIDEOENC_SET,
    };
    static const enum e1_device_function_kind audio_order[] = {
        E1_DEVICE_FUNCTION_AUDIOENC_OPEN, E1_DEVICE_FUNCTION_AUDIOENC_START,
        E1_DEVICE_FUNCTION_AUDIOENC_STOP, E1_DEVICE_FUNCTION_AUDIOENC_CLOSE,
        E1_DEVICE_FUNCTION_AUDIOENC_GET, E1_DEVICE_FUNCTION_AUDIOENC_SET,
        E1_DEVICE_FUNCTION_AUDIOENC_PUSH, E1_DEVICE_FUNCTION_AUDIOENC_PULL,
        E1_DEVICE_FUNCTION_AUDIOENC_RELEASE,
    };
    size_t kind;
    size_t offset;
    size_t audio_stop_matches = 0;
    uintptr_t audio_stop = 0;

    memset(functions, 0, sizeof(*functions));
    if (length < E1_DEVICE_FUNCTION_SIGNATURE_BYTES ||
        mapping_address > UINTPTR_MAX - length) {
        return 0;
    }
    for (kind = 0; kind < E1_DEVICE_FUNCTION_COUNT; ++kind) {
        size_t matches = 0;

        for (offset = 0;
             offset <= length - E1_DEVICE_FUNCTION_SIGNATURE_BYTES;
             offset += 2U) {
            if (memcmp(mapping + offset,
                       e1_device_function_signatures[kind].bytes,
                       E1_DEVICE_FUNCTION_SIGNATURE_BYTES) != 0) {
                continue;
            }
            ++matches;
            if (kind != E1_DEVICE_FUNCTION_AUDIOENC_STOP && matches == 1U) {
                functions->address[kind] = mapping_address + offset;
            }
        }
        if (kind == E1_DEVICE_FUNCTION_AUDIOENC_STOP) {
            audio_stop_matches = matches;
        } else if (matches != 1U) {
            return 0;
        }
    }
    if (audio_stop_matches == 0U) {
        return 0;
    }
    for (offset = 0; offset <= length - E1_DEVICE_FUNCTION_SIGNATURE_BYTES;
         offset += 2U) {
        uintptr_t candidate = mapping_address + offset;

        if (memcmp(mapping + offset,
                   e1_device_function_signatures
                       [E1_DEVICE_FUNCTION_AUDIOENC_STOP]
                           .bytes,
                   E1_DEVICE_FUNCTION_SIGNATURE_BYTES) == 0 &&
            candidate > functions->address[E1_DEVICE_FUNCTION_AUDIOENC_START] &&
            candidate < functions->address[E1_DEVICE_FUNCTION_AUDIOENC_CLOSE]) {
            if (audio_stop != 0U) {
                return 0;
            }
            audio_stop = candidate;
        }
    }
    if (audio_stop == 0U) {
        return 0;
    }
    functions->address[E1_DEVICE_FUNCTION_AUDIOENC_STOP] = audio_stop;
    if (!e1_device_function_range_ordered(
            functions, E1_DEVICE_FUNCTION_MEM_MMAP,
            E1_DEVICE_FUNCTION_MEM_FREE, memory_order,
            sizeof(memory_order) / sizeof(memory_order[0])) ||
        !e1_device_function_range_ordered(
            functions, E1_DEVICE_FUNCTION_VIDEOENC_OPEN,
            E1_DEVICE_FUNCTION_VIDEOENC_SET, video_order,
            sizeof(video_order) / sizeof(video_order[0])) ||
        !e1_device_function_range_ordered(
            functions, E1_DEVICE_FUNCTION_AUDIOENC_OPEN,
            E1_DEVICE_FUNCTION_AUDIOENC_RELEASE, audio_order,
            sizeof(audio_order) / sizeof(audio_order[0]))) {
        memset(functions, 0, sizeof(*functions));
        return 0;
    }
    return 1;
}

#endif
