// [t8] TSAN 环境探针：与 state->queue 完全同型的最小正确程序
//   producer 线程: lock(queue_mtx) -> push_back -> unlock -> notify_one
//   consumer 线程: unique_lock(queue_mtx) -> wait_for(pred) -> 取 front/pop_front
// 若该程序在 setarch -R 下仍报 double lock / data race，则在**同一环境**下
// TSan 对 mutex+condvar 的组合存在误报，可用作证伪依据。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct Item { int v; };
struct State {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Item>> q;
};

int main() {
    auto st = std::make_shared<State>();
    std::atomic<bool> stop{false};
    std::atomic<int> got{0};
    std::thread consumer([&] {
        while (!stop.load()) {
            std::shared_ptr<Item> it;
            {
                std::unique_lock<std::mutex> lk(st->mtx);
                st->cv.wait_for(lk, std::chrono::milliseconds(5), [&] { return !st->q.empty() || stop.load(); });
                if (st->q.empty()) continue;
                it = std::move(st->q.front());
                st->q.pop_front();
            }
            if (it) got.fetch_add(1);
        }
    });
    for (int i = 0; i < 20000; ++i) {
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            if (st->q.size() >= 64) st->q.pop_front();
            st->q.push_back(std::make_shared<Item>(Item{i}));
        }
        st->cv.notify_one();
    }
    stop.store(true);
    st->cv.notify_all();
    consumer.join();
    std::printf("probe done got=%d\n", got.load());
    return 0;
}
