/*
** Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
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
**
** Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
** SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#define LOG_TAG "PLUGIN: AGMIO"
#define DUMP_OPEN 0
#include <stdio.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <alsa/pcm.h>
#include <agm/agm_api.h>
#include <agm/agm_list.h>
#include <snd-card-def.h>
#include "utils.h"
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#if DUMP_OPEN
#include <alsa/pcm.h>
#endif

#define ARRAY_SIZE(a)   (sizeof(a)/sizeof(a[0]))
#define DUMP_BUFFER 1024
static char* dump_file_name;

/* Add a listening thread for SSR events for aplay */
#define SNDCARD_PATH "/sys/kernel/snd_card/card_state"
static void* card_status_monitor(void *arg);

/* pull-push mode macros */
#define AGM_PULL_PUSH_IDX_RETRY_COUNT 2
#define AGM_PULL_PUSH_FRAME_CNT_RETRY_COUNT 5

enum {
    AGM_IO_STATE_XRUN = -1,
    AGM_IO_STATE_OPEN = 1,
    AGM_IO_STATE_SETUP,
    AGM_IO_STATE_PREPARED,
    AGM_IO_STATE_RUNNING,
};

struct agmio_shared_pos_buffer {
    volatile uint32_t frame_counter;
    volatile uint32_t read_index;
    volatile uint32_t wall_clock_us_lsw;
    volatile uint32_t wall_clock_us_msw;
};

struct pcm_plugin_pos_buf_info {
    void *pos_buf_addr;
    snd_pcm_uframes_t circ_buf_pos;
    snd_pcm_uframes_t hw_ptr_base;
};

struct agmio_priv {
    snd_pcm_ioplug_t io;

    int card;
    int device;

    int session_id;
    void *card_node;
    void *pcm_node;
    uint64_t handle;
    struct agm_media_config *media_config;
    struct agm_buffer_config *buffer_config;
    struct agm_session_config *session_config;
    unsigned int period_size;
    size_t frame_size;
    int state;
    snd_pcm_uframes_t hw_pointer;
    snd_pcm_uframes_t boundary;
    int event_fd;
    pthread_cond_t eos_cond;
    pthread_mutex_t eos_lock;
    bool eos;
    struct agm_buf_info *buf_info;
    struct pcm_plugin_pos_buf_info *pos_buf;
    pthread_t monitor_thread;
    int SSR_RUNNING;
/* add private variables here */
};

static int agm_io_plugin_get_shared_pos(struct pcm_plugin_pos_buf_info *pos_buf,
    uint32_t *read_index)
{
    struct agmio_shared_pos_buffer *buf;
    int i, j;
    uint32_t frame_cnt1, frame_cnt2;

    buf = (struct agmio_shared_pos_buffer*)pos_buf->pos_buf_addr;
    for (i = 0; i < AGM_PULL_PUSH_IDX_RETRY_COUNT; ++i) {
        for (j = 0; j < AGM_PULL_PUSH_FRAME_CNT_RETRY_COUNT; ++j) {
            frame_cnt1 = buf->frame_counter;
            if (frame_cnt1 != 0)
                break;
        }
        *read_index = buf->read_index; /* 0,.... Circ_buf_size-1 */
        frame_cnt2 = buf->frame_counter;

        if (frame_cnt1 != frame_cnt2)
            continue;

        return 0;
    }

    return -EAGAIN;
}

static int agm_get_session_handle(struct agmio_priv *priv,
                                  uint64_t *handle)
{
    if (!priv)
        return -EINVAL;

    *handle = priv->handle;
    if (!*handle)
        return -EINVAL;
    return 0;
}

