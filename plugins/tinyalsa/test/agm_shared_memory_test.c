/*
** Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
** SPDX-License-Identifier: BSD-3-Clause
*/

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

#include "agmmixer.h"

/*
 * This test targets the "AUDIO_LOAD" test CAPI module (capi_v2_load, see
 * module/load/capi_v2/src/capi_v2_load.c and
 * module/load/lib/inc/load_calibration_api.h). The param id and payload
 * layout below are hard-coded to match that module exactly.
 *
 * CAPI_V2_PARAM_ID_LOAD_DUMMY is a dummy parameter implemented purely for
 * testing getters/setters: it caches a variable-length uint32_t array
 * (capi_v2_load_dummy_t.array[]) inside the module and returns it unchanged
 * on a subsequent get.
 */
#define CAPI_V2_PARAM_ID_LOAD_DUMMY     0x080012A6
#define MAX_TARGET_GKVS                  16
#define DEFAULT_PCM_PERIOD_SIZE          1024
#define DEFAULT_PCM_PERIOD_COUNT         4
#define MAX_INTERACTIVE_TARGETS          8
#define MAX_INTERACTIVE_OWNER_GRAPHS     8
#define INTERACTIVE_TARGET_NAME_LEN      32
#define INTERACTIVE_INTF_NAME_LEN        80
#define INTERACTIVE_LINE_LEN             512

/* Mirrors module/load/lib/inc/load_calibration_api.h capi_v2_load_dummy_t */
typedef struct {
    uint32_t num;
    uint32_t array[];
} capi_v2_load_dummy_t;

#define LOAD_DUMMY_TYPESIZE(num) \
    (sizeof(capi_v2_load_dummy_t) + (num) * sizeof(uint32_t))
#define SHARED_MEM_ALIGN_8BYTE(x) (((x) + 7) & (~7))

/*
 * Mirrors the internal apm_module_param_data_t header used by AGM/GSL custom
 * config APIs. For tagged APIs the module_instance_id field is ignored by GSL,
 * so this tool leaves it as 0 and targets modules by tag id.
 */
struct apm_module_param_data_t {
    uint32_t module_instance_id;
    uint32_t param_id;
    uint32_t param_size;
    uint32_t error_code;
};

struct target_gkv {
    uint32_t num_gkvs;
    struct agm_key_value kv[MAX_TARGET_GKVS];
};

struct owner_graph_config {
    bool enabled;
    bool opened;
    bool connected;
    bool started;
    unsigned int device;
    unsigned int channels;
    unsigned int rate;
    unsigned int bits;
    uint32_t stream_kv;
    uint32_t device_kv;
    uint32_t devicepp_kv;
    uint32_t instance_kv;
    uint32_t vmid_kv;
    uint32_t offload_proc_kv;
    char *intf_name;
    struct pcm *pcm;
    struct pcm_config pcm_config;
};

struct interactive_target {
    bool valid;
    char name[INTERACTIVE_TARGET_NAME_LEN];
    struct target_gkv target;
};

struct interactive_owner_graph {
    bool valid;
    char name[INTERACTIVE_TARGET_NAME_LEN];
    char intf_name[INTERACTIVE_INTF_NAME_LEN];
    struct owner_graph_config graph;
    struct target_gkv target;
};

static int gkv_key_from_name(const char *name, uint32_t *key)
{
    if (!name || !key)
        return -EINVAL;

    if (!strcasecmp(name, "skv") || !strcasecmp(name, "streamrx"))
        *key = STREAMRX;
    else if (!strcasecmp(name, "streamtx"))
        *key = STREAMTX;
    else if (!strcasecmp(name, "dkv") || !strcasecmp(name, "devicerx"))
        *key = DEVICERX;
    else if (!strcasecmp(name, "devicetx"))
        *key = DEVICETX;
    else if (!strcasecmp(name, "dppkv") || !strcasecmp(name, "devicepp_rx"))
        *key = DEVICEPP_RX;
    else if (!strcasecmp(name, "devicepp_tx"))
        *key = DEVICEPP_TX;
    else if (!strcasecmp(name, "ikv") || !strcasecmp(name, "instance"))
        *key = INSTANCE;
    else if (!strcasecmp(name, "vmid") || !strcasecmp(name, "vmid_kv"))
        *key = VMID;
    else if (!strcasecmp(name, "opkv") || !strcasecmp(name, "offload_processor"))
        *key = OFFLOAD_PROCESSOR;
    else
        *key = (uint32_t)strtoul(name, NULL, 0);

    return (*key == 0) ? -EINVAL : 0;
}

