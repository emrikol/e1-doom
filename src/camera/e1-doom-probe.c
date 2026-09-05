#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <gnu/libc-version.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define BUILD_ID "e1-doom-probe-v1"

#ifndef __ARM_PCS_VFP
#error "The E1 Zoom build requires the ARM hard-float procedure-call ABI"
#endif

static int inspect_path(const char *path)
{
    struct stat metadata;

    if (stat(path, &metadata) != 0) {
        fprintf(stderr, "path=%s status=missing errno=%d\n", path, errno);
        return -1;
    }

    printf("path=%s status=present size=%lld\n", path,
           (long long)metadata.st_size);
    return 0;
}

static int inspect_library(const char *path, const char *symbol)
{
    void *library = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    void *resolved;

    if (library == NULL) {
        fprintf(stderr, "dlopen=%s status=failed error=%s\n", path, dlerror());
        return -1;
    }

    (void)dlerror();
    resolved = dlsym(library, symbol);
    printf("dlopen=%s status=ok symbol=%s resolved=%s\n", path, symbol,
           resolved == NULL ? "no" : "yes");
    (void)dlclose(library);
    return 0;
}

int main(int argc, char **argv)
{
    struct utsname system_name;
    struct timespec now;
    const uint16_t endian_word = UINT16_C(0x0102);
    const unsigned char *endian_bytes = (const unsigned char *)&endian_word;
    int result = 0;

    printf("build_id=%s\n", BUILD_ID);
    printf("pointer_bits=%zu endian=%s arm_hard_float=%s\n",
           sizeof(void *) * 8,
           endian_bytes[0] == 0x02 ? "little" : "big",
#ifdef __ARM_PCS_VFP
           "yes"
#else
           "no"
#endif
    );
    printf("glibc_runtime=%s\n", gnu_get_libc_version());

    if (uname(&system_name) == 0) {
        printf("uname_sysname=%s uname_release=%s uname_machine=%s\n",
               system_name.sysname, system_name.release, system_name.machine);
    } else {
        fprintf(stderr, "uname status=failed errno=%d\n", errno);
        result = 1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        printf("clock_monotonic=ok seconds=%lld\n", (long long)now.tv_sec);
    } else {
        fprintf(stderr, "clock_monotonic=failed errno=%d\n", errno);
        result = 1;
    }

    if (inspect_path("/usr/lib/libhdal.so") != 0 ||
        inspect_path("/mnt/app/libstreambuffer.so") != 0 ||
        inspect_path("/mnt/sda") != 0 ||
        inspect_path("/mnt/tmp") != 0) {
        result = 1;
    }

    if (argc == 2 && strcmp(argv[1], "--dlopen") == 0) {
        if (inspect_library("/usr/lib/libhdal.so", "hd_common_init") != 0 ||
            inspect_library("/mnt/app/libstreambuffer.so", "sb_write_video") != 0) {
            result = 1;
        }
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--dlopen]\n", argv[0]);
        return 2;
    }

    return result;
}