static int agm_io_stop(snd_pcm_ioplug_t * io)
{
    struct agmio_priv *pcm = io->private_data;
    uint64_t handle;
    int ret;

    pthread_mutex_lock(&pcm->eos_lock);
    if (pcm->eos) {
          pthread_cond_wait(&pcm->eos_cond, &pcm->eos_lock);
    }
    pthread_mutex_unlock(&pcm->eos_lock);
    ret = agm_get_session_handle(pcm, &handle);
    if (ret)
        return ret;
    ret = agm_session_stop(handle);
    if(DUMP_OPEN)
        free(dump_file_name);
    AGM_LOGD("%s: exit\n", __func__);
    return ret;
}

static void agm_io_xrun(snd_pcm_ioplug_t * io)
{
    struct agmio_priv *pcm = io->private_data;

    agm_io_stop(io);
    pcm->state = AGM_IO_STATE_XRUN;
}

void agm_pcm_event_cb(uint32_t session_id __unused,
                           struct agm_event_cb_params *event_params,
                           void *client_data)
{
    struct agmio_priv *priv = (struct agmio_priv *)client_data;
    snd_pcm_sframes_t new_circ_buf_pos, hw_base;
    uint32_t read_index;

    if (!priv) {
        AGM_LOGE("%s: Private data is NULL\n", __func__);
        return;
    }
    if (!event_params) {
        AGM_LOGE("%s: event params is NULL\n", __func__);
        return;
    }

    if (event_params->event_id == AGM_EVENT_WRITE_DONE) {
        /*
         * Write done cb is expected for every DSP write with
         * fragment size even for partial buffers
         */
        priv->hw_pointer += priv->period_size;
        if (priv->hw_pointer > priv->boundary)
            priv->hw_pointer -= priv->boundary;
        eventfd_write(priv->event_fd, 1);
    } else if (event_params->event_id == AGM_EVENT_READ_DONE) {
        /* Read done cb expected for every DSP read with Fragment size */
        priv->hw_pointer += priv->period_size;
        if (priv->hw_pointer > priv->boundary)
            priv->hw_pointer -= priv->boundary;
        eventfd_write(priv->event_fd, 1);
    } else if (event_params->event_id == AGM_EVENT_EOS_RENDERED) {
        AGM_LOGD("%s: EOS event received \n", __func__);
        pthread_mutex_lock(&priv->eos_lock);
        if (priv->eos) {
            pthread_cond_signal(&priv->eos_cond);
            priv->eos = false;
        }
        pthread_mutex_unlock(&priv->eos_lock);
    } else if (event_params->event_id == AGM_EVENT_EARLY_EOS) {
        AGM_LOGD("%s: Early EOS event received \n", __func__);
    } else if (event_params->event_id == AGM_EVENT_PULL_PUSH_MODE_WATERMARK) {
        AGM_LOGD("%s: AGM_EVENT_PULL_PUSH_MODE_WATERMARK event received \n", __func__);
        agm_io_plugin_get_shared_pos(priv->pos_buf, &read_index);
        new_circ_buf_pos = read_index / priv->frame_size;
        hw_base = priv->pos_buf->hw_ptr_base;
        if (new_circ_buf_pos < priv->pos_buf->circ_buf_pos) {
            hw_base += priv->buffer_config->count * priv->period_size;
            if (hw_base > priv->boundary)
                hw_base -= priv->boundary;
            priv->pos_buf->hw_ptr_base = hw_base;
        }
        priv->pos_buf->circ_buf_pos = new_circ_buf_pos;
        priv->hw_pointer = hw_base + new_circ_buf_pos;
        if (priv->hw_pointer > priv->boundary)
            priv->hw_pointer -= priv->boundary;
        eventfd_write(priv->event_fd, 1);
    }  else if (event_params->event_id == AGM_EVENT_UNDERRUN) {
        AGM_LOGE("%s: detect underrun event happen \n", __func__);
        agm_io_xrun(&priv->io);
    }  else if (event_params->event_id == AGM_EVENT_OVERRUN) {
        AGM_LOGE("%s: detect overrun event happen \n", __func__);
        agm_io_xrun(&priv->io);
    } else {
        AGM_LOGE("%s: error: Invalid event params id: %u\n", __func__,
           event_params->event_id);
    }
}

