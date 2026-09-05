#ifndef E1_PTZ_EVENT_H
#define E1_PTZ_EVENT_H

#include <stdint.h>

#define E1_PTZ_EVENT_MAGIC UINT32_C(0x4550545a)
#define E1_PTZ_EVENT_VERSION UINT32_C(1)
#define E1_PTZ_SOCKET_PATH "/mnt/tmp/e1-doom/state/ptz.sock"

struct e1_ptz_event {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    int32_t channel;
    int32_t command;
    int32_t speed;
    uint64_t monotonic_ms;
};

#endif
