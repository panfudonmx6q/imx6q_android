/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/* Copyright (C) 2012 Freescale Semiconductor, Inc. */

#define LOG_TAG "audio_hw_primary"
#define LOG_NDEBUG 0

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#include <tinyalsa/asoundlib.h>
#include <audio_utils/resampler.h>
#include <audio_utils/echo_reference.h>
#include <hardware/audio_effect.h>
#include <audio_effects/effect_aec.h>

#include "audio_hardware.h"
#include "config_wm8962.h"
#include "config_wm8958.h"
#include "config_hdmi.h"
#include "config_usbaudio.h"
#include "config_nullcard.h"


/* ALSA ports for IMX */
#define PORT_MM     0
#define PORT_MM2_UL 0
#define PORT_SPDIF  6 /*not used*/
#define PORT_HDMI   0

/*align the definition in kernel for hdmi audio*/
#define HDMI_PERIOD_SIZE       768
#define PLAYBACK_HDMI_PERIOD_COUNT      8

/* number of frames per short period (low latency) */
#define SHORT_PERIOD_SIZE       256
/* number of short periods in a long period (low power) */
#define LONG_PERIOD_MULTIPLIER  2
/* number of frames per long period (low power) */
#define LONG_PERIOD_SIZE (SHORT_PERIOD_SIZE * LONG_PERIOD_MULTIPLIER)
/* number of periods for low power playback */
#define PLAYBACK_LONG_PERIOD_COUNT  4
/* number of pseudo periods for low latency playback */
#define PLAYBACK_SHORT_PERIOD_COUNT 4
/* number of periods for capture */
#define CAPTURE_PERIOD_COUNT 4
/* minimum sleep time in out_write() when write threshold is not reached */
#define MIN_WRITE_SLEEP_US 5000

#define RESAMPLER_BUFFER_FRAMES (LONG_PERIOD_SIZE * 2)
#define RESAMPLER_BUFFER_SIZE   (4 * RESAMPLER_BUFFER_FRAMES)

#define DEFAULT_OUT_SAMPLING_RATE 44100

/* sampling rate when using MM low power port */
#define MM_LOW_POWER_SAMPLING_RATE  44100
/* sampling rate when using MM full power port */
#define MM_FULL_POWER_SAMPLING_RATE 44100
#define MM_USB_AUDIO_IN_RATE   16000

/* product-specific defines */
#define PRODUCT_DEVICE_PROPERTY "ro.product.device"
#define PRODUCT_NAME_PROPERTY   "ro.product.name"
#define PRODUCT_DEVICE_IMX      "imx"
#define SUPPORT_CARD_NUM        5

/*"null_card" must be in the end of this array*/
struct audio_card *audio_card_list[SUPPORT_CARD_NUM] = {
    &wm8958_card,
    &wm8962_card,
    &hdmi_card,
    &usbaudio_card,
    &null_card,
};

struct pcm_config pcm_config_mm_out = {
    .channels = 2,
    .rate = MM_FULL_POWER_SAMPLING_RATE,
    .period_size = LONG_PERIOD_SIZE,
    .period_count = PLAYBACK_LONG_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_mm_in = {
    .channels = 2,
    .rate = MM_FULL_POWER_SAMPLING_RATE,
    .period_size = LONG_PERIOD_SIZE,
    .period_count = CAPTURE_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};


/**
 * NOTE: when multiple mutexes have to be acquired, always respect the following order:
 *        hw device > in stream > out stream
 */
static void select_output_device(struct imx_audio_device *adev);
static void select_input_device(struct imx_audio_device *adev);
static int adev_set_voice_volume(struct audio_hw_device *dev, float volume);
static int do_input_standby(struct imx_stream_in *in);
static int do_output_standby(struct imx_stream_out *out);
static int scan_available_device(struct imx_audio_device *adev);
static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer);
static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer);
static int adev_get_rate_for_device(struct imx_audio_device *adev, uint32_t devices, unsigned int flag);

/* Returns true on devices that are toro, false otherwise */
static int is_device_imx(void)
{
    char property[PROPERTY_VALUE_MAX];

    property_get(PRODUCT_DEVICE_PROPERTY, property, PRODUCT_DEVICE_IMX);

    /* return true if the property matches the given value */
    return strcmp(property, PRODUCT_DEVICE_IMX) == 0;
}

/* The enable flag when 0 makes the assumption that enums are disabled by
 * "Off" and integers/booleans by 0 */
static int set_route_by_array(struct mixer *mixer, struct route_setting *route,
                              int enable)
{
    struct mixer_ctl *ctl;
    unsigned int i, j;

    if(!mixer) return 0;
    if(!route) return 0;
    /* Go through the route array and set each value */
    i = 0;
    while (route[i].ctl_name) {
        ctl = mixer_get_ctl_by_name(mixer, route[i].ctl_name);
        if (!ctl)
            return -EINVAL;

        if (route[i].strval) {
            if (enable)
                mixer_ctl_set_enum_by_string(ctl, route[i].strval);
            else
                mixer_ctl_set_enum_by_string(ctl, "Off");
        } else {
            /* This ensures multiple (i.e. stereo) values are set jointly */
            for (j = 0; j < mixer_ctl_get_num_values(ctl); j++) {
                if (enable)
                    mixer_ctl_set_value(ctl, j, route[i].intval);
                else
                    mixer_ctl_set_value(ctl, j, 0);
            }
        }
        i++;
    }

    return 0;
}



static void force_all_standby(struct imx_audio_device *adev)
{
    struct imx_stream_in *in;
    struct imx_stream_out *out;
    int i;

    for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
        if (adev->active_output[i]) {
            out = adev->active_output[i];
            pthread_mutex_lock(&out->lock);
            do_output_standby(out);
            pthread_mutex_unlock(&out->lock);
        }

    if (adev->active_input) {
        in = adev->active_input;
        pthread_mutex_lock(&in->lock);
        do_input_standby(in);
        pthread_mutex_unlock(&in->lock);
    }
}

static void select_mode(struct imx_audio_device *adev)
{
    if (adev->mode == AUDIO_MODE_IN_CALL) {
        LOGE("Entering IN_CALL state, in_call=%d", adev->in_call);
        if (!adev->in_call) {
            force_all_standby(adev);
            /* force earpiece route for in call state if speaker is the
            only currently selected route. This prevents having to tear
            down the modem PCMs to change route from speaker to earpiece
            after the ringtone is played, but doesn't cause a route
            change if a headset or bt device is already connected. If
            speaker is not the only thing active, just remove it from
            the route. We'll assume it'll never be used initally during
            a call. This works because we're sure that the audio policy
            manager will update the output device after the audio mode
            change, even if the device selection did not change. */
            if ((adev->devices & AUDIO_DEVICE_OUT_ALL) == AUDIO_DEVICE_OUT_SPEAKER)
                adev->devices = AUDIO_DEVICE_OUT_EARPIECE |
                                AUDIO_DEVICE_IN_BUILTIN_MIC;
            else
                adev->devices &= ~AUDIO_DEVICE_OUT_SPEAKER;
            select_output_device(adev);

            adev_set_voice_volume(&adev->hw_device, adev->voice_volume);
            adev->in_call = 1;
        }
    } else {
        LOGE("Leaving IN_CALL state, in_call=%d, mode=%d",
             adev->in_call, adev->mode);
        if (adev->in_call) {
            adev->in_call = 0;
            force_all_standby(adev);
            select_output_device(adev);
            select_input_device(adev);
        }
    }
}