static int agm_io_start(snd_pcm_ioplug_t * io)
{
    struct agmio_priv *pcm = io->private_data;
    uint64_t handle;
    int ret;
    if(DUMP_OPEN)
        dump_file_name = malloc(DUMP_BUFFER);
    ret = agm_get_session_handle(pcm, &handle);
    if (ret)
        return ret;

    if (pcm->state < AGM_IO_STATE_PREPARED) {
        ret = agm_session_prepare(handle);
        errno = ret;
        if (ret)
            return ret;
        pcm->state = AGM_IO_STATE_PREPARED;
    }

    if (pcm->state != AGM_IO_STATE_RUNNING) {
        ret = agm_session_start(handle);
        if (!ret)
            pcm->state = AGM_IO_STATE_RUNNING;
    }

    AGM_LOGD("%s: exit\n", __func__);
    return ret;
}

static int agm_io_drain(snd_pcm_ioplug_t *io)
{
    struct agmio_priv *pcm = io->private_data;
    uint64_t handle;
    int ret = 0;

    if (io->mmap_rw) {
        AGM_LOGE("%s: No need EOS for mmap mode\n", __func__);
        return ret;
    }

    ret = agm_get_session_handle(pcm, &handle);
    if (ret)
        return ret;

    pthread_mutex_lock(&pcm->eos_lock);
    ret = agm_session_eos(handle);
    if (ret) {
        AGM_LOGE("%s: EOS fail\n", __func__);
        pthread_mutex_unlock(&pcm->eos_lock);
        return ret;
    }
    pcm->eos = true;
    pthread_mutex_unlock(&pcm->eos_lock);
    AGM_LOGD("%s: exit\n", __func__);
    return 0;
}

static snd_pcm_sframes_t agm_io_pointer(snd_pcm_ioplug_t *io)
{
    struct agmio_priv *pcm = io->private_data;
    snd_pcm_sframes_t new_hw_ptr;

    if(pcm->state == AGM_IO_STATE_XRUN)
        return -EPIPE;

    new_hw_ptr = pcm->hw_pointer;

    return new_hw_ptr;
}

static snd_pcm_sframes_t agm_io_transfer(snd_pcm_ioplug_t * io,
                                     const snd_pcm_channel_area_t * areas,
                                     snd_pcm_uframes_t offset,
                                     snd_pcm_uframes_t size)
{
    struct agmio_priv *pcm = io->private_data;
    uint64_t handle;
    uint8_t *buf = (uint8_t *) areas->addr + (areas->first + areas->step * offset) / 8;
    size_t count;
    int ret = 0;
    int sess_mode = 0;

    ret = agm_get_session_handle(pcm, &handle);
    if (ret)
        return ret;

    if (pcm->state != AGM_IO_STATE_RUNNING) {
        ret = agm_io_start(io);
        if (ret)
            return ret;
    }

    count = size * pcm->frame_size;

    snd_card_def_get_int(pcm->pcm_node, "session_mode", &sess_mode);
    if (sess_mode == AGM_SESSION_NO_HOST) {
        AGM_LOGE("%s: no data transfer for hostless session, exit\n", __func__);
        goto done;
    }
    if (io->stream == SND_PCM_STREAM_PLAYBACK)
        ret = agm_session_write(handle, buf, &count);
    else {
        ret = agm_session_read(handle, buf, &count);
        if (io->appl_ptr != 0 && count != 0 && count < size * pcm->frame_size) {
            AGM_LOGE("XRUN happen! reqested size %lu, actual filled size %lu", size * pcm->frame_size, count);
            agm_io_xrun(io);
            ret = -EPIPE;
        }
    }

    //write into file
    if (DUMP_OPEN) {
        snprintf(dump_file_name,100,"/data/test_session_id_%d_device_%d_rate_%u",pcm->session_id, pcm->device, pcm->media_config->rate);
        AGM_LOGE("%s: dump_file_name = %s \n", __func__, dump_file_name);
        FILE *fp = fopen(dump_file_name, "a+");
        if (fp) {
            int fwrite_len = fwrite((char *)buf, 1, count, fp);
            if(!fwrite_len){
                AGM_LOGE("%s: dump data write size is 0!!!\n", __func__);
            }
            fclose(fp);
        }else {
            AGM_LOGE("%s: open fail \n", __func__);
        }
    }

done:
    if (ret == 0) {
        ret = snd_pcm_bytes_to_frames(io->pcm, count);
    }

    if (pcm->hw_pointer > pcm->boundary)
         pcm->hw_pointer -= pcm->boundary;

    AGM_LOGD("%s: exit\n", __func__);
    return ret;
}

