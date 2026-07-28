/*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
    SPDX-License-Identifier: BSD-3-Clause-Clear
*/
#define LOG_TAG "AGM_CLIENT_PROXY"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/neutrino.h>
#include <pthread.h>
#include <limits.h>
#include "agm_client_proxy.h"
#include "agm_server_proxy.h"


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

// Client-side handle table to map client handles to server tokens
#define AGM_CLIENT_MAX_SESSIONS 128

#define AGM_CLIENT_HANDLE_MAGIC      0xAA
#define AGM_CLIENT_HANDLE_MAGIC_MASK 0xFF

typedef struct {
    bool in_use;
    uint32_t token;  // Server token
    uint32_t session_id; // For metadata operations
} agm_client_handle_t;

typedef struct {
    uint32_t session_id;
    agm_event_cb cb;
    void* client_data;
    mqd_t mq_des;
    char queue_name[32];
} agm_client_cb_data_t;

static agm_client_cb_data_t g_client_cb_data[AGM_CLIENT_MAX_SESSIONS];
static pthread_t g_cb_thread;
static bool g_cb_thread_running = false;

static agm_client_handle_t g_handle_table[AGM_CLIENT_MAX_SESSIONS];
static pthread_mutex_t g_handle_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cb_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_client_handle = 1;
static uint32_t g_txid_counter = 1;  // for transaction IDs
static void* agm_cb_thread_func(void* arg);
static volatile uint32_t g_active_callback_count = 0;

AGM_DEVICE_TYPE *g_agm_client = NULL;
pthread_mutex_t g_mutex_open_cnt = PTHREAD_MUTEX_INITIALIZER;

static uint32_t agm_client_alloc_handle(uint32_t server_token, uint32_t session_id);
static uint32_t agm_client_get_token(uint64_t client_handle);
static uint32_t agm_client_get_session_id(uint64_t client_handle);
static void agm_client_free_handle(uint64_t client_handle);

static uint32_t agm_client_alloc_handle(uint32_t server_token, uint32_t session_id)
{
    AGM_LOGI("agm_client_alloc_handle: storing token=%u", server_token);
    pthread_mutex_lock(&g_handle_lock);

    if (server_token == 0 || session_id ==0) {
        AGM_LOGE("agm_client_alloc_handle: invalid server_token: %d or session_id: %d", server_token, session_id);
        pthread_mutex_unlock(&g_handle_lock);
    return 0;
    }

    for (uint32_t i = 0; i < AGM_CLIENT_MAX_SESSIONS; ++i) {
        uint32_t handle_idx = (g_next_client_handle + i) % AGM_CLIENT_MAX_SESSIONS;
        if (!g_handle_table[handle_idx].in_use) {
            g_handle_table[handle_idx].in_use = true;
            g_handle_table[handle_idx].token = server_token;
            g_handle_table[handle_idx].session_id = session_id;
            g_next_client_handle = (handle_idx + 1) % AGM_CLIENT_MAX_SESSIONS;
            pthread_mutex_unlock(&g_handle_lock);

          /*
           * Client handle is a packed value:
           *  - Upper bits store the index into g_handle_table[]
           *  - Lower 8 bits store a magic byte for handle validation
           */
            return (handle_idx << 8) | AGM_CLIENT_HANDLE_MAGIC;
        }
    }
    pthread_mutex_unlock(&g_handle_lock);
    return 0;
}

static uint32_t agm_client_get_token(uint64_t client_handle)
{
    uint64_t handle_idx = client_handle >> 8; //extract handle table index from packed client handle

    // Validate:
    // 1. Index is within bounds
    // 2. Entry is in use
    // 3. Magic byte matches expected value

    if (handle_idx >= AGM_CLIENT_MAX_SESSIONS ||
        !g_handle_table[handle_idx].in_use ||
        (client_handle & AGM_CLIENT_HANDLE_MAGIC_MASK) != AGM_CLIENT_HANDLE_MAGIC) {
        return 0;
    }
    AGM_LOGI("token=%u", g_handle_table[handle_idx].token);
    return g_handle_table[handle_idx].token;
}

static uint32_t agm_client_get_session_id(uint64_t client_handle)
{
    uint64_t handle_idx = client_handle >> 8; //extract handle table index from packed client handle

    if (handle_idx >= AGM_CLIENT_MAX_SESSIONS ||
        !g_handle_table[handle_idx].in_use ||
        (client_handle & AGM_CLIENT_HANDLE_MAGIC_MASK) != AGM_CLIENT_HANDLE_MAGIC) {
        return 0;
    }
    return g_handle_table[handle_idx].session_id;
}

static void agm_client_free_handle(uint64_t client_handle)
{
    uint64_t handle_idx = client_handle >> 8;
    if (handle_idx < AGM_CLIENT_MAX_SESSIONS &&
        g_handle_table[handle_idx].in_use &&
        (client_handle & AGM_CLIENT_HANDLE_MAGIC_MASK) == AGM_CLIENT_HANDLE_MAGIC) {
        pthread_mutex_lock(&g_handle_lock);
        memset(&g_handle_table[handle_idx], 0, sizeof(agm_client_handle_t));
        pthread_mutex_unlock(&g_handle_lock);
    }
}

static void agm_cb_handler_cleanup(void *param)
{
    uint8_t *event_buf = (uint8_t *)param;
    if (event_buf) {
        AGM_LOGD("AGM_CLIENT: agm_cb_handler_cleanup - Freeing event buffer memory");
        free(event_buf);
    }
}

int agm_init()
{
     if (g_agm_client) {
        pthread_mutex_lock(&g_mutex_open_cnt);
        g_agm_client->init_count++;
        pthread_mutex_unlock(&g_mutex_open_cnt);
        return 0;
    }

    uint32_t out_len = AGM_PROXY_MAX_MSG_SIZE;

    AGM_DEVICE_TYPE *dev = malloc(sizeof(AGM_DEVICE_TYPE));
    if (!dev) {
        AGM_LOGE("Failed to allocate memory for AGM_DEVICE_TYPE");
        return -ENOMEM;
    }

    memset(dev, 0, sizeof(AGM_DEVICE_TYPE));
    dev->coid = open(AGM_COMPONENT_NAME, O_RDWR);
    if (dev->coid == -1) {
        free(dev);
        AGM_LOGE("Failed to open %s: %s", AGM_COMPONENT_NAME, strerror(errno));
        return -errno;
    }

    g_agm_client = dev;
    g_agm_client->init_count = 1;


    // Initialize callback slots
    for (int i = 0; i < AGM_CLIENT_MAX_SESSIONS; i++) {
        g_client_cb_data[i].mq_des = (mqd_t)-1;
        g_client_cb_data[i].session_id = 0;
        g_client_cb_data[i].cb = NULL;
        g_client_cb_data[i].client_data = NULL;
        memset(g_client_cb_data[i].queue_name, 0, sizeof(g_client_cb_data[i].queue_name));
    }

    uint8_t *out_buf = malloc(AGM_PROXY_MAX_MSG_SIZE);
    if (!out_buf) {
        AGM_LOGE("Failed to allocate response buffer");
        free(dev);
        g_agm_client = NULL;
        return -ENOMEM;
    }

    int rc = agm_send_ipc_msg(dev, AGM_MSG_TYPE_INIT, AGM_OP_INIT, NULL, 0, out_buf, &out_len);
    if (rc != 0) {
        AGM_LOGE("AGM_OP_INIT failed with error: %d", rc);
        close(dev->coid);
        free(dev);
        g_agm_client = NULL;
        free(out_buf);
        return rc;
    }

/* Do not free dev here: published as g_agm_client, freed in agm_deinit() */
    free(out_buf);
    AGM_LOGE("AGM client initialized successfully, %d", rc);
    return 0;
}

