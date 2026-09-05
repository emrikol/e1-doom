#define _GNU_SOURCE

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <asm/ptrace.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BUILD_ID "e1-doom-injector-v3"
#define DEVICE_PATH "/mnt/app/device"
#define STATE_DIR "/mnt/tmp/e1-doom/state"
#define PATCH_MARKER STATE_DIR "/injector.patch.active"

/* Retained only for recovery of a marker written by the retired v1 injector. */
#define DEVICE_DLCLOSE_PLT UINT32_C(0x0001ba20)
#define DEVICE_DLSYM_PLT UINT32_C(0x0001bd1c)
#define DEVICE_PLT_FIRST_WORD UINT32_C(0xe28fc603)
#define ARM_BREAKPOINT_WORD UINT32_C(0xe1200070)
#define ARM_RETURN_SENTINEL UINT32_C(0)
#define ARM_CPSR_T UINT32_C(0x20)
#define SCRATCH_SIZE 0x3000U
#define CALL_STACK_GAP 0x1000U
#define PATH_GAP 0x200U
#define STOP_TIMEOUT_MS 2000U
#define REMOTE_TIMEOUT_MS 5000U

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

struct loader_got {
    uint32_t dlopen_slot;
    uint32_t dlclose_slot;
    uint32_t dlsym_slot;
};

static int range_valid(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

static int resolve_loader_got(struct loader_got *loader)
{
    struct stat metadata;
    const unsigned char *image = MAP_FAILED;
    const Elf32_Ehdr *header;
    const Elf32_Shdr *sections;
    int descriptor = -1;
    int dlopen_matches = 0;
    int dlclose_matches = 0;
    int dlsym_matches = 0;
    int result = -1;
    size_t section_bytes;

    memset(loader, 0, sizeof(*loader));
    descriptor = open(DEVICE_PATH, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
        metadata.st_size < (off_t)sizeof(Elf32_Ehdr)) {
        goto done;
    }
    image = mmap(NULL, (size_t)metadata.st_size, PROT_READ, MAP_PRIVATE,
                 descriptor, 0);
    if (image == MAP_FAILED) {
        goto done;
    }
    header = (const Elf32_Ehdr *)(const void *)image;
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
        header->e_ident[EI_CLASS] != ELFCLASS32 ||
        header->e_ident[EI_DATA] != ELFDATA2LSB || header->e_machine != EM_ARM ||
        header->e_type != ET_EXEC || header->e_shentsize != sizeof(Elf32_Shdr) ||
        header->e_shnum == 0U) {
        goto done;
    }
    section_bytes = (size_t)header->e_shnum * sizeof(Elf32_Shdr);
    if (!range_valid(header->e_shoff, section_bytes, (size_t)metadata.st_size)) {
        goto done;
    }
    sections = (const Elf32_Shdr *)(const void *)(image + header->e_shoff);
    for (size_t section_index = 0; section_index < header->e_shnum;
         ++section_index) {
        const Elf32_Shdr *relocation_section = &sections[section_index];
        const Elf32_Shdr *symbol_section;
        const Elf32_Shdr *string_section;
        const Elf32_Rel *relocations;
        const Elf32_Sym *symbols;
        const char *strings;
        size_t relocation_count;
        size_t symbol_count;

        if (relocation_section->sh_type != SHT_REL ||
            relocation_section->sh_entsize != sizeof(Elf32_Rel) ||
            relocation_section->sh_link >= header->e_shnum ||
            !range_valid(relocation_section->sh_offset,
                         relocation_section->sh_size,
                         (size_t)metadata.st_size)) {
            continue;
        }
        symbol_section = &sections[relocation_section->sh_link];
        if (symbol_section->sh_type != SHT_DYNSYM ||
            symbol_section->sh_entsize != sizeof(Elf32_Sym) ||
            symbol_section->sh_link >= header->e_shnum ||
            !range_valid(symbol_section->sh_offset, symbol_section->sh_size,
                         (size_t)metadata.st_size)) {
            continue;
        }
        string_section = &sections[symbol_section->sh_link];
        if (string_section->sh_type != SHT_STRTAB ||
            !range_valid(string_section->sh_offset, string_section->sh_size,
                         (size_t)metadata.st_size)) {
            continue;
        }
        relocations = (const Elf32_Rel *)(const void *)(
            image + relocation_section->sh_offset);
        symbols = (const Elf32_Sym *)(const void *)(image + symbol_section->sh_offset);
        strings = (const char *)(const void *)(image + string_section->sh_offset);
        relocation_count = relocation_section->sh_size / sizeof(Elf32_Rel);
        symbol_count = symbol_section->sh_size / sizeof(Elf32_Sym);
        for (size_t index = 0; index < relocation_count; ++index) {
            size_t symbol_index = ELF32_R_SYM(relocations[index].r_info);
            const char *name;

            if (ELF32_R_TYPE(relocations[index].r_info) != R_ARM_JUMP_SLOT ||
                symbol_index >= symbol_count ||
                symbols[symbol_index].st_name >= string_section->sh_size) {
                continue;
            }
            name = strings + symbols[symbol_index].st_name;
            if (memchr(name, '\0', string_section->sh_size -
                                      symbols[symbol_index].st_name) == NULL) {
                continue;
            }
            if (strcmp(name, "dlopen") == 0) {
                loader->dlopen_slot = relocations[index].r_offset;
                ++dlopen_matches;
            } else if (strcmp(name, "dlclose") == 0) {
                loader->dlclose_slot = relocations[index].r_offset;
                ++dlclose_matches;
            } else if (strcmp(name, "dlsym") == 0) {
                loader->dlsym_slot = relocations[index].r_offset;
                ++dlsym_matches;
            }
        }
    }
    if (dlopen_matches == 1 && dlclose_matches == 1 && dlsym_matches == 1 &&
        loader->dlopen_slot >= UINT32_C(0x10000) &&
        loader->dlclose_slot >= UINT32_C(0x10000) &&
        loader->dlsym_slot >= UINT32_C(0x10000)) {
        result = 0;
    }

done:
    if (image != MAP_FAILED) {
        (void)munmap((void *)(uintptr_t)image, (size_t)metadata.st_size);
    }
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
    return result;
}

