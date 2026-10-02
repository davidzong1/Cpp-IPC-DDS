#!/usr/bin/env python3
"""验收矩阵覆盖、二进制身份和计数守恒；产品测试失败可以记录，证据自相矛盾则拒绝交付。"""
import argparse
import hashlib
import itertools
import json
import pathlib

BACKENDS = ("shm","socket","a","b","prebuilt","dds-udp","dds-iox")
SIZES = [8 << k for k in range(18)]

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def inspect(directory, prefix, expected):
    files=sorted((directory/"cases").glob(prefix+"*.json"))
    cases=[json.loads(p.read_text()) for p in files]
    actual={(d["mode"],d["backend"],d["bytes"],d["topics"],d["repeat"]) for d in cases}
    require(actual==expected, f"{directory}/{prefix} 矩阵不完整：缺 {len(expected-actual)}，多 {len(actual-expected)}")
    require(len(cases)==len(expected), "存在重复记录")
    binary=hashlib.sha256((directory/"build/comparison").read_bytes()).hexdigest()
    for f,d in zip(files,cases):
        identity=d["id"]
        if d["status"]=="unsupported":
            require(d["mode"]=="rpc" and d["backend"] in ("b","prebuilt"), f"{identity} 意外标记不支持")
            continue
        require(d.get("binary_sha256")==binary, f"{identity} 二进制不一致")
        start=d.get("started_unix_ns",int((f.stat().st_mtime-d["wall_seconds"])*1e9))
        for role in ("pub","sub"):
            if role not in d:
                continue
            role_file=directory/"runs"/identity/(role+".json")
            require(role_file.exists(), f"{identity} 缺少角色来源")
            require(role_file.stat().st_mtime_ns>=start, f"{identity} 角色 {role} 统计来自旧运行")
            require(json.loads(role_file.read_text())==d[role], f"{identity} 聚合与原角色输出不同")
        if d["status"]=="failed":
            require(any(code!=0 for code in d["exit"].values()), f"{identity} 失败状态缺退出码证据")
            continue
        require("pub" in d and "sub" in d, f"{identity} 缺计数")
        p,s=d["pub"],d["sub"]
        require(p["attempts"]==p["sent"]+p["failed"], f"{identity} 尝试数不守恒")
        require(s["received_in_window"]<=s["received"], f"{identity} 窗口数大于最终数")
        if d["mode"]=="stress":
            require(d["duration"]==10 and p["publish_lanes"]==4, f"{identity} 压力参数不符合正式方案")
            require(p["plan"]==d["topics"]*1000*10 and p["attempts"]<=p["plan"], f"{identity} 计划无效")
        if d["status"]=="ok":
            require(not p["failed"] and not s["bad"] and not s["duplicate"], f"{identity} 错误轮被写成无错误")
            if d["mode"]=="pubsub":
                require(p["sent"]==s["received"], f"{identity} 速度收发不守恒")
            if d["mode"]!="rpc":
                key={"shm":"tlv","a":"a","b":"b","prebuilt":"prebuilt"}.get(d["backend"])
                if key: require(p[key]==p["sent"], f"{identity} 实际路径不符")
            if d["backend"]=="dds-iox":
                require(p["loan_in_shm"]>0 and s["dds_shm_endpoints"]>0, f"{identity} 缺 SHM 证据")
    return cases

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--current",type=pathlib.Path,default=pathlib.Path("/var/tmp/cppipc-comparison-current"))
    p.add_argument("--scaled",type=pathlib.Path,default=pathlib.Path("/var/tmp/cppipc-comparison-scaled"))
    a=p.parse_args()
    speed_expected={(mode,b,n,1,r) for mode,b,n,r in itertools.product(("pubsub","rpc"),BACKENDS,SIZES,(1,2,3))}
    stress_expected={("stress",b,64,n,r) for b,n,r in itertools.product(("shm","dds-udp","dds-iox"),(1,100,1000),(1,2,3))}
    speed=inspect(a.current,"speed",speed_expected)
    current=inspect(a.current,"stress",stress_expected)
    scaled=inspect(a.scaled,"stress",stress_expected)
    invalidation=a.current/"overlap_invalidation.json"
    if invalidation.exists():
        span=json.loads(invalidation.read_text())
        for d in speed:
            if d["status"]=="unsupported": continue
            f=a.current/"cases"/(d["id"]+".json")
            finish=d.get("finished_unix_ns",int(f.stat().st_mtime*1e9))/1e9
            start=d.get("started_unix_ns",int((finish-d["wall_seconds"])*1e9))/1e9
            require(start>span["end"] or finish<span["start"], f"{d['id']} 仍在作废时间区间内")
    print(f"覆盖与证据审计通过：速度 {len(speed)} 格，压力 {len(current)+len(scaled)} 格。")

if __name__=="__main__":
    main()
