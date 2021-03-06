#include "aoo_source.hpp"
#include "aoo/aoo_utils.hpp"
#include "aoo/aoo_osc.hpp"

#include <cstring>
#include <algorithm>
#include <random>

/*//////////////////// AoO source /////////////////////*/

#define AOO_DATA_HEADERSIZE 80
// address pattern string: max 32 bytes
// typetag string: max. 12 bytes
// args (without blob data): 36 bytes

namespace aoo {

isource * isource::create(int32_t id){
    return new aoo_source(id);
}

void isource::destroy(isource *x){
    delete x;
}

} // aoo

aoo_source * aoo_source_new(int32_t id) {
    return new aoo_source(id);
}

aoo_source::aoo_source(int32_t id)
    : id_(id){}

void aoo_source_free(aoo_source *src){
    delete src;
}

aoo_source::~aoo_source() {}

void aoo_source_setformat(aoo_source *src, aoo_format *f) {
    if (f){
        src->set_format(*f);
    }
}

void aoo_source::set_format(aoo_format &f){
    salt_ = make_salt();

    if (!encoder_ || strcmp(encoder_->name(), f.codec)){
        auto codec = aoo::find_codec(f.codec);
        if (codec){
            encoder_ = codec->create_encoder();
        } else {
            LOG_ERROR("codec '" << f.codec << "' not supported!");
            return;
        }
        if (!encoder_){
            LOG_ERROR("couldn't create encoder!");
            return;
        }
    }
    encoder_->setup(f);

    sequence_ = 0;
    update();
    for (auto& sink : sinks_){
        send_format(sink);
    }
}

void aoo_source_setup(aoo_source *src, aoo_source_settings *settings){
    if (settings){
        src->setup(*settings);
    }
}

void aoo_source::setup(aoo_source_settings &settings){
    blocksize_ = settings.blocksize;
    nchannels_ = settings.nchannels;
    samplerate_ = settings.samplerate;
    buffersize_ = std::max<int32_t>(settings.buffersize, 0);
    resend_buffersize_ = std::max<int32_t>(settings.resend_buffersize, 0);

    // packet size
    const int32_t minpacketsize = AOO_DATA_HEADERSIZE + 64;
    if (settings.packetsize < minpacketsize){
        LOG_WARNING("packet size too small! setting to " << minpacketsize);
        packetsize_ = minpacketsize;
    } else if (settings.packetsize > AOO_MAXPACKETSIZE){
        LOG_WARNING("packet size too large! setting to " << AOO_MAXPACKETSIZE);
        packetsize_ = AOO_MAXPACKETSIZE;
    } else {
        packetsize_ = settings.packetsize;
    }

    // time filter
    bandwidth_ = settings.time_filter_bandwidth;
    starttime_ = 0; // will update

    if (encoder_){
        update();
    }
}

void aoo_source::update(){
    assert(encoder_ != nullptr && encoder_->blocksize() > 0 && encoder_->samplerate() > 0);
    if (blocksize_ > 0 && samplerate_ > 0 && nchannels_ > 0){
        // setup audio buffer
        {
            auto nsamples = encoder_->blocksize() * nchannels_;
            double bufsize = (double)buffersize_ * encoder_->samplerate() * 0.001;
            auto d = div(bufsize, encoder_->blocksize());
            int32_t nbuffers = d.quot + (d.rem != 0); // round up
            nbuffers = std::max<int32_t>(nbuffers, 1); // need at least 1 buffer!
            audioqueue_.resize(nbuffers * nsamples, nsamples);
            srqueue_.resize(nbuffers, 1);
            LOG_DEBUG("aoo_source::update: nbuffers = " << nbuffers);
        }
        // setup resampler
        if (blocksize_ != encoder_->blocksize() || samplerate_ != encoder_->samplerate()){
            resampler_.setup(blocksize_, encoder_->blocksize(),
                             samplerate_, encoder_->samplerate(), nchannels_);
            resampler_.update(samplerate_, encoder_->samplerate());
        } else {
            resampler_.clear();
        }
        // setup history buffer
        {
            double bufsize = (double)resend_buffersize_ * 0.001 * samplerate_;
            auto d = div(bufsize, encoder_->blocksize());
            int32_t nbuffers = d.quot + (d.rem != 0); // round up
            // empty buffer is allowed! (no resending)
            history_.resize(nbuffers);
        }
    }
}

void aoo_source_addsink(aoo_source *src, void *sink, int32_t id, aoo_replyfn fn) {
    src->add_sink(sink, id, fn);
}