static int verify_target(pid_t pid)
{
    char path[64];
    char target[PATH_MAX];
    ssize_t length;

    if (snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid) >=
        (int)sizeof(path)) {
        return -1;
    }
    length = readlink(path, target, sizeof(target) - 1U);
    if (length < 0 || (size_t)length >= sizeof(target)) {
        return -1;
    }
    target[length] = '\0';
    if (strcmp(target, DEVICE_PATH) != 0) {
        fprintf(stderr, "target exe=%s expected=%s\n", target, DEVICE_PATH);
        return -1;
    }
    return 0;
}

static int address_in_mapping(pid_t pid, uint32_t address, char permission,
                              const char *required_path)
{
    char path[64];
    char line[PATH_MAX + 128];
    FILE *maps;
    int result = 0;

    if (snprintf(path, sizeof(path), "/proc/%ld/maps", (long)pid) >=
        (int)sizeof(path)) {
        return 0;
    }
    maps = fopen(path, "r");
    if (maps == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long start;
        unsigned long end;
        char permissions[5] = {0};

        if (sscanf(line, "%lx-%lx %4s", &start, &end, permissions) != 3 ||
            address < start || (uint64_t)address + sizeof(uint32_t) > end ||
            strchr(permissions, permission) == NULL ||
            (required_path != NULL && strstr(line, required_path) == NULL)) {
            continue;
        }
        result = 1;
        break;
    }
    (void)fclose(maps);
    return result;
}