int agm_deinit()
{
    if (!g_agm_client) {
        AGM_LOGE("AGM client already deinitialized");
        return 0;
    }

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    if (g_cb_thread_running) {
        g_cb_thread_running = false;
        pthread_join(g_cb_thread, NULL);

        for (int i = 0; i < AGM_CLIENT_MAX_SESSIONS; i++) {
            if (g_client_cb_data[i].mq_des != (mqd_t)-1) {
                mq_close(g_client_cb_data[i].mq_des);
                mq_unlink(g_client_cb_data[i].queue_name);
                memset(&g_client_cb_data[i], 0, sizeof(agm_client_cb_data_t));
                g_client_cb_data[i].mq_des = (mqd_t)-1;
            }
        }
    }

    pthread_mutex_lock(&g_mutex_open_cnt);
    if (--g_agm_client->init_count == 0) {
        int rc = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_DEINIT, AGM_OP_DEINIT, NULL, 0, out_buf, &out_len);
        if (rc != 0) {
            AGM_LOGE("AGM_OP_DEINIT failed with error: %d", rc);
        }

        close(g_agm_client->coid);
        free(g_agm_client);
        g_agm_client = NULL;
    }
    pthread_mutex_unlock(&g_mutex_open_cnt);

    AGM_LOGI("AGM client deinitialized");
    return 0;
}

int agm_get_aif_info_list(struct aif_info *aif_list, size_t *num_aif_info)
{
    if (!num_aif_info) return -EINVAL;

    uint8_t in_buf[sizeof(uint32_t)];
    uint32_t capacity = 0;
    uint32_t out_len = AGM_PROXY_MAX_MSG_SIZE;
    uint32_t count_u32 = 0;

    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc = agm_init();
        pthread_mutex_unlock(&g_mutex_open_cnt);
        if (rc != 0) return rc;
    } else {
        pthread_mutex_unlock(&g_mutex_open_cnt);
    }

    if (aif_list) {
        if (*num_aif_info == 0) return -EINVAL;
        if (*num_aif_info > UINT32_MAX) return -EINVAL;
        capacity = (uint32_t)(*num_aif_info);
    }

    memcpy(in_buf, &capacity, sizeof(uint32_t));

    uint8_t *out_buf = (uint8_t *)malloc(AGM_PROXY_MAX_MSG_SIZE);
    if (!out_buf) return -ENOMEM;

    int status = agm_send_ipc_msg(g_agm_client,
                                  AGM_MSG_TYPE_GET_AIF_INFO_LIST,
                                  AGM_OP_GET_AIF_INFO_LIST,
                                  in_buf, sizeof(in_buf),
                                  out_buf, &out_len);
    if (status != 0 && status != -ENOSPC) {
        free(out_buf);
        return status;
    }

    if (out_len < sizeof(uint32_t)) {
        free(out_buf);
        return -EBADMSG;
    }

    uint8_t *payload = out_buf + sizeof(agm_ipc_response_hdr_t);

    memcpy(&count_u32, payload, sizeof(uint32_t));
    *num_aif_info = (size_t)count_u32;

    if (!aif_list) {
        free(out_buf);
        return 0;
    }

    if (status == -ENOSPC) {
        free(out_buf);
        return -ENOSPC;
    }

    size_t bytes_needed = sizeof(uint32_t) + ((size_t)count_u32 * sizeof(struct aif_info));
    if (out_len < bytes_needed) {
        free(out_buf);
        return -EBADMSG;
    }

    memcpy(aif_list, payload + sizeof(uint32_t), count_u32 * sizeof(struct aif_info));
    for (int i = 0; i < *num_aif_info; i++) {
        AGM_LOGE("%s: [%d] name='%s', dir=%d\n", __func__,
                 i, aif_list[i].aif_name, aif_list[i].dir);
    }
    free(out_buf);
    return 0;
}

int agm_get_group_aif_info_list(struct aif_info *aif_list, size_t *num_aif_info)
{
    if (!num_aif_info) return -EINVAL;

    /* Ensure client initialized (same pattern as your other proxy calls) */
    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc = agm_init();
        pthread_mutex_unlock(&g_mutex_open_cnt);
        if (rc != 0) return rc;
    } else {
        pthread_mutex_unlock(&g_mutex_open_cnt);
    }

    uint32_t capacity = 0;
    if (aif_list) {
        if (*num_aif_info == 0) return -EINVAL;
        if (*num_aif_info > UINT32_MAX) return -EINVAL;
        capacity = (uint32_t)(*num_aif_info);
    }

    uint8_t in_buf[sizeof(uint32_t)];
    memcpy(in_buf, &capacity, sizeof(uint32_t));

    uint8_t *out_buf = (uint8_t *)malloc(AGM_PROXY_MAX_MSG_SIZE);
    if (!out_buf) return -ENOMEM;
    uint32_t out_len = AGM_PROXY_MAX_MSG_SIZE;

    int status = agm_send_ipc_msg(g_agm_client,
                                  AGM_MSG_TYPE_AGM_GET_GROUP_AIF_INFO_LIST,
                                  AGM_OP_AGM_GET_GROUP_AIF_INFO_LIST,
                                  in_buf, sizeof(in_buf),
                                  out_buf, &out_len);

    if (status != 0 && status != -ENOSPC) {
        free(out_buf);
        return status;
    }

    if (out_len < sizeof(uint32_t)) {
        free(out_buf);
        return -EBADMSG;
    }

    uint8_t *payload = out_buf + sizeof(agm_ipc_response_hdr_t);
    uint32_t count_u32 = 0;
    memcpy(&count_u32, payload, sizeof(uint32_t));
    *num_aif_info = (size_t)count_u32;

    if (!aif_list) {
        free(out_buf);
        return 0; /* count-only success */
    }

    if (status == -ENOSPC) {
        free(out_buf);
        return -ENOSPC; /* caller must realloc to *num_aif_info and retry */
    }

    size_t bytes_needed = sizeof(uint32_t) + ((size_t)count_u32 * sizeof(struct aif_info));
    if (out_len < bytes_needed) {
        free(out_buf);
        return -EBADMSG;
    }

    memcpy(aif_list, payload + sizeof(uint32_t), count_u32 * sizeof(struct aif_info));

    AGM_LOGD("%s aftr server",__func__);
    for (int i = 0; i < *num_aif_info; i++) {
        AGM_LOGE("%s: [%d] name='%s', dir=%d\n", __func__,
                 i, aif_list[i].aif_name, aif_list[i].dir);
    }
    free(out_buf);
    return 0;
}

int agm_set_params_to_acdb_tunnel(void *payload, size_t size)
{
    AGM_LOGD("agm_set_params_to_acdb_tunnel: enter");

    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc = agm_init();
        if (rc != 0) {
            AGM_LOGE("agm_init failed rc=%d", rc);
            pthread_mutex_unlock(&g_mutex_open_cnt);
            return rc;
        }
        pthread_mutex_unlock(&g_mutex_open_cnt);
    } else {
        pthread_mutex_unlock(&g_mutex_open_cnt);
        AGM_LOGD("agm_client already initialized");
    }

    uint32_t payload_sz = (uint32_t)size;
    uint32_t in_len = 4 + payload_sz;
    uint8_t *in_buf = (uint8_t *)malloc(in_len);
    if (!in_buf) {
        AGM_LOGE("malloc(%u) failed for in_buf", in_len);
        return -ENOMEM;
    }
    memcpy(in_buf, &payload_sz, 4);
    memcpy(in_buf+4,payload,payload_sz);

    uint8_t out_buf[4];
    uint32_t out_len = sizeof(out_buf);
    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SET_PARAMS_ACDB_TUNNEL,
                                  AGM_OP_SET_PARAMS_ACDB_TUNNEL,
                                  in_buf, in_len,
                                  out_buf, &out_len);

    free(in_buf);
    if(status)
        AGM_LOGE("agm_set_params_to_acdb_tunnel failed %d", status);

    return status;
}


