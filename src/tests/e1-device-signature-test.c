#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "e1-device-functions.h"
#include "e1-device-signatures.h"

static const unsigned char ptz_actuator_old[] = {
    0x2d, 0xe9, 0xf0, 0x4f, 0x04, 0x46, 0x15, 0x46,
    0x85, 0xb0, 0x98, 0x46, 0xb1, 0xf1, 0x00, 0x09,
    0x0e, 0x9f, 0x08, 0xda, 0x99, 0x4b, 0x1b, 0x68,
    0xda, 0x04, 0x1e, 0xd4, 0x4f, 0xf0, 0xff, 0x30,
};
static const unsigned char ptz_actuator_new[] = {
    0x2d, 0xe9, 0xf0, 0x4f, 0x04, 0x46, 0x15, 0x46,
    0x2d, 0xed, 0x02, 0x8b, 0x87, 0xb0, 0x98, 0x46,
    0xb1, 0xf1, 0x00, 0x09, 0x12, 0x9f, 0x06, 0xda,
    0xb1, 0x4b, 0x1b, 0x68, 0xdd, 0x04, 0x1c, 0xd4,
};
static const unsigned char audio[] = {
    0x10, 0xb5, 0x00, 0xf5, 0xcc, 0x20, 0x00, 0x2a,
    0x9a, 0xb0, 0x4f, 0xf0, 0x00, 0x04, 0xc0, 0xef,
    0x50, 0x00, 0x0c, 0xab, 0x05, 0x92, 0xcc, 0xbf,
    0x52, 0x10, 0x4f, 0xf0, 0xff, 0x32, 0x0a, 0x92,
};

static void place_function(unsigned char *mapping, size_t offset,
                           enum e1_device_function_kind kind)
{
    memcpy(mapping + offset, e1_device_function_signatures[kind].bytes,
           E1_DEVICE_FUNCTION_SIGNATURE_BYTES);
}

static void populate_function_mapping(unsigned char *mapping, size_t length)
{
    static const size_t offsets[E1_DEVICE_FUNCTION_COUNT] = {
        [E1_DEVICE_FUNCTION_MEM_ALLOC] = 0x0400,
        [E1_DEVICE_FUNCTION_MEM_FREE] = 0x0500,
        [E1_DEVICE_FUNCTION_VIDEOENC_OPEN] = 0x1000,
        [E1_DEVICE_FUNCTION_VIDEOENC_START] = 0x1100,
        [E1_DEVICE_FUNCTION_VIDEOENC_STOP] = 0x1200,
        [E1_DEVICE_FUNCTION_VIDEOENC_CLOSE] = 0x1300,
        [E1_DEVICE_FUNCTION_VIDEOENC_SET] = 0x1400,
        [E1_DEVICE_FUNCTION_VIDEOPROC_PULL] = 0x1500,
        [E1_DEVICE_FUNCTION_VIDEOPROC_RELEASE] = 0x1600,
        [E1_DEVICE_FUNCTION_MEM_MMAP] = 0x0200,
        [E1_DEVICE_FUNCTION_MEM_MUNMAP] = 0x0300,
        [E1_DEVICE_FUNCTION_AUDIOENC_OPEN] = 0x2000,
        [E1_DEVICE_FUNCTION_AUDIOENC_START] = 0x2100,
        [E1_DEVICE_FUNCTION_AUDIOENC_STOP] = 0x2200,
        [E1_DEVICE_FUNCTION_AUDIOENC_CLOSE] = 0x2300,
        [E1_DEVICE_FUNCTION_AUDIOENC_GET] = 0x2400,
        [E1_DEVICE_FUNCTION_AUDIOENC_SET] = 0x2500,
        [E1_DEVICE_FUNCTION_AUDIOENC_PUSH] = 0x2600,
        [E1_DEVICE_FUNCTION_AUDIOENC_PULL] = 0x2700,
        [E1_DEVICE_FUNCTION_AUDIOENC_RELEASE] = 0x2800,
    };
    size_t kind;

    memset(mapping, 0, length);
    for (kind = 0; kind < E1_DEVICE_FUNCTION_COUNT; ++kind) {
        place_function(mapping, offsets[kind],
                       (enum e1_device_function_kind)kind);
    }
    /* The real firmware contains a second copy of this prologue outside the
     * audio wrapper cluster. */
    place_function(mapping, 0x1800, E1_DEVICE_FUNCTION_AUDIOENC_STOP);
}

