/* W06 诊断探针 J（判定性）：`ipc::route::clear_storage(name)` 会不会把已挂载订阅者的
 * 等待状态段从发布端"换掉"？
 *
 * 假设：dz 的 shm_pub_ipc::InitChannel() 在建 sender 之前调用 clear_storage(name)，
 * 它 unlink 掉同名段；随后发布端 acquire 建出**新段**（新内存），于是发布端
 * `notify_readers` 递增的 seq 字与订阅端（旧映射）等的是**两个不同的字**。
 *   · 兼容臂：阻塞 recv(50) 只用 seq 当"早点醒"的提示，没醒也会超时轮询 ⇒ 不丢消息（只是慢）。
 *   · worker 臂：wait-set 是**唯一**通知通道 ⇒ 该 route 只能靠 wait_timeout 才发现，
 *     发布端随即析构/摘 route 时消息就丢。
 *
 * 三种变体输出 seq 字地址与 publish 后的值，一望即知。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

#include "libipc/ipc.h"
#include "libipc/recv_wait_set.h"

using namespace std::chrono_literals;

static const void* tok_ptr(const ipc::route& r)
{
    return static_cast<const void*>(r.read_wait_token().sequence());
}
static std::uint32_t tok_val(const ipc::route& r)
{
    const auto t = r.read_wait_token();
    return (t.sequence() != nullptr) ? t.sequence()->load() : 0u;
}

static void run(const char* tag, bool clear_before_sender)
{
    const std::string name = std::string("w06probeJ_") + tag + "_" + std::to_string(::getpid());
    std::printf("=== variant %s (clear_storage before sender: %d) ===\n", tag, static_cast<int>(clear_before_sender));

    ipc::route sub{name.c_str(), ipc::receiver, false};
    const void* p_sub = tok_ptr(sub);
    std::printf("  sub   rd-waiter seq ptr=%p val=%u\n", p_sub, tok_val(sub));

    if (clear_before_sender)
    {
        ipc::route::clear_storage(name.c_str());
        std::printf("  [clear_storage called]\n");
    }

    ipc::route pub{name.c_str(), ipc::sender, false};
    std::printf("  pub   rd-waiter seq ptr=%p val=%u\n", tok_ptr(pub), tok_val(pub));
    std::printf("  sub   rd-waiter seq ptr=%p val=%u (re-read)\n", tok_ptr(sub), tok_val(sub));

    ipc::recv_wait_set set;
    const bool added = set.add(sub.read_wait_token());
    std::printf("  wait-set add=%d\n", static_cast<int>(added));

    std::this_thread::sleep_for(100ms);
    const bool sent = pub.try_send("HELLO", 100);
    std::printf("  try_send=%d -> sub seq val=%u  pub seq val=%u\n", static_cast<int>(sent), tok_val(sub),
                tok_val(pub));

    const bool woke = added && set.wait(300ms);
    std::printf("  set.wait(300ms)=%d (true 立即唤醒 / false 只能等超时)\n", static_cast<int>(woke));
    const auto ready = set.consume_ready();
    std::printf("  ready=%zu\n", ready.size());
    const auto d = sub.recv(50);
    std::printf("  sub.recv size=%zu\n", d.size());

    pub.clear();
    sub.clear();
    ipc::route::clear_storage(name.c_str());
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    run("noclear", false);
    run("clear", true);
    return 0;
}