static void select_output_device(struct imx_audio_device *adev)
{
    int headset_on;
    int headphone_on;
    int speaker_on;
    int earpiece_on;
    int bt_on;
    bool tty_volume = false;
    unsigned int channel;
    int i;

    headset_on      = adev->devices & AUDIO_DEVICE_OUT_WIRED_HEADSET;
    headphone_on    = adev->devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
    speaker_on      = adev->devices & AUDIO_DEVICE_OUT_SPEAKER;
    earpiece_on     = adev->devices & AUDIO_DEVICE_OUT_EARPIECE;
    bt_on           = adev->devices & AUDIO_DEVICE_OUT_ALL_SCO;

    /* force rx path according to TTY mode when in call */
    if (adev->mode == AUDIO_MODE_IN_CALL && !bt_on) {
        switch(adev->tty_mode) {
            case TTY_MODE_FULL:
            case TTY_MODE_VCO:
                /* rx path to headphones */
                headphone_on = 1;
                headset_on = 0;
                speaker_on = 0;
                earpiece_on = 0;
                tty_volume = true;
                break;
            case TTY_MODE_HCO:
                /* rx path to device speaker */
                headphone_on = 0;
                headset_on = 0;
                speaker_on = 1;
                earpiece_on = 0;
                break;
            case TTY_MODE_OFF:
            default:
                /* force speaker on when in call and HDMI is selected as voice DL audio
                 * cannot be routed to HDMI by ABE */
                if (adev->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL)
                    speaker_on = 1;
                break;
        }
    }
    /*if mode = AUDIO_MODE_IN_CALL*/
    LOGW("headphone %d ,headset %d ,speaker %d, earpiece %d, \n", headphone_on, headset_on, speaker_on, earpiece_on);
    /* select output stage */
    for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
        set_route_by_array(adev->mixer[i], adev->card_list[i]->bt_output, bt_on);
    for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
        set_route_by_array(adev->mixer[i], adev->card_list[i]->hs_output, headset_on | headphone_on);
    for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
        set_route_by_array(adev->mixer[i], adev->card_list[i]->speaker_output, speaker_on);
    for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
        set_route_by_array(adev->mixer[i], adev->card_list[i]->earpiece_output, earpiece_on);

    /* Special case: select input path if in a call, otherwise
       in_set_parameters is used to update the input route
       todo: use sub mic for handsfree case */
    if (adev->mode == AUDIO_MODE_IN_CALL) {
        if (bt_on)
            for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
                set_route_by_array(adev->mixer[i], adev->card_list[i]->vx_bt_mic_input, bt_on);
        else {
            /* force tx path according to TTY mode when in call */
            switch(adev->tty_mode) {
                case TTY_MODE_FULL:
                case TTY_MODE_HCO:
                    /* tx path from headset mic */
                    headphone_on = 0;
                    headset_on = 1;
                    speaker_on = 0;
                    earpiece_on = 0;
                    break;
                case TTY_MODE_VCO:
                    /* tx path from device sub mic */
                    headphone_on = 0;
                    headset_on = 0;
                    speaker_on = 1;
                    earpiece_on = 0;
                    break;
                case TTY_MODE_OFF:
                default:
                    break;
            }

            if (headset_on)
                for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
                    set_route_by_array(adev->mixer[i], adev->card_list[i]->vx_hs_mic_input, 1);
            else if (headphone_on || earpiece_on || speaker_on)
                for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
                    set_route_by_array(adev->mixer[i], adev->card_list[i]->vx_main_mic_input, 1);
            else
                for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
                    set_route_by_array(adev->mixer[i], adev->card_list[i]->vx_main_mic_input, 0);
        }
    }
}