int agm_get_params_from_acdb_tunnel(void *payload, size_t *size)
{
    AGM_LOGD("agm_set_params_to_acdb_tunnel: Not impl return");
    return 0;
    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc = agm_init();
        if (rc != 0) {
            AGM_LOGE("agm_init failed rc=%d", rc);
            pthread_mutex_unlock(&g_mutex_open_cnt);
            return rc;
        }
        pthread_mutex_unlock(&g_mutex_open_cnt);
    } else {
        pthread_mutex_unlock(&g_mutex_open_cnt);
        AGM_LOGD("agm_client already initialized");
    }

    uint32_t payload_sz = (uint32_t)size;
    uint32_t in_len = 4 + payload_sz;
    uint8_t *in_buf = (uint8_t *)malloc(in_len);
    if (!in_buf) {
        AGM_LOGE("malloc(%u) failed for in_buf", in_len);
        return -ENOMEM;
    }
    memcpy(in_buf, &payload_sz, 4);
    memcpy(in_buf+4,payload,payload_sz);

    uint8_t out_buf[4];
    uint32_t out_len = sizeof(out_buf);
    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_GET_PARAMS_ACDB_TUNNEL,
                                  AGM_OP_GET_PARAMS_ACDB_TUNNEL,
                                  in_buf, in_len,
                                  out_buf, &out_len);

    free(in_buf);
    if(status)
        AGM_LOGE("agm_set_params_to_acdb_tunnel failed %d", status);

    return status;
}

int agm_session_aif_set_cal(uint32_t session_id,
                 uint32_t aif_id,
                 struct agm_cal_config *cal_config)
{
    AGM_LOGD("%s: enter",__func__);

    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc = agm_init();
        pthread_mutex_unlock(&g_mutex_open_cnt);
        if (rc != 0) {
            AGM_LOGE("agm_init failed rc=%d", rc);
            return rc;
        }
        AGM_LOGE("agm_init done");
    } else {
        pthread_mutex_unlock(&g_mutex_open_cnt);
        AGM_LOGE("agm_client already initialized");
    }

    if(!cal_config){
        AGM_LOGD("%s: cal_config NULL",__func__);
    }
    uint32_t cal_size = sizeof(struct agm_cal_config);
    uint32_t in_len = 12 + cal_size;
    uint8_t *in_buf = (uint8_t *)malloc(in_len);
    if (!in_buf) {
        AGM_LOGE("malloc(%u) failed for in_buf", in_len);
        return -ENOMEM;
    }
    memcpy(in_buf,&session_id,4);
    memcpy(in_buf+4,&aif_id,4);
    memcpy(in_buf+8,&cal_size,4);
    memcpy(in_buf+12,cal_config,cal_size);

    uint8_t out_buf[4];
    uint32_t out_len = sizeof(out_buf);
    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SESSION_AIF_SET_CAL,
                                  AGM_OP_SESSION_AIF_SET_CAL,
                                  in_buf, in_len,
                                  out_buf, &out_len);

    free(in_buf);
    if(status)
        AGM_LOGE("%s failed %d",__func__, status);

    return status;

}
int agm_session_get_params(uint32_t session_id, void* payload, size_t size)
{
    AGM_LOGD("%s: enter",__func__);

    if (!payload || size == 0) return -EINVAL;

    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc = agm_init();
        pthread_mutex_unlock(&g_mutex_open_cnt);
        if (rc != 0) {
            AGM_LOGE("agm_init failed rc=%d", rc);
            return rc;
        }
        AGM_LOGE("agm_init done");
    } else {
        pthread_mutex_unlock(&g_mutex_open_cnt);
        AGM_LOGE("agm_client already initialized");
    }

    uint32_t payload_sz = (uint32_t)size;
    uint32_t in_len = 8;
    uint8_t in_buf[8];
    memcpy(in_buf, &session_id, 4);
    memcpy(in_buf+4, &payload_sz, 4);

    uint8_t *out_buf = (uint8_t *)malloc(4 + payload_sz);
    if (!out_buf) return -ENOMEM;

    uint32_t out_len = 4 + payload_sz;

    int status = agm_send_ipc_msg(g_agm_client,
                                  AGM_MSG_TYPE_SESSION_GET_PARAMS,
                                  AGM_OP_SESSION_GET_PARAMS,
                                  in_buf, in_len,
                                  out_buf, &out_len);

    if (status != 0) {
        AGM_LOGE("%s: agm_send_ipc_msg failed %d", __func__, status);
        free(out_buf);
        return status;
    }

    if (out_len < 4) {
        AGM_LOGE("%s: Invalid response length: %u", __func__, out_len);
        free(out_buf);
        return -EBADMSG;
    }

    uint32_t returned_sz;
    memcpy(&returned_sz, out_buf, 4);

    if (returned_sz > payload_sz) {
        AGM_LOGE("%s: Returned size %u exceeds requested size %u",
                 __func__, returned_sz, payload_sz);
        free(out_buf);
        return -EMSGSIZE;
    }

    if (out_len < 4 + returned_sz) {
        AGM_LOGE("%s: Buffer underrun: out_len=%u, need=%u",
                 __func__, out_len, 4 + returned_sz);
        free(out_buf);
        return -EBADMSG;
    }

    memcpy(payload, out_buf+4, returned_sz);
    free(out_buf);

    AGM_LOGD("%s: success, returned %u bytes", __func__, returned_sz);
    return 0;
}

int agm_aif_set_params(uint32_t aif_id,
                        void* payload, size_t size)
{
    AGM_LOGD("%s: enter",__func__);

    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc = agm_init();
        pthread_mutex_unlock(&g_mutex_open_cnt);
        if (rc != 0) {
            AGM_LOGE("agm_init failed rc=%d", rc);
            return rc;
        }
        AGM_LOGE("agm_init done");
    } else {
        pthread_mutex_unlock(&g_mutex_open_cnt);
        AGM_LOGE("agm_client already initialized");
    }

    uint32_t payload_sz = (uint32_t)size;
    uint32_t in_len = 8 + payload_sz;
    uint8_t *in_buf = (uint8_t *)malloc(in_len);
    if (!in_buf) {
        AGM_LOGE("malloc(%u) failed for in_buf", in_len);
        return -ENOMEM;
    }
    memcpy(in_buf, &aif_id, 4);
    memcpy(in_buf+4, &payload_sz, 4);
    memcpy(in_buf+8,payload,payload_sz);

    uint8_t out_buf[4];
    uint32_t out_len = sizeof(out_buf);
    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_AIF_SET_PARAMS,
                                  AGM_OP_AIF_SET_PARAMS,
                                  in_buf, in_len,
                                  out_buf, &out_len);

    free(in_buf);
    if(status)
        AGM_LOGE("%s failed %d", __func__,status);

    return status;
}
int agm_session_get_buf_info(uint32_t session_id, struct agm_buf_info *buf_info, uint32_t flag)
{
    if (!buf_info) return -EINVAL;

    /* Ensure client initialized (same pattern as your other proxy calls) */
    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc = agm_init();
        pthread_mutex_unlock(&g_mutex_open_cnt);
        if (rc != 0) return rc;
    } else {
        pthread_mutex_unlock(&g_mutex_open_cnt);
    }

    /* Prepare input buffer: session_id + flag */
    uint8_t in_buf[sizeof(uint32_t) * 2];
    memcpy(in_buf, &session_id, sizeof(uint32_t));
    memcpy(in_buf + sizeof(uint32_t), &flag, sizeof(uint32_t));

    /* Prepare output buffer */
    uint8_t *out_buf = (uint8_t *)malloc(AGM_PROXY_MAX_MSG_SIZE);
    if (!out_buf) return -ENOMEM;

    uint32_t out_len = AGM_PROXY_MAX_MSG_SIZE;

    int status = agm_send_ipc_msg(g_agm_client,
                                  AGM_MSG_TYPE_SESSION_GET_BUF_INFO,
                                  AGM_OP_SESSION_GET_BUF_INFO,
                                  in_buf, sizeof(in_buf),
                                  out_buf, &out_len);

    if (status != 0) {
        AGM_LOGE("%s: agm_send_ipc_msg failed with status=%d\n", __func__, status);
        free(out_buf);
        return status;
    }

    /* Validate response size */
    if (out_len < sizeof(agm_ipc_response_hdr_t) + sizeof(struct agm_buf_info)) {
        AGM_LOGE("%s: Invalid response length: %u\n", __func__, out_len);
        free(out_buf);
        return -EBADMSG;
    }

    /* Extract buf_info from payload */
    uint8_t *payload = out_buf + sizeof(agm_ipc_response_hdr_t);
    memcpy(buf_info, payload, sizeof(struct agm_buf_info));

    AGM_LOGE("%s: session_id=%u, flag=%u, buf_info retrieved successfully\n",
             __func__, session_id, flag);

    free(out_buf);
    return 0;
}

