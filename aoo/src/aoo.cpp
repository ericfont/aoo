#include "aoo/aoo.h"
#if USE_AOO_NET
#include "aoo/aoo_net.h"
#include "common/net_utils.hpp"
#endif

#include "imp.hpp"

#include "common/time.hpp"
#include "common/utils.hpp"

#include "aoo/codec/aoo_pcm.h"
#if USE_CODEC_OPUS
#include "aoo/codec/aoo_opus.h"
#endif

#include <iostream>
#include <sstream>
#include <atomic>

namespace aoo {

/*//////////////////// helper /////////////////////////*/

char * copy_string(const char * s){
    if (s){
        auto len = strlen(s);
        auto result = aoo::allocate(len + 1);
        memcpy(result, s, len + 1);
        return (char *)result;
    } else {
        return nullptr;
    }
}

void free_string(char *s){
    if (s){
        auto len = strlen(s);
        aoo::deallocate(s, len + 1);
    }
}

void * copy_sockaddr(const void *sa, int32_t len){
    if (sa){
        auto result = aoo::allocate(len);
        memcpy(result, sa, len);
        return result;
    } else {
        return nullptr;
    }
}

void free_sockaddr(void *sa, int32_t len){
    if (sa){
        aoo::deallocate(sa, len);
    }
}

} // aoo

/*//////////////////// allocator /////////////////////*/

#if AOO_USE_ALLOCATOR

#ifndef AOO_DEBUG_MEMORY
#define AOO_DEBUG_MEMORY 0
#endif

namespace aoo {

static aoo_allocator g_allocator {
    [](size_t n, void *){ return malloc(n); },
    nullptr,
    [](void *ptr, size_t, void *){ free(ptr); },
    nullptr
};


#if AOO_DEBUG_MEMORY
std::atomic<int64_t> total_memory{0};
#endif

void * allocate(size_t size){
#if AOO_DEBUG_MEMORY
    auto total = total_memory.fetch_add(size, std::memory_order_relaxed) + size;
    fprintf(stderr, "allocate %d bytes (total: %d)\n", size, total);
    fflush(stderr);
#endif
    return g_allocator.alloc(size, g_allocator.context);
}

void deallocate(void *ptr, size_t size){
#if AOO_DEBUG_MEMORY
    auto total = total_memory.fetch_sub(size, std::memory_order_relaxed) - size;
    fprintf(stderr, "deallocate %d bytes (total: %d)\n", size, total);
    fflush(stderr);
#endif
    return g_allocator.free(ptr, size, g_allocator.context);
}

} // aoo

void aoo_set_allocator(const aoo_allocator *alloc){
    aoo::g_allocator = *alloc;
}

#endif

/*//////////////////// Log ////////////////////////////*/

static aoo_logfunction gLogFunction = nullptr;

void aoo_set_logfunction(aoo_logfunction f){
    gLogFunction = f;
}

static const char *errmsg[] = {

};

const char *aoo_error_string(aoo_error e){
    return "unknown error"; // TODO
}

namespace aoo {

Log::~Log(){
    stream_ << "\n";
    std::string msg = stream_.str();
    if (gLogFunction){
        gLogFunction(msg.c_str());
    } else {
        std::cerr << msg;
        std::flush(std::cerr);
    }
}

}

/*//////////////////// OSC ////////////////////////////*/

