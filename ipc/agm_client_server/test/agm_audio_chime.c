/*  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
    SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include "agm_client_proxy.h"
#include "kvh2xml.h"
#include "agm_api.h"

#define AGM_LOGE(fmt, ...) fprintf(stderr, "AGM_CHIME[E] " fmt "\n", ##__VA_ARGS__)
#define AGM_LOGI(fmt, ...) fprintf(stdout, "AGM_CHIME[I] " fmt "\n", ##__VA_ARGS__)
#define AGM_LOGD(fmt, ...) fprintf(stdout, "AGM_CHIME[D] " fmt "\n", ##__VA_ARGS__)

#ifndef AGM_SESSION_DEFAULT
#define AGM_SESSION_DEFAULT 0
#endif

typedef enum {
    MODE_PLAYBACK = 1,
    MODE_CAPTURE = 2
} app_mode_t;

typedef enum {
    AUDIO_FORMAT_WAV = 1,
    AUDIO_FORMAT_RAW = 2
} audio_format_t;

// WAV format constants
#define ID_RIFF 0x46464952
#define ID_WAVE 0x45564157
#define ID_FMT  0x20746d66
#define ID_DATA 0x61746164
#define FORMAT_PCM 1

struct wav_header {
    uint32_t riff_id;
    uint32_t riff_sz;
    uint32_t riff_fmt;
    uint32_t fmt_id;
    uint32_t fmt_sz;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint32_t data_id;
    uint32_t data_sz;
};

static bool g_running = true;
static uint8_t *g_audio_buf = NULL;
static uint32_t g_session_id = 100;
static uint32_t g_aif_id = 0;
static uint64_t g_handle = 0;
static uint32_t g_sample_rate = 48000;
static uint32_t g_bits_per_sample = 16;
static uint32_t g_channels = 2;
static uint32_t g_data_size = 0;
static uint32_t g_frame_period = 1024;
static uint32_t g_frame_count = 4;
static uint32_t g_record_time_sec = 10;  // Default recording time in seconds


// New KV parameters
static uint32_t g_device_kv = 0xA2000016;     // Default device KV (playback)
static uint32_t g_devicepp_kv = 0xAC000002;   // Default devicepp KV (playback)
static uint32_t g_stream_kv = 0xA1000001;     // Default stream KV (playback)
static uint32_t g_vmid = 0xDD000001;          // PVM usecase
static uint32_t g_instance_kv = 0;
static const char *g_interface = "TDM-LPAIF_RXTX-RX-PRIMARY";  // Default interface
static uint32_t g_offload_kv = 0;

uint8_t g_mode;

/* Event callback */
static void agm_event_callback(uint32_t session_id,
                              struct agm_event_cb_params *event_params,
                              void *client_data) {
    AGM_LOGD("EVENT CALLBACK: session_id=%u, source_module=%u, event_id=0x%08X, payload_size=%u",
             session_id,
             event_params->source_module_id,
             event_params->event_id,
             event_params->event_payload_size);

    if (event_params->event_id == 0x08001126) { // AGM_EVENT_EARLY_EOS (agm_api.h)
        AGM_LOGD("Received EOS event - playback completed");
        g_running = false;
        return;
    }

    if (event_params->event_payload_size > 0) {
        AGM_LOGI("EVENT PAYLOAD (first %u bytes):",
                event_params->event_payload_size < 32 ?
                event_params->event_payload_size : 32);
        for (uint32_t i = 0; i < event_params->event_payload_size && i < 32; i++) {
            if (i % 16 == 0) AGM_LOGE("\n");
            AGM_LOGI("%02X ", event_params->event_payload[i]);
        }
        AGM_LOGD("\n");
    }
}

static void signal_handler(int signum) {
    AGM_LOGI("Received signal %d, stopping...", signum);
    g_running = false;
}

