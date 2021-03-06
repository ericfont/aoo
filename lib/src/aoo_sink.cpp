#include "aoo_sink.hpp"
#include "aoo/aoo_utils.hpp"
#include "aoo/aoo_osc.hpp"

#include <algorithm>

namespace aoo {

/*////////////////////////// source_desc /////////////////////////////*/

source_desc::source_desc(void *_endpoint, aoo_replyfn _fn, int32_t _id, int32_t _salt)
    : endpoint(_endpoint), fn(_fn), id(_id), salt(_salt), laststate(AOO_SOURCE_STOP) {}

void source_desc::send(const char *data, int32_t n){
    fn(endpoint, data, n);
}

} // aoo

/*//////////////////// aoo_sink /////////////////////*/

namespace aoo {

isink * isink::create(int32_t id){
    return new aoo_sink(id);
}

void isink::destroy(isink *x){
    delete x;
}

} // aoo

aoo_sink * aoo_sink_new(int32_t id) {
    return new aoo_sink(id);
}

void aoo_sink_free(aoo_sink *sink) {
    delete sink;
}

void aoo_sink_setup(aoo_sink *sink, aoo_sink_settings *settings) {
    if (settings){
        sink->setup(*settings);
    }
}

void aoo_sink::setup(aoo_sink_settings& settings){
    processfn_ = settings.processfn;
    user_ = settings.userdata;
    nchannels_ = settings.nchannels;
    samplerate_ = settings.samplerate;
    blocksize_ = settings.blocksize;
    buffersize_ = std::max<int32_t>(0, settings.buffersize);
    resend_limit_ = std::max<int32_t>(0, settings.resend_limit);
    resend_interval_ = std::max<int32_t>(0, settings.resend_interval);
    resend_maxnumframes_ = std::max<int32_t>(1, settings.resend_maxnumframes);
    resend_packetsize_ = std::max<int32_t>(64, std::min<int32_t>(AOO_MAXPACKETSIZE, settings.resend_packetsize));
    bandwidth_ = std::max<double>(0, std::min<double>(1, settings.time_filter_bandwidth));
    starttime_ = 0; // will update time DLL
    elapsedtime_.reset();

    buffer_.resize(blocksize_ * nchannels_);
    for (auto& src : sources_){
        // don't need to lock
        update_source(src);
    }
}

int32_t aoo_sink_handlemessage(aoo_sink *sink, const char *data, int32_t n,
                            void *src, aoo_replyfn fn) {
    return sink->handle_message(data, n, src, fn);
}

// /AoO/<sink>/format <src> <salt> <numchannels> <samplerate> <blocksize> <codec> <settings...>
// /AoO/<sink>/data <src> <salt> <seq> <sr> <channel_onset> <totalsize> <numpackets> <packetnum> <data>