int agm_session_set_loopback(uint32_t capture_session_id,
                             uint32_t playback_session_id,
                             bool state)
{
    /* Ensure client initialized (same pattern as your other proxy calls) */
    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc = agm_init();
        pthread_mutex_unlock(&g_mutex_open_cnt);
        if (rc != 0) return rc;
    } else {
        pthread_mutex_unlock(&g_mutex_open_cnt);
    }

    /* Prepare input buffer: capture_session_id + playback_session_id + state */
    uint8_t in_buf[sizeof(uint32_t) * 3];
    uint32_t state_u32 = state ? 1 : 0;

    memcpy(in_buf, &capture_session_id, sizeof(uint32_t));
    memcpy(in_buf + sizeof(uint32_t), &playback_session_id, sizeof(uint32_t));
    memcpy(in_buf + sizeof(uint32_t) * 2, &state_u32, sizeof(uint32_t));

    /* Prepare output buffer */
    uint8_t *out_buf = (uint8_t *)malloc(AGM_PROXY_MAX_MSG_SIZE);
    if (!out_buf) return -ENOMEM;

    uint32_t out_len = AGM_PROXY_MAX_MSG_SIZE;

    int status = agm_send_ipc_msg(g_agm_client,
                                  AGM_MSG_TYPE_SESSION_SET_LOOPBACK,
                                  AGM_OP_SESSION_SET_LOOPBACK,
                                  in_buf, sizeof(in_buf),
                                  out_buf, &out_len);

    if (status != 0) {
        AGM_LOGE("%s: agm_send_ipc_msg failed with status=%d\n", __func__, status);
        free(out_buf);
        return status;
    }

    AGM_LOGE("%s: capture_session_id=%u, playback_session_id=%u, state=%d - success\n",
             __func__, capture_session_id, playback_session_id, state);

    free(out_buf);
    return 0;
}

int agm_session_set_ec_ref(uint32_t capture_session_id,
                           uint32_t aif_id,
                           bool state)
{
    /* Ensure client initialized (same pattern as your other proxy calls) */
    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc = agm_init();
        pthread_mutex_unlock(&g_mutex_open_cnt);
        if (rc != 0) return rc;
    } else {
        pthread_mutex_unlock(&g_mutex_open_cnt);
    }

    /* Prepare input buffer: capture_session_id + aif_id + state */
    uint8_t in_buf[sizeof(uint32_t) * 3];
    uint32_t state_u32 = state ? 1 : 0;

    memcpy(in_buf, &capture_session_id, sizeof(uint32_t));
    memcpy(in_buf + sizeof(uint32_t), &aif_id, sizeof(uint32_t));
    memcpy(in_buf + sizeof(uint32_t) * 2, &state_u32, sizeof(uint32_t));

    /* Prepare output buffer */
    uint8_t *out_buf = (uint8_t *)malloc(AGM_PROXY_MAX_MSG_SIZE);
    if (!out_buf) return -ENOMEM;

    uint32_t out_len = AGM_PROXY_MAX_MSG_SIZE;

    int status = agm_send_ipc_msg(g_agm_client,
                                  AGM_MSG_TYPE_SESSION_SET_EC_REF,
                                  AGM_OP_SESSION_SET_EC_REF,
                                  in_buf, sizeof(in_buf),
                                  out_buf, &out_len);

    if (status != 0) {
        AGM_LOGE("%s: agm_send_ipc_msg failed with status=%d\n", __func__, status);
        free(out_buf);
        return status;
    }

    AGM_LOGE("%s: capture_session_id=%u, aif_id=%u, state=%d - success\n",
             __func__, capture_session_id, aif_id, state);

    free(out_buf);
    return 0;
}

int agm_get_buffer_timestamp(uint32_t session_id, uint64_t *timestamp)
{
    if (!timestamp) return -EINVAL;

    /* Ensure client initialized (same pattern as your other proxy calls) */
    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc = agm_init();
        pthread_mutex_unlock(&g_mutex_open_cnt);
        if (rc != 0) return rc;
    } else {
        pthread_mutex_unlock(&g_mutex_open_cnt);
    }

    /* Prepare input buffer: session_id */
    uint8_t in_buf[sizeof(uint32_t)];
    memcpy(in_buf, &session_id, sizeof(uint32_t));

    /* Prepare output buffer */
    uint8_t *out_buf = (uint8_t *)malloc(AGM_PROXY_MAX_MSG_SIZE);
    if (!out_buf) return -ENOMEM;

    uint32_t out_len = AGM_PROXY_MAX_MSG_SIZE;

    int status = agm_send_ipc_msg(g_agm_client,
                                  AGM_MSG_TYPE_GET_BUFFER_TIMESTAMP,
                                  AGM_OP_GET_BUFFER_TIMESTAMP,
                                  in_buf, sizeof(in_buf),
                                  out_buf, &out_len);

    if (status != 0) {
        AGM_LOGE("%s: agm_send_ipc_msg failed with status=%d\n", __func__, status);
        free(out_buf);
        return status;
    }

    /* Validate response size */
    if (out_len < sizeof(agm_ipc_response_hdr_t) + sizeof(uint64_t)) {
        AGM_LOGE("%s: Invalid response length: %u\n", __func__, out_len);
        free(out_buf);
        return -EBADMSG;
    }

    /* Extract timestamp from payload */
    uint8_t *payload = out_buf + sizeof(agm_ipc_response_hdr_t);
    memcpy(timestamp, payload, sizeof(uint64_t));

    AGM_LOGE("%s: session_id=%u, timestamp=%llu - success\n",
             __func__, session_id, (unsigned long long)*timestamp);

    free(out_buf);
    return 0;
}


int agm_set_params_with_tag_to_acdb(uint32_t session_id, uint32_t aif_id,
                                       void *payload, size_t size) {return 0;}

int agm_session_set_params(uint32_t session_id,
                         void* payload, size_t size) {return 0;}

