#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "e1-device-signatures.h"

#define BUILD_ID "e1-ptz-patch-supervisor-v8"
#define STATE_DIR "/mnt/tmp/e1-doom/state"
#define DUMMY_TARGET "/mnt/tmp/e1-doom/e1-ptz-patch-dummy"
#define DUMMY_OWNER "/mnt/tmp/e1-doom/e1-ptz-hook-owner-dummy"
#define DEVICE_TARGET "/mnt/app/device"
#define DEVICE_LIBRARY "/mnt/tmp/e1-doom/e1-doom-injected.so"
#define DOOM_DEV_TARGET "/mnt/sda/e1-doom-dev/e1-doom"
#define DOOM_MOUNT_TARGET "/mnt/tmp/e1-doom/root/bin/e1-doom"
#define DOOM_FLAT_TARGET "/mnt/sda/e1-doom-flat/bin/e1-doom"
#define CONTROLLER_DEV_TARGET "/mnt/sda/e1-doom-dev/e1-doom-controller"
#define CONTROLLER_MOUNT_TARGET "/mnt/tmp/e1-doom/root/bin/e1-doom-controller"
#define CONTROLLER_FLAT_TARGET "/mnt/sda/e1-doom-flat/bin/e1-doom-controller"
#define PATCH_MARKER STATE_DIR "/ptz.patch.active"
#define AUDIO_PATCH_MARKER STATE_DIR "/audio.patch.active"
#define MAX_THREADS 128U
#define STOP_TIMEOUT_MS 2000U
#define GO_TIMEOUT_MS 30000U

static const unsigned char original_bytes[8] = {
    0x11, 0x20, 0x70, 0x47, 0x00, 0xbf, 0x00, 0xbf
};
static const unsigned char patched_bytes[8] = {
    0x22, 0x20, 0x70, 0x47, 0x00, 0xbf, 0x00, 0xbf
};
static const unsigned char device_original_bytes[8] = {
    0x2d, 0xe9, 0xf0, 0x4f, 0x04, 0x46, 0x15, 0x46
};
static const unsigned char device_audio_original_bytes[8] = {
    0x10, 0xb5, 0x00, 0xf5, 0xcc, 0x20, 0x00, 0x2a
};

struct patch_spec {
    const char *label;
    const char *target_path;
    const char *mapping_path;
    const unsigned char *original;
    const unsigned char *patch;
};

struct thread_entry {
    pid_t tid;
    int stopped;
};

struct thread_set {
    struct thread_entry entries[MAX_THREADS];
    size_t count;
};

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static void delay_ms(unsigned int milliseconds)
{
    struct timespec delay;

    delay.tv_sec = (time_t)(milliseconds / 1000U);
    delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int state_path(char *output, size_t size, const char *name)
{
    int length = snprintf(output, size, "%s/%s", STATE_DIR, name);

    return length >= 0 && (size_t)length < size ? 0 : -1;
}

static int write_state(const char *name, const char *format, ...)
{
    char path[PATH_MAX];
    char buffer[512];
    va_list arguments;
    int descriptor;
    int length;
    ssize_t written;

    if (state_path(path, sizeof(path), name) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    va_start(arguments, format);
    length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        errno = EOVERFLOW;
        return -1;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return -1;
    }
    written = write(descriptor, buffer, (size_t)length);
    if (written != length || fsync(descriptor) != 0) {
        (void)close(descriptor);
        return -1;
    }
    if (close(descriptor) != 0) {
        return -1;
    }
    return 0;
}

static int parse_pid(const char *text, pid_t *pid)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > 4194304UL) {
        return -1;
    }
    *pid = (pid_t)value;
    return 0;
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int verify_executable(pid_t pid, const char *expected)
{
    char proc_path[64];
    char executable[PATH_MAX];
    ssize_t length;

    if (snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", (long)pid) >=
        (int)sizeof(proc_path)) {
        return -1;
    }
    length = readlink(proc_path, executable, sizeof(executable) - 1U);
    if (length < 0 || (size_t)length >= sizeof(executable)) {
        return -1;
    }
    executable[length] = '\0';
    if (strcmp(executable, expected) != 0) {
        fprintf(stderr, "refusing target exe=%s expected=%s\n", executable,
                expected);
        errno = EPERM;
        return -1;
    }
    return 0;
}

static int address_is_executable(pid_t pid, uint32_t address,
                                 const char *mapping_path)
{
    char maps_path[64];
    char line[PATH_MAX + 128];
    FILE *maps;
    int result = 0;

    if (snprintf(maps_path, sizeof(maps_path), "/proc/%ld/maps", (long)pid) >=
        (int)sizeof(maps_path)) {
        return 0;
    }
    maps = fopen(maps_path, "r");
    if (maps == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long start;
        unsigned long end;
        char permissions[5] = {0};

        if (sscanf(line, "%lx-%lx %4s", &start, &end, permissions) == 3 &&
            address >= start && (uint64_t)address + sizeof(original_bytes) <= end &&
            permissions[0] == 'r' && permissions[2] == 'x' &&
            strstr(line, mapping_path) != NULL) {
            result = 1;
            break;
        }
    }
    (void)fclose(maps);
    return result;
}

static int wait_stopped(pid_t tid, int *exited)
{
    uint64_t deadline = monotonic_ms() + STOP_TIMEOUT_MS;

    *exited = 0;
    while (monotonic_ms() < deadline) {
        int status = 0;
        pid_t result = waitpid(tid, &status, __WALL | WNOHANG);

        if (result == tid) {
            if (WIFSTOPPED(status)) {
                return 0;
            }
            *exited = 1;
            errno = ESRCH;
            return -1;
        }
        if (result == 0) {
            delay_ms(10);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == ECHILD || errno == ESRCH)) {
            *exited = 1;
        }
        return -1;
    }
    errno = ETIMEDOUT;
    return -1;
}