static int parse_target_gkv(const char *spec, struct target_gkv *target)
{
    struct target_gkv parsed;
    char *work = NULL, *saveptr = NULL, *token = NULL;
    uint32_t count = 0;
    int ret = 0;

    if (!spec || !target)
        return -EINVAL;

    work = strdup(spec);
    if (!work)
        return -ENOMEM;

    memset(&parsed, 0, sizeof(parsed));
    token = strtok_r(work, ",", &saveptr);
    while (token) {
        char *sep;

        if (count >= MAX_TARGET_GKVS) {
            ret = -EINVAL;
            goto done;
        }

        sep = strchr(token, '=');
        if (!sep) {
            ret = -EINVAL;
            goto done;
        }
        *sep = '\0';
        sep++;

        ret = gkv_key_from_name(token, &parsed.kv[count].key);
        if (ret)
            goto done;
        parsed.kv[count].value = (uint32_t)strtoul(sep, NULL, 0);
        count++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    if (count == 0)
        ret = -EINVAL;
    else {
        parsed.num_gkvs = count;
        *target = parsed;
    }

done:
    free(work);
    return ret;
}

static void print_target_gkv(const struct target_gkv *target)
{
    uint32_t i;

    if (!target || target->num_gkvs == 0) {
        printf("target_gkv: none\n");
        return;
    }

    for (i = 0; i < target->num_gkvs; i++)
        printf("target_gkv[%u]: key 0x%x, value 0x%x\n", i,
               target->kv[i].key, target->kv[i].value);
}

static int target_gkv_add(struct target_gkv *target, uint32_t key,
                          uint32_t value)
{
    if (!target || key == 0 || value == 0)
        return -EINVAL;

    if (target->num_gkvs >= MAX_TARGET_GKVS)
        return -ENOMEM;

    target->kv[target->num_gkvs].key = key;
    target->kv[target->num_gkvs].value = value;
    target->num_gkvs++;
    return 0;
}

static int owner_graph_build_target_gkv(const struct owner_graph_config *graph,
                                        struct target_gkv *target)
{
    int ret;

    if (!graph || !target || graph->stream_kv == 0 || graph->device_kv == 0)
        return -EINVAL;

    memset(target, 0, sizeof(*target));

    ret = target_gkv_add(target, STREAMRX, graph->stream_kv);
    if (ret)
        return ret;

    if (graph->instance_kv != 0) {
        ret = target_gkv_add(target, INSTANCE, graph->instance_kv);
        if (ret)
            return ret;
    }

    if (graph->vmid_kv != 0) {
        ret = target_gkv_add(target, VMID, graph->vmid_kv);
        if (ret)
            return ret;
    }

    if (graph->offload_proc_kv != 0) {
        ret = target_gkv_add(target, OFFLOAD_PROCESSOR, graph->offload_proc_kv);
        if (ret)
            return ret;
    }

    ret = target_gkv_add(target, DEVICERX, graph->device_kv);
    if (ret)
        return ret;

    if (graph->devicepp_kv != 0) {
        ret = target_gkv_add(target, DEVICEPP_RX, graph->devicepp_kv);
        if (ret)
            return ret;
    }

    return 0;
}

static enum pcm_format pcm_format_from_bits(unsigned int bits)
{
    switch (bits) {
    case 32:
        return PCM_FORMAT_S32_LE;
    case 24:
        return PCM_FORMAT_S24_3LE;
    case 16:
    default:
        return PCM_FORMAT_S16_LE;
    }
}

static int owner_graph_validate(const struct owner_graph_config *graph)
{
    if (!graph || !graph->enabled)
        return 0;

    if (graph->device == UINT_MAX) {
        printf("Error : --open-graph requires -d <pcm_device>\n");
        return -EINVAL;
    }

    if (!graph->intf_name) {
        printf("Error : --open-graph requires -i <interface>\n");
        return -EINVAL;
    }

    if (graph->stream_kv == 0 || graph->device_kv == 0) {
        printf("Error : --open-graph requires -skv and -dkv\n");
        return -EINVAL;
    }

    if (graph->channels == 0 || graph->rate == 0 || graph->bits == 0) {
        printf("Error : --open-graph requires non-zero channels/rate/bits\n");
        return -EINVAL;
    }

    return 0;
}

static int owner_graph_open(struct mixer *mixer, unsigned int card,
                            struct owner_graph_config *graph)
{
    struct device_config dev_config;
    int ret;

    if (!mixer || !graph || !graph->enabled)
        return -EINVAL;

    ret = owner_graph_validate(graph);
    if (ret)
        return ret;

    memset(&dev_config, 0, sizeof(dev_config));
    ret = get_device_media_config(BACKEND_CONF_FILE, graph->intf_name,
                                  &dev_config);
    if (ret) {
        printf("Invalid input, entry not found for : %s\n", graph->intf_name);
        return ret;
    }

    if (dev_config.format != PCM_FORMAT_INVALID)
        dev_config.bits = get_tinyalsa_pcm_bit_width(dev_config.format);

    printf("Backend %s rate ch bit fmt : %u, %u, %u %d\n",
           graph->intf_name, dev_config.rate, dev_config.ch, dev_config.bits,
           dev_config.format);

    ret = set_agm_device_media_config(mixer, graph->intf_name, &dev_config);
    if (ret) {
        printf("Failed to set device media config\n");
        return ret;
    }

    ret = set_agm_audio_intf_metadata(mixer, graph->intf_name,
                                      graph->device_kv, PLAYBACK,
                                      dev_config.rate, dev_config.bits,
                                      graph->stream_kv);
    if (ret) {
        printf("Failed to set device metadata\n");
        return ret;
    }

    ret = set_agm_stream_metadata(mixer, graph->device, graph->stream_kv,
                                  PLAYBACK, STREAM_PCM, graph->instance_kv,
                                  graph->vmid_kv, graph->offload_proc_kv,
                                  NULL, 0);
    if (ret) {
        printf("Failed to set pcm metadata\n");
        return ret;
    }

    if (graph->devicepp_kv != 0) {
        ret = set_agm_streamdevice_metadata(mixer, graph->device,
                                            graph->stream_kv, PLAYBACK,
                                            STREAM_PCM, graph->intf_name,
                                            graph->devicepp_kv);
        if (ret) {
            printf("Failed to set stream-device metadata\n");
            return ret;
        }
    }

    ret = connect_agm_audio_intf_to_stream(mixer, graph->device,
                                           graph->intf_name, STREAM_PCM,
                                           true);
    if (ret) {
        printf("Failed to connect pcm to audio interface\n");
        return ret;
    }
    graph->connected = true;

    memset(&graph->pcm_config, 0, sizeof(graph->pcm_config));
    graph->pcm_config.channels = graph->channels;
    graph->pcm_config.rate = graph->rate;
    graph->pcm_config.period_size = DEFAULT_PCM_PERIOD_SIZE;
    graph->pcm_config.period_count = DEFAULT_PCM_PERIOD_COUNT;
    graph->pcm_config.format = pcm_format_from_bits(graph->bits);
    graph->pcm_config.start_threshold = 0;
    graph->pcm_config.stop_threshold = INT32_MAX;
    graph->pcm_config.silence_threshold = 0;

    graph->pcm = pcm_open(card, graph->device, PCM_OUT, &graph->pcm_config);
    if (!graph->pcm || !pcm_is_ready(graph->pcm)) {
        printf("Unable to open PCM device %u (%s)\n", graph->device,
               graph->pcm ? pcm_get_error(graph->pcm) : "pcm_open failed");
        return -ENODEV;
    }

    graph->opened = true;
    printf("owner graph opened: card %u device %u intf %s, %u ch, %u hz, %u bit\n",
           card, graph->device, graph->intf_name, graph->channels,
           graph->rate, graph->bits);
    return 0;
}

static int owner_graph_start(struct owner_graph_config *graph)
{
    int ret;

    if (!graph || !graph->enabled || !graph->opened || !graph->pcm)
        return -EINVAL;

    ret = pcm_start(graph->pcm);
    if (ret < 0) {
        printf("owner graph start error: %s\n", pcm_get_error(graph->pcm));
        return ret;
    }

    graph->started = true;
    printf("owner graph started\n");
    return 0;
}
static int owner_graph_stop(struct owner_graph_config *graph)
{
    if (!graph || !graph->enabled || !graph->opened || !graph->pcm)
        return -EINVAL;

    if (graph->started) {
        pcm_stop(graph->pcm);
        graph->started = false;
        printf("owner graph stopped\n");
    }

    return 0;
}

static void owner_graph_close(struct mixer *mixer, struct owner_graph_config *graph)
{
    if (!graph || !graph->enabled)
        return;

    if (graph->started && graph->pcm) {
        pcm_stop(graph->pcm);
        graph->started = false;
    }

    if (graph->connected && mixer) {
        connect_agm_audio_intf_to_stream(mixer, graph->device,
                                         graph->intf_name, STREAM_PCM, false);
        graph->connected = false;
    }

    if (graph->pcm) {
        pcm_close(graph->pcm);
        graph->pcm = NULL;
    }
    graph->opened = false;
}

static void fill_index_pattern(uint32_t *values, uint32_t num)
{
    uint32_t i;

    for (i = 0; i < num; i++)
        values[i] = i;
}

static int load_dummy_payload_size(uint32_t num, size_t *payload_size)
{
    size_t dummy_size;
    size_t total_size;

    if (!payload_size)
        return -EINVAL;

    if (num > (SIZE_MAX - sizeof(capi_v2_load_dummy_t))/sizeof(uint32_t))
        return -EINVAL;

    dummy_size = LOAD_DUMMY_TYPESIZE((size_t)num);
    if (dummy_size > UINT32_MAX)
        return -EINVAL;

    if (dummy_size > SIZE_MAX - sizeof(struct apm_module_param_data_t) - 7)
        return -EINVAL;

    total_size = sizeof(struct apm_module_param_data_t) + dummy_size;
    *payload_size = SHARED_MEM_ALIGN_8BYTE(total_size);
    if (*payload_size > UINT32_MAX)
        return -EINVAL;

    return 0;
}

static int prepare_load_dummy_shmem_payload(void *shmem_addr,
                                            const struct agm_shmem_info *info,
                                            const uint32_t *values,
                                            uint32_t num,
                                            uint32_t *payload_size_out)
{
    struct apm_module_param_data_t *header;
    capi_v2_load_dummy_t *dummy;
    size_t dummy_size, payload_size;
    int ret;

    if (!shmem_addr || !info)
        return -EINVAL;

    ret = load_dummy_payload_size(num, &payload_size);
    if (ret) {
        printf("prepare_load_dummy_shmem_payload: invalid write count %u\n",
               num);
        return ret;
    }

    dummy_size = LOAD_DUMMY_TYPESIZE((size_t)num);
    if (payload_size > info->size) {
        printf("prepare_load_dummy_shmem_payload: payload_size %zu exceeds shared memory size %u\n",
               payload_size, info->size);
        return -EINVAL;
    }

    memset(shmem_addr, 0, payload_size);
    header = (struct apm_module_param_data_t *)shmem_addr;
    header->module_instance_id = 0;
    header->param_id = CAPI_V2_PARAM_ID_LOAD_DUMMY;
    header->error_code = 0;
    header->param_size = (uint32_t)dummy_size;

    dummy = (capi_v2_load_dummy_t *)((uint8_t *)shmem_addr + sizeof(*header));
    dummy->num = num;
    if (values && num)
        memcpy(dummy->array, values, num * sizeof(uint32_t));

    if (payload_size_out)
        *payload_size_out = (uint32_t)payload_size;
    return 0;
}

static void usage(void)
{
    printf(" Usage : agm_shared_memory_test [-D card] [-u size -w write_count -f free_flag]"
           " [-T tag_id {-gkv key=value,... | --open-graph graph_opts} -P2 -R2 direct_count -Q2 get_count]"
           " [-qi start_index] [-rs delay_ms] [--start-graph] [--interactive]\n\n");
    printf(" Options are processed in the order given on the command line."
           " Default mode controls an already-open graph selected by -gkv."
           " Owner mode (--open-graph) opens a playback PCM graph in this app,"
           " lets -P2 share memory while it is open, and starts it when"
           " --start-graph is processed.\n\n");
    printf(" -D  mixer/PCM card number, default 100. Use the same card as the running AGM usecase.\n");
    printf(" -u  shared memory size in bytes to allocate with the Tinyalsa"
           " global ShmemAlloc mixer control immediately.\n");
    printf(" -w  write a CAPI_V2_PARAM_ID_LOAD_DUMMY payload containing"
           " a uint32_t test pattern (indices 0..n-1) into the allocated"
           " shared memory immediately; requires -u first.\n");
    printf(" -T  tag id (hex) of the active module to target with -P2/-Q2.\n");
    printf(" -gkv/--target-gkv comma-separated target graph KVs for external mode."
           " Example: skv=0xA1000001,dkv=0xA2000016,dppkv=0xAC000002,vmid=0xDD000001.\n");
    printf(" --open-graph open/configure a playback graph in this app. Requires"
           " -d, -i, -skv, -dkv. Optional: -dppkv, -ikv, -vmid_kv, -offload,"
           " --channels, --rate, --bits. If -gkv is not supplied, target GKV"
           " is inferred from these graph options.\n");
    printf(" -d  PCM device id for --open-graph.\n");
    printf(" -i  audio interface name for --open-graph, for example PCM_RT_PROXY-RX-2.\n");
    printf(" -skv/-dkv/-dppkv/-ikv/-vmid_kv/-offload graph KVs for --open-graph.\n");
    printf(" --channels/-C, --rate, --bits PCM config for --open-graph; defaults 2/48000/16.\n");
    printf(" --start-graph start the owner-mode PCM graph after any prior -P2.\n");
    printf(" --hold-ms sleep after --start-graph in non-interactive owner mode, default 0.\n");
    printf(" --interactive/-I enter prompt after initial command-line setup.\n");
    printf(" -P2 issue mixer setParam(shared-mem) for CAPI_V2_PARAM_ID_LOAD_DUMMY"
           " using the current shared-memory payload; requires -T and prior -u/-w.\n");
    printf(" -R2 directly read CAPI_V2_PARAM_ID_LOAD_DUMMY from the app's"
           " mapped shared-memory virtual address after an optional -rs delay.\n");
    printf(" -Q2 issue mixer getParam(tag) for CAPI_V2_PARAM_ID_LOAD_DUMMY"
           " through the normal copy-based tagged GET path; requires -T.\n");
    printf(" -qi starting array index for -R2/-Q2 comparison window, default 0."
           " Example: -w 32 -qi 3 -R2 15 verifies array[3]..array[17].\n");
    printf(" -rs delay in milliseconds before -R2 direct shared-memory read, default 5. Must be given before -R2.\n");
    printf(" -f  free the current shared memory handle on exit (0/1), default 1.\n");
}

static int handle_set_param_shmem(struct mixer *mixer, uint32_t tag_id,
                                  const struct agm_shmem_info *info,
                                  const struct target_gkv *target,
                                  uint32_t payload_size)
{
    struct agm_tagged_shmem_config *shmem_cfg;
    size_t cfg_size;
    int ret;

    if (!mixer || !info || !target || (target->num_gkvs == 0) ||
        (tag_id == 0) || (payload_size == 0))
        return -EINVAL;
    if (payload_size > info->size) {
        printf("handle_set_param_shmem: payload_size %u exceeds shared memory size %u\n",
               payload_size, info->size);
        return -EINVAL;
    }

    cfg_size = AGM_TAGGED_SHMEM_CONFIG_SIZE(target->num_gkvs);
    shmem_cfg = calloc(1, cfg_size);
    if (!shmem_cfg)
        return -ENOMEM;

    shmem_cfg->tag_id = tag_id;
    shmem_cfg->spf_mem_handle = info->spf_mem_handle;
    shmem_cfg->payload_size = payload_size;
    shmem_cfg->payload_offset = 0;
    shmem_cfg->num_gkvs = target->num_gkvs;
    memcpy(shmem_cfg->gkv, target->kv,
           target->num_gkvs * sizeof(struct agm_key_value));

    printf(">>>>>>Mixer setParam(shared-mem) CAPI_V2_PARAM_ID_LOAD_DUMMY\n");
    printf("tag_id = 0x%x, param_id = 0x%x, spf_mem_handle = 0x%x, payload_size = %u, num_gkvs = %u\n",
           tag_id, CAPI_V2_PARAM_ID_LOAD_DUMMY, info->spf_mem_handle,
           payload_size, target->num_gkvs);

    ret = agm_mixer_set_param_shmem_by_tag(mixer, shmem_cfg);
    if (ret == 0)
        printf("Mixer setParam(shared-mem) Successful with rc = %d\n", ret);
    else
        printf("Mixer setParam(shared-mem) Unsuccessful with rc = %d\n", ret);

    free(shmem_cfg);
    return ret;
}

static int handle_get_param_tag(struct mixer *mixer, uint32_t tag_id,
                                const struct target_gkv *target,
                                uint32_t *values, uint32_t num)
{
    struct agm_tagged_get_config *get_cfg;
    struct apm_module_param_data_t *header;
    capi_v2_load_dummy_t *dummy;
    uint8_t *buf;
    size_t dummy_size, payload_size;
    size_t cfg_size, rsp_size;
    int ret;

    if (!mixer || !target || (target->num_gkvs == 0))
        return -EINVAL;

    ret = load_dummy_payload_size(num, &payload_size);
    if (ret) {
        printf("handle_get_param_tag: invalid get count %u\n", num);
        return ret;
    }
    dummy_size = LOAD_DUMMY_TYPESIZE((size_t)num);

    cfg_size = AGM_TAGGED_GET_CONFIG_SIZE(target->num_gkvs);
    get_cfg = calloc(1, cfg_size);
    if (!get_cfg)
        return -ENOMEM;

    get_cfg->tag_id = tag_id;
    get_cfg->num_gkvs = target->num_gkvs;
    memcpy(get_cfg->gkv, target->kv,
           target->num_gkvs * sizeof(struct agm_key_value));

    buf = calloc(1, payload_size);
    if (!buf) {
        printf("handle_get_param_tag: failed to allocate %zu bytes\n",
               payload_size);
        free(get_cfg);
        return -ENOMEM;
    }

    header = (struct apm_module_param_data_t *)buf;
    header->module_instance_id = 0;
    header->param_id = CAPI_V2_PARAM_ID_LOAD_DUMMY;
    header->error_code = 0;
    header->param_size = (uint32_t)dummy_size;

    printf(">>>>>>Mixer getParam(tag) CAPI_V2_PARAM_ID_LOAD_DUMMY\n");
    printf("tag_id = 0x%x, param_id = 0x%x, num = %u, num_gkvs = %u\n",
           tag_id, CAPI_V2_PARAM_ID_LOAD_DUMMY, num, target->num_gkvs);

    rsp_size = payload_size;
    ret = agm_mixer_get_param_by_tag(mixer, get_cfg, buf, rsp_size);
    if (ret) {
        printf("Mixer getParam(tag) Unsuccessful with rc = %d\n", ret);
        free(get_cfg);
        free(buf);
        return ret;
    }

    printf("Mixer getParam(tag) Successful with rc = %d\n", ret);
    dummy = (capi_v2_load_dummy_t *)(buf + sizeof(*header));
    printf("module returned num = %u\n", dummy->num);
    if (values && num)
        memcpy(values, dummy->array, num * sizeof(uint32_t));

    free(get_cfg);
    free(buf);
    return 0;
}

static int handle_direct_shmem_read(void *shmem_addr, uint32_t payload_size,
                                    const uint32_t *expected_values,
                                    uint32_t expected_count,
                                    uint32_t start_idx, uint32_t read_count,
                                    uint32_t delay_ms)
{
    struct apm_module_param_data_t *header;
    capi_v2_load_dummy_t *dummy;
    uint32_t compare_end;
    uint32_t i;
    int mismatches = 0;

    if (!shmem_addr || payload_size < sizeof(*header) + sizeof(*dummy))
        return -EINVAL;

    if (read_count == 0)
        return -EINVAL;

    compare_end = start_idx + read_count;
    if (compare_end < start_idx)
        return -EINVAL;

    if (delay_ms)
        usleep(delay_ms * 1000);

    header = (struct apm_module_param_data_t *)shmem_addr;
    dummy = (capi_v2_load_dummy_t *)((uint8_t *)shmem_addr + sizeof(*header));

    if (dummy->num >
        (payload_size - sizeof(*header) - sizeof(*dummy))/sizeof(uint32_t)) {
        printf("ERROR : direct read dummy num %u exceeds payload capacity %zu\n",
               dummy->num,
               (payload_size - sizeof(*header) - sizeof(*dummy))/sizeof(uint32_t));
        return -EINVAL;
    }

    printf(">>>>>>Direct shared-memory read from app virtual address\n");
    printf("param_id = 0x%x, param_size = %u, module returned num = %u, delay_ms = %u\n",
           header->param_id, header->param_size, dummy->num, delay_ms);

    if (header->param_id != CAPI_V2_PARAM_ID_LOAD_DUMMY) {
        printf("ERROR : direct read param_id mismatch, got 0x%x expected 0x%x\n",
               header->param_id, CAPI_V2_PARAM_ID_LOAD_DUMMY);
        return -EINVAL;
    }

    if (compare_end > dummy->num) {
        printf("ERROR : direct read window [%u..%u] exceeds dummy num %u\n",
               start_idx, compare_end - 1, dummy->num);
        return -EINVAL;
    }

    for (i = start_idx; i < compare_end; i++) {
        uint32_t expected = i;

        if (expected_values && (i < expected_count))
            expected = expected_values[i];

        printf("SHMEM:array[%u] = 0x%x, expected[%u] = 0x%x\n",
               i, dummy->array[i], i, expected);
        if (dummy->array[i] != expected) {
            printf("ERROR : direct shmem mismatch at index %u\n", i);
            mismatches++;
        }
    }

    if (mismatches == 0)
        printf("PASS: direct shared-memory read verified %u values (index window [%u..%u])\n",
               read_count, start_idx, compare_end - 1);
    else
        printf("FAIL: direct shared-memory read found %d mismatches out of %u values (index window [%u..%u])\n",
               mismatches, read_count, start_idx, compare_end - 1);

    return mismatches ? -EIO : 0;
}

static char *trim_space(char *str)
{
    char *end;

    if (!str)
        return NULL;

    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')
        str++;

    if (*str == '\0')
        return str;

    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' ||
           *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }

    return str;
}

