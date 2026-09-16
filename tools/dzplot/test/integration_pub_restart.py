#!/usr/bin/env python3
"""
Integration test: Publisher restart → generation change → Sniffer reattach.

Verifies the publisher-restart fix for dzplot's LiveSniffSource:
  1. Create a SHM publisher (generation 1)
  2. Attach Sniffer + TopicControlPlane — verify sniffer receives data
  3. Destroy publisher
  4. Create a NEW publisher for the same topic (generation 2)
  5. Poll control plane until generation changes and state==Ready
  6. Reattach: close old sniffer → open new → skip_to_latest
  7. Verify new sniffer receives data from the restarted publisher

Uses the SAME channel naming convention as dzplot._channel_name_for_topic
and _control_plane_name_for_topic, so this is a faithful E2E of the fix.

用法: python3.10 tools/dzplot/test/integration_pub_restart.py
      (绑定只有 cpython-310 的构建; 3.12 载入失败 ⇒ 退出码 2 = SKIP, 不当成通过)
退出码: 0=全过, 1=有失败, 2=缺 dzipc 绑定(跳过)
"""

import sys
import os
import time
import gc
import importlib.util

# Use the same path setup as main.py (2026-09-13 由 dzplot.py 改名而来):
# append python/ for dzipc import.
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_DZPLOT_DIR = os.path.dirname(_SCRIPT_DIR)
_REPO_ROOT = os.path.normpath(os.path.join(_DZPLOT_DIR, "..", ".."))
_PYTHON_DIR = os.path.join(_REPO_ROOT, "python")
if _PYTHON_DIR not in sys.path:
    sys.path.append(_PYTHON_DIR)

# 段名一律取自**被测工具自己**的静态方法 —— 本文件不复刻任何名字。
# ⛔ 原先这里各抄了一份 sanitize_topic_name / channel_name_for_topic /
#    control_plane_name_for_topic, 于是 C++ 侧改规则时它不会跟着动, 而且那份
#    sanitize 用的是 str.isalnum()(认 Unicode, C++ 只认 ASCII)。E2E 用例里
#    "两份字符串各自自洽"是最坏的情况: 它自证正确, 却与传输层无关。
_DZPLOT_SRC = os.path.join(_DZPLOT_DIR, "main.py")
_spec = importlib.util.spec_from_file_location("dzplot", _DZPLOT_SRC)
dzplot = importlib.util.module_from_spec(_spec)
sys.modules["dzplot"] = dzplot
_spec.loader.exec_module(dzplot)
LSS = dzplot.LiveSniffSource

# ⛔ 绑定缺失必须是**显式 SKIP(rc=2)**, 不能是 import 异常回溯。三个运行时核对
#    脚本(本文件 + verify_segment_naming + verify_runtime_no_garbage)共用同一套
#    退出码约定, CI 门(scripts/ci_check.sh)按码分流; 少了这个 guard, 在默认
#    python3(3.12) 下本文件会以 traceback 收场 —— 退出码 1, 与"真失败"无法区分,
#    而且 pytest 收集阶段直接 ERROR, 整个文件一条用例都跑不到。
try:
    import dzipc as ipc
except Exception as exc:                      # noqa: BLE001 — 任何载入失败都算 SKIP
    print(f"SKIP: dzipc 绑定不可用({type(exc).__name__}: {exc})")
    print("      → 用 python3.10 跑本脚本(绑定是 cpython-310 构建)")
    sys.exit(2)


sanitize_topic_name = LSS._sanitize_topic_name
channel_name_for_topic = LSS._channel_name_for_topic
control_plane_name_for_topic = LSS._control_plane_name_for_topic


def make_publisher(topic: str) -> "ipc.PublisherIPC":
    """Create a SHM publisher for the given topic."""
    msg_template = ipc.TestMsg()
    topic_data = ipc.make_topic_data(msg_template)
    pub = ipc.PublisherIPCPtrMake(
        topic_data, topic, domain_id=0, ipc_type=ipc.IPC_SHM, verbose=False,
    )
    pub.InitChannel()
    return pub


def try_recv_poll(sniffer, attempts=10, interval=0.01):
    """Poll sniffer.try_recv() until data arrives or attempts exhausted."""
    for _ in range(attempts):
        result = sniffer.try_recv()
        if result is not None:
            return result
        time.sleep(interval)
    return None


PASS = 0
FAIL = 0


def check(condition, msg):
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  PASS  {msg}")
    else:
        FAIL += 1
        print(f"  FAIL  {msg}")