int32_t aoo_sink::handle_message(const char *data, int32_t n, void *endpoint, aoo_replyfn fn){
    aoo::osc::received_packet packet(data, n);

    if (packet.is_bundle()){
        LOG_WARNING("OSC bundles are not supported (yet)");
        return 0; // ?
    }

    aoo::osc::received_message msg(packet);
    if (!msg.check()){
        LOG_ERROR("received malformed OSC message!");
        return 0; // ?
    }

    if (samplerate_ == 0){
        return 1; // not setup yet
    }

    int32_t sink = 0;
    auto onset = aoo_parsepattern(data, n, &sink);
    if (!onset){
        LOG_WARNING("not an AoO message!");
        return 1; // ?
    }
    if (sink != id_ && sink != AOO_ID_WILDCARD){
        LOG_WARNING("wrong sink ID!");
        return 1; // ?
    }

    if (!strcmp(msg.address_pattern() + onset, AOO_FORMAT)){
        if (msg.count() == AOO_FORMAT_NARGS){
        #if 0
            std::cerr << "received format message:\n";
            for (int i = 0; i < msg.size(); ++i){
                std::cerr << (int)(uint8_t)msg.data()[i] << " ";
            }
            std::cerr << std::endl;
        #endif
            auto it = msg.begin();
            int32_t id = (it++)->as_int32();
            int32_t salt = (it++)->as_int32();
            // get format from arguments
            aoo_format f;
            f.nchannels = (it++)->as_int32();
            f.samplerate = (it++)->as_int32();
            f.blocksize = (it++)->as_int32();
            f.codec = (it++)->as_string();
            auto b = (it++)->as_blob();

            std::cerr << id << " " << salt << " " << f.nchannels << " "
                      << f.samplerate << " " << f.blocksize << " " << f.codec;

            if (!f.codec){
                LOG_ERROR("missing codec argument in /format message!");
                std::terminate();
                return 1; // ?
            }

            handle_format_message(endpoint, fn, id, salt, f, b.data, b.size);
        } else {
            LOG_ERROR("wrong number of arguments for /format message");
        }
    } else if (!strcmp(msg.address_pattern() + onset, AOO_DATA)){
        if (msg.count() == AOO_DATA_NARGS){
        #if 0
            std::cerr << "received data message:\n";
            for (int i = 0; i < msg.size(); ++i){
                std::cerr << (int)(uint8_t)msg.data()[i] << " ";
            }
            std::cerr << std::endl;
        #endif
            auto it = msg.begin();
            // get header from arguments
            auto id = (it++)->as_int32();
            auto salt = (it++)->as_int32();
            aoo::data_packet d;
            d.sequence = (it++)->as_int32();
            d.samplerate = (it++)->as_double();
            d.channel = (it++)->as_int32();
            d.totalsize = (it++)->as_int32();
            d.nframes = (it++)->as_int32();
            d.framenum = (it++)->as_int32();
            auto b = (it++)->as_blob();
            d.data = b.data;
            d.size = b.size;

            handle_data_message(endpoint, fn, id, salt, d);
        } else {
            LOG_ERROR("wrong number of arguments for /data message");
        }
    } else {
        LOG_WARNING("unknown message '" << (msg.address_pattern() + onset) << "'");
    }
    return 1; // ?
}

void aoo_sink::handle_format_message(void *endpoint, aoo_replyfn fn,
                                     int32_t id, int32_t salt, const aoo_format& f,
                                     const char *settings, int32_t size){
    LOG_DEBUG("handle format message");

    auto update_format = [&](aoo::source_desc& src){
        if (!src.decoder || strcmp(src.decoder->name(), f.codec)){
            auto c = aoo::find_codec(f.codec);
            if (c){
                src.decoder = c->create_decoder();
            } else {
                LOG_ERROR("codec '" << f.codec << "' not supported!");
                return;
            }
            if (!src.decoder){
                LOG_ERROR("couldn't create decoder!");
                return;
            }
        }
        src.decoder->read(f.nchannels, f.samplerate, f.blocksize, settings, size);

        update_source(src);
    };

    if (id == AOO_ID_WILDCARD){
        // update all sources from this endpoint
        for (auto& src : sources_){
            if (src.endpoint == endpoint){
                std::unique_lock<std::mutex> lock(mutex_); // !
                src.salt = salt;
                update_format(src);
            }
        }
    } else {
        // try to find existing source
        auto src = std::find_if(sources_.begin(), sources_.end(), [&](auto& s){
            return (s.endpoint == endpoint) && (s.id == id);
        });
        std::unique_lock<std::mutex> lock(mutex_); // !
        if (src == sources_.end()){
            // not found - add new source
            sources_.emplace_back(endpoint, fn, id, salt);
            src = sources_.end() - 1;
        } else {
            src->salt = salt;
        }
        // update source
        update_format(*src);
    }
}