static void interactive_print_help(void)
{
    printf("Interactive commands:\n");
    printf("\nCommon shared-memory setup:\n");
    printf("  help\n");
    printf("  status\n");
    printf("  tag <tag_id>\n");
    printf("  alloc <size>               allocate shared memory if not already allocated\n");
    printf("  write <count>\n");

    printf("\nExternal graph mode: graph is opened by another app, for example agmplay:\n");
    printf("  gkv <gkv>                  set an unnamed external target\n");
    printf("  target add <name> <gkv>    save an external target by name\n");
    printf("  target use <name>\n");
    printf("  target list\n");
    printf("  set                        send shared-memory SET to current external target\n");
    printf("  get <count> [start]        copy-based GET from current target\n");

    printf("\nOwner graph mode: graph is opened and controlled by this test app:\n");
    printf("  graph add <name> -d <dev> -i <intf> -skv <kv> -dkv <kv> [owner opts]\n");
    printf("  graph use <name>\n");
    printf("  graph list\n");
    printf("  graph open [name]\n");
    printf("  graph set [name]           open if needed and share memory to owner graph\n");
    printf("  graph start [name]\n");
    printf("  graph stop [name]\n");
    printf("  graph close [name]\n");

    printf("\nVerification and session control:\n");
    printf("  read <count> [start]       direct app virtual-address read\n");
    printf("  sleep <ms>\n");
    printf("  exit\n");
}

