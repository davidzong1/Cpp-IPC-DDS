#pragma once
#include <iostream>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#define IPC_INVALID_VALUE 0xFFFFFFFFFFFFFFFFULL

// --- 你的计数信号量实现 ---
namespace ipc
{
    namespace sync
    {

        class count_sem
        {
            count_sem(count_sem const &) = delete;
            count_sem &operator=(count_sem const &) = delete;

        public:
            explicit count_sem(std::uint32_t count = 0,
                               std::uint32_t max_count = (std::numeric_limits<std::uint32_t>::max)())
                : count_(count), max_count_(max_count) {}

            bool try_wait() noexcept
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (count_ == 0)
                {
                    return false;
                }
                --count_;
                return true;
            }

            bool wait(std::uint64_t tm = IPC_INVALID_VALUE) noexcept
            {
                std::unique_lock<std::mutex> lock(mtx_);
                if (tm == IPC_INVALID_VALUE)
                {
                    cv_.wait(lock, [this]()
                             { return count_ > 0; });
                    --count_;
                    return true;
                }

                if (tm == 0)
                {
                    if (count_ == 0)
                    {
                        return false;
                    }
                    --count_;
                    return true;
                }

                auto timeout = std::chrono::milliseconds(static_cast<std::uint64_t>(tm));
                if (!cv_.wait_for(lock, timeout, [this]()
                                  { return count_ > 0; }))
                {
                    return false;
                }
                --count_;
                return true;
            }

            bool post(std::uint32_t count = 1) noexcept
            {
                if (count == 0)
                {
                    return true;
                }

                std::lock_guard<std::mutex> lock(mtx_);
                if (count_ > max_count_ - count)
                {
                    return false;
                }
                count_ += count;
                cv_.notify_all();
                return true;
            }

            std::uint32_t value() const noexcept
            {
                std::lock_guard<std::mutex> lock(mtx_);
                return count_;
            }

        private:
            mutable std::mutex mtx_;
            std::condition_variable cv_;
            std::uint32_t count_ = 0;
            std::uint32_t max_count_ = (std::numeric_limits<std::uint32_t>::max)();
        };

    } // namespace sync
} // namespace ipc

#undef IPC_INVALID_VALUE
// --- 测试程序开始 ---

// // 用于线程安全输出的互斥锁
// std::mutex print_mtx;
// void safe_print(const std::string& msg) {
//     std::lock_guard<std::mutex> lock(print_mtx);
//     std::cout << msg << std::endl;
// }

// // 测试 1：并发度限制 (例如：最多允许 2 个线程同时访问)
// void test_concurrency_limit() {
//     safe_print("\n--- 启动测试 1: 并发控制 (最大并发度 = 2) ---");
//     ipc::sync::count_sem sem(2); // 初始信号量为 2

//     auto worker = [&sem](int id) {
//         safe_print("线程 " + std::to_string(id) + " 正在等待资源...");
//         sem.wait(); // 获取信号量
//         safe_print("--> 线程 " + std::to_string(id) + " 获取到资源，正在执行操作。当前剩余资源: " + std::to_string(sem.value()));

//         std::this_thread::sleep_for(std::chrono::milliseconds(800)); // 模拟耗时任务

//         safe_print("<-- 线程 " + std::to_string(id) + " 执行完毕，释放资源。");
//         sem.post(); // 释放信号量
//     };

//     std::vector<std::thread> threads;
//     for (int i = 1; i <= 5; ++i) {
//         threads.emplace_back(worker, i);
//     }

//     for (auto& t : threads) {
//         t.join();
//     }
// }

// // 测试 2：生产者 - 消费者模式
// void test_producer_consumer() {
//     safe_print("\n--- 启动测试 2: 生产者-消费者模式 ---");
//     ipc::sync::count_sem sem(0); // 初始为 0

//     std::thread consumer([&sem]() {
//         safe_print("[消费者] 等待产品放入...");
//         sem.wait();
//         safe_print("[消费者] 成功获取到 1 个产品! 当前剩余: " + std::to_string(sem.value()));
//     });

//     std::thread producer([&sem]() {
//         std::this_thread::sleep_for(std::chrono::seconds(1)); // 模拟生产耗时
//         safe_print("[生产者] 生产了 1 个产品，发送信号...");
//         sem.post(1);
//     });

//     consumer.join();
//     producer.join();
// }

// // 测试 3：超时机制 (wait_for) 与 非阻塞尝试 (try_wait)
// void test_timeout_and_try() {
//     safe_print("\n--- 启动测试 3: 超时等待与 try_wait 测试 ---");
//     ipc::sync::count_sem sem(0); // 初始为 0

//     // 测试 try_wait
//     if (!sem.try_wait()) {
//         safe_print("[Try Wait] 成功拦截：资源为空时 try_wait 正确返回 false。");
//     }

//     // 测试超时 wait
//     safe_print("[Timeout] 尝试获取资源，超时时间设置为 500ms...");
//     auto start = std::chrono::steady_clock::now();

//     bool result = sem.wait(500); // 期望返回 false

//     auto end = std::chrono::steady_clock::now();
//     auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

//     if (!result) {
//         safe_print("[Timeout] 成功拦截：获取资源失败，耗时约 " + std::to_string(elapsed) + " ms。");
//     } else {
//         safe_print("[Timeout] 错误：不应获取到资源！");
//     }
// }

// int main() {
//     std::cout << "===== 计数信号量 (count_sem) 综合测试 =====" << std::endl;

//     test_concurrency_limit();
//     test_producer_consumer();
//     test_timeout_and_try();

//     std::cout << "\n===== 所有测试完成 =====" << std::endl;
//     return 0;
// }