static int parse_wav_header(FILE *fp, uint32_t *rate, uint32_t *bits, uint32_t *chs, uint32_t *size) {
    struct wav_header header;

    if (fread(&header, sizeof(struct wav_header), 1, fp) != 1) {
        AGM_LOGE("Failed to read WAV header");
        return -1;
    }

    if (header.riff_id != ID_RIFF || header.riff_fmt != ID_WAVE ||
        header.fmt_id != ID_FMT || header.audio_format != FORMAT_PCM) {
        AGM_LOGE("Invalid WAV format");
        return -1;
    }

    *rate = header.sample_rate;
    *bits = header.bits_per_sample;
    *chs = header.num_channels;
    *size = header.data_sz;

    return 0;
}

static int write_wav_header(FILE *file, uint32_t sample_rate, uint32_t bits_per_sample,
                           uint32_t channels, uint32_t data_size) {
    struct wav_header header;

    header.riff_id = ID_RIFF;
    header.riff_sz = data_size + sizeof(struct wav_header) - 8;
    header.riff_fmt = ID_WAVE;
    header.fmt_id = ID_FMT;
    header.fmt_sz = 16;
    header.audio_format = FORMAT_PCM;
    header.num_channels = channels;
    header.sample_rate = sample_rate;
    header.bits_per_sample = bits_per_sample;
    header.byte_rate = (bits_per_sample / 8) * channels * sample_rate;
    header.block_align = channels * (bits_per_sample / 8);
    header.data_id = ID_DATA;
    header.data_sz = data_size;

    return fwrite(&header, sizeof(struct wav_header), 1, file) == 1 ? 0 : -1;
}

static int create_stream_metadata(uint8_t **stream_data, uint32_t *stream_size, uint8_t dir)
{
    uint32_t sampling_rate_val = (g_sample_rate == 8000  ? SAMPLINGRATE_8K :
                                  g_sample_rate == 16000 ? SAMPLINGRATE_16K :
                                  g_sample_rate == 32000 ? SAMPLINGRATE_32K :
                                  g_sample_rate == 44100 ? SAMPLINGRATE_44K :
                                                            SAMPLINGRATE_48K);

    uint32_t bitwidth_val = (g_bits_per_sample == 24 ? BITWIDTH_24 : BITWIDTH_16);


    uint32_t stream_meta[64];
    uint32_t idx = 0;

    uint32_t stream_key = (dir == TX) ? 0xB1000000 : 0xA1000000;

    uint32_t num_gkvs = 2; // stream + vmid

    if (g_instance_kv != 0)
        num_gkvs++;

    if ((dir == RX) && (g_offload_kv != 0))
        num_gkvs++;

    stream_meta[idx++] = num_gkvs;
    stream_meta[idx++] = stream_key; stream_meta[idx++] = g_stream_kv;
    stream_meta[idx++] = 0xDD000000; stream_meta[idx++] = g_vmid;
    if (g_instance_kv != 0) { stream_meta[idx++] = 0xAB000000; stream_meta[idx++] = g_instance_kv; }

    /* MDF offload KV (playback only) */
    if ((dir == RX) && (g_offload_kv != 0)) {
        stream_meta[idx++] = OFFLOAD_PROCESSOR;
        stream_meta[idx++] = g_offload_kv;
    }

    stream_meta[idx++] = 3;
    stream_meta[idx++] = SAMPLINGRATE; stream_meta[idx++] = sampling_rate_val;
    stream_meta[idx++] = BITWIDTH;      stream_meta[idx++] = bitwidth_val;
    stream_meta[idx++] = GAIN;          stream_meta[idx++] = 0;

    stream_meta[idx++] = 0;
    stream_meta[idx++] = 0;
    stream_meta[idx++] = 0;

    *stream_size = idx * sizeof(uint32_t);
    *stream_data = (uint8_t *)malloc(*stream_size);
    if (!*stream_data) return -ENOMEM;

    memcpy(*stream_data, stream_meta, *stream_size);
    return 0;
}

