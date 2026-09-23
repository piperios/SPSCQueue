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

#include <chrono>
#include <iostream>
#include <thread>

#include <rigtorp/spsc_queue.hpp>

#if __has_include(<boost/lockfree/spsc_queue.hpp> )
#include <boost/lockfree/spsc_queue.hpp>
#endif

#if __has_include(<folly/ProducerConsumerQueue.h>)
#include <folly/ProducerConsumerQueue.h>
#endif

static void pin_thread(int const cpu) {
    if (cpu < 0) { return; }

#ifdef __APPLE__
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) != 0) {
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == -1) {
#endif
        perror("pthread_setaffinity_no");
        exit(1);
    }
}

int main([[maybe_unused]] int const argc, [[maybe_unused]] char* argv[]) {

    using namespace rigtorp;

    int cpu1 = -1;
    int cpu2 = -1;

    if (argc == 3) {
        cpu1 = std::stoi(argv[1]);
        cpu2 = std::stoi(argv[2]);
    }

    constexpr size_t queue_size = 10000000;
    constexpr int64_t iters = 10000000;

    std::cout << "SPSCQueue:" << std::endl;

    {
        spsc_queue<int> q(queue_size);
        auto t = std::thread([&] {
            pin_thread(cpu1);
            for (auto i = 0; i < iters; ++i) {
                while (not q.front());
                if (*q.front() != i) throw std::runtime_error("");
                q.pop();
            }
        });

        pin_thread(cpu2);

        auto const start = std::chrono::steady_clock::now();
        for (auto i = 0; i < iters; ++i) q.emplace(i);
        t.join();
        auto const stop = std::chrono::steady_clock::now();
        std::cout << iters * 1000000 / std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count() << " ops/ms" << std::endl;
    }

    {
        spsc_queue<int> q1(queue_size), q2(queue_size);
        auto t = std::thread([&] {
            pin_thread(cpu1);
            for (auto i = 0; i < iters; ++i) {
                while (not q1.front());
                q2.emplace(*q1.front());
                q1.pop();
            }
        });

        pin_thread(cpu2);

        auto const start = std::chrono::steady_clock::now();
        for (auto i = 0; i < iters; ++i) {
            q1.emplace(i);
            while (!q2.front());
            q2.pop();
        }
        auto const stop = std::chrono::steady_clock::now();
        t.join();
        std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count() / iters << " ns RTT" << std::endl;
    }

#if __has_include(<boost/lockfree/spsc_queue.hpp> )
    std::cout << "boost::lockfree::spsc:" << std::endl;
    {
        boost::lockfree::spsc_queue<int> q(queue_size);
        auto t = std::thread([&] {
            pin_thread(cpu1);
            for (int i = 0; i < iters; ++i) {
                int val;
                while (q.pop(&val, 1) != 1);
                if (val != i) { throw std::runtime_error(""); }
            }
        });

        pin_thread(cpu2);

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) { while (!q.push(i)); }
        t.join();
        auto stop = std::chrono::steady_clock::now();
        std::cout << iters * 1000000 / std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count() << " ops/ms" << std::endl;
    }

    {
        boost::lockfree::spsc_queue<int> q1(queue_size), q2(queue_size);
        auto t = std::thread([&] {
            pin_thread(cpu1);
            for (int i = 0; i < iters; ++i) {
                int val;
                while (q1.pop(&val, 1) != 1);
                while (!q2.push(val));
            }
        });

        pin_thread(cpu2);

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            while (!q1.push(i));
            int val;
            while (q2.pop(&val, 1) != 1);
        }
        auto stop = std::chrono::steady_clock::now();
        t.join();
        std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count() / iters << " ns RTT" << std::endl;
    }
#endif

#if __has_include(<folly/ProducerConsumerQueue.h>)
    std::cout << "folly::ProducerConsumerQueue:" << std::endl;

    {
        folly::ProducerConsumerQueue<int> q(queue_size);
        auto t = std::thread([&] {
            pin_thread(cpu1);
            for (int i = 0; i < iters; ++i) {
                int val;
                while (!q.read(val));
                if (val != i) { throw std::runtime_error(""); }
            }
        });

        pin_thread(cpu2);

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) { while (!q.write(i)); }
        t.join();
        auto stop = std::chrono::steady_clock::now();
        std::cout << iters * 1000000 / std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count() << " ops/ms" << std::endl;
    }

    {
        folly::ProducerConsumerQueue<int> q1(queue_size), q2(queue_size);
        auto t = std::thread([&] {
            pin_thread(cpu1);
            for (int i = 0; i < iters; ++i) {
                int val;
                while (!q1.read(val));
                q2.write(val);
            }
        });

        pin_thread(cpu2);

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            while (!q1.write(i));
            int val;
            while (!q2.read(val));
        }
        auto stop = std::chrono::steady_clock::now();
        t.join();
        std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count() / iters << " ns RTT" << std::endl;
    }
#endif

    return 0;
}