static int agm_io_prepare(snd_pcm_ioplug_t * io)
{
    uint64_t handle;
    struct agmio_priv *pcm = io->private_data;
    int ret = 0;

    ret = agm_get_session_handle(pcm, &handle);
    if (ret)
        return ret;

    ret = agm_session_prepare(handle);
    if (ret)
        return ret;
    pcm->hw_pointer = 0;
    pcm->state = AGM_IO_STATE_PREPARED;

    AGM_LOGD("%s: exit\n", __func__);
    return ret;
}

static enum agm_media_format alsa_to_agm_fmt(int fmt)
{
    enum agm_media_format agm_pcm_fmt = AGM_FORMAT_INVALID;

    switch (fmt) {
    case SND_PCM_FORMAT_S8:
        agm_pcm_fmt = AGM_FORMAT_PCM_S8;
        break;
    case SND_PCM_FORMAT_S16_LE:
        agm_pcm_fmt = AGM_FORMAT_PCM_S16_LE;
        break;
    case SND_PCM_FORMAT_S24_LE:
        agm_pcm_fmt = AGM_FORMAT_PCM_S24_LE;
        break;
    case SND_PCM_FORMAT_S24_3LE:
        agm_pcm_fmt = AGM_FORMAT_PCM_S24_3LE;
        break;
    case SND_PCM_FORMAT_S32_LE:
        agm_pcm_fmt = AGM_FORMAT_PCM_S32_LE;
        break;
    default:
        AGM_LOGE("%s: Unsupport format\n", __func__);
        break;
    }

    return agm_pcm_fmt;
}

static int agm_io_hw_params(snd_pcm_ioplug_t * io,
                           snd_pcm_hw_params_t * params)
{
    struct agmio_priv *pcm = io->private_data;
    struct agm_media_config *media_config;
    struct agm_buffer_config *buffer_config;
    struct agm_session_config *session_config = NULL;
    uint64_t handle;
    int ret = 0, sess_mode = 0;

    ret = agm_get_session_handle(pcm, &handle);

    pcm->frame_size = (snd_pcm_format_physical_width(io->format) * io->channels) / 8;

    media_config = pcm->media_config;
    buffer_config = pcm->buffer_config;
    session_config = pcm->session_config;

    media_config->rate =  io->rate;
    media_config->channels = io->channels;
    media_config->format = alsa_to_agm_fmt(io->format);

    buffer_config->count = io->buffer_size / io->period_size;
    if (io->buffer_size != io->period_size * buffer_config->count)
    {
        AGM_LOGE("%s: buffer_size[%lu] is not multiple times of period_size[%lu]!\n", __func__, io->buffer_size, io->period_size);
        return -EINVAL;
    }

    pcm->period_size = io->period_size;
    buffer_config->size = io->period_size * pcm->frame_size;

    snd_card_def_get_int(pcm->pcm_node, "session_mode", &sess_mode);

    session_config->dir = (io->stream == SND_PCM_STREAM_PLAYBACK) ? RX : TX;
    session_config->sess_mode = sess_mode;
    ret = agm_session_set_config(pcm->handle, session_config,
                                 pcm->media_config, pcm->buffer_config);
    if (!ret)
        pcm->state = AGM_IO_STATE_SETUP;

    AGM_LOGD("%s: exit\n", __func__);
    return ret;
}