void aoo_sink::handle_data_message(void *endpoint, aoo_replyfn fn, int32_t id,
                                   int32_t salt, const aoo::data_packet& d){
    // first try to find existing source
    auto result = std::find_if(sources_.begin(), sources_.end(), [&](auto& s){
        return (s.endpoint == endpoint) && (s.id == id);
    });
    // check if the 'salt' values match. the source format might have changed and we haven't noticed,
    // e.g. because of dropped UDP packets.
    if (result != sources_.end() && result->salt == salt){
        auto& src = *result;
        auto& queue = src.blockqueue;
        auto& acklist = src.ack_list;
    #if 1
        if (!src.decoder){
            LOG_DEBUG("ignore data message");
            return;
        }
    #else
        assert(src.decoder != nullptr);
    #endif
        LOG_DEBUG("got block: seq = " << d.sequence << ", sr = " << d.samplerate
                  << ", chn = " << d.channel << ", totalsize = " << d.totalsize
                  << ", nframes = " << d.nframes << ", frame = " << d.framenum << ", size " << d.size);

        if (src.next < 0){
            src.next = d.sequence;
        }

        if (d.sequence < src.next){
            // block too old, discard!
            LOG_VERBOSE("discarded old block " << d.sequence);
            return;
        }

        if (d.sequence < src.newest){
            if (acklist.find(d.sequence)){
                LOG_DEBUG("resent block " << d.sequence);
            } else {
                LOG_VERBOSE("block " << d.sequence << " out of order!");
            }
        } else if ((d.sequence - src.newest) > 1){
            LOG_VERBOSE("skipped " << (d.sequence - src.newest - 1) << " blocks");
        }

        if ((d.sequence - src.newest) > queue.capacity()){
            // too large gap between incoming block and most recent block.
            // either network problem or stream has temporarily stopped.
            // clear the block queue and fill audio buffer with zeros.
            queue.clear();
            acklist.clear();
            src.next = d.sequence;
            // push silent blocks to keep the buffer full, but leave room for one block!
            int count = 0;
            auto nsamples = src.audioqueue.blocksize();
            while (src.audioqueue.write_available() > 1 && src.infoqueue.write_available() > 1){
                auto ptr = src.audioqueue.write_data();
                for (int i = 0; i < nsamples; ++i){
                    ptr[i] = 0;
                }
                src.audioqueue.write_commit();
                // push nominal samplerate + default channel (0)
                aoo::source_desc::info i;
                i.sr = src.decoder->samplerate();
                i.channel = 0;
                i.state = AOO_SOURCE_STOP;
                src.infoqueue.write(i);

                count++;
            }
            LOG_VERBOSE("wrote " << count << " silent blocks for transmission gap");
        }
        auto block = queue.find(d.sequence);
        if (!block){
            if (queue.full()){
                // if the queue is full, we have to drop a block;
                // in this case we send a block of zeros to the audio buffer
                auto nsamples = src.audioqueue.blocksize();
                if (src.audioqueue.write_available() && src.infoqueue.write_available()){
                    auto ptr = src.audioqueue.write_data();
                    for (int i = 0; i < nsamples; ++i){
                        ptr[i] = 0;
                    }
                    src.audioqueue.write_commit();
                    // push nominal samplerate + default channel (0)
                    aoo::source_desc::info i;
                    i.sr = src.decoder->samplerate();
                    i.channel = 0;
                    i.state = AOO_SOURCE_STOP;
                    src.infoqueue.write(i);
                }
                LOG_VERBOSE("dropped block " << queue.front().sequence);
                // remove block from acklist
                acklist.remove(queue.front().sequence);
            }
            // add new block
            block = queue.insert(d.sequence, d.samplerate, d.channel, d.totalsize, d.nframes);
        } else if (block->has_frame(d.framenum)){
            LOG_VERBOSE("frame " << d.framenum << " of block " << d.sequence << " already received!");
            return;
        }

        // add frame to block
        block->add_frame(d.framenum, (const char *)d.data, d.size);

    #if 1
        if (block->complete()){
            // remove block from acklist as early as possible
            acklist.remove(block->sequence);
        }
    #endif

        // update newest sequence number
        if (d.sequence > src.newest){
            src.newest = d.sequence;
        }

        // Transfer all consecutive complete blocks as long as
        // no previous (expected) blocks are missing.
        if (!queue.empty()){
            block = queue.begin();
            int32_t count = 0;
            int32_t next = src.next;
            while ((block != queue.end()) && block->complete()
                   && (block->sequence == next)
                   && src.audioqueue.write_available() && src.infoqueue.write_available())
            {
                LOG_DEBUG("write samples (" << block->sequence << ")");

                auto ptr = src.audioqueue.write_data();
                auto nsamples = src.audioqueue.blocksize();
                assert(block->data() != nullptr && block->size() > 0 && ptr != nullptr && nsamples > 0);
                if (src.decoder->decode(block->data(), block->size(), ptr, nsamples) <= 0){
                    LOG_VERBOSE("bad block: size = " << block->size() << ", nsamples = " << nsamples);
                    // decoder failed - fill with zeros
                    std::fill(ptr, ptr + nsamples, 0);
                }

                src.audioqueue.write_commit();

                // push info
                aoo::source_desc::info i;
                i.sr = block->samplerate;
                i.channel = block->channel;
                i.state = AOO_SOURCE_PLAY;
                src.infoqueue.write(i);

                next++;
                count++;
                block++;
            }
            src.next = next;
            // pop blocks
            while (count--){
            #if 0
                // remove block from acklist
                acklist.remove(queue.front().sequence);
            #endif
                // pop block
                LOG_DEBUG("pop block " << queue.front().sequence);
                queue.pop_front();
            }
            LOG_DEBUG("next: " << src.next);
        }

    #if 1
        // pop outdated blocks (shouldn't really happen...)
        while (!queue.empty() &&
               (src.newest - queue.front().sequence) >= queue.capacity())
        {
            auto old = queue.front().sequence;
            LOG_VERBOSE("pop outdated block " << old);
            // remove block from acklist
            acklist.remove(old);
            // pop block
            queue.pop_front();
            // update 'next'
            if (src.next <= old){
                src.next = old + 1;
            }
        }
    #endif

        // deal with "holes" in block queue
        if (!queue.empty()){
        #if LOGLEVEL >= 3
            std::cerr << queue << std::endl;
        #endif
            int32_t numframes = 0;
            retransmit_list_.clear();

            // resend incomplete blocks except for the last block
            LOG_DEBUG("resend incomplete blocks");
            for (auto it = queue.begin(); it != (queue.end() - 1); ++it){
                if (!it->complete()){
                    // insert ack (if needed)
                    auto& ack = acklist.get(it->sequence);
                    if (ack.check(elapsedtime_.get(), resend_interval_ * 0.001)){
                        for (int i = 0; i < it->num_frames(); ++i){
                            if (!it->has_frame(i)){
                                if (numframes < resend_maxnumframes_){
                                    retransmit_list_.push_back(data_request { it->sequence, i });
                                    numframes++;
                                } else {
                                    goto resend_incomplete_done;
                                }
                            }
                        }
                    }
                }
            }
            resend_incomplete_done:

            // resend missing blocks before any (half)completed blocks
            LOG_DEBUG("resend missing blocks");
            int32_t next = src.next;
            for (auto it = queue.begin(); it != queue.end(); ++it){
                auto missing = it->sequence - next;
                if (missing > 0){
                    for (int i = 0; i < missing; ++i){
                        // insert ack (if necessary)
                        auto& ack = acklist.get(next + i);
                        if (ack.check(elapsedtime_.get(), resend_interval_ * 0.001)){
                            if (numframes + it->num_frames() <= resend_maxnumframes_){
                                retransmit_list_.push_back(data_request { next + i, -1 }); // whole block
                                numframes += it->num_frames();
                            } else {
                                goto resend_missing_done;
                            }
                        }
                    }
                } else if (missing < 0){
                    LOG_VERBOSE("bug: sequence = " << it->sequence << ", next = " << next);
                    assert(false);
                }
                next = it->sequence + 1;
            }
            resend_missing_done:

            assert(numframes <= resend_maxnumframes_);
            if (numframes > 0){
                LOG_DEBUG("requested " << numframes << " frames");
            }

            // request data
            request_data(*result);

        #if 1
            // clean ack list
            auto removed = acklist.remove_before(src.next);
            if (removed > 0){
                LOG_DEBUG("block_ack_list: removed " << removed << " outdated items");
            }
        #endif
        } else {
            if (!acklist.empty()){
                LOG_WARNING("bug: acklist not empty");
                acklist.clear();
            }
        }
    #if LOGLEVEL >= 3
        std::cerr << acklist << std::endl;
    #endif
    } else {
        // discard data and request format!
        request_format(endpoint, fn, id);
    }
}

