#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

template <typename T, std::size_t N>
class RingBuffer {
public:
    static_assert(N > 0, "RingBuffer size must be greater than zero.");

    void push(T item)
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            buf_[write_] = std::move(item);
            write_ = (write_ + 1) % N;

            if (count_ == N) {
                read_ = (read_ + 1) % N;
            } else {
                ++count_;
            }
        }

        cv_.notify_one();
    }

    T wait_pop()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return count_ > 0 || closed_; });

        if (count_ == 0) {
            return T{};
        }

        T item = std::move(*buf_[read_]);
        buf_[read_].reset();
        read_ = (read_ + 1) % N;
        --count_;
        return item;
    }

    template <typename Rep, typename Period>
    std::optional<T> wait_pop_for(std::chrono::duration<Rep, Period> timeout)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        const bool ready = cv_.wait_for(lock, timeout,
                                        [this] { return count_ > 0 || closed_; });
        if (!ready || count_ == 0) return std::nullopt;

        T item = std::move(*buf_[read_]);
        buf_[read_].reset();
        read_ = (read_ + 1) % N;
        --count_;
        return item;
    }

    bool try_pop(T& out)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (count_ == 0) {
            return false;
        }

        out = std::move(*buf_[read_]);
        buf_[read_].reset();
        read_ = (read_ + 1) % N;
        --count_;
        return true;
    }

    std::vector<T> snapshot(std::size_t max_count = N) const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const std::size_t take = (max_count < count_) ? max_count : count_;
        std::vector<T> frames;
        frames.reserve(take);

        const std::size_t start = (read_ + count_ - take) % N;
        for (std::size_t i = 0; i < take; ++i) {
            const std::size_t index = (start + i) % N;
            if (buf_[index].has_value()) {
                frames.push_back(*buf_[index]);
            }
        }

        return frames;
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return count_;
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::array<std::optional<T>, N> buf_{};
    std::size_t write_ = 0;
    std::size_t read_ = 0;
    std::size_t count_ = 0;
    bool closed_ = false;
};