static int agm_io_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params)
{
    struct agmio_priv *pcm = io->private_data;
    struct agm_session_config *session_config = NULL;
    uint64_t handle = 0;
    int ret = 0, sess_mode = 0, data_mode = 0;
    snd_pcm_uframes_t start_threshold;
    snd_pcm_uframes_t stop_threshold;

    ret = agm_get_session_handle(pcm, &handle);
    if (ret)
        return ret;
    session_config = pcm->session_config;

    snd_card_def_get_int(pcm->pcm_node, "session_mode", &sess_mode);
    if (io->mmap_rw) {
        session_config->data_mode = AGM_DATA_PUSH_PULL;
    } else {
        snd_card_def_get_int(pcm->pcm_node, "agm_data_mode", &data_mode);
        session_config->data_mode = data_mode;
    }
    session_config->dir = (io->stream == SND_PCM_STREAM_PLAYBACK) ? RX : TX;
    session_config->sess_mode = sess_mode;
    snd_pcm_sw_params_get_start_threshold(params, &start_threshold);
    snd_pcm_sw_params_get_stop_threshold(params, &stop_threshold);
    snd_pcm_sw_params_get_boundary(params, &pcm->boundary);
    snd_pcm_sw_params_set_tstamp_type((snd_pcm_t*)pcm, params, SND_PCM_TSTAMP_TYPE_MONOTONIC);
    session_config->start_threshold = (uint32_t)start_threshold;
    session_config->stop_threshold = (uint32_t)stop_threshold;
    ret = agm_session_set_config(pcm->handle, session_config,
                                 pcm->media_config, pcm->buffer_config);

    AGM_LOGD("%s: exit\n", __func__);
    return ret;
}

static int agm_io_close(snd_pcm_ioplug_t * io)
{
    struct agmio_priv *pcm = io->private_data;
    uint64_t handle;
    int ret = 0;

    ret = agm_get_session_handle(pcm, &handle);
    if (ret)
        return ret;
    ret = agm_session_register_cb(pcm->session_id, NULL,
              AGM_EVENT_DATA_PATH, (void *)pcm);
    ret = agm_session_close(handle);

    pthread_cancel(pcm->monitor_thread);
    pthread_join(pcm->monitor_thread, NULL);

    snd_card_def_put_card(pcm->card_node);
    close(pcm->event_fd);
    free(pcm->buffer_config);
    free(pcm->media_config);
    free(pcm->session_config);
    free(io->private_data);

    AGM_LOGD("%s: exit\n", __func__);
    return 0;
}

static int agm_io_pause(snd_pcm_ioplug_t * io, int enable)
{
    struct agmio_priv *pcm = io->private_data;
    uint64_t handle;
    int ret = 0;

     ret = agm_get_session_handle(pcm, &handle);
     if (ret)
         return ret;

     if (enable)
         ret = agm_session_pause(handle);
     else
         ret = agm_session_resume(handle);

     AGM_LOGD("%s: exit\n", __func__);
     return ret;
}

static int agm_io_mmap(snd_pcm_ioplug_t *io)
{
    struct agmio_priv *pcm = io->private_data;
    struct agm_buf_info* buf_info = NULL;
    struct pcm_plugin_pos_buf_info *pos = NULL;
    uint64_t handle;
    int flag = DATA_BUF|POS_BUF;
    int ret = 0;

    ret = agm_get_session_handle(pcm, &handle);
    if (ret)
        return ret;

    if (!pcm->buf_info) {
        buf_info = calloc(1, sizeof(struct agm_buf_info));
        if (!buf_info)
            return -ENOMEM;

        ret = agm_session_get_buf_info(pcm->session_id, buf_info, flag);
        if (ret) {
            free(buf_info);
            return ret;
        }
        pcm->buf_info = buf_info;
    }

    if (io->mmap_rw) {
        if (!pcm->pos_buf) {
            pos = calloc(1, sizeof(struct pcm_plugin_pos_buf_info));
            if (!pos) {
                free(buf_info);
                return -ENOMEM;
            }

            pos->pos_buf_addr = mmap(0, pcm->buf_info->pos_buf_size,
                PROT_READ | PROT_WRITE, MAP_SHARED,
                pcm->buf_info->pos_buf_fd, 0);

            pcm->pos_buf = pos;
        }
    }

    AGM_LOGD("%s: exit\n", __func__);
    return ret;
}