int agm_session_aif_set_params(uint32_t session_id,
                        uint32_t aif_id,
                        void* payload, size_t size) { return 0;}

int agm_session_aif_get_tag_module_info(uint32_t session_id,
                                 uint32_t aif_id, void *payload, size_t *size) {return 0;}

int agm_session_write_datapath_params(uint32_t session_id, struct agm_buff *buff) {return 0;}

int agm_sessionid_flush(uint32_t session_id)  {return 0; }

int agm_session_register_for_events(uint32_t session_id,
                            struct agm_event_reg_cfg *evt_reg_cfg)  {return 0; }

int agm_set_params_with_tag(uint32_t session_id, uint32_t aif_id,
                            struct agm_tag_config *tag_config) {return 0; }

// Stream metadata:
int agm_session_set_metadata(uint32_t session_id, uint32_t size, uint8_t *metadata)
{

    if (!g_agm_client) {
        AGM_LOGE("g_agm_client null");
        return -EINVAL;
    }

    if (!metadata || size == 0) return -EINVAL;

    const uint32_t in_len = 8 + size;
    uint8_t* in_buf = (uint8_t*)malloc(in_len);
    if (!in_buf) return -ENOMEM;

    memcpy(in_buf,       &session_id, 4);
    memcpy(in_buf + 4,   &size, 4);
    memcpy(in_buf + 8,   metadata,  size);

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SET_STREAM_METADATA,
                                  AGM_OP_STREAM_METADATA,
                                  in_buf, in_len,
                                  out_buf, &out_len);
    free(in_buf);

    if (status != 0) {
        AGM_LOGE("AGM_OP_STREAM_METADATA failed: %d", status);
    }
    return status;
}

// Device metadata: AIF-scoped
int agm_aif_set_metadata(uint32_t aif_id, uint32_t size, uint8_t *metadata)
{

    if (!g_agm_client) {
        AGM_LOGE("g_agm_client null");
        return -EINVAL;
    }

    if (!metadata || size == 0) return -EINVAL;

    const uint32_t in_len = 8 + size;
    uint8_t* in_buf = (uint8_t*)malloc(in_len);
    if (!in_buf) return -ENOMEM;

    memcpy(in_buf,       &aif_id,      4);
    memcpy(in_buf + 4,   &size, 4);
    memcpy(in_buf + 8,   metadata,  size);

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SET_DEVICE_METADATA,
                                  AGM_OP_DEVICE_METADATA,
                                  in_buf, in_len,
                                  out_buf, &out_len);
    free(in_buf);

    if (status != 0) {
        AGM_LOGE("AGM_OP_DEVICE_METADATA failed: %d", status);
    }
    return status;
}

// Device-PP metadata: session + AIF scoped
int agm_session_aif_set_metadata(uint32_t session_id,uint32_t aif_id,
                                    uint32_t size, uint8_t *metadata)
{
    if (!g_agm_client) {
        AGM_LOGE("g_agm_client null");
        return -EINVAL;
    }

    if (!metadata || size == 0) return -EINVAL;

    const uint32_t in_len = 12 + size;
    uint8_t* in_buf = (uint8_t*)malloc(in_len);
    if (!in_buf) return -ENOMEM;


    memcpy(in_buf,       &session_id,    4);
    memcpy(in_buf + 4,   &aif_id,        4);
    memcpy(in_buf + 8,   &size, 4);
    memcpy(in_buf + 12,  metadata,  size);

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SET_DEVICE_PP_METADATA,
                                  AGM_OP_DEVICE_PP_METADATA,
                                  in_buf, in_len,
                                  out_buf, &out_len);
    free(in_buf);

    if (status != 0) {
        AGM_LOGE("AGM_OP_DEVICE_PP_METADATA failed: %d", status);
    }
    return status;
}

int agm_session_aif_connect(uint32_t session_id, uint32_t aif_id, bool state)
{

    if (!g_agm_client) {
        AGM_LOGE("g_agm_client null");
        return -EINVAL;
    }

    uint8_t in_buf[12];
    uint32_t state_u32 = state ? 1u : 0u;

    memcpy(in_buf,     &session_id,   4);
    memcpy(in_buf + 4, &aif_id,       4);
    memcpy(in_buf + 8, &state_u32,  4);

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SESSION_AIF_CONNECT,
                                  AGM_OP_SESSION_AIF_CONNECT,
                                  in_buf, sizeof(in_buf),
                                  out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_OP_SESSION_AIF_CONNECT failed: %d", status);
    }
    return status;
}


/*sample rate,  number of channels, format, data format */
int agm_aif_set_media_config(uint32_t aif_id, struct agm_media_config* media_cfg)
{
    if (!g_agm_client) {
        AGM_LOGE("g_agm_client null");
        return -EINVAL;
    }

    if (!media_cfg) return -EINVAL;
    uint8_t in_buf[4 + sizeof(*media_cfg)];
    memcpy(in_buf, &aif_id,  4);
    memcpy(in_buf + 4, media_cfg,  sizeof(*media_cfg));

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SET_MEDIA_CONFIG,
                                  AGM_OP_SET_MEDIA_CONFIG,
                                  in_buf, sizeof(in_buf),
                                  out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_OP_SET_AIF_MEDIA_CONFIG failed: %d", status);
    }
    return status;
}

// Session config (buffer, media, flags…)
int agm_session_set_config(uint64_t handle,
                                  struct agm_session_config* stream_config,
                                  struct agm_media_config*  media_cfg,
                                  struct agm_buffer_config* buf_cfg)
{
    if (!g_agm_client || handle == 0 || !stream_config || !media_cfg || !buf_cfg)
        return -EINVAL;

    uint32_t token = agm_client_get_token(handle);
    if (!token) return -EINVAL;

    const size_t in_len = sizeof(token)
                        + sizeof(*stream_config)
                        + sizeof(*media_cfg)
                        + sizeof(*buf_cfg);

    uint8_t in_buf[in_len];
    size_t off = 0;

    memcpy(in_buf + off, &token, sizeof(token)); off += sizeof(token);
    memcpy(in_buf + off, stream_config, sizeof(*stream_config)); off += sizeof(*stream_config);
    memcpy(in_buf + off, media_cfg, sizeof(*media_cfg)); off += sizeof(*media_cfg);
    memcpy(in_buf + off, buf_cfg, sizeof(*buf_cfg)); off += sizeof(*buf_cfg);

    uint8_t  out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SET_SESSION_CONFIG,
                            AGM_OP_SET_SESSION_CONFIG,
                            in_buf, (uint32_t)in_len,
                            out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_MSG_TYPE_SET_SESSION_CONFIG failed: %d", status);
    }
    return status;
}

int agm_aif_group_set_media_config(uint32_t aif_group_id,struct agm_group_media_config *media_config)
{
    AGM_LOGD(" agm_se_aif_media_config enter");

    if (!g_agm_client) {
        AGM_LOGE("g_agm_client null");
        return -EINVAL;
    }

    if (!media_config) return -EINVAL;
    uint8_t in_buf[4 + sizeof(*media_config)];
    memcpy(in_buf, &aif_group_id,  4);
    memcpy(in_buf + 4, media_config,  sizeof(*media_config));

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_GROUP_SET_MEDIA_CONFIG,
                                  AGM_OP_GROUP_SET_MEDIA_CONFIG,
                                  in_buf, sizeof(in_buf),
                                  out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_OP_GROUP_SET_MEDIA_CONFIG failed: %d", status);
    }
    return status;
}

