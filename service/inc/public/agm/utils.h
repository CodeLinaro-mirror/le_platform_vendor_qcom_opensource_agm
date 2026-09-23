/*
** Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above
**     copyright notice, this list of conditions and the following
**     disclaimer in the documentation and/or other materials provided
**     with the distribution.
**   * Neither the name of The Linux Foundation nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
** WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
** ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
** BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
** BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
** WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
** OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
** IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**
** Changes from Qualcomm Innovation Center, Inc. are provided under the following license:
** Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
** SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#ifndef __UTILS_H__
#define __UTILS_H__

#include "ar_osal_error.h"
#include <string.h>

#ifdef AGM_AUDIO_LOG_UTIL_BRIDGE
#include "audio_log_util.h"
#endif

#ifdef AGM_USE_SYSLOG
#include <stdint.h>
#include <syslog.h>

#ifndef ALOGD
#define ALOGD(fmt, arg...) syslog (LOG_DEBUG, fmt, ##arg)
#endif

#ifndef ALOGI
#define ALOGI(fmt, arg...) syslog (LOG_INFO, fmt, ##arg)
#endif

#ifndef ALOGE
#define ALOGE(fmt, arg...) syslog (LOG_ERR, fmt, ##arg)
#endif

#ifndef ALOGV
#define ALOGV(fmt, arg...) syslog (LOG_DEBUG, fmt, ##arg)
#endif

#else
#include <log/log.h>
#endif

#ifndef LOG_TAG
#define LOG_TAG "AGM"
#endif

#ifdef AGM_AUDIO_LOG_UTIL_BRIDGE
#ifndef AGM_LOG_MODULE
#define AGM_LOG_MODULE MODULE_AGM_CORE
#endif
#endif

static inline const char *agm_log_file_short(const char *file)
{
    const char *p = strrchr(file, '/');

    return p ? p + 1 : file;
}

#ifdef AGM_AUDIO_LOG_UTIL_BRIDGE
#define AGM_LOGE(arg,...) audio_log_logger(BASE_LOG_ERROR, AGM_LOG_MODULE, \
                                "%s - :[%s:%d]: %s: " arg, LOG_TAG, \
                                agm_log_file_short(__FILE__), __LINE__, __func__, ##__VA_ARGS__)
#define AGM_LOGI(arg,...) audio_log_logger(BASE_LOG_INFO, AGM_LOG_MODULE, \
                                "%s - :[%s:%d]: %s: " arg, LOG_TAG, \
                                agm_log_file_short(__FILE__), __LINE__, __func__, ##__VA_ARGS__)
#define AGM_LOGD(arg,...) audio_log_logger(BASE_LOG_DEBUG, AGM_LOG_MODULE, \
                                "%s - :[%s:%d]: %s: " arg, LOG_TAG, \
                                agm_log_file_short(__FILE__), __LINE__, __func__, ##__VA_ARGS__)
#define AGM_LOGV(arg,...) audio_log_logger(BASE_LOG_VERBOSE, AGM_LOG_MODULE, \
                                "%s - :[%s:%d]: %s: " arg, LOG_TAG, \
                                agm_log_file_short(__FILE__), __LINE__, __func__, ##__VA_ARGS__)
#else
#define AGM_LOGE(arg,...) ALOGE("%s - :[%s:%d]: %s: " arg, LOG_TAG, \
                                agm_log_file_short(__FILE__), __LINE__, __func__, ##__VA_ARGS__)
#define AGM_LOGI(arg,...) ALOGI("%s - :[%s:%d]: %s: " arg, LOG_TAG, \
                                agm_log_file_short(__FILE__), __LINE__, __func__, ##__VA_ARGS__)
#ifdef AGM_LOG_DEBUG_ENABLE
#define AGM_LOGD(arg,...) ALOGD("%s - :[%s:%d]: %s: " arg, LOG_TAG, \
                                agm_log_file_short(__FILE__), __LINE__, __func__, ##__VA_ARGS__)
#define AGM_LOGV(arg,...) ALOGV("%s - :[%s:%d]: %s: " arg, LOG_TAG, \
                                agm_log_file_short(__FILE__), __LINE__, __func__, ##__VA_ARGS__)
#else
#define AGM_LOGD(arg,...) do {} while (0)
#define AGM_LOGV(arg,...) do {} while (0)
#endif
#endif

/*convert osal error codes to lnx error codes*/
int ar_err_get_lnx_err_code(uint32_t error);
/*helper to print errors in string form*/
char *ar_err_get_err_str(uint32_t error);

#endif /*__UTILS_H*/