void aoo_source::add_sink(void *sink, int32_t id, aoo_replyfn fn){
    if (id == AOO_ID_WILDCARD){
        // remove all existing descriptors matching sink
        remove_sink(sink, AOO_ID_WILDCARD);
    }
    auto result = std::find_if(sinks_.begin(), sinks_.end(), [&](auto& s){
        return (s.endpoint == sink) && (s.id == id);
    });
    if (result == sinks_.end()){
        sink_desc sd = { sink, fn, id, 0 };
        sinks_.push_back(sd);
        send_format(sd);
    } else {
        LOG_WARNING("aoo_source::add_sink: sink already added!");
    }
}

void aoo_source_removesink(aoo_source *src, void *sink, int32_t id) {
    src->remove_sink(sink, id);
}

void aoo_source::remove_sink(void *sink, int32_t id){
    if (id == AOO_ID_WILDCARD){
        // remove all descriptors matching sink (ignore id)
        auto it = std::remove_if(sinks_.begin(), sinks_.end(), [&](auto& s){
            return s.endpoint == sink;
        });
        sinks_.erase(it, sinks_.end());
    } else {
        auto result = std::find_if(sinks_.begin(), sinks_.end(), [&](auto& s){
            return (s.endpoint == sink) && (s.id == id);
        });
        if (result != sinks_.end()){
            sinks_.erase(result);
        } else {
            LOG_WARNING("aoo_source::remove_sink: sink not found!");
        }
    }
}

void aoo_source_removeall(aoo_source *src) {
    src->remove_all();
}

void aoo_source::remove_all(){
    sinks_.clear();
}

void aoo_source_setsinkchannel(aoo_source *src, void *sink, int32_t id, int32_t chn){
    src->set_sink_channel(sink, id, chn);
}

void aoo_source::set_sink_channel(void *sink, int32_t id, int32_t chn){
    if (chn < 0){
        LOG_ERROR("aoo_source: channel onset " << chn << " out of range!");
    }
    if (id == AOO_ID_WILDCARD){
        for (auto& s : sinks_){
            if (s.endpoint == sink){
                LOG_VERBOSE("aoo_source: send to sink " << s.id << " on channel " << chn);
                s.channel = chn;
            }
        }
    } else {
        auto result = std::find_if(sinks_.begin(), sinks_.end(), [&](auto& s){
            return (s.endpoint == sink) && (s.id == id);
        });
        if (result != sinks_.end()){
            LOG_VERBOSE("aoo_source: send to sink " << result->id << " on channel " << chn);
            result->channel = chn;
        } else {
            LOG_ERROR("aoo_source::set_sink_channel: sink not found!");
        }
    }
}

void aoo_source_handlemessage(aoo_source *src, const char *data, int32_t n,
                              void *sink, aoo_replyfn fn) {
    src->handle_message(data, n, sink, fn);
}

// /AoO/<src>/request <sink>
void aoo_source::handle_message(const char *data, int32_t n, void *endpoint, aoo_replyfn fn){
    aoo::osc::received_packet packet(data, n);

    if (packet.is_bundle()){
        LOG_WARNING("OSC bundles are not supported (yet)");
        return;
    }

    aoo::osc::received_message msg(packet);

    int32_t src = 0;
    auto onset = aoo_parsepattern(data, n, &src);
    if (!onset){
        LOG_WARNING("not an AoO message!");
        return;
    }
    if (src != id_ && src != AOO_ID_WILDCARD){
        LOG_WARNING("wrong source ID!");
        return;
    }

    if (!strcmp(msg.address_pattern() + onset, AOO_REQUEST)){
        if (msg.count() == 1){
            auto it = msg.begin();
            auto id = it->as_int32();
            auto sink = std::find_if(sinks_.begin(), sinks_.end(), [&](auto& s){
                return (s.endpoint == endpoint) && (s.id == id);
            });
            if (sink != sinks_.end()){
                // just resend format (the last format message might have been lost)
                send_format(*sink);
            } else {
                // add new sink
                add_sink(endpoint, id, fn);
            }
        } else {
            LOG_ERROR("wrong number of arguments for /request message");
        }
    } else if (!strcmp(msg.address_pattern() + onset, AOO_RESEND)){
        if (!history_.capacity()){
            return;
        }
        if (msg.count() >= 4){
            auto it = msg.begin();
            // get ID
            auto id = (it++)->as_int32();
            auto sink = std::find_if(sinks_.begin(), sinks_.end(), [&](auto& s){
                return (s.endpoint == endpoint) && (s.id == id);
            });
            if (sink == sinks_.end()){
                LOG_VERBOSE("ignoring '/resend' message: sink not found");
                return;
            }
            // get salt
            auto salt = (it++)->as_int32();
            if (salt != salt_){
                LOG_VERBOSE("ignoring '/resend' message: source has changed");
                return;
            }
            // get pairs of [seq, frame]
            int npairs = (msg.count() - 2) / 2;
            while (npairs--){
                auto seq = (it++)->as_int32();
                auto framenum = (it++)->as_int32();
                auto block = history_.find(seq);
                if (block){
                    aoo::data_packet d;
                    d.sequence = block->sequence;
                    d.samplerate = block->samplerate;
                    d.totalsize = block->size();
                    d.nframes = block->num_frames();
                    if (framenum < 0){
                        // whole block
                        for (int i = 0; i < d.nframes; ++i){
                            d.framenum = i;
                            block->get_frame(i, d.data, d.size);
                            send_data(*sink, d);
                        }
                    } else {
                        // single frame
                        d.framenum = framenum;
                        block->get_frame(framenum, d.data, d.size);
                        send_data(*sink, d);
                    }
                } else {
                    LOG_VERBOSE("couldn't find block " << seq);
                }
            }
        } else {
            LOG_ERROR("bad number of arguments for /resend message");
        }
    } else {
        LOG_WARNING("unknown message '" << (msg.address_pattern() + onset) << "'");
    }
}

