#!/usr/bin/env python3
"""生成测试 RouDi 配置；--original 保留原安装的最大 1 MiB 池档。"""
import argparse
import pathlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("target", type=pathlib.Path)
parser.add_argument("--original", action="store_true")
args = parser.parse_args()
args.target.parent.mkdir(parents=True, exist_ok=True)
pools = [(128, 10000), (1024, 5000), (16384, 1000), (131072, 200), (1048576, 50)]
if not args.original:
    pools.append((2097152, 50))
text = "[general]\nversion = 1\n[[segment]]\n"
for size, count in pools:
    text += f"[[segment.mempool]]\nsize = {size}\ncount = {count}\n"
args.target.write_text(text)
