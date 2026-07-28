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

#include <sys/neutrino.h>
#include <sys/iomsg.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <pthread.h>
#include<time.h>

#include <../inc/agm_server_proxy.h>


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


static bool running = false;
/* Parser interface (implemented in agm_msg_parser.c) */
extern int agm_msg_parser_validate(const agm_ipc_request_hdr_t* hdr, size_t msg_len);
extern int agm_msg_parser_dispatch(uint32_t opcode,
                                   const uint8_t* in, uint32_t in_len,
                                   uint8_t* out, uint32_t* out_len);
extern void agm_msg_parser_set_current_scoid(int scoid);
extern void agm_msg_parser_cleanup_scoid(int scoid);

agm_server_ctx_t g_agm_srv_ctx;


/* -------------------- message_attach handler -------------------- */
static int agm_msg_handler(message_context_t *ctp, int code __unused, unsigned flags __unused, void *handle __unused) {
    if (ctp == NULL) {
        AGM_LOGE("agm_msg_handler: FATAL - ctp is NULL!");
        return 0;
    }

    int rcvid = ctp->rcvid;
    uint8_t *in_buf = NULL;
    uint8_t *rsp_buf = NULL;
    int ret = 0;

    AGM_LOGI("agm_msg_handler: ENTER - rcvid=%d, scoid=0x%08x",
             rcvid, ctp->info.scoid);

    if (rcvid <= 0) {
        /* pulse or error; nothing to do */
        AGM_LOGE("Ignoring non-message event: rcvid=%d (code=%d)",
                 rcvid, code);
        return 0;
    }

    /* Make scoid visible to parser for per-client session bookkeeping */
    agm_msg_parser_set_current_scoid(ctp->info.scoid);
    AGM_LOGI("Processing IPC message: scoid=0x%08x, rcvid=%d",
             ctp->info.scoid, rcvid);

    /* First, read the QNX message header (MUST be first in message) */
    agm_msg_hdr_t *msg_hdr = (agm_msg_hdr_t *)ctp->msg;

    AGM_LOGI("QNX Message header: msgType=0x%04X, msgInSize=%u, msgOutSize=%u",
             msg_hdr->msgType, msg_hdr->msgInSize, msg_hdr->msgOutSize);

    /* Validate message type is in our expected range */
    if (msg_hdr->msgType < AGM_PROXY_MSG_CODE_START ||
        msg_hdr->msgType > AGM_PROXY_MSG_CODE_MAX) {
        AGM_LOGE("INVALID MESSAGE TYPE - "
                 "scoid=0x%08x, rcvid=%d, msgType=0x%04X (expected range: 0x%04X-0x%04X)",
                 ctp->info.scoid, rcvid, msg_hdr->msgType,
                 AGM_PROXY_MSG_CODE_START, AGM_PROXY_MSG_CODE_MAX);
        MsgError(rcvid, ENOSYS);
        return 0;
    }

    /* Now read our protocol header which follows the QNX message header */
    agm_ipc_request_hdr_t req_hdr;

    /* Read the protocol header from message body */
    int r = resmgr_msgread(ctp, &req_hdr, sizeof(req_hdr), sizeof(agm_msg_hdr_t));

    if (r < 0 || r != sizeof(req_hdr)) {
        AGM_LOGE("----PROTOCOL HEADER READ FAILED - "
                 "scoid=0x%08x, rcvid=%d, msgType=0x%04X, "
                 "errno=%d (%s), bytes_read=%d",
                 ctp->info.scoid, rcvid, msg_hdr->msgType,
                 errno, strerror(errno), r);
        MsgError(rcvid, EFAULT);
        return 0;
    }

    AGM_LOGI("Request header parsed: ver=%u, op=0x%08X, txid=%u, payload_len=%u",
             req_hdr.version, req_hdr.opcode, req_hdr.txid, req_hdr.payload_len);

    if (req_hdr.payload_len > AGM_PROXY_MAX_PAYLOAD) {
        AGM_LOGE("PAYLOAD EXCEEDS MAX LIMIT - "
                 "scoid=0x%08x, rcvid=%d, op=0x%08X, "
                 "payload_len=%u (max=%u), txid=%u",
                 ctp->info.scoid, rcvid, req_hdr.opcode,
                 req_hdr.payload_len, AGM_PROXY_MAX_PAYLOAD, req_hdr.txid);
        MsgError(rcvid, EMSGSIZE);
        return 0;
    }

    if (req_hdr.payload_len > 0) {
        in_buf = malloc(req_hdr.payload_len);
        if (!in_buf) {
            AGM_LOGE("Memory allocation failed for input payload (%u bytes)", req_hdr.payload_len);
            MsgError(rcvid, ENOMEM);
            return 0;
        }

        /* Read payload from message body (offset is size of both headers) */
        r = resmgr_msgread(ctp, in_buf, req_hdr.payload_len,
                          sizeof(agm_msg_hdr_t) + sizeof(agm_ipc_request_hdr_t));
        if (r < 0) {
            AGM_LOGE("PAYLOAD READ FAILED - "
                     "scoid=0x%08x, rcvid=%d, op=0x%08X, "
                     "payload_len=%u, errno=%d (%s)",
                     ctp->info.scoid, rcvid, req_hdr.opcode,
                     req_hdr.payload_len, errno, strerror(errno));
            free(in_buf);
            MsgError(rcvid, EFAULT);
            return 0;
        }
        AGM_LOGI("Payload read: %d bytes", r);
    }

    /* Validate total message length (envelope + payload) */
    size_t total_len = sizeof(agm_ipc_request_hdr_t) + (size_t)req_hdr.payload_len;
    AGM_LOGI("Validating message: total_len=%zu (hdr=%zu + payload=%u)",
             total_len, sizeof(agm_ipc_request_hdr_t), req_hdr.payload_len);

    int vret = agm_msg_parser_validate(&req_hdr, total_len);

    rsp_buf = malloc(AGM_PROXY_MAX_MSG_SIZE);
    if (!rsp_buf) {
        AGM_LOGE("Memory allocation failed for response buffer (%u bytes)", AGM_PROXY_MAX_MSG_SIZE);
        ret = -ENOMEM;
        goto cleanup;
    }

    agm_ipc_response_hdr_t* rsp = (agm_ipc_response_hdr_t*)rsp_buf;
    uint8_t* out_payload = rsp_buf + sizeof(agm_ipc_response_hdr_t);
    uint32_t out_cap = AGM_PROXY_MAX_MSG_SIZE - (uint32_t)sizeof(agm_ipc_response_hdr_t);
    uint32_t out_len = out_cap;

    rsp->txid = req_hdr.txid;

    if (vret != 0) {
        AGM_LOGE("INVALID REQUEST - "
                 "scoid=0x%08x, rcvid=%d, op=0x%08X, "
                 "txid=%u, payload_len=%u, "
                 "version=%u, vret=%d (%s)",
                 ctp->info.scoid, rcvid, req_hdr.opcode,
                 req_hdr.txid, req_hdr.payload_len,
                 req_hdr.version, vret, strerror(-vret));
        rsp->status = vret;
        rsp->payload_len = 0;

        (void)resmgr_msgreply(ctp, rsp_buf, sizeof(agm_ipc_response_hdr_t));
        AGM_LOGE("Sent validation error response: status=%d", vret);
        ret = 0;
        goto cleanup;
    }

    int hret = agm_msg_parser_dispatch(req_hdr.opcode, in_buf, req_hdr.payload_len,
                                      out_payload, &out_len);
    rsp->status = hret;

    if (hret == 0) {
        rsp->payload_len = out_len;
    uint32_t max_response_size = msg_hdr->msgOutSize;
    uint32_t actual_response_size = sizeof(agm_ipc_response_hdr_t) + out_len;
    uint32_t response_size = (actual_response_size > max_response_size) ?
                            max_response_size : actual_response_size;

    (void)resmgr_msgreply(ctp, rsp_buf, response_size);

    } else {
        AGM_LOGE("HANDLER FAILED - "
                 "scoid=0x%08x, rcvid=%d, op=0x%08X, "
                 "txid=%u, status=%d (%s), out_len=%u",
                 ctp->info.scoid, rcvid, req_hdr.opcode,
                 req_hdr.txid, hret, strerror(-hret), out_len);
        rsp->payload_len = 0;
        (void)resmgr_msgreply(ctp, rsp_buf, sizeof(agm_ipc_response_hdr_t));
    }
    ret = 0;

cleanup:
    if (in_buf) {
        free(in_buf);
    }

    if (rsp_buf) {
        free(rsp_buf);
    }

    AGM_LOGI("agm_msg_handler: EXIT - scoid=0x%08x, rcvid=%d, op=0x%08X, txid=%u, ret=%d",
             ctp->info.scoid, rcvid, req_hdr.opcode, req_hdr.txid, ret);

    return ret;
}

