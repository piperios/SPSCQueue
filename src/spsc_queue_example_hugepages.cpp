#include <iostream>
#include <rigtorp/spsc_queue.hpp>
#include <sys/mman.h>
#include <thread>

template <typename T>
struct allocator {
    using value_type = T;

    struct allocation_result {
        T* ptr;
        size_t count;
    };

    size_t round_up(size_t n) { return (((n - 1) >> 21) + 1) << 21; }

    allocation_result allocate_at_least(size_t n) {
        size_t count = round_up(sizeof(T) * n);
        auto p = static_cast<T*>(mmap(nullptr, count, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0));
        if (p == MAP_FAILED) { throw std::bad_alloc(); }
        return {p, count / sizeof(T)};
    }

    void deallocate(T* p, size_t n) { munmap(p, round_up(sizeof(T) * n)); }
};

int main(int argc, char* argv[]) {
    (void)argc, (void)argv;

    using namespace rigtorp;

    spsc_queue<int, allocator<int>> q(2);
    std::cout << q.capacity() << std::endl;
    auto t = std::thread([&] {
        while (!q.front());
        std::cout << *q.front() << std::endl;
        q.pop();
    });
    q.push(1);
    t.join();

    return 0;
}