static int agm_io_munmap(snd_pcm_ioplug_t *io)
{
    struct agmio_priv* pcm = io->private_data;
    uint64_t handle;
    int ret = 0;

    if (io->mmap_rw) {
        if (pcm->pos_buf) {
            munmap(pcm->pos_buf->pos_buf_addr,
                pcm->buf_info->pos_buf_size);
            free(pcm->pos_buf);
            pcm->pos_buf = NULL;
        }
        if (pcm->buf_info) {
            if (pcm->buf_info->data_buf_fd != -1)
                close(pcm->buf_info->data_buf_fd);
            free(pcm->buf_info);
       }
    }

    AGM_LOGD("%s: exit\n", __func__);
    return ret;
}

static int agm_io_channel_info(snd_pcm_ioplug_t *io, int *fd)
{
    struct agmio_priv *pcm = io->private_data;
    uint64_t handle;
    int ret = 0;

    if (io->mmap_rw) {
        *fd = pcm->buf_info->data_buf_fd;
    }

    AGM_LOGD("%s: exit\n", __func__);
    return ret;
}

static int agm_io_poll_desc_count(snd_pcm_ioplug_t *io) {
    (void)io;
    /* TODO : Needed for ULL usecases */
    AGM_LOGD("%s: exit\n", __func__);
    return 1;
}

static int agm_io_poll_desc(snd_pcm_ioplug_t *io, struct pollfd *pfd,
                            unsigned int space)
{
    struct agmio_priv *pcm = io->private_data;

    /* TODO : Needed for ULL usecases, Need update */
    if (space != 1) {
        AGM_LOGE("%s space %u is not correct!\n", __func__, space);
        return -EINVAL;
    }

    pfd[0].fd = pcm->event_fd;
    pfd[0].events = POLLIN;

    AGM_LOGD("%s: exit\n", __func__);
    return space;
}

static int agm_io_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
                               unsigned int nfds, unsigned short *revents)
{
    struct agmio_priv *pcm = io->private_data;
    eventfd_t evfd;
    /* TODO : Needed for ULL usecases, Need update */
    if (nfds != 1) {
        AGM_LOGE("%s nfds %u is not correct!\n", __func__, nfds);
        return -EINVAL;
    }

    if (io->stream == SND_PCM_STREAM_PLAYBACK) {
        *revents = POLLOUT;
    } else {
        *revents = POLLIN;
    }

    eventfd_read(pcm->event_fd, &evfd);
    /*stop aplay or arecord when SSR_RUNNING*/
    if (pcm->SSR_RUNNING==1){
        AGM_LOGE("SSR detected , aplay return error and exit !");
        return -EINVAL ;
    }
    AGM_LOGD("%s: exit\n", __func__);
    return 0;
}

static const snd_pcm_ioplug_callback_t agm_io_callback = {
    .start = agm_io_start,
    .stop = agm_io_stop,
    .pointer = agm_io_pointer,
    .drain = agm_io_drain,
    .transfer = agm_io_transfer,
    .prepare = agm_io_prepare,
    .hw_params = agm_io_hw_params,
    .sw_params = agm_io_sw_params,
    .close = agm_io_close,
    .pause = agm_io_pause,
    .poll_descriptors_count = agm_io_poll_desc_count,
    .poll_descriptors = agm_io_poll_desc,
    .poll_revents = agm_io_poll_revents,
    .mmap = agm_io_mmap,
    .munmap = agm_io_munmap,
    .channel_info = agm_io_channel_info,
};