static int wait_stopped(pid_t pid, int *status)
{
    const struct timespec delay = {0, 10000000L};
    unsigned int elapsed_ms = 0;

    while (elapsed_ms < STOP_TIMEOUT_MS) {
        pid_t result = waitpid(pid, status, __WALL | WNOHANG);
        if (result == pid) {
            return WIFSTOPPED(*status) ? 0 : -1;
        }
        if (result == 0) {
            (void)nanosleep(&delay, NULL);
            elapsed_ms += 10U;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    errno = ETIMEDOUT;
    return -1;
}

static int attach_target(pid_t pid)
{
    int status;

    if (ptrace(PTRACE_SEIZE, pid, NULL,
               (void *)(uintptr_t)PTRACE_O_EXITKILL) != 0) {
        perror("PTRACE_SEIZE");
        return -1;
    }
    if (ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) != 0 ||
        wait_stopped(pid, &status) != 0) {
        perror("PTRACE_INTERRUPT/wait");
        (void)ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }
    return 0;
}

static int read_remote(pid_t pid, uint32_t address, void *output,
                       size_t length)
{
    unsigned char *bytes = output;
    size_t offset = 0;

    while (offset < length) {
        long word;
        size_t take = sizeof(word);

        errno = 0;
        word = ptrace(PTRACE_PEEKDATA, pid,
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

static int return_sentinel_is_unmapped(pid_t pid)
{
    long word;

    errno = 0;
    word = ptrace(PTRACE_PEEKDATA, pid,
                  (void *)(uintptr_t)ARM_RETURN_SENTINEL, NULL);
    if (word == -1 && errno != 0) {
        return 1;
    }
    fprintf(stderr, "return sentinel %#x is unexpectedly mapped\n",
            ARM_RETURN_SENTINEL);
    errno = EADDRINUSE;
    return 0;
}

static int write_remote(pid_t pid, uint32_t address, const void *input,
                        size_t length)
{
    const unsigned char *bytes = input;
    size_t offset = 0;

    while (offset < length) {
        long word = 0;
        size_t take = sizeof(word);

        if (take > length - offset) {
            errno = 0;
            word = ptrace(PTRACE_PEEKDATA, pid,
                          (void *)(uintptr_t)(address + (uint32_t)offset),
                          NULL);
            if (word == -1 && errno != 0) {
                return -1;
            }
            take = length - offset;
        }
        memcpy(&word, bytes + offset, take);
        if (ptrace(PTRACE_POKEDATA, pid,
                   (void *)(uintptr_t)(address + (uint32_t)offset),
                   (void *)(uintptr_t)(unsigned long)word) != 0) {
            return -1;
        }
        offset += take;
    }
    return 0;
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

static int wait_for_remote_return(pid_t pid, struct pt_regs *result,
                                  int *target_stopped)
{
    const struct timespec delay = {0, 10000000L};
    uint64_t deadline = monotonic_ms() + REMOTE_TIMEOUT_MS;

    *target_stopped = 0;
    while (monotonic_ms() < deadline) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, __WALL | WNOHANG);

        if (waited == 0) {
            (void)nanosleep(&delay, NULL);
            continue;
        }
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (!WIFSTOPPED(status)) {
            fprintf(stderr, "remote wait status=%#x not-stopped\n", status);
            errno = ECHILD;
            return -1;
        }
        *target_stopped = 1;
        if (ptrace(PTRACE_GETREGS, pid, NULL, result) != 0) {
            return -1;
        }
        if (WSTOPSIG(status) != SIGSEGV) {
            fprintf(stderr, "remote wait signal=%d pc=%#lx expected-signal=%d\n",
                    WSTOPSIG(status), (unsigned long)result->ARM_pc, SIGSEGV);
            errno = EPROTO;
            return -1;
        }
        if ((uint32_t)result->ARM_pc != ARM_RETURN_SENTINEL) {
            fprintf(stderr, "remote return pc=%#lx expected=%#x\n",
                    (unsigned long)result->ARM_pc, ARM_RETURN_SENTINEL);
            errno = EPROTO;
            return -1;
        }
        return 0;
    }
    errno = ETIMEDOUT;
    return -1;
}

static int remote_call(pid_t pid, uint32_t function_source, int source_is_got,
                       uint32_t argument0, uint32_t argument1,
                       const char *remote_string, int remote_string_argument,
                       uint32_t *return_value)
{
    struct pt_regs original_registers;
    struct pt_regs call_registers;
    struct pt_regs result_registers;
    unsigned char *saved_stack = NULL;
    unsigned char *call_stack = NULL;
    uint32_t function_address;
    uint32_t scratch_base;
    uint32_t remote_string_address;
    size_t remote_string_length = 0;
    int attached = 0;
    int registers_saved = 0;
    int stack_dirty = 0;
    int target_stopped = 0;
    int result = -1;

    if (verify_target(pid) != 0 || attach_target(pid) != 0) {
        goto done;
    }
    attached = 1;
    target_stopped = 1;
    if (!return_sentinel_is_unmapped(pid)) {
        goto done;
    }
    function_address = function_source;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &original_registers) != 0) {
        perror("read target registers");
        goto done;
    }
    registers_saved = 1;
    if ((source_is_got &&
         (!address_in_mapping(pid, function_source, 'w', DEVICE_PATH) ||
          read_remote(pid, function_source, &function_address,
                      sizeof(function_address)) != 0))) {
        perror("read target state");
        goto done;
    }
    if (function_address < UINT32_C(0x10000) ||
        !address_in_mapping(pid, function_address & ~UINT32_C(1), 'x', NULL) ||
        (uint32_t)original_registers.ARM_sp <
            SCRATCH_SIZE + UINT32_C(0x10000)) {
        fprintf(stderr, "target state mismatch function=%#x sp=%#lx\n",
                function_address,
                (unsigned long)original_registers.ARM_sp);
        goto done;
    }

    scratch_base = ((uint32_t)original_registers.ARM_sp - SCRATCH_SIZE) &
                   ~UINT32_C(7);
    remote_string_address =
        ((uint32_t)original_registers.ARM_sp - PATH_GAP) & ~UINT32_C(3);
    saved_stack = malloc(SCRATCH_SIZE);
    call_stack = malloc(SCRATCH_SIZE);
    if (saved_stack == NULL || call_stack == NULL ||
        read_remote(pid, scratch_base, saved_stack, SCRATCH_SIZE) != 0) {
        perror("save remote stack");
        goto done;
    }
    memcpy(call_stack, saved_stack, SCRATCH_SIZE);
    if (remote_string != NULL) {
        size_t offset = remote_string_address - scratch_base;
        remote_string_length = strlen(remote_string) + 1U;
        if (remote_string_length > PATH_GAP ||
            offset + remote_string_length > SCRATCH_SIZE) {
            errno = ENAMETOOLONG;
            goto done;
        }
        memcpy(call_stack + offset, remote_string, remote_string_length);
        if (remote_string_argument == 0) {
            argument0 = remote_string_address;
        } else if (remote_string_argument == 1) {
            argument1 = remote_string_address;
        } else {
            errno = EINVAL;
            goto done;
        }
    }

    stack_dirty = 1;
    if (write_remote(pid, scratch_base, call_stack, SCRATCH_SIZE) != 0) {
        perror("write remote stack");
        goto done;
    }

    call_registers = original_registers;
    call_registers.ARM_r0 = argument0;
    call_registers.ARM_r1 = argument1;
    call_registers.ARM_sp =
        ((uint32_t)original_registers.ARM_sp - CALL_STACK_GAP) &
        ~UINT32_C(7);
    call_registers.ARM_lr = ARM_RETURN_SENTINEL;
    call_registers.ARM_pc = function_address & ~UINT32_C(1);
    if ((function_address & 1U) != 0U) {
        call_registers.ARM_cpsr |= ARM_CPSR_T;
    } else {
        call_registers.ARM_cpsr &= ~ARM_CPSR_T;
    }
    if (ptrace(PTRACE_SETREGS, pid, NULL, &call_registers) != 0) {
        perror("set remote call registers");
        goto done;
    }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0) {
        perror("continue remote call");
        goto done;
    }
    target_stopped = 0;
    if (wait_for_remote_return(pid, &result_registers, &target_stopped) != 0) {
        perror("remote call did not return");
        if (!target_stopped &&
            ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) == 0) {
            int ignored_status;
            if (wait_stopped(pid, &ignored_status) == 0) {
                target_stopped = 1;
            }
        }
        goto done;
    }
    *return_value = (uint32_t)result_registers.ARM_r0;
    result = 0;