static int thread_index(const struct thread_set *threads, pid_t tid)
{
    size_t index;

    for (index = 0; index < threads->count; ++index) {
        if (threads->entries[index].tid == tid) {
            return (int)index;
        }
    }
    return -1;
}

static int parse_tid_name(const char *name, pid_t *tid)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(name, &end, 10);
    if (errno != 0 || end == name || *end != '\0' || value == 0 ||
        value > 4194304UL) {
        return -1;
    }
    *tid = (pid_t)value;
    return 0;
}

static int attach_all_threads(pid_t pid, struct thread_set *threads)
{
    char task_path[64];
    unsigned int pass;

    memset(threads, 0, sizeof(*threads));
    if (snprintf(task_path, sizeof(task_path), "/proc/%ld/task", (long)pid) >=
        (int)sizeof(task_path)) {
        return -1;
    }
    for (pass = 0; pass < 16U; ++pass) {
        DIR *directory = opendir(task_path);
        struct dirent *entry;
        size_t added = 0;

        if (directory == NULL) {
            return -1;
        }
        while ((entry = readdir(directory)) != NULL) {
            pid_t tid;
            int exited = 0;

            if (parse_tid_name(entry->d_name, &tid) != 0 ||
                thread_index(threads, tid) >= 0) {
                continue;
            }
            if (threads->count >= MAX_THREADS) {
                (void)closedir(directory);
                errno = E2BIG;
                return -1;
            }
            if (ptrace(PTRACE_SEIZE, tid, NULL, NULL) != 0) {
                if (errno == ESRCH) {
                    continue;
                }
                (void)closedir(directory);
                return -1;
            }
            threads->entries[threads->count].tid = tid;
            threads->entries[threads->count].stopped = 0;
            ++threads->count;
            ++added;
            if (ptrace(PTRACE_INTERRUPT, tid, NULL, NULL) != 0) {
                if (errno == ESRCH) {
                    threads->entries[threads->count - 1U].tid = 0;
                    --threads->count;
                    --added;
                    continue;
                }
                (void)closedir(directory);
                return -1;
            }
            if (wait_stopped(tid, &exited) != 0) {
                if (exited) {
                    threads->entries[threads->count - 1U].tid = 0;
                    --threads->count;
                    --added;
                    continue;
                }
                (void)closedir(directory);
                return -1;
            }
            threads->entries[threads->count - 1U].stopped = 1;
        }
        if (closedir(directory) != 0) {
            return -1;
        }
        if (added == 0U) {
            if (thread_index(threads, pid) < 0) {
                errno = ESRCH;
                return -1;
            }
            return 0;
        }
    }
    errno = EAGAIN;
    return -1;
}

static int detach_all_threads(struct thread_set *threads)
{
    size_t index;
    int result = 0;

    for (index = threads->count; index > 0; --index) {
        struct thread_entry *entry = &threads->entries[index - 1U];

        if (!entry->stopped) {
            int exited = 0;
            if (ptrace(PTRACE_INTERRUPT, entry->tid, NULL, NULL) == 0 &&
                wait_stopped(entry->tid, &exited) == 0) {
                entry->stopped = 1;
            } else if (errno != ESRCH && !exited) {
                result = -1;
                continue;
            }
        }
        if (entry->stopped &&
            ptrace(PTRACE_DETACH, entry->tid, NULL, NULL) != 0 &&
            errno != ESRCH) {
            result = -1;
        }
        entry->stopped = 0;
    }
    return result;
}

static int read_remote(pid_t tid, uint32_t address, unsigned char *bytes,
                       size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        long word;
        size_t take = sizeof(word);

        errno = 0;
        word = ptrace(PTRACE_PEEKDATA, tid,
                      (void *)(uintptr_t)(address + (uint32_t)offset), NULL);
        if (word == -1 && errno != 0) {
            return -1;
        }
        if (take > length - offset) {
            take = length - offset;
        }
        memcpy(bytes + offset, &word, take);
        offset += take;
    }
    return 0;
}

/* Exact-byte inspection does not require stopping the target.  The old path
 * seized and interrupted every device thread for each read-only verification,
 * so normal shutdown performed several avoidable stop-the-world cycles around
 * the two real patch restorations.  On this appliance those pauses compete
 * with the stock module-heartbeat deadline. */