static int agm_hw_constraint(struct agmio_priv* priv)
{
    snd_pcm_ioplug_t *io = &priv->io;
    int ret;

    static const snd_pcm_access_t access_list[] = {
        SND_PCM_ACCESS_RW_INTERLEAVED,
        SND_PCM_ACCESS_MMAP_INTERLEAVED
    };
    static const unsigned int formats[] = {
        SND_PCM_FORMAT_U8,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_S32_LE,
        SND_PCM_FORMAT_S24_3LE,
        SND_PCM_FORMAT_S24_LE,
    };

    ret = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
                                        ARRAY_SIZE(access_list),
                                        access_list);
    if (ret < 0)
        return ret;

    ret = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
                                        ARRAY_SIZE(formats), formats);
    if (ret < 0)
        return ret;

    ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS,
                                          1, 32);
    if (ret < 0)
            return ret;

    ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE,
                                          8000, 384000);
    if (ret < 0)
            return ret;

    ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
                                          64, 122880);
    if (ret < 0)
            return ret;

    ret = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS,
                                          1, 8);
    if (ret < 0)
            return ret;

    return 0;
}

void cleanup_sndcard_fd(void *fd_ptr){
    int fd = *(int *)fd_ptr;
    if(fd >=0) {
        close(fd);
    }
}

