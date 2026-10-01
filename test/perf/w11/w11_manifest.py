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


def _read_tsv_with_hash_header(path):
    """读**以 `#` 开头表头**的 tsv。

    ⛔ 常见陷阱（t68 自己踩到）：`DictReader(l for l in f if not l.startswith('#'))`
    会把表头行一起滤掉 ⇒ DictReader 拿**第一行数据**当字段名 ⇒ 所有 `row['xxx']` 变 None。
    正确做法：**保留表头并剥掉它的前导 `#`**。
    """
    import csv as _csv
    with open(path) as f:
        lines = f.readlines()
    if lines and lines[0].startswith('#'):
        lines[0] = lines[0][1:]
    return list(_csv.DictReader(lines, delimiter='\t'))



def fingerprint_at_collection(root):
    """从 run 目录的 `fingerprint.txt` 读**采集时**的库/工装指纹（R0-A1 权威来源）。

    ⛔ 为什么必须读它而不是现场 `sha(LIB)`（t81/N1）：
    `manifest.json` 的生成时刻**晚于**采集时刻（W11：新批采集 09:53、manifest 生成 10:10），
    期间 `build/lib/libipc.so.1.3.0` 可能被**他人重编** ⇒ 现场 `sha(LIB)` 得到的是
    **manifest 生成时刻**的库，与**采集时刻**真实运行的库**可能不是同一代**。
    实测：新批 manifest 写 `0298b1df…`（生成时），而采集时 = **`f0ebc3ef2df5b276…`**（fingerprint.txt）。
    ⇒ 本函数把「采集时」与「生成时」两个语义**显式分开**写进 manifest。

    返回 {'library': sha|None, 'tools': {name: sha}, 'source': <路径或 None>, 'started': ...}
    """
    import re as _re
    fp = os.path.join(root, 'fingerprint.txt')
    out = {'library': None, 'tools': {}, 'source': None, 'started': None}
    if not os.path.exists(fp):
        return out
    out['source'] = os.path.relpath(fp, REPO)
    lib_re = _re.compile(r'^\s*(?:libipc[\w.\-]*)\s+([0-9a-f]{64})\s*$')
    tool_re = _re.compile(r'^\s*([A-Za-z0-9_][\w.\-]*)\s+([0-9a-f]{64})\s*$')
    for line in open(fp):
        line = line.rstrip('\n')
        m = lib_re.match(line)
        if m and out['library'] is None:
            out['library'] = m.group(1)
            continue
        if line.startswith('started='):
            out['started'] = line.split('=', 1)[1].strip()
            continue
        m2 = tool_re.match(line)
        if m2 and m2.group(1) != 'libipc.so.1.3.0':
            out['tools'].setdefault(m2.group(1), m2.group(2))
    return out