static int read_remote_live(pid_t pid, uint32_t address, unsigned char *bytes,
                            size_t length)
{
    char path[64];
    int descriptor;
    ssize_t count;

    if (snprintf(path, sizeof(path), "/proc/%ld/mem", (long)pid) >=
        (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return -1;
    }
    count = pread(descriptor, bytes, length, (off_t)address);
    if (close(descriptor) != 0 && count == (ssize_t)length) {
        return -1;
    }
    if (count != (ssize_t)length) {
        if (count >= 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

static int resolve_device_site(pid_t pid, enum e1_device_site_kind kind,
                               struct e1_device_site *resolved)
{
    char maps_path[64];
    char memory_path[64];
    char line[PATH_MAX + 128];
    struct e1_device_site candidate;
    FILE *maps = NULL;
    int memory = -1;
    int matches = 0;
    int result = -1;

    if (verify_executable(pid, DEVICE_TARGET) != 0 ||
        snprintf(maps_path, sizeof(maps_path), "/proc/%ld/maps", (long)pid) >=
            (int)sizeof(maps_path) ||
        snprintf(memory_path, sizeof(memory_path), "/proc/%ld/mem", (long)pid) >=
            (int)sizeof(memory_path)) {
        return -1;
    }
    maps = fopen(maps_path, "r");
    memory = open(memory_path, O_RDONLY | O_CLOEXEC);
    if (maps == NULL || memory < 0) {
        goto done;
    }
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long start;
        unsigned long end;
        char permissions[5] = {0};
        unsigned char *bytes;
        size_t length;
        ssize_t count;
        int found;

        if (sscanf(line, "%lx-%lx %4s", &start, &end, permissions) != 3 ||
            permissions[0] != 'r' || permissions[2] != 'x' ||
            strstr(line, DEVICE_TARGET) == NULL || end <= start ||
            end - start > UINT32_C(32 * 1024 * 1024)) {
            continue;
        }
        length = (size_t)(end - start);
        bytes = malloc(length);
        if (bytes == NULL) {
            goto done;
        }
        count = pread(memory, bytes, length, (off_t)start);
        if (count != (ssize_t)length) {
            free(bytes);
            goto done;
        }
        found = e1_find_device_site(bytes, length, (uintptr_t)start, kind,
                                    &candidate);
        free(bytes);
        if (found < 0 || (found == 1 && matches != 0)) {
            errno = ENOTUNIQ;
            goto done;
        }
        if (found == 1) {
            *resolved = candidate;
            matches = 1;
        }
    }
    if (matches != 1 || resolved->address > UINT32_MAX ||
        (resolved->address & 3U) != 0U) {
        errno = ENOENT;
        goto done;
    }
    printf("resolve target=%s address=%#lx\n",
           kind == E1_DEVICE_SITE_PTZ ? "device" : "device-audio",
           (unsigned long)resolved->address);
    result = 0;

done:
    if (memory >= 0) {
        (void)close(memory);
    }
    if (maps != NULL) {
        (void)fclose(maps);
    }
    return result;
}

static int write_remote(pid_t tid, uint32_t address,
                        const unsigned char *bytes, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        long word = 0;
        size_t take = sizeof(word);

        if (take > length - offset) {
            errno = 0;
            word = ptrace(PTRACE_PEEKDATA, tid,
                          (void *)(uintptr_t)(address + (uint32_t)offset),
                          NULL);
            if (word == -1 && errno != 0) {
                return -1;
            }
            take = length - offset;
        }
        memcpy(&word, bytes + offset, take);
        if (ptrace(PTRACE_POKEDATA, tid,
                   (void *)(uintptr_t)(address + (uint32_t)offset),
                   (void *)(uintptr_t)(unsigned long)word) != 0) {
            return -1;
        }
        offset += take;
    }
    return 0;
}

static int is_safe_partial_patch(const struct patch_spec *spec,
                                 const unsigned char *current)
{
    size_t offset;

    for (offset = 0; offset < sizeof(original_bytes); offset += sizeof(long)) {
        if (memcmp(current + offset, spec->original + offset, sizeof(long)) != 0 &&
            memcmp(current + offset, spec->patch + offset, sizeof(long)) != 0) {
            return 0;
        }
    }
    return 1;
}

static int inspect_or_modify(pid_t pid, uint32_t address, int operation,
                             const struct patch_spec *spec)
{
    struct thread_set threads;
    unsigned char current[sizeof(original_bytes)];
    unsigned char verified[sizeof(original_bytes)];
    int result = -1;
    int attached = 0;

    memset(&threads, 0, sizeof(threads));
    if (verify_executable(pid, spec->target_path) != 0 ||
        (address & 3U) != 0U ||
        !address_is_executable(pid, address, spec->mapping_path)) {
        fprintf(stderr, "validate %s target failed\n", spec->label);
        return -1;
    }
    if (operation == 0 || operation == 2) {
        const unsigned char *expected =
            operation == 0 ? spec->original : spec->patch;

        if (read_remote_live(pid, address, current, sizeof(current)) != 0) {
            perror("read live patch site");
            return -1;
        }
        printf("%s target=%s address=%#x bytes=",
               operation == 0 ? "inspect" : "inspect-patch",
               spec->label, address);
        for (size_t index = 0; index < sizeof(current); ++index) {
            printf("%02x", current[index]);
        }
        printf(" threads=running\n");
        return memcmp(current, expected, sizeof(current)) == 0 ? 0 : -1;
    }
    attached = 1;
    if (attach_all_threads(pid, &threads) != 0) {
        perror("validate/stop dummy target");
        (void)detach_all_threads(&threads);
        return -1;
    }
    if (read_remote(pid, address, current, sizeof(current)) != 0) {
        perror("read patch site");
        goto done;
    }
    if (operation == 1) {
        if (memcmp(current, spec->original, sizeof(current)) != 0) {
            fprintf(stderr, "install refused: original bytes differ\n");
            goto done;
        }
        if (write_remote(pid, address, spec->patch, sizeof(patched_bytes)) != 0 ||
            read_remote(pid, address, verified, sizeof(verified)) != 0 ||
            memcmp(verified, spec->patch, sizeof(verified)) != 0) {
            perror("install/verify patch");
            if (write_remote(pid, address, spec->original,
                             sizeof(original_bytes)) != 0) {
                perror("rollback failed after install error");
            }
            goto done;
        }
        printf("install target=%s address=%#x threads=%zu status=verified\n",
               spec->label, address, threads.count);
        result = 0;
        goto done;
    }
    if (!is_safe_partial_patch(spec, current)) {
        fprintf(stderr, "restore refused: patch site has unknown bytes\n");
        goto done;
    }
    if (write_remote(pid, address, spec->original, sizeof(original_bytes)) != 0 ||
        read_remote(pid, address, verified, sizeof(verified)) != 0 ||
        memcmp(verified, spec->original, sizeof(verified)) != 0) {
        perror("restore/verify patch");
        goto done;
    }
    printf("restore target=%s address=%#x threads=%zu status=verified\n",
           spec->label, address, threads.count);
    result = 0;

done:
    if (attached && detach_all_threads(&threads) != 0) {
        perror("detach target threads");
        result = -1;
    }
    return result;
}

static void build_device_spec(uint32_t hook_address,
                              unsigned char device_patch[8],
                              struct patch_spec *spec)
{
    static const unsigned char jump_prefix[4] = {0xdf, 0xf8, 0x00, 0xf0};

    memcpy(device_patch, jump_prefix, sizeof(jump_prefix));
    memcpy(device_patch + 4, &hook_address, sizeof(hook_address));
    spec->label = "device";
    spec->target_path = DEVICE_TARGET;
    spec->mapping_path = DEVICE_TARGET;
    spec->original = device_original_bytes;
    spec->patch = device_patch;
}

static void build_device_audio_spec(uint32_t hook_address,
                                    unsigned char device_patch[8],
                                    struct patch_spec *spec)
{
    static const unsigned char jump_prefix[4] = {0xdf, 0xf8, 0x00, 0xf0};

    memcpy(device_patch, jump_prefix, sizeof(jump_prefix));
    memcpy(device_patch + 4, &hook_address, sizeof(hook_address));
    spec->label = "device-audio";
    spec->target_path = DEVICE_TARGET;
    spec->mapping_path = DEVICE_TARGET;
    spec->original = device_audio_original_bytes;
    spec->patch = device_patch;
}

static int path_exists(const char *name)
{
    char path[PATH_MAX];

    return state_path(path, sizeof(path), name) == 0 && access(path, F_OK) == 0;
}

static int wait_for_go(const char *marker)
{
    uint64_t deadline = monotonic_ms() + GO_TIMEOUT_MS;

    while (!stop_requested && monotonic_ms() < deadline) {
        if (path_exists(marker)) {
            return 0;
        }
        delay_ms(20);
    }
    errno = stop_requested ? EINTR : ETIMEDOUT;
    return -1;
}

static int wait_for_owner(pid_t owner)
{
    while (!stop_requested) {
        int status = 0;
        pid_t result = waitpid(owner, &status, WNOHANG);

        if (result == owner) {
            printf("owner-exit pid=%ld status=%#x\n", (long)owner, status);
            return 0;
        }
        if (result < 0 && errno != EINTR) {
            return -1;
        }
        delay_ms(20);
    }
    (void)kill(owner, SIGTERM);
    for (unsigned int attempt = 0; attempt < 50U; ++attempt) {
        int status = 0;
        pid_t result = waitpid(owner, &status, WNOHANG);

        if (result == owner || (result < 0 && errno == ECHILD)) {
            return 0;
        }
        delay_ms(20);
    }
    (void)kill(owner, SIGKILL);
    while (waitpid(owner, NULL, 0) < 0 && errno == EINTR) {
    }
    return 0;
}

static int verify_doom_owner(pid_t owner, int noisy)
{
    char proc_path[64];
    char executable[PATH_MAX];
    ssize_t length;

    if (snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", (long)owner) >=
        (int)sizeof(proc_path)) {
        return -1;
    }
    length = readlink(proc_path, executable, sizeof(executable) - 1U);
    if (length < 0 || (size_t)length >= sizeof(executable)) {
        return -1;
    }
    executable[length] = '\0';
    if (strcmp(executable, DOOM_DEV_TARGET) != 0 &&
        strcmp(executable, DOOM_MOUNT_TARGET) != 0 &&
        strcmp(executable, DOOM_FLAT_TARGET) != 0 &&
        strcmp(executable, CONTROLLER_DEV_TARGET) != 0 &&
        strcmp(executable, CONTROLLER_MOUNT_TARGET) != 0 &&
        strcmp(executable, CONTROLLER_FLAT_TARGET) != 0) {
        if (noisy) {
            fprintf(stderr, "refusing PTZ owner exe=%s\n", executable);
        }
        errno = EPERM;
        return -1;
    }
    return 0;
}

static int wait_for_doom_owner(pid_t owner)
{
    while (!stop_requested) {
        if (verify_doom_owner(owner, 0) != 0) {
            printf("doom-owner-exit pid=%ld\n", (long)owner);
            return 0;
        }
        delay_ms(20);
    }

    if (verify_doom_owner(owner, 0) == 0) {
        (void)kill(owner, SIGTERM);
        for (unsigned int attempt = 0; attempt < 50U; ++attempt) {
            if (verify_doom_owner(owner, 0) != 0) {
                return 0;
            }
            delay_ms(20);
        }
        (void)kill(owner, SIGKILL);
        for (unsigned int attempt = 0; attempt < 100U; ++attempt) {
            if (verify_doom_owner(owner, 0) != 0) {
                return 0;
            }
            delay_ms(20);
        }
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int supervise_dummy(pid_t target, uint32_t address)
{
    const struct patch_spec spec = {
        "dummy", DUMMY_TARGET, DUMMY_TARGET, original_bytes, patched_bytes
    };
    pid_t owner = -1;
    int patch_installed = 0;
    int marker_written = 0;
    int result = 1;

    if (inspect_or_modify(target, address, 0, &spec) != 0) {
        fprintf(stderr, "initial dummy inspection failed\n");
        goto done;
    }
    if (write_state("ptz-supervisor.ready", "ready\n") != 0) {
        goto done;
    }
    if (wait_for_go("ptz-dummy.go") != 0) {
        perror("wait for mutation release");
        goto done;
    }
    owner = fork();
    if (owner < 0) {
        perror("fork owner");
        goto done;
    }
    if (owner == 0) {
        execl(DUMMY_OWNER, DUMMY_OWNER, (char *)NULL);
        _exit(127);
    }
    if (write_state("ptz-owner.pid", "%ld\n", (long)owner) != 0 ||
        write_state("ptz.patch.active",
                    "version=1 mode=dummy target=%ld address=%#x length=8 "
                    "original=1120704700bf00bf patch=2220704700bf00bf\n",
                    (long)target, address) != 0) {
        goto done;
    }
    marker_written = 1;
    if (inspect_or_modify(target, address, 1, &spec) != 0) {
        goto done;
    }
    patch_installed = 1;
    if (write_state("ptz-supervisor.active", "active\n") != 0) {
        goto done;
    }
    (void)wait_for_owner(owner);
    owner = -1;
    if (inspect_or_modify(target, address, -1, &spec) != 0) {
        fprintf(stderr, "restore failed\n");
        goto done;
    }
    patch_installed = 0;
    if (unlink(PATCH_MARKER) != 0 && errno != ENOENT) {
        goto done;
    }
    marker_written = 0;
    (void)unlink(STATE_DIR "/ptz-supervisor.active");
    if (write_state("ptz.restored", "restored\n") != 0) {
        goto done;
    }
    result = 0;

done:
    if (owner > 0) {
        (void)kill(owner, SIGKILL);
        while (waitpid(owner, NULL, 0) < 0 && errno == EINTR) {
        }
    }
    if (patch_installed && inspect_or_modify(target, address, -1, &spec) == 0) {
        patch_installed = 0;
        (void)unlink(PATCH_MARKER);
        marker_written = 0;
        (void)unlink(STATE_DIR "/ptz-supervisor.active");
        (void)write_state("ptz.restored", "restored-after-error\n");
    }
    if (result != 0) {
        (void)write_state("ptz.recovery.failed",
                          patch_installed ? "patch-still-installed\n" :
                                            "supervisor-error\n");
    }
    if (!patch_installed && marker_written) {
        (void)unlink(PATCH_MARKER);
    }
    (void)unlink(STATE_DIR "/ptz-supervisor.ready");
    printf("supervisor-exit build_id=%s result=%d\n", BUILD_ID, result);
    return result;
}

static int wait_for_marker(const char *name, unsigned int timeout_ms)
{
    uint64_t deadline = monotonic_ms() + timeout_ms;

    while (monotonic_ms() < deadline) {
        if (path_exists(name)) {
            return 0;
        }
        delay_ms(20);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int supervise_device_log(pid_t target, uint32_t hook_address)
{
    unsigned char device_patch[8];
    struct patch_spec spec;
    struct e1_device_site site;
    uint32_t patch_address;
    pid_t owner = -1;
    int patch_installed = 0;
    int marker_written = 0;
    int result = 1;

    build_device_spec(hook_address, device_patch, &spec);
    if (resolve_device_site(target, E1_DEVICE_SITE_PTZ, &site) != 0) {
        fprintf(stderr, "unable to resolve device PTZ hook site\n");
        goto done;
    }
    patch_address = (uint32_t)site.address;
    if ((hook_address & 1U) == 0U ||
        !address_is_executable(target, hook_address & ~UINT32_C(1),
                               DEVICE_LIBRARY)) {
        fprintf(stderr, "hook address is not Thumb code in injected library\n");
        goto done;
    }
    if (inspect_or_modify(target, patch_address, 0, &spec) != 0) {
        fprintf(stderr, "initial device inspection failed\n");
        goto done;
    }
    if (write_state("ptz-supervisor.ready", "device-log-ready\n") != 0) {
        goto done;
    }
    if (wait_for_go("ptz-device.go") != 0) {
        perror("wait for device mutation release");
        goto done;
    }
    owner = fork();
    if (owner < 0) {
        perror("fork owner");
        goto done;
    }
    if (owner == 0) {
        execl(DUMMY_OWNER, DUMMY_OWNER, (char *)NULL);
        _exit(127);
    }
    if (write_state("ptz-owner.pid", "%ld\n", (long)owner) != 0 ||
        write_state("ptz.patch.used", "device-log\n") != 0 ||
        write_state("ptz.patch.active",
                    "version=1 mode=device-log target=%ld address=%#x "
                    "length=8 original=2de9f04f04461546 "
                    "patch=dff800f0%02x%02x%02x%02x\n",
                    (long)target, patch_address, device_patch[4],
                    device_patch[5], device_patch[6], device_patch[7]) != 0) {
        goto done;
    }
    marker_written = 1;
    if (inspect_or_modify(target, patch_address, 1, &spec) != 0) {
        goto done;
    }
    patch_installed = 1;
    if (write_state("ptz-supervisor.active", "device-log\n") != 0) {
        goto done;
    }
    (void)wait_for_owner(owner);
    owner = -1;
    if (inspect_or_modify(target, patch_address, -1, &spec) != 0) {
        fprintf(stderr, "device restore failed\n");
        goto done;
    }
    patch_installed = 0;
    if (write_state("ptz.patch.restored", "bytes-verified\n") != 0 ||
        wait_for_marker("ptz.hook.quiesced", 3000U) != 0) {
        perror("wait for hook quiescence");
        goto done;
    }
    if (unlink(PATCH_MARKER) != 0 && errno != ENOENT) {
        goto done;
    }
    marker_written = 0;
    (void)unlink(STATE_DIR "/ptz-supervisor.active");
    if (write_state("ptz.restored", "device-bytes-and-hook-quiesced\n") != 0) {
        goto done;
    }
    result = 0;

done:
    if (owner > 0) {
        (void)kill(owner, SIGKILL);
        while (waitpid(owner, NULL, 0) < 0 && errno == EINTR) {
        }
    }
    if (patch_installed &&
        inspect_or_modify(target, patch_address, -1, &spec) == 0) {
        patch_installed = 0;
        (void)write_state("ptz.patch.restored", "bytes-verified-after-error\n");
        if (wait_for_marker("ptz.hook.quiesced", 3000U) == 0) {
            (void)unlink(PATCH_MARKER);
            marker_written = 0;
            (void)unlink(STATE_DIR "/ptz-supervisor.active");
            (void)write_state("ptz.restored",
                              "device-restored-after-error\n");
        }
    }
    if (result != 0) {
        (void)write_state("ptz.recovery.failed",
                          patch_installed ? "device-patch-still-installed\n" :
                                            "device-supervisor-error\n");
    }
    if (!patch_installed && marker_written &&
        path_exists("ptz.hook.quiesced")) {
        (void)unlink(PATCH_MARKER);
    }
    (void)unlink(STATE_DIR "/ptz-supervisor.ready");
    printf("supervisor-exit build_id=%s mode=device-log result=%d\n",
           BUILD_ID, result);
    return result;
}

static int supervise_device_audio_log(pid_t target, uint32_t hook_address)
{
    unsigned char device_patch[8];
    struct patch_spec spec;
    struct e1_device_site site;
    uint32_t patch_address;
    pid_t owner = -1;
    int patch_installed = 0;
    int marker_written = 0;
    int result = 1;

    build_device_audio_spec(hook_address, device_patch, &spec);
    if (resolve_device_site(target, E1_DEVICE_SITE_AUDIO, &site) != 0) {
        fprintf(stderr, "unable to resolve device audio hook site\n");
        goto done;
    }
    patch_address = (uint32_t)site.address;
    if ((hook_address & 1U) == 0U ||
        !address_is_executable(target, hook_address & ~UINT32_C(1),
                               DEVICE_LIBRARY)) {
        fprintf(stderr,
                "audio hook address is not Thumb code in injected library\n");
        goto done;
    }
    if (inspect_or_modify(target, patch_address, 0, &spec) != 0) {
        fprintf(stderr, "initial device audio inspection failed\n");
        goto done;
    }
    if (write_state("audio-supervisor.ready", "device-audio-ready\n") != 0) {
        goto done;
    }
    if (wait_for_go("audio-device.go") != 0) {
        perror("wait for device audio mutation release");
        goto done;
    }
    owner = fork();
    if (owner < 0) {
        perror("fork audio owner");
        goto done;
    }
    if (owner == 0) {
        execl(DUMMY_OWNER, DUMMY_OWNER, (char *)NULL);
        _exit(127);
    }
    if (write_state("audio-owner.pid", "%ld\n", (long)owner) != 0 ||
        write_state("audio.patch.used", "device-audio\n") != 0 ||
        write_state("audio.patch.active",
                    "version=1 mode=device-audio target=%ld address=%#x "
                    "length=8 original=10b500f5cc20002a "
                    "patch=dff800f0%02x%02x%02x%02x\n",
                    (long)target, patch_address, device_patch[4],
                    device_patch[5], device_patch[6], device_patch[7]) != 0) {
        goto done;
    }
    marker_written = 1;
    if (inspect_or_modify(target, patch_address, 1, &spec) != 0) {
        goto done;
    }
    patch_installed = 1;
    if (write_state("audio-supervisor.active", "device-audio\n") != 0) {
        goto done;
    }
    (void)wait_for_owner(owner);
    owner = -1;
    if (inspect_or_modify(target, patch_address, -1, &spec) != 0) {
        fprintf(stderr, "device audio restore failed\n");
        goto done;
    }
    patch_installed = 0;
    if (write_state("audio.patch.restored", "bytes-verified\n") != 0 ||
        wait_for_marker("audio.hook.quiesced", 3000U) != 0) {
        perror("wait for audio hook quiescence");
        goto done;
    }
    if (unlink(AUDIO_PATCH_MARKER) != 0 && errno != ENOENT) {
        goto done;
    }
    marker_written = 0;
    (void)unlink(STATE_DIR "/audio-supervisor.active");
    if (write_state("audio.restored",
                    "device-bytes-and-hook-quiesced\n") != 0) {
        goto done;
    }
    result = 0;

done:
    if (owner > 0) {
        (void)kill(owner, SIGKILL);
        while (waitpid(owner, NULL, 0) < 0 && errno == EINTR) {
        }
    }
    if (patch_installed &&
        inspect_or_modify(target, patch_address, -1, &spec) == 0) {
        patch_installed = 0;
        (void)write_state("audio.patch.restored",
                          "bytes-verified-after-error\n");
        if (wait_for_marker("audio.hook.quiesced", 3000U) == 0) {
            (void)unlink(AUDIO_PATCH_MARKER);
            marker_written = 0;
            (void)unlink(STATE_DIR "/audio-supervisor.active");
            (void)write_state("audio.restored",
                              "device-restored-after-error\n");
        }
    }
    if (result != 0) {
        (void)write_state("audio.recovery.failed",
                          patch_installed ?
                              "device-audio-patch-still-installed\n" :
                              "device-audio-supervisor-error\n");
    }
    if (!patch_installed && marker_written &&
        path_exists("audio.hook.quiesced")) {
        (void)unlink(AUDIO_PATCH_MARKER);
    }
    (void)unlink(STATE_DIR "/audio-supervisor.ready");
    printf("supervisor-exit build_id=%s mode=device-audio result=%d\n",
           BUILD_ID, result);
    return result;
}

static int supervise_device_doom(pid_t target, uint32_t hook_address,
                                 pid_t owner)
{
    unsigned char device_patch[8];
    struct patch_spec spec;
    struct e1_device_site site;
    uint32_t patch_address;
    int patch_installed = 0;
    int marker_written = 0;
    int result = 1;

    build_device_spec(hook_address, device_patch, &spec);
    if (resolve_device_site(target, E1_DEVICE_SITE_PTZ, &site) != 0) {
        fprintf(stderr, "unable to resolve device PTZ hook site\n");
        goto done;
    }
    patch_address = (uint32_t)site.address;
    if ((hook_address & 1U) == 0U ||
        !address_is_executable(target, hook_address & ~UINT32_C(1),
                               DEVICE_LIBRARY)) {
        fprintf(stderr, "hook address is not Thumb code in injected library\n");
        goto done;
    }
    if (verify_doom_owner(owner, 1) != 0) {
        goto done;
    }
    if (inspect_or_modify(target, patch_address, 0, &spec) != 0) {
        fprintf(stderr, "initial device inspection failed\n");
        goto done;
    }
    if (write_state("ptz-supervisor.ready", "device-doom-ready\n") != 0) {
        goto done;
    }
    if (wait_for_go("ptz-device.go") != 0) {
        perror("wait for device mutation release");
        goto done;
    }
    if (verify_doom_owner(owner, 1) != 0) {
        goto done;
    }
    if (write_state("ptz-owner.pid", "%ld\n", (long)owner) != 0 ||
        write_state("ptz-owner.kind", "doom\n") != 0 ||
        write_state("ptz.patch.used", "device-doom\n") != 0 ||
        write_state("ptz.patch.active",
                    "version=1 mode=device-doom target=%ld address=%#x "
                    "length=8 original=2de9f04f04461546 "
                    "patch=dff800f0%02x%02x%02x%02x owner=%ld\n",
                    (long)target, patch_address, device_patch[4],
                    device_patch[5], device_patch[6], device_patch[7],
                    (long)owner) != 0) {
        goto done;
    }
    marker_written = 1;
    if (inspect_or_modify(target, patch_address, 1, &spec) != 0) {
        goto done;
    }
    patch_installed = 1;
    if (write_state("ptz-supervisor.active", "device-doom\n") != 0) {
        goto done;
    }
    if (wait_for_doom_owner(owner) != 0) {
        perror("wait for Doom owner");
        goto done;
    }
    if (inspect_or_modify(target, patch_address, -1, &spec) != 0) {
        fprintf(stderr, "device restore failed\n");
        goto done;
    }
    patch_installed = 0;
    if (write_state("ptz.patch.restored", "bytes-verified\n") != 0 ||
        wait_for_marker("ptz.hook.quiesced", 3000U) != 0) {
        perror("wait for hook quiescence");
        goto done;
    }
    if (unlink(PATCH_MARKER) != 0 && errno != ENOENT) {
        goto done;
    }
    marker_written = 0;
    (void)unlink(STATE_DIR "/ptz-supervisor.active");
    if (write_state("ptz.restored", "device-bytes-and-hook-quiesced\n") != 0) {
        goto done;
    }
    result = 0;

done:
    if (result != 0 && verify_doom_owner(owner, 0) == 0) {
        stop_requested = 1;
        (void)wait_for_doom_owner(owner);
    }
    if (patch_installed &&
        inspect_or_modify(target, patch_address, -1, &spec) == 0) {
        patch_installed = 0;
        (void)write_state("ptz.patch.restored", "bytes-verified-after-error\n");
        if (wait_for_marker("ptz.hook.quiesced", 3000U) == 0) {
            (void)unlink(PATCH_MARKER);
            marker_written = 0;
            (void)unlink(STATE_DIR "/ptz-supervisor.active");
            (void)write_state("ptz.restored",
                              "device-restored-after-error\n");
        }
    }
    if (result != 0) {
        (void)write_state("ptz.recovery.failed",
                          patch_installed ? "device-patch-still-installed\n" :
                                            "device-supervisor-error\n");
    }
    if (!patch_installed && marker_written &&
        path_exists("ptz.hook.quiesced")) {
        (void)unlink(PATCH_MARKER);
    }
    (void)unlink(STATE_DIR "/ptz-supervisor.ready");
    printf("supervisor-exit build_id=%s mode=device-doom result=%d\n",
           BUILD_ID, result);
    return result;
}

static int emergency_restore_device(pid_t target, uint32_t hook_address)
{
    unsigned char device_patch[8];
    struct patch_spec spec;
    struct e1_device_site site;
    uint32_t patch_address;

    build_device_spec(hook_address, device_patch, &spec);
    if (resolve_device_site(target, E1_DEVICE_SITE_PTZ, &site) != 0) {
        return 1;
    }
    patch_address = (uint32_t)site.address;
    if ((hook_address & 1U) == 0U ||
        !address_is_executable(target, hook_address & ~UINT32_C(1),
                               DEVICE_LIBRARY)) {
        fprintf(stderr, "restore hook address is not in injected library\n");
        return 1;
    }
    if (inspect_or_modify(target, patch_address, -1, &spec) != 0) {
        return 1;
    }
    if (write_state("ptz.patch.restored", "emergency-bytes-verified\n") != 0 ||
        wait_for_marker("ptz.hook.quiesced", 3000U) != 0) {
        return 1;
    }
    if (unlink(PATCH_MARKER) != 0 && errno != ENOENT) {
        return 1;
    }
    (void)unlink(STATE_DIR "/ptz-supervisor.active");
    if (write_state("ptz.restored", "emergency-device-restore\n") != 0) {
        return 1;
    }
    printf("emergency-restore target=device status=verified\n");
    return 0;
}

static int emergency_restore_device_audio(pid_t target,
                                          uint32_t hook_address)
{
    unsigned char device_patch[8];
    struct patch_spec spec;
    struct e1_device_site site;
    uint32_t patch_address;

    build_device_audio_spec(hook_address, device_patch, &spec);
    if (resolve_device_site(target, E1_DEVICE_SITE_AUDIO, &site) != 0) {
        return 1;
    }
    patch_address = (uint32_t)site.address;
    if ((hook_address & 1U) == 0U ||
        !address_is_executable(target, hook_address & ~UINT32_C(1),
                               DEVICE_LIBRARY)) {
        fprintf(stderr, "restore audio hook address is not in injected library\n");
        return 1;
    }
    if (inspect_or_modify(target, patch_address, -1, &spec) != 0) {
        return 1;
    }
    if (write_state("audio.patch.restored", "emergency-bytes-verified\n") != 0 ||
        wait_for_marker("audio.hook.quiesced", 3000U) != 0) {
        return 1;
    }
    if (unlink(AUDIO_PATCH_MARKER) != 0 && errno != ENOENT) {
        return 1;
    }
    (void)unlink(STATE_DIR "/audio-supervisor.active");
    if (write_state("audio.restored", "emergency-device-audio-restore\n") !=
        0) {
        return 1;
    }
    printf("emergency-restore target=device-audio status=verified\n");
    return 0;
}

int main(int argc, char **argv)
{
    pid_t target;
    pid_t owner;
    uint32_t address;
    struct sigaction action;

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGHUP, &action, NULL);
    (void)sigaction(SIGUSR1, &action, NULL);

    if (argc == 4 && strcmp(argv[1], "--dummy-gate") == 0 &&
        parse_pid(argv[2], &target) == 0 &&
        parse_u32(argv[3], &address) == 0) {
        return supervise_dummy(target, address);
    }
    if (argc == 4 && strcmp(argv[1], "--inspect-dummy") == 0 &&
        parse_pid(argv[2], &target) == 0 &&
        parse_u32(argv[3], &address) == 0) {
        const struct patch_spec spec = {
            "dummy", DUMMY_TARGET, DUMMY_TARGET, original_bytes, patched_bytes
        };
        return inspect_or_modify(target, address, 0, &spec) == 0 ? 0 : 1;
    }
    if (argc == 4 && strcmp(argv[1], "--device-log-gate") == 0 &&
        parse_pid(argv[2], &target) == 0 &&
        parse_u32(argv[3], &address) == 0) {
        return supervise_device_log(target, address);
    }
    if (argc == 4 && strcmp(argv[1], "--device-audio-log-gate") == 0 &&
        parse_pid(argv[2], &target) == 0 &&
        parse_u32(argv[3], &address) == 0) {
        return supervise_device_audio_log(target, address);
    }
    if (argc == 5 && strcmp(argv[1], "--device-doom-owner") == 0 &&
        parse_pid(argv[2], &target) == 0 &&
        parse_u32(argv[3], &address) == 0 &&
        parse_pid(argv[4], &owner) == 0) {
        return supervise_device_doom(target, address, owner);
    }
    if (argc == 3 && strcmp(argv[1], "--inspect-device") == 0 &&
        parse_pid(argv[2], &target) == 0) {
        unsigned char unused_patch[8] = {0};
        struct e1_device_site site;
        const struct patch_spec spec = {
            "device", DEVICE_TARGET, DEVICE_TARGET,
            device_original_bytes, unused_patch
        };
        if (resolve_device_site(target, E1_DEVICE_SITE_PTZ, &site) != 0) {
            return 1;
        }
        return inspect_or_modify(target, (uint32_t)site.address, 0, &spec) == 0 ?
            0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "--inspect-device-audio") == 0 &&
        parse_pid(argv[2], &target) == 0) {
        unsigned char unused_patch[8] = {0};
        struct e1_device_site site;
        const struct patch_spec spec = {
            "device-audio", DEVICE_TARGET, DEVICE_TARGET,
            device_audio_original_bytes, unused_patch
        };
        if (resolve_device_site(target, E1_DEVICE_SITE_AUDIO, &site) != 0) {
            return 1;
        }
        return inspect_or_modify(target, (uint32_t)site.address, 0,
                                 &spec) == 0 ? 0 : 1;
    }
    if (argc == 4 && strcmp(argv[1], "--verify-device-patch") == 0 &&
        parse_pid(argv[2], &target) == 0 &&
        parse_u32(argv[3], &address) == 0) {
        unsigned char device_patch[8];
        struct patch_spec spec;
        struct e1_device_site site;
        build_device_spec(address, device_patch, &spec);
        if ((address & 1U) == 0U ||
            !address_is_executable(target, address & ~UINT32_C(1),
                                   DEVICE_LIBRARY)) {
            return 1;
        }
        if (resolve_device_site(target, E1_DEVICE_SITE_PTZ, &site) != 0) {
            return 1;
        }
        return inspect_or_modify(target, (uint32_t)site.address, 2, &spec) == 0 ?
            0 : 1;
    }
    if (argc == 4 &&
        strcmp(argv[1], "--verify-device-audio-patch") == 0 &&
        parse_pid(argv[2], &target) == 0 &&
        parse_u32(argv[3], &address) == 0) {
        unsigned char device_patch[8];
        struct patch_spec spec;
        struct e1_device_site site;
        build_device_audio_spec(address, device_patch, &spec);
        if ((address & 1U) == 0U ||
            !address_is_executable(target, address & ~UINT32_C(1),
                                   DEVICE_LIBRARY)) {
            return 1;
        }
        if (resolve_device_site(target, E1_DEVICE_SITE_AUDIO, &site) != 0) {
            return 1;
        }
        return inspect_or_modify(target, (uint32_t)site.address, 2,
                                 &spec) == 0 ? 0 : 1;
    }
    if (argc == 4 && strcmp(argv[1], "--restore-device") == 0 &&
        parse_pid(argv[2], &target) == 0 &&
        parse_u32(argv[3], &address) == 0) {
        return emergency_restore_device(target, address);
    }
    if (argc == 4 && strcmp(argv[1], "--restore-device-audio") == 0 &&
        parse_pid(argv[2], &target) == 0 &&
        parse_u32(argv[3], &address) == 0) {
        return emergency_restore_device_audio(target, address);
    }
    fprintf(stderr,
            "usage: %s --dummy-gate PID ADDRESS | "
            "--inspect-dummy PID ADDRESS | --device-log-gate PID HOOK | "
            "--device-audio-log-gate PID HOOK | "
            "--device-doom-owner PID HOOK DOOM_PID | "
            "--inspect-device PID | --verify-device-patch PID HOOK | "
            "--restore-device PID HOOK | --inspect-device-audio PID | "
            "--verify-device-audio-patch PID HOOK | "
            "--restore-device-audio PID HOOK\n",
            argv[0]);
    return 2;
}
