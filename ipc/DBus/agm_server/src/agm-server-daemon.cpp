/*
** Copyright (c) 2020, The Linux Foundation. All rights reserved.
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
**/

/*
** Changes from Qualcomm Innovation Center are provided under the following license:
** Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted (subject to the limitations in the
** disclaimer below) provided that the following conditions are met:
**
**    * Redistributions of source code must retain the above copyright
**      notice, this list of conditions and the following disclaimer.
**
**    * Redistributions in binary form must reproduce the above
**      copyright notice, this list of conditions and the following
**      disclaimer in the documentation and/or other materials provided
**      with the distribution.
**
**    * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
**      contributors may be used to endorse or promote products derived
**      from this software without specific prior written permission.
**
** NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
** GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
** HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
** WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
** MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
** ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
** GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
** IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
** OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
** IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**/

#define LOG_TAG "agm_server_daemon"

#include <glib.h>
#include <signal.h>
#include <stdio.h>

#ifdef ARE_ON_APPS
#include <posal.h>
#include <spf_main.h>
#include <gpr_api.h>
#endif

#include "agm_server_wrapper_dbus.h"
#include <utils.h>

GMainLoop *mainloop = NULL;

void signal_handler(int sig) {
    int ret = 0;
    switch (sig) {
        case SIGINT:
        case SIGTERM:
        case SIGABRT:
        case SIGQUIT:
        case SIGKILL:
        default:
            AGM_LOGE("Terminating signal received\n");

/*
 * ToDo: To use ref count based approach for gpr when are_on_apps supports
 * bootup laoding of dynamic modules.
 *
 * In current approach spf framework deinit calls are done before
 * session_obj_deinit() as session_obj_deinit() calls gpr_deinit() function and
 * framework services need to de-register from gpr as part of deinit call flow.
 *
 * In case when ARE on APPS support bootup loading for dynamic modules, spf
 * framework deinit need to be called after session_obj_deinit() to handle AMDB
 * commands to deregister dynamic modules. As gpr_deinit() is called as part of
 * session_obj_deinit() call flow, need to use ref count based approach for gpr
 * and the last deinit call to gpr should be after session object and spf
 * framework are deinitialized.
 */
#ifdef ARE_ON_APPS
            ret = spf_framework_pre_deinit();
            if (0 != ret) {
                AGM_LOGE("spf_framework_pre_deinit() failed with status %d", ret);
                return;
            }

            ret = spf_framework_post_deinit();
            if (0 != ret) {
                AGM_LOGE("spf_framework_post_deinit() failed with status %d", ret);
                return;
            }

            posal_deinit();
#endif

            ipc_agm_deinit();
            g_main_loop_quit(mainloop);
            break;
    }
}

int main() {
    int rc = 0;
    mainloop = g_main_loop_new(NULL, false);

#ifdef ARE_ON_APPS
    posal_init();
    rc = gpr_init();
    if (0 != rc) {
        AGM_LOGE("gpr_init() failed with status %d", rc);
        return rc;
    }

    rc = spf_framework_pre_init();
    if (0 != rc) {
        AGM_LOGE("spf_framework_pre_init() failed with status %d", rc);
        return rc;
    }

    rc = spf_framework_post_init();
    if (0 != rc) {
        AGM_LOGE("spf_framework_post_init() failed with status %d", rc);
        return rc;
    }
#endif
    rc = ipc_agm_init();
    if (rc != 0) {
        AGM_LOGE("AGM init failed\n");
        return rc;
    }

    AGM_LOGD("agm init done\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);
    signal(SIGKILL, signal_handler);
    signal(SIGABRT, signal_handler);

    g_main_loop_run(mainloop);
    return 0;
}
