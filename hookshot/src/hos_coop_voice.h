/* SPDX-License-Identifier: GPL-3.0-or-later
   Proximity voice chat between the co-op module and Hookshot (oot-remake-multiplayer, docs/voice.md).

   Hookshot's host/coop_voice.c (our patch 0009) finds the co-op module once it is loaded and, when the module exports
   hos_coop_voice_attach, hands it this table: the game's microphone (the one chosen in the Insert menu, shared with the
   game's own input streams in host/nn_audio_in.c) and Hookshot's libopus. While attach said 1, the game's mixer
   (host/nn_audio.c) calls the module's hos_coop_voice_render every 5 ms with the final mix's buffers, so the voices are
   mixed in under the game's own volume, effects and speaker layout. The same file is in Hookshot's host/ through our
   patch and in our repository (hookshot/src): keep them equal. Plain C. */
#ifndef HOS_COOP_VOICE_H
#define HOS_COOP_VOICE_H
#include <stddef.h>
#include <stdint.h>

#define HOS_COOP_VOICE_VERSION 1u
#define HOS_COOP_VOICE_RATE 48000     /* every sample here: 48 kHz, mono, 16-bit */
#define HOS_COOP_VOICE_FRAME 960      /* one Opus frame: 20 ms */

typedef struct HosCoopVoiceApi {
    uint32_t version;   /* HOS_COOP_VOICE_VERSION */
    uint32_t size;      /* sizeof(HosCoopVoiceApi) */
    /* The microphone, any one thread: a stream of its own on the source the game's streams use (opened once, the Insert
       menu's choice, silence when that is off or there is none). start: 1 when started; read: up to max samples, the
       number read (what came since the last read, at most a quarter second: older is dropped). */
    int (*mic_start)(void);
    int (*mic_read)(int16_t *out, int max);
    void (*mic_stop)(void);
    /* Opus, 48 kHz mono. An encoder for speech (VOIP, in-band FEC for `loss_percent` loss) at `bitrate` bits a second;
       encode: one HOS_COOP_VOICE_FRAME frame into at most max bytes, the bytes made or 0. A decoder; decode: one frame
       into pcm (HOS_COOP_VOICE_FRAME samples), from data, from data's FEC when fec is 1, or concealed when data is
       NULL; the samples made or 0. Each object is used by one thread at a time. */
    void *(*opus_encoder_new)(int bitrate, int loss_percent);
    int (*opus_encode)(void *encoder, const int16_t *pcm, uint8_t *out, int max);
    void (*opus_encoder_free)(void *encoder);
    void *(*opus_decoder_new)(void);
    int (*opus_decode)(void *decoder, const uint8_t *data, int size, int fec, int16_t *pcm);
    void (*opus_decoder_free)(void *decoder);
    /* Microseconds on the mixer's clock (the one hos_coop_voice_render's frames follow), any thread. */
    uint64_t (*now_us)(void);
} HosCoopVoiceApi;

/* The module's optional exports (extern "C"). attach: any thread, once; 1 = render me. */
typedef int (*hos_coop_voice_attach_fn)(const HosCoopVoiceApi *api);
/* The game's mixer thread, every 5 ms: add `frames` samples of the voices into out[0..channels-1], which are the final
   mix's buffers for 2 (left, right) or 6 (front left, front right, centre, LFE, rear left, rear right) speakers, full
   scale 1.0. Quick: the game's sound waits on it. */
typedef void (*hos_coop_voice_render_fn)(float *const *out, int channels, int frames);

#endif
