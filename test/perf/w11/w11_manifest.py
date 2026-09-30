#!/usr/bin/env python3
"""W11 (t12) —— 生成 **run 级 manifest.json**（方案 §12 要求字段 + W11 冻结三元组声明）。

方案 §12 要求 manifest 至少含 run_id / work_package / source_revision / working_tree_clean /
build_dir / binary_sha256 / loaded_library_paths / transport / process_model / topic_count /
unique_topic_count / worker_count / fallback_count / registered_count / valid_rx_count /
sample_count / config_hash / result_files；空值写 null 并在 verdict 说明。

W11 另外**必须**声明（任务口径硬约束）：窗口、项数（注册项数）、tick 口径、工具版本。

用法:
  w11_manifest.py cpu   <run_root>
  w11_manifest.py dzflat <pair_root>
"""
import glob, hashlib, json, os, subprocess, sys, datetime

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(ROOT, '..', '..', '..'))
BUILD = os.path.join(REPO, 'build')
LIB = os.path.join(BUILD, 'lib', 'libipc.so.1.3.0')


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def git(args):
    try:
        return subprocess.check_output(['git'] + args, cwd=REPO, text=True).strip()
    except Exception:
        return None


def base(run_id, wp):
    return {
        "run_id": run_id,
        "work_package": wp,
        "source_revision": git(['rev-parse', 'HEAD']),
        "working_tree_clean": (git(['status', '--porcelain', '--', 'src', 'include', 'test']) or '') == '',
        "build_dir": BUILD,
        "binary_sha256": {},
        "loaded_library_paths": [os.path.join(BUILD, 'lib', 'libipc.so')],
        "transport": None,
        "process_model": "cross-process",
        "topic_count": None,
        "unique_topic_count": None,
        "worker_count": None,
        "fallback_count": None,
        "registered_count": None,
        "valid_rx_count": None,
        "sample_count": None,
        "config_hash": None,
        "result_files": [],
        "generated_at": datetime.datetime.now().astimezone().isoformat(),
    }


# ---- 冻结三元组与口径声明（W00 §11.3 R-3；⛔ 两口径不得互换） ----
CALIBER = {
    "frozen_triple": {
        "window": "--window-s 60 + --settle-s 4（窗口前静置，不含启动瞬态；外部采样器须完整落在窗口内）",
        "entry_count": "**注册项数**：state2 = n（仅订阅）；state3 = 2n（订阅 + 发布）",
        "tick_caliber": "ShmControlScheduler::stats().tick_count 的**同窗口差值 ÷ wall**（⛔ 不用公式、⛔ 不用 wakescan 推算）",
    },
    "cpu_normalized_form": "cpu ≈ k_态 × 注册项数 × tick_率；k = 每注册项每 tick 成本",
    "two_calibers_must_not_be_swapped": {
        "per_tick_cost_us": "cpu_cores ÷ tick_rate × 1e6        —— µs/tick",
        "per_entry_ns": "cpu_cores ÷ (注册项数 × tick_rate) × 1e9  —— ns/项·tick",
        "relation": "每注册项 = 每 tick 成本 × 1000 ÷ 注册项数（差一个「注册项数」因子 ⇒ 恒不相等）",
    },
    "reference_values_from_t51": {"state2_n1000_ns_per_entry": 11.9, "state3_n1000_ns_per_entry": 22.9,
                                  "note": "⛔ t45 原表 45.2 是每 tick 成本（µs/tick），不得当每注册项用"},
    "state2_n1000_single_not_a_criterion": "该档双峰不可稳定测量；若保留取上界 0.18829 + 以最坏值为准",
    "tool_version": {"harness": "w10_r5_matrix (W10-R5/§6.2-§6.3/v1)",
                     "note": "W11 复用 W10 已验证工装以保证与 R5 可比；⛔ 不使用第二工装做对比"},
}