static int create_device_metadata(uint8_t **device_data, uint32_t *device_size, uint8_t dir)
{
    uint32_t sampling_rate_val = (g_sample_rate == 8000  ? SAMPLINGRATE_8K :
                                  g_sample_rate == 16000 ? SAMPLINGRATE_16K :
                                  g_sample_rate == 32000 ? SAMPLINGRATE_32K :
                                  g_sample_rate == 44100 ? SAMPLINGRATE_44K :
                                                            SAMPLINGRATE_48K);
    uint32_t bitwidth_val = (g_bits_per_sample == 24 ? BITWIDTH_24 : BITWIDTH_16);

    uint32_t dev_meta[] = {
        1,
        (dir == RX) ? 0xA2000000 : 0xA3000000, g_device_kv,
        2,
        SAMPLINGRATE, sampling_rate_val,
        BITWIDTH,      bitwidth_val,
        0, 0, 0
    };

    *device_size = sizeof(dev_meta);
    *device_data = (uint8_t *)malloc(*device_size);
    if (!*device_data) return -ENOMEM;

    memcpy(*device_data, dev_meta, *device_size);
    return 0;
}

static int create_devicepp_metadata(uint8_t **devicepp_data, uint32_t *devicepp_size, uint8_t dir)
{
    uint32_t dpp_meta[] = {
        1,
        (dir == RX) ? 0xAC000000 : 0xAD000000, g_devicepp_kv,
        0,
        0, 0, 0
    };

    *devicepp_size = sizeof(dpp_meta);
    *devicepp_data = (uint8_t *)malloc(*devicepp_size);
    if (!*devicepp_data) return -ENOMEM;

    memcpy(*devicepp_data, dpp_meta, *devicepp_size);
    return 0;
}

static int playback_audio(FILE *file, audio_format_t format, uint8_t dir)
{
    AGM_LOGD("Starting playback with %u Hz, %u bits, %u channels",
             g_sample_rate, g_bits_per_sample, g_channels);

    int ret = 0;
    size_t buffer_size;

    const uint32_t bytes_per_sample = g_bits_per_sample / 8;
    const uint32_t block_align = bytes_per_sample * g_channels;

    if (g_frame_period > SIZE_MAX / block_align / g_frame_count) {
        AGM_LOGE("Buffer size calculation would overflow");
        return -EINVAL;
    }
    buffer_size = g_frame_period * block_align * g_frame_count;
    if (buffer_size > 65536) buffer_size = 65536;

    // Ensure minimum size
    if (buffer_size < block_align) {
        AGM_LOGE("Buffer size too small: %zu bytes", buffer_size);
        return -EINVAL;
    }
    g_audio_buf = malloc(buffer_size);
    if (!g_audio_buf) {
        AGM_LOGE("Failed to allocate audio buffer of size %zu", buffer_size);
        return -ENOMEM;
    }

    ret = agm_session_prepare(g_handle);
    if (ret) {
        AGM_LOGE("Failed to prepare session: %d", ret);
        goto fail_free_buffer;
    }

    ret = agm_session_start(g_handle);
    if (ret) {
        AGM_LOGE("Failed to start session: %d", ret);
        goto fail_free_buffer;
    }
    AGM_LOGI("Session started successfully");

    // Read until EOF
    size_t num_read = 0;
    for (;;) {
        num_read = fread(g_audio_buf, 1, buffer_size, file);
        if (num_read == 0) {
            if (feof(file)) {
                AGM_LOGI("Reached EOF");
            } else if (ferror(file)) {
                AGM_LOGE("File read error: %s", strerror(errno));
            }
            break;
        }

        size_t bytes_to_write = num_read;

        ret = agm_session_write(g_handle, g_audio_buf, &bytes_to_write);

        if (bytes_to_write != num_read) {
            AGM_LOGD("Warning: Only wrote %zu of %zu bytes", bytes_to_write, num_read);
        }
    }
    size_t cnt = agm_get_hw_processed_buff_cnt(g_handle, dir);
    AGM_LOGI("HW processed buffer count: %zu", cnt);

    AGM_LOGD("Stopping session...");
    ret = agm_session_stop(g_handle);
    AGM_LOGI("agm_stop returned %d", ret);

fail_free_buffer:
    return ret < 0 ? ret : 0;
}

