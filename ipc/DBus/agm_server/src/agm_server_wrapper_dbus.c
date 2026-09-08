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
** Changes from Qualcomm Technologies, Inc. are provided under the following license:
** Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
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

#define LOG_TAG "AGM: ipc_server"

#include <dbus/dbus.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <agm/agm_api.h>
#include "agm-dbus-utils.h"
#include "agm_server_wrapper_dbus.h"

#include "agm/utils.h"

#define AGM_OBJECT_PATH "/org/qti/agm"
#define AGM_MODULE_IFACE "org.Qti.Agm"
#define AGM_SESSION_IFACE "org.Qti.Agm.Session"
#define AGM_DBUS_CONNECTION "org.Qti.AgmService"

/* Matches the GSL ADSP IPC timeout that unblocks graph_read/write and the
 * client-side AGM_DBUS_ASYNC_CALL_TIMEOUT_MS ack budget. The worker is
 * expected to return within this window in all non-catastrophic cases. */
#define AGM_WORKER_JOIN_TIMEOUT_MS 1000

/* Module Level data */
typedef struct {
    /* Dbus path where agm module listens for connections */
    char *dbus_obj_path;
    /* Dbus connection */
    agm_dbus_connection *conn;
    /* Hashmap containing all sessions info */
    GHashTable *sessions;
    /* spf_mem_handle -> dup'ed fd kept alive for nonfd client path */
    GHashTable *shmem_fd_map;
} agm_module_dbus_data;

/* Session specific data */
typedef struct {
    /* Session id */
    uint32_t session_id;
    /* Session handle */
    uint64_t handle;
    /* Dbus path for session specific APIs */
    char *dbus_obj_path;
    /* List which maintains all the callbacks associated with a session id.
       Used to de-register callbacks when client dies abruptly */
    GList *callbacks;

    void *buf;
    uint32_t buf_size;
    int thread_state;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t ses_tid;
    char eventType[50];
    /* D-Bus unique name of the client that opened this session (":1.NN").
       Used by the NameOwnerChanged filter to clean up when the client
       disappears without calling AgmSessionClose (e.g. SIGKILL). */
    char *owner_unique_name;
} agm_session_data;

typedef struct {
    uint32_t session_id;
    uint32_t event_type;
    uint64_t client_data;
} agm_callback_data;

static agm_module_dbus_data *mdata = NULL;

static bool agm_fd_is_mmap_candidate(int fd)
{
    struct stat st;

    if ((fd < 0) || (fcntl(fd, F_GETFD) == -1))
        return false;
    if (fstat(fd, &st) != 0)
        return false;

    return !S_ISSOCK(st.st_mode);
}

static void agm_shmem_fd_close_value(gpointer data)
{
    int *fdp = (int *)data;

    if (!fdp)
        return;
    if (*fdp >= 0)
        close(*fdp);
    free(fdp);
}

static int agm_shmem_fd_track_add(uint32_t spf_mem_handle, int fd)
{
    int *fdp = NULL;

    if (!mdata || !mdata->shmem_fd_map || fd < 0)
        return -EINVAL;

    fdp = (int *)calloc(1, sizeof(*fdp));
    if (!fdp)
        return -ENOMEM;

    *fdp = fd;
    g_hash_table_replace(mdata->shmem_fd_map,
                         GUINT_TO_POINTER(spf_mem_handle), fdp);
    return 0;
}

static void agm_shmem_fd_track_remove(uint32_t spf_mem_handle)
{
    if (!mdata || !mdata->shmem_fd_map)
        return;

    g_hash_table_remove(mdata->shmem_fd_map, GUINT_TO_POINTER(spf_mem_handle));
}

enum AgmModuleMethods {
    AgmAifSetMediaConfig,
    AgmAifSetMetadata,
    AgmSessionAifSetMetadata,
    AgmSessionSetMetadata,
    AgmSessionSetLoopback,
    AgmSetParamsWithTag,
    AgmGetAifInfoListSize,
    AgmGetAifInfoList,
    AgmSessionSetParams,
    AgmSessionRegisterForEvents,
    AgmSessionRegisterCb,
    AgmSessionDeRegisterCb,
    AgmSessionSetEcRef,
    AgmSessionAifConnect,
    AgmSessionAifGetTagModuleInfo,
    AgmSessionAifGetTagModuleInfoSize,
    AgmSessionAifSetParams,
    AgmAifSetParams,
    AgmSessionAifSetCal,
    AgmSessionGetParams,
    AgmSessionGetBufInfo,
    AgmGetBufferTimestamp,
    AgmDump,
    AgmShmemBufAlloc,
    AgmShmemBufFree,
    AgmSetParamsWithShmemByTag,
    AgmGetParamsWithTag,
    AgmSessionOpen,
    AgmDbusModuleMethodMax
};

enum AgmSessionMethods {
    AgmSessionClose,
    AgmSessionPrepare,
    AgmSessionStart,
    AgmSessionStop,
    AgmSessionPause,
    AgmSessionFlush,
    AgmSessionSuspend,
    AgmSessionResume,
    AgmSessionRead,
    AgmSessionWrite,
    AgmSessionReadWithMetadata,
    AgmSessionWriteWithMetadata,
    AgmSessionSetConfig,
    AgmSessionSetNonTunnelModeConfig,
    AgmSessionEos,
    AgmSetGaplessSessionMetadata,
    AgmSessionGetTime,
    AgmGetHwProcessedBufCount,
    AgmDbusSessionMethodMax
};

enum AgmEventSignals {
    AgmEventCb,
    AgmSesEventCb,
    AgmSignalMax
};

enum AgmModuleSignals {
    AgmSsrEventCb,
    AgmModuleSignalMax
};

enum {
    SES_THREAD_IDLE,
    SES_THREAD_READ_QUEUED,
    SES_THREAD_WRITE_QUEUED,
    SES_THREAD_EXIT,
};

static void ipc_agm_audio_intf_set_metadata(DBusConnection *conn,
                                            DBusMessage *msg,
                                            void *userdata);
static void ipc_agm_audio_intf_set_media_config(DBusConnection *conn,
                                                DBusMessage *msg,
                                                void *userdata);
static void ipc_agm_session_audio_inf_set_metadata(DBusConnection *conn,
                                                   DBusMessage *msg,
                                                   void *userdata);
static void ipc_agm_session_set_metadata(DBusConnection *conn,
                                         DBusMessage *msg,
                                         void *userdata);
static void ipc_agm_session_set_loopback(DBusConnection *conn,
                                         DBusMessage *msg,
                                         void *userdata);
static void ipc_agm_set_params_with_tag(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata);
static void ipc_agm_get_aif_info_list_size(DBusConnection *conn,
                                           DBusMessage *msg,
                                           void *userdata);
static void ipc_agm_get_aif_info_list(DBusConnection *conn,
                                      DBusMessage *msg,
                                      void *userdata);
static void ipc_agm_session_set_params(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata);
static void ipc_agm_session_register_for_events(DBusConnection *conn,
                                                DBusMessage *msg,
                                                void *userdata);
static void ipc_agm_session_set_ec_ref(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata);
static void ipc_agm_session_audio_inf_connect(DBusConnection *conn,
                                              DBusMessage *msg,
                                              void *userdata);
static void ipc_agm_session_aif_get_tag_module_info(DBusConnection *conn,
                                                    DBusMessage *msg,
                                                    void *userdata);
static void ipc_agm_session_aif_get_tag_module_info_size(DBusConnection *conn,
                                                         DBusMessage *msg,
                                                         void *userdata);
static void ipc_agm_session_aif_set_params(DBusConnection *conn,
                                           DBusMessage *msg,
                                           void *userdata);
static void ipc_agm_aif_set_params(DBusConnection *conn,
                                   DBusMessage *msg,
                                   void *userdata);
static void ipc_agm_session_aif_set_cal(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata);
static void ipc_agm_session_get_params(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata);
static void ipc_agm_session_get_buf_info(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata);
static void ipc_agm_session_open(DBusConnection *conn,
                                 DBusMessage *msg,
                                 void *userdata);
static void ipc_agm_session_close(DBusConnection *conn,
                                  DBusMessage *msg,
                                  void *userdata);
static void ipc_agm_session_prepare(DBusConnection *conn,
                                    DBusMessage *msg,
                                    void *userdata);
static void ipc_agm_session_start(DBusConnection *conn,
                                  DBusMessage *msg,
                                  void *userdata);
static void ipc_agm_session_stop(DBusConnection *conn,
                                 DBusMessage *msg,
                                 void *userdata);
static void ipc_agm_session_pause(DBusConnection *conn,
                                  DBusMessage *msg,
                                  void *userdata);
static void ipc_agm_session_flush(DBusConnection *conn,
                                  DBusMessage *msg,
                                  void *userdata);
static void ipc_agm_session_suspend(DBusConnection *conn,
                                    DBusMessage *msg,
                                    void *userdata);
static void ipc_agm_session_resume(DBusConnection *conn,
                                   DBusMessage *msg,
                                   void *userdata);
static void ipc_agm_session_read(DBusConnection *conn,
                                 DBusMessage *msg,
                                 void *userdata);
static void ipc_agm_session_write(DBusConnection *conn,
                                  DBusMessage *msg,
                                  void *userdata);
static void ipc_agm_session_read_with_metadata(DBusConnection *conn,
                                               DBusMessage *msg,
                                               void *userdata);
static void ipc_agm_session_write_with_metadata(DBusConnection *conn,
                                                DBusMessage *msg,
                                                void *userdata);
static void ipc_agm_session_set_config(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata);
static void ipc_agm_session_set_non_tunnel_mode_config(DBusConnection *conn,
                                                       DBusMessage *msg,
                                                       void *userdata);
static void ipc_agm_session_eos(DBusConnection *conn,
                                DBusMessage *msg,
                                void *userdata);
static void ipc_agm_set_gapless_session_metadata(DBusConnection *conn,
                                                 DBusMessage *msg,
                                                 void *userdata);
static void ipc_agm_get_session_time(DBusConnection *conn,
                                     DBusMessage *msg,
                                     void *userdata);
static void ipc_agm_get_hw_processed_buff_cnt(DBusConnection *conn,
                                              DBusMessage *msg,
                                              void *userdata);
static void ipc_agm_get_buffer_timestamp(DBusConnection *conn,
                                         DBusMessage *msg,
                                         void *userdata);
static void ipc_agm_shmem_buf_alloc(DBusConnection *conn,
                                    DBusMessage *msg,
                                    void *userdata);
static void ipc_agm_shmem_buf_free(DBusConnection *conn,
                                   DBusMessage *msg,
                                   void *userdata);
static void ipc_agm_set_params_with_shmem_by_tag(DBusConnection *conn,
                                                 DBusMessage *msg,
                                                 void *userdata);
static void ipc_agm_get_params_with_tag(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata);
static void ipc_agm_session_register_cb(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata);
static void ipc_agm_session_deregister_cb(DBusConnection *conn,
                                          DBusMessage *msg,
                                          void *userdata);
static void ipc_agm_dump(DBusConnection *conn,
                         DBusMessage *msg,
                         void *userdata);
static void ses_write_done(agm_session_data *ses_data, uint32_t status);
static void ses_read_done(agm_session_data *ses_data, uint32_t status);

