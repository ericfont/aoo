#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef AOO_SAMPLETYPE
#define AOO_SAMPLETYPE float
#endif

typedef AOO_SAMPLETYPE aoo_sample;

#define AOO_MAXPACKETSIZE 4096 // ?
#define AOO_DEFPACKETSIZE 512 // ?
#define AOO_DOMAIN "/AoO"
#define AOO_FORMAT "/format"
#define AOO_FORMAT_NARGS 7
#define AOO_FORMAT_WILDCARD "/AoO/*/format"
#define AOO_DATA "/data"
#define AOO_DATA_NARGS 9
#define AOO_DATA_WILDCARD "/AoO/*/data"
#define AOO_REQUEST "/request"
#define AOO_RESEND "/resend"

#ifndef AOO_CLIP_OUTPUT
#define AOO_CLIP_OUTPUT 0
#endif

// 0: error, 1: warning, 2: verbose, 3: debug
#ifndef LOGLEVEL
 #define LOGLEVEL 2
#endif

// time DLL:
// default bandwidth
#ifndef AOO_DLL_BW
 #define AOO_DLL_BW 0.012
#endif

#ifndef AOO_DEBUG_DLL
 #define AOO_DEBUG_DLL 0
#endif

#ifndef AOO_DEBUG_RESAMPLING
 #define AOO_DEBUG_RESAMPLING 0
#endif

#define AOO_RESEND_BUFSIZE 1000
#define AOO_RESEND_LIMIT 4
#define AOO_RESEND_INTERVAL 5
#define AOO_RESEND_MAXNUMFRAMES 64
#define AOO_RESEND_PACKETSIZE 256

void aoo_setup(void);
void aoo_close(void);

/*//////////////////// OSC ////////////////////////////*/

// id: the source or sink ID
// returns: the offset to the remaining address pattern

#define AOO_ID_WILDCARD -1
#define AOO_ID_NONE INT32_MIN

int32_t aoo_parsepattern(const char *msg, int32_t n, int32_t *id);

uint64_t aoo_osctime_get(void);

double aoo_osctime_toseconds(uint64_t t);

uint64_t aoo_osctime_fromseconds(double s);

uint64_t aoo_osctime_addseconds(uint64_t t, double s);

typedef void (*aoo_replyfn)(
        void *,         // endpoint
        const char *,   // data
        int32_t         // number of bytes
);

/*//////////////////// AoO source /////////////////////*/

#define AOO_SOURCE_DEFBUFSIZE 10

typedef struct aoo_source aoo_source;

typedef struct aoo_format
{
    const char *codec;
    int32_t nchannels;
    int32_t samplerate;
    int32_t blocksize;
} aoo_format;

typedef struct aoo_format_storage
{
    aoo_format header;
    char buf[256];
} aoo_format_storage;

typedef struct aoo_source_settings
{
    int32_t samplerate;
    int32_t blocksize;
    int32_t nchannels;
    int32_t buffersize;
    int32_t packetsize;
    int32_t resend_buffersize;
    double time_filter_bandwidth;
} aoo_source_settings;

aoo_source * aoo_source_new(int32_t id);

void aoo_source_free(aoo_source *src);

void aoo_source_setup(aoo_source *src, aoo_source_settings *settings);

void aoo_source_setformat(aoo_source *src, aoo_format *f);

// will send /AoO/<id>/start message
void aoo_source_addsink(aoo_source *src, void *sink, int32_t id, aoo_replyfn fn);

// will send /AoO/<id>/stop message
void aoo_source_removesink(aoo_source *src, void *sink, int32_t id);

// stop all sinks
void aoo_source_removeall(aoo_source *src);

void aoo_source_setsinkchannel(aoo_source *src, void *sink, int32_t id, int32_t chn);

// e.g. /request
void aoo_source_handlemessage(aoo_source *src, const char *data, int32_t n,
                              void *sink, aoo_replyfn fn);