static int capture_audio(FILE *file, audio_format_t format) {
    AGM_LOGI("Starting capture with %u Hz, %u bits, %u channels for %u seconds",
             g_sample_rate, g_bits_per_sample, g_channels, g_record_time_sec);

    int ret = 0;
    char *buffer = NULL;
    size_t buffer_size;
    unsigned int bytes_read = 0;
    unsigned int frames = 0;
    struct timespec end, now;
    struct wav_header header;

    uint32_t bytes_per_sample = g_bits_per_sample / 8;
    uint32_t block_align = g_channels * bytes_per_sample;
    buffer_size = g_frame_period * g_frame_count;

    if (format == AUDIO_FORMAT_WAV) {
        header.riff_id = ID_RIFF;
        header.riff_sz = 0;
        header.riff_fmt = ID_WAVE;
        header.fmt_id = ID_FMT;
        header.fmt_sz = 16;
        header.audio_format = FORMAT_PCM;
        header.num_channels = g_channels;
        header.sample_rate = g_sample_rate;
        header.bits_per_sample = g_bits_per_sample;
        header.byte_rate = (g_bits_per_sample / 8) * g_channels * g_sample_rate;
        header.block_align = block_align;
        header.data_id = ID_DATA;
        header.data_sz = 0;

        fseek(file, sizeof(struct wav_header), SEEK_SET);
    }

    buffer = malloc(buffer_size);
    if (!buffer) {
        AGM_LOGE("Unable to allocate %zu bytes", buffer_size);
        return -ENOMEM;
    }

    ret = agm_session_prepare(g_handle);
    if (ret) {
        AGM_LOGE("Failed to prepare session: %d", ret);
        goto fail_free_buffer;
    }

    ret = agm_session_start(g_handle);
    if (ret) {
        AGM_LOGE("Failed to start session: %d", ret);
        goto fail_free_buffer;
    }

    AGM_LOGI("Capturing sample: %u ch, %u hz, %u bit for %u seconds",
             g_channels, g_sample_rate, g_bits_per_sample, g_record_time_sec);

    // Set capture timeout
    clock_gettime(CLOCK_MONOTONIC, &now);
    end.tv_sec = now.tv_sec + g_record_time_sec;
    end.tv_nsec = now.tv_nsec;

    while (g_running) {
        size_t bytes_to_read = buffer_size;

        ret = agm_session_read(g_handle, buffer, &bytes_to_read);
        if (ret) {
            AGM_LOGE("Error reading from session: %d", ret);
            break;
        }

        if (fwrite(buffer, 1, bytes_to_read, file) != bytes_to_read) {
            AGM_LOGE("Error writing to file");
            ret = -EIO;
            break;
        }

        bytes_read += bytes_to_read;

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > end.tv_sec ||
            (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec)) {
            break;
        }
    }

    frames = bytes_read / block_align;
    AGM_LOGI("Captured %u frames (%u bytes)", frames, bytes_read);

    ret = agm_session_stop(g_handle);
    if (ret) {
        AGM_LOGE("Failed to stop session: %d", ret);
    }

    if (format == AUDIO_FORMAT_WAV) {
        header.data_sz = bytes_read;
        header.riff_sz = header.data_sz + sizeof(header) - 8;
        fseek(file, 0, SEEK_SET);
        if (fwrite(&header, sizeof(struct wav_header), 1, file) != 1) {
            AGM_LOGE("Failed to write WAV header");
            ret = -EIO;
        }
    }

    AGM_LOGI("Capture completed successfully");
    ret = 0;

fail_free_buffer:
    free(buffer);
    return ret;
}


int device_get_aif_info(const char *interface_name,
                                   uint32_t *aif_id_out,
                                   uint8_t  *dir_out)
{
    if (!interface_name || !aif_id_out || !dir_out)
        return -EINVAL;

    int ret;
    size_t count = 0;
    struct aif_info *aif_list = NULL;

    ret = agm_get_aif_info_list(NULL, &count);
    if (ret) {
        AGM_LOGE("AGM_CHIME agm_get_aif_info_list(count) failed: %d", ret);
        return ret;
    }
    if (count == 0) {
        AGM_LOGE("AGM_CHIME No AIFs returned by AGM");
        return -ENOENT;
    }

    aif_list = (struct aif_info *)calloc(count, sizeof(*aif_list));
    if (!aif_list) return -ENOMEM;

    size_t cap = count;
    ret = agm_get_aif_info_list(aif_list, &cap);
    if (ret) {
        AGM_LOGE("AGM_CHIME agm_get_aif_info_list(list) failed: %d", ret);
        free(aif_list);
        return ret;
    }
    if (cap == 0) {
        AGM_LOGE("AGM_CHIME Empty AIF list");
        free(aif_list);
        return -ENOENT;
    }

    /* Resolve by name */
    int found = 0;
    for (size_t i = 0; i < cap; ++i) {
        if (strcmp(aif_list[i].aif_name, interface_name) == 0) {
            *aif_id_out = (uint32_t)i;
            *dir_out    = (uint8_t)aif_list[i].dir;
            found = 1;
            break;
        }
    }

    free(aif_list);
    return found ? 0 : -ENOENT;
}