static void select_input_device(struct imx_audio_device *adev)
{
    int i;
    int headset_on = 0;
    int main_mic_on = 0;
    int sub_mic_on = 0;
    int bt_on = adev->devices & AUDIO_DEVICE_IN_ALL_SCO;

    if (!bt_on) {
        if ((adev->mode != AUDIO_MODE_IN_CALL) && (adev->active_input != 0)) {
            /* sub mic is used for camcorder or VoIP on speaker phone */
            sub_mic_on = (adev->active_input->source == AUDIO_SOURCE_CAMCORDER) ||
                         ((adev->devices & AUDIO_DEVICE_OUT_SPEAKER) &&
                          (adev->active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION));
        }

        headset_on = adev->devices & AUDIO_DEVICE_IN_WIRED_HEADSET;
        main_mic_on = adev->devices & AUDIO_DEVICE_IN_BUILTIN_MIC;
    }

   /* TODO: check how capture is possible during voice calls or if
    * both use cases are mutually exclusive.
    */
    if (bt_on)
        for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
            set_route_by_array(adev->mixer[i], adev->card_list[i]->mm_bt_mic_input, 1);
    else {
        /* Select front end */
        if (headset_on)
            for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
                set_route_by_array(adev->mixer[i], adev->card_list[i]->mm_hs_mic_input, 1);
        else if (main_mic_on || sub_mic_on)
            for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
                set_route_by_array(adev->mixer[i], adev->card_list[i]->mm_main_mic_input, 1);
        else
            for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
                set_route_by_array(adev->mixer[i], adev->card_list[i]->mm_main_mic_input, 0);
    }
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct imx_stream_out *out)
{
    struct imx_audio_device *adev = out->dev;
    unsigned int card = -1;
    unsigned int port = 0;
    int i;

    LOGW("start_output_stream...");
    adev->active_output[out->out_id] = out;

    if (adev->mode != AUDIO_MODE_IN_CALL) {
        /* FIXME: only works if only one output can be active at a time */
        select_output_device(adev);
    }
    /* S/PDIF takes priority over HDMI audio. In the case of multiple
     * devices, this will cause use of S/PDIF or HDMI only */
    for(i = 0; i < MAX_AUDIO_CARD_NUM; i++) {
        if(out->device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
            if(adev->card_list[i]->supported_devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
                   card = adev->card_list[i]->card;
                   port = 0;
                   break;
            }
        } else if((out->device & AUDIO_DEVICE_OUT_ALL) & adev->card_list[i]->supported_devices) {
            card = adev->card_list[i]->card;
            port = 0;
            break;
        }
        if(i == MAX_AUDIO_CARD_NUM-1) {
            LOGE("can not find supported device for %d",out->device);
            return -EINVAL;
        }
    }
    LOGW("card %d, port %d device %x", card, port, out->device);

    out->write_flags            = PCM_OUT | PCM_MMAP;
    out->config.period_size     = LONG_PERIOD_SIZE;
    out->config.period_count    = PLAYBACK_LONG_PERIOD_COUNT;
    out->write_threshold        = PLAYBACK_LONG_PERIOD_COUNT * LONG_PERIOD_SIZE;
    out->config.start_threshold = PLAYBACK_LONG_PERIOD_COUNT * LONG_PERIOD_SIZE;
    out->config.avail_min       = LONG_PERIOD_SIZE;

    if(out->device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        out->write_flags            = PCM_OUT;
        out->config.period_size     = HDMI_PERIOD_SIZE;
        out->config.period_count    = PLAYBACK_HDMI_PERIOD_COUNT;
        out->write_threshold        = HDMI_PERIOD_SIZE * PLAYBACK_HDMI_PERIOD_COUNT;
        out->config.start_threshold = HDMI_PERIOD_SIZE;
        out->config.avail_min       = HDMI_PERIOD_SIZE;
    }
    /* default to low power: will be corrected in out_write if necessary before first write to
     * tinyalsa.
     */

    out->low_power   = 0;
    out->frame_count = 0;

    out->pcm = pcm_open(card, port, out->write_flags, &out->config);

    if (!pcm_is_ready(out->pcm)) {
        LOGE("cannot open pcm_out driver: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        adev->active_output[out->out_id] = NULL;
        return -ENOMEM;
    }

    if (adev->echo_reference != NULL)
        out->echo_reference = adev->echo_reference;
    if (out->resampler)
        out->resampler->reset(out->resampler);

    return 0;
}

static int check_input_parameters(uint32_t sample_rate, int format, int channel_count)
{
    if (format != AUDIO_FORMAT_PCM_16_BIT)
        return -EINVAL;

    if ((channel_count < 1) || (channel_count > 2))
        return -EINVAL;

    switch(sample_rate) {
    case 8000:
    case 11025:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static size_t get_input_buffer_size(uint32_t sample_rate, int format, int channel_count)
{
    size_t size;
    size_t device_rate;

    if (check_input_parameters(sample_rate, format, channel_count) != 0)
        return 0;

    /* take resampling into account and return the closest majoring
    multiple of 16 frames, as audioflinger expects audio buffers to
    be a multiple of 16 frames */
    size = (pcm_config_mm_in.period_size * sample_rate) / pcm_config_mm_in.rate;
    size = ((size + 15) / 16) * 16;

    LOGW("get_input_buffer_size size = %d, channel_count = %d",size,channel_count);
    return size * channel_count * sizeof(short);
}

static void add_echo_reference(struct imx_stream_out *out,
                               struct echo_reference_itfe *reference)
{
    pthread_mutex_lock(&out->lock);
    out->echo_reference = reference;
    pthread_mutex_unlock(&out->lock);
}

static void remove_echo_reference(struct imx_stream_out *out,
                                  struct echo_reference_itfe *reference)
{
    pthread_mutex_lock(&out->lock);
    if (out->echo_reference == reference) {
        /* stop writing to echo reference */
        reference->write(reference, NULL);
        out->echo_reference = NULL;
    }
    pthread_mutex_unlock(&out->lock);
}

static void put_echo_reference(struct imx_audio_device *adev,
                          struct echo_reference_itfe *reference)
{
    int i;
    if (adev->echo_reference != NULL &&
            reference == adev->echo_reference) {
        for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
            if (adev->active_output[i] != NULL)
                remove_echo_reference(adev->active_output[i], reference);
        release_echo_reference(reference);
        adev->echo_reference = NULL;
    }
}

static struct echo_reference_itfe *get_echo_reference(struct imx_audio_device *adev,
                                               audio_format_t format,
                                               uint32_t channel_count,
                                               uint32_t sampling_rate)
{
    put_echo_reference(adev, adev->echo_reference);
    /*only for mixer output, only one output*/
    if(adev->out_stream_num == 1)
        if (adev->active_output[0] != NULL &&
            adev->active_output[0]->config.format == AUDIO_FORMAT_PCM_16_BIT ) {
            struct audio_stream *stream = &adev->active_output[0]->stream.common;
            uint32_t wr_channel_count = popcount(stream->get_channels(stream));
            uint32_t wr_sampling_rate = stream->get_sample_rate(stream);

            int status = create_echo_reference(AUDIO_FORMAT_PCM_16_BIT,
                                           channel_count,
                                           sampling_rate,
                                           AUDIO_FORMAT_PCM_16_BIT,
                                           wr_channel_count,
                                           wr_sampling_rate,
                                           &adev->echo_reference);
            if (status == 0)
                add_echo_reference(adev->active_output[0], adev->echo_reference);
        }

    return adev->echo_reference;
}

static int get_playback_delay(struct imx_stream_out *out,
                       size_t frames,
                       struct echo_reference_buffer *buffer)
{
    size_t kernel_frames;
    int status;

    status = pcm_get_htimestamp(out->pcm, &kernel_frames, &buffer->time_stamp);
    if (status < 0) {
        buffer->time_stamp.tv_sec  = 0;
        buffer->time_stamp.tv_nsec = 0;
        buffer->delay_ns           = 0;
        LOGV("get_playback_delay(): pcm_get_htimestamp error,"
                "setting playbackTimestamp to 0");
        return status;
    }

    kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;

    /* adjust render time stamp with delay added by current driver buffer.
     * Add the duration of current frame as we want the render time of the last
     * sample being written. */
    buffer->delay_ns = (long)(((int64_t)(kernel_frames + frames)* 1000000000)/
                            MM_FULL_POWER_SAMPLING_RATE);

    return 0;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    struct imx_stream_out *out = (struct imx_stream_out *)stream;
    return out->config.rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    LOGE("out_set_sample_rate %d", rate);
    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct imx_stream_out *out = (struct imx_stream_out *)stream;

    /* take resampling into account and return the closest majoring
    multiple of 16 frames, as audioflinger expects audio buffers to
    be a multiple of 16 frames */
    size_t size = (out->config.period_size * DEFAULT_OUT_SAMPLING_RATE) / out->config.rate;
    size = ((size + 15) / 16) * 16;
    return size * audio_stream_frame_size((struct audio_stream *)stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    struct imx_stream_out *out = (struct imx_stream_out *)stream;
    if(out->config.channels == 1)
        return AUDIO_CHANNEL_OUT_MONO;
    else
        return AUDIO_CHANNEL_OUT_STEREO;
}

static int out_get_format(const struct audio_stream *stream)
{
    struct imx_stream_out *out = (struct imx_stream_out *)stream;
    switch(out->config.format) {
    case PCM_FORMAT_S16_LE:
         return AUDIO_FORMAT_PCM_16_BIT;
    case PCM_FORMAT_S32_LE:
         return AUDIO_FORMAT_PCM_32_BIT;
    default:
         return AUDIO_FORMAT_PCM_16_BIT;
    }
}

static int out_set_format(struct audio_stream *stream, int format)
{
    LOGE("out_set_format %d", format);
    return 0;
}

/* must be called with hw device and output stream mutexes locked */
static int do_output_standby(struct imx_stream_out *out)
{
    struct imx_audio_device *adev = out->dev;

    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;
        out->frame_count = 0;

        LOGW("do_out_standby...");
        adev->active_output[out->out_id] = 0;

        /* if in call, don't turn off the output stage. This will
        be done when the call is ended */
        if (adev->mode != AUDIO_MODE_IN_CALL) {
            /* FIXME: only works if only one output can be active at a time */
        }

        /* stop writing to echo reference */
        if (out->echo_reference != NULL) {
            out->echo_reference->write(out->echo_reference, NULL);
            out->echo_reference = NULL;
        }

        out->standby = 1;
    }
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    struct imx_stream_out *out = (struct imx_stream_out *)stream;
    int status;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    status = do_output_standby(out);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);
    return status;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct imx_stream_out *out = (struct imx_stream_out *)stream;
    struct imx_audio_device *adev = out->dev;
    struct imx_stream_in *in;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret, val = 0;
    bool force_input_standby = false;
    bool out_is_active = false;
    int  i;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        pthread_mutex_lock(&adev->lock);
        pthread_mutex_lock(&out->lock);
        if (((adev->devices & AUDIO_DEVICE_OUT_ALL) != val) && (val != 0)) {
            for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
                if(out == adev->active_output[i]) out_is_active = true;
            if (out_is_active) {
                /* a change in output device may change the microphone selection */
                if (adev->active_input &&
                        adev->active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION) {
                    force_input_standby = true;
                }
                /* force standby if moving to/from HDMI */
                if (((val & AUDIO_DEVICE_OUT_AUX_DIGITAL) ^
                        (adev->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL)) ||
                        ((val & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) ^
                        (adev->devices & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET)))
                    do_output_standby(out);
            }
            adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
            adev->devices |= val;
            out->device    = val;
            select_output_device(adev);
        }
        pthread_mutex_unlock(&out->lock);
        if (force_input_standby) {
            in = adev->active_input;
            pthread_mutex_lock(&in->lock);
            do_input_standby(in);
            pthread_mutex_unlock(&in->lock);
        }
        pthread_mutex_unlock(&adev->lock);
    }
    LOGW("out_set_parameters %s, ret %d",kvpairs, ret);
    str_parms_destroy(parms);
    return 0;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    struct imx_stream_out *out = (struct imx_stream_out *)stream;
    struct imx_audio_device *adev = out->dev;
    struct str_parms *parms;
    int ret, val = 0;

    parms = str_parms_create_str(keys);

    val = adev->devices & AUDIO_DEVICE_OUT_ALL;

    ret = str_parms_add_int(parms, AUDIO_PARAMETER_STREAM_ROUTING, val);

    LOGW("out get parameters %s",str_parms_to_str(parms));
    return str_parms_to_str(parms);
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct imx_stream_out *out = (struct imx_stream_out *)stream;

    return (out->config.period_size * out->config.period_count * 1000) / out->config.rate;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret;
    struct imx_stream_out *out = (struct imx_stream_out *)stream;
    struct imx_audio_device *adev = out->dev;
    size_t frame_size = audio_stream_frame_size(&out->stream.common);
    size_t in_frames = bytes / frame_size;
    size_t out_frames = RESAMPLER_BUFFER_SIZE / frame_size;
    bool force_input_standby = false;
    struct imx_stream_in *in;
    int kernel_frames = 0;
    void *buf;

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the output stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            pthread_mutex_unlock(&adev->lock);
            goto exit;
        }
        out->standby = 0;
        /* a change in output device may change the microphone selection */
        if (adev->active_input &&
                adev->active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION)
            force_input_standby = true;
    }
    pthread_mutex_unlock(&adev->lock);

    /* only use resampler if required */
    if (out->config.rate != DEFAULT_OUT_SAMPLING_RATE) {
        out->resampler->resample_from_input(out->resampler,
                                            (int16_t *)buffer,
                                            &in_frames,
                                            (int16_t *)out->buffer,
                                            &out_frames);
        buf = out->buffer;
    } else {
        out_frames = in_frames;
        buf = (void *)buffer;
    }
    if (out->echo_reference != NULL) {
        struct echo_reference_buffer b;
        b.raw = (void *)buffer;
        b.frame_count = in_frames;

        get_playback_delay(out, out_frames, &b);
        out->echo_reference->write(out->echo_reference, &b);
    }

    /* do not allow more than out->write_threshold frames in kernel pcm driver buffer */
    do {
        struct timespec time_stamp;

        if (pcm_get_htimestamp(out->pcm, (unsigned int *)&kernel_frames, &time_stamp) < 0)
            break;
        /* kernel_frames the free buffer size; */
        kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;
        /* kernel_frames the data size in the buffer; */
        if (kernel_frames > out->write_threshold) {
            unsigned long time = (unsigned long)
                    (((int64_t)(kernel_frames - out->write_threshold) * 1000000) /
                            MM_FULL_POWER_SAMPLING_RATE);
            if (time < MIN_WRITE_SLEEP_US)
                time = MIN_WRITE_SLEEP_US;
            usleep(time);
        }
    } while (kernel_frames > out->write_threshold);

    out->frame_count += in_frames;

    if(out->write_flags & PCM_MMAP)
        ret = pcm_mmap_write(out->pcm, (void *)buf, out_frames * frame_size);
    else
        ret = pcm_write(out->pcm, (void *)buf, out_frames * frame_size);

    if(ret !=0) {
        LOGW("ret %d, pcm write %d error %s.", ret, bytes, pcm_get_error(out->pcm));

        switch(pcm_state(out->pcm)) {
            case PCM_STATE_SETUP:
            case PCM_STATE_XRUN:
                 ret = pcm_prepare(out->pcm);
                 if(ret != 0) goto exit;
                 break;
            case PCM_STATE_DISCONNECTED:
                 do_output_standby(out);
                 goto exit;
                 break;
            default:
                 goto exit;
        }

        if(out->write_flags & PCM_MMAP)
            ret = pcm_mmap_write(out->pcm, (void *)buf, out_frames * frame_size);
        else
            ret = pcm_write(out->pcm, (void *)buf, out_frames * frame_size);
     }