static void* card_status_monitor(void *arg) {
    struct agmio_priv *priv = (struct agmio_priv *)arg;
    int fd = -1;
    char buf[2] = {0};
    struct pollfd pfd = {0};

    if ((fd = open(SNDCARD_PATH, O_RDONLY)) < 0) {
        AGM_LOGE("Open %s failed: %s", SNDCARD_PATH, strerror(errno));
        return NULL;
    }

    pthread_cleanup_push(cleanup_sndcard_fd,&fd);

    pfd.fd = fd;
    pfd.events = POLLPRI;
    while (1) {
        if (poll(&pfd, 1, 100) < 0) {
            if (errno == EINTR){
                continue;
            } 
            break;
        }

        lseek(fd, 0, SEEK_SET);
        if (read(fd, buf, sizeof(buf)) <= 0) {
            AGM_LOGE("read %s failed ,exit thread ",SNDCARD_PATH);
            break;
        }

        if (buf[0] == '0') {
            priv->SSR_RUNNING = 1;
            eventfd_write(priv->event_fd, 1);
            AGM_LOGE("SSR trigger, card status in CARD_STATUS_OFFLINE !");
            break;
        }
    }
    pthread_cleanup_pop(1);
    return NULL;
}
SND_PCM_PLUGIN_DEFINE_FUNC(agm)
{
    snd_config_iterator_t it, next;
    struct agmio_priv *priv = NULL;
    long card = 0, device = 100;
    struct agm_session_config *session_config;
    struct agm_media_config *media_config;
    struct agm_buffer_config *buffer_config;
    void *card_node, *pcm_node;
    enum agm_session_mode sess_mode = AGM_SESSION_DEFAULT;
    uint64_t handle;
    int ret = 0, session_id = device;

    priv = calloc(1, sizeof(*priv));
    if (!priv)
        return -ENOMEM;

    media_config = calloc(1, sizeof(struct agm_media_config));
    if (!media_config) {
        ret = -ENOMEM;
        goto err_free_priv;
    }

    buffer_config = calloc(1, sizeof(struct agm_buffer_config));
    if (!buffer_config) {
        ret = -ENOMEM;
        goto err_free_media;
    }

    session_config = calloc(1, sizeof(struct agm_session_config));
    if (!session_config) {
        ret = -ENOMEM;
        goto err_free_buf;
    }

    snd_config_for_each(it, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(it);
        const char *id;

        if (snd_config_get_id(n, &id) < 0)
            continue;
        if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
            continue;
        if (strcmp(id, "card") == 0) {
            if (snd_config_get_integer(n, &card) < 0) {
                AGM_LOGE("Invalid type for %s", id);
                ret = -EINVAL;
                goto err_free_session;
            }
            AGM_LOGD("card id is %d\n", card);
            priv->card = card;
            continue;
        }
        if (strcmp(id, "device") == 0) {
            if (snd_config_get_integer(n, &device) < 0) {
                AGM_LOGE("Invalid type for %s", id);
                ret = -EINVAL;
                goto err_free_session;
            }
            AGM_LOGD("device id is %d\n", device);
            priv->device = device;
            continue;
        }
    }

    card_node = snd_card_def_get_card(card);
    if (!card_node) {
        AGM_LOGE("card node is NULL\n");
        ret = -EINVAL;
        goto err_free_session;
    }
    priv->card_node = card_node;

    pcm_node = snd_card_def_get_node(card_node, device, SND_NODE_TYPE_PCM);
    if (!pcm_node) {
        AGM_LOGE("pcm node is NULL\n");
        ret = -EINVAL;
        goto err_free_card;
    }
    priv->pcm_node = pcm_node;

    snd_card_def_get_int(pcm_node, "session_mode", &sess_mode);

    session_id = priv->device;
    ret = agm_session_open(session_id, sess_mode, &handle);
    if (ret) {
        AGM_LOGE("handle is NULL\n");
        ret = -EINVAL;
        goto err_free_card;
    }
    priv->session_id = session_id;
    priv->media_config = media_config;
    priv->buffer_config = buffer_config;
    priv->session_config = session_config;
    priv->handle = handle;
    priv->event_fd = -1;
    priv->state = AGM_IO_STATE_OPEN;
    priv->io.version = SND_PCM_IOPLUG_VERSION;
    priv->io.name = "AGM PCM I/O Plugin";
    priv->io.mmap_rw = 0;
    priv->io.callback = &agm_io_callback;
    priv->io.private_data = priv;
    priv->SSR_RUNNING = 0;

    ret = agm_session_register_cb(session_id, agm_pcm_event_cb,
              AGM_EVENT_DATA_PATH, (void *)priv);
    if (ret) {
        AGM_LOGE("register event callback failure\n");
        ret = -EINVAL;
        goto err_close_session;
    }

    ret = snd_pcm_ioplug_create(&priv->io, name, stream, mode);
    if (ret < 0) {
        AGM_LOGE("IO plugin create failed\n");
        goto err_free_cb;
    }

    if ((priv->event_fd = eventfd(0, EFD_NONBLOCK)) == -1) {
        AGM_LOGE("failed to create event_fd\n");
        ret = -EINVAL;
        goto err_free_cb;
    }

    ret = agm_hw_constraint(priv);
    if (ret < 0) {
        snd_pcm_ioplug_delete(&priv->io);
        goto err_close_eventfd;
    }

    *pcmp = priv->io.pcm;

    pthread_mutex_init(&priv->eos_lock, (const pthread_mutexattr_t *) NULL);
    /*Create a thread to exit the playback or recording when the SSR is triggered*/
    ret = pthread_create(&priv->monitor_thread,NULL,card_status_monitor,(void *)priv);
    if (ret){
        AGM_LOGE("pthread_create card_status_monitor failed\n");
        ret = -EINVAL;
        goto err_close_eventfd;
    }

    return 0;

err_close_eventfd:
    close(priv->event_fd);
err_free_cb:
    agm_session_register_cb(session_id, NULL, AGM_EVENT_DATA_PATH, (void *)priv);
err_close_session:
    agm_session_close(handle);
err_free_card:
    snd_card_def_put_card(card_node);
err_free_session:
    free(session_config);
err_free_buf:
    free(buffer_config);
err_free_media:
    free(media_config);
err_free_priv:
    free(priv);
    if (ret < 0)
        return ret;
    else
        return -ret;
}

SND_PCM_PLUGIN_SYMBOL(agm);