int32_t aoo_source_send(aoo_source *src) {
    return src->send();
}

bool aoo_source::send(){
    if (!encoder_){
        return false;
    }

    if (audioqueue_.read_available() && srqueue_.read_available()){
        const auto nchannels = encoder_->nchannels();
        const auto blocksize = encoder_->blocksize();
        aoo::data_packet d;
        d.sequence = sequence_;
        d.samplerate = srqueue_.read();

        // copy and convert audio samples to blob data
        const auto blobmaxsize = sizeof(double) * nchannels * blocksize; // overallocate
        char * blobdata = (char *)alloca(blobmaxsize);

        d.totalsize = encoder_->encode(audioqueue_.read_data(), audioqueue_.blocksize(),
                                        blobdata, blobmaxsize);

        auto maxpacketsize = packetsize_ - AOO_DATA_HEADERSIZE;
        auto dv = div(d.totalsize, maxpacketsize);
        d.nframes = dv.quot + (dv.rem != 0);

        // save block
        history_.push(d.sequence, d.samplerate,
                      blobdata, d.totalsize, d.nframes, maxpacketsize);

        // send a single frame to all sink
        // /AoO/<sink>/data <src> <salt> <seq> <sr> <channel_onset> <totalsize> <numpackets> <packetnum> <data>
        auto dosend = [&](int32_t frame, const char* data, auto n){
            d.framenum = frame;
            d.data = data;
            d.size = n;
            for (auto& sink : sinks_){
                send_data(sink, d);
            }
        };

        auto blobptr = blobdata;
        // send large frames (might be 0)
        for (int32_t i = 0; i < dv.quot; ++i, blobptr += maxpacketsize){
            dosend(i, blobptr, maxpacketsize);
        }
        // send remaining bytes as a single frame (might be the only one!)
        if (dv.rem){
            dosend(dv.quot, blobptr, dv.rem);
        }

        audioqueue_.read_commit(); // commit the read after sending!

        sequence_++;
        // handle overflow (with 64 samples @ 44.1 kHz this happens every 36 days)
        // for now just force a reset by changing the salt, LATER think how to handle this better
        if (sequence_ == INT32_MAX){
            salt_ = make_salt();
        }
        return true;
    } else {
        // LOG_DEBUG("couldn't send");
        return false;
    }
}

int32_t aoo_source_process(aoo_source *src, const aoo_sample **data, int32_t n, uint64_t t) {
    return src->process(data, n, t);
}