# ---------------------------------------------------------------------------
# Test: Publisher restart → generation change → Sniffer reattach
# ---------------------------------------------------------------------------
def test_publisher_restart_generation_change_and_reattach():
    global PASS, FAIL
    topic = "/e2e_restart_test"
    ch_name = channel_name_for_topic(topic)
    cp_name = control_plane_name_for_topic(topic)

    print("=" * 72)
    print(f"E2E: Publisher restart → Sniffer reattach")
    print(f"  topic:        {topic}")
    print(f"  data channel: {ch_name}")
    print(f"  control plane:{cp_name}")
    print("=" * 72)

    # ---- Phase 1: Create first publisher ----
    print("\n--- Phase 1: First publisher + Sniffer ---")
    pub1 = make_publisher(topic)
    check(pub1 is not None, "Publisher #1 created")
    time.sleep(0.05)  # Let IPC settle

    # Open Sniffer (passive, identical to dzplot)
    sniffer = ipc.Sniffer()
    ok = sniffer.open(ch_name, ipc.SnifferTopology.route)
    check(ok, f"Sniffer.open('{ch_name}', route) succeeded")

    # Open control plane — 走 dzplot 自己的只读路径, 不是裸 open。
    # (裸 open 是 create|open: 段不存在时它会静默建一个空壳, 于是这条用例即使在
    #  "控制面根本没被建出来"的情况下也会 PASS —— 那正是要防的恒绿。)
    cp, gen_from_helper, cp_status = LSS._open_control_plane_readonly(ipc, cp_name)
    check(cp_status == "attached" and cp is not None,
          f"read-only control-plane attach succeeded (status={cp_status})")

    gen1 = cp.generation()
    state1 = cp.state()
    print(f"  Initial:  generation={gen1}, state={state1}")
    check(gen1 == gen_from_helper, f"generation from helper == plane ({gen_from_helper})")
    check(gen1 > 0, f"Initial generation > 0 (got {gen1})")
    check(state1 == ipc.TopicState.Ready,
          f"Initial state is Ready (got {state1})")

    # Publish messages through pub1; sniffer should receive them.
    # IMPORTANT: publish_for_sniffer uses a pipelined buffer — each call
    # flushes the PREVIOUS message to the route channel and queues the new
    # one. An intermediate try_recv() (even one that returns None) syncs the
    # sniffer's read cursor so the flush becomes visible. This matches
    # dzplot's real poll loop where try_recv runs every iteration.
    msg = ipc.TestMsg()
    msg.data1 = [1.0, 2.0, 3.0]
    msg.data2 = [42, 43]
    msg.data3 = ["phase1"]
    msg.data4 = True
    msg_gm = msg.to_generic()
    pub1.publish_for_sniffer(msg_gm)  # queue first message
    time.sleep(0.05)
    sniffer.try_recv()                # sync read cursor (returns None)
    time.sleep(0.02)
    pub1.publish_for_sniffer(msg_gm)  # flush first message, queue second
    time.sleep(0.05)

    sample1 = try_recv_poll(sniffer)
    check(sample1 is not None, "Sniffer receives data from publisher #1")
    if sample1:
        print(f"    sample1 keys: {list(sample1.keys())}")

    # ---- Phase 2: Destroy publisher ----
    print("\n--- Phase 2: Destroy first publisher ---")
    del pub1
    gc.collect()
    time.sleep(0.15)  # Allow SHM cleanup (TopicControl destructor → generation=0)

    # After destruction, generation may reset to 0 (or state may become non-Ready).
    gen_post_destroy = cp.generation()
    state_post_destroy = cp.state()
    print(f"  Post-destroy: generation={gen_post_destroy}, state={state_post_destroy}")

    # ---- Phase 3: Create second publisher (same topic) ----
    print("\n--- Phase 3: Second publisher (same topic) ---")
    pub2 = make_publisher(topic)
    check(pub2 is not None, "Publisher #2 created")
    time.sleep(0.05)

    # Poll control plane until state==Ready and generation != gen1.
    gen2 = cp.generation()
    state2 = cp.state()
    poll_count = 0
    while state2 != ipc.TopicState.Ready and poll_count < 100:
        time.sleep(0.01)
        gen2 = cp.generation()
        state2 = cp.state()
        poll_count += 1

    print(f"  After publish #2 init: generation={gen2}, state={state2}")

    check(gen2 != gen1, f"Generation changed ({gen1} → {gen2})")
    check(state2 == ipc.TopicState.Ready,
          f"State is Ready (got {state2})")
    check(gen2 > 0, f"New generation > 0 (got {gen2})")

    # ---- Phase 4: Reattach Sniffer (mirrors dzplot reattach logic) ----
    print("\n--- Phase 4: Reattach Sniffer ---")
    old_dropped = sniffer.dropped()
    sniffer.close()
    print(f"  Old sniffer closed (dropped={old_dropped})")

    new_sniffer = ipc.Sniffer()
    ok = new_sniffer.open(ch_name, ipc.SnifferTopology.route)
    check(ok, "Re-opened Sniffer after publisher restart")

    new_sniffer.skip_to_latest()
    print("  skip_to_latest() called")

    # Publish messages through pub2; new sniffer should receive them.
    pub2.publish_for_sniffer(msg_gm)   # queue first message
    time.sleep(0.05)
    new_sniffer.try_recv()             # sync read cursor (returns None)
    time.sleep(0.02)
    pub2.publish_for_sniffer(msg_gm)   # flush first message, queue second
    time.sleep(0.05)

    sample2 = try_recv_poll(new_sniffer)
    check(sample2 is not None,
          "Re-attached Sniffer receives data from publisher #2")
    if sample2:
        print(f"    sample2 keys: {list(sample2.keys())}")

    # ---- Phase 5: "Second restart" cycle — verify loop works ----
    print("\n--- Phase 5: Second restart cycle ---")
    del pub2
    gc.collect()
    time.sleep(0.15)

    pub3 = make_publisher(topic)
    time.sleep(0.05)

    gen3 = cp.generation()
    state3 = cp.state()
    poll_count = 0
    while state3 != ipc.TopicState.Ready and poll_count < 100:
        time.sleep(0.01)
        gen3 = cp.generation()
        state3 = cp.state()
        poll_count += 1

    print(f"  Third publisher: generation={gen3}, state={state3}")
    check(gen3 != gen2, f"Generation changed again ({gen2} → {gen3})")
    check(state3 == ipc.TopicState.Ready, f"State Ready (got {state3})")

    new_sniffer.close()
    sniffer3 = ipc.Sniffer()
    ok = sniffer3.open(ch_name, ipc.SnifferTopology.route)
    check(ok, "Re-opened Sniffer for third publisher")
    sniffer3.skip_to_latest()

    pub3.publish_for_sniffer(msg_gm)   # queue first message
    time.sleep(0.05)
    sniffer3.try_recv()               # sync read cursor (returns None)
    time.sleep(0.02)
    pub3.publish_for_sniffer(msg_gm)   # flush first message, queue second
    time.sleep(0.05)
    sample3 = try_recv_poll(sniffer3)
    check(sample3 is not None,
          "Sniffer #3 receives data from publisher #3")

    # ---- Phase 6: initial generation=0 scenario ----
    print("\n--- Phase 6: Initial generation=0 (no publisher) scenario ---")
    del pub3
    gc.collect()
    time.sleep(0.3)

    cp2 = ipc.TopicControlPlane()
    ok = cp2.open(cp_name)
    check(ok, "Control plane open with no publisher (generation may be 0)")

    gen_zero = cp2.generation()
    state_zero = cp2.state()
    print(f"  No-publisher state: generation={gen_zero}, state={state_zero}")

    # Verify generation==0 logic: should NOT trigger reattach
    should_reattach = (state_zero == ipc.TopicState.Ready
                       and gen_zero != 0
                       and gen_zero != gen3)
    check(not should_reattach,
          f"generation==0 skips reattach (gen={gen_zero}, state={state_zero})")

    # ---- Phase 7: 只读纪律 —— 段不存在时**不建段** ----
    # 这是与旧写法的分界。判据不看"open 返回了什么"(create|open 恒真), 只看**盘**:
    # 一个从没有过发布端的 topic, 走完只读路径后 /dev/shm 里必须干干净净。
    print("\n--- Phase 7: read-only discipline (no segment created when absent) ---")
    lonesome = "/e2e_restart_never_published"
    lonely_cp_name = control_plane_name_for_topic(lonesome)
    check(not LSS._control_plane_segment_exists(lonely_cp_name),
          f"precondition: {lonely_cp_name} 不存在")

    plane_lonely, gen_lonely, status_lonely = LSS._open_control_plane_readonly(
        ipc, lonely_cp_name)
    check(status_lonely == "absent" and plane_lonely is None,
          f"absent 段返回 (None, 0, 'absent'), 得到 "
          f"({plane_lonely}, {gen_lonely}, {status_lonely!r})")
    check(not os.path.exists("/dev/shm/" + lonely_cp_name),
          f"只读路径**没有**在 /dev/shm 建出空壳段 ({lonely_cp_name})")

    # 反面对照: 旧写法(裸 create|open)确实会建段 —— 证明上一条不是恒真。
    # 用一次性名字, 不碰上面的正确名字; 验完立刻清掉。
    probe_name = lonely_cp_name + "_probe_must_be_removed"
    probe = ipc.TopicControlPlane()
    probe.open(probe_name)
    created = os.path.exists("/dev/shm/" + probe_name)
    check(created, "反面对照: 裸 create|open 确实会建段(证明判据有区分力)")
    del probe
    gc.collect()
    if os.path.exists("/dev/shm/" + probe_name):
        os.unlink("/dev/shm/" + probe_name)

    # Cleanup
    sniffer3.close()
    del cp2

    # ---- Summary ----
    print()
    print("=" * 72)
    print(f"Results: {PASS} passed, {FAIL} failed")
    print("=" * 72)
    return FAIL == 0


if __name__ == "__main__":
    ok = test_publisher_restart_generation_change_and_reattach()
    sys.exit(0 if ok else 1)