int32_t aoo_parse_pattern(const char *msg, int32_t n,
                          aoo_type *type, aoo_id *id)
{
    int32_t offset = 0;
    if (n >= AOO_MSG_DOMAIN_LEN
        && !memcmp(msg, AOO_MSG_DOMAIN, AOO_MSG_DOMAIN_LEN))
    {
        offset += AOO_MSG_DOMAIN_LEN;
        if (n >= (offset + AOO_MSG_SOURCE_LEN)
            && !memcmp(msg + offset, AOO_MSG_SOURCE, AOO_MSG_SOURCE_LEN))
        {
            *type = AOO_TYPE_SOURCE;
            offset += AOO_MSG_SOURCE_LEN;
        } else if (n >= (offset + AOO_MSG_SINK_LEN)
            && !memcmp(msg + offset, AOO_MSG_SINK, AOO_MSG_SINK_LEN))
        {
            *type = AOO_TYPE_SINK;
            offset += AOO_MSG_SINK_LEN;
    #if USE_AOO_NET
        } else if (n >= (offset + AOO_NET_MSG_CLIENT_LEN)
            && !memcmp(msg + offset, AOO_NET_MSG_CLIENT, AOO_NET_MSG_CLIENT_LEN))
        {
            *type = AOO_TYPE_CLIENT;
            return offset + AOO_NET_MSG_CLIENT_LEN;
        } else if (n >= (offset + AOO_NET_MSG_SERVER_LEN)
            && !memcmp(msg + offset, AOO_NET_MSG_SERVER, AOO_NET_MSG_SERVER_LEN))
        {
            *type = AOO_TYPE_SERVER;
            return offset + AOO_NET_MSG_SERVER_LEN;
        } else if (n >= (offset + AOO_NET_MSG_PEER_LEN)
            && !memcmp(msg + offset, AOO_NET_MSG_PEER, AOO_NET_MSG_PEER_LEN))
        {
            *type = AOO_TYPE_PEER;
            return offset + AOO_NET_MSG_PEER_LEN;
        } else if (n >= (offset + AOO_NET_MSG_RELAY_LEN)
            && !memcmp(msg + offset, AOO_NET_MSG_RELAY, AOO_NET_MSG_RELAY_LEN))
        {
            *type = AOO_TYPE_RELAY;
            return offset + AOO_NET_MSG_RELAY_LEN;
    #endif
        } else {
            return 0;
        }

        if (!id){
            return offset;
        }
        int32_t skip = 0;
        if (sscanf(msg + offset, "/%d%n", id, &skip) > 0){
            return offset + skip;
        } else {
            // TODO only print relevant part of OSC address string
            LOG_ERROR("aoo_parse_pattern: bad ID " << (msg + offset));
            return 0;
        }
    } else {
        return 0; // not an AoO message
    }
}

// OSC time stamp (NTP time)
uint64_t aoo_osctime_now(void){
    return aoo::time_tag::now();
}

double aoo_osctime_to_seconds(uint64_t t){
    return aoo::time_tag(t).to_seconds();
}

uint64_t aoo_osctime_from_seconds(double s){
    return aoo::time_tag::from_seconds(s);
}

double aoo_osctime_duration(uint64_t t1, uint64_t t2){
    return aoo::time_tag::duration(t1, t2);
}

/*/////////////// version ////////////////////*/

void aoo_version(int32_t *major, int32_t *minor,
                 int32_t *patch, int32_t *pre){
    if (major) *major = AOO_VERSION_MAJOR;
    if (minor) *minor = AOO_VERSION_MINOR;
    if (patch) *patch = AOO_VERSION_PATCH;
    if (pre) *pre = AOO_VERSION_PRERELEASE;
}

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

const char *aoo_version_string(){
    return STR(AOO_VERSION_MAJOR) "." STR(AOO_VERSION_MINOR)
    #if AOO_VERSION_PATCH > 0
        "." STR(AOO_VERSION_PATCH)
    #endif
    #if AOO_VERSION_PRERELEASE > 0
       "-pre" STR(AOO_VERSION_PRERELEASE)
    #endif
        ;
}

namespace aoo {

bool check_version(uint32_t version){
    auto major = (version >> 24) & 255;
    auto minor = (version >> 16) & 255;
    auto bugfix = (version >> 8) & 255;

    if (major != AOO_VERSION_MAJOR){
        return false;
    }

    return true;
}

uint32_t make_version(){
    // make version: major, minor, bugfix, [protocol]
    return ((uint32_t)AOO_VERSION_MAJOR << 24) | ((uint32_t)AOO_VERSION_MINOR << 16)
            | ((uint32_t)AOO_VERSION_PATCH << 8);
}

} // aoo

/*/////////////// (de)initialize //////////////////*/

void aoo_codec_pcm_setup(aoo_codec_registerfn fn, const aoo_allocator *alloc);
#if USE_CODEC_OPUS
void aoo_codec_opus_setup(aoo_codec_registerfn fn, const aoo_allocator *alloc);
#endif

#if AOO_USE_ALLOCATOR
#define ALLOCATOR &aoo::g_allocator
#else
#define ALLOCATOR nullptr
#endif

void aoo_initialize(){
    static bool initialized = false;
    if (!initialized){
    #if USE_AOO_NET
        aoo::socket_init();
    #endif

        // register codecs
        aoo_codec_pcm_setup(aoo_register_codec, ALLOCATOR);

    #if USE_CODEC_OPUS
        aoo_codec_opus_setup(aoo_register_codec, ALLOCATOR);
    #endif

        initialized = true;
    }
}

void aoo_terminate() {}


