#!/usr/bin/env python3
"""在构建目录生成固定尺寸 DDS 类型，避免变长序列使共享内存路径失效。"""
import pathlib
import subprocess
import sys

out = pathlib.Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
sizes = [8 << k for k in range(18)]
idl = "module comparison {\n"
for n in sizes:
    idl += f"  struct Sample{n} {{ unsigned long long seq; unsigned long long stamp; octet data[{n}]; }};\n"
idl += "};\n"
(out / "comparison_types.idl").write_text(idl)
subprocess.run([sys.argv[2], "-o", str(out), str(out / "comparison_types.idl")], check=True)
select = '#pragma once\n#include "comparison_types.h"\n'
select += "inline const dds_topic_descriptor_t* descriptor(size_t n) { switch(n) {\n"
for n in sizes:
    select += f"case {n}: return &comparison_Sample{n}_desc;\n"
select += 'default: throw std::runtime_error("不支持的载荷档位"); }}\n'
(out / "types_select.hpp").write_text(select)
