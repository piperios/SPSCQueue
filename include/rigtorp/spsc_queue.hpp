/*
Copyright (c) 2020 Erik Rigtorp <erik@rigtorp.se>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>       // std::allocator
#include <new>          // std::hardware_destructive_interference_size
#include <type_traits>  // std::enable_if, std::is_*_constructible

#ifdef __has_cpp_attribute
#if __has_cpp_attribute(nodiscard)
#define RIGTORP_NODISCARD [[nodiscard]]
#else
#define RIGTORP_NODISCARD
#endif

#if __has_cpp_attribute(no_unique_address)
#define RIGTORP_NO_UNIQUE_ADDRESS [[no_unique_address]]
#else
#define RIGTORP_NO_UNIQUE_ADDRESS
#endif

#endif

namespace rigtorp {

template <typename T, std::size_t Capacity = 1024, typename Allocator = std::allocator<T>>
class spsc_queue {

    template <typename, typename = void>
    struct has_allocate_at_least : std::false_type {};

    template <typename Alloc>
    struct has_allocate_at_least<Alloc, std::void_t<typename Alloc::value_type, decltype(std::declval<Alloc&>().allocate_at_least(size_t{}))>>
        : std::true_type {};

public:
    explicit spsc_queue(Allocator const& allocator = Allocator()) : allocator_(allocator) {

        if constexpr (has_allocate_at_least<Allocator>::value) {
            auto res = allocator_.allocate_at_least(allocation_count_ + 2 * cache_line_padding);
            slots_ = res.ptr;
            allocation_count_ = res.count - 2 * cache_line_padding;
        } else {
            slots_ = std::allocator_traits<Allocator>::allocate(allocator_, allocation_count_ + 2 * cache_line_padding);
        }

        static_assert(alignof(spsc_queue<T>) == cache_line_size, "queue should be cache-aligned");
        static_assert(sizeof(spsc_queue<T>) >= 3 * cache_line_size, "queue should be at least three cache lines long");

        assert(reinterpret_cast<char*>(&read_idx_) - reinterpret_cast<char*>(&write_idx_) >= static_cast<std::ptrdiff_t>(cache_line_size));
    }

    spsc_queue(spsc_queue const&) = delete;
    spsc_queue(spsc_queue&&) noexcept = delete;

    spsc_queue& operator=(spsc_queue const&) = delete;
    spsc_queue& operator=(spsc_queue&&) noexcept = delete;

    ~spsc_queue() {
        while (front()) pop();
        std::allocator_traits<Allocator>::deallocate(allocator_, slots_, allocation_count_ + 2 * cache_line_padding);
    }

    template <typename... Args>
    void emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
        static_assert(std::is_constructible_v<T, Args&&...>, "T must be constructible with Args&&...");

        auto const write_idx = write_idx_.load(std::memory_order_relaxed);

        auto const next_write_idx = (write_idx + 1) & access_mask;
        while (next_write_idx == cached_read_idx_) cached_read_idx_ = read_idx_.load(std::memory_order_acquire);

        new (&slots_[write_idx + cache_line_padding]) T(std::forward<Args>(args)...);

        write_idx_.store(next_write_idx, std::memory_order_release);
    }

    template <typename... Args>
    RIGTORP_NODISCARD bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
        static_assert(std::is_constructible_v<T, Args&&...>, "T must be constructible with Args&&...");

        auto const write_idx = write_idx_.load(std::memory_order_relaxed);

        auto const next_write_idx = (write_idx + 1) & access_mask;
        if (next_write_idx == cached_read_idx_) {
            cached_read_idx_ = read_idx_.load(std::memory_order_acquire);
            if (next_write_idx == cached_read_idx_) return false;
        }

        new (&slots_[write_idx + cache_line_padding]) T(std::forward<Args>(args)...);

        write_idx_.store(next_write_idx, std::memory_order_release);

        return true;
    }

    void push(T const& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        static_assert(std::is_copy_constructible_v<T>, "T must be copy constructible");
        emplace(v);
    }

    template <typename P>
        requires std::is_constructible_v<T, P&&>
    void push(P&& v) noexcept(std::is_nothrow_constructible_v<T, P&&>) {
        emplace(std::forward<P>(v));
    }

    RIGTORP_NODISCARD bool try_push(T const& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        static_assert(std::is_copy_constructible_v<T>, "T must be copy constructible");
        return try_emplace(v);
    }

    template <typename P>
        requires std::is_constructible_v<T, P&&>
    RIGTORP_NODISCARD bool try_push(P&& v) noexcept(std::is_nothrow_constructible_v<T, P&&>) {
        return try_emplace(std::forward<P>(v));
    }

    RIGTORP_NODISCARD T* front() noexcept {
        auto const read_idx = read_idx_.load(std::memory_order_relaxed);
        if (read_idx == cached_write_idx_) {
            cached_write_idx_ = write_idx_.load(std::memory_order_acquire);
            if (cached_write_idx_ == read_idx) return nullptr;
        }

        return &slots_[read_idx + cache_line_padding];
    }

    void pop() noexcept {
        static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");

        auto const read_idx = read_idx_.load(std::memory_order_relaxed);
        assert(write_idx_.load(std::memory_order_acquire) != read_idx && "Can only call pop() after front() has returned a non-nullptr");

        slots_[read_idx + cache_line_padding].~T();

        auto next_read_idx = (read_idx + 1) & access_mask;

        read_idx_.store(next_read_idx, std::memory_order_release);
    }

    RIGTORP_NODISCARD std::size_t size() const noexcept {
        return (write_idx_.load(std::memory_order_acquire) - read_idx_.load(std::memory_order_acquire)) & access_mask;
    }

    RIGTORP_NODISCARD bool empty() const noexcept { return write_idx_.load(std::memory_order_acquire) == read_idx_.load(std::memory_order_acquire); }

    RIGTORP_NODISCARD static std::size_t capacity() noexcept { return queue_capacity - 1; }

private:
    static_assert(Capacity >= 2, "Capacity must provide at least two ring slots");
    static_assert(Capacity <= std::bit_floor(std::numeric_limits<std::size_t>::max()), "Capacity is too large for the ring");

    constexpr static std::size_t queue_capacity = std::bit_ceil(Capacity);

    constexpr static std::size_t access_mask = queue_capacity - 1;

    constexpr static std::size_t cache_line_size = std::hardware_destructive_interference_size;

    constexpr static std::size_t cache_line_padding = ((cache_line_size - 1) / sizeof(T)) + 1;

    std::size_t allocation_count_{queue_capacity};
    T* slots_{nullptr};

    RIGTORP_NO_UNIQUE_ADDRESS Allocator allocator_;

    // Align to cache line size in order to avoid false sharing.
    // `cached_read_idx_` and `cached_write_idx_` are used to reduce cache traffic.
    alignas(cache_line_size) std::atomic<size_t> write_idx_{};
    alignas(cache_line_size) size_t cached_write_idx_{};
    alignas(cache_line_size) size_t cached_read_idx_{};
    alignas(cache_line_size) std::atomic<size_t> read_idx_{};
};
}  // namespace rigtorp