static agm_dbus_method agm_dbus_module_methods[AgmDbusModuleMethodMax] = {
    {"AgmAifSetMediaConfig", "u(uuiu)", ipc_agm_audio_intf_set_media_config},
    {"AgmAifSetMetadata", "uuay", ipc_agm_audio_intf_set_metadata},
    {"AgmSessionAifSetMetadata", "uuuay",
                                      ipc_agm_session_audio_inf_set_metadata},
    {"AgmSessionSetMetadata", "uuay", ipc_agm_session_set_metadata},
    {"AgmSessionSetLoopback", "uub", ipc_agm_session_set_loopback},
    {"AgmSetParamsWithTag", "uu(uua(uu))", ipc_agm_set_params_with_tag},
    {"AgmGetAifInfoListSize", "", ipc_agm_get_aif_info_list_size},
    {"AgmGetAifInfoList", "u", ipc_agm_get_aif_info_list},
    {"AgmSessionSetParams", "uuay", ipc_agm_session_set_params},
    {"AgmSessionRegisterForEvents", "u(uuuyay)",
                                         ipc_agm_session_register_for_events},
    {"AgmSessionRegisterCb", "uut", ipc_agm_session_register_cb},
    {"AgmSessionDeRegisterCb", "uut", ipc_agm_session_deregister_cb},
    {"AgmSessionSetEcRef", "uub", ipc_agm_session_set_ec_ref},
    {"AgmSessionAifConnect", "uub", ipc_agm_session_audio_inf_connect},
    {"AgmSessionAifGetTagModuleInfo", "uuu",
                                     ipc_agm_session_aif_get_tag_module_info},
    {"AgmSessionAifGetTagModuleInfoSize", "uuu",
                                  ipc_agm_session_aif_get_tag_module_info_size},
    {"AgmSessionAifSetParams", "uuuay", ipc_agm_session_aif_set_params},
    {"AgmAifSetParams", "uuay", ipc_agm_aif_set_params},
    {"AgmSessionAifSetCal", "uuuay", ipc_agm_session_aif_set_cal},
    {"AgmSessionGetParams", "uuay", ipc_agm_session_get_params},
    {"AgmSessionGetBufInfo", "uu", ipc_agm_session_get_buf_info},
    {"AgmGetBufferTimestamp", "u", ipc_agm_get_buffer_timestamp},
    {"AgmDump", "(iuu)", ipc_agm_dump},
    {"AgmShmemBufAlloc", "uu", ipc_agm_shmem_buf_alloc},
    {"AgmShmemBufFree", "u", ipc_agm_shmem_buf_free},
    {"AgmSetParamsWithShmemByTag", "ay", ipc_agm_set_params_with_shmem_by_tag},
    {"AgmGetParamsWithTag", "ayuay", ipc_agm_get_params_with_tag},
    {"AgmSessionOpen", "uu", ipc_agm_session_open}
};

static agm_dbus_method agm_dbus_session_methods[AgmDbusSessionMethodMax] = {
    {"AgmSessionClose", "", ipc_agm_session_close},
    {"AgmSessionPrepare", "", ipc_agm_session_prepare},
    {"AgmSessionStart", "", ipc_agm_session_start},
    {"AgmSessionStop", "", ipc_agm_session_stop},
    {"AgmSessionPause", "", ipc_agm_session_pause},
    {"AgmSessionFlush", "", ipc_agm_session_flush},
    {"AgmSessionSuspend", "", ipc_agm_session_suspend},
    {"AgmSessionResume", "", ipc_agm_session_resume},
    {"AgmSessionRead", "u", ipc_agm_session_read},
    {"AgmSessionWrite", "uay", ipc_agm_session_write},
    {"AgmSessionReadWithMetadata", "u(tuuayuay(iuu))",
                                      ipc_agm_session_read_with_metadata},
    {"AgmSessionWriteWithMetadata", "(tuuayuay(iuu))",
                                       ipc_agm_session_write_with_metadata},
    {"AgmSessionSetConfig", "(uuu)(uu)ay", ipc_agm_session_set_config},
    {"AgmSessionSetNonTunnelModeConfig", "(uuuu)(uuuu)(uuu)(uuu)ay",
                                         ipc_agm_session_set_non_tunnel_mode_config},
    {"AgmSessionEos", "", ipc_agm_session_eos},
    {"AgmSetGaplessSessionMetadata", "uu", ipc_agm_set_gapless_session_metadata},
    {"AgmSessionGetTime", "", ipc_agm_get_session_time},
    {"AgmGetHwProcessedBufCount", "u", ipc_agm_get_hw_processed_buff_cnt}
};

static agm_dbus_signal event_callback[AgmSignalMax] = {
    {"AgmEventCb", "uuuay"},
    {"AgmSesEventCb", "uuuay"}
};

static agm_dbus_signal module_signals[AgmModuleSignalMax] = {
    {"AgmSsrEventCb", "u"}
};

agm_dbus_interface_info module_interface_info = {
    .name=AGM_MODULE_IFACE,
    .methods=agm_dbus_module_methods,
    .method_count=AgmDbusModuleMethodMax,
    .signals=module_signals,
    .signal_count=AgmModuleSignalMax
};

agm_dbus_interface_info session_interface_info = {
    .name=AGM_SESSION_IFACE,
    .methods=agm_dbus_session_methods,
    .method_count=AgmDbusSessionMethodMax,
    .signals=event_callback,
    .signal_count=AgmSignalMax
};

static void* async_thread_func(void *userdata) {
    agm_session_data *ses_data = (agm_session_data *)userdata;
    int ret = 0;

    AGM_LOGD("%s:Starting Async Thread\n", __func__);

    pthread_mutex_lock(&ses_data->lock);
    if (strcmp(ses_data->eventType, "Wait") == 0) {
        while (ses_data->thread_state != SES_THREAD_EXIT) {
            ret = pthread_cond_wait(&ses_data->cond, &ses_data->lock);
            AGM_LOGV("%s:returned value from wait:%d\n", __func__, ret);

            if(ses_data->thread_state == SES_THREAD_EXIT){
                AGM_LOGE("%s:thread_state is: SES_THREAD_EXIT\n", __func__);
                break;
            }

            if (ses_data->thread_state == SES_THREAD_WRITE_QUEUED) {
                pthread_mutex_unlock(&ses_data->lock);

                if (agm_session_write(ses_data->handle, ses_data->buf, (size_t *) &ses_data->buf_size)) {
                    AGM_LOGE("%s:agm_session_write failed\n", __func__);
                    ret = -1;
                }
                pthread_mutex_lock(&ses_data->lock);
                ses_write_done(ses_data, ret);
                /* Preserve EXIT if cleanup set it during write(). */
                if (ses_data->thread_state != SES_THREAD_EXIT)
                    ses_data->thread_state = SES_THREAD_IDLE;
            }

            if (ses_data->thread_state == SES_THREAD_READ_QUEUED) {
                pthread_mutex_unlock(&ses_data->lock);
                if (agm_session_read(ses_data->handle, ses_data->buf, (size_t *) &ses_data->buf_size)) {
                AGM_LOGE("agm_session_read failed");
                   ret = -1;
                }
                pthread_mutex_lock(&ses_data->lock);
                ses_read_done(ses_data, ret);
                if (ses_data->thread_state != SES_THREAD_EXIT)
                    ses_data->thread_state = SES_THREAD_IDLE;
            }
        }

    }
    if (strcmp(ses_data->eventType, "Signal") == 0) {
        AGM_LOGV("%s:Signal Event\n", __func__);
            pthread_cond_signal(&ses_data->cond);
    }

    pthread_mutex_unlock(&ses_data->lock);
    AGM_LOGV("Exiting asycn thread for session id = %d\n", ses_data->session_id);
    return NULL;
}

/*
 * Clean up a session whose owner vanished without calling AgmSessionClose.
 * Returns true if fully cleaned; the caller must then g_hash_table_remove()
 * to free ses_data. Returns false if the worker could not be joined; the
 * caller must leave the session in the hash to avoid UAF.
 */
static bool cleanup_session_force(agm_session_data *ses_data)
{
    GList *node = NULL;
    agm_callback_data *cb_data = NULL;

    if (ses_data == NULL)
        return false;

    AGM_LOGE("%s: force-cleaning session %u (owner=%s)", __func__,
             ses_data->session_id,
             ses_data->owner_unique_name ? ses_data->owner_unique_name : "<none>");

    /* Deregister event callbacks before freeing. If deregister fails the
     * AGM core may still fire into ses_data, so bail out to avoid UAF. */
    for (node = ses_data->callbacks; node != NULL; node = node->next) {
        cb_data = (agm_callback_data *)node->data;
        if (cb_data == NULL)
            continue;
        if (agm_session_register_cb(cb_data->session_id,
                                    NULL,
                                    (enum event_type)cb_data->event_type,
                                    (void *)ses_data) != 0) {
            AGM_LOGE("%s: deregister cb failed sid=%u evt=%u; "
                     "leaving session in hash to avoid UAF",
                     __func__, cb_data->session_id, cb_data->event_type);
            return false;
        }
        free(cb_data);
        node->data = NULL;
    }
    g_list_free(ses_data->callbacks);
    ses_data->callbacks = NULL;

    /* Signal the worker to exit and wait for it. The join budget bounds how
     * long we stall the D-Bus dispatch thread if the worker is still inside
     * agm_session_read/write(); see AGM_WORKER_JOIN_TIMEOUT_MS. */
    if (ses_data->ses_tid != 0) {
        struct timespec deadline;
        long add_ns = (long)AGM_WORKER_JOIN_TIMEOUT_MS * 1000L * 1000L;

        pthread_mutex_lock(&ses_data->lock);
        ses_data->thread_state = SES_THREAD_EXIT;
        pthread_cond_broadcast(&ses_data->cond);
        pthread_mutex_unlock(&ses_data->lock);

        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec  += add_ns / 1000000000L;
        deadline.tv_nsec += add_ns % 1000000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }

        if (pthread_timedjoin_np(ses_data->ses_tid, NULL, &deadline) == 0) {
            ses_data->ses_tid = 0;
            /* Worker took the EXIT short-circuit without running
             * ses_write/read_done, so a queued buf was never freed. */
            if (ses_data->buf != NULL) {
                free(ses_data->buf);
                ses_data->buf = NULL;
                ses_data->buf_size = 0;
            }
            pthread_cond_destroy(&ses_data->cond);
            pthread_mutex_destroy(&ses_data->lock);
        } else {
            /* Worker still alive past the join budget; GSL/DSP is likely
             * hung. Leak ses_data on purpose to avoid UAF on its lock/cond,
             * and skip stop/close which would deadlock on sess_obj->lock. */
            AGM_LOGE("%s: worker thread stuck past %dms sid=%u; "
                     "leaving session in hash to avoid UAF",
                     __func__, AGM_WORKER_JOIN_TIMEOUT_MS,
                     ses_data->session_id);
            return false;
        }
    }

    /* Safe to touch the handle only after the worker has joined. */
    if (agm_session_stop(ses_data->handle) != 0)
        AGM_LOGE("%s: agm_session_stop failed sid=%u",
                 __func__, ses_data->session_id);
    if (agm_session_close(ses_data->handle) != 0)
        AGM_LOGE("%s: agm_session_close failed sid=%u",
                 __func__, ses_data->session_id);

    /* Remove the D-Bus object and clear the path so agm_free_session
     * doesn't try to remove it again. */
    if (ses_data->dbus_obj_path != NULL) {
        if (agm_dbus_remove_interface(mdata->conn,
                                      ses_data->dbus_obj_path,
                                      session_interface_info.name) != 0)
            AGM_LOGE("%s: remove_interface failed for %s",
                     __func__, ses_data->dbus_obj_path);
        free(ses_data->dbus_obj_path);
        ses_data->dbus_obj_path = NULL;
    }

    return true;
}

/*
 * D-Bus filter for NameOwnerChanged. A peer death on the bus shows up as
 * name == ":1.NN" with new_owner == "". We ignore well-known names and
 * acquisitions.
 */