void aoo_sink::update_source(aoo::source_desc &src){
    // resize audio ring buffer
    if (src.decoder && src.decoder->blocksize() > 0 && src.decoder->samplerate() > 0){
        LOG_DEBUG("update source");
        // recalculate buffersize from ms to samples
        double bufsize = (double)buffersize_ * src.decoder->samplerate() * 0.001;
        auto d = div(bufsize, src.decoder->blocksize());
        int32_t nbuffers = d.quot + (d.rem != 0); // round up
        nbuffers = std::max<int32_t>(1, nbuffers); // e.g. if buffersize_ is 0
        // resize audio buffer and initially fill with zeros.
        auto nsamples = src.decoder->nchannels() * src.decoder->blocksize();
        src.audioqueue.resize(nbuffers * nsamples, nsamples);
        src.infoqueue.resize(nbuffers, 1);
        while (src.audioqueue.write_available() && src.infoqueue.write_available()){
            LOG_VERBOSE("write silent block");
            src.audioqueue.write_commit();
            // push nominal samplerate + default channel (0)
            aoo::source_desc::info i;
            i.sr = src.decoder->samplerate();
            i.channel = 0;
            i.state = AOO_SOURCE_STOP;
            src.infoqueue.write(i);
        };
        // setup resampler
        src.resampler.setup(src.decoder->blocksize(), blocksize_,
                            src.decoder->samplerate(), samplerate_, src.decoder->nchannels());
        // resize block queue
        src.blockqueue.resize(nbuffers);
        src.newest = 0;
        src.next = -1;
        src.channel = 0;
        src.samplerate = src.decoder->samplerate();
        src.ack_list.setup(resend_limit_);
        src.ack_list.clear();
        LOG_VERBOSE("update source " << src.id << ": sr = " << src.decoder->samplerate()
                    << ", blocksize = " << src.decoder->blocksize() << ", nchannels = "
                    << src.decoder->nchannels() << ", bufsize = " << nbuffers * nsamples);
    }
}