done:
    if (attached) {
        int cleanup_ok = target_stopped;

        if (stack_dirty &&
            (!target_stopped ||
             write_remote(pid, scratch_base, saved_stack, SCRATCH_SIZE) != 0)) {
            perror("restore remote stack");
            cleanup_ok = 0;
            result = -1;
        }
        if (registers_saved &&
            (!target_stopped ||
             ptrace(PTRACE_SETREGS, pid, NULL, &original_registers) != 0)) {
            perror("restore target registers");
            cleanup_ok = 0;
            result = -1;
        }
        if (!target_stopped || ptrace(PTRACE_DETACH, pid, NULL, NULL) != 0) {
            perror("PTRACE_DETACH");
            cleanup_ok = 0;
            result = -1;
        }
        (void)cleanup_ok;
    }
    free(call_stack);
    free(saved_stack);
    return result;
}

static int probe(pid_t pid, const struct loader_got *loader)
{
    uint32_t dlopen_address;
    uint32_t dlclose_address;
    uint32_t dlsym_address;
    int result = 1;

    if (verify_target(pid) != 0 || attach_target(pid) != 0) {
        return 1;
    }
    if (address_in_mapping(pid, loader->dlopen_slot, 'w', DEVICE_PATH) &&
        address_in_mapping(pid, loader->dlclose_slot, 'w', DEVICE_PATH) &&
        address_in_mapping(pid, loader->dlsym_slot, 'w', DEVICE_PATH) &&
        read_remote(pid, loader->dlopen_slot, &dlopen_address,
                    sizeof(dlopen_address)) == 0 &&
        read_remote(pid, loader->dlclose_slot, &dlclose_address,
                    sizeof(dlclose_address)) == 0 &&
        read_remote(pid, loader->dlsym_slot, &dlsym_address,
                    sizeof(dlsym_address)) == 0 &&
        dlopen_address >= UINT32_C(0x10000) &&
        dlclose_address >= UINT32_C(0x10000) &&
        dlsym_address >= UINT32_C(0x10000) &&
        address_in_mapping(pid, dlopen_address & ~UINT32_C(1), 'x', NULL) &&
        address_in_mapping(pid, dlclose_address & ~UINT32_C(1), 'x', NULL) &&
        address_in_mapping(pid, dlsym_address & ~UINT32_C(1), 'x', NULL) &&
        !address_in_mapping(pid, dlopen_address & ~UINT32_C(1), 'x',
                            DEVICE_PATH) &&
        !address_in_mapping(pid, dlsym_address & ~UINT32_C(1), 'x',
                            DEVICE_PATH)) {
        printf("build_id=%s pid=%ld dlopen_slot=%#x dlclose_slot=%#x "
               "dlsym_slot=%#x dlopen=%#x dlclose=%#x dlsym=%#x\n",
               BUILD_ID, (long)pid, loader->dlopen_slot,
               loader->dlclose_slot, loader->dlsym_slot, dlopen_address,
               dlclose_address, dlsym_address);
        result = 0;
    } else {
        fprintf(stderr, "probe target state mismatch\n");
    }
    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) != 0) {
        result = 1;
    }
    return result;
}