static DBusHandlerResult name_owner_changed_filter(DBusConnection *conn,
                                                   DBusMessage *msg,
                                                   void *userdata __unused)
{
    const char *name = NULL;
    const char *old_owner = NULL;
    const char *new_owner = NULL;
    DBusError err;
    GHashTableIter iter;
    gpointer key, value;
    GList *victims = NULL;
    GList *node = NULL;

    if (conn == NULL || msg == NULL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (!dbus_message_is_signal(msg,
                                DBUS_INTERFACE_DBUS,
                                "NameOwnerChanged"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_STRING, &old_owner,
                               DBUS_TYPE_STRING, &new_owner,
                               DBUS_TYPE_INVALID)) {
        AGM_LOGE("%s: failed to parse NameOwnerChanged: %s",
                 __func__, err.message);
        dbus_error_free(&err);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    dbus_error_free(&err);

    /* Only unique names represent a peer connection, and only
     * new_owner == "" means that peer went away. */
    if (name == NULL || name[0] != ':' ||
        new_owner == NULL || new_owner[0] != '\0')
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (mdata == NULL || mdata->sessions == NULL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    AGM_LOGI("%s: client %s vanished, scanning for orphan sessions",
             __func__, name);

    /* Collect victims first; we cannot modify the hash during iteration. */
    g_hash_table_iter_init(&iter, mdata->sessions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        agm_session_data *ses_data = (agm_session_data *)value;
        if (ses_data == NULL || ses_data->owner_unique_name == NULL)
            continue;
        if (strcmp(ses_data->owner_unique_name, name) == 0)
            victims = g_list_prepend(victims, ses_data);
    }

    if (victims == NULL) {
        AGM_LOGI("%s: no sessions owned by %s", __func__, name);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    /* Clean each victim; skip hash removal if cleanup fails (stuck worker). */
    for (node = victims; node != NULL; node = node->next) {
        agm_session_data *ses_data = (agm_session_data *)node->data;
        uint32_t sid = ses_data->session_id;
        if (cleanup_session_force(ses_data))
            g_hash_table_remove(mdata->sessions, GUINT_TO_POINTER(sid));
    }
    g_list_free(victims);

    /* Signal observed only; let other filters run too. */
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void agm_free_session(gpointer c_data) {
    agm_session_data *data = (agm_session_data *)c_data;

    if(data == NULL)
        return;

    if (data->dbus_obj_path != NULL) {
        if (agm_dbus_remove_interface(mdata->conn,
                                      data->dbus_obj_path,
                                      session_interface_info.name))
            AGM_LOGE("Unable to remove interface %s for session %u.",
                     data->dbus_obj_path, data->session_id);
        free(data->dbus_obj_path);
        data->dbus_obj_path = NULL;
    }

    if (data->owner_unique_name != NULL) {
        free(data->owner_unique_name);
        data->owner_unique_name = NULL;
    }

    free(data);
    data = NULL;
}

static agm_session_data * get_session_data(agm_module_dbus_data *mdata,
                                           uint32_t session_id) {
    agm_session_data *ses_data = NULL;
    int id_length = 0;
    size_t obj_length = 0;

    AGM_LOGV("%s:Enter", __func__);

    if (mdata == NULL)
        return NULL;

      if ((ses_data = (agm_session_data *)
                        g_hash_table_lookup(mdata->sessions,
                                       GUINT_TO_POINTER(session_id))) == NULL) {
        ses_data = (agm_session_data *)malloc(sizeof(agm_session_data));
        if (ses_data == NULL) {
            AGM_LOGE("ses_data is NULL\n");
            return NULL;
        }
        ses_data->session_id = session_id;
        id_length = snprintf(NULL, 0, "%d", session_id);
        obj_length = sizeof(char)*(strlen(AGM_OBJECT_PATH)) +
                     strlen("/session_") +
                     id_length + 1;
        ses_data->dbus_obj_path = (char *)malloc(obj_length);
        if (ses_data->dbus_obj_path == NULL) {
            AGM_LOGE("dbus_obj_path is NULL\n");
            free(ses_data);
            ses_data = NULL;
            return NULL;
        }
        snprintf(ses_data->dbus_obj_path,
                 obj_length,
                 "%s%s%d",
                 AGM_OBJECT_PATH,
                 "/session_",
                 session_id);
        ses_data->callbacks = NULL;
        ses_data->owner_unique_name = NULL;
        ses_data->ses_tid = 0;

        if (agm_dbus_add_interface(mdata->conn,
                                   ses_data->dbus_obj_path,
                                   &session_interface_info,
                                   ses_data)) {
            free(ses_data->dbus_obj_path);
            ses_data->dbus_obj_path = NULL;
            free(ses_data);
            ses_data = NULL;
            return NULL;
        }

        g_hash_table_insert(mdata->sessions,
                            GUINT_TO_POINTER(session_id),
                            ses_data);
    }
    AGM_LOGV("%s:Exit", __func__);
    return ses_data;
}

static void agmevent_cb(uint32_t session_id,
                        struct agm_event_cb_params *event_params,
                        void *client_data) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i, array_i;
    dbus_uint32_t i, j;
    void *buf = NULL;
    agm_session_data *ses_data = (agm_session_data *)client_data;

    AGM_LOGE("%s: Received event for session %d", __func__, session_id);

    buf = malloc(event_params->event_payload_size);
    if (buf == NULL) {
        AGM_LOGE("buf is NULL");
        return;
    }
    memcpy(buf, event_params->event_payload, event_params->event_payload_size);

    message = dbus_message_new_signal(ses_data->dbus_obj_path,
                                      session_interface_info.name,
                                      event_callback[AgmEventCb].method_name);

    dbus_message_iter_init_append(message, &arg_i);
    dbus_message_iter_append_basic(&arg_i,
                                   DBUS_TYPE_UINT32,
                                   &event_params->source_module_id);
    dbus_message_iter_append_basic(&arg_i,
                                   DBUS_TYPE_UINT32,
                                   &event_params->event_id);
    dbus_message_iter_append_basic(&arg_i,
                                   DBUS_TYPE_UINT32,
                                   &event_params->event_payload_size);
    dbus_message_iter_open_container(&arg_i,
                                     DBUS_TYPE_ARRAY,
                                     "y",
                                     &array_i);
    dbus_message_iter_append_fixed_array(&array_i,
                                         DBUS_TYPE_BYTE,
                                         &buf,
                                         event_params->event_payload_size);
    dbus_message_iter_close_container(&arg_i, &array_i);
    agm_dbus_send_signal(mdata->conn, message);
    dbus_message_unref(message);
    free(buf);
    buf = NULL;
}

static void ses_write_done(agm_session_data *ses_data, uint32_t status) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i, array_i;
    uint32_t dir = 1;

    AGM_LOGD("%s: Received write done event for session %d, status:%d", __func__,
        ses_data->session_id, status);

    message = dbus_message_new_signal(ses_data->dbus_obj_path,
                                      session_interface_info.name,
                                      event_callback[AgmSesEventCb].method_name);

    dbus_message_iter_init_append(message, &arg_i);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT32, &dir);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT32, &status);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT32, &ses_data->session_id);
    dbus_message_iter_open_container(&arg_i,
                                     DBUS_TYPE_ARRAY,
                                     "y",
                                     &array_i);
    dbus_message_iter_close_container(&arg_i, &array_i);

    agm_dbus_send_signal(mdata->conn, message);
    dbus_message_unref(message);
    if(ses_data->buf) {
        free(ses_data->buf);
        ses_data->buf = NULL;
    }
    AGM_LOGD("%s:Exit", __func__);
    return;
}

static void ses_read_done(agm_session_data *ses_data, uint32_t status) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i, array_i;
    uint32_t dir = 0;
    void *arr = ses_data->buf;

    AGM_LOGD("%s: Received read done event for session %d, status:%d", __func__,
        ses_data->session_id, status);

    message = dbus_message_new_signal(ses_data->dbus_obj_path,
                                      session_interface_info.name,
                                      event_callback[AgmSesEventCb].method_name);

    dbus_message_iter_init_append(message, &arg_i);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT32, &dir);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT32, &status);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT32, &ses_data->session_id);
    dbus_message_iter_open_container(&arg_i,
                                     DBUS_TYPE_ARRAY,
                                     "y",
                                     &array_i);
    dbus_message_iter_append_fixed_array(&array_i,
                                         DBUS_TYPE_BYTE,
                                         &arr,
                                         ses_data->buf_size);
    dbus_message_iter_close_container(&arg_i, &array_i);

    agm_dbus_send_signal(mdata->conn, message);
    dbus_message_unref(message);
    if(ses_data->buf) {
        free(ses_data->buf);
        ses_data->buf = NULL;
    }
    AGM_LOGD("%s:Exit", __func__);
    return;
}

static void ipc_agm_session_deregister_cb(DBusConnection *conn,
                                          DBusMessage *msg,
                                          void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    uint32_t session_id;
    uint32_t evt_type;
    agm_session_data *ses_data = NULL;
    agm_module_dbus_data *mdata = (agm_module_dbus_data *)userdata;
    uint64_t client_data;
    GList *node = NULL;
    agm_callback_data *cb_data = NULL;
    gboolean newly_created = FALSE;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_deregister_cb has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_deregister_cb has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uut")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_deregister_cb.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                        "Invalid signature for ipc_agm_session_deregister_cb.");
        return;
    }

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &evt_type);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &client_data);
    newly_created = (g_hash_table_lookup(mdata->sessions,
                                         GUINT_TO_POINTER(session_id)) == NULL);
    ses_data = get_session_data(mdata, session_id);
    if (ses_data == NULL) {
        AGM_LOGE("%s: get_session_data failed for session %u", __func__, session_id);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "get_session_data failed.");
        return;
    }

    if (ses_data == NULL) {
        AGM_LOGE("get_session_data failed");
        agm_dbus_send_error(mdata->conn,
                            msg, DBUS_ERROR_FAILED, "ses_data is NULL, get_session_data failed");
        return;
    }

    if (agm_session_register_cb(session_id, NULL,
                                (enum event_type)evt_type,
                                (void *)ses_data) != 0) {
        AGM_LOGE("agm_session_register_cb failed.");
        if (newly_created)
            g_hash_table_remove(mdata->sessions, GUINT_TO_POINTER(session_id));
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
            "Failed to de-register callback. agm_session_register_cb failed.");
        return;
    }

    cb_data = (agm_callback_data *)malloc(sizeof(agm_callback_data));
    if (cb_data == NULL) {
        AGM_LOGE("cb_data is NULL");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                             "cb_data is NULL");
        return;
    }
    cb_data->session_id = session_id;
    cb_data->event_type = evt_type;
    cb_data->client_data = client_data;
    node = g_list_find(ses_data->callbacks, cb_data);
    if (node != NULL) {
        ses_data->callbacks = g_list_remove(ses_data->callbacks, cb_data);
        free(node->data);
        node->data = NULL;
    }
    free(cb_data);
    cb_data = NULL;

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_append_basic(&arg_i,
                                   DBUS_TYPE_OBJECT_PATH,
                                   &ses_data->dbus_obj_path);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_register_cb(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    uint32_t session_id;
    uint32_t evt_type;
    agm_session_data *ses_data = NULL;
    agm_module_dbus_data *mdata = (agm_module_dbus_data *)userdata;
    uint64_t client_data;
    agm_callback_data *cb_data = NULL;
    gboolean newly_created = FALSE;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_register_cb has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_register_cb has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uut")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_register_cb.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                          "Invalid signature for ipc_agm_session_register_cb.");
        return;
    }

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &evt_type);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &client_data);
    newly_created = (g_hash_table_lookup(mdata->sessions,
                                         GUINT_TO_POINTER(session_id)) == NULL);
    ses_data = get_session_data(mdata, session_id);
    if (ses_data == NULL) {
        AGM_LOGE("%s: get_session_data failed for session %u", __func__, session_id);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "get_session_data failed.");
        return;
    }

    if (ses_data == NULL) {
        AGM_LOGE("get_session_data failed");
        agm_dbus_send_error(mdata->conn,
                            msg, DBUS_ERROR_FAILED, "ses_data is NULL, get_session_data failed");
        return;
    }

    if (agm_session_register_cb(session_id,
                                agmevent_cb,
                                (enum event_type)evt_type,
                                (void *)ses_data) != 0) {
        AGM_LOGE("agm_session_register_cb failed.");
        if (newly_created)
            g_hash_table_remove(mdata->sessions, GUINT_TO_POINTER(session_id));
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_register_cb failed.");
        return;
    }

    cb_data = (agm_callback_data *)malloc(sizeof(agm_callback_data));
    if (cb_data == NULL) {
        AGM_LOGE("cb_data is NULL");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                             "cb_data is NULL");
        return;
    }
    cb_data->session_id = session_id;
    cb_data->event_type = evt_type;
    cb_data->client_data = client_data;
    ses_data->callbacks = g_list_prepend(ses_data->callbacks, cb_data);

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_OBJECT_PATH,
                                   &ses_data->dbus_obj_path);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}