static void usage(const char *program_name) {
    printf("\nUsage: %s [options] <file>\n", program_name);
    printf("  -m mode          : 1=playback, 2=capture (default: 1)\n");
    printf("  -f format        : WAV or RAW (default: WAV)\n");
    printf("  -D aif_id        : Audio interface ID (default: 0)\n");
    printf("  -d device_id     : Device ID (default: 100)\n");
    printf("  -i interface     : Interface name (default: TDM-LPAIF_RXTX-RX-PRIMARY)\n");
    printf("  -dkv device_kv   : Device KV (hex, default: 0xA2000016 for playback, 0xA3000016 for capture)\n");
    printf("  -dppkv dpp_kv    : Device PP KV (hex, default: 0xAC000002 for playback, 0xAD000002 for capture)\n");
    printf("  -skv stream_kv   : Stream KV (hex, default: 0xA1000001 for playback, 0xB1000001 for capture)\n");
    printf("  -ikv instance_kv : Instance KV (hex, default: 0 - no instance kv in graph)\n");
    printf("  -vmid vmid       : Voice/Media ID (default: 0xDD000001)\n");
    printf("  -offload offload_kv  : Offload KV value (key-0xE7060000, value:0xE7060003 )\n");
    printf("  -c channels      : Number of channels (default: 2)\n");
    printf("  -r rate          : Sample rate for capture (default: 48000)\n");
    printf("  -b bits          : Bits per sample for capture (default: 16)\n");
    printf("  -T time          : Recording time in seconds for capture (default: 10)\n");
    printf("  -fp period       : Frame period (default: 8)\n");
    printf("  -fc count        : Frame count (default: 4096)\n");
    printf("  -h               : Help\n\n");
    printf("Playbook example: %s /firmware/yesterday_48KHz.wav -D 100 -d 100 -i TDM-LPAIF_RXTX-RX-PRIMARY -dkv 0xA2000016 -dppkv 0xAC000002 -skv 0xA1000001 -ikv A2B_instance_Val -vmid 0xDD000001 -offload 0 -c 2\n", program_name);
    printf("Capture example:  %s /data/agmcap_rec.wav -m 2 -D 100 -d 101 -i TDM-LPAIF_RXTX-TX-PRIMARY -dkv 0xA3000016 -dppkv 0xAD000002 -skv 0xB1000001 -ikv 0 -vmid 0xDD000001 -T 20 -r 48000 -c 2 -b 16\n", program_name);
}

