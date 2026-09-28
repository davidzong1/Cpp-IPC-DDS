// [t8] TSAN 环境探针 2：**对象复用**（与 stop/restart churn 同形）
// 每轮 make_shared<State>() → 起 consumer → push 若干 → join consumer → 销毁 State。
// 与探针 1 的唯一差别：State（含 mutex/condvar/deque）被反复创建/销毁、地址被回收复用。
// 若 TSan 的误报只在「地址复用」时出现，则说明其 mutex shadow 记录在复用后失真。
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
    for (int round = 0; round < 30; ++round) {
        auto st = std::make_shared<State>();
        std::atomic<bool> stop{false};
        std::atomic<int> got{0};
        std::thread consumer([&] {
            while (!stop.load()) {
                std::shared_ptr<Item> it;
                {
                    std::unique_lock<std::mutex> lk(st->mtx);
                    st->cv.wait_for(lk, std::chrono::milliseconds(1), [&] { return !st->q.empty() || stop.load(); });
                    if (st->q.empty()) continue;
                    it = std::move(st->q.front());
                    st->q.pop_front();
                }
                if (it) got.fetch_add(1);
            }
        });
        for (int i = 0; i < 200; ++i) {
            {
                std::lock_guard<std::mutex> lk(st->mtx);
                if (st->q.size() >= 8) st->q.pop_front();
                st->q.push_back(std::make_shared<Item>(Item{i}));
            }
            st->cv.notify_one();
        }
        stop.store(true);
        st->cv.notify_all();
        consumer.join();
    }
    std::printf("recycle probe done\n");
    return 0;
}