int32_t aoo_source_send(aoo_source *src);

int32_t aoo_source_process(aoo_source *src, const aoo_sample **data, int32_t n, uint64_t t);

/*//////////////////// AoO sink /////////////////////*/

#define AOO_SINK_DEFBUFSIZE 10

typedef struct aoo_sink aoo_sink;

// event types
typedef enum aoo_event_type
{
    AOO_SOURCE_STATE_EVENT
} aoo_event_type;

// source state event
typedef enum aoo_source_state
{
    AOO_SOURCE_STOP,
    AOO_SOURCE_PLAY
} aoo_source_state;

typedef struct aoo_source_state_event
{
    aoo_event_type type;
    void *endpoint;
    int32_t id;
    aoo_source_state state;
} aoo_source_state_event;

// event union
typedef union aoo_event
{
    aoo_event_type type;
    aoo_source_state_event source_state;
} aoo_event;

typedef void (*aoo_processfn)(
        void *,                 // user data
        const aoo_sample **,    // sample data
        int32_t,                // number of samples per channel
        const aoo_event *,      // event array
        int32_t                 // number of events
);

typedef struct aoo_sink_settings
{
    void *userdata;
    aoo_processfn processfn;
    int32_t samplerate;
    int32_t blocksize;
    int32_t nchannels;
    int32_t buffersize;
    int32_t resend_limit;
    int32_t resend_interval;
    int32_t resend_maxnumframes;
    int32_t resend_packetsize;
    double time_filter_bandwidth;
} aoo_sink_settings;

aoo_sink * aoo_sink_new(int32_t id);

void aoo_sink_free(aoo_sink *sink);

void aoo_sink_setup(aoo_sink *sink, aoo_sink_settings *settings);

// e.g. /start, /stop, /data.
// Might reply with /AoO/<id>/request
int32_t aoo_sink_handlemessage(aoo_sink *sink, const char *data, int32_t n,
                            void *src, aoo_replyfn fn);

int32_t aoo_sink_process(aoo_sink *sink, uint64_t t);


/*//////////////////// Codec //////////////////////////*/

#define AOO_CODEC_MAXSETTINGSIZE 256

typedef void* (*aoo_codec_new)(void);

typedef void (*aoo_codec_free)(void *);

typedef void (*aoo_codec_setup)(void *, aoo_format *);

typedef int32_t (*aoo_codec_writeformat)(
        void *,         // the encoder instance
        int32_t *,      // nchannels
        int32_t *,      // samplerate
        int32_t *,      // blocksize
        char *,         // output buffer
        int32_t         // buffer size
);

typedef int32_t (*aoo_codec_readformat)(
        void *,         // the decoder instance
        int32_t,        // nchannels
        int32_t,        // samplerate
        int32_t,        // blocksize
        const char *,   // input buffer
        int32_t         // number of bytes
);

typedef int32_t (*aoo_codec_encode)(
        void *,             // the encoder instance
        const aoo_sample *, // input samples (interleaved)
        int32_t,            // number of samples
        char *,             // output buffer
        int32_t             // max. size of output buffer
);

typedef int32_t (*aoo_codec_decode)(
        void *,         // the decoder instance
        const char *,   // input bytes
        int32_t,        // input size
        aoo_sample *,   // output samples (interleaved)
        int32_t         // number of samples

);

typedef struct aoo_codec
{
    const char *name;
    // encoder
    aoo_codec_new encoder_new;
    aoo_codec_free encoder_free;
    aoo_codec_setup encoder_setup;
    aoo_codec_encode encoder_encode;
    aoo_codec_writeformat encoder_write;
    // decoder
    aoo_codec_new decoder_new;
    aoo_codec_free decoder_free;
    aoo_codec_decode decoder_decode;
    aoo_codec_readformat decoder_read;
} aoo_codec;

typedef void (*aoo_codec_registerfn)(const char *, const aoo_codec *);

#ifdef __cplusplus
} // extern "C"
#endif
