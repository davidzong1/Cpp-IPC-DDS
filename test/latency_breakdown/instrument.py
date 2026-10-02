#!/usr/bin/env python3
"""只在仓库外的源码副本插入诊断点；精确匹配失败时拒绝构建。"""
import argparse
import hashlib
import json
import pathlib
import shutil

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"计时插入位置不是唯一：{old[:100]!r}")
    return text.replace(old, new)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("work", type=pathlib.Path)
    a = ap.parse_args()
    work = a.work.resolve()
    if work == ROOT or ROOT in work.parents:
        ap.error("测试构建目录必须在仓库外")
    work.mkdir(parents=True, exist_ok=True)
    source = work / "source"
    if source.exists():
        shutil.rmtree(source)
    for name in ("src", "include"):
        shutil.copytree(ROOT / name, source / name)
    shutil.copyfile(HERE / "trace.h", source / "include/trace.h")
    shutil.copyfile(HERE / "trace.cpp", source / "src/dzIPC/breakdown_trace.cc")
    changed = {}
    def patch(relative, changes):
        p = source / relative
        s = p.read_text()
        original = hashlib.sha256(p.read_bytes()).hexdigest()
        for old, new in changes:
            s = replace_once(s, old, new)
        p.write_text('#include "trace.h"\n' + s)
        changed[relative] = {"original": original, "instrumented": hashlib.sha256(p.read_bytes()).hexdigest()}

    patch("src/libipc/ipc.cpp", [
        ("      auto id = lo.id;\n      bool pushed = wait_for(",
         "      auto id = lo.id;\n      breakdown::mark(breakdown::SubmitBegin);\n      bool pushed = wait_for(")])
    patch("src/dzIPC/shm_pub_sub_ipc.cc", [
        ("    /* 测试缝(§4.2)：此刻 inflight 仍为 1",
         "    breakdown::received(raw_data.data(), raw_data.size());\n    /* 测试缝(§4.2)：此刻 inflight 仍为 1"),
        ("    process_received_buffer(state, std::move(raw_data));\n    return out;",
         "    breakdown::mark(breakdown::LeaseReleased);\n    process_received_buffer(state, std::move(raw_data));\n    return out;"),
        ("            state->view_queue->push(std::make_shared<Sample>(std::move(raw_data), seg_id, exp_hash));",
         """            breakdown::mark(breakdown::Validated);
            auto sampled_message = std::make_shared<Sample>(std::move(raw_data), seg_id, exp_hash);
            breakdown::mark(breakdown::Allocated);
            state->view_queue->push(std::move(sampled_message));""")])
    patch("src/dzIPC/threepools/recv_worker.cc", [
        ("        const auto t0 = Clock::now();",
         "        const auto t0 = Clock::now();\n        breakdown::worker_begin(std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count());")])
    patch("include/dzIPC/common/circularqueue.h", [
        ("#include <atomic>", "#include <atomic>\n#include <type_traits>\nnamespace dzIPC { class Sample; }"),
        ("    cell->sequence.store(pos + 1, std::memory_order_release);",
         "    if constexpr (std::is_same<msgType, dzIPC::Sample>::value) breakdown::mark(breakdown::QueueVisible);\n    cell->sequence.store(pos + 1, std::memory_order_release);")])

    s = (ROOT / "test/transport_comparison/comparison.cpp").read_text()
    for old, new in [
        ('#include <dds/dds.h>', '#include "trace.h"\n#include <cstdlib>\n#include <dds/dds.h>'),
        ('        const u64 arrival=now_ns();',
         '        const u64 arrival=now_ns();\n        breakdown::begin(seq); breakdown::stamp(breakdown::App,arrival);'),
        ('            const int n=dds_take(rd,samples,info,32,32);',
         '            const u64 take_begin=breakdown::enabled()?now_ns():0;\n            const int n=dds_take(rd,samples,info,32,32);\n            const u64 take_end=breakdown::enabled()?now_ns():0;'),
        ('                auto* meta=static_cast<DDSMeta*>(samples[j]);',
         '                auto* meta=static_cast<DDSMeta*>(samples[j]);\n                breakdown::begin(meta->seq);\n                breakdown::stamp(breakdown::TakeBegin,take_begin);\n                breakdown::stamp(breakdown::TakeEnd,take_end);'),
        ('        u64 begin=now_ns(), entered=begin,finished=0;',
         '        u64 begin=now_ns(), entered=begin,finished=0;\n        breakdown::begin(seq); breakdown::stamp(breakdown::Produced,begin);'),
        ('            ok=dds_write(writers[i],sample)==DDS_RETCODE_OK;finished=now_ns();',
         '            breakdown::stamp(breakdown::PublishEnter,entered);\n            ok=dds_write(writers[i],sample)==DDS_RETCODE_OK;finished=now_ns();'),
        ('                entered=now_ns();ok=p->publish_loaned(std::move(lo));',
         '                entered=now_ns(); breakdown::stamp(breakdown::PublishEnter,entered); ok=p->publish_loaned(std::move(lo));'),
        ('            } else { entered=now_ns();ok=pubs[i]->publish(m); }',
         '            } else { entered=now_ns(); breakdown::stamp(breakdown::PublishEnter,entered); ok=pubs[i]->publish(m); }'),
        ('        if(!finished) finished=now_ns();',
         '        if(!finished) finished=now_ns();\n        breakdown::stamp(breakdown::PublishReturn,finished);'),
        ('        Bench b(c,ctl);b.init();\n        if(c.role=="pub") b.run_pub();else b.run_sub();',
         '        breakdown::init();\n        { Bench b(c,ctl);b.init();\n          if(c.role=="pub") b.run_pub();else b.run_sub(); }\n        breakdown::dump((c.out+".trace.csv").c_str());')
    ]:
        s = replace_once(s, old, new)
    (work / "comparison.cpp").write_text(s)
    manifest = {
        "base_comparison_sha256": hashlib.sha256((ROOT / "test/transport_comparison/comparison.cpp").read_bytes()).hexdigest(),
        "comparison_sha256": hashlib.sha256(s.encode()).hexdigest(), "patches": changed}
    (work / "instrumentation.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2))
    print("已生成独立计时源码副本", work)

if __name__ == "__main__":
    main()