int main(int argc, char **argv) {
    app_mode_t mode = MODE_PLAYBACK;
    audio_format_t format = AUDIO_FORMAT_WAV;
    const char *filename = NULL;
    FILE *file = NULL;
    size_t payload_size = 0;
    int ret = -1;
    uint32_t device_id = 100;

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            mode = strtoul(argv[i + 1], NULL, 0);
            if (mode == MODE_CAPTURE) {
                g_device_kv = 0xA3000016;     // Default capture device KV
                g_devicepp_kv = 0xAD000002;   // Default capture devicepp KV
                g_stream_kv = 0xB1000001;     // Default capture stream KV
                g_interface = "TDM-LPAIF_RXTX-TX-PRIMARY";  // Default capture interface
                AGM_LOGE("MODE_CAPTURE");
            }
            i += 2;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            format = (strcasecmp(argv[i + 1], "RAW") == 0) ? AUDIO_FORMAT_RAW : AUDIO_FORMAT_WAV;
            i += 2;
        } else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
            g_aif_id = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            g_session_id = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            g_interface = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            g_channels = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            g_sample_rate = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            g_bits_per_sample = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            g_record_time_sec = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-dkv") == 0 && i + 1 < argc) {
            g_device_kv = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-dppkv") == 0 && i + 1 < argc) {
            g_devicepp_kv = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-skv") == 0 && i + 1 < argc) {
            g_stream_kv = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-ikv") == 0 && i + 1 < argc) {
            g_instance_kv = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        }else if (strcmp(argv[i], "-vmid") == 0 && i + 1 < argc) {
            g_vmid = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-offload") == 0 && i + 1 < argc) {
            g_offload_kv = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-fp") == 0 && i + 1 < argc) {
            g_frame_period = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-fc") == 0 && i + 1 < argc) {
            g_frame_count = strtoul(argv[i + 1], NULL, 0);
            i += 2;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            filename = argv[i];
            i++;
        } else {
            AGM_LOGE("Unknown option: %s", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }

    if (!filename) {
        AGM_LOGE("Missing filename");
        usage(argv[0]);
        return -1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    file = fopen(filename, (mode == MODE_PLAYBACK) ? "rb" : "wb");
    if (!file) {
        AGM_LOGE("Failed to open file: %s", strerror(errno));
        return -1;
    }

    if (mode == MODE_PLAYBACK && format == AUDIO_FORMAT_WAV) {
        if (parse_wav_header(file, &g_sample_rate, &g_bits_per_sample, &g_channels, &g_data_size)) {
            AGM_LOGE("Invalid WAV header");
            fclose(file);
            return -1;
        }
        AGM_LOGI("WAV header parsed: %u Hz, %u bits, %u channels, %u bytes",
                 g_sample_rate, g_bits_per_sample, g_channels, g_data_size);
    }
    else if (mode == MODE_PLAYBACK) {
        fseek(file, 0, SEEK_END);
        g_data_size = ftell(file);
        fseek(file, 0, SEEK_SET);
        AGM_LOGI("RAW file size: %u bytes", g_data_size);
    }
    else {
        AGM_LOGD("capture mode - using parameters: %u Hz, %u bits, %u channels, %u seconds",
                 g_sample_rate, g_bits_per_sample, g_channels, g_record_time_sec);
    }

    AGM_LOGI("Using KV values - device: 0x%08X, devicepp: 0x%08X, stream: 0x%08X, instance: 0x%08X, vmid: 0x%08X",
             g_device_kv, g_devicepp_kv, g_stream_kv, g_instance_kv, g_vmid);

    // offload kv check
    if (mode != MODE_PLAYBACK && g_offload_kv != 0) {
        AGM_LOGE("Offload KV is only supported for playback. Ignoring -offload.");
        g_offload_kv = 0;
    }

    AGM_LOGI("Offload KV: %s",
        g_offload_kv ? "ENABLED" : "DISABLED");

    if (g_offload_kv)
    AGM_LOGI("Offload KV value: 0x%08X", g_offload_kv);

    //get aif_info
    uint32_t aif_id = 0;
    uint8_t dir = 0;
    ret = device_get_aif_info(g_interface, &aif_id, &dir);

    if (ret != 0) {
        AGM_LOGE("Failed to get AIF info for '%s', ret=%d", g_interface, ret);
        goto cleanup;
    }
    g_mode = dir;

    AGM_LOGI("Interface '%s' -> aif_id=%u, dir=%u", g_interface, aif_id, dir);

    //Stream metadata
    uint8_t *stream_data = NULL; uint32_t stream_size = 0;
    ret = create_stream_metadata(&stream_data, &stream_size, dir);
    if (ret) { AGM_LOGE("create_stream_metadata failed: %d", ret); goto cleanup; }

   ret = agm_session_set_metadata(g_session_id, stream_size, stream_data);
   if (ret) { AGM_LOGE("agm_session_set_metadata failed: %d", ret); goto cleanup; }

    free(stream_data);

    //device metadata
    uint8_t *device_data = NULL; uint32_t device_size = 0;
    ret = create_device_metadata(&device_data, &device_size, dir);
    if (ret) { AGM_LOGE("create_device_metadata failed: %d", ret); goto cleanup; }

    AGM_LOGD("Calling agm_aif_set_metadata: aif_id=%u, size=%u, dir=%d", aif_id, device_size, dir);
    ret = agm_aif_set_metadata(aif_id, device_size, device_data);
    free(device_data);

    //device-pp metadata
    uint8_t *devicepp_data = NULL; uint32_t devicepp_size = 0;
    ret = create_devicepp_metadata(&devicepp_data, &devicepp_size, dir);
    if (ret) { AGM_LOGE("create_devicepp_metadata failed: %d", ret);  goto cleanup; }

    AGM_LOGD("Calling agm_session_aif_set_metadata (device-PP): sid=%u aif_id=%u size=%u",
             g_session_id, aif_id, devicepp_size);
    ret = agm_session_aif_set_metadata(g_session_id, aif_id, devicepp_size, devicepp_data);
    free(devicepp_data);
    if (ret) { AGM_LOGE("agm_session_aif_set_metadata failed: %d", ret); goto cleanup; }

    struct agm_media_config media_cfg = {0};
    media_cfg.rate        = g_sample_rate;
    media_cfg.channels    = g_channels;
    media_cfg.format      = 2;             //AGM_FORMAT_PCM_S16_LE
    media_cfg.data_format = 1;

    ret = agm_aif_set_media_config(aif_id, &media_cfg);
    if (ret) { AGM_LOGE("agm_set_aif_media_config failed: %d", ret); goto cleanup; }

    // --- Connect session <-> AIF ---
    AGM_LOGD("Calling agm_session_aif_connect: sid=%u aif_id=%u connect=true", g_session_id, aif_id);
    ret = agm_session_aif_connect(g_session_id, aif_id, true);
    if (ret) { AGM_LOGE("agm_session_aif_connect failed: %d", ret); goto cleanup; }


    //session_open
    AGM_LOGD("Opening session: id=%u session_mode=%u", g_session_id, AGM_SESSION_DEFAULT);
    ret = agm_session_open(g_session_id, AGM_SESSION_DEFAULT, &g_handle);
    if (ret || g_handle == 0) {
    AGM_LOGE("Failed to open session (ret=%d, handle=0x%x)", ret, g_handle);
    goto cleanup;
    }
    AGM_LOGI("Session opened: handle=0x%llx", (unsigned long long)g_handle);

    //set config
    struct agm_session_config sess_cfg = {0};
        sess_cfg.dir = dir;
        sess_cfg.sess_mode = AGM_SESSION_DEFAULT;
        sess_cfg.start_threshold = 0;
        sess_cfg.stop_threshold = 0;
        sess_cfg.sess_flags = 0;

    const uint32_t bytes_per_period = g_frame_period * g_channels * (g_bits_per_sample/8);

    AGM_LOGI("agm_session_set_config call: g_frame_count=%u, g_frame_period=%u, bytes_per_period=%u", g_frame_count, g_frame_period, bytes_per_period);

    struct agm_buffer_config buff_cfg = {0};
    buff_cfg.count = g_frame_count;
    buff_cfg.size  = bytes_per_period;

    AGM_LOGD("Calling agm_session_set_config: sid=%u count=%u size=%u",
             g_session_id, buff_cfg.count, buff_cfg.size);
    ret = agm_session_set_config(g_handle, &sess_cfg, &media_cfg, &buff_cfg);
    if (ret) { AGM_LOGE("agm_session_set_config failed: %d", ret); goto cleanup; }



    if (agm_session_register_cb(g_session_id, agm_event_callback, AGM_EVENT_DATA_PATH, NULL) != 0) {
        AGM_LOGE("Failed to register event callback.");
        goto cleanup;
    } else
        AGM_LOGI("success to register event callback");

    if (mode == MODE_PLAYBACK) {
        ret = playback_audio(file, format, dir);
    } else {
        ret = capture_audio(file, format);
    }

    // Cleanup
cleanup:
    AGM_LOGI("enter cleanup");
    if (g_handle) agm_session_close(g_handle);
    if (g_audio_buf) free(g_audio_buf);
    if (file) fclose(file);

    AGM_LOGI("Application %s", (ret == 0) ? "completed successfully" : "failed");
    return (ret == 0) ? 0 : -1;
}