exit:
    pthread_mutex_unlock(&out->lock);

    if (ret != 0) {
        LOGW("write error, sleep few ms");
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               out_get_sample_rate(&stream->common));
    }

    if (force_input_standby) {
        pthread_mutex_lock(&adev->lock);
        if (adev->active_input) {
            in = adev->active_input;
            pthread_mutex_lock(&in->lock);
            do_input_standby(in);
            pthread_mutex_unlock(&in->lock);
        }
        pthread_mutex_unlock(&adev->lock);
    }
    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    struct imx_stream_out *out = (struct imx_stream_out *)stream;
    *dsp_frames = out->frame_count;
    LOGW("get render position....");
    return 0;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

/** audio_stream_in implementation **/

/* must be called with hw device and input stream mutexes locked */
static int start_input_stream(struct imx_stream_in *in)
{
    int ret = 0;
    int i;
    struct imx_audio_device *adev = in->dev;
    unsigned int card = -1;
    unsigned int port = 0;
    LOGW("start_input_stream....");
    adev->active_input = in;

    if (adev->mode != AUDIO_MODE_IN_CALL) {
        adev->devices &= ~AUDIO_DEVICE_IN_ALL;
        adev->devices |= in->device;
        select_input_device(adev);
    }

    for(i = 0; i < MAX_AUDIO_CARD_NUM; i++) {
        if((adev->devices & AUDIO_DEVICE_IN_ALL) & adev->card_list[i]->supported_devices) {
            card = adev->card_list[i]->card;
            port = 0;
            break;
        }
        if(i == MAX_AUDIO_CARD_NUM-1) {
            LOGE("can not find supported device for %d",in->device);
            return -EINVAL;
        }
    }
    LOGW("card %d, port %d device %x", card, port, in->device);

    in->config.stop_threshold = in->config.period_size * in->config.period_count;

    if(in->device & AUDIO_DEVICE_IN_ANLG_DOCK_MIC) {
        if((int)in->config.rate != adev_get_rate_for_device(adev, AUDIO_DEVICE_IN_ANLG_DOCK_MIC, PCM_IN) ||
           in->config.channels != 1) {
           LOGE("Input 2 does not support this format!");
           return -EINVAL;
        }
    }else {
        if(in->config.rate != MM_FULL_POWER_SAMPLING_RATE ||
           in->config.channels != 2) {
           LOGE("Input 1 does not support this format!");
           return -EINVAL;
        }
    }

    if (in->need_echo_reference && in->echo_reference == NULL)
        in->echo_reference = get_echo_reference(adev,
                                        AUDIO_FORMAT_PCM_16_BIT,
                                        in->config.channels,
                                        in->requested_rate);

    /* this assumes routing is done previously */
    in->pcm = pcm_open(card, port, PCM_IN, &in->config);
    if (!pcm_is_ready(in->pcm)) {
        LOGE("cannot open pcm_in driver: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        adev->active_input = NULL;
        return -ENOMEM;
    }

    /* if no supported sample rate is available, use the resampler */
    if (in->resampler) {
        in->resampler->reset(in->resampler);
        in->frames_in = 0;
    }
    return 0;
}

static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct imx_stream_in *in = (struct imx_stream_in *)stream;

    return in->requested_rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct imx_stream_in *in = (struct imx_stream_in *)stream;

    return get_input_buffer_size(in->requested_rate,
                                 AUDIO_FORMAT_PCM_16_BIT,
                                 in->config.channels);
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    struct imx_stream_in *in = (struct imx_stream_in *)stream;

    if (in->config.channels == 1) {
        return AUDIO_CHANNEL_IN_MONO;
    } else {
        return AUDIO_CHANNEL_IN_STEREO;
    }
}