static void ipc_agm_session_register_for_events(DBusConnection *conn,
                                                DBusMessage *msg,
                                                void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, struct_i, struct_ii, array_i;
    struct agm_tag_config tag_config;
    uint32_t session_id;
    uint32_t i = 0;
    uint8_t *value = NULL;
    uint8_t **addr_value = &value;
    int arg_type;
    int n_elements = 0;
    struct agm_event_reg_cfg *evt_reg_cfg;
    uint32_t module_instance_id;
    uint32_t event_id;
    uint32_t event_config_payload_size;
    uint8_t is_register;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_register_for_events has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                        "ipc_agm_session_register_for_events has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "u(uuuyay)")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_register_for_events.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                  "Invalid signature for ipc_agm_session_register_for_events.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &module_instance_id);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &event_id);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &event_config_payload_size);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &is_register);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    evt_reg_cfg = (struct agm_event_reg_cfg *)
                    calloc (1,(sizeof(struct agm_event_reg_cfg) +
                            (event_config_payload_size)*sizeof(uint8_t)));
    if (evt_reg_cfg == NULL) {
        AGM_LOGE("alloc memory failed, evt_reg_cfg is NULL.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                             "alloc memory failed, evt_reg_cfg is NULL.");
        return;
    }
    evt_reg_cfg->module_instance_id = module_instance_id;
    evt_reg_cfg->event_id = event_id;
    evt_reg_cfg->event_config_payload_size = event_config_payload_size;
    evt_reg_cfg->is_register = is_register;
    memcpy(&evt_reg_cfg->event_config_payload[0], value, n_elements);


    if (agm_session_register_for_events(session_id, evt_reg_cfg) != 0) {
        AGM_LOGE("agm_session_register_for_events failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_register_for_events failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_get_params(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;
    DBusMessageIter r_arg, r_array_i;
    uint32_t session_id, size;
    void *payload = NULL;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_get_params has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_get_params has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uuay")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_get_params.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for ipc_agm_session_get_params.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    payload = (void *)malloc(n_elements*sizeof(char));
    if (payload == NULL) {
        AGM_LOGE("payload is NULL.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                              "Ipayload is NULL.");
        return;
    }
    memcpy(payload, value, n_elements);

    if (agm_session_get_params(session_id, (void *)payload, size) != 0) {
        AGM_LOGE("agm_session_get_params failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_get_params failed.");
        free(payload);
        payload = NULL;
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_open_container(&r_arg, DBUS_TYPE_ARRAY, "y", &r_array_i);
    dbus_message_iter_append_fixed_array(&r_array_i, DBUS_TYPE_BYTE, &payload,
                                         size);
    dbus_message_iter_close_container(&r_arg, &r_array_i);
    dbus_connection_send(conn, reply, NULL);
    free(payload);
    payload = NULL;
    dbus_message_unref(reply);
}

static void ipc_agm_session_get_buf_info(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;
    DBusMessageIter r_arg, struct_i;
    uint32_t session_id, flag;
    struct agm_buf_info *buf_info;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_get_buf_info has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_get_buf_info has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uu")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_get_buf_info.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for ipc_agm_session_get_buf_info.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &flag);
    //dbus_message_iter_next(&arg_i);
    buf_info = (struct agm_buf_info *) calloc(1,(sizeof(struct agm_buf_info)));

    if (buf_info == NULL) {
        AGM_LOGE("buf_info is NULL");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,"alloc memory failed, buf_info is NULL.");
        return;
    }

    if (agm_session_get_buf_info(session_id, buf_info, flag) != 0) {
        AGM_LOGE("agm_session_get_buf_info failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_get_buf_info failed.");
        free(buf_info);
        buf_info = NULL;
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_open_container(&r_arg, DBUS_TYPE_STRUCT, NULL, &struct_i);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &buf_info->data_buf_fd);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &buf_info->data_buf_size);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &buf_info->pos_buf_fd);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &buf_info->pos_buf_size);
    dbus_message_iter_close_container(&r_arg, &struct_i);
    dbus_connection_send(conn, reply, NULL);
    free(buf_info);
    buf_info = NULL;
    dbus_message_unref(reply);
}

static void ipc_agm_session_aif_set_cal(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;
    struct agm_cal_config *cal_config = NULL;
    uint32_t session_id, aif_id, num_ckv;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;
    AGM_LOGD("%s:Enter\n", __func__);

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("%s:ipc_agm_session_aif_set_cal has no arguments\n", __func__);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_aif_set_cal has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uuuay")) {
        AGM_LOGE("%s:Invalid signature for ipc_agm_session_aif_set_cal.\n", __func__);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                          "Invalid signature for ipc_agm_session_aif_set_cal.");
        return;
    }

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &aif_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &num_ckv);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);

    cal_config = (struct agm_cal_config *)
                        calloc (1, sizeof(struct agm_cal_config) +
                                num_ckv * sizeof(struct agm_key_value));
    if (cal_config == NULL) {
        AGM_LOGE("alloc memory failed, cal_config is NULL.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                             "alloc memory failed, cal_config is NULL.");
        return;
    }
    cal_config->num_ckvs = num_ckv;
    memcpy(cal_config->kv, value,
                             cal_config->num_ckvs*sizeof(struct agm_key_value));

    if (agm_session_aif_set_cal(session_id, aif_id, cal_config)) {
        AGM_LOGE("%s:agm_session_aif_set_cal failed.\n", __func__);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_aif_set_cal failed.");
        free(cal_config);
        cal_config = NULL;
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    free(cal_config);
    cal_config = NULL;
    AGM_LOGD("%s:Exit\n", __func__);
    return;
}

static void ipc_agm_session_aif_set_params(DBusConnection *conn,
                                          DBusMessage *msg,
                                          void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, r_arg;
    uint32_t session_id, aif_id, size;
    void *buf = NULL;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_aif_set_params has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_aif_set_params has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uuuay")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_aif_set_params.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                       "Invalid signature for ipc_agm_session_aif_set_params.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &aif_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    buf = (void *)malloc(n_elements*sizeof(char));
    if (buf == NULL) {
        AGM_LOGE("buf is NULL");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED, "buf is NULL");
        return;
    }
    memcpy(buf, value, n_elements);

    if (agm_session_aif_set_params(session_id, aif_id, buf, size) != 0) {
        AGM_LOGE("agm_session_aif_set_params failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_aif_set_params failed.");
        free(buf);
        buf = NULL;
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    free(buf);
    buf = NULL;
    dbus_message_unref(reply);
}

static void ipc_agm_aif_set_params(DBusConnection *conn,
                                   DBusMessage *msg,
                                   void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, r_arg;
    uint32_t aif_id, size;
    void *buf = NULL;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_aif_set_params has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_aif_set_params has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uuay")) {
        AGM_LOGE("Invalid signature for ipc_agm_aif_set_params.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                       "Invalid signature for ipc_agm_aif_set_params.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &aif_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    buf = (void *)malloc(n_elements*sizeof(char));

    if (buf == NULL) {
        AGM_LOGE("buf is NULL");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,"alloc memory failed, buf is NULL.");
        return;
    }

    memcpy(buf, value, n_elements);

    if (agm_aif_set_params(aif_id, buf, size) != 0) {
        AGM_LOGE("agmaif_set_params failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_aif_set_params failed.");
        free(buf);
        buf = NULL;
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    free(buf);
    buf = NULL;
    dbus_message_unref(reply);
}


static void ipc_agm_session_aif_get_tag_module_info_size(DBusConnection *conn,
                                                         DBusMessage *msg,
                                                         void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, r_arg;
    uint32_t session_id, aif_id;
    size_t size=0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
               "ipc_agm_session_aif_get_tag_module_info_size has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uuu")) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
         "Invalid signature for ipc_agm_session_aif_get_tag_module_info_size.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &aif_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &size);

    if (agm_session_aif_get_tag_module_info(session_id,
                                            aif_id,
                                            NULL,
                                            &size) != 0) {
        AGM_LOGE("agm_session_aif_get_tag_module_info failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_aif_get_tag_module_info failed.");
        return;
    }

    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT32, &size);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_aif_get_tag_module_info(DBusConnection *conn,
                                                    DBusMessage *msg,
                                                    void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, r_arg, r_array_i;
    uint32_t session_id, aif_id;
    size_t size=0;
    void *buf = NULL;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_aif_get_tag_module_info has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                    "ipc_agm_session_aif_get_tag_module_info has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uuu")) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
              "Invalid signature for ipc_agm_session_aif_get_tag_module_info.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &aif_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &size);

    buf = calloc(size, sizeof(uint8_t));

    if (agm_session_aif_get_tag_module_info(session_id,
                                            aif_id,
                                            buf,
                                            &size) != 0) {
        AGM_LOGE("agm_session_aif_get_tag_module_info failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_aif_get_tag_module_info failed.");
        free(buf);
        buf = NULL;
        return;
    }

    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT32, &size);
    dbus_message_iter_open_container(&r_arg, DBUS_TYPE_ARRAY, "y", &r_array_i);
    dbus_message_iter_append_fixed_array(&r_array_i, DBUS_TYPE_BYTE, &buf,
                                         size);
    dbus_message_iter_close_container(&r_arg, &r_array_i);
    dbus_connection_send(conn, reply, NULL);
    free(buf);
    buf = NULL;
    dbus_message_unref(reply);
}

