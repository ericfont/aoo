#include "timer.hpp"

namespace aoo {

timer::timer(const timer& other){
    last_ = other.last_.load();
    elapsed_ = other.elapsed_.load();
#if AOO_TIMEFILTER_CHECK
    static_assert(is_pow2(buffersize_), "buffer size must be power of 2!");
    delta_ = other.delta_;
    sum_ = other.sum_;
    buffer_ = other.buffer_;
    head_ = other.head_;
#endif
}

timer& timer::operator=(const timer& other){
    last_ = other.last_.load();
    elapsed_ = other.elapsed_.load();
#if AOO_TIMEFILTER_CHECK
    static_assert(is_pow2(buffersize_), "buffer size must be power of 2!");
    delta_ = other.delta_;
    sum_ = other.sum_;
    buffer_ = other.buffer_;
    head_ = other.head_;
#endif
    return *this;
}

void timer::setup(int32_t sr, int32_t blocksize){
#if AOO_TIMEFILTER_CHECK
    delta_ = (double)blocksize / (double)sr; // shouldn't tear
#endif
    reset();
}

void timer::reset(){
    _scoped_lock<spinlock> l(lock_);
    last_ = 0;
    elapsed_ = 0;
#if AOO_TIMEFILTER_CHECK
    // fill ringbuffer with nominal delta
    std::fill(buffer_.begin(), buffer_.end(), delta_);
    sum_ = delta_ * buffer_.size(); // initial sum
    head_ = 0;
#endif
}

double timer::get_elapsed() const {
    return elapsed_.load();
}

time_tag timer::get_absolute() const {
    return last_.load();
}

timer::state timer::update(time_tag t, double& error){
    std::unique_lock<spinlock> l(lock_);
    time_tag last = last_.load();
    if (!last.empty()){
        last_ = t.to_uint64(); // first!

        auto delta = time_tag::duration(last, t);
        elapsed_ = elapsed_ + delta;

    #if AOO_TIMEFILTER_CHECK
        // check delta and return error

        // if we're in a callback scheduler,
        // there shouldn't be any delta larger than
        // the nominal delta +- tolerance

        // If we're in a ringbuffer scheduler and we have a
        // DSP blocksize of N and a hardware buffer size of M,
        // there will be M / N blocks calculated in a row, so we
        // usually see one large delta and (M / N) - 1 short deltas.
        // The arithmetic mean should still be the nominal delta +- tolerance.
        // If it is larger than that, we assume that one or more DSP ticks
        // took too long, so we reset the timer and output the error.
        // Note that this also happens when we start the timer
        // in the middle of the ringbuffer scheduling sequence
        // (i.e. we didn't get all short deltas before the long delta),
        // so resetting the timer makes sure that the next time we start
        // at the beginning.
        // Since the relation between hardware buffersize and DSP blocksize
        // is a power of 2, our ringbuffer size also has to be a power of 2!

        // recursive moving average filter
        head_ = (head_ + 1) & (buffer_.size() - 1);
        sum_ += delta - buffer_[head_];
        buffer_[head_] = delta;

        auto average = sum_ / buffer_.size();
        auto average_error = average - delta_;
        auto last_error = delta - delta_;

        l.unlock();

        if (average_error > delta_ * AOO_TIMEFILTER_TOLERANCE){
            LOG_WARNING("DSP tick(s) took too long!");
            LOG_VERBOSE("last period: " << (delta * 1000.0)
                        << " ms, average period: " << (average * 1000.0)
                        << " ms, error: " << (last_error * 1000.0)
                        << " ms, average error: " << (average_error * 1000.0) << " ms");
            error = std::max<double>(0, delta - delta_);
            return state::error;
        } else {
        #if AOO_DEBUG_TIMEFILTER
            DO_LOG("delta : " << (delta * 1000.0)
                      << ", average delta: " << (average * 1000.0)
                      << ", error: " << (last_error * 1000.0)
                      << ", average error: " << (average_error * 1000.0));
        #endif
        }
    #endif

        return state::ok;
    } else {
        last_ = t.to_uint64();
        return state::reset;
    }
}

} // aoo