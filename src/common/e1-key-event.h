#ifndef E1_KEY_EVENT_H
#define E1_KEY_EVENT_H

#include <stdint.h>

#define E1_KEY_EVENT_MAGIC UINT32_C(0x45314b59)
#define E1_KEY_EVENT_VERSION UINT32_C(1)

enum e1_key_event_action {
    E1_KEY_EVENT_UP = 0,
    E1_KEY_EVENT_DOWN = 1,
};

struct e1_key_event {
    uint32_t magic;
    uint32_t version;
    int32_t key;
    int32_t action;
};

#endif