def _existing_collection_provenance(root):
    """读**已存在的** manifest.json，取出**采集/首生成时**的 provenance 字段。

    ⛔ 为什么（t81 自踩并已修）：`w11_manifest.py` 会被**重跑**（如本次修 N1 后重新生成）。
    若每次都现场重算 `source_revision` / `config_hash` / `generated_at`，**重生成本身**就会
    把「采集时」的 provenance **覆盖成「重生成时」的** —— 这正是 N1 同一类错误的另一种形态
    （我 13:40 实测：`source_revision` 由 `e800ccc4…` 被改成 `dcaa0d97…`、
    `config_hash` 由 `18443358…` 改成 `8d49fa8b…`、`generated_at` 改成当天）。
    ⇒ 首次生成时这些字段就是「采集期记录」，此后**一律继承、不再重算**；
    重生成的信息另存 `manifest_regeneration`（append-only 语义）。
    """
    prev = os.path.join(root, 'manifest.json')
    if not os.path.exists(prev):
        return None
    try:
        m = json.load(open(prev))
    except Exception:
        return None
    keep = ('source_revision', 'config_hash', 'generated_at', 'working_tree_clean', 'binary_sha256')
    return {k: m[k] for k in keep if k in m}


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
    # ⛔ t81：继承「采集/首生成时」的 provenance，⛔ 重生成不得覆盖（见 _existing_collection_provenance）
    kept = _existing_collection_provenance(root)
    # ⛔ t68/F8：把 runs.tsv 的 domain 汇总进 manifest，使「逐 run 唯一 domain」可被第三方核验
    doms, domains_per_sub, dom_src = [], {}, None
    # ① 新批：runs.tsv 有 domain 列（t68/F8 起）
    for cand, src in ((os.path.join(root, 'runs.tsv'), 'runs.tsv'),
                      (os.path.join(root, 'runs_annotated.tsv'), 'runs_annotated.tsv')):
        if not os.path.exists(cand):
            continue
        try:
            for row in _read_tsv_with_hash_header(cand):
                if row.get('domain') and row['domain'].isdigit():
                    doms.append(int(row['domain'])); domains_per_sub[row['sub']] = int(row['domain'])
            if doms:
                dom_src = src
                break
        except Exception as e:
            domains_per_sub = {'__error__': str(e)}
    # ② 回填/补齐：manifest 的 input_manifest_hash 含 `r5_<state>_<dom>_0..` ⇒ 可反推
    #    ⚠️ **不是** `if not doms:` —— runs.tsv 只覆盖主批，`recheck/` 复核轮不在其中
    #    （t68 实测：仅读 runs.tsv 会得 60 而非 63）⇒ 必须**逐 run 补齐**。
    import re as _re
    for mf in glob.glob(root + '/*/*/manifest.json'):
            try:
                m2 = json.load(open(mf))
                mm = _re.search(r'r5_<state>_(\d+)_0\.\.', m2.get('input_manifest_hash', '') or '')
                if mm:
                    sub = os.path.basename(os.path.dirname(mf))
                    if sub not in domains_per_sub:      # 只补缺，不覆盖 runs.tsv 的权威值
                        doms.append(int(mm.group(1)))
                        domains_per_sub[sub] = int(mm.group(1))
            except Exception:
                pass
    if dom_src is None and domains_per_sub:
        dom_src = 'input_manifest_hash 反推（旧批回填）'
    elif dom_src is not None and domains_per_sub:
        dom_src = dom_src + ' + input_manifest_hash 反推（补齐复核轮）'
    doms = [v for k, v in domains_per_sub.items() if k != '__error__']
    m['domains'] = {'source': dom_src, 'count': len(doms), 'unique': len(set(doms)),
                    'min': min(doms) if doms else None, 'max': max(doms) if doms else None,
                    'all_unique': len(doms) == len(set(doms)) and len(doms) > 0,
                    'per_sub_run': domains_per_sub}
    m["transport"] = "shm"
    m["topic_count"] = None
    m["unique_topic_count"] = None
    m["worker_count"] = 32
    m["fallback_count"] = 0
    m["registered_count"] = None
    m["valid_rx_count"] = None
    m["sample_count"] = 0
    m["measurement"] = "control-plane CPU（tick 率归一化）+ 扫描成本（§10.2）"
    # ⛔ t81/N1：字段语义**显式分离**「生成时快照」与「采集时快照」。
    #    选方案 (b)（改名 + 新增）而非 (a)（把 library 改成采集时值）的理由：
    #    · 本 run 目录由「采集脚本」与「manifest 生成器」**两次**写入（09:53 采集、10:10 生成），
    #      期间 build/lib 可能被他人重编 ⇒ 两个时刻的库**都**是真实发生过的事实，
    #      ⛔ 不能只留一个（准确做法是把两者都写下来并标明语义）；
    #    · 通用性：任何「manifest 生成晚于采集」的 run 都会错代 ⇒ 保留生成时值可作**诊断信号**
    #      （两值不等 ⇒ 明确提示"期间 build 变过"），而 (a) 会把这个信号抹掉；
    #    · 模板价值：本仓其它 run 若有同样结构可照此改（通用建议已写入报告 §4）。
    fp = fingerprint_at_collection(root)
    now_sha = {
        "library": sha(LIB),
        "w10_r5_matrix": sha(os.path.join(BUILD, 'bin', 'w10_r5_matrix')),
    }
    # ⛔ 生成时快照也是**历史事实** ⇒ 已存在则**继承**（重生成本身不得改写它）。
    gen_sha = dict((kept or {}).get('binary_sha256') or now_sha)
    m["binary_sha256"] = gen_sha
    m["binary_sha256_at_manifest_generation"] = dict(gen_sha)
    m["binary_sha256_at_manifest_generation"]["_semantics"] = (
        "**首生成时快照**（历史事实，重生成时**继承**不改写）：manifest.json 首次写入的那一刻对 build/ 现场求 sha。"
        "⛔ **不得**当作采集时库指纹（采集可能早得多，期间 build 可能被重编）。")
    m["binary_sha256_at_collection"] = {
        "library": fp["library"],
        "tools": fp["tools"],
        "source": fp["source"],
        "started": fp["started"],
        "_semantics": "**采集时快照**（R0-A1 权威）：来自本 run 目录的 fingerprint.txt（采集脚本写的）。"
                      "⛔ 引用「本批读数用了哪个库」时必须用这一组。",
    }
    m["library_binding_authority"] = (
        "fingerprint.txt —— ⛔ 库绑定的**唯一权威**是 `binary_sha256_at_collection.source` 指向的 "
        "fingerprint.txt；`binary_sha256*` 只是**生成时**快照，语义不同、⛔ 不可互相替代。")
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
    if kept:
        m["manifest_regeneration"] = {
            "at": datetime.datetime.now().astimezone().isoformat(),
            "reason": "t81/N1：修正 binary_sha256 语义（生成时 vs 采集时）；⛔ 不重跑采集、只重写 manifest 字段",
            "source_revision_now": git(['rev-parse', 'HEAD']),
            "config_hash_now": sha(os.path.join(ROOT, 'w11_cpu_scan_sweep.sh')),
            "superseded_generated_at": kept.get('generated_at'),
            "current_build_snapshot": now_sha,
            "_note": "顶层 source_revision/config_hash/generated_at/binary_sha256 保持为**采集/首生成时**的值"
                     "（⛔ 未被本次重生成改写）；重生成时刻的现场 sha 放在 current_build_snapshot。",
        }
        for k, v in kept.items():
            m[k] = v
    json.dump(m, open(os.path.join(root, 'manifest.json'), 'w'), ensure_ascii=False, indent=2)
    print(f"cpu manifest: {root}/manifest.json  sub_runs={len(runs)} consistent={m['sub_run_fingerprints']['consistent']}"
          + (f"  regenerated(kept {sorted(kept)})" if kept else ""))


