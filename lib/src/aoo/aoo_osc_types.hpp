#pragma once

#include "aoo_utils.hpp"

namespace aoo {
namespace osc {

namespace detail {

constexpr int32_t roundup(int32_t i);

int32_t strlen_safe(const char *s, const char *end);

const char * skipstr(const char *s, const char *end);

int32_t writebytes(const char *s, int32_t n, char *buf, int32_t size);

const char * skipblob(const char *b, const char *end);

bool tag_isnumeric(char t);

} // detail

struct true_tag {};

struct false_tag {};

struct nil_tag {};

struct inf_tag {};

using timetag = uint64_t;

struct midi {
    midi(const char *d)
        : id(d[0]), status(d[1]), data1(d[2]), data2(d[3]){}
    midi(uint8_t _id = 0, uint8_t _status = 0,
         uint8_t _data1 = 0, uint8_t _data2 = 0)
        : id(_id), status(_status), data1(_data1), data2(_data2){}

    uint8_t id;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;

    uint32_t to_int() const {
        return from_bytes<uint32_t>(reinterpret_cast<const char *>(this));
    }
    const uint8_t *to_bytes() const {
        return reinterpret_cast<const uint8_t *>(this);
    }
};

struct rgba {
    rgba(const char *d)
        : r(d[0]), g(d[1]), b(d[2]), a(d[3]){}
    rgba(uint8_t _r = 0, uint8_t _g = 0,
         uint8_t _b = 0, uint8_t _a = 0)
        : r(_r), g(_g), b(_b), a(_a){}

    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;

    uint32_t to_int() const {
        return from_bytes<uint32_t>(reinterpret_cast<const char *>(this));
    }
    const uint8_t *to_bytes() const {
        return reinterpret_cast<const uint8_t *>(this);
    }
};

struct blob {
    blob(const char *_data = nullptr, int32_t _size = 0)
        : data(_data), size(_size){}

    const char *data;
    int32_t size;
};

class arg_iterator {
public:
    arg_iterator(const char *typetag, const char *data, const char *end)
        : typetag_(typetag), data_(data), end_(end){}
    arg_iterator(const char *end)
        : typetag_(nullptr), data_(end), end_(end){}

    arg_iterator& operator++(){
        advance();
        return *this;
    }
    arg_iterator operator++(int){
        arg_iterator temp(*this);
        advance();
        return temp;
    }
    const arg_iterator * operator->() const {
        return this;
    }
    const arg_iterator& operator*() const {
        return *this;
    }

    bool operator==(const arg_iterator& other) const {
        return other.data_ == data_;
    }

    bool operator!=(const arg_iterator& other) const {
        return other.data_ != data_;
    }

    char type() const { return *typetag_; }

    int32_t as_int32(int32_t def = 0) const {
        return as_number<int32_t>(def);
    }

    int64_t as_int64(int64_t def = 0) const {
        return as_number<int64_t>(def);
    }

    float as_float(float def = 0) const {
        return as_number<float>(def);
    }

    double as_double(double def = 0) const {
        return as_number<double>(def);
    }

    const char * as_string(const char *def = nullptr) const;

    blob as_blob() const;

    uint64_t as_timetag(uint64_t def = 0) const;

    rgba as_rgba(rgba def = rgba{}) const;

    midi as_midi(midi def = midi{}) const;
private:
    template<typename T>
    T as_number(T def) const;
    void advance();

    const char *typetag_;
    const char *data_;
    const char *end_;
};

class received_packet {
public:
    received_packet(const char *data, int32_t size);

    bool is_bundle() const { return type_ == type::bundle; }

    bool is_message() const { return type_ == type::message; }

    const char *data() const { return data_; }

    int32_t size() const { return size_; }
private:
    const char *data_;
    int32_t size_;
    enum class type {
        none,
        message,
        bundle
    };
    type type_;
};

class received_message {
public:
    received_message(const received_packet& packet);

    bool check() const { return address_pattern_ != nullptr; }

    const char *address_pattern() const { return address_pattern_; }

    const char *data() const { return address_pattern_; }

    int32_t size() const { return end_ - address_pattern_; }

    int32_t count() const { return nargs_; }

    arg_iterator begin() const { return arg_iterator(type_tags_ + 1, data_, end_); }

    arg_iterator end() const { return arg_iterator(end_); }
private:
    const char *address_pattern_;
    const char *type_tags_;
    const char *data_;
    const char *end_;
    int32_t nargs_;
};

class message_builder {
public:
    message_builder(char *buffer, int32_t size);

    bool valid() const { return *buffer_; }

    const char *data() const { return buffer_; }
    int32_t size() const { return data_ - buffer_; }

    template<typename T, typename... U>
    void set(const T& address, U&&... args);

    void set_address(const char *s);
    void set_address(const char *s, int32_t n);
    void set_address(const std::string& s);

    void set_args(const char *tags);
    void set_args(const char *tags, int32_t n);
    void set_args(const std::string& tags);

    template<typename T>
    message_builder& operator <<(T t);
    message_builder& operator <<(const char *s);
    message_builder& operator <<(const std::string& s);
    message_builder& operator <<(blob b);
    message_builder& operator <<(timetag t);
    message_builder& operator <<(midi m);
    message_builder& operator <<(rgba c);
    message_builder& operator <<(true_tag t);
    message_builder& operator <<(false_tag f);
    message_builder& operator <<(nil_tag n);
    message_builder& operator <<(inf_tag i);
private:
    char *buffer_;
    char *end_;
    char *tag_;
    char *data_;
    // helper methods
    template<typename T, typename... U>
    void make_typetags(char *tag, T&& arg, U&&... args);
    void make_typetags(char *tag){} // sentinel
    template<typename T, typename... U>
    void add_args(T&& arg, U&&... args);
    void add_args() {} // sentinel
    bool check_space(int32_t n);
    void invalidate();
    message_builder& add_string(const char *s, int32_t n);
};

} // osc
} // aoo


