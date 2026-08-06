
/*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
    SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#ifndef AGM_SERVER_PROXY_H
#define AGM_SERVER_PROXY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mqueue.h>

#ifdef __cplusplus
extern "C" {
#endif

/* QNX resource manager types */
#include <sys/dispatch.h>
#include <sys/resmgr.h>
#include <sys/iofunc.h>

/* Protocol version */
#define AGM_IPC_PROTO_VER 1u
#define AGM_COMPONENT_NAME "/dev/agmProxy"

/* Size limits */
#define AGM_PROXY_MAX_MSG_SIZE   (128u * 1024u)
#define AGM_PROXY_MAX_PAYLOAD    (AGM_PROXY_MAX_MSG_SIZE - 16u)
#define AGM_PROXY_MAX_SESSIONS   128u
#define AGM_PROXY_MSG_CODE_START 6000
#define AGM_PROXY_MSG_CODE_MAX   6050

/* Message type to opcode mapping */
#define AGM_MSG_TYPE_INIT                            (AGM_PROXY_MSG_CODE_START)
#define AGM_MSG_TYPE_DEINIT                          (AGM_PROXY_MSG_CODE_START + 1)
#define AGM_MSG_TYPE_SESSION_OPEN                    (AGM_PROXY_MSG_CODE_START + 2)
#define AGM_MSG_TYPE_SESSION_PREPARE                 (AGM_PROXY_MSG_CODE_START + 3)
#define AGM_MSG_TYPE_SESSION_START                   (AGM_PROXY_MSG_CODE_START + 4)
#define AGM_MSG_TYPE_SESSION_STOP                    (AGM_PROXY_MSG_CODE_START + 5)
#define AGM_MSG_TYPE_SESSION_CLOSE                   (AGM_PROXY_MSG_CODE_START + 6)
#define AGM_MSG_TYPE_SESSION_PAUSE                   (AGM_PROXY_MSG_CODE_START + 7)
#define AGM_MSG_TYPE_SESSION_RESUME                  (AGM_PROXY_MSG_CODE_START + 8)
#define AGM_MSG_TYPE_SESSION_WRITE                   (AGM_PROXY_MSG_CODE_START + 9)
#define AGM_MSG_TYPE_SESSION_READ                    (AGM_PROXY_MSG_CODE_START + 10)

#define AGM_MSG_TYPE_GET_AIF_INFO_LIST                (AGM_PROXY_MSG_CODE_START + 11)
#define AGM_MSG_TYPE_SET_STREAM_METADATA             (AGM_PROXY_MSG_CODE_START + 12)
#define AGM_MSG_TYPE_SET_DEVICE_METADATA             (AGM_PROXY_MSG_CODE_START + 13)
#define AGM_MSG_TYPE_SET_DEVICE_PP_METADATA          (AGM_PROXY_MSG_CODE_START + 14)
#define AGM_MSG_TYPE_SESSION_AIF_CONNECT             (AGM_PROXY_MSG_CODE_START + 15)
#define AGM_MSG_TYPE_SET_MEDIA_CONFIG                (AGM_PROXY_MSG_CODE_START + 16)
#define AGM_MSG_TYPE_SET_SESSION_CONFIG              (AGM_PROXY_MSG_CODE_START + 17)

#define AGM_MSG_TYPE_SESSION_REGISTER_CB             (AGM_PROXY_MSG_CODE_START + 19)
#define AGM_MSG_TYPE_SESSION_EOS                     (AGM_PROXY_MSG_CODE_START + 21)
#define AGM_MSG_TYPE_SESSION_HW_BUFF_CNT             (AGM_PROXY_MSG_CODE_START + 24)
#define AGM_MSG_TYPE_AGM_GET_AIF_INFO_LIST           (AGM_PROXY_MSG_CODE_START + 25)
#define AGM_MSG_TYPE_AGM_GET_GROUP_AIF_INFO_LIST     (AGM_PROXY_MSG_CODE_START + 26)
#define AGM_MSG_TYPE_GROUP_SET_MEDIA_CONFIG          (AGM_PROXY_MSG_CODE_START + 27)
#define AGM_MSG_TYPE_SET_PARAMS_ACDB_TUNNEL          (AGM_PROXY_MSG_CODE_START + 28)
#define AGM_MSG_TYPE_GET_PARAMS_ACDB_TUNNEL          (AGM_PROXY_MSG_CODE_START + 29)
#define AGM_MSG_TYPE_SESSION_AIF_SET_CAL           (AGM_PROXY_MSG_CODE_START + 30)
#define AGM_MSG_TYPE_AIF_SET_PARAMS           (AGM_PROXY_MSG_CODE_START + 31)
#define AGM_MSG_TYPE_SESSION_GET_PARAMS           (AGM_PROXY_MSG_CODE_START + 32)
#define AGM_MSG_TYPE_SESSION_GET_BUF_INFO           (AGM_PROXY_MSG_CODE_START + 33)
#define AGM_MSG_TYPE_SESSION_SET_LOOPBACK           (AGM_PROXY_MSG_CODE_START + 34)
#define AGM_MSG_TYPE_SESSION_SET_EC_REF           (AGM_PROXY_MSG_CODE_START + 35)
#define AGM_MSG_TYPE_GET_BUFFER_TIMESTAMP           (AGM_PROXY_MSG_CODE_START + 36)


