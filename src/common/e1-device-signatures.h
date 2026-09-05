#ifndef E1_DEVICE_SIGNATURES_H
#define E1_DEVICE_SIGNATURES_H

#include <stddef.h>
#include <stdint.h>

enum e1_device_site_kind {
    E1_DEVICE_SITE_PTZ = 0,
    E1_DEVICE_SITE_AUDIO = 1,
};

struct e1_device_site {
    uintptr_t address;
    uint32_t marker;
};

/* The first eight bytes are deliberately masked: they are displaced by the
 * trampoline while a hook is active. The remaining instructions identify the
 * actuator dispatcher that receives channel, decoded command, and speed as
 * direct ABI arguments. The app/Home Hub has more than one message route into
 * PTZ, so this common dispatcher is the first site that observes both short
 * taps and held controls. The two known firmware continuations differ and are
 * kept explicit so each pattern remains unique. */
static const unsigned char e1_ptz_site_pattern_old[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x85, 0xb0, 0x98, 0x46, 0xb1, 0xf1, 0x00, 0x09,
    0x0e, 0x9f, 0x08, 0xda, 0x99, 0x4b, 0x1b, 0x68,
    0xda, 0x04, 0x1e, 0xd4, 0x4f, 0xf0, 0xff, 0x30,
};
static const unsigned char e1_ptz_site_pattern_new[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2d, 0xed, 0x02, 0x8b, 0x87, 0xb0, 0x98, 0x46,
    0xb1, 0xf1, 0x00, 0x09, 0x12, 0x9f, 0x06, 0xda,
    0xb1, 0x4b, 0x1b, 0x68, 0xdd, 0x04, 0x1c, 0xd4,
};
static const unsigned char e1_ptz_site_mask_old[] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
static const unsigned char e1_ptz_site_mask_new[] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
static const unsigned char e1_audio_site_pattern[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9a, 0xb0, 0x4f, 0xf0, 0x00, 0x04, 0xc0, 0xef,
    0x50, 0x00, 0x0c, 0xab, 0x05, 0x92, 0xcc, 0xbf,
    0x52, 0x10, 0x4f, 0xf0, 0xff, 0x32, 0x0a, 0x92,
};
static const unsigned char e1_audio_site_mask[] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
};

static int e1_signature_matches(const unsigned char *candidate,
                                const unsigned char *pattern,
                                const unsigned char *mask, size_t length)
{
    size_t index;

    for (index = 0; index < length; ++index) {
        if (mask[index] != 0U && candidate[index] != pattern[index]) {
            return 0;
        }
    }
    return 1;
}

/* Returns zero for no match, one for exactly one match, and minus one if the
 * signature is ambiguous. Callers aggregate the result across executable
 * mappings and refuse unless the final result is unique. */
static int e1_find_device_site(const unsigned char *mapping, size_t length,
                               uintptr_t mapping_address,
                               enum e1_device_site_kind kind,
                               struct e1_device_site *site)
{
    const unsigned char *pattern = e1_audio_site_pattern;
    const unsigned char *mask = e1_audio_site_mask;
    size_t pattern_length;
    size_t offset;
    int found = 0;

    if (kind == E1_DEVICE_SITE_PTZ) {
        pattern_length = sizeof(e1_ptz_site_pattern_new);
    } else {
        pattern_length = sizeof(e1_audio_site_pattern);
    }
    if (length < pattern_length) {
        return 0;
    }
    for (offset = 0; offset <= length - pattern_length; offset += 2U) {
        int matches;

        if (kind == E1_DEVICE_SITE_PTZ) {
            matches = e1_signature_matches(
                          mapping + offset, e1_ptz_site_pattern_new,
                          e1_ptz_site_mask_new,
                          sizeof(e1_ptz_site_pattern_new)) ||
                      e1_signature_matches(
                          mapping + offset, e1_ptz_site_pattern_old,
                          e1_ptz_site_mask_old,
                          sizeof(e1_ptz_site_pattern_old));
        } else {
            matches = e1_signature_matches(mapping + offset, pattern, mask,
                                           pattern_length);
        }
        if (!matches) {
            continue;
        }
        if (found != 0) {
            return -1;
        }
        site->address = mapping_address + offset;
        site->marker = 0U;
        found = 1;
    }
    return found;
}

#endif