void aoo_sink::request_format(void *endpoint, aoo_replyfn fn, int32_t id){
    LOG_DEBUG("request format");
    char buf[AOO_MAXPACKETSIZE];
    aoo::osc::message_builder msg(buf, sizeof(buf));

    // make OSC address pattern
    const int32_t max_addr_size = sizeof(AOO_DOMAIN) + 16 + sizeof(AOO_REQUEST);
    char address[max_addr_size];
    snprintf(address, sizeof(address), "%s/%d%s", AOO_DOMAIN, id, AOO_REQUEST);

    msg.set(address, id_);

    fn(endpoint, msg.data(), msg.size());
}

void aoo_sink::request_data(aoo::source_desc& src){
    char buf[AOO_MAXPACKETSIZE];
    aoo::osc::message_builder msg(buf, sizeof(buf));

    // make OSC address pattern
    const int32_t maxaddrsize = sizeof(AOO_DOMAIN) + 16 + sizeof(AOO_RESEND);
    char address[maxaddrsize];
    auto addrsize = snprintf(address, sizeof(address), "%s/%d%s", AOO_DOMAIN, src.id, AOO_RESEND);

    const int32_t maxdatasize = resend_packetsize_ - maxaddrsize - 16;
    const int32_t maxrequests = maxdatasize / 10; // 2 * int32_t + overhead for typetags
    auto d = div(retransmit_list_.size(), maxrequests);

    auto dorequest = [&](const data_request* data, int32_t n){
        msg.set_address(address, addrsize);

        int ntags = n * 2 + 2;
        auto typetags = (char *)alloca(ntags);
        memset(typetags, 'i', ntags);
        msg.set_args(typetags, ntags);

        msg << id_ << src.salt;
        for (int i = 0; i < n; ++i){
            msg << data[i].sequence << data[i].frame;
        }

        src.send(msg.data(), msg.size());
    };

    for (int i = 0; i < d.quot; ++i){
        dorequest(retransmit_list_.data() + i * maxrequests, maxrequests);
    }
    if (d.rem > 0){
        dorequest(retransmit_list_.data() + retransmit_list_.size() - d.rem, d.rem);
    }
}

#if AOO_DEBUG_RESAMPLING
thread_local int32_t debug_counter = 0;
#endif

int32_t aoo_sink_process(aoo_sink *sink, uint64_t t) {
    return sink->process(t);
}

#define AOO_MAXNUMEVENTS 256