/* -------------------- close_dup: per-client cleanup -------------------- */
static int agm_dup_io_cleanup(resmgr_context_t *ctp, io_close_t *msg __unused, RESMGR_OCB_T *ocb __unused)
{

    AGM_LOGD("AGM close_dup_updated: rcvid=%ld, scoid=%d", (long)ctp->rcvid, ctp->info.scoid);
    agm_msg_parser_cleanup_scoid(ctp->info.scoid);
    return iofunc_close_dup_default(ctp, msg, ocb);
}


/* -------------------- lifecycle -------------------- */
int agm_server_proxy_init(void)
{
    /*Reset server state:  free slot handling
      ensure all g_server_cb_data[] slots start clean (mq_des = -1)
    */
    init_handle_table();

    agm_server_cb_data_init(); // callback (mqueue) slots: mark as unused

    if (g_agm_srv_ctx.dpp) return 0;

    g_agm_srv_ctx.dpp = dispatch_create();
    if (!g_agm_srv_ctx.dpp) {
        AGM_LOGE("dispatch_create failed");
        return -errno;
    }
    g_agm_srv_ctx.pid = getpid();

    /* Resource manager setup */
    memset(&g_agm_srv_ctx.rm_attr, 0, sizeof(g_agm_srv_ctx.rm_attr));
    g_agm_srv_ctx.rm_attr.nparts_max = 4;
    g_agm_srv_ctx.rm_attr.msg_max_size = AGM_PROXY_MAX_MSG_SIZE;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_agm_srv_ctx.rm_cfuncs,
                     _RESMGR_IO_NFUNCS, &g_agm_srv_ctx.rm_iofuncs);
    iofunc_attr_init(&g_agm_srv_ctx.rm_io_attr, S_IFCHR | 0666, NULL, NULL);

    g_agm_srv_ctx.rm_iofuncs.close_dup = agm_dup_io_cleanup;

    /* Attach device name */
    AGM_LOGI("agm_server_proxy_init start");
    g_agm_srv_ctx.rm_pathID = resmgr_attach(g_agm_srv_ctx.dpp, &g_agm_srv_ctx.rm_attr,
                                            AGM_COMPONENT_NAME, _FTYPE_ANY, _RESMGR_FLAG_SELF,
                                            &g_agm_srv_ctx.rm_cfuncs,
                                            &g_agm_srv_ctx.rm_iofuncs,
                                            &g_agm_srv_ctx.rm_io_attr);
    if (g_agm_srv_ctx.rm_pathID == -1) {
        AGM_LOGE("resmgr_attach failed for %s: %d", AGM_COMPONENT_NAME, errno);
        dispatch_destroy(g_agm_srv_ctx.dpp);
        g_agm_srv_ctx.dpp = NULL;
        return -errno;
    }

    /* Attach custom message range */
    memset(&g_agm_srv_ctx.agm_msg_attr, 0, sizeof(g_agm_srv_ctx.agm_msg_attr));
    g_agm_srv_ctx.agm_msg_attr.nparts_max   = 4;
    g_agm_srv_ctx.agm_msg_attr.msg_max_size = sizeof(agm_ipc_request_hdr_t);

    if (message_attach(g_agm_srv_ctx.dpp, &g_agm_srv_ctx.agm_msg_attr,
                       AGM_PROXY_MSG_CODE_START, AGM_PROXY_MSG_CODE_MAX,
                       &agm_msg_handler, (void*)&g_agm_srv_ctx) == -1)
    {
        AGM_LOGE("message_attach failed: errno=%d", errno);
        resmgr_detach(g_agm_srv_ctx.dpp, g_agm_srv_ctx.rm_pathID, _RESMGR_DETACH_ALL);
        dispatch_destroy(g_agm_srv_ctx.dpp);
        g_agm_srv_ctx.dpp = NULL;
        return -errno;
    }
    AGM_LOGI("agm_server_proxy_init start1 after msg_attch");

    /* Thread pool setup */
    thread_pool_attr_t pool_attr;
    memset(&pool_attr, 0, sizeof(pool_attr));
    pool_attr.handle        = g_agm_srv_ctx.dpp;
    pool_attr.context_alloc = (void*)dispatch_context_alloc;
    pool_attr.block_func    = (void*)dispatch_block;
    pool_attr.unblock_func  = (void*)dispatch_unblock;
    pool_attr.handler_func  = (void*)dispatch_handler;
    pool_attr.context_free  = (void*)dispatch_context_free;

    pool_attr.lo_water      = 8;
    pool_attr.hi_water      = 12;
    pool_attr.increment     = 5;
    pool_attr.maximum       = 16;
    pool_attr.tid_name      = "agm_dispatcher";

    g_agm_srv_ctx.tpp = thread_pool_create(&pool_attr, 0);
    if (!g_agm_srv_ctx.tpp) {
        AGM_LOGE("thread_pool_create failed");
        resmgr_detach(g_agm_srv_ctx.dpp, g_agm_srv_ctx.rm_pathID, _RESMGR_DETACH_ALL);
        dispatch_destroy(g_agm_srv_ctx.dpp);
        g_agm_srv_ctx.dpp = NULL;
        return -errno;
    }

    thread_pool_start(g_agm_srv_ctx.tpp);
    AGM_LOGI("AGM hybrid server attached at %s", AGM_COMPONENT_NAME);

    return 0;
}