static int in_get_format(const struct audio_stream *stream)
{
    struct imx_stream_in *in = (struct imx_stream_in *)stream;
    switch(in->config.format) {
    case PCM_FORMAT_S16_LE:
         return AUDIO_FORMAT_PCM_16_BIT;
    case PCM_FORMAT_S32_LE:
         return AUDIO_FORMAT_PCM_32_BIT;
    default:
         return AUDIO_FORMAT_PCM_16_BIT;
    }

}

static int in_set_format(struct audio_stream *stream, int format)
{
    return 0;
}

/* must be called with hw device and input stream mutexes locked */
static int do_input_standby(struct imx_stream_in *in)
{
    struct imx_audio_device *adev = in->dev;

    if (!in->standby) {
        LOGW("do_in_standby..");
        pcm_close(in->pcm);
        in->pcm = NULL;
        in->last_time_of_xrun = 0;

        adev->active_input = 0;
        if (adev->mode != AUDIO_MODE_IN_CALL) {
            adev->devices &= ~AUDIO_DEVICE_IN_ALL;
            select_input_device(adev);
        }

        if (in->echo_reference != NULL) {
            /* stop reading from echo reference */
            in->echo_reference->read(in->echo_reference, NULL);
            put_echo_reference(adev, in->echo_reference);
            in->echo_reference = NULL;
        }

        in->standby = 1;
    }
    return 0;
}

static int in_standby(struct audio_stream *stream)
{
    struct imx_stream_in *in = (struct imx_stream_in *)stream;
    int status;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    status = do_input_standby(in);
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct imx_stream_in *in = (struct imx_stream_in *)stream;
    struct imx_audio_device *adev = in->dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret, val = 0;
    bool do_standby = false;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, value, sizeof(value));

    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    if (ret >= 0) {
        val = atoi(value);
        /* no audio source uses val == 0 */
        if ((in->source != val) && (val != 0)) {
            in->source = val;
            do_standby = true;
        }
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        if ((in->device != val) && (val != 0)) {
            in->device = val;
            do_standby = true;
        }
    }

    if (do_standby)
        do_input_standby(in);

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&adev->lock);

    str_parms_destroy(parms);
    return ret;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static void get_capture_delay(struct imx_stream_in *in,
                       size_t frames,
                       struct echo_reference_buffer *buffer)
{

    /* read frames available in kernel driver buffer */
    size_t kernel_frames;
    struct timespec tstamp;
    long buf_delay;
    long rsmp_delay;
    long kernel_delay;
    long delay_ns;

    if (pcm_get_htimestamp(in->pcm, &kernel_frames, &tstamp) < 0) {
        buffer->time_stamp.tv_sec  = 0;
        buffer->time_stamp.tv_nsec = 0;
        buffer->delay_ns           = 0;
        LOGW("read get_capture_delay(): pcm_htimestamp error");
        return;
    }

    /* read frames available in audio HAL input buffer
     * add number of frames being read as we want the capture time of first sample
     * in current buffer */
    buf_delay = (long)(((int64_t)(in->frames_in + in->proc_frames_in) * 1000000000)
                                    / in->config.rate);
    /* add delay introduced by resampler */
    rsmp_delay = 0;
    if (in->resampler) {
        rsmp_delay = in->resampler->delay_ns(in->resampler);
    }

    kernel_delay = (long)(((int64_t)kernel_frames * 1000000000) / in->config.rate);

    delay_ns = kernel_delay + buf_delay + rsmp_delay;

    buffer->time_stamp = tstamp;
    buffer->delay_ns   = delay_ns;
    LOGV("get_capture_delay time_stamp = [%ld].[%ld], delay_ns: [%d],"
         " kernel_delay:[%ld], buf_delay:[%ld], rsmp_delay:[%ld], kernel_frames:[%d], "
         "in->frames_in:[%d], in->proc_frames_in:[%d], frames:[%d]",
         buffer->time_stamp.tv_sec , buffer->time_stamp.tv_nsec, buffer->delay_ns,
         kernel_delay, buf_delay, rsmp_delay, kernel_frames,
         in->frames_in, in->proc_frames_in, frames);

}

static int32_t update_echo_reference(struct imx_stream_in *in, size_t frames)
{
    struct echo_reference_buffer b;
    b.delay_ns = 0;

    LOGV("update_echo_reference, frames = [%d], in->ref_frames_in = [%d],  "
          "b.frame_count = [%d]",
         frames, in->ref_frames_in, frames - in->ref_frames_in);
    if (in->ref_frames_in < frames) {
        if (in->ref_buf_size < frames) {
            in->ref_buf_size = frames;
            in->ref_buf = (int16_t *)realloc(in->ref_buf,
                                             in->ref_buf_size *
                                                 in->config.channels * sizeof(int16_t));
        }

        b.frame_count = frames - in->ref_frames_in;
        b.raw = (void *)(in->ref_buf + in->ref_frames_in * in->config.channels);

        get_capture_delay(in, frames, &b);

        if (in->echo_reference->read(in->echo_reference, &b) == 0)
        {
            in->ref_frames_in += b.frame_count;
            LOGV("update_echo_reference: in->ref_frames_in:[%d], "
                    "in->ref_buf_size:[%d], frames:[%d], b.frame_count:[%d]",
                 in->ref_frames_in, in->ref_buf_size, frames, b.frame_count);
        }
    } else
        LOGW("update_echo_reference: NOT enough frames to read ref buffer");
    return b.delay_ns;
}

static int set_preprocessor_param(effect_handle_t handle,
                           effect_param_t *param)
{
    uint32_t size = sizeof(int);
    uint32_t psize = ((param->psize - 1) / sizeof(int) + 1) * sizeof(int) +
                        param->vsize;

    int status = (*handle)->command(handle,
                                   EFFECT_CMD_SET_PARAM,
                                   sizeof (effect_param_t) + psize,
                                   param,
                                   &size,
                                   &param->status);
    if (status == 0)
        status = param->status;

    return status;
}

static int set_preprocessor_echo_delay(effect_handle_t handle,
                                     int32_t delay_us)
{
    uint32_t buf[sizeof(effect_param_t) / sizeof(uint32_t) + 2];
    effect_param_t *param = (effect_param_t *)buf;

    param->psize = sizeof(uint32_t);
    param->vsize = sizeof(uint32_t);
    *(uint32_t *)param->data = AEC_PARAM_ECHO_DELAY;
    *((int32_t *)param->data + 1) = delay_us;

    return set_preprocessor_param(handle, param);
}

static void push_echo_reference(struct imx_stream_in *in, size_t frames)
{
    /* read frames from echo reference buffer and update echo delay
     * in->ref_frames_in is updated with frames available in in->ref_buf */
    int32_t delay_us = update_echo_reference(in, frames)/1000;
    int i;
    audio_buffer_t buf;

    if (in->ref_frames_in < frames)
        frames = in->ref_frames_in;

    buf.frameCount = frames;
    buf.raw = in->ref_buf;

    for (i = 0; i < in->num_preprocessors; i++) {
        if ((*in->preprocessors[i])->process_reverse == NULL)
            continue;

        (*in->preprocessors[i])->process_reverse(in->preprocessors[i],
                                               &buf,
                                               NULL);
        set_preprocessor_echo_delay(in->preprocessors[i], delay_us);
    }

    in->ref_frames_in -= buf.frameCount;
    if (in->ref_frames_in) {
        memcpy(in->ref_buf,
               in->ref_buf + buf.frameCount * in->config.channels,
               in->ref_frames_in * in->config.channels * sizeof(int16_t));
    }
}