static int scan_device_file(const char *path)
{
    const unsigned char *mapping;
    struct e1_device_site ptz_site;
    struct e1_device_functions functions;
    struct stat metadata;
    int descriptor;
    size_t kind;

    descriptor = open(path, O_RDONLY);
    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
        metadata.st_size <= 0) {
        perror(path);
        return 1;
    }
    mapping = mmap(NULL, (size_t)metadata.st_size, PROT_READ, MAP_PRIVATE,
                   descriptor, 0);
    if (mapping == MAP_FAILED) {
        perror(path);
        (void)close(descriptor);
        return 1;
    }
    if (e1_resolve_device_functions(mapping, (size_t)metadata.st_size,
                                    UINT32_C(0x10000), &functions) != 1) {
        fprintf(stderr, "%s: function signatures did not resolve\n", path);
        (void)munmap((void *)mapping, (size_t)metadata.st_size);
        (void)close(descriptor);
        return 1;
    }
    if (e1_find_device_site(mapping, (size_t)metadata.st_size,
                            UINT32_C(0x10000), E1_DEVICE_SITE_PTZ,
                            &ptz_site) != 1) {
        fprintf(stderr, "%s: PTZ signature did not resolve uniquely\n", path);
        (void)munmap((void *)mapping, (size_t)metadata.st_size);
        (void)close(descriptor);
        return 1;
    }
    printf("%s:\n", path);
    printf("  ptz-controller=%#lx\n", (unsigned long)ptz_site.address);
    for (kind = 0; kind < E1_DEVICE_FUNCTION_COUNT; ++kind) {
        printf("  %s=%#lx\n", e1_device_function_signatures[kind].name,
               (unsigned long)functions.address[kind]);
    }
    (void)munmap((void *)mapping, (size_t)metadata.st_size);
    (void)close(descriptor);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char mapping[512] = {0};
    unsigned char function_mapping[0x3000];
    struct e1_device_functions functions;
    struct e1_device_site site;
    const uintptr_t base = UINT32_C(0x10000);

    memcpy(mapping + 32, ptz_actuator_old, sizeof(ptz_actuator_old));
    assert(e1_find_device_site(mapping, sizeof(mapping), base,
                               E1_DEVICE_SITE_PTZ, &site) == 1);
    assert(site.address == base + 32U && site.marker == 0U);

    memcpy(mapping + 32, ptz_actuator_new, sizeof(ptz_actuator_new));
    memset(mapping + 32, 0xa5, 8);
    assert(e1_find_device_site(mapping, sizeof(mapping), base,
                               E1_DEVICE_SITE_PTZ, &site) == 1);
    assert(site.address == base + 32U && site.marker == 0U);

    memcpy(mapping + 128, ptz_actuator_old, sizeof(ptz_actuator_old));
    assert(e1_find_device_site(mapping, sizeof(mapping), base,
                               E1_DEVICE_SITE_PTZ, &site) == -1);

    memset(mapping, 0, sizeof(mapping));
    memcpy(mapping + 64, audio, sizeof(audio));
    memset(mapping + 64, 0x5a, 8);
    assert(e1_find_device_site(mapping, sizeof(mapping), base,
                               E1_DEVICE_SITE_AUDIO, &site) == 1);
    assert(site.address == base + 64U && site.marker == 0U);

    populate_function_mapping(function_mapping, sizeof(function_mapping));
    assert(e1_resolve_device_functions(function_mapping,
                                       sizeof(function_mapping), base,
                                       &functions) == 1);
    assert(functions.address[E1_DEVICE_FUNCTION_MEM_MMAP] == base + 0x0200U);
    assert(functions.address[E1_DEVICE_FUNCTION_VIDEOENC_OPEN] ==
           base + 0x1000U);
    assert(functions.address[E1_DEVICE_FUNCTION_AUDIOENC_STOP] ==
           base + 0x2200U);

    /* A second stop-like function inside the start/close interval is
     * ambiguous and must fail closed. */
    place_function(function_mapping, 0x2280,
                   E1_DEVICE_FUNCTION_AUDIOENC_STOP);
    assert(e1_resolve_device_functions(function_mapping,
                                       sizeof(function_mapping), base,
                                       &functions) == 0);

    /* Missing exact wrappers and implausible ordering are also refused. */
    populate_function_mapping(function_mapping, sizeof(function_mapping));
    memset(function_mapping + 0x1400, 0, E1_DEVICE_FUNCTION_SIGNATURE_BYTES);
    assert(e1_resolve_device_functions(function_mapping,
                                       sizeof(function_mapping), base,
                                       &functions) == 0);
    populate_function_mapping(function_mapping, sizeof(function_mapping));
    memcpy(function_mapping + 0x2450,
           e1_device_function_signatures[E1_DEVICE_FUNCTION_AUDIOENC_CLOSE]
               .bytes,
           E1_DEVICE_FUNCTION_SIGNATURE_BYTES);
    memset(function_mapping + 0x2300, 0, E1_DEVICE_FUNCTION_SIGNATURE_BYTES);
    assert(e1_resolve_device_functions(function_mapping,
                                       sizeof(function_mapping), base,
                                       &functions) == 0);

    puts("e1 device signature tests passed");
    for (int index = 1; index < argc; ++index) {
        if (scan_device_file(argv[index]) != 0) {
            return 1;
        }
    }
    return 0;
}