static void ipc_agm_session_audio_inf_connect(DBusConnection *conn,
                                              DBusMessage *msg,
                                              void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    uint32_t session_id, aif_id;
    dbus_bool_t state;
    AGM_LOGD("%s :Enter ", __func__);

    if (userdata == NULL) {
        AGM_LOGE("%s :Invalid userdata", __func__);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("%s :ipc_agm_session_audio_inf_connect has no arguments", __func__);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                          "ipc_agm_session_audio_inf_connect has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uub")) {
        AGM_LOGE("%s :Invalid signature for ipc_agm_session_audio_inf_connect.", __func__);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                    "Invalid signature for ipc_agm_session_audio_inf_connect.");
        return;
    }

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &aif_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &state);

    if (agm_session_aif_connect(session_id, aif_id, (bool)state) != 0) {
        AGM_LOGE("%s :agm_session_aif_connect failed.", __func__);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_aif_connect failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_set_ec_ref(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    uint32_t capture_session_id, aif_id;
    dbus_bool_t state;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_set_ec_ref has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_set_ec_ref has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uub")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_set_ec_ref.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for ipc_agm_session_set_ec_ref.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &capture_session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &aif_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &state);

    if (agm_session_set_ec_ref(capture_session_id, aif_id, (bool)state) != 0) {
        AGM_LOGE("agm_session_set_ec_ref failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_set_ec_ref failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_set_params(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;
    uint32_t session_id, size;
    void *payload = NULL;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_set_params has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_set_params has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uuay")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_set_params.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for ipc_agm_session_set_params.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    payload = (void *)malloc(n_elements*sizeof(char));

    if (payload == NULL) {
        AGM_LOGE("payload is NULL");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,"alloc memory failed, payload is NULL.");
        return;
    }

    memcpy(payload, value, n_elements);

    if (agm_session_set_params(session_id, (void *)payload, size) != 0) {
        AGM_LOGE("agm_session_set_params failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_set_params failed.");
        free(payload);
        payload = NULL;
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    free(payload);
    payload = NULL;
    dbus_message_unref(reply);
}

static void ipc_agm_get_aif_info_list_size(DBusConnection *conn,
                                           DBusMessage *msg,
                                           void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter r_arg;
    agm_module_dbus_data *mdata = (agm_module_dbus_data *)userdata;
    size_t num_aif_info = 0;

    if (agm_get_aif_info_list(NULL, &num_aif_info) != 0) {
        AGM_LOGE("agm_get_aif_info_list failed");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_get_aif_info_list_size failed");
        return;
    }

    AGM_LOGV("%s with size: %d", __func__, num_aif_info);

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT32, &num_aif_info);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_get_aif_info_list(DBusConnection *conn,
                                      DBusMessage *msg,
                                      void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    DBusMessageIter r_arg, array_i, struct_i;
    agm_module_dbus_data *mdata = (agm_module_dbus_data *)userdata;
    uint32_t num_aif_info;
    struct aif_info *aifinfo = NULL;
    char *name;
    int i = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_get_aif_info_list has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_get_aif_info_list has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "u")) {
        AGM_LOGE("Invalid signature for ipc_agm_get_aif_info_list.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid signature for ipc_agm_get_aif_info_list.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &num_aif_info);

    aifinfo = (struct aif_info *)calloc(num_aif_info, sizeof(struct aif_info));

    if (agm_get_aif_info_list(aifinfo, &num_aif_info)) {
        AGM_LOGE("agm_get_aif_info_list failed");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_get_aif_info_list failed");
        free(aifinfo);
        aifinfo = NULL;
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_open_container(&r_arg, DBUS_TYPE_ARRAY, "(us)", &array_i);
    for (i = 0; i < num_aif_info; i++) {
        dbus_message_iter_open_container(&array_i, DBUS_TYPE_STRUCT, NULL,
                                         &struct_i);
        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
                                       &aifinfo[i].dir);
        name = aifinfo[i].aif_name;
        dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_STRING, &name);
        dbus_message_iter_close_container(&array_i, &struct_i);
    }
    dbus_message_iter_close_container(&r_arg, &array_i);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    free(aifinfo);
    aifinfo = NULL;
}

static void ipc_agm_set_params_with_tag(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, struct_i, struct_ii, array_i;
    struct agm_tag_config *tag_config;
    uint32_t tag;
    uint32_t num_tkvs;
    uint32_t session_id, aif_id;
    uint32_t i = 0;
    int arg_type;
    size_t size_local = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_set_params_with_tag has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_set_params_with_tag has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uu(uua(uu))")) {
        AGM_LOGE("Invalid signature for ipc_agm_set_params_with_tag.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                          "Invalid signature for ipc_agm_set_params_with_tag.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &aif_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &tag);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &num_tkvs);
    dbus_message_iter_next(&struct_i);
    size_local = (sizeof(struct agm_tag_config) +
                        (num_tkvs) * sizeof(struct agm_key_value));
    tag_config = (struct agm_tag_config *) calloc(1,size_local);
    if (tag_config == NULL) {
        AGM_LOGE("tag_config is NULL.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                             "tag_config is NULL.");
        return;
    }
    tag_config->tag = tag;
    tag_config->num_tkvs = num_tkvs;
    dbus_message_iter_recurse(&struct_i, &array_i);
    while (((arg_type = dbus_message_iter_get_arg_type(&array_i))
                                                != DBUS_TYPE_INVALID)
             && (i < tag_config->num_tkvs)) {
        dbus_message_iter_recurse(&array_i, &struct_ii);
        dbus_message_iter_get_basic(&struct_ii, &tag_config->kv[i].key);
        dbus_message_iter_next(&struct_ii);
        dbus_message_iter_get_basic(&struct_ii, &tag_config->kv[i].value);
        dbus_message_iter_next(&array_i);
        i++;
    }

    if (agm_set_params_with_tag(session_id, aif_id, tag_config)) {
        AGM_LOGE("agm_set_params_with_tag failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_set_params_with_tag failed.");
        free(tag_config);
        tag_config = NULL;
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    free(tag_config);
    tag_config = NULL;
}

static void ipc_agm_session_set_loopback(DBusConnection *conn,
                                         DBusMessage *msg,
                                         void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    uint32_t capture_session_id, playback_session_id;
    dbus_bool_t flag;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_set_loopback has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_set_loopback has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uub")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_set_loopback.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                         "Invalid signature for ipc_agm_session_set_loopback.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &capture_session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &playback_session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &flag);

    if (agm_session_set_loopback(capture_session_id,
                                 playback_session_id,
                                 (bool)flag) != 0) {
        AGM_LOGE("agm_session_set_loopback failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_set_loopback failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_set_metadata(DBusConnection *conn,
                                         DBusMessage *msg,
                                         void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;
    uint32_t session_id, size;
    void *metadata;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_set_metadata has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_set_metadata has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uuay")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_set_metadata.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                         "Invalid signature for ipc_agm_session_set_metadata.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    metadata = (void *)malloc(n_elements*sizeof(char));
    if (metadata == NULL) {
        AGM_LOGE("metadata is NULL");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED, "metadata is NULL");
        return;
    }
    memcpy(metadata, value, n_elements);

    if (agm_session_set_metadata(session_id, size, (uint8_t *)metadata) != 0) {
        AGM_LOGE("agm_session_set_metadata failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_set_metadata failed.");
        free(metadata);
        metadata = NULL;
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    free(metadata);
    metadata = NULL;
    dbus_message_unref(reply);
}

static void ipc_agm_session_audio_inf_set_metadata(DBusConnection *conn,
                                                   DBusMessage *msg,
                                                   void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;
    uint32_t session_id, aif_id, size;
    void *metadata;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_audio_inf_set_metadata has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                     "ipc_agm_session_audio_inf_set_metadata has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uuuay")) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
               "Invalid signature for ipc_agm_session_audio_inf_set_metadata.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &aif_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    metadata = (void *)malloc(n_elements*sizeof(char));
    if (metadata == NULL) {
        AGM_LOGE("metadata is NULL");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED, "metadata is NULL");
        return;
    }
    memcpy(metadata, value, n_elements);

    if (agm_session_aif_set_metadata(session_id,
                                     aif_id,
                                     size,
                                     (uint8_t *)metadata) != 0) {
        AGM_LOGE("agm_session_aif_set_metadata failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_aif_set_metadata failed.");
        free(metadata);
        metadata = NULL;
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    free(metadata);
    metadata = NULL;
    dbus_message_unref(reply);
}

static void ipc_agm_audio_intf_set_metadata(DBusConnection *conn,
                                            DBusMessage *msg,
                                            void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;
    uint32_t aif_id, size;
    void *metadata;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_audio_intf_set_metadata has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_audio_intf_set_metadata has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uuay")) {
        AGM_LOGE("Invalid signature for ipc_agm_audio_intf_set_metadata.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                      "Invalid signature for ipc_agm_audio_intf_set_metadata.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &aif_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    metadata = (void *)malloc(n_elements*sizeof(char));
    if (metadata == NULL) {
        AGM_LOGE("metadata is NULL");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED, "metadata is NULL");
        return;
    }
    memcpy(metadata, value, n_elements);

    if (agm_aif_set_metadata(aif_id, size, (uint8_t *)metadata) != 0) {
        AGM_LOGE("agm_aif_set_metadata failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_aif_set_metadata failed.");
        free(metadata);
        metadata = NULL;
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    free(metadata);
    metadata = NULL;
    dbus_message_unref(reply);
}

static void ipc_agm_audio_intf_set_media_config(DBusConnection *conn,
                                                DBusMessage *msg,
                                                void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, struct_i;
    struct agm_media_config media_config;
    uint32_t aif_id;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_audio_intf_set_media_config has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                        "ipc_agm_audio_intf_set_media_config has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "u(uuiu)")) {
        AGM_LOGE("Invalid signature for ipc_agm_audio_intf_set_media_config.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                  "Invalid signature for ipc_agm_audio_intf_set_media_config.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &aif_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &media_config.rate);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &media_config.channels);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &media_config.format);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &media_config.data_format);

    if (agm_aif_set_media_config(aif_id, &media_config)) {
        AGM_LOGE("agm_aif_set_media_config failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_aif_set_media_config failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_get_buffer_timestamp(DBusConnection *conn,
                                         DBusMessage *msg,
                                         void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    DBusMessageIter r_arg, r_array_i;
    uint64_t timestamp = 0;
    uint32_t session_id;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_get_buffer_timestamp has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_get_buffer_timestamp has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "u")) {
        AGM_LOGE("Invalid signature for ipc_agm_get_buffer_timestamp.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                         "Invalid signature for ipc_agm_get_buffer_timestamp.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &session_id);

    if (agm_get_buffer_timestamp(session_id, &timestamp)) {
        AGM_LOGE("agm_get_buffer_timestamp failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_get_buffer_timestamp failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT64, &timestamp);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_dump(DBusConnection *conn,
                         DBusMessage *msg,
                         void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, struct_i;
    struct agm_dump_info dump_info;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_dump has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_dump has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "(iuu)")) {
        AGM_LOGE("Invalid signature for ipc_agm_dump.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid signature for ipc_agm_dump.");
        return;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &dump_info.signal);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &dump_info.pid);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &dump_info.uid);

    if (agm_dump(&dump_info)) {
        AGM_LOGE("agm_dump failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_dump failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_shmem_buf_alloc(DBusConnection *conn,
                                    DBusMessage *msg,
                                    void *userdata)
{
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    DBusMessageIter r_arg;
    struct agm_shmem_info info;
    uint32_t size = 0;
    uint32_t cache = 0;
    uint32_t ion_fd_reply = 0;
    uint32_t service_pid = 0;
    int stable_fd = -1;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_shmem_buf_alloc has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_shmem_buf_alloc has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uu")) {
        AGM_LOGE("Invalid signature for ipc_agm_shmem_buf_alloc.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid signature for ipc_agm_shmem_buf_alloc.");
        return;
    }

    memset(&info, 0, sizeof(info));
    dbus_message_iter_get_basic(&arg_i, &size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &cache);
    info.size = size;
    info.cache = cache;
    if (agm_shmem_buf_alloc(&info) != 0) {
        AGM_LOGE("agm_shmem_buf_alloc failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_shmem_buf_alloc failed.");
        return;
    }

    if (info.ion_fd > (uint32_t)INT_MAX ||
        !agm_fd_is_mmap_candidate((int)info.ion_fd)) {
        AGM_LOGE("%s: invalid/non-mmap fd %u for spf_handle 0x%x",
                 __func__, info.ion_fd, info.spf_mem_handle);
        agm_shmem_buf_free(info.spf_mem_handle);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "invalid shared-memory fd");
        return;
    }

    stable_fd = dup((int)info.ion_fd);
    if (stable_fd < 0) {
        AGM_LOGE("%s: dup failed for ion_fd %u, errno %d",
                 __func__, info.ion_fd, errno);
        agm_shmem_buf_free(info.spf_mem_handle);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "failed to duplicate shared-memory fd");
        return;
    }
    if (agm_shmem_fd_track_add(info.spf_mem_handle, stable_fd) != 0) {
        AGM_LOGE("%s: failed to track stable fd %d for spf_handle 0x%x",
                 __func__, stable_fd, info.spf_mem_handle);
        close(stable_fd);
        agm_shmem_buf_free(info.spf_mem_handle);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "failed to track shared-memory fd");
        return;
    }

    ion_fd_reply = (uint32_t)stable_fd;
    service_pid = (uint32_t)getpid();
    reply = dbus_message_new_method_return(msg);
    if (!reply) {
        AGM_LOGE("%s: failed to allocate reply message", __func__);
        agm_shmem_fd_track_remove(info.spf_mem_handle);
        agm_shmem_buf_free(info.spf_mem_handle);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "failed to build reply");
        return;
    }

    dbus_message_iter_init_append(reply, &r_arg);
    if (!dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT32, &ion_fd_reply) ||
        !dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT64, &info.spf_addr) ||
        !dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT32,
                                        &info.spf_mem_handle) ||
        !dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT32,
                                        &service_pid)) {
        AGM_LOGE("%s: failed to append reply args", __func__);
        dbus_message_unref(reply);
        agm_shmem_fd_track_remove(info.spf_mem_handle);
        agm_shmem_buf_free(info.spf_mem_handle);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "failed to encode shared-memory reply");
        return;
    }

    if (!dbus_connection_send(conn, reply, NULL)) {
        AGM_LOGE("%s: dbus_connection_send failed", __func__);
        agm_shmem_fd_track_remove(info.spf_mem_handle);
        agm_shmem_buf_free(info.spf_mem_handle);
    }
    dbus_message_unref(reply);
}

