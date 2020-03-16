#pragma once

#include "aoo_osc_types.hpp"

#include <cstring>
#include <string>
#include <type_traits>

namespace aoo {
namespace osc {

namespace detail {

inline constexpr int32_t roundup(int32_t i){
    return (i + 3) & ~3;
}

inline int32_t strlen_safe(const char *s, const char *end){
    auto it = s;
    while (*it++) {
        if (it == end){
            return -1;
        }
    }
    return it - s - 1;
}

inline const char * skipstr(const char *s, const char *end){
    auto it = s;
    while (*it++) {
        if (it == end){
            return end;
        }
    };
    return s + roundup(it - s);
}

inline int32_t writebytes(const char *s, int32_t n, char *buf, int32_t size){
    auto totalsize = roundup(n);
    if (totalsize > size){
        return -1;
    }
    memset(buf + totalsize - 4, 0, 4); // first zero pad the last four bytes
    memcpy(buf, s, n); // now copy string
    return totalsize;
}

inline const char * skipblob(const char *b, const char *end){
    auto n = from_bytes<int32_t>(b);
    auto out = b + n + 4;
    return out < end ? out : end;
}

inline bool tag_isnumeric(char t){
    switch(t){
    case 'i':
    case 'h':
    case 'f':
    case 'd':
    case 'c':
        return true;
    default:
        return false;
    }
}

} // detail

/*//////////////////////////// arg_iterator ///////////////////////////////*/

inline const char * arg_iterator::as_string(const char *def) const {
    switch (*typetag_){
    case 's':
    case 'S':
    #if 1
        if (detail::strlen_safe(data_, end_) < 0){
            return def;
        }
    #endif
        return data_;
    default:
        return def;
    }
}

inline blob arg_iterator::as_blob() const {
    if (*typetag_ == 'b'){
        int size = from_bytes<int32_t>(data_);
        auto data = data_ + 4;
    #if 1
        int limit = (end_ - data);
        if (size > limit){
            size = limit;
        }
    #endif
        return blob(data, size);
    } else {
        return blob();
    }
}

inline uint64_t arg_iterator::as_timetag(uint64_t def) const {
    if (*typetag_ == 't'){
        return from_bytes<uint64_t>(data_);
    } else {
        return def;
    }
}

inline rgba arg_iterator::as_rgba(rgba def) const {
    if (*typetag_ == 'r'){
        return rgba(data_);
    } else {
        return def;
    }
}

inline midi arg_iterator::as_midi(midi def) const {
    if (*typetag_ == 'm'){
        return midi(data_);
    } else {
        return def;
    }
}

template<typename T>
inline T arg_iterator::as_number(T def) const {
    switch(*typetag_){
    case 'i':
        return from_bytes<int32_t>(data_);
    case 'f':
        return from_bytes<float>(data_);
    case 'h':
        return from_bytes<int64_t>(data_);
    case 'd':
        return from_bytes<double>(data_);
#if 0
    case 'r':
    case 'm':
    case 'c':
        return from_bytes<uint32_t>(data_);
    case 't':
        return from_bytes<uint64_t>(data_);
#endif
    default:
        return def;
    }
}

inline void arg_iterator::advance() {
    switch(*typetag_++){
    case 'i':
    case 'f':
    case 'r':
    case 'm':
    case 'c':
        data_ += 4;
        break;
    case 'h':
    case 'd':
    case 't':
        data_ += 8;
        break;
    case 's':
    case 'S':
        data_ = detail::skipstr(data_, end_);
        break;
    case 'b':
        data_ = detail::skipblob(data_, end_);
        break;
    case 'T':
    case 'F':
    case 'N':
    case 'I':
        break;
    default:
        // end or not supported
        data_ = end_;
        break;
    }
}

/*///////////////////////// received_packet /////////////////////////*/

inline received_packet::received_packet(const char *data, int32_t size)
    : data_(data), size_(size){
    if (!std::strncmp("#bundle", data_, size_)){
        type_ = type::bundle;
    } else {
        type_ = type::message;
    }
}

/*///////////////////////// received_message //////////////////////////*/

inline received_message::received_message(const received_packet& packet)
{
    auto data = packet.data();
    auto size = packet.size();
    // packet size must be multiple of 4!
    if ((size & 3) != 0){
        LOG_ERROR("OSC message size must be multiple of 4!");
        goto fail;
    }
    address_pattern_ = data;
    end_ = data + size;
    type_tags_ = detail::skipstr(address_pattern_, end_);
    if (type_tags_ != end_){
        auto len = detail::strlen_safe(type_tags_, end_);
        if (len < 1){
            LOG_ERROR("bad OSC type tag string!");
            goto fail; // bad type tag string
        }
        nargs_ = len - 1; // ignore leading ','
        data_ = type_tags_ + detail::roundup(len + 1); // include null terminator
    #if 1
        if (nargs_ > 0 && data_ == end_){
            // missing data
            LOG_ERROR("not enough data in OSC message!");
            goto fail;
        }
    #endif
    } else {
        // missing typetags (old OSC implementations)
        LOG_WARNING("OSC message without type tag string!");
        nargs_ = 0;
        data_ = end_;
    }
    return;
fail:
    address_pattern_ = nullptr;
    type_tags_ = nullptr;
    data_ = nullptr;
    end_ = nullptr;
    nargs_ = 0;
}

/*/////////////////////// message_builder //////////////////////*/

inline message_builder::message_builder(char *buffer, int32_t size)
    : buffer_(buffer), end_(buffer + size), tag_(nullptr), data_(buffer){}

namespace detail {

template<typename T>
char get_tag() = delete;

// specializations
template<> inline char get_tag<char>() { return 'c'; }
template<> inline char get_tag<int32_t>() { return 'i'; }
template<> inline char get_tag<int64_t>() { return 'h'; }
template<> inline char get_tag<float>() { return 'f'; }
template<> inline char get_tag<double>() { return 'd'; }
template<> inline char get_tag<uint64_t>() { return 't'; }
template<> inline char get_tag<const char *>() { return 's'; }
template<> inline char get_tag<std::string>() { return 's'; }
template<> inline char get_tag<rgba>() { return 'r'; }
template<> inline char get_tag<midi>() { return 'm'; }
template<> inline char get_tag<blob>() { return 'b'; }
template<> inline char get_tag<true_tag>() { return 'T'; }
template<> inline char get_tag<false_tag>() { return 'F'; }
template<> inline char get_tag<nil_tag>() { return 'N'; }
template<> inline char get_tag<inf_tag>() { return 'I'; }

}

inline bool message_builder::check_space(int32_t n){
    if ((end_ - data_) >= n){
        return true;
    } else {
        invalidate();
        return false;
    }
}

inline void message_builder::invalidate(){
    LOG_ERROR("invalidate!");
    data_ = buffer_;
    tag_ = buffer_;
    *tag_ = 0;
}

template<typename T, typename... U>
inline void message_builder::make_typetags(char *tags, T&& arg, U&&... args){
    *tags = detail::get_tag<typename std::decay<T>::type>();

    make_typetags(tags + 1, std::forward<U>(args)...);
}

template<typename T, typename... U>
inline void message_builder::add_args(T&& arg, U&&... args){
    *this << std::forward<T>(arg);

    add_args(std::forward<U>(args)...);
}

template<typename T, typename... U>
inline void message_builder::set(const T& address, U&&... args){
    set_address(address);

    if (!tag_){
        invalidate();
        return;
    }

    const size_t nargs = sizeof...(args);
    const size_t size = detail::roundup(nargs + 2); // include ',' and '0'
    if ((tag_ + size) > end_){
        invalidate();
        return;
    }
    memset(tag_ + size - 4, 0, 4); // zero pad last 4 bytes
    *tag_ = ',';
    make_typetags(tag_ + 1, std::forward<U>(args)...);
    data_ = tag_ + size;
    tag_++; // skip ','

    add_args(std::forward<U>(args)...);
}

inline void message_builder::set_address(const char *s){
    set_address(s, strlen(s));
}

inline void message_builder::set_address(const std::string& s){
    set_address(s.data(), s.size());
}

inline void message_builder::set_address(const char *s, int32_t n){
    data_ = buffer_;
    // include null terminator!
    auto len = detail::writebytes(s, n + 1, buffer_, end_ - buffer_);
    if (len > 0){
        tag_ = buffer_ + len;
        if (tag_ + 4 <= end_){
            // temporary empty type tag
            memcpy(tag_, ",\0\0\0", 4);
            data_ = tag_ + 4;
            // don't skip ',' because we will overwrite type tags
            return;
        }
    }
    // error
    tag_ = nullptr;
}

inline void message_builder::set_args(const char *tags){
    set_args(tags, strlen(tags));
}

inline void message_builder::set_args(const std::string& tags){
    set_args(tags.data(), tags.size());
}

inline void message_builder::set_args(const char *tags, int32_t n){
    if (!tag_){
        invalidate();
        return;
    }

    auto size = detail::roundup(n + 2); // include ',' and '0'
    if ((tag_ + size) > end_){
        invalidate();
        return;
    }
    *tag_ = ',';
    memset(tag_ + size - 4, 0, 4); // first zero pad the last four bytes
    memcpy(tag_ + 1, tags, n); // now copy tags
    data_ = tag_ + size;
    tag_++; // skip ','
}

template<typename T>
message_builder& message_builder::operator <<(T t){
    static_assert(std::is_arithmetic<T>::value, "type is not arithmetic");
    switch (*tag_++){
    case 'c':
    case 'i':
        if (!check_space(4)) return *this;
        to_bytes((int32_t)t, data_);
        data_ += 4;
        break;
    case 'h':
        if (!check_space(8)) return *this;
        to_bytes((int64_t)t, data_);
        data_ += 8;
        break;
    case 'f':
        if (!check_space(4)) return *this;
        to_bytes((float)t, data_);
        data_ += 4;
        break;
    case 'd':
        if (!check_space(8)) return *this;
        to_bytes((double)t, data_);
        data_ += 8;
        break;
    default:
        invalidate();
        break;
    }
    return *this;
}

inline message_builder& message_builder::operator <<(const char *s){
    return add_string(s, strlen(s));
}

inline message_builder& message_builder::operator <<(const std::string& s){
    return add_string(s.data(), s.size());
}

inline message_builder& message_builder::add_string(const char *s, int32_t n){
    switch (*tag_++){
    case 's':
    case 'S':
    {
        auto size = detail::writebytes(s, n + 1, data_, end_ - data_);
        if (size > 0){
            data_ += size;
        } else {
            invalidate();
        }
        break;
    }
    default:
        invalidate();
    }
    return *this;
}

inline message_builder& message_builder::operator <<(timetag t){
    if (!check_space(8)) return *this;
    if (*tag_++ == 't'){
        to_bytes(t, data_);
        data_ += 8;
    } else {
        invalidate();
    }
    return *this;
}

inline message_builder& message_builder::operator <<(blob b){
    if (*tag_++ == 'b' && (data_ + 4) <= end_){
        if (b.size > 0){
            auto n = detail::writebytes(b.data, b.size, data_ + 4, end_ - data_ - 4);
            if (n > 0){
                to_bytes((int32_t)b.size, data_);
                data_ += n + 4;
            } else {
                invalidate();
            }
        } else {
            // empty blob (is this even allowed?)
            to_bytes((int32_t)0, data_);
            data_ += 4;
        }
    } else {
        invalidate();
    }
    return *this;
}

inline message_builder& message_builder::operator <<(midi m){
    if (!check_space(4)) return *this;
    if (*tag_++ == 'm'){
        memcpy(data_, m.to_bytes(), 4);
        data_ += 4;
    } else {
        invalidate();
    }
    return *this;
}

inline message_builder& message_builder::operator <<(rgba c){
    if (!check_space(4)) return *this;
    if (*tag_++ == 'r'){
        memcpy(data_, c.to_bytes(), 4);
        data_ += 4;
    } else {
        invalidate();
    }
    return *this;
}

inline message_builder& message_builder::operator <<(true_tag t){
    if (*tag_++ != 'T'){
        invalidate();
    }
    return *this;
}

inline message_builder& message_builder::operator <<(false_tag f){
    if (*tag_++ != 'F'){
        invalidate();
    }
    return *this;
}

inline message_builder& message_builder::operator <<(nil_tag n){
    if (*tag_++ != 'N'){
        invalidate();
    }
    return *this;
}

inline message_builder& message_builder::operator <<(inf_tag i){
    if (*tag_++ != 'I'){
        invalidate();
    }
    return *this;
}

} // osc
} // aoo
