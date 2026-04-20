/*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
    SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#ifndef AGM_CLIENT_PROXY_H
#define AGM_CLIENT_PROXY_H

#include <stdint.h>
#include <mqueue.h>
#include <agm/agm_api.h>
#include "agm_server_proxy.h"

#define AGM_CLIENT_MAX_SESSIONS 128
#define AGM_HANDLE_INDEX_BITS_MASK 0x000003F
#define AGM_CLIENT_CB_THREAD_SYNC_TIMEOUT 10

struct agm_event_cb_params;  //agm-core
enum event_type;             //even data(r/w done), module (event raised by)

typedef void (*agm_event_cb)(uint32_t session_id,
                           struct agm_event_cb_params *event_params,
                           void *client_data);

// client-side context structure
typedef struct {
    int coid;                  // Connection ID to AGM server
    uint32_t init_count;       // Init reference count
} AGM_DEVICE_TYPE;

int agm_send_ipc_msg(AGM_DEVICE_TYPE *dev,
                     uint16_t msgType,
                     uint32_t opcode,
                     const uint8_t *in_buf, uint32_t in_len,
                     uint8_t *out_buf, uint32_t *out_len);

#endif // AGM_CLIENT_PROXY_H