static void ipc_agm_shmem_buf_free(DBusConnection *conn,
                                   DBusMessage *msg,
                                   void *userdata)
{
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    uint32_t spf_mem_handle = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_shmem_buf_free has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_shmem_buf_free has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "u")) {
        AGM_LOGE("Invalid signature for ipc_agm_shmem_buf_free.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid signature for ipc_agm_shmem_buf_free.");
        return;
    }

    dbus_message_iter_get_basic(&arg_i, &spf_mem_handle);
    if (agm_shmem_buf_free(spf_mem_handle) != 0) {
        AGM_LOGE("agm_shmem_buf_free failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_shmem_buf_free failed.");
        return;
    }

    agm_shmem_fd_track_remove(spf_mem_handle);

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_set_params_with_shmem_by_tag(DBusConnection *conn,
                                                 DBusMessage *msg,
                                                 void *userdata)
{
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;
    struct agm_tagged_shmem_config *cfg = NULL;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;
    size_t cfg_size = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_set_params_with_shmem_by_tag has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_set_params_with_shmem_by_tag has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "ay")) {
        AGM_LOGE("Invalid signature for ipc_agm_set_params_with_shmem_by_tag.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid signature for ipc_agm_set_params_with_shmem_by_tag.");
        return;
    }

    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    if (n_elements < (int)sizeof(*cfg)) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid tagged shmem config payload.");
        return;
    }

    cfg = malloc(n_elements);
    if (!cfg) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Failed to allocate tagged shmem config.");
        return;
    }
    memcpy(cfg, value, n_elements);

    if ((cfg->num_gkvs == 0) || (cfg->num_gkvs > AGM_MAX_TAGGED_GKVS)) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid tagged shmem GKV count.");
        free(cfg);
        return;
    }

    cfg_size = AGM_TAGGED_SHMEM_CONFIG_SIZE(cfg->num_gkvs);
    if (cfg_size > (size_t)n_elements) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid tagged shmem GKV payload.");
        free(cfg);
        return;
    }

    if (agm_set_params_with_shmem_by_tag(cfg) != 0) {
        AGM_LOGE("agm_set_params_with_shmem_by_tag failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_set_params_with_shmem_by_tag failed.");
        free(cfg);
        return;
    }

    free(cfg);
    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_get_params_with_tag(DBusConnection *conn,
                                        DBusMessage *msg,
                                        void *userdata)
{
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, r_arg, r_array_i;
    struct agm_tagged_get_config *cfg = NULL;
    uint32_t size = 0;
    void *payload = NULL;
    uint8_t *payload_bytes = NULL;
    char *cfg_value = NULL, *payload_value = NULL;
    char **addr_cfg_value = &cfg_value;
    char **addr_payload_value = &payload_value;
    int cfg_elements = 0, payload_elements = 0;
    size_t cfg_size = 0, payload_size = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_get_params_with_tag has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_get_params_with_tag has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "ayuay")) {
        AGM_LOGE("Invalid signature for ipc_agm_get_params_with_tag.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid signature for ipc_agm_get_params_with_tag.");
        return;
    }

    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_cfg_value, &cfg_elements);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_payload_value, &payload_elements);

    if ((cfg_elements < (int)sizeof(*cfg)) || (size == 0) ||
        (payload_elements <= 0) || ((uint32_t)payload_elements < size)) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid get params with tag payload.");
        return;
    }

    cfg = malloc(cfg_elements);
    payload = malloc(size);
    if (!cfg || !payload) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Failed to allocate get params payload.");
        free(cfg);
        free(payload);
        return;
    }
    memcpy(cfg, cfg_value, cfg_elements);
    memcpy(payload, payload_value, size);

    if ((cfg->num_gkvs == 0) || (cfg->num_gkvs > AGM_MAX_TAGGED_GKVS)) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid tagged get GKV count.");
        free(cfg);
        free(payload);
        return;
    }

    cfg_size = AGM_TAGGED_GET_CONFIG_SIZE(cfg->num_gkvs);
    if (cfg_size > (size_t)cfg_elements) {
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid tagged get GKV payload.");
        free(cfg);
        free(payload);
        return;
    }

    payload_size = size;
    if (agm_get_params_with_tag(cfg, payload, &payload_size) != 0) {
        AGM_LOGE("agm_get_params_with_tag failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_get_params_with_tag failed.");
        free(cfg);
        free(payload);
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_open_container(&r_arg, DBUS_TYPE_ARRAY, "y", &r_array_i);
    payload_bytes = (uint8_t *)payload;
    dbus_message_iter_append_fixed_array(&r_array_i, DBUS_TYPE_BYTE,
                                         &payload_bytes, payload_size);
    dbus_message_iter_close_container(&r_arg, &r_array_i);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    free(cfg);
    free(payload);
}

static void ipc_agm_get_hw_processed_buff_cnt(DBusConnection *conn,
                                              DBusMessage *msg,
                                              void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    DBusMessageIter r_arg, r_array_i;
    agm_session_data *ses_data = (agm_session_data *)userdata;
    uint32_t direction;
    size_t buf_count;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_get_hw_processed_buff_cnt has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                          "ipc_agm_get_hw_processed_buff_cnt has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "u")) {
        AGM_LOGE("Invalid signature for ipc_agm_get_hw_processed_buff_cnt.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                    "Invalid signature for ipc_agm_get_hw_processed_buff_cnt.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &direction);

    buf_count = agm_get_hw_processed_buff_cnt(ses_data->handle,
                                              (enum direction)direction);

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_INT32, &buf_count);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_get_session_time(DBusConnection *conn,
                                     DBusMessage *msg,
                                     void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter r_arg;
    agm_session_data *ses_data = (agm_session_data *)userdata;
    uint64_t timestamp;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (agm_get_session_time(ses_data->handle, &timestamp)) {
        AGM_LOGE("agm_get_session_time failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_get_session_time failed.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT64, &timestamp);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_set_config(DBusConnection *conn,
                                       DBusMessage *msg,
                                       void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, struct_i, struct_ii;
    agm_session_data *ses_data = (agm_session_data *)userdata;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;
    struct agm_session_config session_config;
    struct agm_media_config media_config;
    struct agm_buffer_config buffer_config;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_set_config has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_set_config has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "(uuu)(uu)ay")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_set_config.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                           "Invalid signature for ipc_agm_session_set_config.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &media_config.rate);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &media_config.channels);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &media_config.format);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &struct_ii);
    dbus_message_iter_get_basic(&struct_ii, &buffer_config.count);
    dbus_message_iter_next(&struct_ii);
    dbus_message_iter_get_basic(&struct_ii, &buffer_config.size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    if (n_elements < 0 ||
        (size_t)n_elements != sizeof(struct agm_session_config)) {
        AGM_LOGE("Invalid session config size.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid session config size.");
        return;
    }
    memcpy(&session_config, value, n_elements);

    if (agm_session_set_config(ses_data->handle,
                               &session_config,
                               &media_config,
                               &buffer_config)) {
        AGM_LOGE("agm_session_set_config failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_set_config failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_set_non_tunnel_mode_config(DBusConnection *conn,
                                                       DBusMessage *msg,
                                                       void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, struct_i;
    agm_session_data *ses_data = (agm_session_data *)userdata;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;
    struct agm_session_config session_config;
    struct agm_media_config in_media_config;
    struct agm_media_config out_media_config;
    struct agm_buffer_config in_buffer_config;
    struct agm_buffer_config out_buffer_config;
    uint32_t tmp_size = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_set_non_tunnel_mode_config has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
              "ipc_agm_session_set_non_tunnel_mode_config has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "(uuuu)(uuuu)(uuu)(uuu)ay")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_set_non_tunnel_mode_config.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
          "Invalid signature for ipc_agm_session_set_non_tunnel_mode_config.");
        return;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &in_media_config.rate);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &in_media_config.channels);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &in_media_config.format);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &in_media_config.data_format);

    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &out_media_config.rate);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &out_media_config.channels);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &out_media_config.format);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &out_media_config.data_format);

    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &in_buffer_config.count);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &tmp_size);
    in_buffer_config.size = tmp_size;
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &tmp_size);
    in_buffer_config.max_metadata_size = tmp_size;

    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &out_buffer_config.count);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &tmp_size);
    out_buffer_config.size = tmp_size;
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &tmp_size);
    out_buffer_config.max_metadata_size = tmp_size;

    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    if (n_elements < 0 ||
        (size_t)n_elements != sizeof(struct agm_session_config)) {
        AGM_LOGE("Invalid session config size.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                        "Invalid session config size.");
        return;
    }
    memcpy(&session_config, value, n_elements);

    if (agm_session_set_non_tunnel_mode_config(ses_data->handle,
                                               &session_config,
                                               &in_media_config,
                                               &out_media_config,
                                               &in_buffer_config,
                                               &out_buffer_config)) {
        AGM_LOGE("agm_session_set_non_tunnel_mode_config failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                     "agm_session_set_non_tunnel_mode_config failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_eos(DBusConnection *conn,
                                DBusMessage *msg,
                                void *userdata) {
    DBusMessage *reply = NULL;
    agm_session_data *ses_data = (agm_session_data *)userdata;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (agm_session_eos(ses_data->handle)) {
        AGM_LOGE("agm_session_eos failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_eos failed.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_set_gapless_session_metadata(DBusConnection *conn,
                                                 DBusMessage *msg,
                                                 void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    agm_session_data *ses_data = (agm_session_data *)userdata;
    uint32_t type = 0;
    uint32_t silence = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_set_gapless_session_metadata has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                       "ipc_agm_set_gapless_session_metadata has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uu")) {
        AGM_LOGE("Invalid signature for ipc_agm_set_gapless_session_metadata.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
               "Invalid signature for ipc_agm_set_gapless_session_metadata.");
        return;
    }

    dbus_message_iter_get_basic(&arg_i, &type);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &silence);

    if (agm_set_gapless_session_metadata(ses_data->handle,
            (enum agm_gapless_silence_type)type, silence)) {
        AGM_LOGE("agm_set_gapless_session_metadata failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_set_gapless_session_metadata failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_write(DBusConnection *conn,
                                  DBusMessage *msg,
                                  void *userdata) {

    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i, r_arg;
    agm_session_data *ses_data = (agm_session_data *)userdata;
    uint32_t buf_size=0;
    char *value = NULL;
    char **addr_value = &value;
    int n_elements = 0;

    AGM_LOGD("%s :Enter ", __func__);

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_write has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_write has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uay")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_write.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid signature for ipc_agm_session_write.");
        return;
    }

    dbus_message_iter_get_basic(&arg_i, &buf_size);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);

    pthread_mutex_lock(&ses_data->lock);
    ses_data->buf_size = buf_size;
    ses_data->buf = calloc(1, buf_size);
    if (ses_data->buf == NULL) {
        pthread_mutex_unlock(&ses_data->lock);
        AGM_LOGE("buf is NULL");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,"alloc memory failed, buf is NULL.");
        return;
    }
    if (ses_data->thread_state != SES_THREAD_IDLE) {
        pthread_mutex_unlock(&ses_data->lock);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED, "write via async failed");
        return;
    }
    memcpy(ses_data->buf, value, n_elements);
    ses_data->thread_state = SES_THREAD_WRITE_QUEUED;
    snprintf(ses_data->eventType, sizeof("Signal"), "%s", "Signal");
    pthread_mutex_unlock(&ses_data->lock);
    async_thread_func(ses_data);

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT32, &buf_size);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    AGM_LOGD("%s :Exit ", __func__);
    return;
}

static void ipc_agm_session_read(DBusConnection *conn,
                                 DBusMessage *msg,
                                 void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    DBusMessageIter r_arg, r_array_i;
    size_t buf_size=0;
    agm_session_data *ses_data = (agm_session_data *)userdata;
    AGM_LOGD("%s :Enter ", __func__);

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_read has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_read has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "u")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_read.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid signature for ipc_agm_session_read.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    dbus_message_iter_get_basic(&arg_i, &buf_size);

    pthread_mutex_lock(&ses_data->lock);
    ses_data->buf_size = buf_size;
    ses_data->buf = calloc(1, buf_size);
    if (ses_data->thread_state != SES_THREAD_IDLE) {
        pthread_mutex_unlock(&ses_data->lock);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED, "read via async failed");
        return;
    }
    ses_data->thread_state = SES_THREAD_READ_QUEUED;
    snprintf(ses_data->eventType, sizeof("Signal"), "%s", "Signal");
    pthread_mutex_unlock(&ses_data->lock);
    async_thread_func(ses_data);
    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    AGM_LOGD("%s :Exit ", __func__);
    return;
}