def do_dzflat(root):
    rounds = sorted(r for r in glob.glob(root + '/round*') if os.path.isdir(r) and os.path.basename(r)[5:].isdigit())
    m = base(os.path.basename(root.rstrip('/')), 'W11')
    # ⛔ t81：同 do_cpu —— 继承采集期 provenance
    kept = _existing_collection_provenance(root)
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
    # ⛔ t81/N1：同 cpu 分支 —— 生成时 vs 采集时**显式分离**（见 do_cpu 的说明）。
    fp = fingerprint_at_collection(root)
    now_sha = {
        "library": sha(LIB),
        "test_w08_dzflat_ab": sha(os.path.join(BUILD, 'bin', 'test_w08_dzflat_ab')),
    }
    gen_sha = dict((kept or {}).get('binary_sha256') or now_sha)
    m["binary_sha256"] = gen_sha
    m["binary_sha256_at_manifest_generation"] = dict(gen_sha)
    m["binary_sha256_at_manifest_generation"]["_semantics"] = (
        "**首生成时快照**（历史事实，重生成时**继承**不改写）：manifest.json 首次写入的那一刻对 build/ 现场求 sha。"
        "⛔ **不得**当作采集时库指纹（W11 新批：采集 09:53、生成 10:10，期间 build 被他人重编过）。")
    m["binary_sha256_at_collection"] = {
        "library": fp["library"],
        "tools": fp["tools"],
        "source": fp["source"],
        "started": fp["started"],
        "_semantics": "**采集时快照**（R0-A1 权威）：来自本 run 目录的 fingerprint.txt（采集脚本写的）。"
                      "⛔ 引用「本批读数用了哪个库」时必须用这一组。",
    }
    m["library_binding_authority"] = (
        "fingerprint.txt —— ⛔ 库绑定的**唯一权威**是 `binary_sha256_at_collection.source` 指向的 "
        "fingerprint.txt；`binary_sha256*` 只是**生成时**快照，语义不同、⛔ 不可互相替代。")
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
    # ⛔ t68/F8：逐轮负载与窗口（rounds.tsv 的全部列）
    rt = os.path.join(root, 'rounds.tsv')
    if os.path.exists(rt):
        m["round_windows"] = _read_tsv_with_hash_header(rt)
    else:
        m["round_windows"] = None
    if kept:
        m["manifest_regeneration"] = {
            "at": datetime.datetime.now().astimezone().isoformat(),
            "reason": "t81/N1：修正 binary_sha256 语义（生成时 vs 采集时）；⛔ 不重跑采集、只重写 manifest 字段",
            "source_revision_now": git(['rev-parse', 'HEAD']),
            "config_hash_now": sha(os.path.join(ROOT, 'w11_dzflat_pair.sh')),
            "superseded_generated_at": kept.get('generated_at'),
            "current_build_snapshot": now_sha,
            "_note": "顶层 source_revision/config_hash/generated_at/binary_sha256 保持为**采集/首生成时**的值"
                     "（⛔ 未被本次重生成改写）；重生成时刻的现场 sha 放在 current_build_snapshot。",
        }
        for k, v in kept.items():
            m[k] = v
    json.dump(m, open(os.path.join(root, 'manifest.json'), 'w'), ensure_ascii=False, indent=2)
    print(f"dzflat manifest: {root}/manifest.json  rounds={len(rounds)} samples={n_total}"
          + (f"  regenerated(kept {sorted(kept)})" if kept else ""))


if __name__ == '__main__':
    mode = sys.argv[1] if len(sys.argv) > 1 else 'cpu'
    target = sys.argv[2] if len(sys.argv) > 2 else None
    if not target:
        sys.exit('usage: w11_manifest.py cpu|dzflat <root>')
    (do_cpu if mode == 'cpu' else do_dzflat)(target)
