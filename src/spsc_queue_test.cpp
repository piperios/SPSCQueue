/*
Copyright (c) 2018 Erik Rigtorp <erik@rigtorp.se>

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

#undef NDEBUG

#include <cassert>
#include <chrono>
#include <iostream>
#include <rigtorp/spsc_queue.hpp>
#include <set>
#include <thread>

namespace {
// test_type tracks correct usage of constructors and destructors
struct test_type {
    static std::set<test_type const*> constructed;

    test_type() noexcept {
        assert(not constructed.contains(this));
        constructed.insert(this);
    };

    test_type(test_type const& other) noexcept {
        assert(not constructed.contains(this));
        assert(constructed.count(&other) == 1);
        constructed.insert(this);
    };

    test_type(test_type&& other) noexcept {
        assert(not constructed.contains(this));
        assert(constructed.count(&other) == 1);
        constructed.insert(this);
    };

    test_type& operator=(test_type const& other) noexcept {
        assert(constructed.count(this) == 1);
        assert(constructed.count(&other) == 1);
        return *this;
    };

    test_type& operator=(test_type&& other) noexcept {
        assert(constructed.count(this) == 1);
        assert(constructed.count(&other) == 1);
        return *this;
    }

    ~test_type() noexcept {
        assert(constructed.count(this) == 1);
        constructed.erase(this);
    };
};
}  // namespace

std::set<test_type const*> test_type::constructed;

int main(int argc, char* argv[]) {
    (void)argc, (void)argv;

    using namespace rigtorp;

    // Functionality test
    {
        spsc_queue<test_type, 10> q{};
        assert(q.front() == nullptr);
        assert(q.empty());
        assert(q.empty() == true);
        assert(q.capacity() == 15);
        for (int i = 0; i < 15; i++) { q.emplace(); }
        assert(q.front() != nullptr);
        assert(q.size() == 15);
        assert(q.empty() == false);
        assert(test_type::constructed.size() == 15);
        assert(q.try_emplace() == false);
        q.pop();
        assert(q.size() == 14);
        assert(test_type::constructed.size() == 14);
        q.pop();
        assert(q.try_emplace() == true);
        assert(test_type::constructed.size() == 14);
    }
    assert(test_type::constructed.empty());

    // A two-slot ring holds one item and remains usable after wrapping.
    {
        spsc_queue<int, 2> q{};
        assert(q.capacity() == 1);
        for (int i = 0; i < 4; ++i) {
            assert(q.try_emplace(i));
            assert(q.size() == 1);
            assert(!q.try_emplace(i));
            assert(*q.front() == i);
            q.pop();
            assert(q.empty());
        }
    }

    // A power-of-two ring holds one fewer item than its slot count.
    {
        spsc_queue<int, 4> q{};
        assert(q.capacity() == 3);
        for (int i = 0; i < 3; ++i) assert(q.try_emplace(i));
        assert(!q.try_emplace(3));
        q.pop();
        q.pop();
        assert(q.try_emplace(3));
        assert(q.size() == 2);
    }

    {
        spsc_queue<int, 512> q{};
        assert(q.capacity() == 511);
    }

    // Copyable only type
    {
        struct test {
            test() {}

            test(test const&) {}

            test(test&&) = delete;
        };

        spsc_queue<test, 16> q{};
        // lvalue
        test v;
        q.emplace(v);
        (void)q.try_emplace(v);
        q.push(v);
        (void)q.try_push(v);
        static_assert(noexcept(q.emplace(v)) == false, "");
        static_assert(noexcept(q.try_emplace(v)) == false, "");
        static_assert(noexcept(q.push(v)) == false, "");
        static_assert(noexcept(q.try_push(v)) == false, "");
        // xvalue
        q.push(test());
        (void)q.try_push(test());
        static_assert(noexcept(q.push(test())) == false, "");
        static_assert(noexcept(q.try_push(test())) == false, "");
    }

    // Copyable only type (noexcept)
    {
        struct test {
            test() noexcept {}

            test(test const&) noexcept {}

            test(test&&) = delete;
        };

        spsc_queue<test, 16> q{};
        // lvalue
        test v;
        q.emplace(v);
        (void)q.try_emplace(v);
        q.push(v);
        (void)q.try_push(v);
        static_assert(noexcept(q.emplace(v)) == true, "");
        static_assert(noexcept(q.try_emplace(v)) == true, "");
        static_assert(noexcept(q.push(v)) == true, "");
        static_assert(noexcept(q.try_push(v)) == true, "");
        // xvalue
        q.push(test());
        (void)q.try_push(test());
        static_assert(noexcept(q.push(test())) == true, "");
        static_assert(noexcept(q.try_push(test())) == true, "");
    }

    // Movable only type
    {
        spsc_queue<std::unique_ptr<int>, 16> q{};
        // lvalue
        // auto v = std::unique_ptr<int>(new int(1));
        // q.emplace(v);
        // q.try_emplace(v);
        // q.push(v);
        // q.try_push(v);
        // xvalue
        q.emplace(std::unique_ptr<int>(new int(1)));
        (void)q.try_emplace(std::unique_ptr<int>(new int(1)));
        q.push(std::unique_ptr<int>(new int(1)));
        (void)q.try_push(std::unique_ptr<int>(new int(1)));
        auto v = std::unique_ptr<int>(new int(1));
        static_assert(noexcept(q.emplace(std::move(v))) == true, "");
        static_assert(noexcept(q.try_emplace(std::move(v))) == true, "");
        static_assert(noexcept(q.push(std::move(v))) == true, "");
        static_assert(noexcept(q.try_push(std::move(v))) == true, "");
    }

    // Fuzz and performance test
    {
        size_t const iter = 100000;
        spsc_queue<size_t, 101> q{};
        std::atomic<bool> flag(false);
        std::thread producer([&] {
            while (!flag);
            for (size_t i = 0; i < iter; ++i) { q.emplace(i); }
        });

        size_t sum = 0;
        auto start = std::chrono::system_clock::now();
        flag = true;
        for (size_t i = 0; i < iter; ++i) {
            while (!q.front());
            sum += *q.front();
            q.pop();
        }
        auto end = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

        assert(q.front() == nullptr);
        assert(sum == iter * (iter - 1) / 2);

        producer.join();

        std::cout << duration.count() / iter << " ns/iter" << std::endl;
    }

    return 0;
}