static int restore_word(pid_t pid, uint32_t address, uint32_t original)
{
    uint32_t current;
    int result = 1;

    if (verify_target(pid) != 0 || attach_target(pid) != 0) {
        return 1;
    }
    if (read_remote(pid, address, &current, sizeof(current)) != 0) {
        perror("read restore word");
    } else if (current == original) {
        printf("restore_word status=already-restored address=%#x\n", address);
        result = 0;
    } else if (current == ARM_BREAKPOINT_WORD &&
               write_remote(pid, address, &original, sizeof(original)) == 0) {
        printf("restore_word status=restored address=%#x\n", address);
        result = 0;
    } else {
        fprintf(stderr, "restore_word refused address=%#x current=%#x\n",
                address, current);
    }
    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) != 0) {
        result = 1;
    }
    if (result == 0) {
        (void)unlink(PATCH_MARKER);
    }
    return result;
}

int main(int argc, char **argv)
{
    struct loader_got loader;
    pid_t pid;
    uint32_t value;

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    if (argc == 3 && strcmp(argv[1], "--probe") == 0 &&
        parse_pid(argv[2], &pid) == 0) {
        return resolve_loader_got(&loader) == 0 ? probe(pid, &loader) : 1;
    }
    if (argc == 4 && strcmp(argv[1], "--load") == 0 &&
        parse_pid(argv[2], &pid) == 0 && argv[3][0] == '/') {
        if (resolve_loader_got(&loader) != 0 ||
            remote_call(pid, loader.dlopen_slot, 1, 0, 2, argv[3], 0,
                        &value) != 0 || value == 0U) {
            fprintf(stderr, "load status=failed\n");
            return 1;
        }
        printf("load status=ok handle=%#x\n", value);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "--unload") == 0 &&
        parse_pid(argv[2], &pid) == 0 && parse_u32(argv[3], &value) == 0 &&
        value != 0U) {
        uint32_t dlclose_address = 0;
        uint32_t status = UINT32_MAX;
        if (resolve_loader_got(&loader) != 0 ||
            remote_call(pid, loader.dlsym_slot, 1, 0, 0, "dlclose", 1,
                        &dlclose_address) != 0 ||
            dlclose_address < UINT32_C(0x10000) ||
            !address_in_mapping(pid, dlclose_address & ~UINT32_C(1), 'x',
                                NULL) ||
            address_in_mapping(pid, dlclose_address & ~UINT32_C(1), 'x',
                               DEVICE_PATH) ||
            remote_call(pid, dlclose_address, 0, value, 0, NULL, -1,
                        &status) != 0 || status != 0U) {
            fprintf(stderr, "unload status=failed result=%#x\n", status);
            return 1;
        }
        printf("unload status=ok handle=%#x\n", value);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "--restore-word") == 0 &&
        parse_pid(argv[2], &pid) == 0) {
        uint32_t address;
        uint32_t original;
        if (parse_u32(argv[3], &address) == 0 &&
            parse_u32(argv[4], &original) == 0 &&
            (address == DEVICE_DLCLOSE_PLT || address == DEVICE_DLSYM_PLT) &&
            original == DEVICE_PLT_FIRST_WORD) {
            return restore_word(pid, address, original);
        }
    }

    fprintf(stderr,
            "usage: %s --probe PID | --load PID ABSOLUTE_SO | "
            "--unload PID HANDLE | --restore-word PID ADDRESS WORD\n",
            argv[0]);
    return 2;
}
