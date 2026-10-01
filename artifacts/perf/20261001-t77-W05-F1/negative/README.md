### t77 反向验证（判据的判据）—— 两次消融

消融① 恢复 buggy 门控（删掉 else 分支）后跑**常驻用例** test_w05_stale_slot_gate:
❌ peer_count()==0 期间陈旧槽位**未被回收** —— has_peers() 为假时也必须按低频（周期 ≥ peer_dead_timeout 量级）扫一次；跳过整条扫描会让槽位/cc_id 位泄漏。
❌ 活订阅者被误断：37/100 —— 陈旧 cc_id 位在活订阅者复用后被 disconnect_receivers() 摘掉，是「回收时机被推迟」的直接后果。
[  FAILED  ] ShmControlScheduler.StaleSlotIsReapedWhenPeerCountZeroAndLiveSubscriberSurvives (18675 ms)
[  FAILED  ] 1 test, listed below:
[  FAILED  ] ShmControlScheduler.StaleSlotIsReapedWhenPeerCountZeroAndLiveSubscriberSurvives
 1 FAILED TEST

消融② 同一消融下跑**修正后的** test_shm_control_scheduler.PublisherHeartbeatWithoutPeers:
  peer_count()==0 时 stale 扫描被**整条跳过**（陈旧槽位/cc_id 位永不回收）—— 这正是 R1-W05-F1 的正确性回归形态；必须按 peer_dead_timeout 量级兜底扫一次
  [  FAILED  ] ShmControlScheduler.PublisherHeartbeatWithoutPeers (4153 ms)
  [  PASSED  ] 0 tests.

⇒ 两条用例都**有牙**：修复前红、修复后绿（各自 3 次/1 次实跑）。