static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer)
{
    struct imx_stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return -EINVAL;

    in = (struct imx_stream_in *)((char *)buffer_provider -
                                   offsetof(struct imx_stream_in, buf_provider));

    if (in->pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->frames_in == 0) {
        in->read_status = pcm_read(in->pcm,
                                   (void*)in->buffer,
                                   in->config.period_size *
                                       audio_stream_frame_size(&in->stream.common));
        if (in->read_status != 0) {
            LOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }
        in->frames_in = in->config.period_size;
    }

    buffer->frame_count = (buffer->frame_count > in->frames_in) ?
                                in->frames_in : buffer->frame_count;
    buffer->i16 = in->buffer + (in->config.period_size - in->frames_in) *
                                                in->config.channels;

    return in->read_status;

}

static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer)
{
    struct imx_stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (struct imx_stream_in *)((char *)buffer_provider -
                                   offsetof(struct imx_stream_in, buf_provider));

    in->frames_in -= buffer->frame_count;
}

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
static ssize_t read_frames(struct imx_stream_in *in, void *buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;
        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                    (int16_t *)((char *)buffer +
                            frames_wr * audio_stream_frame_size(&in->stream.common)),
                    &frames_rd);
        } else {
            struct resampler_buffer buf = {
                    { raw : NULL, },
                    frame_count : frames_rd,
            };
            get_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer +
                           frames_wr * audio_stream_frame_size(&in->stream.common),
                        buf.raw,
                        buf.frame_count * audio_stream_frame_size(&in->stream.common));
                frames_rd = buf.frame_count;
            }
            release_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }
    return frames_wr;
}

/* process_frames() reads frames from kernel driver (via read_frames()),
 * calls the active audio pre processings and output the number of frames requested
 * to the buffer specified */
static ssize_t process_frames(struct imx_stream_in *in, void* buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;
    audio_buffer_t in_buf;
    audio_buffer_t out_buf;
    int i;

    while (frames_wr < frames) {
        /* first reload enough frames at the end of process input buffer */
        if (in->proc_frames_in < (size_t)frames) {
            ssize_t frames_rd;

            if (in->proc_buf_size < (size_t)frames) {
                in->proc_buf_size = (size_t)frames;
                in->proc_buf = (int16_t *)realloc(in->proc_buf,
                                         in->proc_buf_size *
                                             in->config.channels * sizeof(int16_t));
                LOGV("process_frames(): in->proc_buf %p size extended to %d frames",
                     in->proc_buf, in->proc_buf_size);
            }
            frames_rd = read_frames(in,
                                    in->proc_buf +
                                        in->proc_frames_in * in->config.channels,
                                    frames - in->proc_frames_in);
            if (frames_rd < 0) {
                frames_wr = frames_rd;
                break;
            }
            in->proc_frames_in += frames_rd;
        }

        if (in->echo_reference != NULL)
            push_echo_reference(in, in->proc_frames_in);

         /* in_buf.frameCount and out_buf.frameCount indicate respectively
          * the maximum number of frames to be consumed and produced by process() */
        in_buf.frameCount = in->proc_frames_in;
        in_buf.s16 = in->proc_buf;
        out_buf.frameCount = frames - frames_wr;
        out_buf.s16 = (int16_t *)buffer + frames_wr * in->config.channels;

        for (i = 0; i < in->num_preprocessors; i++)
            (*in->preprocessors[i])->process(in->preprocessors[i],
                                               &in_buf,
                                               &out_buf);

        /* process() has updated the number of frames consumed and produced in
         * in_buf.frameCount and out_buf.frameCount respectively
         * move remaining frames to the beginning of in->proc_buf */
        in->proc_frames_in -= in_buf.frameCount;
        if (in->proc_frames_in) {
            memcpy(in->proc_buf,
                   in->proc_buf + in_buf.frameCount * in->config.channels,
                   in->proc_frames_in * in->config.channels * sizeof(int16_t));
        }

        /* if not enough frames were passed to process(), read more and retry. */
        if (out_buf.frameCount == 0)
            continue;

        frames_wr += out_buf.frameCount;
    }
    return frames_wr;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    int ret = 0;
    struct imx_stream_in *in = (struct imx_stream_in *)stream;
    struct imx_audio_device *adev = in->dev;
    size_t frames_rq = bytes / audio_stream_frame_size(&stream->common);

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the input stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->standby) {
        ret = start_input_stream(in);
        if (ret == 0) {
            in->standby = 0;
            in->mute_500ms = in->requested_rate * audio_stream_frame_size(&stream->common)/2;
        }
    }
    pthread_mutex_unlock(&adev->lock);

    if (ret < 0)
        goto exit;

    if (in->num_preprocessors != 0)
        ret = process_frames(in, buffer, frames_rq);
    else if (in->resampler != NULL)
        ret = read_frames(in, buffer, frames_rq);
    else
        ret = pcm_read(in->pcm, buffer, bytes);

    if(ret < 0) LOGW("ret %d, pcm read error %s.", ret, pcm_get_error(in->pcm));

    if (ret > 0)
        ret = 0;

    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

    if (in->mute_500ms > 0) {
        if(bytes <= in->mute_500ms) {
                memset(buffer, 0, bytes);
                in->mute_500ms = in->mute_500ms - bytes;
        } else {
                memset(buffer, 0, in->mute_500ms);
                in->mute_500ms = 0;
        }
    }

exit:
    if (ret < 0)
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               in_get_sample_rate(&stream->common));

    pthread_mutex_unlock(&in->lock);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    int times, diff;
    struct imx_stream_in *in = (struct imx_stream_in *)stream;
    times = pcm_get_time_of_xrun(in->pcm);
    diff = times - in->last_time_of_xrun;
    LOGW_IF((diff != 0), "in_get_input_frames_lost %d ms total %d ms\n",diff, times);
    in->last_time_of_xrun = times;
    return diff * in->requested_rate / 1000;
}

static int in_add_audio_effect(const struct audio_stream *stream,
                               effect_handle_t effect)
{
    struct imx_stream_in *in = (struct imx_stream_in *)stream;
    int status;
    effect_descriptor_t desc;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->num_preprocessors >= MAX_PREPROCESSORS) {
        status = -ENOSYS;
        goto exit;
    }

    status = (*effect)->get_descriptor(effect, &desc);
    if (status != 0)
        goto exit;

    in->preprocessors[in->num_preprocessors++] = effect;

    if (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0) {
        in->need_echo_reference = true;
        do_input_standby(in);
    }

exit:

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}

static int in_remove_audio_effect(const struct audio_stream *stream,
                                  effect_handle_t effect)
{
    struct imx_stream_in *in = (struct imx_stream_in *)stream;
    int i;
    int status = -EINVAL;
    bool found = false;
    effect_descriptor_t desc;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->num_preprocessors <= 0) {
        status = -ENOSYS;
        goto exit;
    }

    for (i = 0; i < in->num_preprocessors; i++) {
        if (found) {
            in->preprocessors[i - 1] = in->preprocessors[i];
            continue;
        }
        if (in->preprocessors[i] == effect) {
            in->preprocessors[i] = NULL;
            status = 0;
            found = true;
        }
    }

    if (status != 0)
        goto exit;

    in->num_preprocessors--;

    status = (*effect)->get_descriptor(effect, &desc);
    if (status != 0)
        goto exit;
    if (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0) {
        in->need_echo_reference = false;
        do_input_standby(in);
    }