static int interactive_find_target(struct interactive_target *targets,
                                   const char *name)
{
    int i;

    if (!targets || !name)
        return -1;

    for (i = 0; i < MAX_INTERACTIVE_TARGETS; i++) {
        if (targets[i].valid && !strcmp(targets[i].name, name))
            return i;
    }

    return -1;
}

static int interactive_add_target(struct interactive_target *targets,
                                  const char *name, const char *gkv_spec)
{
    int idx;
    int ret;

    if (!targets || !name || !gkv_spec || strlen(name) == 0 ||
        strlen(name) >= INTERACTIVE_TARGET_NAME_LEN)
        return -EINVAL;

    idx = interactive_find_target(targets, name);
    if (idx < 0) {
        for (idx = 0; idx < MAX_INTERACTIVE_TARGETS; idx++) {
            if (!targets[idx].valid)
                break;
        }
        if (idx == MAX_INTERACTIVE_TARGETS)
            return -ENOMEM;
    }

    ret = parse_target_gkv(gkv_spec, &targets[idx].target);
    if (ret)
        return ret;

    targets[idx].valid = true;
    snprintf(targets[idx].name, sizeof(targets[idx].name), "%s", name);
    return idx;
}

static void interactive_list_targets(struct interactive_target *targets,
                                     int current_idx)
{
    int i;

    for (i = 0; i < MAX_INTERACTIVE_TARGETS; i++) {
        if (!targets[i].valid)
            continue;
        printf("%c %s\n", (i == current_idx) ? '*' : ' ', targets[i].name);
        print_target_gkv(&targets[i].target);
    }
}

static int interactive_find_owner_graph(struct interactive_owner_graph *graphs,
                                        const char *name)
{
    int i;

    if (!graphs || !name)
        return -1;

    for (i = 0; i < MAX_INTERACTIVE_OWNER_GRAPHS; i++) {
        if (graphs[i].valid && !strcmp(graphs[i].name, name))
            return i;
    }

    return -1;
}

static int interactive_owner_graph_init(struct interactive_owner_graph *entry,
                                        const char *name,
                                        const struct owner_graph_config *graph)
{
    int ret;

    if (!entry || !name || !graph || strlen(name) == 0 ||
        strlen(name) >= INTERACTIVE_TARGET_NAME_LEN)
        return -EINVAL;

    memset(entry, 0, sizeof(*entry));
    entry->graph = *graph;
    entry->graph.enabled = true;
    entry->graph.opened = false;
    entry->graph.connected = false;
    entry->graph.started = false;
    entry->graph.pcm = NULL;
    memset(&entry->graph.pcm_config, 0, sizeof(entry->graph.pcm_config));

    if (graph->intf_name) {
        if (strlen(graph->intf_name) >= sizeof(entry->intf_name))
            return -EINVAL;
        snprintf(entry->intf_name, sizeof(entry->intf_name), "%s",
                 graph->intf_name);
        entry->graph.intf_name = entry->intf_name;
    }

    ret = owner_graph_build_target_gkv(&entry->graph, &entry->target);
    if (ret)
        return ret;

    entry->valid = true;
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    return 0;
}

static int interactive_parse_owner_graph_opts(char *opts,
                                              struct owner_graph_config *graph,
                                              char *intf_name,
                                              size_t intf_name_size)
{
    char *saveptr = NULL;
    char *tok;

    if (!opts || !graph || !intf_name || intf_name_size == 0)
        return -EINVAL;

    memset(graph, 0, sizeof(*graph));
    graph->enabled = true;
    graph->device = UINT_MAX;
    graph->channels = 2;
    graph->rate = 48000;
    graph->bits = 16;

    tok = strtok_r(opts, " \t\r\n", &saveptr);
    while (tok) {
        char *val = strtok_r(NULL, " \t\r\n", &saveptr);

        if (!strcmp(tok, "-d")) {
            if (!val)
                return -EINVAL;
            graph->device = (unsigned int)atoi(val);
        } else if (!strcmp(tok, "-i")) {
            if (!val || strlen(val) >= intf_name_size)
                return -EINVAL;
            snprintf(intf_name, intf_name_size, "%s", val);
            graph->intf_name = intf_name;
        } else if (!strcmp(tok, "-skv")) {
            if (!val)
                return -EINVAL;
            graph->stream_kv = convert_char_to_hex(val);
        } else if (!strcmp(tok, "-dkv")) {
            if (!val)
                return -EINVAL;
            graph->device_kv = convert_char_to_hex(val);
        } else if (!strcmp(tok, "-dppkv")) {
            if (!val)
                return -EINVAL;
            graph->devicepp_kv = convert_char_to_hex(val);
        } else if (!strcmp(tok, "-ikv")) {
            if (!val)
                return -EINVAL;
            graph->instance_kv = (uint32_t)strtoul(val, NULL, 0);
        } else if (!strcmp(tok, "-vmid_kv")) {
            if (!val)
                return -EINVAL;
            graph->vmid_kv = convert_char_to_hex(val);
        } else if (!strcmp(tok, "-offload")) {
            if (!val)
                return -EINVAL;
            graph->offload_proc_kv = convert_char_to_hex(val);
        } else if (!strcmp(tok, "--channels") || !strcmp(tok, "-C")) {
            if (!val)
                return -EINVAL;
            graph->channels = (unsigned int)atoi(val);
        } else if (!strcmp(tok, "--rate")) {
            if (!val)
                return -EINVAL;
            graph->rate = (unsigned int)atoi(val);
        } else if (!strcmp(tok, "--bits")) {
            if (!val)
                return -EINVAL;
            graph->bits = (unsigned int)atoi(val);
        } else {
            printf("unknown graph option '%s'\n", tok);
            return -EINVAL;
        }

        tok = strtok_r(NULL, " \t\r\n", &saveptr);
    }

    return owner_graph_validate(graph);
}

static int interactive_add_owner_graph(struct mixer *mixer,
                                       struct interactive_owner_graph *graphs,
                                       const char *name, char *opts)
{
    struct owner_graph_config graph;
    char intf_name[INTERACTIVE_INTF_NAME_LEN] = { 0 };
    int idx;
    int ret;

    if (!mixer || !graphs || !name || !opts)
        return -EINVAL;

    ret = interactive_parse_owner_graph_opts(opts, &graph, intf_name,
                                             sizeof(intf_name));
    if (ret)
        return ret;

    idx = interactive_find_owner_graph(graphs, name);
    if (idx >= 0)
        owner_graph_close(mixer, &graphs[idx].graph);
    else {
        for (idx = 0; idx < MAX_INTERACTIVE_OWNER_GRAPHS; idx++) {
            if (!graphs[idx].valid)
                break;
        }
        if (idx == MAX_INTERACTIVE_OWNER_GRAPHS)
            return -ENOMEM;
    }

    ret = interactive_owner_graph_init(&graphs[idx], name, &graph);
    if (ret)
        return ret;

    return idx;
}

static void interactive_list_owner_graphs(struct interactive_owner_graph *graphs,
                                          int current_idx)
{
    int i;

    for (i = 0; i < MAX_INTERACTIVE_OWNER_GRAPHS; i++) {
        if (!graphs[i].valid)
            continue;

        printf("%c %s: device %u intf %s opened=%u started=%u\n",
               (i == current_idx) ? '*' : ' ', graphs[i].name,
               graphs[i].graph.device,
               graphs[i].graph.intf_name ? graphs[i].graph.intf_name : "",
               graphs[i].graph.opened, graphs[i].graph.started);
        print_target_gkv(&graphs[i].target);
    }
}

static int interactive_resolve_owner_graph(struct interactive_owner_graph *graphs,
                                           int current_idx,
                                           const char *name)
{
    if (name)
        return interactive_find_owner_graph(graphs, name);

    return current_idx;
}

