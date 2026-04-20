/*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
    SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "../inc/agm_server_proxy.h"

#include <agm/agm_api.h>

#define LOG_TAG "AGM_SERVER_PROXY"

#include <agm/utils.h>
#ifndef AGM_MEMLOG_UNSUPPORTED
#include <agm/agm_memlogger.h>
#endif

#ifdef DYNAMIC_LOG_ENABLED
#include <log_xml_parser.h>
#define LOG_MASK AGM_MOD_FILE_AGM_SRC
#include <log_utils.h>
#endif

#ifndef __unused
#define __unused __attribute__((unused))
#endif



/* Thread-local current client scoid set by server handlers before dispatch */
static __thread int g_current_scoid = -1;
static __thread uint32_t g_current_txid = 0;

void agm_msg_parser_set_current_scoid(int scoid)
{
    g_current_scoid = scoid;
}

void agm_msg_parser_set_current_txid(uint32_t txid)
{
    g_current_txid = txid;
}

/* Token-based session handle table; track owning client scoid for cleanup */
typedef struct {
    bool in_use;
    uint32_t token;
    uint64_t hndl;
    int scoid; /* owning client's scoid */
} agm_handle_slot_t;

typedef struct {
    uint32_t session_id;
    agm_event_cb cb;                // Original callback
    void* client_data;              // Original client data
    mqd_t mq_des;                   // Client's message queue
    char queue_name[32];            // Queue name
} agm_server_cb_data_t;

static agm_server_cb_data_t g_server_cb_data[AGM_PROXY_MAX_SESSIONS];

static agm_handle_slot_t g_handle_table[AGM_PROXY_MAX_SESSIONS];
static uint32_t g_next_token = 1;
static pthread_mutex_t g_handle_lock = PTHREAD_MUTEX_INITIALIZER;


void init_handle_table(void) {
    for (int i = 0; i < AGM_PROXY_MAX_SESSIONS; i++) {
        g_handle_table[i].in_use = false;
        g_handle_table[i].token = 0;
        g_handle_table[i].hndl = 0;
        g_handle_table[i].scoid = -1;
    }
    g_next_token = 1;
}