int agm_session_open(uint32_t session_id,enum agm_session_mode sess_mode,uint64_t *handle)
{
    if (!handle) {
        AGM_LOGE(" agm_open: handle pointer is NULL");
        return -EINVAL;
    }

    pthread_mutex_lock(&g_mutex_open_cnt);
    if (!g_agm_client) {
        int rc =agm_init() ;
        if (rc) {
            pthread_mutex_unlock(&g_mutex_open_cnt);
            AGM_LOGE(" agm_open: agm_init failed");
            return rc;
        }
    }
    pthread_mutex_unlock(&g_mutex_open_cnt);

    AGM_LOGI(" agm_session_open: enter (session_id=%u, session_mode=%u, handle_ptr=%p)",
             session_id, sess_mode, (void*)handle);

    uint32_t server_token = 0;
    uint8_t in_buf[8];
    memcpy(in_buf,     &session_id,   4);
    memcpy(in_buf + 4, &sess_mode, 4);

    uint8_t *out_buf = (uint8_t *)malloc(AGM_PROXY_MAX_MSG_SIZE);
    if (!out_buf) {
        AGM_LOGE(" agm_open: malloc(%u) failed", AGM_PROXY_MAX_MSG_SIZE);
        return -ENOMEM;
    }
    uint32_t out_len = AGM_PROXY_MAX_MSG_SIZE;

    int status = agm_send_ipc_msg(g_agm_client,
                                  AGM_MSG_TYPE_SESSION_OPEN,
                                  AGM_OP_SESSION_OPEN,
                                  in_buf, sizeof(in_buf),
                                  out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE(" agm_open: agm_send_ipc_msg failed (status=%d)", status);
        free(out_buf);
        return status;
    }

    if (out_len < sizeof(uint32_t)) {
        AGM_LOGE(" agm_open: response payload too small (out_len=%u)", out_len);
        free(out_buf);
        return -EINVAL;
    }

    agm_ipc_response_hdr_t *rsp_hdr = (agm_ipc_response_hdr_t *)out_buf;
    if (rsp_hdr->payload_len < sizeof(uint32_t)) {
        AGM_LOGE(" agm_open: rsp_hdr->payload_len too small (%u)", rsp_hdr->payload_len);
        free(out_buf);
        return -EINVAL;
    }

    memcpy(&server_token, out_buf + sizeof(agm_ipc_response_hdr_t), sizeof(server_token));
    AGM_LOGI(" agm_open: server_token=%u", server_token);

    *handle = agm_client_alloc_handle(server_token, session_id);
    free(out_buf);

    if (*handle == 0) {
        AGM_LOGE(" agm_open: agm_client_alloc_handle returned 0");
        return -EINVAL;
    }

    AGM_LOGI(" agm_open: success (handle=0x%08x)", *handle);
    return 0;
}

int agm_session_close(uint64_t handle)
{
    if (!g_agm_client || handle == 0) {
        AGM_LOGE("Invalid client state for agm_close");
        return -EINVAL;
    }
    uint8_t in_buf[sizeof(uint32_t)];
  uint32_t session_id = agm_client_get_session_id(handle);
    uint32_t server_token = agm_client_get_token(handle);
    if (server_token == 0) {
        AGM_LOGE("Invalid client handle 0x%08x", handle);
        return -EINVAL;
    }

    memcpy(in_buf, &server_token, sizeof(server_token));

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SESSION_CLOSE, AGM_OP_SESSION_CLOSE,
                                 in_buf, sizeof(in_buf),
                                 out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_OP_SESSION_CLOSE failed with error: %d", status);
    }

    agm_client_free_handle(handle);

    // unregister any callbacks for this session
    pthread_mutex_lock(&g_cb_lock);
    for (int i = 0; i < AGM_CLIENT_MAX_SESSIONS; i++) {
        if (g_client_cb_data[i].session_id == session_id) {
            if (g_client_cb_data[i].mq_des != (mqd_t)-1) {
                mq_close(g_client_cb_data[i].mq_des);
                mq_unlink(g_client_cb_data[i].queue_name);
            }
            memset(&g_client_cb_data[i], 0, sizeof(agm_client_cb_data_t));
            g_client_cb_data[i].mq_des = (mqd_t)-1;
            break;
        }
    }
    pthread_mutex_unlock(&g_cb_lock);

    /* ref-counted client shutdown */
    pthread_mutex_lock(&g_mutex_open_cnt);

    if (!g_agm_client) {
        pthread_mutex_unlock(&g_mutex_open_cnt);
        AGM_LOGE("AGM client already deinitialized");
        return -EFAULT;
    }

    if (--g_agm_client->init_count == 0) {
        agm_deinit();   // g_agm_client =NULL is set in agm_deinit()
    }

    pthread_mutex_unlock(&g_mutex_open_cnt);

    AGM_LOGI("Closed client handle 0x%08x", handle);
    return status;
}

// Prepare a session
int agm_session_prepare(uint64_t handle)
{

    if (!g_agm_client || handle == 0) {
        AGM_LOGE("Invalid client state for agm_start");
        return -EINVAL;
    }
    uint32_t server_token = agm_client_get_token(handle);
    if (server_token == 0) {
        AGM_LOGE("Invalid client handle 0x%08x", handle);
        return -EINVAL;
    }

    uint8_t in_buf[sizeof(uint32_t)];
    memcpy(in_buf, &server_token, sizeof(server_token));

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SESSION_PREPARE, AGM_OP_SESSION_PREPARE,
                                 in_buf, sizeof(in_buf),
                                 out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_OP_SESSION_PREPARE failed with error: %d", status);
    }

    AGM_LOGI("Started client handle 0x%08x", handle);
    return status;
}

int agm_session_start(uint64_t handle)
{
    if (!g_agm_client || handle == 0) {
        AGM_LOGE("Invalid client state for agm_start");
        return -EINVAL;
    }
    uint32_t server_token = agm_client_get_token(handle);
    if (server_token == 0) {
        AGM_LOGE("Invalid client handle 0x%08x", handle);
        return -EINVAL;
    }

    uint8_t in_buf[sizeof(uint32_t)];
    memcpy(in_buf, &server_token, sizeof(server_token));

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SESSION_START, AGM_OP_SESSION_START,
                                 in_buf, sizeof(in_buf),
                                 out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_OP_SESSION_START failed with error: %d", status);
    }

    AGM_LOGE("Started client handle 0x%08x", handle);
    return status;
}

int agm_session_stop(uint64_t handle)
{
    if (!g_agm_client || handle == 0) {
        AGM_LOGE("Invalid client state for agm_stop");
        return -EINVAL;
    }

    uint32_t server_token = agm_client_get_token(handle);
    if (server_token == 0) {
        AGM_LOGE("Invalid client handle 0x%08x", handle);
        return -EINVAL;
    }

    uint8_t in_buf[sizeof(uint32_t)];
    memcpy(in_buf, &server_token, sizeof(server_token));

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SESSION_STOP, AGM_OP_SESSION_STOP,
                                 in_buf, sizeof(in_buf),
                                 out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_OP_SESSION_STOP failed with error: %d", status);
    }

    AGM_LOGI("Stopped client handle 0x%08x", handle);
    return status;
}

int agm_session_pause(uint64_t handle)
{
    if (!g_agm_client || handle == 0) {
        AGM_LOGE("Invalid client state for agm_pause");
        return -EINVAL;
    }

    uint32_t server_token = agm_client_get_token(handle);
    if (server_token == 0) {
        AGM_LOGE("Invalid client handle 0x%08x", handle);
        return -EINVAL;
    }

    uint8_t in_buf[sizeof(uint32_t)];
    memcpy(in_buf, &server_token, sizeof(server_token));

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SESSION_PAUSE, AGM_OP_SESSION_PAUSE,
                                 in_buf, sizeof(in_buf),
                                 out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_OP_SESSION_PAUSE failed with error: %d", status);
    }

    AGM_LOGI("Paused client handle 0x%08x", handle);
    return status;
}