static int interactive_owner_graph_set(struct mixer *mixer, unsigned int card,
                                       struct interactive_owner_graph *entry,
                                       uint32_t tag_id,
                                       const struct agm_shmem_info *info,
                                       bool shmem_active,
                                       uint32_t payload_size)
{
    int ret;

    if (!mixer || !entry || !entry->valid)
        return -EINVAL;
    if (tag_id == 0 || !shmem_active || payload_size == 0)
        return -EINVAL;

    if (!entry->graph.opened) {
        ret = owner_graph_open(mixer, card, &entry->graph);
        if (ret)
            return ret;
    }

    ret = handle_set_param_shmem(mixer, tag_id, info, &entry->target,
                                 payload_size);
    printf("graph %s set rc %d\n", entry->name, ret);
    return ret;
}

static void interactive_close_all_owner_graphs(struct mixer *mixer,
                                               struct interactive_owner_graph *graphs)
{
    int i;

    if (!graphs)
        return;

    for (i = 0; i < MAX_INTERACTIVE_OWNER_GRAPHS; i++) {
        if (graphs[i].valid)
            owner_graph_close(mixer, &graphs[i].graph);
    }
}

static int interactive_write_payload(void *shmem_addr,
                                     const struct agm_shmem_info *info,
                                     uint32_t count,
                                     uint32_t **write_shadow,
                                     uint32_t *write_shadow_count,
                                     uint32_t *write_count,
                                     uint32_t *payload_size)
{
    uint32_t *new_shadow = NULL;
    int ret;

    if (!shmem_addr || !info || !write_shadow || !write_shadow_count ||
        !write_count || !payload_size)
        return -EINVAL;

    if (count > 0) {
        new_shadow = calloc(count, sizeof(uint32_t));
        if (!new_shadow)
            return -ENOMEM;
        fill_index_pattern(new_shadow, count);
    }

    ret = prepare_load_dummy_shmem_payload(shmem_addr, info, new_shadow,
                                           count, payload_size);
    if (ret) {
        free(new_shadow);
        return ret;
    }

    free(*write_shadow);
    *write_shadow = new_shadow;
    *write_shadow_count = count;
    *write_count = count;

    printf("wrote %u uint32_t values to shared memory payload\n", count);
    return 0;
}

static int interactive_get_and_compare(struct mixer *mixer, uint32_t tag_id,
                                       const struct target_gkv *target,
                                       const uint32_t *write_shadow,
                                       uint32_t write_shadow_count,
                                       uint32_t write_count,
                                       uint32_t start_idx,
                                       uint32_t get_count)
{
    uint32_t compare_end;
    uint32_t total_get_count;
    uint32_t *get_buf;
    uint64_t needed_elems;
    uint32_t i;
    int ret;
    int mismatches = 0;

    if (!mixer || tag_id == 0 || !target || target->num_gkvs == 0 ||
        get_count == 0)
        return -EINVAL;

    compare_end = start_idx + get_count;
    if (compare_end < start_idx)
        return -EINVAL;

    needed_elems = (uint64_t)start_idx + (uint64_t)get_count;
    if (needed_elems > UINT32_MAX)
        return -EINVAL;

    total_get_count = (write_count > 0) ? write_count : (uint32_t)needed_elems;
    if (total_get_count < (uint32_t)needed_elems)
        total_get_count = (uint32_t)needed_elems;

    get_buf = calloc(total_get_count, sizeof(uint32_t));
    if (!get_buf)
        return -ENOMEM;

    ret = handle_get_param_tag(mixer, tag_id, target, get_buf,
                               total_get_count);
    if (ret) {
        printf("handle_get_param(tag), rc %d\n", ret);
        free(get_buf);
        return ret;
    }

    for (i = start_idx; i < compare_end; i++) {
        uint32_t expected = i;

        if (write_shadow && (i < write_shadow_count))
            expected = write_shadow[i];

        printf("SPF:array[%u] = 0x%x, expected[%u] = 0x%x\n",
               i, get_buf[i], i, expected);
        if (get_buf[i] != expected) {
            printf("ERROR : mismatch at index %u\n", i);
            mismatches++;
        }
    }

    if (mismatches == 0)
        printf("PASS: %u values verified (index window [%u..%u])\n",
               get_count, start_idx, compare_end - 1);
    else
        printf("FAIL: %d mismatches out of %u values (index window [%u..%u])\n",
               mismatches, get_count, start_idx, compare_end - 1);

    free(get_buf);
    return mismatches ? -EIO : 0;
}