static uint32_t agm_proxy_alloc_token(uint64_t hndl)
{
    uint32_t token = 0;
    pthread_mutex_lock(&g_handle_lock);

    int slot = -1;
    for (size_t i = 0; i < AGM_PROXY_MAX_SESSIONS; ++i) {
        if (!g_handle_table[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        AGM_LOGE("No free slots in handle table");
        pthread_mutex_unlock(&g_handle_lock);
        return 0;
    }

    do {
        token = g_next_token++;
        if (token == 0) token = g_next_token++;
    } while (token == 0);

    g_handle_table[slot].in_use = true;
    g_handle_table[slot].token = token;
    g_handle_table[slot].hndl = hndl;
    g_handle_table[slot].scoid = g_current_scoid;

    pthread_mutex_unlock(&g_handle_lock);
    return token;
}

static uint64_t agm_proxy_lookup_hndl(uint32_t token)
{
    if (token == 0) return 0;

    uint64_t h = 0;
    pthread_mutex_lock(&g_handle_lock);
    for (size_t i = 0; i < AGM_PROXY_MAX_SESSIONS; ++i) {
        if (g_handle_table[i].in_use && g_handle_table[i].token == token) {
            h = g_handle_table[i].hndl;
            break;
        }
    }

    pthread_mutex_unlock(&g_handle_lock);
    return h;
}

void agm_server_cb_data_init(void) {
    for (int i = 0; i < AGM_PROXY_MAX_SESSIONS; i++) {
        g_server_cb_data[i].mq_des = (mqd_t)-1;
        g_server_cb_data[i].session_id = 0;
        memset(g_server_cb_data[i].queue_name, 0, sizeof(g_server_cb_data[i].queue_name));
    }
}


/* Cleanup all sessions for a disconnecting client (called from close_dup) */
void agm_msg_parser_cleanup_scoid(int scoid)
{
    pthread_mutex_lock(&g_handle_lock);

    uint32_t session_ids[AGM_PROXY_MAX_SESSIONS];
    size_t session_count = 0;

    for (size_t i = 0; i < AGM_PROXY_MAX_SESSIONS; ++i) {
        if (g_handle_table[i].in_use && g_handle_table[i].scoid == scoid) {
            uint64_t h = g_handle_table[i].hndl;
            session_ids[session_count++] = g_handle_table[i].token;
            (void)agm_session_close(h);
            g_handle_table[i].in_use = false;
        }
    }

    for (size_t i = 0; i < AGM_PROXY_MAX_SESSIONS; ++i) {
        if (g_server_cb_data[i].mq_des != (mqd_t)-1) {
            bool should_cleanup = false;
            for (size_t j = 0; j < session_count; ++j) {
                if (g_server_cb_data[i].session_id == session_ids[j]) {
                    should_cleanup = true;
                    break;
                }
            }

            if (should_cleanup) {
                mq_close(g_server_cb_data[i].mq_des);
                mq_unlink(g_server_cb_data[i].queue_name);
                memset(&g_server_cb_data[i], 0, sizeof(g_server_cb_data[i]));
                g_server_cb_data[i].mq_des = (mqd_t)-1;
            }
        }
    }

    for (size_t i = 0; i < AGM_PROXY_MAX_SESSIONS; ++i) {
        if (!g_handle_table[i].in_use && g_handle_table[i].scoid == scoid) {
            memset(&g_handle_table[i], 0, sizeof(g_handle_table[i]));
        }
    }

    pthread_mutex_unlock(&g_handle_lock);

    AGM_LOGI("Cleaned up %zu sessions for scoid=%d", session_count, scoid);
}

/* Validator exposed to server_proxy */
int agm_msg_parser_validate(const agm_ipc_request_hdr_t* hdr, size_t msg_len)
{
    if (!hdr) return -EFAULT;
    if (msg_len < sizeof(*hdr)) return -EMSGSIZE;
    if (hdr->version != AGM_IPC_PROTO_VER) return -EPROTONOSUPPORT;
    if (hdr->payload_len > AGM_PROXY_MAX_PAYLOAD) return -EMSGSIZE;
    if (sizeof(*hdr) + (size_t)hdr->payload_len != msg_len) return -EINVAL;
    return 0;
}

typedef int (*agm_dispatch_fn)(const uint8_t* in, uint32_t in_len,
                               uint8_t* out, uint32_t* out_len);

typedef struct {
    uint32_t opcode;
    const char* name;
    agm_dispatch_fn fn;
} agm_dispatch_entry_t;

/* requested APIs */
static int agm_server_agm_init(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_agm_deinit(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_open(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_prepare(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_start(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_read(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_write(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_stop(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_close(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_pause(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_resume(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_register_cb(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);

//metadata media/buff configs start
static int agm_server_get_aif_info_list(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_set_metadata(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_aif_set_metadata(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_aif_set_metadata(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_set_aif_media_config(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_aif_connect(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_session_set_config(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);
static int agm_server_get_aif_info_list1(const uint8_t* in, uint32_t in_len,uint8_t* out, uint32_t* out_len);
static int agm_server_get_group_aif_info_list(const uint8_t* in, uint32_t in_len,uint8_t* out, uint32_t* out_len);
static int agm_server_aif_group_set_media_config(const uint8_t* in, uint32_t in_len,uint8_t* out, uint32_t* out_len);
static int agm_server_set_params_to_acdb_tunnel(const uint8_t* in, uint32_t in_len,uint8_t* out, uint32_t* out_len);
static int agm_server_get_params_to_acdb_tunnel(const uint8_t* in, uint32_t in_len,uint8_t* out, uint32_t* out_len);
static int agm_server_session_aif_set_cal(const uint8_t* in, uint32_t in_len,uint8_t* out, uint32_t* out_len);
static int agm_server_aif_set_params(const uint8_t* in, uint32_t in_len,uint8_t* out, uint32_t* out_len);
static int agm_server_session_get_buf_info(const uint8_t* in, uint32_t in_len,uint8_t* out, uint32_t* out_len);
static int agm_server_session_set_loopback(const uint8_t* in, uint32_t in_len,uint8_t* out __unused, uint32_t* out_len);
static int agm_server_session_set_ec_ref(const uint8_t* in, uint32_t in_len,uint8_t* out __unused, uint32_t* out_len);
static int agm_server_get_buffer_timestamp(const uint8_t* in, uint32_t in_len,uint8_t* out, uint32_t* out_len);
//metadata media/buff configs end

static int agm_server_session_eos(const uint8_t* in, uint32_t in_len, uint8_t* out __unused, uint32_t* out_len);
static int agm_server_get_hw_processed_buff_cnt(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len);

/* Dispatch table for the requested APIs */
static const agm_dispatch_entry_t g_dispatch_tbl[] = {
    { AGM_OP_INIT,                        "AGM_OP_INIT",                     agm_server_agm_init },
    { AGM_OP_DEINIT,                      "AGM_DEINIT",                      agm_server_agm_deinit },
    { AGM_OP_SESSION_OPEN,                "SESSION_OPEN",                    agm_server_session_open },
    { AGM_OP_SESSION_PREPARE,             "SESSION_PREPARE",                 agm_server_session_prepare },
    { AGM_OP_SESSION_START,               "SESSION_START",                   agm_server_session_start },
    { AGM_OP_SESSION_READ,                "SESSION_READ",                    agm_server_session_read },
    { AGM_OP_SESSION_WRITE,               "SESSION_WRITE",                   agm_server_session_write },
    { AGM_OP_SESSION_STOP,                "SESSION_STOP",                    agm_server_session_stop },
    { AGM_OP_SESSION_CLOSE,               "SESSION_CLOSE",                   agm_server_session_close },
    { AGM_OP_SESSION_PAUSE,               "SESSION_PAUSE",                   agm_server_session_pause },
    { AGM_OP_SESSION_RESUME,              "SESSION_RESUME",                  agm_server_session_resume },
    { AGM_OP_GET_AIF_INFO_LIST,           "SESSION_GET_AGM_AIF_INFO",        agm_server_get_aif_info_list },
    { AGM_OP_STREAM_METADATA,             "SESSION_STREAM_METADATA",         agm_server_session_set_metadata },
    { AGM_OP_DEVICE_METADATA,             "SESSION_DEVICE_METADATA",         agm_server_aif_set_metadata },
    { AGM_OP_DEVICE_PP_METADATA,          "SESSION_DEVICE_PP_METADATA",      agm_server_session_aif_set_metadata },
    { AGM_OP_SET_MEDIA_CONFIG,            "SESSION_SET_MEDIA_CONFIG",        agm_server_set_aif_media_config },
    { AGM_OP_SESSION_AIF_CONNECT,         "SESSION_SESSION_AIF_CONNECT",     agm_server_session_aif_connect },
    { AGM_OP_SET_SESSION_CONFIG,          "SESSION_SET_SESSION_CONFIG",      agm_server_session_set_config },
    { AGM_OP_SESSION_REGISTER_CB,         "SESSION_REGISTER_CB",             agm_server_session_register_cb },
    { AGM_OP_SESSION_EOS,                 "SESSION_EOS",                     agm_server_session_eos },
    { AGM_OP_GET_HW_PROCESSED_BUFF_CNT,   "SESSION_GET_HW_PROCESSED_BUFF_CNT",   agm_server_get_hw_processed_buff_cnt },
    { AGM_OP_AGM_GET_AIF_INFO_LIST,       "AGM_GET_AIF_INFO_LIST",            agm_server_get_aif_info_list1 },
    { AGM_OP_AGM_GET_GROUP_AIF_INFO_LIST,  "AGM_GET_GROUP_AIF_INFO_LIST",     agm_server_get_group_aif_info_list },
    { AGM_OP_GROUP_SET_MEDIA_CONFIG,       "GROUP_SET_MEDIA_CONFIG",          agm_server_aif_group_set_media_config },
    { AGM_OP_SET_PARAMS_ACDB_TUNNEL,       "SET_PARAMS_ACDB_TUNNEL",          agm_server_set_params_to_acdb_tunnel },
    { AGM_OP_GET_PARAMS_ACDB_TUNNEL,       "GET_PARAMS_ACDB_TUNNEL",          agm_server_get_params_to_acdb_tunnel },
    { AGM_OP_SESSION_AIF_SET_CAL,       "SESSION_AIF_SET_CAL",          agm_server_session_aif_set_cal },
    { AGM_OP_AIF_SET_PARAMS,       "AIF_SET_PARAMS",          agm_server_aif_set_params },
    { AGM_OP_SESSION_GET_BUF_INFO,       "SESSION_GET_BUF_INFO",          agm_server_session_get_buf_info },
    { AGM_OP_SESSION_SET_LOOPBACK,       "SESSION_SET_LOOPBACK",          agm_server_session_set_loopback },
    { AGM_OP_SESSION_SET_EC_REF,       "SESSION_SET_EC_REF",          agm_server_session_set_ec_ref },
    { AGM_OP_GET_BUFFER_TIMESTAMP,       "GET_BUFFER_TIMESTAMP",          agm_server_get_buffer_timestamp },
};

static const size_t g_dispatch_tbl_count = sizeof(g_dispatch_tbl) / sizeof(g_dispatch_tbl[0]);

static const agm_dispatch_entry_t* find_dispatch(uint32_t opcode)
{
    for (size_t i = 0; i < g_dispatch_tbl_count; ++i) {
        if (g_dispatch_tbl[i].opcode == opcode) return &g_dispatch_tbl[i];
    }
    return NULL;
}

/* Parser entry point used by agm_server_proxy.c */
int agm_msg_parser_dispatch(uint32_t opcode,
                            const uint8_t* in, uint32_t in_len,
                            uint8_t* out, uint32_t* out_len)
{
    const agm_dispatch_entry_t* ent = find_dispatch(opcode);

    if (!ent || !ent->fn) {
        AGM_LOGE("Unknown opcode: %u", opcode);
        return -ENOSYS;
    }

    return ent->fn(in, in_len, out, out_len);
}

/* Handlers implementations  */
static int agm_server_agm_init(const uint8_t* in, uint32_t in_len, uint8_t* out __unused, uint32_t* out_len)
{
    if (in_len != 0){
        AGM_LOGE("agm_server_agm_init length not 0");
        return -EINVAL;
    }
    int ret = agm_init();
    *out_len = 0;
    if (ret){
        AGM_LOGE("agm_init failed with ret: %d", ret);
        return ret;
    }
    AGM_LOGI("agm_init success with ret: %d", ret);
    return ret;
}

/* Note: out buffer unused; retained for IPC ABI compatibility */
static int agm_server_agm_deinit(const uint8_t* in, uint32_t in_len,
     uint8_t* out __unused, uint32_t* out_len)
{
    if (in_len != 0) return -EINVAL;
    int ret = agm_deinit();
    if (ret){
        AGM_LOGE("agm_deinit failed with ret: %d", ret);
        return ret;
    }
    *out_len = 0;
    return ret;
}

static int agm_server_get_group_aif_info_list(const uint8_t* in, uint32_t in_len,
                                        uint8_t* out, uint32_t* out_len)
{
    AGM_LOGD("%s enter 1",__func__);
    if (in_len != sizeof(uint32_t)) return -EINVAL;

    uint32_t capacity = 0;
    memcpy(&capacity, in, sizeof(uint32_t));

    size_t count = 0;
    int ret = agm_get_group_aif_info_list(NULL, &count);
    if (ret) return ret;

    if (*out_len < sizeof(uint32_t)) return -ENOSPC;

    uint32_t count_u32 = (uint32_t)count;
    memcpy(out, &count_u32, sizeof(uint32_t));

    if (capacity == 0) {
        *out_len = sizeof(uint32_t);
        return 0;
    }

    if ((size_t)capacity < count) {
        *out_len = sizeof(uint32_t);
        return -ENOSPC;
    }

    if (count > (SIZE_MAX - sizeof(uint32_t)) / sizeof(struct aif_info)) {
        *out_len = sizeof(uint32_t);
        return -EOVERFLOW;
    }
    size_t bytes_needed = sizeof(uint32_t) + count * sizeof(struct aif_info);
    if (*out_len < bytes_needed) {
        *out_len = sizeof(uint32_t);
        return -ENOSPC;
    }

    struct aif_info *tmp = (struct aif_info *)calloc(count, sizeof(struct aif_info));
    if (!tmp) {
        *out_len = sizeof(uint32_t);
        return -ENOMEM;
    }

    size_t tmp_count = count;
    ret = agm_get_group_aif_info_list(tmp, &tmp_count);
    AGM_LOGD("%s aftr infolist %d count %zu",__func__,ret,tmp_count);
    if (ret) {
        free(tmp);
        *out_len = sizeof(uint32_t);
        return ret;
    }

    memcpy(out + sizeof(uint32_t), tmp, tmp_count * sizeof(struct aif_info));
    *out_len = sizeof(uint32_t) + (uint32_t)(tmp_count * sizeof(struct aif_info));

    for (size_t i = 0; i < tmp_count; i++) {
    AGM_LOGE("%s:aif_info: [%zu] name='%s', dir=%d\n", __func__,
             i, tmp[i].aif_name, tmp[i].dir);
    }
    free(tmp);
    return 0;
}

static int agm_server_get_aif_info_list1(const uint8_t* in, uint32_t in_len,
                                        uint8_t* out, uint32_t* out_len)
{
    AGM_LOGE("agm_server_get_aif_info_list1 enter");
    if (in_len < 4) return -EINVAL;
    int ret =0;
    size_t aif_count=0;
    memcpy(&aif_count, in, 4);

    AGM_LOGE("agm_server_get_aif_info_list1 enter %d",aif_count);

    if (aif_count == 0){
        ret = agm_get_aif_info_list(NULL, &aif_count);
        if(ret)
            return ret;

        memcpy(out,&aif_count,4);
        *out_len = 4;
        AGM_LOGE("agm_server_get_aif_info_list1 after %d",aif_count);
        return ret;
    }

    struct aif_info* aif_list = calloc(aif_count, sizeof(struct aif_info));
    if (!aif_list) return -ENOMEM;

    ret = agm_get_aif_info_list(aif_list, &aif_count);
    AGM_LOGE("agm_server_get_aif_info_list1 2nd enter %d",aif_count);
    AGM_LOGE("%s: Got %zu backends from dev:ret %d \n", __func__, aif_count,ret);

    if (ret)
        goto done;

    memcpy(out, &aif_count, 4);
    memcpy(out+4, aif_list, aif_count * (sizeof(struct aif_info)));
    *out_len = 4 + (aif_count * (sizeof(struct aif_info)));
    for (int i = 0; i < aif_count; i++) {
        AGM_LOGE("%s: [%d] name='%s', dir=%d\n", __func__,
                 i, aif_list[i].aif_name, aif_list[i].dir);
    }
    done:
        free(aif_list);
        return ret;
}

static int agm_server_set_params_to_acdb_tunnel(const uint8_t* in, uint32_t in_len,
                                        uint8_t* out, uint32_t* out_len)
{
    AGM_LOGD("agm_server_set_params_to_acdb_tunnel enter");
    if (in_len < 4) return -EINVAL;

    uint32_t payload_size_ipc;
    memcpy(&payload_size_ipc,in,4);

    if(payload_size_ipc == 0) return -EINVAL;
    if(in_len < 4 + payload_size_ipc) return -EINVAL;  // Validate total length

    size_t payload_size = (size_t) payload_size_ipc;
    const void * payload = in+4;
    int ret = agm_set_params_to_acdb_tunnel(payload,payload_size);

    if(ret) return ret;
    *out_len =0;
    return 0;
}

static int agm_server_get_params_to_acdb_tunnel(const uint8_t* in, uint32_t in_len,
                                        uint8_t* out, uint32_t* out_len)
{
    AGM_LOGD("agm_server_get_params_to_acdb_tunnel enter");
    if (in_len < 4) return -EINVAL;

    uint32_t payload_size_ipc;
    memcpy(&payload_size_ipc,in,4);

    if(payload_size_ipc == 0) return -EINVAL;

    size_t payload_size = (size_t) payload_size_ipc;
    const void * payload = in+4;

    return 0;
}

static int agm_server_session_aif_set_cal(const uint8_t* in, uint32_t in_len,
                                        uint8_t* out, uint32_t* out_len)
{
    AGM_LOGD("agm_server_session_aif_set_cal enter");
    if (in_len < 12) return -EINVAL;

    uint32_t sess_id, aid_id, cal_size;
    memcpy(&sess_id,in,4);
    memcpy(&aid_id,in+4,4);
    memcpy(&cal_size,in+8,4);

    if(cal_size != sizeof(struct agm_cal_config)) return -EINVAL;
    if(in_len < 12 + cal_size) return -EINVAL;

    const struct agm_cal_config *cal_config = (struct agm_cal_config *)(in+12);
    int ret = agm_session_aif_set_cal(sess_id,aid_id,cal_config);

    if(ret) return ret;
    *out_len =0;
    return 0;
}

static int agm_server_aif_set_params(const uint8_t* in, uint32_t in_len,
                                        uint8_t* out, uint32_t* out_len)
{
    AGM_LOGD("agm_server_aif_set_params enter");
    if (in_len < 4) return -EINVAL;

    uint32_t payload_size_ipc,aid_id;
    memcpy(&aid_id,in,4);
    memcpy(&payload_size_ipc,in+4,4);

    if(payload_size_ipc == 0) return -EINVAL;

    size_t payload_size = (size_t) payload_size_ipc;
    const void * payload = in+8;
    int ret = agm_aif_set_params(aid_id,payload,payload_size);

    if(ret) return ret;
    *out_len =0;
    return 0;

}

static int agm_server_session_get_params(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len)
{
    AGM_LOGD("agm_server_aif_set_params enter");
    if (in_len < 4) return -EINVAL;

    uint32_t payload_size_ipc,aid_id;
    memcpy(&aid_id,in,4);
    memcpy(&payload_size_ipc,in+4,4);

    if(payload_size_ipc == 0) return -EINVAL;

    size_t payload_size = (size_t) payload_size_ipc;
    void * payload = malloc(payload_size);
    if (!payload) return -ENOMEM;

    int ret = agm_session_get_params(aid_id,payload,payload_size);

    if(ret) {
        free(payload);
        return ret;
    }

    memcpy(out,&payload_size,4);
    memcpy(out+4,payload,payload_size);
    free(payload);
    *out_len = 4 + payload_size;
    return 0;
}

static int agm_server_session_get_buf_info(const uint8_t* in, uint32_t in_len,
                                           uint8_t* out, uint32_t* out_len)
{
    AGM_LOGD("%s enter", __func__);

    /* Validate input: session_id + flag */
    if (in_len != sizeof(uint32_t) * 2) {
        AGM_LOGE("%s: Invalid input length: %u (expected %zu)\n",
                 __func__, in_len, sizeof(uint32_t) * 2);
        return -EINVAL;
    }

    uint32_t session_id, flag;
    memcpy(&session_id, in, sizeof(uint32_t));
    memcpy(&flag, in + sizeof(uint32_t), sizeof(uint32_t));

    AGM_LOGD("%s: session_id=%u, flag=%u\n", __func__, session_id, flag);

    /* Validate output buffer size */
    if (*out_len < sizeof(struct agm_buf_info)) {
        AGM_LOGE("%s: Output buffer too small: %u (need %zu)\n",
                 __func__, *out_len, sizeof(struct agm_buf_info));
        return -ENOSPC;
    }

    /* Allocate temporary buffer for buf_info */
    struct agm_buf_info *tmp_buf_info = (struct agm_buf_info *)calloc(1, sizeof(struct agm_buf_info));
    if (!tmp_buf_info) {
        AGM_LOGE("%s: Failed to allocate memory\n", __func__);
        return -ENOMEM;
    }

    /* Call the actual AGM API */
    int ret = agm_session_get_buf_info(session_id, tmp_buf_info, flag);
    if (ret) {
        AGM_LOGE("%s: agm_session_get_buf_info failed: %d\n", __func__, ret);
        free(tmp_buf_info);
        return ret;
    }

    /* Copy buf_info to output buffer */
    memcpy(out, tmp_buf_info, sizeof(struct agm_buf_info));
    *out_len = sizeof(struct agm_buf_info);

    AGM_LOGD("%s: Successfully retrieved buf_info\n", __func__);

    free(tmp_buf_info);
    return 0;
}

static int agm_server_session_set_loopback(const uint8_t* in, uint32_t in_len,
                                           uint8_t* out __unused, uint32_t* out_len)
{
    AGM_LOGD("%s enter", __func__);

    /* Validate input: capture_session_id + playback_session_id + state */
    if (in_len != sizeof(uint32_t) * 3) {
        AGM_LOGE("%s: Invalid input length: %u (expected %zu)\n",
                 __func__, in_len, sizeof(uint32_t) * 3);
        return -EINVAL;
    }

    uint32_t capture_session_id, playback_session_id, state_u32;
    memcpy(&capture_session_id, in, sizeof(uint32_t));
    memcpy(&playback_session_id, in + sizeof(uint32_t), sizeof(uint32_t));
    memcpy(&state_u32, in + sizeof(uint32_t) * 2, sizeof(uint32_t));

    bool state = (state_u32 != 0);

    AGM_LOGD("%s: capture_session_id=%u, playback_session_id=%u, state=%d\n",
             __func__, capture_session_id, playback_session_id, state);

    /* Call the actual AGM API */
    int ret = agm_session_set_loopback(capture_session_id, playback_session_id, state);
    if (ret) {
        AGM_LOGE("%s: agm_session_set_loopback failed: %d\n", __func__, ret);
        return ret;
    }

    AGM_LOGD("%s: Successfully set loopback\n", __func__);

    *out_len = 0;
    return 0;
}

static int agm_server_session_set_ec_ref(const uint8_t* in, uint32_t in_len,
                                         uint8_t* out __unused, uint32_t* out_len)
{
    AGM_LOGD("%s enter", __func__);

    /* Validate input: capture_session_id + aif_id + state */
    if (in_len != sizeof(uint32_t) * 3) {
        AGM_LOGE("%s: Invalid input length: %u (expected %zu)\n",
                 __func__, in_len, sizeof(uint32_t) * 3);
        return -EINVAL;
    }

    uint32_t capture_session_id, aif_id, state_u32;
    memcpy(&capture_session_id, in, sizeof(uint32_t));
    memcpy(&aif_id, in + sizeof(uint32_t), sizeof(uint32_t));
    memcpy(&state_u32, in + sizeof(uint32_t) * 2, sizeof(uint32_t));

    bool state = (state_u32 != 0);

    AGM_LOGD("%s: capture_session_id=%u, aif_id=%u, state=%d\n",
             __func__, capture_session_id, aif_id, state);

    /* Call the actual AGM API */
    int ret = agm_session_set_ec_ref(capture_session_id, aif_id, state);
    if (ret) {
        AGM_LOGE("%s: agm_session_set_ec_ref failed: %d\n", __func__, ret);
        return ret;
    }

    AGM_LOGD("%s: Successfully set EC reference\n", __func__);

    *out_len = 0;
    return 0;
}

static int agm_server_get_aif_info_list(const uint8_t* in, uint32_t in_len,
                                        uint8_t* out, uint32_t* out_len)
{
    if (in_len != sizeof(uint32_t)) return -EINVAL;

    uint32_t capacity = 0;
    memcpy(&capacity, in, sizeof(uint32_t));

    size_t count = 0;
    int ret = agm_get_aif_info_list(NULL, &count);
    if (ret) return ret;

    if (*out_len < sizeof(uint32_t)) return -ENOSPC;

    uint32_t count_u32 = (uint32_t)count;
    memcpy(out, &count_u32, sizeof(uint32_t));

    if (capacity == 0) {
        *out_len = sizeof(uint32_t);
        return 0;
    }

    if ((size_t)capacity < count) {
        *out_len = sizeof(uint32_t);
        return -ENOSPC;
    }

    size_t bytes_needed = sizeof(uint32_t) + count * sizeof(struct aif_info);
    if (*out_len < bytes_needed) {
        *out_len = sizeof(uint32_t);
        return -ENOSPC;
    }

    struct aif_info *tmp = (struct aif_info *)calloc(count, sizeof(struct aif_info));
    if (!tmp) {
        *out_len = sizeof(uint32_t);
        return -ENOMEM;
    }

    size_t tmp_count = count;
    ret = agm_get_aif_info_list(tmp, &tmp_count);
    if (ret) {
        free(tmp);
        *out_len = sizeof(uint32_t);
        return ret;
    }

    memcpy(out + sizeof(uint32_t), tmp, tmp_count * sizeof(struct aif_info));
    *out_len = sizeof(uint32_t) + (uint32_t)(tmp_count * sizeof(struct aif_info));
    free(tmp);
    return 0;
}

static int agm_server_get_buffer_timestamp(const uint8_t* in, uint32_t in_len,
                                           uint8_t* out, uint32_t* out_len)
{
    AGM_LOGD("%s enter", __func__);

    /* Validate input: session_id */
    if (in_len != sizeof(uint32_t)) {
        AGM_LOGE("%s: Invalid input length: %u (expected %zu)\n",
                 __func__, in_len, sizeof(uint32_t));
        return -EINVAL;
    }

    uint32_t session_id;
    memcpy(&session_id, in, sizeof(uint32_t));

    AGM_LOGD("%s: session_id=%u\n", __func__, session_id);

    /* Validate output buffer size */
    if (*out_len < sizeof(uint64_t)) {
        AGM_LOGE("%s: Output buffer too small: %u (need %zu)\n",
                 __func__, *out_len, sizeof(uint64_t));
        return -ENOSPC;
    }

    uint64_t timestamp = 0;

    /* Call the actual AGM API */
    int ret = agm_get_buffer_timestamp(session_id, &timestamp);
    if (ret) {
        AGM_LOGE("%s: agm_get_buffer_timestamp failed: %d\n", __func__, ret);
        return ret;
    }

    /* Copy timestamp to output buffer */
    memcpy(out, &timestamp, sizeof(uint64_t));
    *out_len = sizeof(uint64_t);

    AGM_LOGD("%s: Successfully retrieved timestamp=%llu\n", __func__,
             (unsigned long long)timestamp);
    return 0;
}

//stream metadata
static int agm_server_session_set_metadata(const uint8_t* in, uint32_t in_len,
                                           uint8_t* out, uint32_t* out_len)
{
    if (in_len < 8) return -EINVAL;

    uint32_t session_id, stream_size;
    memcpy(&session_id,  in,     4);
    memcpy(&stream_size, in + 4, 4);

    if (8 + stream_size > in_len) return -EINVAL;

    int ret = agm_session_set_metadata(session_id, stream_size, (uint8_t*)(in + 8));
    if (ret) {
        AGM_LOGE("failed: agm_session_set_metadata, ret=%d", ret);
        return ret;
    }
    return 0;
}

//device TX/RX  metadata
static int agm_server_aif_set_metadata(const uint8_t* in, uint32_t in_len,
                                       uint8_t* out, uint32_t* out_len)
{
    if (in_len < 8) return -EINVAL;

    uint32_t aif_id, device_size;
    memcpy(&aif_id,      in,     4);
    memcpy(&device_size, in + 4, 4);

    if (8 + device_size > in_len) return -EINVAL;

    int ret = agm_aif_set_metadata(aif_id, device_size, (uint8_t*)(in + 8));
    if (ret) {
        AGM_LOGE("failed: agm_aif_set_metadata, ret=%d", ret);
        return ret;
    }
    return 0;
}

//device-pp  metadata
static int agm_server_session_aif_set_metadata(const uint8_t* in, uint32_t in_len,
                                               uint8_t* out, uint32_t* out_len)
{

    if (in_len < 12) return -EINVAL;

    uint32_t session_id, aif_id, devicepp_size;
    memcpy(&session_id, in, 4);
    memcpy(&aif_id, in + 4, 4);
    memcpy(&devicepp_size, in + 8, 4);

    if (12 + devicepp_size > in_len) return -EINVAL;

    int ret = agm_session_aif_set_metadata(session_id, aif_id, devicepp_size, (uint8_t*)(in + 12));

    if (ret) {
        AGM_LOGE("failed: agm_session_aif_set_metadata (devicepp), ret=%d", ret);
        return ret;
    }

    return 0;
}


static int agm_server_set_aif_media_config(const uint8_t* in, uint32_t in_len,
                                           uint8_t* out, uint32_t* out_len)
{
    if (in_len < 4 + sizeof(struct agm_media_config)) return -EINVAL;

    uint32_t aif_id;
    memcpy(&aif_id, in, 4);

    const struct agm_media_config* cfg = (const struct agm_media_config*)(in + 4);

    int ret = agm_aif_set_media_config(aif_id, cfg);
    if (ret) {
        AGM_LOGE("agm_aif_set_media_config failed: %d", ret);
        return ret;
    }
    return 0;
}

static int agm_server_aif_group_set_media_config(const uint8_t* in, uint32_t in_len,
                                           uint8_t* out, uint32_t* out_len)
{
    AGM_LOGD("agm_server_aif_group_set_media_config enter");
    if (in_len < 4 + sizeof(struct agm_group_media_config)) return -EINVAL;

    uint32_t aif_group_id;
    memcpy(&aif_group_id, in, 4);

    const struct agm_group_media_config* cfg = (const struct agm_group_media_config*)(in + 4);

    int ret = agm_aif_group_set_media_config(aif_group_id, cfg);
    if (ret) {
        AGM_LOGE("agm_server_aif_group_set_media_config failed: %d", ret);
        return ret;
    }
    return 0;
}

static int agm_server_session_aif_connect(const uint8_t* in, uint32_t in_len,
                                          uint8_t* out, uint32_t* out_len)
{
    if (in_len < 12) return -EINVAL;

    uint32_t session_id, aif_id, connect_u32;
    memcpy(&session_id, in, 4);
    memcpy(&aif_id, in + 4, 4);
    memcpy(&connect_u32, in + 8, 4);

    bool connect = (connect_u32 != 0);

    int ret = agm_session_aif_connect(session_id, aif_id, connect);
    if (ret) {
        AGM_LOGE("agm_session_aif_connect failed: %d", ret);
        return ret;
    }
    return 0;
}

static int agm_server_session_set_config(const uint8_t* in, uint32_t in_len,
                                         uint8_t* out __unused, uint32_t* out_len __unused)
{
    const size_t need = sizeof(uint32_t)
                      + sizeof(struct agm_session_config)
                      + sizeof(struct agm_media_config)
                      + sizeof(struct agm_buffer_config);
    if (in_len < need) return -EINVAL;

    size_t off = 0;
    AGM_LOGI("server sizes: sc=%zu mc=%zu bc=%zu",
             sizeof(struct agm_session_config),
             sizeof(struct agm_media_config),
             sizeof(struct agm_buffer_config));

    uint32_t token = 0;
    memcpy(&token, in + off, sizeof(token)); off += sizeof(token);

    const struct agm_session_config* ses_cfg =
        (const struct agm_session_config*)(in + off);
    off += sizeof(struct agm_session_config);

    const struct agm_media_config* media_cfg =
        (const struct agm_media_config*)(in + off);
    off += sizeof(struct agm_media_config);

    const struct agm_buffer_config* buf_cfg =
        (const struct agm_buffer_config*)(in + off);

    AGM_LOGI("Decoded ses_cfg: dir=%u mode=%u start=%u stop=%u flags=0x%x",
             ses_cfg->dir, ses_cfg->sess_mode,
             ses_cfg->start_threshold, ses_cfg->stop_threshold, ses_cfg->sess_flags);
    AGM_LOGI("Decoded media_cfg: rate=%u ch=%u format=%u data_format=%u",
             media_cfg->rate, media_cfg->channels,
             media_cfg->format, media_cfg->data_format);
    AGM_LOGI("Decoded buf_cfg: count=%u size=%u", buf_cfg->count, buf_cfg->size);

    uint64_t hndl = agm_proxy_lookup_hndl(token);
    if (!hndl) return -EINVAL;

    int ret = agm_session_set_config(hndl, ses_cfg, media_cfg, buf_cfg);
    if (ret) {
        AGM_LOGE("agm_session_set_config failed: %d", ret);
        return ret;
    }
    return 0;
}

/*SESSION OPEN*/
static int agm_server_session_open(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len)
{

    if (in_len < 8) return -EINVAL;

    uint32_t session_id, session_mode;
    memcpy(&session_id,   in,     4);
    memcpy(&session_mode, in + 4, 4);

    uint64_t hndl = 0ull;
    int ret = agm_session_open(session_id, (enum agm_session_mode)session_mode, &hndl);
    if (ret){
        AGM_LOGE("agm_session_open failed: %d", ret);
        return ret;
    }

    uint32_t token = agm_proxy_alloc_token(hndl);
    if (*out_len < 4) return -ENOSPC;

    memcpy(out, &token, 4);
    *out_len = 4;
    return 0;
}


/* SESSION_PREPARE */
static int agm_server_session_prepare(const uint8_t* in, uint32_t in_len,
               uint8_t* out __unused, uint32_t* out_len)
{

    if (in_len != sizeof(uint32_t)){
        AGM_LOGE("invalid in_len\n ");
        return -EINVAL;
    }
    uint32_t token; memcpy(&token, in, sizeof(uint32_t));
    AGM_LOGE("Preparing session with token=%u", token);

    uint64_t h = agm_proxy_lookup_hndl(token);
    if (!h) {
        AGM_LOGE("Failed to lookup handle for token=%u, handle: 0x%08x", token, h);
        return -EINVAL;
    }

    int ret = agm_session_prepare(h);
    if (ret) {
        AGM_LOGE("failed! agm_session_prepare ret=%d", ret);
        return ret;
    }

    *out_len = 0;
    AGM_LOGE("agm_session_prepare success with ret: %d", ret);
    return ret;

}

/* SESSION_START */
static int agm_server_session_start(const uint8_t* in, uint32_t in_len, uint8_t* out __unused, uint32_t* out_len)
{
    if (in_len != sizeof(uint32_t)) return -EINVAL;

    uint32_t token; memcpy(&token, in, sizeof(uint32_t));
    uint64_t h = agm_proxy_lookup_hndl(token);
    if (!h) {
        AGM_LOGE("Failed to lookup handle for token=%u, handle: 0x%08x", token, h);
        return -EINVAL;
    }
    int ret = agm_session_start(h);
    if (ret) {
        AGM_LOGE("failed! agm_session_start ret=%d", ret);
        return ret;
    }
    *out_len = 0;
    return ret;
}

/* SESSION_READ
 * Input: { uint32_t token; uint32_t size; }
 * Output: { uint32_t captured; uint8_t data[captured]; }
 */
static int agm_server_session_read(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len)
{
    if (in_len != 8) {
        AGM_LOGE("ERROR: Invalid input length: %u (expected 8)", in_len);
        return -EINVAL;
    }

    uint32_t token, size;
    memcpy(&token, in, 4);
    memcpy(&size, in + 4, 4);
    uint32_t required = 4 + size;

    AGM_LOGI("Request: token=%u, size=%u", token, size);

    uint64_t h = agm_proxy_lookup_hndl(token);
    if (!h) {
        AGM_LOGE("ERROR: Invalid token %u", token);
        return -EINVAL;
    }

    if (*out_len < required) {
        AGM_LOGE("ERROR: Output buffer too small: have=%u, need=%u", *out_len, required);
        return -EMSGSIZE;
    }

    void* audio_buf = out + 4;
    size_t cnt = size;

    AGM_LOGI("agm_session_read call: handle=0x%llx, buffer=%p, size=%zu",
             h, audio_buf, cnt);

    int ret = agm_session_read(h, audio_buf, &cnt);

    AGM_LOGI("agm_session_read returned: ret=%d, cnt=%zu", ret, cnt);
    if (ret == 0) {
        uint32_t captured = (uint32_t)cnt;
        memcpy(out, &captured, 4);
        *out_len = 4 + captured;
        AGM_LOGI("SUCCESS: captured=%u bytes, out_len=%u", captured, *out_len);
    } else {
        AGM_LOGE("ERROR: agm_session_read failed: %d", ret);
        *out_len = 0;
    }

    AGM_LOGI("=== agm_server_session_read EXIT: ret=%d ===", ret);
    return ret;
}

/* SESSION_WRITE */
static int agm_server_session_write(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len)
{

    if (in_len < 8) return -EINVAL;

    uint32_t token, size;
    memcpy(&token, in, 4);
    memcpy(&size, in + 4, 4);

    if (in_len != 8 + size) return -EINVAL;

    uint64_t h = agm_proxy_lookup_hndl(token);
    if (!h) return -EINVAL;

    size_t cnt = size;

    int ret = agm_session_write(h, (void*)(in + 8), &cnt);


    if (ret == 0) {
        if (*out_len < 4) return -EMSGSIZE;
        uint32_t consumed = (uint32_t)cnt;
        memcpy(out, &consumed, 4);
        *out_len = 4;
    } else {
        AGM_LOGE("agm_server_session_write failed: ret=%d", ret);
        *out_len = 0;
    }
    return ret;

}

/* SESSION_STOP */
static int agm_server_session_stop(const uint8_t* in, uint32_t in_len, uint8_t* out __unused, uint32_t* out_len)
{
    if (in_len != sizeof(uint32_t)) return -EINVAL;
    uint32_t token; memcpy(&token, in, sizeof(uint32_t));
    uint64_t h = agm_proxy_lookup_hndl(token);
    if (!h) return -EINVAL;
    int ret = agm_session_stop(h);

    if (ret) {
        AGM_LOGE("failed! agm_session_stop ret=%d", ret);
        return ret;
    }

    *out_len = 0; return ret;
}

/* SESSION_CLOSE */
static int agm_server_session_close(const uint8_t* in, uint32_t in_len, uint8_t* out __unused, uint32_t* out_len)
{
    if (in_len != sizeof(uint32_t)) return -EINVAL;
    uint32_t token; memcpy(&token, in, sizeof(uint32_t));
    uint64_t h = agm_proxy_lookup_hndl(token);
    if (!h) return -EINVAL;
    int ret = agm_session_close(h);

    if (ret) {
        AGM_LOGE("failed! agm_session_close ret=%d", ret);
        return ret;
    } else
        AGM_LOGD("success! agm_session_close ret=%d", ret);

    *out_len = 0; return ret;
}

/* SESSION_PAUSE */
static int agm_server_session_pause(const uint8_t* in, uint32_t in_len, uint8_t* out __unused, uint32_t* out_len)
{
    if (in_len != sizeof(uint32_t)) return -EINVAL;
    uint32_t token; memcpy(&token, in, sizeof(uint32_t));
    uint64_t h = agm_proxy_lookup_hndl(token);
    if (!h) return -EINVAL;
    int ret = agm_session_pause(h);
    if (ret) {
        AGM_LOGE("failed! agm_session_pause ret=%d", ret);
        return ret;
    }
    *out_len = 0; return ret;
}

/* SESSION_RESUME */
static int agm_server_session_resume(const uint8_t* in, uint32_t in_len, uint8_t* out __unused, uint32_t* out_len)
{
    if (in_len != sizeof(uint32_t)) return -EINVAL;
    uint32_t token; memcpy(&token, in, sizeof(uint32_t));
    uint64_t h = agm_proxy_lookup_hndl(token);
    if (!h) return -EINVAL;
    int ret = agm_session_resume(h);
    if (ret) {
        AGM_LOGE("failed! agm_session_resume ret=%d", ret);
        return ret;
    }
    *out_len = 0; return ret;
}

// forwards events to client
static void agm_server_event_callback(uint32_t session_id,
                                    struct agm_event_cb_params *event_params,
                                    void *client_data)
{
    agm_server_cb_data_t *cb_data = (agm_server_cb_data_t*)client_data;

    if (!cb_data || cb_data->mq_des == (mqd_t)-1) {
        AGM_LOGE("Invalid callback data in agm_server_event_callback");
        return;
    }

    uint8_t event_buf[sizeof(struct agm_event_cb_params) + event_params->event_payload_size];
    struct agm_event_cb_params *event_copy = (struct agm_event_cb_params *)event_buf;

    event_copy->source_module_id = event_params->source_module_id;
    event_copy->event_id = event_params->event_id;
    event_copy->event_payload_size = event_params->event_payload_size;

    if (event_params->event_payload_size > 0) {
        memcpy(event_copy->event_payload,
               event_params->event_payload,
               event_params->event_payload_size);
    }

    if (-1 == mq_send(cb_data->mq_des,
                     (const char*)event_buf,
                     sizeof(struct agm_event_cb_params) + event_params->event_payload_size,
                     0)) {
        AGM_LOGE("mq_send failed! errno=[%d, %s]", errno, strerror(errno));
    }
}

// Handler for callback registration
static int agm_server_session_register_cb(const uint8_t* in, uint32_t in_len,
                                          uint8_t* out __unused, uint32_t* out_len)
{
    if (in_len < sizeof(uint32_t)*2 + 32)
        return -EINVAL;

    uint32_t session_id = 0, evt_type = 0;
    char mq_name[32];

    memcpy(&session_id, in, sizeof(uint32_t));
    memcpy(&evt_type,    in + sizeof(uint32_t), sizeof(uint32_t));
    memcpy(mq_name,      in + sizeof(uint32_t)*2, 32);
    mq_name[31] = '\0';

    AGM_LOGE("SESSION_REGISTER_CB: sid=%u evt_type=%u mq=%s", session_id, evt_type, mq_name);

    uint32_t slot = AGM_PROXY_MAX_SESSIONS;
    for (uint32_t i = 0; i < AGM_PROXY_MAX_SESSIONS; i++) {
        if (g_server_cb_data[i].mq_des == (mqd_t)-1) { slot = i; break; }
    }
    if (slot == AGM_PROXY_MAX_SESSIONS) return -ENOSPC;

    mqd_t mq_des = mq_open(mq_name, O_WRONLY | O_NONBLOCK);
    if (mq_des == (mqd_t)-1) {
        AGM_LOGE("mq_open(%s) failed: %s", mq_name, strerror(errno));
        return -errno;
    }

    g_server_cb_data[slot].session_id = session_id;
    g_server_cb_data[slot].mq_des     = mq_des;
    memcpy(g_server_cb_data[slot].queue_name,
        mq_name, sizeof(g_server_cb_data[slot].queue_name) - 1);
    g_server_cb_data[slot].queue_name[
        sizeof(g_server_cb_data[slot].queue_name) - 1] = '\0';

    int ret = agm_session_register_cb(session_id,
                                      agm_server_event_callback,
                                      (enum event_type)evt_type,
                                      &g_server_cb_data[slot]);
    if (ret != 0) {
        AGM_LOGE("agm_session_register_cb(core) failed: %d", ret);
        mq_close(mq_des);
        memset(&g_server_cb_data[slot], 0, sizeof(g_server_cb_data[slot]));
        g_server_cb_data[slot].mq_des = (mqd_t)-1;
        return ret;
    } else
        AGM_LOGE("agm_session_register_cb(core) success: %d", ret);

    *out_len = 0;
    return 0;
}


/* SESSION_EOS */
static int agm_server_session_eos(const uint8_t* in, uint32_t in_len, uint8_t* out __unused, uint32_t* out_len)
{
    if (in_len != sizeof(uint32_t)) return -EINVAL;
    uint32_t token; memcpy(&token, in, sizeof(uint32_t));
    uint64_t h = agm_proxy_lookup_hndl(token);
    if (!h) return -EINVAL;
    int ret = agm_session_eos(h);

    if (ret) {
        AGM_LOGE("failed! agm_session_eos ret=%d", ret);
        return ret;
    }

    *out_len = 0; return ret;
}

static int agm_server_get_hw_processed_buff_cnt(const uint8_t* in, uint32_t in_len,
                                             uint8_t* out, uint32_t* out_len)
{
    if (in_len != sizeof(uint32_t) * 2) return -EINVAL;

    uint32_t token;
    enum direction dir;

    memcpy(&token, in, sizeof(uint32_t));
    memcpy(&dir, in + sizeof(uint32_t), sizeof(enum direction));

    uint64_t h = agm_proxy_lookup_hndl(token);
    if (!h) {
        AGM_LOGE("Failed to lookup handle for token=%u", token);
        return -EINVAL;
    }

    size_t buff_cnt = agm_get_hw_processed_buff_cnt(h, dir);

    // response
    if (*out_len < sizeof(size_t)) return -EMSGSIZE;

    memcpy(out, &buff_cnt, sizeof(size_t));
    *out_len = sizeof(size_t);

    AGM_LOGI("HW processed buffer count: %zu", buff_cnt);
    return 0;
}