int agm_session_resume(uint64_t handle)
{
    if (!g_agm_client || handle == 0) {
        AGM_LOGE("Invalid client state for agm_resume");
        return -EINVAL;
    }

    uint32_t server_token = agm_client_get_token(handle);
    if (server_token == 0) {
        AGM_LOGE("Invalid client handle 0x%08x", handle);
        return -EINVAL;
    }

    uint8_t in_buf[sizeof(uint32_t)];
    memcpy(in_buf, &server_token, sizeof(server_token));

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SESSION_RESUME, AGM_OP_SESSION_RESUME,
                                 in_buf, sizeof(in_buf),
                                 out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_OP_SESSION_RESUME failed with error: %d", status);
    }

    AGM_LOGI("Resumed client handle 0x%08x", handle);
    return status;
}

int agm_session_read(uint64_t handle, void *buffer, size_t *bytes_read)
{
    if (!g_agm_client || handle == 0 || !buffer || !bytes_read || *bytes_read == 0) {
        AGM_LOGE("Invalid parameters for agm_read");
        return -EINVAL;
    }

    uint32_t server_token = agm_client_get_token(handle);
    if (server_token == 0) {
        AGM_LOGE("Invalid client handle 0x%08x", handle);
        return -EINVAL;
    }

    uint32_t req = (uint32_t)(*bytes_read);

    uint8_t in_buf[sizeof(uint32_t) + sizeof(uint32_t)];
    memcpy(in_buf, &server_token, sizeof(uint32_t));
    memcpy(in_buf + sizeof(uint32_t), &req, sizeof(uint32_t));

    uint32_t max_payload = 4 + req;
    uint32_t out_len = sizeof(agm_ipc_response_hdr_t) + max_payload;
    uint8_t *out_buf = malloc(out_len);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SESSION_READ, AGM_OP_SESSION_READ,
                                 in_buf, sizeof(in_buf),
                                 out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_OP_SESSION_READ failed with error: %d", status);
        free(out_buf);
        return status;
    }

    if (out_len < 4) {
        AGM_LOGE("Invalid response length for AGM_OP_SESSION_READ: %u", out_len);
        free(out_buf);
        return -EFAULT;
    }

    uint8_t *payload = out_buf + sizeof(agm_ipc_response_hdr_t);

    uint32_t captured = 0;
    memcpy(&captured, payload, 4);

    if (captured > 0 && captured <= req) {
        memcpy(buffer, payload + sizeof(uint32_t), captured);
        *bytes_read = captured;
    } else {
        *bytes_read = 0;
    }
    free(out_buf);
    AGM_LOGI("Read %u bytes from client handle 0x%08x", *bytes_read, handle);
    return 0;
}


int agm_session_write(uint64_t handle, void *buffer, size_t *bytes_written)
{
    if (!g_agm_client || handle == 0 || !buffer || !bytes_written || *bytes_written == 0)
        return -EINVAL;

    uint32_t token = agm_client_get_token(handle);
    if (!token) return -EINVAL;

    uint32_t sz = (uint32_t)(*bytes_written);

    size_t in_len = 8 + sz;
    uint8_t *in_buf = malloc(in_len);
    if (!in_buf) return -ENOMEM;

    memcpy(in_buf, &token, 4);
    memcpy(in_buf + 4, &sz, 4);
    memcpy(in_buf + 8, buffer, sz);

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t) + 4];
    uint32_t out_len = sizeof(out_buf);
    uint32_t consumed = 0;

    int status = agm_send_ipc_msg(g_agm_client,
                                  AGM_MSG_TYPE_SESSION_WRITE,
                                  AGM_OP_SESSION_WRITE,
                                  in_buf, (uint32_t)in_len,
                                  out_buf, &out_len);
    free(in_buf);
    if (status) return status;

    if (out_len < 4) return -EFAULT;

    memcpy(&consumed, out_buf + sizeof(agm_ipc_response_hdr_t), 4);

    *bytes_written = (consumed <= sz) ? consumed : 0;

    AGM_LOGI("Wrote %u bytes to client handle 0x%08x", *bytes_written, handle);
    return 0;
}


int agm_send_ipc_msg(AGM_DEVICE_TYPE *dev,
                     uint16_t msgType,
                     uint32_t opcode,
                     const uint8_t *in_buf, uint32_t in_len,
                     uint8_t *out_buf, uint32_t *out_len)
{
    if (!dev || dev->coid < 0) return -EINVAL;

    uint32_t txid = __sync_fetch_and_add(&g_txid_counter, 1);
    if (txid == 0) txid = __sync_fetch_and_add(&g_txid_counter, 1);

    agm_ipc_request_hdr_t req_hdr = {
        .version = AGM_IPC_PROTO_VER,
        .opcode = opcode,
        .txid = txid,
        .payload_len = in_len
    };

    agm_msg_hdr_t msg_hdr = {
        .msgType = msgType,
        .msgInSize = sizeof(req_hdr) + in_len,
        .msgOutSize = *out_len
    };

    iov_t siov[3], riov[1];
    SETIOV(&siov[0], &msg_hdr, sizeof(msg_hdr));
    SETIOV(&siov[1], &req_hdr, sizeof(req_hdr));
    int siov_count = 2;
    if (in_len > 0) {
        SETIOV(&siov[2], (void *)in_buf, in_len);
        siov_count = 3;
    }

    SETIOV(&riov[0], out_buf, *out_len);

    int rc = MsgSendv(dev->coid, siov, siov_count, riov, 1);
    if (rc == -1) {
        AGM_LOGE("MsgSendv failed: %s", strerror(errno));
        return -errno;
    }

    agm_ipc_response_hdr_t *rsp_hdr = (agm_ipc_response_hdr_t *)out_buf;
    if (*out_len < sizeof(*rsp_hdr)) {
        AGM_LOGE("Response too small");
        return -EBADMSG;
    }

    if (rsp_hdr->txid != txid) {
        AGM_LOGE("TXID mismatch");
        return -EBADMSG;
    }

    *out_len = rsp_hdr->payload_len;
    return rsp_hdr->status;
}

static void* agm_cb_thread_func(void* arg)
{
    struct timespec sleep_time = {0, 10000000};  // 10ms

    while (g_cb_thread_running) {
        if (g_active_callback_count == 0) {
            nanosleep(&sleep_time, NULL);
            continue;
        }

        pthread_mutex_lock(&g_cb_lock);
        for (uint32_t i = 0; i < AGM_CLIENT_MAX_SESSIONS; i++) {
            if (g_client_cb_data[i].mq_des != (mqd_t)-1) {
                struct mq_attr attr;
                mq_getattr(g_client_cb_data[i].mq_des, &attr);

                // Only allocate and read if messages are pending
                if (attr.mq_curmsgs == 0) continue;

                uint8_t *event_buf = malloc(attr.mq_msgsize);
                if (!event_buf) continue;

                ssize_t bytes_read = mq_receive(g_client_cb_data[i].mq_des,
                                                (char*)event_buf,
                                                attr.mq_msgsize, NULL);
                if (bytes_read > (ssize_t)sizeof(struct agm_event_cb_params) &&
                    g_client_cb_data[i].cb) {
                    struct agm_event_cb_params *event_params =
                        (struct agm_event_cb_params *)event_buf;
                    g_client_cb_data[i].cb(g_client_cb_data[i].session_id,
                                           event_params,
                                           g_client_cb_data[i].client_data);
                }
                free(event_buf);
            }
        }
        pthread_mutex_unlock(&g_cb_lock);

        nanosleep(&sleep_time, NULL);
    }
    return NULL;
}


