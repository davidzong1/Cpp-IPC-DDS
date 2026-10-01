/* T02 §4.3.2：唯一判据 `pub_control_tick()` 的节拍状态转移四分情形（纯头文件，进程内）。
 *
 * 用一个可控 now 的替身 PubControlState（记录回调次数/时刻）验证：
 *   ① 首次 tick（无 peer）      -> 只设置 next_stale_due，**不**立刻扫；
 *   ② 有 peer                  -> 每拍都扫（周期 = 驱动方拍长）；
 *   ③ 无 peer 且未到期          -> 不扫；
 *   ④ 无 peer 且到期            -> 扫一次并重排 next_stale_due（= now + dead_timeout）。
 * 另验 ⓪：on_pub_heartbeat 每拍**无条件**执行（含 has_peers() 抛异常的那一拍）。
 *
 * 用法: ./w05_tick_transition   （退出码 0 = 四分情形全部符合契约；非 0 = 断言失败数）
 */
#include <chrono>
#include <cstdio>
#include <stdexcept>

#include "dzIPC/threepools/shm_control_scheduler.h"

namespace {

using dzIPC::shm_control::ControlClock;
using dzIPC::shm_control::PubControlState;
using dzIPC::shm_control::PubTickState;
using dzIPC::shm_control::pub_control_tick;

constexpr auto kDead = std::chrono::nanoseconds{2'000'000'000LL};   /* peer_dead_timeout */

struct Clock
{
    ControlClock::time_point t{};
    void advance_ms(long ms) { t += std::chrono::milliseconds{ms}; }
};

struct Fake : PubControlState
{
    int hb{0};
    int scan{0};
    bool peers{false};
    bool throw_on_has_peers{false};
    ControlClock::time_point last_scan{};

    void on_pub_heartbeat(ControlClock::time_point) override { ++hb; }
    bool has_peers() const override
    {
        if (throw_on_has_peers)
        {
            throw std::runtime_error("has_peers() threw (替身，模拟控制面不可用)");
        }
        return peers;
    }
    void on_pub_stale_scan(ControlClock::time_point now, std::chrono::nanoseconds) override
    {
        ++scan;
        last_scan = now;
    }
};

int fails = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", what);
    if (!ok) { ++fails; }
}

}   // namespace

int main()
{
    /* 替身尺寸守卫：判据不改变导出抽象基类的布局（仅 vtable 指针）。 */
    std::printf("sizeof(PubControlState)=%zu sizeof(PubTickState)=%zu\n",
                sizeof(PubControlState), sizeof(PubTickState));

    /* ① 首次 tick（无 peer）：只设 next_stale_due，不扫。 */
    {
        Clock c;
        Fake f;
        PubTickState st;
        pub_control_tick(f, c.t, kDead, st);
        check(f.hb == 1, "①首次 tick：on_pub_heartbeat 被执行（无条件）");
        check(f.scan == 0, "①首次 tick：**不**立刻扫 stale");
        check(st.init, "①首次 tick：init 置位");
        check(st.next_stale_due == c.t + kDead, "①首次 tick：next_stale_due = now + dead_timeout");

        c.advance_ms(100);   /* 远未到期 */
        pub_control_tick(f, c.t, kDead, st);
        check(f.scan == 0, "③无 peer 且未到期：不扫");
        check(f.hb == 2, "③无 peer 且未到期：heartbeat 仍每拍无条件");
    }

    /* ② 有 peer：每拍扫。 */
    {
        Clock c;
        Fake f;
        f.peers = true;
        PubTickState st;
        for (int i = 0; i < 5; ++i)
        {
            pub_control_tick(f, c.t, kDead, st);
            c.advance_ms(50);   /* 驱动方拍长 = pub_heartbeat */
        }
        check(f.scan == 5, "②有 peer：每拍都扫（5 拍 = 5 次）");
        check(f.hb == 5, "②有 peer：heartbeat 同样每拍执行");
    }

    /* ④ 无 peer 且到期：扫一次并重排。 */
    {
        Clock c;
        Fake f;
        PubTickState st;
        pub_control_tick(f, c.t, kDead, st);         /* 首次：安排 */
        check(f.scan == 0, "④前置：首次不扫");
        c.advance_ms(2000);                          /* 恰好到期 */
        pub_control_tick(f, c.t, kDead, st);
        check(f.scan == 1, "④无 peer 且到期：扫一次");
        check(st.next_stale_due == c.t + kDead, "④无 peer 且到期：重排 next_stale_due = now + dead_timeout");
        c.advance_ms(500);
        pub_control_tick(f, c.t, kDead, st);
        check(f.scan == 1, "④重排后未到期：不再扫");
        c.advance_ms(1500);
        pub_control_tick(f, c.t, kDead, st);
        check(f.scan == 2, "④下一周期到期：再扫一次");
    }

    /* ②→ 有 peer 时把兜底到期点推后；掉回无 peer 后不立刻补扫。 */
    {
        Clock c;
        Fake f;
        PubTickState st;
        f.peers = true;
        pub_control_tick(f, c.t, kDead, st);         /* 有 peer：扫，并把 due 推到 now+2s */
        check(f.scan == 1 && st.next_stale_due == c.t + kDead, "②有 peer：扫并推后兜底到期点");
        f.peers = false;                             /* 掉回无 peer */
        c.advance_ms(50);
        pub_control_tick(f, c.t, kDead, st);         /* 未到期：不扫 */
        check(f.scan == 1, "掉回无 peer 且未到期：不立刻补扫");
    }

    /* ⓪ has_peers() 抛出：本拍放弃 stale 判定，但 heartbeat 已先执行完。 */
    {
        Clock c;
        Fake f;
        f.throw_on_has_peers = true;
        PubTickState st;
        pub_control_tick(f, c.t, kDead, st);
        check(f.hb == 1, "⓪has_peers() 抛出：heartbeat 仍已执行（异常不外逃）");
        check(f.scan == 0, "⓪has_peers() 抛出：本拍不扫");
        check(!st.init, "⓪has_peers() 抛出：不臆造排程状态");
    }

    std::printf("tick_transition_fails=%d\n", fails);
    std::printf("T02_TICK_TRANSITION_DONE\n");
    return fails == 0 ? 0 : 1;
}