int agm_ipc_server_proxy_start(void)
{
    int rc = agm_server_proxy_init();
    if (rc) {
        AGM_LOGE("failed to agm_server_proxy_init");
        return rc;
    }
    running = true;

    int ret = agm_init();
    if (ret){
        AGM_LOGE("failed to agm_init");
        return -errno;
    }
    return 0;
}

int agm_server_proxy_stop(void)
{
    running = false;

    if (g_agm_srv_ctx.tpp) {
        thread_pool_destroy(g_agm_srv_ctx.tpp);
        g_agm_srv_ctx.tpp = NULL;
    }
    if (g_agm_srv_ctx.dpp) {
        /* Detach device node */
        if (g_agm_srv_ctx.rm_pathID >= 0) {
            (void)resmgr_detach(g_agm_srv_ctx.dpp, g_agm_srv_ctx.rm_pathID, _RESMGR_DETACH_ALL);
            g_agm_srv_ctx.rm_pathID = -1;
        }
        dispatch_destroy(g_agm_srv_ctx.dpp);
        g_agm_srv_ctx.dpp = NULL;
    }
    g_agm_srv_ctx.ctp = NULL;

    AGM_LOGI("AGM server stopped");
    return 0;
}

int agm_server_proxy_deinit(void)
{
    return agm_server_proxy_stop();
}