static void ipc_agm_session_write_with_metadata(DBusConnection *conn,
                                                DBusMessage *msg,
                                                void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, r_arg, struct_i, array_i, alloc_i;
    agm_session_data *ses_data = (agm_session_data *)userdata;
    struct agm_buff buff = {0};
    size_t consumed_size = 0;
    uint32_t consumed_size_u32 = 0;
    uint8_t *value = NULL;
    uint8_t **addr_value = &value;
    uint8_t *metadata = NULL;
    uint8_t **metadata_value = &metadata;
    int n_elements = 0;
    int metadata_elements = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_write_with_metadata has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                      "ipc_agm_session_write_with_metadata has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "(tuuayuay(iuu))")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_write_with_metadata.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
              "Invalid signature for ipc_agm_session_write_with_metadata.");
        return;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &buff.timestamp);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &buff.flags);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &buff.size);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    if (n_elements < 0 || (uint32_t)n_elements > buff.size) {
        AGM_LOGE("Invalid buffer size.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid buffer size.");
        return;
    }
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &buff.metadata_size);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, metadata_value,
                                      &metadata_elements);
    if (metadata_elements < 0 ||
        (uint32_t)metadata_elements > buff.metadata_size) {
        AGM_LOGE("Invalid metadata size.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid metadata size.");
        return;
    }
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &alloc_i);
    dbus_message_iter_get_basic(&alloc_i, &buff.alloc_info.alloc_handle);
    dbus_message_iter_next(&alloc_i);
    dbus_message_iter_get_basic(&alloc_i, &buff.alloc_info.alloc_size);
    dbus_message_iter_next(&alloc_i);
    dbus_message_iter_get_basic(&alloc_i, &buff.alloc_info.offset);

    if (buff.size > 0) {
        buff.addr = (uint8_t *)calloc(1, buff.size);
        if (buff.addr == NULL) {
            AGM_LOGE("buff addr is NULL");
            agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                                "buff addr is NULL");
            return;
        }
        if (value != NULL && n_elements > 0)
            memcpy(buff.addr, value, n_elements);
    }

    if (buff.metadata_size > 0) {
        buff.metadata = (uint8_t *)calloc(1, buff.metadata_size);
        if (buff.metadata == NULL) {
            AGM_LOGE("metadata is NULL");
            agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                                "metadata is NULL");
            free(buff.addr);
            return;
        }
        if (metadata != NULL && metadata_elements > 0)
            memcpy(buff.metadata, metadata, metadata_elements);
    }

    if (agm_session_write_with_metadata(ses_data->handle,
                                        &buff,
                                        &consumed_size)) {
        AGM_LOGE("agm_session_write_with_metadata failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_write_with_metadata failed.");
        free(buff.addr);
        free(buff.metadata);
        return;
    }

    consumed_size_u32 = consumed_size;
    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &r_arg);
    dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT32,
                                   &consumed_size_u32);
    dbus_connection_send(conn, reply, NULL);
    free(buff.addr);
    free(buff.metadata);
    dbus_message_unref(reply);
}

static void ipc_agm_session_read_with_metadata(DBusConnection *conn,
                                               DBusMessage *msg,
                                               void *userdata) {
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, r_arg, struct_i, array_i, alloc_i;
    agm_session_data *ses_data = (agm_session_data *)userdata;
    struct agm_buff buff = {0};
    uint32_t captured_size = 0;
    uint8_t *value = NULL;
    uint8_t **addr_value = &value;
    uint8_t *metadata = NULL;
    uint8_t **metadata_value = &metadata;
    int n_elements = 0;
    int metadata_elements = 0;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_read_with_metadata has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                       "ipc_agm_session_read_with_metadata has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "u(tuuayuay(iuu))")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_read_with_metadata.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
               "Invalid signature for ipc_agm_session_read_with_metadata.");
        return;
    }

    dbus_message_iter_get_basic(&arg_i, &captured_size);
    dbus_message_iter_next(&arg_i);

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &buff.timestamp);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &buff.flags);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &buff.size);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    if (n_elements < 0 || (uint32_t)n_elements > buff.size) {
        AGM_LOGE("Invalid buffer size.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid buffer size.");
        return;
    }
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &buff.metadata_size);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, metadata_value,
                                      &metadata_elements);
    if (metadata_elements < 0 ||
        (uint32_t)metadata_elements > buff.metadata_size) {
        AGM_LOGE("Invalid metadata size.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid metadata size.");
        return;
    }
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &alloc_i);
    dbus_message_iter_get_basic(&alloc_i, &buff.alloc_info.alloc_handle);
    dbus_message_iter_next(&alloc_i);
    dbus_message_iter_get_basic(&alloc_i, &buff.alloc_info.alloc_size);
    dbus_message_iter_next(&alloc_i);
    dbus_message_iter_get_basic(&alloc_i, &buff.alloc_info.offset);

    if (buff.size > 0) {
        buff.addr = (uint8_t *)calloc(1, buff.size);
        if (buff.addr == NULL) {
            AGM_LOGE("buff addr is NULL");
            agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                                "buff addr is NULL");
            return;
        }
    }

    if (buff.metadata_size > 0) {
        buff.metadata = (uint8_t *)calloc(1, buff.metadata_size);
        if (buff.metadata == NULL) {
            AGM_LOGE("metadata is NULL");
            agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                                "metadata is NULL");
            free(buff.addr);
            return;
        }
        if (metadata != NULL && metadata_elements > 0)
            memcpy(buff.metadata, metadata, metadata_elements);
    }

    if (agm_session_read_with_metadata(ses_data->handle,
                                       &buff,
                                       &captured_size)) {
        AGM_LOGE("agm_session_read_with_metadata failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_read_with_metadata failed.");
        free(buff.addr);
        free(buff.metadata);
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &r_arg);
    value = buff.addr;
    metadata = buff.metadata;
    dbus_message_iter_open_container(&r_arg, DBUS_TYPE_STRUCT, NULL,
                                     &struct_i);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT64,
                                   &buff.timestamp);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
                                   &buff.flags);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
                                   &buff.size);
    dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "y",
                                     &array_i);
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE,
                                         &value, buff.size);
    dbus_message_iter_close_container(&struct_i, &array_i);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
                                   &buff.metadata_size);
    dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "y",
                                     &array_i);
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE,
                                         &metadata, buff.metadata_size);
    dbus_message_iter_close_container(&struct_i, &array_i);
    dbus_message_iter_open_container(&struct_i, DBUS_TYPE_STRUCT, NULL,
                                     &alloc_i);
    dbus_message_iter_append_basic(&alloc_i, DBUS_TYPE_INT32,
                                   &buff.alloc_info.alloc_handle);
    dbus_message_iter_append_basic(&alloc_i, DBUS_TYPE_UINT32,
                                   &buff.alloc_info.alloc_size);
    dbus_message_iter_append_basic(&alloc_i, DBUS_TYPE_UINT32,
                                   &buff.alloc_info.offset);
    dbus_message_iter_close_container(&struct_i, &alloc_i);
    dbus_message_iter_close_container(&r_arg, &struct_i);
    dbus_message_iter_append_basic(&r_arg, DBUS_TYPE_UINT32, &captured_size);
    dbus_connection_send(conn, reply, NULL);
    free(buff.addr);
    free(buff.metadata);
    dbus_message_unref(reply);
}

