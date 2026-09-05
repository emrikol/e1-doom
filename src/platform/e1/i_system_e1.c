#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "d_ticcmd.h"
#include "i_printf.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"

typedef struct exit_entry_s
{
    atexit_func_t func;
    boolean run_on_error;
    const char *name;
    struct exit_entry_s *next;
} exit_entry_t;

static exit_entry_t *exit_funcs[exit_priority_max];
static exit_priority_t exit_priority;
static char errmsg[2048];
static int exit_code;

ticcmd_t *I_BaseTiccmd(void)
{
    static ticcmd_t emptycmd;
    return &emptycmd;
}

void I_ErrorOrSuccess(int err_code, const char *error, ...)
{
    va_list ap;
    size_t used = strlen(errmsg);
    va_start(ap, error);
    M_vsnprintf(errmsg + used, sizeof(errmsg) - used - 1, error, ap);
    va_end(ap);
    I_Printf(err_code ? VB_ERROR : VB_ALWAYS, "%s\n", errmsg + used);
    if (!exit_code && err_code)
    {
        exit_code = err_code;
    }
    I_SafeExit(exit_code);
}

void I_ErrorMsg(void)
{
}

void I_AtExitPrio(atexit_func_t func, boolean run_on_error, const char *name,
                  exit_priority_t priority)
{
    exit_entry_t *entry = malloc(sizeof(*entry));
    if (!entry)
    {
        I_Error("unable to allocate exit handler");
    }
    entry->func = func;
    entry->run_on_error = run_on_error;
    entry->name = name;
    entry->next = exit_funcs[priority];
    exit_funcs[priority] = entry;
}

void I_SafeExit(int rc)
{
    for (; exit_priority < exit_priority_max; ++exit_priority)
    {
        exit_entry_t *entry;
        while ((entry = exit_funcs[exit_priority]))
        {
            exit_funcs[exit_priority] = entry->next;
            if (!rc || entry->run_on_error)
            {
                I_Printf(VB_DEBUG, "Exit Sequence[%d]: %s (%d)",
                         exit_priority, entry->name, rc);
                entry->func();
            }
            free(entry);
        }
    }
    exit(rc);
}

void *I_Realloc(void *ptr, size_t size)
{
    void *result = realloc(ptr, size);
    if (size && !result)
    {
        I_Error("I_Realloc: failed on reallocation");
    }
    return result;
}

boolean I_GetMemoryValue(unsigned int offset, void *value, int size)
{
    static const unsigned char dos_mem[10] =
        {0x57, 0x92, 0x19, 0x00, 0xf4, 0x06, 0x70, 0x00, 0x16, 0x00};
    if (offset + (unsigned int)size > sizeof(dos_mem))
    {
        return false;
    }
    memcpy(value, dos_mem + offset, (size_t)size);
    return true;
}

const char *I_GetPlatform(void)
{
    return "Reolink E1 Zoom / Linux ARMv7";
}
