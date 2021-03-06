#include "aoo/aoo_pcm.h"
#include "aoo/aoo_utils.hpp"

#include <cassert>
#include <cstring>

namespace {

// conversion routines between aoo_sample and PCM data
union convert {
    int8_t b[8];
    int16_t i16;
    int32_t i32;
    int64_t i64;
    float f;
    double d;
};

int32_t bytes_per_sample(aoo_pcm_bitdepth bd){
    switch (bd){
    case AOO_PCM_INT16:
        return 2;
    case AOO_PCM_INT24:
        return 3;
    case AOO_PCM_FLOAT32:
        return 4;
    case AOO_PCM_FLOAT64:
        return 8;
    default:
        assert(false);
        return 0;
    }
}

void sample_to_int16(aoo_sample in, char *out){
    convert c;
    int32_t temp = in * 0x7fff + 0.5f;
    c.i16 = (temp > INT16_MAX) ? INT16_MAX : (temp < INT16_MIN) ? INT16_MIN : temp;
#if BYTE_ORDER == BIG_ENDIAN
    memcpy(out, c.b, 2); // optimized away
#else
    out[0] = c.b[1];
    out[1] = c.b[0];
#endif
}

void sample_to_int24(aoo_sample in, char *out){
    convert c;
    int32_t temp = in * 0x7fffffff + 0.5f;
    c.i32 = (temp > INT32_MAX) ? INT32_MAX : (temp < INT32_MIN) ? INT32_MIN : temp;
    // only copy the highest 3 bytes!
#if BYTE_ORDER == BIG_ENDIAN
    out[0] = c.b[0];
    out[1] = c.b[1];
    out[2] = c.b[2];
#else
    out[0] = c.b[3];
    out[1] = c.b[2];
    out[2] = c.b[1];
#endif
}

void sample_to_float32(aoo_sample in, char *out){
    aoo::to_bytes<float>(in, out);
}

void sample_to_float64(aoo_sample in, char *out){
    aoo::to_bytes<double>(in, out);
}

aoo_sample int16_to_sample(const char *in){
    convert c;
#if BYTE_ORDER == BIG_ENDIAN
    memcpy(c.b, in, 2); // optimized away
#else
    c.b[0] = in[1];
    c.b[1] = in[0];
#endif
    return(aoo_sample)c.i16 / 32768.f;
}

aoo_sample int24_to_sample(const char *in){
    convert c;
    // copy to the highest 3 bytes!
#if BYTE_ORDER == BIG_ENDIAN
    c.b[0] = in[0];
    c.b[1] = in[1];
    c.b[2] = in[2];
    c.b[3] = 0;
#else
    c.b[0] = 0;
    c.b[1] = in[2];
    c.b[2] = in[1];
    c.b[3] = in[0];
#endif
    return (aoo_sample)c.i32 / 0x7fffffff;
}

aoo_sample float32_to_sample(const char *in){
    return aoo::from_bytes<float>(in);
}

aoo_sample float64_to_sample(const char *in){
    return aoo::from_bytes<double>(in);
}

struct codec {
    aoo_format_pcm format;
};

void print_settings(const aoo_format_pcm& f){
    LOG_VERBOSE("PCM settings: "
                << "nchannels = " << f.header.nchannels
                << ", blocksize = " << f.header.blocksize
                << ", samplerate = " << f.header.samplerate
                << ", bitdepth = " << bytes_per_sample(f.bitdepth));
}

void *encoder_new(){
    return new codec;
}

void encoder_free(void *enc){
    delete (codec *)enc;
}

void encoder_setup(void *enc, aoo_format *f){
    assert(!strcmp(f->codec, AOO_CODEC_PCM));
    auto c = static_cast<codec *>(enc);
    auto fmt = reinterpret_cast<aoo_format_pcm *>(f);

    // validate blocksize
    if (fmt->header.blocksize <= 0){
        LOG_WARNING("PCM: bad blocksize " << fmt->header.blocksize
                    << ", using 64 samples");
        fmt->header.blocksize = 64;
    }
    // validate samplerate
    if (fmt->header.samplerate <= 0){
        LOG_WARNING("PCM: bad samplerate " << fmt->header.samplerate
                    << ", using 44100");
        fmt->header.samplerate = 44100;
    }
    // validate channels
    if (fmt->header.nchannels <= 0 || fmt->header.nchannels > 255){
        LOG_WARNING("PCM: bad channel count " << fmt->header.nchannels
                    << ", using 1 channel");
        fmt->header.nchannels = 1;
    }
    // validate bitdepth
    if (fmt->bitdepth < 0 || fmt->bitdepth > AOO_PCM_BITDEPTH_SIZE){
        LOG_WARNING("PCM: bad bitdepth, using 32bit float");
        fmt->bitdepth = AOO_PCM_FLOAT32;
    }

    // save and print settings
    memcpy(&c->format, fmt, sizeof(aoo_format_pcm));
    print_settings(c->format);
}

int32_t encoder_encode(void *enc,
                       const aoo_sample *s, int32_t n,
                       char *buf, int32_t size)
{
    auto bitdepth = static_cast<codec *>(enc)->format.bitdepth;
    auto samplesize = bytes_per_sample(bitdepth);

    assert(size >= n * samplesize);

    auto samples_to_blob = [&](auto fn){
        auto b = buf;
        for (int i = 0; i < n; ++i){
            fn(s[i], b);
            b += samplesize;
        }
    };

    switch (bitdepth){
    case AOO_PCM_INT16:
        samples_to_blob(sample_to_int16);
        break;
    case AOO_PCM_INT24:
        samples_to_blob(sample_to_int24);
        break;
    case AOO_PCM_FLOAT32:
        samples_to_blob(sample_to_float32);
        break;
    case AOO_PCM_FLOAT64:
        samples_to_blob(sample_to_float64);
        break;
    default:
        // unknown bitdepth
        break;
    }

    return n * samplesize;
}

int32_t encoder_write(void *enc, int32_t *nchannels, int32_t *samplerate,
                      int32_t *blocksize, char *buf, int32_t size){
    if (size >= 4){
        auto c = static_cast<codec *>(enc);
        *nchannels = c->format.header.nchannels;
        *samplerate = c->format.header.samplerate;
        *blocksize = c->format.header.blocksize;
        aoo::to_bytes<int32_t>(c->format.bitdepth, buf);

        return 4;
    } else {
        LOG_ERROR("PCM: couldn't write settings - buffer too small!");
        return -1;
    }
}

void *decoder_new(){
    return new codec;
}

void decoder_free(void *dec){
    delete (codec *)dec;
}

int32_t decoder_decode(void *dec,
                       const char *buf, int32_t size,
                       aoo_sample *s, int32_t n)
{
    auto c = static_cast<codec *>(dec);
    assert(c->format.header.blocksize != 0);

    auto samplesize = bytes_per_sample(c->format.bitdepth);

    assert(n >= (size / samplesize));

    auto blob_to_samples = [&](auto convfn){
        auto b = buf;
        for (int i = 0; i < n; ++i, b += samplesize){
            s[i] = convfn(b);
        }
    };

    switch (c->format.bitdepth){
    case AOO_PCM_INT16:
        blob_to_samples(int16_to_sample);
        break;
    case AOO_PCM_INT24:
        blob_to_samples(int24_to_sample);
        break;
    case AOO_PCM_FLOAT32:
        blob_to_samples(float32_to_sample);
        break;
    case AOO_PCM_FLOAT64:
        blob_to_samples(float64_to_sample);
        break;
    default:
        // unknown bitdepth
        return 0;
    }

    return size / samplesize;
}

int32_t decoder_read(void *dec, int32_t nchannels, int32_t samplerate,
                     int32_t blocksize, const char *buf, int32_t size){
    if (size >= 4){
        auto c = static_cast<codec *>(dec);
        c->format.header.nchannels = nchannels;
        c->format.header.samplerate = samplerate;
        c->format.header.blocksize = blocksize;
        c->format.bitdepth = (aoo_pcm_bitdepth)aoo::from_bytes<int32_t>(buf);
        // TODO validate
        print_settings(c->format);
        return 4;
    } else {
        LOG_ERROR("PCM: couldn't read settings - not enough data!");
    }
    return -1;
}

aoo_codec codec_class = {
    AOO_CODEC_PCM,
    encoder_new,
    encoder_free,
    encoder_setup,
    encoder_encode,
    encoder_write,
    decoder_new,
    decoder_free,
    decoder_decode,
    decoder_read
};

} // namespace

void aoo_codec_pcm_setup(aoo_codec_registerfn fn){
    fn(AOO_CODEC_PCM, &codec_class);
}