static int interactive_console(struct mixer *mixer,
                               unsigned int card,
                               struct owner_graph_config *initial_owner_graph,
                               struct agm_shmem_info *info,
                               bool *shmem_active,
                               bool *shmem_mapped,
                               void **shmem_addr,
                               uint32_t *tag_id,
                               struct target_gkv *current_target,
                               uint32_t **write_shadow,
                               uint32_t *write_shadow_count,
                               uint32_t *write_count,
                               uint32_t *payload_size,
                               uint32_t direct_read_delay_ms)
{
    struct interactive_target targets[MAX_INTERACTIVE_TARGETS] = { 0 };
    struct interactive_owner_graph owner_graphs[MAX_INTERACTIVE_OWNER_GRAPHS] = { 0 };
    char line[INTERACTIVE_LINE_LEN];
    int current_idx = -1;
    int current_owner_idx = -1;
    int ret;

    if (!mixer || !info || !shmem_active || !shmem_mapped || !shmem_addr ||
        !tag_id || !current_target || !write_shadow || !write_shadow_count ||
        !write_count || !payload_size)
        return -EINVAL;

    printf("Entering interactive mode. Type 'help' for commands.\n");
    if (current_target->num_gkvs != 0) {
        targets[0].valid = true;
        snprintf(targets[0].name, sizeof(targets[0].name), "default");
        targets[0].target = *current_target;
        current_idx = 0;
        printf("current target: default\n");
    }

    if (initial_owner_graph && initial_owner_graph->enabled) {
        ret = interactive_owner_graph_init(&owner_graphs[0], "default",
                                           initial_owner_graph);
        if (ret)
            return ret;
        current_owner_idx = 0;
        current_idx = -1;
        *current_target = owner_graphs[0].target;
        printf("current owner graph: default\n");
        print_target_gkv(current_target);
    }

    while (1) {
        char *cmd;
        char *saveptr = NULL;
        char *arg1;
        char *arg2;

        printf("agm-shmem> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;

        cmd = trim_space(line);
        if (!cmd || *cmd == '\0')
            continue;

        cmd = strtok_r(cmd, " \t\r\n", &saveptr);
        if (!cmd)
            continue;

        if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
            interactive_print_help();
        } else if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit")) {
            break;
        } else if (!strcmp(cmd, "status")) {
            printf("tag_id = 0x%x\n", *tag_id);
            printf("shmem_active = %u, spf_mem_handle = 0x%x, payload_size = %u, addr = %p\n",
                   *shmem_active, info->spf_mem_handle, *payload_size,
                   *shmem_addr);
            if (current_idx >= 0)
                printf("current target = %s\n", targets[current_idx].name);
            else
                printf("current target = unnamed/direct\n");
            if (current_owner_idx >= 0)
                printf("current owner graph = %s\n", owner_graphs[current_owner_idx].name);
            print_target_gkv(current_target);
        } else if (!strcmp(cmd, "tag")) {
            arg1 = strtok_r(NULL, " \t\r\n", &saveptr);
            if (!arg1) {
                printf("usage: tag <tag_id>\n");
                continue;
            }
            *tag_id = (uint32_t)strtoul(arg1, NULL, 0);
            printf("tag_id = 0x%x\n", *tag_id);
        } else if (!strcmp(cmd, "alloc")) {
            uint32_t size;

            arg1 = strtok_r(NULL, " \t\r\n", &saveptr);
            arg2 = strtok_r(NULL, " \t\r\n", &saveptr);
            if (!arg1 || arg2) {
                printf("usage: alloc <size>\n");
                continue;
            }
            if (*shmem_active) {
                printf("Error : shared memory is already allocated; exit to free it\n");
                continue;
            }

            size = (uint32_t)strtoul(arg1, NULL, 0);
            if (size == 0) {
                printf("Wrong shared memory size = %u\n", size);
                continue;
            }

            memset(info, 0, sizeof(*info));
            info->size = size;
            info->cache = 0;

            ret = agm_mixer_shmem_alloc(mixer, info);
            if (ret) {
                printf("agm_mixer_shmem_alloc failed, ret %d\n", ret);
                continue;
            }

            *shmem_addr = mmap(NULL, info->size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, info->ion_fd, 0);
            if (*shmem_addr == MAP_FAILED) {
                printf("mmap failed on ion_fd %d, errno %d\n", info->ion_fd,
                       errno);
                *shmem_addr = NULL;
                agm_mixer_shmem_free(mixer, info->spf_mem_handle);
                if ((int)info->ion_fd > 0)
                    close((int)info->ion_fd);
                memset(info, 0, sizeof(*info));
                continue;
            }

            *shmem_active = true;
            *shmem_mapped = true;
            *payload_size = 0;
            free(*write_shadow);
            *write_shadow = NULL;
            *write_shadow_count = 0;
            *write_count = 0;
            printf("shmemAlloc succeeded: ion_fd %d, spf_addr 0x%llx, spf_mem_handle 0x%x\n",
                   info->ion_fd, (unsigned long long)info->spf_addr,
                   info->spf_mem_handle);
        } else if (!strcmp(cmd, "write")) {
            uint32_t count;

            arg1 = strtok_r(NULL, " \t\r\n", &saveptr);
            if (!arg1) {
                printf("usage: write <count>\n");
                continue;
            }
            if (!*shmem_active || !*shmem_addr) {
                printf("Error : shared memory not allocated/mapped\n");
                continue;
            }
            count = (uint32_t)strtoul(arg1, NULL, 0);
            ret = interactive_write_payload(*shmem_addr, info, count,
                                            write_shadow,
                                            write_shadow_count,
                                            write_count,
                                            payload_size);
            if (ret)
                printf("write failed, ret %d\n", ret);
        } else if (!strcmp(cmd, "target")) {
            arg1 = strtok_r(NULL, " \t\r\n", &saveptr);
            if (!arg1) {
                printf("usage: target add/use/list ...\n");
                continue;
            }

            if (!strcmp(arg1, "list")) {
                interactive_list_targets(targets, current_idx);
            } else if (!strcmp(arg1, "add")) {
                char *name = strtok_r(NULL, " \t\r\n", &saveptr);
                char *gkv_spec = trim_space(saveptr);
                int idx;

                if (!name || !gkv_spec || *gkv_spec == '\0') {
                    printf("usage: target add <name> <gkv>\n");
                    continue;
                }

                idx = interactive_add_target(targets, name, gkv_spec);
                if (idx < 0) {
                    printf("target add failed, ret %d\n", idx);
                    continue;
                }
                current_idx = idx;
                current_owner_idx = -1;
                *current_target = targets[idx].target;
                printf("current target = %s\n", targets[idx].name);
                print_target_gkv(current_target);
            } else if (!strcmp(arg1, "use")) {
                char *name = strtok_r(NULL, " \t\r\n", &saveptr);
                int idx;

                if (!name) {
                    printf("usage: target use <name>\n");
                    continue;
                }

                idx = interactive_find_target(targets, name);
                if (idx < 0) {
                    printf("target '%s' not found\n", name);
                    continue;
                }
                current_idx = idx;
                current_owner_idx = -1;
                *current_target = targets[idx].target;
                printf("current target = %s\n", targets[idx].name);
                print_target_gkv(current_target);
            } else {
                printf("usage: target add/use/list ...\n");
            }
        } else if (!strcmp(cmd, "graph")) {
            arg1 = strtok_r(NULL, " \t\r\n", &saveptr);
            if (!arg1) {
                printf("usage: graph add/use/list/open/set/start/stop/close ...\n");
                continue;
            }

            if (!strcmp(arg1, "list")) {
                interactive_list_owner_graphs(owner_graphs, current_owner_idx);
            } else if (!strcmp(arg1, "add")) {
                char *name = strtok_r(NULL, " \t\r\n", &saveptr);
                char *opts = trim_space(saveptr);
                int idx;

                if (!name || !opts || *opts == '\0') {
                    printf("usage: graph add <name> -d <dev> -i <intf> -skv <kv> -dkv <kv> [owner opts]\n");
                    continue;
                }

                idx = interactive_add_owner_graph(mixer, owner_graphs, name,
                                                  opts);
                if (idx < 0) {
                    printf("graph add failed, ret %d\n", idx);
                    continue;
                }
                current_owner_idx = idx;
                current_idx = -1;
                *current_target = owner_graphs[idx].target;
                printf("current owner graph = %s\n", owner_graphs[idx].name);
                print_target_gkv(current_target);
            } else if (!strcmp(arg1, "use")) {
                char *name = strtok_r(NULL, " \t\r\n", &saveptr);
                int idx;

                if (!name) {
                    printf("usage: graph use <name>\n");
                    continue;
                }

                idx = interactive_find_owner_graph(owner_graphs, name);
                if (idx < 0) {
                    printf("graph '%s' not found\n", name);
                    continue;
                }
                current_owner_idx = idx;
                current_idx = -1;
                *current_target = owner_graphs[idx].target;
                printf("current owner graph = %s\n", owner_graphs[idx].name);
                print_target_gkv(current_target);
            } else if (!strcmp(arg1, "open")) {
                char *name = strtok_r(NULL, " \t\r\n", &saveptr);
                int idx = interactive_resolve_owner_graph(owner_graphs,
                                                          current_owner_idx,
                                                          name);

                if (idx < 0) {
                    printf("owner graph not selected/found\n");
                    continue;
                }
                if (!owner_graphs[idx].graph.opened) {
                    ret = owner_graph_open(mixer, card,
                                           &owner_graphs[idx].graph);
                    if (ret)
                        printf("graph open failed, ret %d\n", ret);
                }
            } else if (!strcmp(arg1, "set")) {
                char *name = strtok_r(NULL, " \t\r\n", &saveptr);
                int idx = interactive_resolve_owner_graph(owner_graphs,
                                                          current_owner_idx,
                                                          name);

                if (idx < 0) {
                    printf("owner graph not selected/found\n");
                    continue;
                }
                ret = interactive_owner_graph_set(mixer, card,
                                                  &owner_graphs[idx],
                                                  *tag_id, info,
                                                  *shmem_active,
                                                  *payload_size);
                if (ret)
                    printf("graph set failed, ret %d\n", ret);
            } else if (!strcmp(arg1, "start")) {
                char *name = strtok_r(NULL, " \t\r\n", &saveptr);
                int idx = interactive_resolve_owner_graph(owner_graphs,
                                                          current_owner_idx,
                                                          name);

                if (idx < 0) {
                    printf("owner graph not selected/found\n");
                    continue;
                }
                if (!owner_graphs[idx].graph.opened) {
                    ret = owner_graph_open(mixer, card,
                                           &owner_graphs[idx].graph);
                    if (ret) {
                        printf("graph open failed, ret %d\n", ret);
                        continue;
                    }
                }
                ret = owner_graph_start(&owner_graphs[idx].graph);
                if (ret)
                    printf("graph start failed, ret %d\n", ret);
            } else if (!strcmp(arg1, "stop")) {
                char *name = strtok_r(NULL, " \t\r\n", &saveptr);
                int idx = interactive_resolve_owner_graph(owner_graphs,
                                                          current_owner_idx,
                                                          name);

                if (idx < 0) {
                    printf("owner graph not selected/found\n");
                    continue;
                }
                ret = owner_graph_stop(&owner_graphs[idx].graph);
                if (ret)
                    printf("graph stop failed, ret %d\n", ret);
            } else if (!strcmp(arg1, "close")) {
                char *name = strtok_r(NULL, " \t\r\n", &saveptr);
                int idx = interactive_resolve_owner_graph(owner_graphs,
                                                          current_owner_idx,
                                                          name);

                if (idx < 0) {
                    printf("owner graph not selected/found\n");
                    continue;
                }
                owner_graph_close(mixer, &owner_graphs[idx].graph);
            } else {
                printf("usage: graph add/use/list/open/set/start/stop/close ...\n");
            }
        } else if (!strcmp(cmd, "gkv")) {
            char *gkv_spec = trim_space(saveptr);

            if (!gkv_spec || *gkv_spec == '\0') {
                printf("usage: gkv <gkv>\n");
                continue;
            }
            ret = parse_target_gkv(gkv_spec, current_target);
            if (ret) {
                printf("invalid gkv, ret %d\n", ret);
                continue;
            }
            current_idx = -1;
            current_owner_idx = -1;
            print_target_gkv(current_target);
        } else if (!strcmp(cmd, "set")) {
            if (*tag_id == 0) {
                printf("Error : tag id is not set\n");
                continue;
            }
            if (current_target->num_gkvs == 0) {
                printf("Error : target GKV is not set\n");
                continue;
            }
            if (!*shmem_active) {
                printf("Error : shared memory is not allocated\n");
                continue;
            }
            if (*payload_size == 0) {
                printf("Error : payload is not prepared, use write <count>\n");
                continue;
            }

            ret = handle_set_param_shmem(mixer, *tag_id, info,
                                         current_target, *payload_size);
            printf("handle_set_param(shared-mem), rc %d\n", ret);
        } else if (!strcmp(cmd, "get")) {
            uint32_t get_count;
            uint32_t start_idx = 0;

            arg1 = strtok_r(NULL, " \t\r\n", &saveptr);
            arg2 = strtok_r(NULL, " \t\r\n", &saveptr);
            if (!arg1) {
                printf("usage: get <count> [start]\n");
                continue;
            }
            get_count = (uint32_t)strtoul(arg1, NULL, 0);
            if (arg2)
                start_idx = (uint32_t)strtoul(arg2, NULL, 0);

            ret = interactive_get_and_compare(mixer, *tag_id, current_target,
                                              *write_shadow,
                                              *write_shadow_count,
                                              *write_count, start_idx,
                                              get_count);
            if (ret)
                printf("get failed, ret %d\n", ret);
        } else if (!strcmp(cmd, "read")) {
            uint32_t read_count;
            uint32_t start_idx = 0;

            arg1 = strtok_r(NULL, " \t\r\n", &saveptr);
            arg2 = strtok_r(NULL, " \t\r\n", &saveptr);
            if (!arg1) {
                printf("usage: read <count> [start]\n");
                continue;
            }
            if (!*shmem_active || !*shmem_addr || *payload_size == 0) {
                printf("Error : shared memory payload is not ready\n");
                continue;
            }
            read_count = (uint32_t)strtoul(arg1, NULL, 0);
            if (arg2)
                start_idx = (uint32_t)strtoul(arg2, NULL, 0);

            ret = handle_direct_shmem_read(*shmem_addr, *payload_size,
                                           *write_shadow,
                                           *write_shadow_count, start_idx,
                                           read_count, direct_read_delay_ms);
            if (ret)
                printf("read failed, ret %d\n", ret);
        } else if (!strcmp(cmd, "sleep")) {
            uint32_t sleep_ms;

            arg1 = strtok_r(NULL, " \t\r\n", &saveptr);
            if (!arg1) {
                printf("usage: sleep <ms>\n");
                continue;
            }
            sleep_ms = (uint32_t)strtoul(arg1, NULL, 0);
            usleep(sleep_ms * 1000);
        } else {
            printf("unknown command '%s'\n", cmd);
        }
    }

    interactive_close_all_owner_graphs(mixer, owner_graphs);
    return 0;
}