static void ipc_agm_session_resume(DBusConnection *conn,
                                   DBusMessage *msg,
                                   void *userdata) {
    DBusMessage *reply = NULL;
    agm_session_data *ses_data = (agm_session_data *)userdata;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (agm_session_resume(ses_data->handle)) {
        AGM_LOGE("agm_session_resume failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_resume failed.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_pause(DBusConnection *conn,
                                  DBusMessage *msg,
                                  void *userdata) {
    DBusMessage *reply = NULL;
    agm_session_data *ses_data = (agm_session_data *)userdata;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (agm_session_pause(ses_data->handle)) {
        AGM_LOGE("agm_session_pause failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_pause failed.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_flush(DBusConnection *conn,
                                  DBusMessage *msg,
                                  void *userdata) {
    DBusMessage *reply = NULL;
    agm_session_data *ses_data = (agm_session_data *)userdata;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (agm_session_flush(ses_data->handle)) {
        AGM_LOGE("agm_session_flush failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_flush failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_suspend(DBusConnection *conn,
                                    DBusMessage *msg,
                                    void *userdata) {
    DBusMessage *reply = NULL;
    agm_session_data *ses_data = (agm_session_data *)userdata;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (agm_session_suspend(ses_data->handle)) {
        AGM_LOGE("agm_session_suspend failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_suspend failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_stop(DBusConnection *conn,
                                 DBusMessage *msg,
                                 void *userdata) {
    DBusMessage *reply = NULL;
    agm_session_data *ses_data = (agm_session_data *)userdata;

    AGM_LOGD("%s : Enter", __func__);

    if (userdata == NULL) {
        AGM_LOGE("%s :Invalid userdata", __func__);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (agm_session_stop(ses_data->handle)) {
        AGM_LOGE("%s :agm_session_stop failed.", __func__);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_stop failed.");
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    AGM_LOGD("%s : Exit", __func__);
}

static void ipc_agm_session_start(DBusConnection *conn,
                                  DBusMessage *msg,
                                  void *userdata) {
    DBusMessage *reply = NULL;
    agm_session_data *ses_data = (agm_session_data *)userdata;

    AGM_LOGD("%s:Enter\n", __func__);

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (agm_session_start(ses_data->handle)) {
        AGM_LOGE("agm_session_start failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_start failed.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    AGM_LOGD("%s:Exit\n", __func__);
}

static void ipc_agm_session_prepare(DBusConnection *conn,
                                    DBusMessage *msg,
                                    void *userdata) {
    DBusMessage *reply = NULL;
    agm_session_data *ses_data = (agm_session_data *)userdata;

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    if (agm_session_prepare(ses_data->handle)) {
        AGM_LOGE("agm_session_prepare failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_prepare failed.");
        return;
    }

    AGM_LOGV("%s : ", __func__);

    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
}

static void ipc_agm_session_close(DBusConnection *conn,
                                  DBusMessage *msg,
                                  void *userdata) {
    DBusMessage *reply = NULL;
    agm_session_data *ses_data = (agm_session_data *)userdata;
    GList *node = NULL;

    AGM_LOGD("%s : Enter", __func__);

    if (userdata == NULL) {
        AGM_LOGE("%s :Invalid userdata", __func__);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "userdata is NULL");
        return;
    }

    /* No per-session D-Bus filter to remove anymore; client-death cleanup
       is driven by the single global name_owner_changed_filter installed
       in ipc_agm_init(). */

    ses_data->thread_state = SES_THREAD_EXIT;
    pthread_cond_signal(&ses_data->cond);
    pthread_join(ses_data->ses_tid, NULL);
    pthread_cond_destroy(&ses_data->cond);
    pthread_mutex_destroy(&ses_data->lock);

    if (agm_session_close(ses_data->handle)) {
        AGM_LOGE("agm_session_close failed.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_close failed.");
        return;
    }

    for (node = ses_data->callbacks; node != NULL; node = node->next) {
        if (node->data != NULL) {
            free(node->data);
            node->data = NULL;
        }
    }

    g_list_free(ses_data->callbacks);

    if (agm_dbus_remove_interface(mdata->conn,
                                  ses_data->dbus_obj_path,
                                  session_interface_info.name)) {
        AGM_LOGE("%s:Unable to remove interface %s for session %u",
                 __func__, ses_data->dbus_obj_path, ses_data->session_id);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                       "agm_session_close failed. Failed to remove interface.");
        return;
    }

    free(ses_data->dbus_obj_path);
    ses_data->dbus_obj_path = NULL;

    g_hash_table_remove(mdata->sessions,
                        GUINT_TO_POINTER(ses_data->session_id));
    reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    AGM_LOGD("%s : Exit", __func__);
    return;
}

static void ipc_agm_session_open(DBusConnection *conn,
                                 DBusMessage *msg,
                                 void *userdata) {
    agm_module_dbus_data *mdata = (agm_module_dbus_data *)userdata;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i;
    uint32_t session_id, sess_mode;
    char *dbus_obj_path = NULL;
    gchar thread_name[32] = "";
    int32_t ret;
    agm_session_data *ses_data = NULL;

    AGM_LOGD("%s :Enter ", __func__);

    if (userdata == NULL) {
        AGM_LOGE("Invalid userdata");
        return;
    }

    if (!dbus_message_iter_init(msg, &arg_i)) {
        AGM_LOGE("ipc_agm_session_open has no arguments");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "ipc_agm_session_open has no arguments");
        return;
    }

    if (strcmp(dbus_message_get_signature(msg), "uu")) {
        AGM_LOGE("Invalid signature for ipc_agm_session_open.");
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "Invalid signature for ipc_agm_session_open.");
        return;
    }
    dbus_message_iter_get_basic(&arg_i, &session_id);
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_get_basic(&arg_i, &sess_mode);
    gboolean newly_created = (g_hash_table_lookup(mdata->sessions,
                                                   GUINT_TO_POINTER(session_id)) == NULL);
    ses_data = get_session_data(mdata, session_id);
    if (ses_data == NULL) {
        AGM_LOGE("%s: get_session_data failed for session %u", __func__, session_id);
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "get_session_data failed.");
        return;
    }

    if (ses_data == NULL) {
        AGM_LOGE("get_session_data failed");
        agm_dbus_send_error(mdata->conn,
                            msg, DBUS_ERROR_FAILED, "ses_data is NULL, get_session_data failed");
        return;
    }

    /* Record the D-Bus unique name of the caller so the global
       name_owner_changed_filter can match this session when the client
       vanishes (SIGKILL, segfault, OOM-kill). Allocate the candidate
       first; only commit it to ses_data->owner_unique_name after the
       whole open path (agm_session_open + pthread_create) has
       succeeded, so a mid-way failure cannot destroy the original
       owner of a re-opened session. */
    char *new_owner = NULL;
    {
        const char *sender = dbus_message_get_sender(msg);
        if (sender != NULL) {
            new_owner = strdup(sender);
            if (new_owner == NULL) {
                AGM_LOGE("%s: strdup(sender=%s) failed for session %u",
                         __func__, sender, session_id);
                if (newly_created)
                    g_hash_table_remove(mdata->sessions,
                                        GUINT_TO_POINTER(session_id));
                agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_NO_MEMORY,
                                    "strdup(sender) failed");
                return;
            }
        } else {
            AGM_LOGE("%s: dbus_message_get_sender returned NULL for session %u",
                     __func__, session_id);
        }
    }

    if (agm_session_open(session_id, (enum agm_session_mode) sess_mode, &ses_data->handle)) {
        free(new_owner);  /* candidate not committed, keep existing owner */
        if (newly_created)
            g_hash_table_remove(mdata->sessions, GUINT_TO_POINTER(session_id));
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "agm_session_open failed.");
        return;
    }

    pthread_mutex_init(&ses_data->lock, NULL);
    pthread_cond_init(&ses_data->cond, NULL);
    ses_data->thread_state = SES_THREAD_IDLE;
    snprintf(ses_data->eventType, sizeof("Wait"), "%s", "Wait");
    AGM_LOGD("%s:Wait Event", __func__);
    snprintf(thread_name, sizeof(thread_name), "agm_ses_async_thread_%d", session_id);
    if (pthread_create(&ses_data->ses_tid, NULL, async_thread_func, ses_data)) {
        AGM_LOGE("%s: agm session async thread creation failed", __func__);
        /* Full rollback: the session is unusable without a worker.
         * Tear down everything we built so far and fail the open
         * request so the client knows the session is not live. */
        ses_data->ses_tid = 0;
        pthread_cond_destroy(&ses_data->cond);
        pthread_mutex_destroy(&ses_data->lock);
        if (agm_session_close(ses_data->handle) != 0)
            AGM_LOGE("%s: agm_session_close during rollback failed sid=%u",
                     __func__, session_id);
        free(new_owner);
        if (newly_created)
            g_hash_table_remove(mdata->sessions, GUINT_TO_POINTER(session_id));
        agm_dbus_send_error(mdata->conn, msg, DBUS_ERROR_FAILED,
                            "pthread_create(async_thread_func) failed");
        return;
    }
    AGM_LOGD("%s : agm session async thread creation success", __func__);

    /* Everything that can fail has succeeded; commit the owner. */
    if (new_owner != NULL) {
        if (ses_data->owner_unique_name != NULL)
            free(ses_data->owner_unique_name);
        ses_data->owner_unique_name = new_owner;
    }

    /* No per-session dbus_connection_add_filter here anymore. Client-death
       cleanup is handled centrally by name_owner_changed_filter, which was
       installed once in ipc_agm_init() and demuxes to the right session
       via ses_data->owner_unique_name. */

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_append_basic(&arg_i,
                                   DBUS_TYPE_OBJECT_PATH,
                                   &ses_data->dbus_obj_path);
    dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    AGM_LOGD("%s :Exit ", __func__);
    return;
}

/**
 * AGM SSR callback registered with the AGM SSR manager.
 * Broadcasts an AgmSsrEventCb D-Bus signal to all connected clients
 * whenever the audio subsystem goes down or comes back up.
 */
static void agm_ssr_event_cb(enum agm_ssr_event_type event_type,
                             void *client_data __unused)
{
    DBusMessage *message = NULL;
    DBusMessageIter arg_i;
    uint32_t evt = (uint32_t)event_type;

    if (!mdata || !mdata->conn) {
        AGM_LOGE("%s: mdata not initialised, dropping SSR event %u",
                 __func__, evt);
        return;
    }

    AGM_LOGI("%s: broadcasting SSR event %u (%s)", __func__, evt,
             (event_type == AGM_SSR_EVENT_AUDIO_SVC_DN) ? "DOWN" : "UP");

    message = dbus_message_new_signal(mdata->dbus_obj_path,
                                      module_interface_info.name,
                                      module_signals[AgmSsrEventCb].method_name);
    if (!message) {
        AGM_LOGE("%s: failed to create SSR signal message", __func__);
        return;
    }

    dbus_message_iter_init_append(message, &arg_i);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT32, &evt);

    agm_dbus_send_signal(mdata->conn, message);
    dbus_message_unref(message);
}

/* Initialize module data. Get dbus connection and register module interface
    with the connection */
int ipc_agm_init() {
    DBusError err;
    int rc = 0;

    AGM_LOGV("%s : ", __func__);

    mdata = (agm_module_dbus_data *)malloc(sizeof(agm_module_dbus_data));
    if (mdata == NULL) {
        AGM_LOGE("mdata is NULL");
        rc = -EINVAL;
        return rc;
    }
    /* Zero fields the fail label touches. */
    mdata->sessions = NULL;
    mdata->shmem_fd_map = NULL;
    mdata->conn = NULL;
    mdata->dbus_obj_path = NULL;
    mdata->dbus_obj_path =
                    (char *)malloc(sizeof(char)*(strlen(AGM_OBJECT_PATH) + 1));
    if (mdata->dbus_obj_path == NULL) {
        AGM_LOGE("dbus_obj_path is NULL");
        free(mdata);
        mdata = NULL;
        rc = -EINVAL;
        return rc;
    }
    memcpy(mdata->dbus_obj_path, AGM_OBJECT_PATH, strlen(AGM_OBJECT_PATH)+1);

    mdata->conn = agm_dbus_new_connection();

    if (mdata->conn == NULL) {
        AGM_LOGE("Failed to request name on bus");
        rc = -EINVAL;
        goto fail;
    }

    dbus_error_init(&err);
    rc = dbus_bus_request_name(mdata->conn->conn,
                               AGM_DBUS_CONNECTION,
                               DBUS_NAME_FLAG_REPLACE_EXISTING,
                               &err);

    if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        AGM_LOGE("Failed to request name on bus: %s", err.message);
        rc = -EINVAL;
        goto fail;
    }

    /* Install the client-death watcher before glib takes over dispatch,
     * so dbus_bus_add_match() can use the blocking path (&err) safely. */
    mdata->sessions = g_hash_table_new_full(g_direct_hash,
                                            g_direct_equal,
                                            NULL,
                                            agm_free_session);
    mdata->shmem_fd_map = g_hash_table_new_full(g_direct_hash,
                                                g_direct_equal,
                                                NULL,
                                                agm_shmem_fd_close_value);

    dbus_bus_add_match(mdata->conn->conn,
                       "type='signal',"
                       "sender='org.freedesktop.DBus',"
                       "interface='org.freedesktop.DBus',"
                       "member='NameOwnerChanged'",
                       &err);
    if (dbus_error_is_set(&err)) {
        AGM_LOGE("%s: dbus_bus_add_match(NameOwnerChanged) failed: %s",
                 __func__, err.message);
        dbus_error_free(&err);
        rc = -EINVAL;
        goto fail;
    }

    if (!dbus_connection_add_filter(mdata->conn->conn,
                                    name_owner_changed_filter,
                                    NULL,
                                    NULL)) {
        AGM_LOGE("%s: dbus_connection_add_filter(name_owner_changed_filter) "
                 "failed; client-death cleanup disabled", __func__);
        rc = -EINVAL;
        goto fail;
    }
    AGM_LOGI("%s: NameOwnerChanged watcher installed", __func__);

    if (agm_setup_dbus_with_main_loop(mdata->conn)) {
        AGM_LOGE("Failed to setup main loop");
        rc = -EINVAL;
        goto fail;
    }

    if ((rc = agm_dbus_add_interface(mdata->conn,
                                     mdata->dbus_obj_path,
                                     &module_interface_info,
                                     mdata)) != 0) {
        AGM_LOGE("Failed to add interface");
        rc = -EINVAL;
        goto fail;
    }

    if ((rc = agm_init()) != 0) {
        AGM_LOGE("agm initialization failed");
        goto fail;
    }
    dbus_error_free(&err);
    return rc;
fail:
    if (mdata->sessions != NULL) {
        g_hash_table_unref(mdata->sessions);
        mdata->sessions = NULL;
    }
    if (mdata->shmem_fd_map != NULL) {
        g_hash_table_unref(mdata->shmem_fd_map);
        mdata->shmem_fd_map = NULL;
    }
    if (mdata->conn != NULL)
        agm_dbus_connection_free(mdata->conn);
    free(mdata->dbus_obj_path);
    mdata->dbus_obj_path = NULL;
    free(mdata);
    mdata = NULL;
    return rc;
}

int ipc_agm_ssr_register(void) {
    int rc = agm_register_ssr_cb(agm_ssr_event_cb, NULL);
    if (rc != 0)
        AGM_LOGE("%s: agm_register_ssr_cb failed rc=%d", __func__, rc);
    return rc;
}

void ipc_agm_ssr_deregister(void) {
    agm_deregister_ssr_cb(agm_ssr_event_cb, NULL);
}

void ipc_agm_deinit() {
    AGM_LOGV("%s : ", __func__);

    if (mdata == NULL) {
        AGM_LOGE("ipc_agm_deinit failed: mdata is NULL");
        return;
    }

    /* Drop the filter so it doesn't fire during cleanup. The match rule
     * on dbus-daemon is cleaned up when we close the connection. */
    if (mdata->conn != NULL && mdata->conn->conn != NULL) {
        dbus_connection_remove_filter(mdata->conn->conn,
                                      name_owner_changed_filter,
                                      NULL);
    }

    if (mdata->sessions != NULL) {
        g_hash_table_remove_all(mdata->sessions);
        g_hash_table_unref(mdata->sessions);
        mdata->sessions = NULL;
    }
    if (mdata->shmem_fd_map != NULL) {
        g_hash_table_remove_all(mdata->shmem_fd_map);
        g_hash_table_unref(mdata->shmem_fd_map);
        mdata->shmem_fd_map = NULL;
    }

    if (mdata->dbus_obj_path != NULL) {
        if (agm_dbus_remove_interface(mdata->conn,
                                      mdata->dbus_obj_path,
                                      module_interface_info.name) != 0)
            AGM_LOGE("Unable to remove interface");
        free(mdata->dbus_obj_path);
        mdata->dbus_obj_path = NULL;
    }

    agm_dbus_connection_free(mdata->conn);
    free(mdata);
    mdata = NULL;

    if (agm_deinit() != 0)
        AGM_LOGE("agm deinitialization failed");
}