/* Opcodes for AGM operations (stable IDs) */
typedef enum {
    AGM_OP_INIT                            = 1,
    AGM_OP_DEINIT                          = 2,
    AGM_OP_SESSION_OPEN                    = 100,
    AGM_OP_SESSION_PREPARE                 = 102,
    AGM_OP_SESSION_START                   = 103,
    AGM_OP_SESSION_STOP                    = 104,
    AGM_OP_SESSION_CLOSE                   = 105,
    AGM_OP_SESSION_PAUSE                   = 106,
    AGM_OP_SESSION_RESUME                  = 107,
    AGM_OP_SESSION_WRITE                   = 108,
    AGM_OP_SESSION_READ                    = 109,
    AGM_OP_SESSION_REGISTER_CB             = 110,
    AGM_OP_SESSION_AIF_SET_METADATA        = 111,

    AGM_OP_GET_AIF_INFO_LIST                 = 122,
    AGM_OP_STREAM_METADATA                   = 123,
    AGM_OP_DEVICE_METADATA                   = 124,
    AGM_OP_DEVICE_PP_METADATA                = 125,
    AGM_OP_SESSION_AIF_CONNECT               = 126,
    AGM_OP_SET_MEDIA_CONFIG                  = 127,
    AGM_OP_SET_SESSION_CONFIG                = 128,
    AGM_OP_AGM_GET_AIF_INFO_LIST             = 129,
    AGM_OP_AGM_GET_GROUP_AIF_INFO_LIST       = 130,
    AGM_OP_GROUP_SET_MEDIA_CONFIG            = 131,

    AGM_OP_SESSION_EOS                       = 134,
    AGM_OP_GET_HW_PROCESSED_BUFF_CNT         = 133,
    AGM_OP_SET_PARAMS_ACDB_TUNNEL            = 135,
    AGM_OP_GET_PARAMS_ACDB_TUNNEL            = 136,
    AGM_OP_SESSION_AIF_SET_CAL               =137,
    AGM_OP_AIF_SET_PARAMS                    =138,
    AGM_OP_SESSION_GET_PARAMS                =139,
    AGM_OP_SESSION_GET_BUF_INFO              =140,
    AGM_OP_SESSION_SET_LOOPBACK              =141,
    AGM_OP_SESSION_SET_EC_REF                =142,
    AGM_OP_GET_BUFFER_TIMESTAMP              =143,

} agm_ipc_opcode_t;

/* Request/Response Envelopes */
#pragma pack(push, 1)
typedef struct {
    uint32_t version;      /* protocol version */
    uint32_t opcode;       /* request opcode */
    uint32_t txid;         /* transaction id */
    uint32_t payload_len;  /* length in bytes of payload following header */
} agm_ipc_request_hdr_t;

_Static_assert(sizeof(agm_ipc_request_hdr_t) == 16, "agm_ipc_request_hdr_t size mismatch");

typedef struct {
    uint32_t txid;         /* transaction id (echoed) */
    int32_t  status;       /* 0 success, -errno on failure */
    uint32_t payload_len;  /* length of payload following this header */
} agm_ipc_response_hdr_t;

/* new structure */
typedef struct {
    uint16_t msgType;      // MUST be first field in the message
    uint32_t msgInSize;    // Size of the input payload
    uint32_t msgOutSize;   // Size of expected output
    uint32_t reserved;     // Can be used for handle or other info
} agm_msg_hdr_t;

_Static_assert(sizeof(agm_ipc_response_hdr_t) == 12, "agm_ipc_response_hdr_t size mismatch");
#pragma pack(pop)


/* Single devctl command that carries the request envelope + payload */
#ifndef DCMD_AGM_REQUEST
#include <sys/ioctl.h>
#define DCMD_AGM_REQUEST  __DIOTF(_DCMD_MISC, 0xA7, uint8_t)
#endif

/* Optional vtable placeholder to mirror csd2/awe pattern */
struct agm_mq_vtable { int unused; };

/* Global server context similar to csd2/awe servers */
typedef struct {
    resmgr_connect_funcs_t  rm_cfuncs;
    resmgr_io_funcs_t       rm_iofuncs;
    resmgr_attr_t           rm_attr;
    iofunc_attr_t           rm_io_attr;
    int                     rm_pathID;
    dispatch_t             *dpp;
    dispatch_context_t     *ctp;
    thread_pool_t          *tpp;
    int                     pid;
    message_attr_t          agm_msg_attr;
    void                   *mq_hdl;
    struct agm_mq_vtable    mq_fn_tbl;
} agm_server_ctx_t;

/* Lifecycle API */
int agm_server_proxy_init(void);
int agm_server_proxy_start(void);
int agm_server_proxy_stop(void);
int agm_server_proxy_deinit(void);

/* Message parser API */
int agm_msg_parser_validate(const agm_ipc_request_hdr_t* hdr, size_t msg_len);
int agm_msg_parser_dispatch(uint32_t opcode,
                            const uint8_t* in, uint32_t in_len,
                            uint8_t* out, uint32_t* out_len);
void agm_server_cb_data_init(void);
void init_handle_table(void);
//void agm_msg_parser_set_current_scoid(int scoid);
void agm_msg_parser_set_current_txid(uint32_t txid);
#ifdef __cplusplus
}
#endif

#endif /* AGM_SERVER_PROXY_H */