bool aoo_source::process(const aoo_sample **data, int32_t n, uint64_t t){
    // update DLL
    aoo::time_tag tt(t);
    if (starttime_ == 0){
        LOG_VERBOSE("setup time DLL for source");
        starttime_ = tt.to_double();
        dll_.setup(samplerate_, blocksize_, bandwidth_, 0);
    } else {
        auto elapsed = tt.to_double() - starttime_;
        dll_.update(elapsed);
    #if AOO_DEBUG_DLL
        fprintf(stderr, "SOURCE\n");
        // fprintf(stderr, "timetag: %llu, seconds: %f\n", tt.to_uint64(), tt.to_double());
        fprintf(stderr, "elapsed: %f, period: %f, samplerate: %f\n",
                elapsed, dll_.period(), dll_.samplerate());
        fflush(stderr);
    #endif
    }

    if (!encoder_ || sinks_.empty()){
        return false;
    }

    // non-interleaved -> interleaved
    auto insamples = blocksize_ * nchannels_;
    auto outsamples = encoder_->blocksize() * nchannels_;
    auto *buf = (aoo_sample *)alloca(insamples * sizeof(aoo_sample));
    for (int i = 0; i < nchannels_; ++i){
        for (int j = 0; j < n; ++j){
            buf[j * nchannels_ + i] = data[i][j];
        }
    }
    if (encoder_->blocksize() != blocksize_ || encoder_->samplerate() != samplerate_){
        // go through resampler
        if (resampler_.write_available() >= insamples){
            resampler_.write(buf, insamples);
        } else {
            LOG_DEBUG("couldn't process");
            return false;
        }
        while (resampler_.read_available() >= outsamples
               && audioqueue_.write_available()
               && srqueue_.write_available())
        {
            // copy audio samples
            resampler_.read(audioqueue_.write_data(), audioqueue_.blocksize());
            audioqueue_.write_commit();

            // push samplerate
            auto ratio = (double)encoder_->samplerate() / (double)samplerate_;
            srqueue_.write(dll_.samplerate() * ratio);
        }

        return true;
    } else {
        // bypass resampler
        if (audioqueue_.write_available() && srqueue_.write_available()){
            // copy audio samples
            std::copy(buf, buf + outsamples, audioqueue_.write_data());
            audioqueue_.write_commit();

            // push samplerate
            srqueue_.write(dll_.samplerate());

            return true;
        } else {
            LOG_DEBUG("couldn't process");
            return false;
        }
    }
}

// /AoO/<sink>/data <src> <salt> <seq> <sr> <channel_onset> <totalsize> <nframes> <frame> <data>

void aoo_source::send_data(sink_desc& sink, const aoo::data_packet& d){
    assert(d.data != nullptr);

    char buf[AOO_MAXPACKETSIZE];
    aoo::osc::message_builder msg(buf, sizeof(buf));

    const int32_t max_addr_size = sizeof(AOO_DOMAIN) + 16 + sizeof(AOO_DATA);
    char address[max_addr_size];
    const char *addr;
    if (sink.id != AOO_ID_WILDCARD){
        snprintf(address, sizeof(address), "%s/%d%s", AOO_DOMAIN, sink.id, AOO_DATA);
        addr = address;
    } else {
        addr = AOO_DATA_WILDCARD;
    }

    msg.set(addr, id_, salt_, d.sequence, d.samplerate, sink.channel,
            d.totalsize, d.nframes, d.framenum, aoo::osc::blob(d.data, d.size));

    if (!msg.valid()){
        LOG_ERROR("invalid data message");
        return;
    } else {
    #if 0
        std::cerr << "send data message:\n";
        for (int i = 0; i < msg.size(); ++i){
            std::cerr << (int)(uint8_t)msg.data()[i] << " ";
        }
        std::cerr << std::endl;
    #endif
    }

    sink.send(msg.data(), msg.size());

    LOG_DEBUG("send block: seq = " << d.sequence << ", sr = " << d.samplerate
              << ", chn = " << sink.channel << ", totalsize = " << d.totalsize
              << ", nframes = " << d.nframes << ", frame = " << d.framenum << ", size " << d.size);
}

// /AoO/<sink>/format <src> <salt> <numchannels> <samplerate> <blocksize> <codec> <options...>

void aoo_source::send_format(sink_desc &sink){
    if (encoder_){
        char buf[AOO_MAXPACKETSIZE];
        aoo::osc::message_builder msg(buf, sizeof(buf));

        const int32_t max_addr_size = sizeof(AOO_DOMAIN) + 16 + sizeof(AOO_FORMAT);
        char address[max_addr_size];
        const char *addr;
        if (sink.id != AOO_ID_WILDCARD){
            snprintf(address, sizeof(address), "%s/%d%s", AOO_DOMAIN, sink.id, AOO_FORMAT);
            addr = address;
        } else {
            addr = AOO_FORMAT_WILDCARD;
        }

        auto settings = (char *)alloca(AOO_CODEC_MAXSETTINGSIZE);
        int32_t nchannels, samplerate, blocksize;
        auto setsize = encoder_->write(nchannels, samplerate, blocksize, settings, AOO_CODEC_MAXSETTINGSIZE);

        msg.set(addr, id_, salt_, nchannels, samplerate, blocksize, encoder_->name(),
                aoo::osc::blob(settings, setsize));

        if (!msg.valid()){
            LOG_ERROR("invalid format message");
            return;
        } else {
        #if 0
            std::cerr << "send format message:\n";
            for (int i = 0; i < msg.size(); ++i){
                std::cerr << (int)(uint8_t)msg.data()[i] << " ";
            }
            std::cerr << std::endl;
        #endif
        }

        sink.send(msg.data(), msg.size());
    }
}

int32_t aoo_source::make_salt(){
    thread_local std::random_device dev;
    thread_local std::mt19937 mt(dev());
    std::uniform_int_distribution<int32_t> dist;
    return dist(mt);
}