int agm_session_register_cb(uint32_t session_id, agm_event_cb cb,
                            enum event_type evt_type, void *client_data)
{
    if (!g_agm_client) {
        AGM_LOGE("AGM client is NULL");
        return -EFAULT;
    }

    if (!cb) {
        AGM_LOGE("Callback is NULL");
        return -EINVAL;
    }

    uint32_t slot = AGM_CLIENT_MAX_SESSIONS;

    pthread_mutex_lock(&g_cb_lock);
    for (uint32_t i = 0; i < AGM_CLIENT_MAX_SESSIONS; i++) {
        if (g_client_cb_data[i].mq_des == (mqd_t)-1) {
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_cb_lock);

    if (slot == AGM_CLIENT_MAX_SESSIONS) {
        AGM_LOGE("No free slots for callback registration");
        return -ENOSPC;
    }

    // Build queue name (PID + session id)
    char mq_name[32];
    snprintf(mq_name, sizeof(mq_name), "/agm_cb_%u_%u", (unsigned)getpid(), session_id);
    mq_name[sizeof(mq_name) - 1] = '\0';

    if (mq_unlink(mq_name) == -1 && errno != ENOENT) {
        AGM_LOGE("mq_unlink(%s) failed: %s", mq_name, strerror(errno));
        return -errno;
    }

    struct mq_attr attr = {0};
    attr.mq_maxmsg  = 32;
    attr.mq_msgsize = sizeof(struct agm_event_cb_params) + 1024;

    // Guard against umask stripping write permissions
    mode_t old_umask = umask(0);

    mqd_t mq_des = mq_open(mq_name, O_CREAT | O_RDWR | O_NONBLOCK, 0666, &attr);

    umask(old_umask);

    if (mq_des == (mqd_t)-1) {
        AGM_LOGE("mq_open(%s) failed: errno=%d (%s), old_umask=%03o",
                 mq_name, errno, strerror(errno), old_umask);
        return -errno;
    }

    pthread_mutex_lock(&g_cb_lock);
    g_client_cb_data[slot].session_id  = session_id;
    g_client_cb_data[slot].cb          = cb;
    g_client_cb_data[slot].client_data = client_data;
    g_client_cb_data[slot].mq_des      = mq_des;
    memcpy(g_client_cb_data[slot].queue_name,
        mq_name, sizeof(g_client_cb_data[slot].queue_name) - 1);
    g_client_cb_data[slot].queue_name[sizeof(g_client_cb_data[slot].queue_name) - 1] = '\0';
    pthread_mutex_unlock(&g_cb_lock);

    if (!g_cb_thread_running) {
        pthread_mutex_lock(&g_cb_lock);
        if (!g_cb_thread_running) {
            pthread_t temp_thread;
            int create_result = pthread_create(&temp_thread, NULL, agm_cb_thread_func, NULL);

            if (create_result != 0) {
                AGM_LOGE("Failed to create callback thread: %s", strerror(errno));
                mq_close(mq_des);
                mq_unlink(mq_name);
                memset(&g_client_cb_data[slot], 0, sizeof(g_client_cb_data[slot]));
                g_client_cb_data[slot].mq_des = (mqd_t)-1;
                pthread_mutex_unlock(&g_cb_lock);
                return -errno;
            }

            g_cb_thread = temp_thread;
            __sync_synchronize();
            g_cb_thread_running = true;
        }
        pthread_mutex_unlock(&g_cb_lock);
    }

    // registration payload prep (session_id | evt_type | queue_name[32])
    uint8_t in_buf[sizeof(uint32_t) * 2 + 32];
    uint8_t  out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    memset(in_buf, 0, sizeof(in_buf));
    memcpy(in_buf, &session_id, sizeof(uint32_t));
    memcpy(in_buf + sizeof(uint32_t), &evt_type, sizeof(uint32_t));
    memcpy(in_buf + sizeof(uint32_t)*2, mq_name, 31);
    in_buf[sizeof(uint32_t)*2 + 31] = '\0';

    int status = agm_send_ipc_msg(g_agm_client,
                                  AGM_MSG_TYPE_SESSION_REGISTER_CB,
                                  AGM_OP_SESSION_REGISTER_CB,
                                  in_buf, sizeof(in_buf),
                                  out_buf, &out_len);

    if (status != 0) {
        AGM_LOGE("AGM_OP_SESSION_REGISTER_CB failed: %d", status);

        pthread_mutex_lock(&g_cb_lock);
        mq_close(mq_des);
        mq_unlink(mq_name);
        memset(&g_client_cb_data[slot], 0, sizeof(g_client_cb_data[slot]));
        g_client_cb_data[slot].mq_des = (mqd_t)-1;
        pthread_mutex_unlock(&g_cb_lock);

        return status;
    }

    AGM_LOGI("Registered event callback: sid=%u evt_type=%u mq=%s",
             session_id, (unsigned)evt_type, mq_name);
    return 0;
}

int agm_session_eos(uint64_t handle)
{
    if (!g_agm_client || handle == 0) {
        AGM_LOGE("Invalid client state for agm_session_eos");
        return -EINVAL;
    }

    uint32_t server_token = agm_client_get_token(handle);
    if (server_token == 0) {
        AGM_LOGE("Invalid client handle 0x%08x", handle);
        return -EINVAL;
    }

    uint8_t in_buf[sizeof(server_token)];
    memcpy(in_buf, &server_token, sizeof(server_token));

    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SESSION_EOS, AGM_OP_SESSION_EOS,
                                 in_buf, sizeof(in_buf),
                                 out_buf, &out_len);
    if (status != 0) {
        AGM_LOGE("AGM_OP_SESSION_EOS failed with error: %d", status);
    }

    AGM_LOGI("Sent EOS for client handle 0x%08x", handle);
    return status;
}

/* query processed buff count */
size_t agm_get_hw_processed_buff_cnt(uint64_t hndl, enum direction dir)
{
    if (!g_agm_client || hndl == 0) {
        AGM_LOGE("Invalid client state for hw_processed_buff_cnt");
        return -EINVAL;
    }

    uint32_t server_token = agm_client_get_token(hndl);
    if (server_token == 0) {
        AGM_LOGE("Invalid client handle 0x%08x", hndl);
        return -EINVAL;
    }

    uint8_t in_buf[sizeof(server_token) + sizeof(uint32_t)];
    memcpy(in_buf, &server_token, sizeof(server_token));
    memcpy(in_buf + sizeof(server_token), &dir, sizeof(uint32_t));

    const size_t payload_size = sizeof(size_t);
    const size_t hdr_size = sizeof(agm_ipc_response_hdr_t);
    uint8_t out_buf[sizeof(agm_ipc_response_hdr_t) + sizeof(size_t)];
    uint32_t out_len = sizeof(out_buf);

    int status = agm_send_ipc_msg(g_agm_client, AGM_MSG_TYPE_SESSION_HW_BUFF_CNT,
                                AGM_OP_GET_HW_PROCESSED_BUFF_CNT,
                                in_buf, sizeof(in_buf),
                                out_buf, &out_len);
    if (status) {
        AGM_LOGE("failed to get session buf count");
       return -EINVAL;
    }

    size_t buff_cnt;
    memcpy(&buff_cnt, out_buf + sizeof(agm_ipc_response_hdr_t), sizeof(size_t));

    AGM_LOGI("HW processed buffer count: %zu", buff_cnt);
    return buff_cnt;
}