int32_t aoo_sink::process(uint64_t t){
    if (!processfn_){
        return 0;
    }
    std::fill(buffer_.begin(), buffer_.end(), 0);

    bool didsomething = false;

    // update time DLL
    aoo::time_tag tt(t);
    if (starttime_ == 0){
        starttime_ = tt.to_double();
        LOG_VERBOSE("setup time DLL for sink");
        dll_.setup(samplerate_, blocksize_, bandwidth_, 0);
    } else {
        auto elapsed = tt.to_double() - starttime_;
        dll_.update(elapsed);
    #if AOO_DEBUG_DLL
        DO_LOG("SINK");
        DO_LOG("elapsed: " << elapsed << ", period: " << dll_.period()
               << ", samplerate: " << dll_.samplerate());
    #endif
        elapsedtime_.set(elapsed);
    }

    // pre-allocate event array (max. 1 per source)
    aoo_event *events = (aoo_event *)alloca(sizeof(aoo_event) * AOO_MAXNUMEVENTS);
    size_t numevents = 0;

    // the mutex is uncontended most of the time, but LATER we might replace
    // this with a lockless and/or waitfree solution
    std::unique_lock<std::mutex> lock(mutex_);
    for (auto& src : sources_){
        if (!src.decoder){
            continue;
        }
        double sr = src.decoder->samplerate();
        int32_t nchannels = src.decoder->nchannels();
        int32_t nsamples = src.audioqueue.blocksize();
        // write samples into resampler
        while (src.audioqueue.read_available() && src.infoqueue.read_available()
               && src.resampler.write_available() >= nsamples){
        #if AOO_DEBUG_RESAMPLING
            if (debug_counter == 0){
                DO_LOG("read available: " << src.audioqueue.read_available());
            }
        #endif
            auto info = src.infoqueue.read();
            src.channel = info.channel;
            src.samplerate = info.sr;
            src.resampler.write(src.audioqueue.read_data(), nsamples);
            src.audioqueue.read_commit();
            // check state
            if (info.state != src.laststate && numevents < AOO_MAXNUMEVENTS){
                aoo_event& event = events[numevents++];
                event.source_state.type = AOO_SOURCE_STATE_EVENT;
                event.source_state.endpoint = src.endpoint;
                event.source_state.id = src.id;
                event.source_state.state = info.state;

                src.laststate = info.state;
            }
        }
        // update resampler
        src.resampler.update(src.samplerate, dll_.samplerate());
        // read samples from resampler
        auto readsamples = blocksize_ * nchannels;
        if (src.resampler.read_available() >= readsamples){
            auto buf = (aoo_sample *)alloca(readsamples * sizeof(aoo_sample));
            src.resampler.read(buf, readsamples);

            // sum source into sink (interleaved -> non-interleaved),
            // starting at the desired sink channel offset.
            // out of bound source channels are silently ignored.
            for (int i = 0; i < nchannels; ++i){
                auto chn = i + src.channel;
                // ignore out-of-bound source channels!
                if (chn < nchannels_){
                    for (int j = 0; j < blocksize_; ++j){
                        buffer_[chn * blocksize_ + j] += buf[j * nchannels + i];
                    }
                }
            }
            LOG_DEBUG("read samples");
            didsomething = true;
        } else {
            // buffer ran out -> send "stop" event
            if (src.laststate != AOO_SOURCE_STOP && numevents < AOO_MAXNUMEVENTS){
                aoo_event& event = events[numevents++];
                event.source_state.type = AOO_SOURCE_STATE_EVENT;
                event.source_state.endpoint = src.endpoint;
                event.source_state.id = src.id;
                event.source_state.state = AOO_SOURCE_STOP;

                src.laststate = AOO_SOURCE_STOP;
                didsomething = true;
            }
        }
    }
    lock.unlock();

    if (didsomething){
    #if AOO_CLIP_OUTPUT
        for (auto it = buffer_.begin(); it != buffer_.end(); ++it){
            if (*it > 1.0){
                *it = 1.0;
            } else if (*it < -1.0){
                *it = -1.0;
            }
        }
    #endif
        // set buffer pointers and pass to audio callback
        auto vec = (const aoo_sample **)alloca(sizeof(aoo_sample *) * nchannels_);
        for (int i = 0; i < nchannels_; ++i){
            vec[i] = &buffer_[i * blocksize_];
        }
        processfn_(user_, vec, blocksize_, events, numevents);
        return 1;
    } else {
        return 0;
    }
}
