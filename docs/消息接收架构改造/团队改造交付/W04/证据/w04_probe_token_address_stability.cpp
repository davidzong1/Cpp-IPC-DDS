/* 探针 3：route 重建后 read_wait_token() 的 seq 地址是否稳定？
 * 稳定 ⇒ 已注册进 worker wait-set 的 token 仍指向同一物理字；
 * 变化 ⇒ 等待层里留的是**旧映射**的地址（unmap 后即悬垂 ⇒ futex_waitv EFAULT / 静默停收）。
 * 有界、不挂死；三种重建方式分别测。 */
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>
#include "libipc/ipc.h"

static const void* mk(const std::string& n) {
    auto r = std::make_shared<ipc::route>(n.c_str(), ipc::receiver, false);
    const void* p = (const void*)r->read_wait_token().sequence();
    return p;
}

int main() {
    const std::string a = "w04probe3_a";
    // ① 长期持有一个 sender（段不会被 unlink），反复重建 receiver
    auto pub = std::make_shared<ipc::route>(a.c_str(), ipc::sender, false);
    auto r1 = std::make_shared<ipc::route>(a.c_str(), ipc::receiver, false);
    const void* p1 = (const void*)r1->read_wait_token().sequence();
    r1->release();
    r1.reset();
    auto r2 = std::make_shared<ipc::route>(a.c_str(), ipc::receiver, false);
    const void* p2 = (const void*)r2->read_wait_token().sequence();
    r2->release(); r2.reset();
    std::printf("PROBE3-A (sender 在册，receiver 重建): before=%p after=%p -> %s\n", p1, p2,
                p1 == p2 ? "STABLE" : "*** CHANGED ***");

    // ② 全部析构后重建（段可能被 unlink/重新建）
    const std::string b = "w04probe3_b";
    {
        auto s = std::make_shared<ipc::route>(b.c_str(), ipc::sender, false);
        auto r = std::make_shared<ipc::route>(b.c_str(), ipc::receiver, false);
        std::printf("PROBE3-B before=%p\n", (const void*)r->read_wait_token().sequence());
        r->release(); r.reset(); s->release(); s.reset();
    }
    {
        auto s2 = std::make_shared<ipc::route>(b.c_str(), ipc::sender, false);
        auto r2b = std::make_shared<ipc::route>(b.c_str(), ipc::receiver, false);
        std::printf("PROBE3-B after =%p -> %s\n", (const void*)r2b->read_wait_token().sequence(),
                    "see above");
        r2b->release(); r2b.reset(); s2->release(); s2.reset();
    }

    // ③ clear_storage 之后重建（段被显式删除）
    const std::string c = "w04probe3_c";
    const void* pc1 = nullptr;
    {
        auto s = std::make_shared<ipc::route>(c.c_str(), ipc::sender, false);
        auto r = std::make_shared<ipc::route>(c.c_str(), ipc::receiver, false);
        pc1 = (const void*)r->read_wait_token().sequence();
        r->release(); r.reset(); s->release(); s.reset();
    }
    ipc::route::clear_storage(c.c_str());
    const void* pc2 = nullptr;
    {
        auto s = std::make_shared<ipc::route>(c.c_str(), ipc::sender, false);
        auto r = std::make_shared<ipc::route>(c.c_str(), ipc::receiver, false);
        pc2 = (const void*)r->read_wait_token().sequence();
        std::printf("PROBE3-C (clear_storage 后重建): before=%p after=%p -> %s\n", pc1, pc2,
                    pc1 == pc2 ? "STABLE" : "*** CHANGED ***");
        r->release(); r.reset(); s->release(); s.reset();
    }
    (void)pub;
    std::fflush(nullptr);
    _exit(0);
}