exit:

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}


static int adev_open_output_stream(struct audio_hw_device *dev,
                                   uint32_t devices, int *format,
                                   uint32_t *channels, uint32_t *sample_rate,
                                   struct audio_stream_out **stream_out)
{
    struct imx_audio_device *ladev = (struct imx_audio_device *)dev;
    struct imx_stream_out *out;
    int ret;

    out = (struct imx_stream_out *)calloc(1, sizeof(struct imx_stream_out));
    if (!out)
        return -ENOMEM;
    LOGW("open output stream devices %d, format %d, channels %d, sample_rate %d",
                        devices, *format, *channels, *sample_rate);
    ret = create_resampler(DEFAULT_OUT_SAMPLING_RATE,
                           MM_FULL_POWER_SAMPLING_RATE,
                           2,
                           RESAMPLER_QUALITY_DEFAULT,
                           NULL,
                           &out->resampler);
    if (ret != 0)
        goto err_open;
    out->buffer = malloc(RESAMPLER_BUFFER_SIZE); /* todo: allow for reallocing */

    out->stream.common.get_sample_rate  = out_get_sample_rate;
    out->stream.common.set_sample_rate  = out_set_sample_rate;
    out->stream.common.get_buffer_size  = out_get_buffer_size;
    out->stream.common.get_channels     = out_get_channels;
    out->stream.common.get_format       = out_get_format;
    out->stream.common.set_format       = out_set_format;
    out->stream.common.standby          = out_standby;
    out->stream.common.dump             = out_dump;
    out->stream.common.set_parameters   = out_set_parameters;
    out->stream.common.get_parameters   = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect  = out_remove_audio_effect;
    out->stream.get_latency                 = out_get_latency;
    out->stream.set_volume                  = out_set_volume;
    out->stream.write                       = out_write;
    out->stream.get_render_position         = out_get_render_position;

    out->config                             = pcm_config_mm_out;

    out->dev = ladev;
    out->standby = 1;
    out->frame_count = 0;
    out->device      = devices;
    out->out_id      = ladev->out_stream_num;
    ladev->out_stream_num++;

    /* FIXME: when we support multiple output devices, we will want to
     * do the following:
     * adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
     * adev->devices |= out->device;
     * select_output_device(adev);
     * This is because out_set_parameters() with a route is not
     * guaranteed to be called after an output stream is opened. */

    *format = out_get_format(&out->stream.common);
    *channels = out_get_channels(&out->stream.common);
    *sample_rate = out_get_sample_rate(&out->stream.common);

    *stream_out = &out->stream;
    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct imx_stream_out *out = (struct imx_stream_out *)stream;
    struct imx_audio_device *ladev = (struct imx_audio_device *)dev;

    ladev->out_stream_num--;

    out_standby(&stream->common);
    if (out->buffer)
        free(out->buffer);
    if (out->resampler)
        release_resampler(out->resampler);
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct imx_audio_device *adev = (struct imx_audio_device *)dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret;
    LOGW("set parameters %s",kvpairs);
    parms = str_parms_create_str(kvpairs);
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_TTY_MODE, value, sizeof(value));
    if (ret >= 0) {
        int tty_mode;

        if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_OFF) == 0)
            tty_mode = TTY_MODE_OFF;
        else if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_VCO) == 0)
            tty_mode = TTY_MODE_VCO;
        else if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_HCO) == 0)
            tty_mode = TTY_MODE_HCO;
        else if (strcmp(value, AUDIO_PARAMETER_VALUE_TTY_FULL) == 0)
            tty_mode = TTY_MODE_FULL;
        else
            return -EINVAL;

        pthread_mutex_lock(&adev->lock);
        if (tty_mode != adev->tty_mode) {
            adev->tty_mode = tty_mode;
            if (adev->mode == AUDIO_MODE_IN_CALL)
                select_output_device(adev);
        }
        pthread_mutex_unlock(&adev->lock);
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_BT_NREC, value, sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->bluetooth_nrec = true;
        else
            adev->bluetooth_nrec = false;
    }

    ret = str_parms_get_str(parms, "screen_state", value, sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->low_power = false;
        else
            adev->low_power = true;
    }

    str_parms_destroy(parms);
    return ret;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    struct imx_audio_device *adev = (struct imx_audio_device *)dev;

    adev->voice_volume = volume;

    return 0;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, int mode)
{
    struct imx_audio_device *adev = (struct imx_audio_device *)dev;
    pthread_mutex_lock(&adev->lock);
    if (adev->mode != mode) {
        adev->mode = mode;
        select_mode(adev);
    }
    pthread_mutex_unlock(&adev->lock);

    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct imx_audio_device *adev = (struct imx_audio_device *)dev;

    adev->mic_mute = state;

    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct imx_audio_device *adev = (struct imx_audio_device *)dev;

    *state = adev->mic_mute;

    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         uint32_t sample_rate, int format,
                                         int channel_count)
{
    size_t size;

    if (check_input_parameters(sample_rate, format, channel_count) != 0)
        return 0;

    return get_input_buffer_size(sample_rate, format, channel_count);
}

static int adev_open_input_stream(struct audio_hw_device *dev, uint32_t devices,
                                  int *format, uint32_t *channel_mask,
                                  uint32_t *sample_rate,
                                  audio_in_acoustics_t acoustics,
                                  struct audio_stream_in **stream_in)
{
    struct imx_audio_device *ladev = (struct imx_audio_device *)dev;
    struct imx_stream_in *in;
    int ret;
    int rate;
    int channel_count = popcount(*channel_mask);

    if (check_input_parameters(*sample_rate, *format, channel_count) != 0)
        return -EINVAL;