def do_cpu(root):
    runs = sorted(glob.glob(root + '/*/*/manifest.json'))
    m = base(os.path.basename(root.rstrip('/')), 'W11')
    m["transport"] = "shm"
    m["topic_count"] = None
    m["unique_topic_count"] = None
    m["worker_count"] = 32
    m["fallback_count"] = 0
    m["registered_count"] = None
    m["valid_rx_count"] = None
    m["sample_count"] = 0
    m["measurement"] = "control-plane CPU（tick 率归一化）+ 扫描成本（§10.2）"
    m["binary_sha256"] = {"library": sha(LIB),
                          "w10_r5_matrix": sha(os.path.join(BUILD, 'bin', 'w10_r5_matrix'))}
    m["caliber"] = CALIBER
    m["config_hash"] = sha(os.path.join(ROOT, 'w11_cpu_scan_sweep.sh'))
    m["result_files"] = ["runs.tsv", "fingerprint.txt", "A_cpu_tickrate_normalized.md", "B_scan_cost.md",
                         "<phase>/<sub>/{manifest.json,windows.csv,windows.jsonl,counters.json,threestate.json,phases.csv,verdict.md}"]
    m["sub_runs"] = [os.path.relpath(os.path.dirname(p), root) for p in runs]
    m["sub_run_count"] = len(runs)
    # 逐 run 指纹一致性（R0-A1：库/工装成对）
    libs, tools = set(), set()
    for p in runs:
        d = json.load(open(p))
        libs.add(d.get('lib_sha256'))
        tools.add(d.get('tool_sha256'))
    m["sub_run_fingerprints"] = {"lib_sha256_set": sorted(x for x in libs if x),
                                 "tool_sha256_set": sorted(x for x in tools if x),
                                 "consistent": len(libs) == 1 and len(tools) == 1}
    json.dump(m, open(os.path.join(root, 'manifest.json'), 'w'), ensure_ascii=False, indent=2)
    print(f"cpu manifest: {root}/manifest.json  sub_runs={len(runs)} consistent={m['sub_run_fingerprints']['consistent']}")


def do_dzflat(root):
    rounds = sorted(r for r in glob.glob(root + '/round*') if os.path.isdir(r) and os.path.basename(r)[5:].isdigit())
    m = base(os.path.basename(root.rstrip('/')), 'W11')
    m["transport"] = "tlv|dzflat-a|dzflat-b（同轮内三档并列；⛔ 不跨档比较）"
    m["process_model"] = "cross-process（生产端 = fork 子进程；消费端 = 父进程）"
    m["topic_count"] = 1
    m["unique_topic_count"] = 1
    m["worker_count"] = 0
    m["fallback_count"] = 0
    n_total = 0
    for rd in rounds:
        for f in glob.glob(rd + '/artifacts/perf/*/samples.csv'):
            n_total += sum(1 for _ in open(f)) - 1
    m["sample_count"] = n_total
    m["measurement"] = "§10.7 三实验组：① 传输机制 transport_ns ② 完整读取 app_read_ns ③ 生产到消费 e2e_ns"
    m["binary_sha256"] = {"library": sha(LIB),
                          "test_w08_dzflat_ab": sha(os.path.join(BUILD, 'bin', 'test_w08_dzflat_ab'))}
    m["caliber"] = {
        "experiment_groups": {
            "1_transport": "transport_done_ns − publish_enter_ns（⛔ **不是**完整应用端到端延迟）",
            "2_full_read": "fully_consumed_ns − app_obtained_ns",
            "3_produced_to_consumed": "fully_consumed_ns − produced_ns（跨进程 CLOCK_MONOTONIC）",
            "extra_delivery": "app_obtained_ns − transport_done_ns（通知与交付，不计入三组）",
        },
        "dzflat_b_claim_rule": "⛔ 不得宣称「N 倍提升」；B 的收益只能表述为「仅省掉一次 0.88 MB 拷贝」+ e2e B vs A ≈ −7%",
        "cross_package_rule": "两套基准：W02 xproc_benchmark（A/B **合并**计数）vs W08 test_w08_dzflat_ab（A/B **分列**）。A/B 可分性**只在发布侧**；消费者侧两档同形（各 via_view=13500、via_object=0）",
        "tool_version": "test_w08_dzflat_ab（W08 权威工装；⛔ 本任务未改其源码）",
        "round_isolation": "每轮独立进程 + W08_ARTIFACT_ROOT 指向本轮的 round<K>/（⛔ 不改工装内嵌 run_id）",
    }
    m["config_hash"] = sha(os.path.join(ROOT, 'w11_dzflat_pair.sh'))
    m["result_files"] = ["fingerprint.txt", "C_dzflat_ab.md", "round<K>/{<log>,artifacts/perf/<run_id>/{samples.csv,samples.jsonl,manifest.json,counters_*.json,README.md}}"]
    m["rounds"] = [os.path.basename(r) for r in rounds]
    m["round_count"] = len(rounds)
    json.dump(m, open(os.path.join(root, 'manifest.json'), 'w'), ensure_ascii=False, indent=2)
    print(f"dzflat manifest: {root}/manifest.json  rounds={len(rounds)} samples={n_total}")


if __name__ == '__main__':
    mode = sys.argv[1] if len(sys.argv) > 1 else 'cpu'
    target = sys.argv[2] if len(sys.argv) > 2 else None
    if not target:
        sys.exit('usage: w11_manifest.py cpu|dzflat <root>')
    (do_cpu if mode == 'cpu' else do_dzflat)(target)
