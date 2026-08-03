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

Usage:
    python3 tools/dzplot/test/integration_pub_restart.py
"""

import sys
import os
import time
import gc

# Use the same path setup as dzplot.py: append python/ for dzipc import
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.normpath(os.path.join(_SCRIPT_DIR, "..", "..", ".."))
_PYTHON_DIR = os.path.join(_REPO_ROOT, "python")
if _PYTHON_DIR not in sys.path:
    sys.path.append(_PYTHON_DIR)

import dzipc as ipc


def sanitize_topic_name(topic: str) -> str:
    """Replicate dzIPC/common/name_operator.h sanitize_topic_name.
    Keeps alphanumeric, '_', '-', '.'; replaces all other chars with '_'.
    """
    result = []
    for ch in topic:
        if ch.isalnum() or ch in ("_", "-", "."):
            result.append(ch)
        else:
            result.append("_")
    return "".join(result)


def channel_name_for_topic(topic: str) -> str:
    """Match dzplot's _channel_name_for_topic."""
    return "dz_ipc_" + sanitize_topic_name(topic) + "_topic"


def control_plane_name_for_topic(topic: str) -> str:
    """Match dzplot's _control_plane_name_for_topic."""
    return "dz_ipc_" + sanitize_topic_name(topic) + "_topic_control"


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

    # Open control plane
    cp = ipc.TopicControlPlane()
    ok = cp.open(cp_name)
    check(ok, f"TopicControlPlane.open('{cp_name}') succeeded")

    gen1 = cp.generation()
    state1 = cp.state()
    print(f"  Initial:  generation={gen1}, state={state1}")
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
