/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Changes from Qualcomm Innovation Center, Inc. are provided under the following license:
 *
 * Copyright (c) 2023-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 */
#ifndef GPH_MODULE_H
#define GPH_MODULE_H

#include <agm/agm_list.h>
#include <agm/device.h>

/*Platfrom Key Value file, defines tag keys and their values*/
#include "kvh2xml.h"

/*Module specific header files used.*/
#include "apm_api.h"
#include "module_cmn_api.h"
#include "wr_sh_mem_ep_api.h"
#include "rd_sh_mem_ep_api.h"
#include "common_enc_dec_api.h"
#include "hw_intf_cmn_api.h"
#include "codec_dma_api.h"
#include "i2s_api.h"
#include "pcm_tdm_api.h"
#include "slimbus_api.h"
#include "spr_api.h"
#include "gapless_api.h"
#include "pcm_encoder_api.h"
#include "aac_encoder_api.h"

/*
 *Internal enum to identify different modules
 *Graph object only should have basic modules which
 *would be implicitly configured by it.
 *For all other modules the client is supposed to form the
 *payload and pass it as an blob, which is then sent to
 *SPF via gsl.
 *NOTE:Ensure that the hw_ep_info.intf in device object
 *also uses the same enum values to define a hw
 *interface.
*/

#define TAG_STREAM_SPR       0xC0000013
#define ALIGN_PAYLOAD(x,a)   x = (((x) + (size_t)(a-1)) & ~(size_t)(a-1))
#define PARAM_VAL_NATIVE     -1

/* Channel masks for wma files */
#define WMAPRO_CHANNEL_MASK_RAW -1
#define WMAPRO_CHANNEL_MASK_ZERO 0x0000
#define WMAPRO_CHANNEL_MASK_1_C 0x0004
#define WMAPRO_CHANNEL_MASK_2_L_R 0x0003
#define WMAPRO_CHANNEL_MASK_3_L_C_R 0x0007
#define WMAPRO_CHANNEL_MASK_2_Bl_Br 0x0030
#define WMAPRO_CHANNEL_MASK_4_L_R_Bl_Br 0x0033
#define WMAPRO_CHANNEL_MASK_4_L_R_C_Bc_HT 0x0107
#define WMAPRO_CHANNEL_MASK_5_L_C_R_Bl_Br 0x0037
#define WMAPRO_CHANNEL_MASK_5_L_C_R_Sl_Sr_HT 0x0607
#define WMAPRO_CHANNEL_MASK_5DOT1_L_C_R_Bl_Br_SLF 0x003F
#define WMAPRO_CHANNEL_MASK_5DOT1_L_C_R_Sl_Sr_SLF_HT 0x060F
#define WMAPRO_CHANNEL_MASK_5DOT1_L_C_R_Bl_Br_Bc 0x0137
#define WMAPRO_CHANNEL_MASK_5DOT1_L_C_R_Sl_Sr_Bc_HT 0x0707
#define WMAPRO_CHANNEL_MASK_6DOT1_L_C_R_Bl_Br_Bc_SLF 0x013F
#define WMAPRO_CHANNEL_MASK_6DOT1_L_C_R_Sl_Sr_Bc_SLF_HT 0x070F
#define WMAPRO_CHANNEL_MASK_6DOT1_L_C_R_Bl_Br_SFLOC_SFROC 0x00F7
#define WMAPRO_CHANNEL_MASK_6DOT1_L_C_R_Sl_Sr_SFLOC_SFROC_HT 0x0637
#define WMAPRO_CHANNEL_MASK_7DOT1_L_C_R_Bl_Br_SLF_SFLOC_SFROC 0x00FF
#define WMAPRO_CHANNEL_MASK_7DOT1_L_C_R_Ls_Rs_SLF_SFLOC_SFROC_HT 0x063F

typedef enum module
{
    MODULE_STREAM_START = 0,
    MODULE_PCM_ENCODER = MODULE_STREAM_START,
    MODULE_PCM_DECODER,
    MODULE_PCM_CONVERTER,
    MODULE_WR_SHARED_MEM,
    MODULE_PLACEHOLDER_ENCODER,
    MODULE_PLACEHOLDER_DECODER,
    MODULE_STREAM_PAUSE,
    MODULE_STREAM_SPR,
    MODULE_STREAM_GAPLESS,
    MODULE_RD_SHARED_MEM,
    MODULE_STREAM_MFC,
    /*
     *Ensure that whenever a new stream module is added it
     *is added in the end of stream module list and the end
     *is updated with the same entry.
     */
    MODULE_STREAM_END = MODULE_STREAM_MFC,
    MODULE_DEVICE_START = 0,
    MODULE_HW_EP_RX = MODULE_DEVICE_START,
    MODULE_HW_EP_TX,
    /*
     *Ensure that whenever a new device module is added it
     *is added in the end of device module list and the end
     *is updated with the same entry.
     */
    MODULE_DEVICE_END = MODULE_HW_EP_RX,
}module_t;


enum  channel_num{
    CHANNEL_1 = 1,
    CHANNEL_2,
    CHANNEL_3,
    CHANNEL_4,
    CHANNEL_5,
    CHANNEL_6,
    CHANNEL_7,
    CHANNEL_8,
    CHANNEL_10 = 10,
    CHANNEL_12 = 12,
    CHANNEL_14 = 14,
    CHANNEL_16 = 16,
    CHANNEL_32 = 32,
};

struct module_info {
    struct listnode list;
    /*local enum based module identification*/
    module_t module;
    /*module tag defined in the platform header file exported to ACDB*/
    uint32_t tag;
    /*module id querried from ACDB*/
    uint32_t mid;
    /*module instance id querried from ACDB*/
    uint32_t miid;
    /*indicates if this module instance was configured*/
    bool is_configured;
    /*Device object if this module is associated with an hardware end point*/
    struct device_obj *dev_obj;
    /*GKV which contains/describes this module*/
    struct agm_key_vector_gsl *gkv;
    /*
     *Every module defines its configuration api, which in turn
     *then is used by graph object to configure the module using
     *gsl_set_config/gsl_set_custom_config api's.
     */
    int (*configure)(struct module_info *mod, struct graph_obj *gph_obj);
};

typedef struct module_info module_info_t;

struct graph_buf_info {
    /* timestamp updated in struct gsl_buff on gsl_read */
    uint64_t timestamp;
};

struct graph_obj {
    pthread_mutex_t lock;
    pthread_mutex_t gph_open_thread_lock;
    pthread_t gph_open_thread;
    pthread_cond_t gph_opened;
    bool gph_open_thread_created;
    graph_state_t state;
    gsl_handle_t graph_handle;
    struct listnode tagged_mod_list;
    struct gsl_cmd_configure_read_write_params buf_config;
    event_cb cb;
    void *client_data;
    struct session_obj *sess_obj;
    uint32_t spr_miid;
    struct graph_buf_info buf_info;
    bool is_config_buf_params_done;
};

void get_stream_module_list_array(module_info_t **info, size_t *size);
void get_hw_ep_module_list_array(module_info_t **info, size_t *size);
bool compare_tdm_custom_config(param_id_tdm_intf_cfg_t *tdm_config_p1, param_id_tdm_intf_cfg_t *tdm_config_p2);
bool compare_hw_ep_media_config(param_id_hw_ep_mf_t *hw_ep_media_conf_p1, param_id_hw_ep_mf_t *hw_ep_media_conf_p2);

#endif /*GPH_MODULE_H*/