    in = (struct imx_stream_in *)calloc(1, sizeof(struct imx_stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate   = in_get_sample_rate;
    in->stream.common.set_sample_rate   = in_set_sample_rate;
    in->stream.common.get_buffer_size   = in_get_buffer_size;
    in->stream.common.get_channels      = in_get_channels;
    in->stream.common.get_format        = in_get_format;
    in->stream.common.set_format        = in_set_format;
    in->stream.common.standby           = in_standby;
    in->stream.common.dump              = in_dump;
    in->stream.common.set_parameters    = in_set_parameters;
    in->stream.common.get_parameters    = in_get_parameters;
    in->stream.common.add_audio_effect  = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    in->requested_rate = *sample_rate;

    LOGW("In channels %d, rate %d, devices %x", channel_count, *sample_rate, devices);
    memcpy(&in->config, &pcm_config_mm_in, sizeof(pcm_config_mm_in));
    //in->config.channels = channel_count;
    //in->config.rate     = *sample_rate;
    /*fix to 2 channel,  caused by the wm8958 driver*/
    *channel_mask       = AUDIO_CHANNEL_IN_STEREO;
    in->config.channels = 2;
 
    if(devices == AUDIO_DEVICE_IN_ANLG_DOCK_MIC) {
        ret = scan_available_device(ladev);
        if(ret != 0) return -EINVAL;
        *channel_mask       = AUDIO_CHANNEL_IN_MONO;
        in->config.channels = 1;
        rate     = adev_get_rate_for_device(ladev, AUDIO_DEVICE_IN_ANLG_DOCK_MIC, PCM_IN);
        LOGW("rate %d", rate);
        if( rate == 0) {
              LOGW("can not get rate for in_device %d ", AUDIO_DEVICE_IN_ANLG_DOCK_MIC);
              return -EINVAL;
        }
        in->config.rate     =  rate;
    }

    in->buffer = malloc(in->config.period_size *
                        audio_stream_frame_size(&in->stream.common));
    if (!in->buffer) {
        ret = -ENOMEM;
        goto err;
    }

    if (in->requested_rate != in->config.rate) {
        in->buf_provider.get_next_buffer = get_next_buffer;
        in->buf_provider.release_buffer = release_buffer;

        ret = create_resampler(in->config.rate,
                               in->requested_rate,
                               in->config.channels,
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);
        if (ret != 0) {
            ret = -EINVAL;
            goto err;
        }
    }

    in->dev = ladev;
    in->standby = 1;
    in->device  = devices;

    *stream_in = &in->stream;
    return 0;

err:
    if (in->resampler)
        release_resampler(in->resampler);

    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *stream)
{
    struct imx_stream_in *in = (struct imx_stream_in *)stream;

    in_standby(&stream->common);

    if (in->resampler) {
        free(in->buffer);
        release_resampler(in->resampler);
    }

    free(stream);
    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct imx_audio_device *adev = (struct imx_audio_device *)device;
    int i;
    for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
        if(adev->mixer[i])
            mixer_close(adev->mixer[i]);

    free(device);
    return 0;
}

static uint32_t adev_get_supported_devices(const struct audio_hw_device *dev)
{
    struct imx_audio_device *adev = (struct imx_audio_device *)dev;
    int i;
    int devices = 0;
    for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
        if(adev->card_list[i])
            devices |= adev->card_list[i]->supported_devices;

    return devices;            
}

static int adev_get_rate_for_device(struct imx_audio_device *adev, uint32_t devices, unsigned int flag)
{
     int i;
     for (i = 0; i < MAX_AUDIO_CARD_NUM; i ++) {
          if(adev->card_list[i]->supported_devices & devices)
		return (flag==PCM_OUT) ? adev->card_list[i]->out_rate:adev->card_list[i]->in_rate;
     }
     return 0;
}


static int scan_available_device(struct imx_audio_device *adev)
{
    int i,j,k;
    int m,n;
    bool found;
    bool scanned;
    struct control *imx_control;
    int left_devices = SUPPORTED_DEVICE_IN_MODULE;
    int rate;
    /* open the mixer for main sound card, main sound cara is like sgtl5000, wm8958, cs428888*/
    /* note: some platform do not have main sound card, only have auxiliary card.*/
    /* max num of supported card is 2 */
    k = adev->audio_card_num;
    for(i = 0; i < k; i++) {
        left_devices &= ~adev->card_list[i]->supported_devices;
    }

    for (i = 0; i < MAX_AUDIO_CARD_SCAN ; i ++) {
        found = false;
        imx_control = control_open(i);
        if(!imx_control)
            break;
        LOGW("card %d, id %s ,driver %s, name %s", i, control_card_info_get_id(imx_control),
                                                      control_card_info_get_driver(imx_control),
                                                      control_card_info_get_name(imx_control));
        for(j = 0; j < SUPPORT_CARD_NUM; j++) {
            if(strstr(control_card_info_get_driver(imx_control), audio_card_list[j]->driver_name) != NULL){
                // check if the device have been scaned before
                scanned = false;
                n = k;
                for (m = 0; m < k; m++) {
                    if (!strcmp(audio_card_list[j]->driver_name, adev->card_list[m]->driver_name)) {
                         scanned = true;
                         found = true;

                         if(!strcmp(adev->card_list[m]->driver_name, "USB-Audio")) {
                             scanned = false;
                             left_devices |= adev->card_list[m]->supported_devices;
                             if(adev->mixer[m])
                                mixer_close(adev->mixer[m]);
                             n = m;
                             k --;
                         }
                    }
                }
                if (scanned) break;
                if(n >= MAX_AUDIO_CARD_NUM) {
                    break;
                }
                adev->card_list[n]  = audio_card_list[j];
                adev->mixer[n] = mixer_open(i);
                adev->card_list[n]->card = i;
                if (!adev->mixer[n]) {
                    LOGE("Unable to open the mixer, aborting.");
                    return -EINVAL;
                }
                rate = 8000;
                if( pcm_get_near_rate(i, 0, PCM_IN, &rate) == 0)
                        adev->card_list[n]->in_rate = rate;
                LOGW("in rate %d",adev->card_list[n]->in_rate);
                left_devices &= ~audio_card_list[j]->supported_devices;
                k ++;
                found = true;
                break;
            }
        }

        control_close(imx_control);
        if(!found){
            LOGW("unrecognized card found.");
        }
    }
    adev->audio_card_num = k;
    /*must have one card*/
    if(!adev->card_list[0]) {
        LOGE("no supported sound card found, aborting.");
        return  -EINVAL;
    }
    /*second card maybe null*/
    while (k < MAX_AUDIO_CARD_NUM) {
        adev->card_list[k]  = audio_card_list[SUPPORT_CARD_NUM-1];
        /*FIXME:This is workaround for some board which only have one card, whose supported device only is not full*/
        adev->card_list[k]->supported_devices  = left_devices;
        k++;
    }

    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct imx_audio_device *adev;
    int ret = 0;
    struct control *imx_control;
    int i,j,k;
    bool found;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct imx_audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag      = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version  = 0;
    adev->hw_device.common.module   = (struct hw_module_t *) module;
    adev->hw_device.common.close    = adev_close;

    adev->hw_device.get_supported_devices   = adev_get_supported_devices;
    adev->hw_device.init_check              = adev_init_check;
    adev->hw_device.set_voice_volume        = adev_set_voice_volume;
    adev->hw_device.set_master_volume       = adev_set_master_volume;
    adev->hw_device.set_mode                = adev_set_mode;
    adev->hw_device.set_mic_mute            = adev_set_mic_mute;
    adev->hw_device.get_mic_mute            = adev_get_mic_mute;
    adev->hw_device.set_parameters          = adev_set_parameters;
    adev->hw_device.get_parameters          = adev_get_parameters;
    adev->hw_device.get_input_buffer_size   = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream      = adev_open_output_stream;
    adev->hw_device.close_output_stream     = adev_close_output_stream;
    adev->hw_device.open_input_stream       = adev_open_input_stream;
    adev->hw_device.close_input_stream      = adev_close_input_stream;
    adev->hw_device.dump                    = adev_dump;

    ret = scan_available_device(adev);
    if (ret != 0) {
        free(adev);
        return ret;
    }

    /* Set the default route before the PCM stream is opened */
    pthread_mutex_lock(&adev->lock);
    for(i = 0; i < MAX_AUDIO_CARD_NUM; i++)
        set_route_by_array(adev->mixer[i], adev->card_list[i]->defaults, 1);
    adev->mode    = AUDIO_MODE_NORMAL;
    adev->devices = AUDIO_DEVICE_OUT_SPEAKER | AUDIO_DEVICE_IN_BUILTIN_MIC;
    select_output_device(adev);

    adev->pcm_modem_dl  = NULL;
    adev->pcm_modem_ul  = NULL;
    adev->voice_volume  = 1.0f;
    adev->tty_mode      = TTY_MODE_OFF;
    adev->device_is_imx = is_device_imx();
    adev->bluetooth_nrec = true;
    adev->wb_amr = 0;
    pthread_mutex_unlock(&adev->lock);

    *device = &adev->hw_device.common;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id = "audio.tinyalsa",
        .name = "tinyalsa audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};