int main(int argc, char **argv)
{
    struct agm_shmem_info info;
    unsigned int card = 100;
    int free_flag = 1;
    bool shmem_active = false;
    bool shmem_mapped = false;
    void *shmem_addr = NULL;
    uint32_t tag_id = 0;
    uint32_t write_count = 0;
    uint32_t shmem_payload_size = 0;
    uint32_t *write_shadow = NULL;
    uint32_t write_shadow_count = 0;
    uint32_t get_start_idx = 0;
    uint32_t direct_read_delay_ms = 5;
    uint32_t *get_buf = NULL;
    struct target_gkv current_target = { 0 };
    bool target_gkv_explicit = false;
    bool interactive_mode = false;
    bool owner_hold_done = false;
    struct owner_graph_config owner_graph = {
        .device = UINT_MAX,
        .channels = 2,
        .rate = 48000,
        .bits = 16,
    };
    uint32_t owner_hold_ms = 0;
    int ret = 0;
    int mismatches = 0;
    char **args;
    char **scan_args;
    struct mixer *mixer = NULL;

    memset(&info, 0, sizeof(info));

    if (argc == 1) {
        usage();
        return 0;
    }

    printf("agm_shared_memory_test\n");

    scan_args = argv + 1;
    while (*scan_args) {
        if (strcmp(*scan_args, "-D") == 0) {
            scan_args++;
            if (*scan_args)
                card = (unsigned int)atoi(*scan_args);
        }
        if (*scan_args)
            scan_args++;
    }

    mixer = mixer_open(card);
    if (!mixer) {
        printf("Failed to open mixer card %u\n", card);
        return -ENODEV;
    }

    args = argv + 1;
    while (*args) {
        if (strcmp(*args, "-D") == 0) {
            args++;
            if (*args)
                card = (unsigned int)atoi(*args);
        } else if ((strcmp(*args, "--interactive") == 0) ||
                   (strcmp(*args, "-I") == 0)) {
            interactive_mode = true;
        } else if (strcmp(*args, "--open-graph") == 0) {
            owner_graph.enabled = true;
        } else if (strcmp(*args, "-d") == 0) {
            args++;
            if (*args)
                owner_graph.device = (unsigned int)atoi(*args);
        } else if (strcmp(*args, "-i") == 0) {
            args++;
            if (*args)
                owner_graph.intf_name = *args;
        } else if (strcmp(*args, "-skv") == 0) {
            args++;
            if (*args)
                owner_graph.stream_kv = convert_char_to_hex(*args);
        } else if (strcmp(*args, "-dkv") == 0) {
            args++;
            if (*args)
                owner_graph.device_kv = convert_char_to_hex(*args);
        } else if (strcmp(*args, "-dppkv") == 0) {
            args++;
            if (*args)
                owner_graph.devicepp_kv = convert_char_to_hex(*args);
        } else if (strcmp(*args, "-ikv") == 0) {
            args++;
            if (*args)
                owner_graph.instance_kv = (uint32_t)strtoul(*args, NULL, 0);
        } else if (strcmp(*args, "-vmid_kv") == 0) {
            args++;
            if (*args)
                owner_graph.vmid_kv = convert_char_to_hex(*args);
        } else if (strcmp(*args, "-offload") == 0) {
            args++;
            if (*args)
                owner_graph.offload_proc_kv = convert_char_to_hex(*args);
        } else if ((strcmp(*args, "--channels") == 0) ||
                   (strcmp(*args, "-C") == 0)) {
            args++;
            if (*args)
                owner_graph.channels = (unsigned int)atoi(*args);
        } else if (strcmp(*args, "--rate") == 0) {
            args++;
            if (*args)
                owner_graph.rate = (unsigned int)atoi(*args);
        } else if (strcmp(*args, "--bits") == 0) {
            args++;
            if (*args)
                owner_graph.bits = (unsigned int)atoi(*args);
        } else if (strcmp(*args, "--hold-ms") == 0) {
            args++;
            if (*args)
                owner_hold_ms = (uint32_t)strtoul(*args, NULL, 0);
        } else if (strcmp(*args, "-T") == 0) {
            args++;
            if (*args) {
                tag_id = (uint32_t)strtoul(*args, NULL, 16);
                if (tag_id == 0) {
                    printf("Error : invalid -T tag id '%s'\n", *args);
                    ret = -EINVAL;
                    goto quit;
                }
                printf("tag_id = 0x%x\n", tag_id);
            }
        } else if ((strcmp(*args, "-gkv") == 0) ||
                   (strcmp(*args, "--target-gkv") == 0)) {
            args++;
            if (*args) {
                ret = parse_target_gkv(*args, &current_target);
                if (ret) {
                    printf("Error : invalid target GKV '%s'\n", *args);
                    goto quit;
                }
                target_gkv_explicit = true;
                print_target_gkv(&current_target);
            }
        } else if (strcmp(*args, "-f") == 0) {
            args++;
            if (*args)
                free_flag = atoi(*args);
        } else if (strcmp(*args, "-u") == 0) {
            args++;
            if (*args) {
                uint32_t size = (uint32_t)strtoul(*args, NULL, 0);

                printf("begin alloc shared memory command\n");
                if (size == 0) {
                    printf("Wrong shared memory size = %u\n", size);
                    ret = -EINVAL;
                    goto quit;
                }

                memset(&info, 0, sizeof(info));
                info.size = size;
                info.cache = 0;

                ret = agm_mixer_shmem_alloc(mixer, &info);
                if (ret) {
                    printf("agm_mixer_shmem_alloc failed, ret %d\n", ret);
                    goto quit;
                }
                shmem_active = true;

                printf("shmemAlloc succeeded: ion_fd %d, spf_addr 0x%llx, spf_mem_handle 0x%x\n",
                       info.ion_fd, (unsigned long long)info.spf_addr,
                       info.spf_mem_handle);
                shmem_addr = mmap(NULL, info.size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, info.ion_fd, 0);
                if (shmem_addr == MAP_FAILED) {
                    printf("mmap failed on ion_fd %d, errno %d\n", info.ion_fd,
                           errno);
                    ret = -errno;
                    shmem_addr = NULL;
                    goto quit;
                }
                shmem_mapped = true;
            }
        } else if (strcmp(*args, "-w") == 0) {
            char *end = NULL;
            unsigned long value;

            args++;
            if (*args) {
                errno = 0;
                value = strtoul(*args, &end, 0);
                if (errno || (end == *args) || *end || value > UINT32_MAX) {
                    printf("Error : invalid write count %s\n", *args);
                    ret = -EINVAL;
                    goto quit;
                }
                write_count = (uint32_t)value;

                if (shmem_addr == NULL) {
                    printf("Error : shared memory not allocated in this process, use -u before -w\n");
                    ret = -EINVAL;
                    goto quit;
                }

                free(write_shadow);
                write_shadow = NULL;
                write_shadow_count = 0;
                if (write_count > 0) {
                    write_shadow = calloc(write_count, sizeof(uint32_t));
                    if (!write_shadow) {
                        printf("Error : failed to allocate write shadow buffer\n");
                        ret = -ENOMEM;
                        goto quit;
                    }
                    fill_index_pattern(write_shadow, write_count);
                    write_shadow_count = write_count;
                }

                ret = prepare_load_dummy_shmem_payload(shmem_addr, &info,
                                                       write_shadow,
                                                       write_count,
                                                       &shmem_payload_size);
                if (ret)
                    goto quit;

                printf("wrote %u uint32_t values to shared memory payload\n",
                       write_count);
            }
        } else if (strcmp(*args, "-P2") == 0) {
            printf("handle_set_param(shared-mem), CAPI_V2_PARAM_ID_LOAD_DUMMY\n");

            if (tag_id == 0) {
                printf("Error : -T <tag_id> must be given before -P2\n");
                ret = -EINVAL;
                goto quit;
            }

            if (owner_graph.enabled) {
                if (!owner_graph.opened) {
                    ret = owner_graph_open(mixer, card, &owner_graph);
                    if (ret)
                        goto quit;
                }

                if (!target_gkv_explicit && current_target.num_gkvs == 0) {
                    ret = owner_graph_build_target_gkv(&owner_graph,
                                                       &current_target);
                    if (ret) {
                        printf("Error : failed to infer owner graph target GKV, ret %d\n",
                               ret);
                        goto quit;
                    }
                    print_target_gkv(&current_target);
                }
            }

            if (current_target.num_gkvs == 0) {
                printf("Error : -gkv/--target-gkv must be given before -P2, unless --open-graph graph options are provided\n");
                ret = -EINVAL;
                goto quit;
            }

            if (!shmem_active) {
                printf("Error : shared memory not allocated, use -u before -P2 in the same command\n");
                ret = -EINVAL;
                goto quit;
            }
            if (shmem_payload_size == 0) {
                printf("Error : shared memory has no prepared payload, use -w before -P2 in the same command\n");
                ret = -EINVAL;
                goto quit;
            }

            ret = handle_set_param_shmem(mixer, tag_id, &info,
                                          &current_target,
                                          shmem_payload_size);
            printf("handle_set_param(shared-mem), rc %d\n", ret);
            if (ret)
                goto quit;
        } else if (strcmp(*args, "--start-graph") == 0) {
            if (!owner_graph.enabled) {
                printf("Error : --start-graph requires --open-graph\n");
                ret = -EINVAL;
                goto quit;
            }

            if (!owner_graph.opened) {
                ret = owner_graph_open(mixer, card, &owner_graph);
                if (ret)
                    goto quit;
            }

            if (!owner_graph.started) {
                ret = owner_graph_start(&owner_graph);
                if (ret)
                    goto quit;
            }

            if (owner_hold_ms) {
                printf("holding owner graph for %u ms\n", owner_hold_ms);
                usleep(owner_hold_ms * 1000);
                owner_hold_done = true;
            }
        } else if (strcmp(*args, "-R2") == 0) {
            args++;
            if (*args) {
                uint32_t read_count = (uint32_t)atoi(*args);

                printf("handle_direct_shmem_read, CAPI_V2_PARAM_ID_LOAD_DUMMY, num = %u\n",
                       read_count);

                if (!shmem_active || !shmem_addr) {
                    printf("Error : shared memory not allocated/mapped, use -u before -R2\n");
                    ret = -EINVAL;
                    goto quit;
                }

                if (shmem_payload_size == 0) {
                    printf("Error : shared memory has no prepared payload, use -w before -R2\n");
                    ret = -EINVAL;
                    goto quit;
                }

                ret = handle_direct_shmem_read(shmem_addr, shmem_payload_size,
                                               write_shadow, write_shadow_count,
                                               get_start_idx, read_count,
                                               direct_read_delay_ms);
                if (ret) {
                    if (ret == -EIO)
                        mismatches++;
                    printf("handle_direct_shmem_read, rc %d\n", ret);
                    goto quit;
                }
            }
        } else if (strcmp(*args, "-Q2") == 0) {
            args++;
            if (*args) {
                uint32_t compare_end;
                uint32_t total_get_count;
                uint32_t get_count = (uint32_t)atoi(*args);
                uint32_t i;
                uint64_t needed_elems;

                printf("handle_get_param(tag), CAPI_V2_PARAM_ID_LOAD_DUMMY, num = %u\n",
                       get_count);

                if (tag_id == 0) {
                    printf("Error : -T <tag_id> must be given before -Q2\n");
                    ret = -EINVAL;
                    goto quit;
                }

                if (owner_graph.enabled) {
                    if (!owner_graph.opened) {
                        ret = owner_graph_open(mixer, card, &owner_graph);
                        if (ret)
                            goto quit;
                    }

                    if (!target_gkv_explicit && current_target.num_gkvs == 0) {
                        ret = owner_graph_build_target_gkv(&owner_graph,
                                                           &current_target);
                        if (ret) {
                            printf("Error : failed to infer owner graph target GKV, ret %d\n",
                                   ret);
                            goto quit;
                        }
                        print_target_gkv(&current_target);
                    }
                }

                if (current_target.num_gkvs == 0) {
                    printf("Error : -gkv/--target-gkv must be given before -Q2, unless --open-graph graph options are provided\n");
                    ret = -EINVAL;
                    goto quit;
                }

                compare_end = get_start_idx + get_count;
                if (compare_end < get_start_idx) {
                    printf("Error : invalid get window, start %u count %u overflow\n",
                           get_start_idx, get_count);
                    ret = -EINVAL;
                    goto quit;
                }
                needed_elems = (uint64_t)get_start_idx + (uint64_t)get_count;
                if (needed_elems > UINT32_MAX) {
                    printf("Error : invalid get window, start %u count %u overflow\n",
                           get_start_idx, get_count);
                    ret = -EINVAL;
                    goto quit;
                }

                total_get_count = (write_count > 0) ? write_count :
                                  (uint32_t)needed_elems;
                if (total_get_count < (uint32_t)needed_elems)
                    total_get_count = (uint32_t)needed_elems;

                get_buf = calloc(total_get_count, sizeof(uint32_t));
                if (!get_buf) {
                    printf("Error : failed to allocate get_param buffer\n");
                    ret = -ENOMEM;
                    goto quit;
                }

                ret = handle_get_param_tag(mixer, tag_id, &current_target,
                                           get_buf, total_get_count);
                if (ret) {
                    printf("handle_get_param(tag), rc %d\n", ret);
                    goto quit;
                }

                for (i = get_start_idx; i < compare_end; i++) {
                    uint32_t expected = i;

                    if ((write_shadow != NULL) && (i < write_shadow_count))
                        expected = write_shadow[i];

                    printf("SPF:array[%u] = 0x%x, expected[%u] = 0x%x\n",
                           i, get_buf[i], i, expected);
                    if (get_buf[i] != expected) {
                        printf("ERROR : mismatch at index %u\n", i);
                        mismatches++;
                    }
                }

                free(get_buf);
                get_buf = NULL;

                if (mismatches == 0)
                    printf("PASS: %u values verified (index window [%u..%u])\n",
                           get_count, get_start_idx,
                           (compare_end == 0) ? get_start_idx :
                           (compare_end - 1));
                else
                    printf("FAIL: %d mismatches out of %u values (index window [%u..%u])\n",
                           mismatches, get_count, get_start_idx,
                           (compare_end == 0) ? get_start_idx :
                           (compare_end - 1));
            }
        } else if (strcmp(*args, "-qi") == 0) {
            args++;
            if (*args)
                get_start_idx = (uint32_t)strtoul(*args, NULL, 0);
        } else if (strcmp(*args, "-rs") == 0) {
            args++;
            if (*args)
                direct_read_delay_ms = (uint32_t)strtoul(*args, NULL, 0);
        } else if ((strcmp(*args, "-P") == 0) ||
                   (strcmp(*args, "-Q") == 0)) {
            printf("Error : %s was removed from this test. Use -P2 for shared-memory SET, -R2 for direct shared-memory read, and -Q2 for tagged GET.\n",
                   *args);
            ret = -EINVAL;
            goto quit;
        } else if (strcmp(*args, "-help") == 0 ||
                   strcmp(*args, "--help") == 0) {
            usage();
            goto quit;
        } else {
            printf("Error : unknown option %s\n", *args);
            usage();
            ret = -EINVAL;
            goto quit;
        }

        if (*args)
            args++;
    }

    if (interactive_mode) {
        ret = interactive_console(mixer, card,
                                  owner_graph.enabled ? &owner_graph : NULL,
                                  &info, &shmem_active, &shmem_mapped,
                                  &shmem_addr, &tag_id,
                                  &current_target, &write_shadow,
                                  &write_shadow_count, &write_count,
                                  &shmem_payload_size,
                                  direct_read_delay_ms);
    }

quit:
    if (get_buf)
        free(get_buf);
    if (write_shadow)
        free(write_shadow);

    if (owner_graph.started && owner_hold_ms && !owner_hold_done) {
        printf("holding owner graph for %u ms before cleanup\n", owner_hold_ms);
        usleep(owner_hold_ms * 1000);
    }

    owner_graph_close(mixer, &owner_graph);

    if (shmem_active) {
        if (shmem_addr) {
            if (shmem_mapped)
                munmap(shmem_addr, info.size);
            else
                free(shmem_addr);
        }

        if (free_flag) {
            int free_ret;

            printf("Freeing shared memory handle 0x%x\n", info.spf_mem_handle);
            free_ret = agm_mixer_shmem_free(mixer, info.spf_mem_handle);
            if (free_ret) {
                printf("agm_mixer_shmem_free failed, ret %d\n", free_ret);
                if (ret == 0)
                    ret = free_ret;
            } else {
                printf("agm_mixer_shmem_free succeeded for handle 0x%x\n",
                       info.spf_mem_handle);
            }
        } else {
            printf("Skipping shmem free due to -f 0\n");
        }

        if ((int)info.ion_fd > 0) {
            if (close((int)info.ion_fd) != 0)
                printf("close(%d) failed, errno %d\n", (int)info.ion_fd,
                       errno);
        }
    }

    if (mixer)
        mixer_close(mixer);

    if (mismatches)
        return -EIO;

    return ret;
}
