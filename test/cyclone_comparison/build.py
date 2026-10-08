#!/usr/bin/env python3
"""构建独立基准；生产库须先从冻结源码完成构建。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--build', type=Path, required=True)
p.add_argument('--dds-root', type=Path, required=True)
a = p.parse_args()
a.build = a.build.resolve()
out = a.build / 'cyclone-comparison'
out.mkdir(exist_ok=True)
sys.path.insert(0, str(ROOT/'generator'))
from batch_msg_srv_generator import process_msg_directory, process_srv_directory, collect_msg_files, build_msg_type_registry
process_msg_directory(str(ROOT/'msg'), str(out/'generated/ipc_msg'))
registry=build_msg_type_registry(collect_msg_files(str(ROOT/'msg')),str(ROOT/'msg'))
process_srv_directory(str(ROOT/'srv'),str(out/'generated/ipc_srv'),registry)
sizes = (64, 4096, 1048576)
(out/'sample.idl').write_text('module ccbench {\n'+''.join(f'  struct Sample{n} {{ octet data[{n}]; }};\n' for n in sizes)+'};\n')
commands = [[str(a.dds_root/'bin/idlc'), '-o', str(out), str(out/'sample.idl')]]
(out/'types_select.hpp').write_text('#pragma once\n#include "sample.h"\n#include <stdexcept>\ninline const dds_topic_descriptor_t* descriptor(size_t n) { switch(n) {\n'+''.join(f'case {n}: return &ccbench_Sample{n}_desc;\n' for n in sizes)+'default: throw std::runtime_error("未知尺寸"); }}\n')
commands += [['cc','-O2','-g','-DNDEBUG','-I'+str(a.dds_root/'include'),'-c',str(out/'sample.c'),'-o',str(out/'sample.o')],
    ['c++','-O2','-g','-DNDEBUG','-std=c++17','-pthread','-DLIBIPC_LIBRARY_SHARED_USING__',
     '-I'+str(ROOT/'include'),'-I'+str(out/'generated'),'-I'+str(ROOT),'-I'+str(ROOT/'3rdparty'),'-I'+str(a.dds_root/'include'),'-I'+str(out),
     str(ROOT/'test/cyclone_comparison/benchmark.cc'),str(out/'sample.o'),'-L'+str(a.build/'lib'),'-lipc',
     str(a.dds_root/'lib/libddsc.so'),'-lrt','-Wl,-rpath,'+str(a.build/'lib')+':'+str(a.dds_root/'lib'),
     '-o',str(out/'benchmark')]]
for command in commands:
    subprocess.run(command, check=True)
files = [out/'benchmark',a.build/'lib/libipc.so',a.build/'bin/dzipc_gateway',a.dds_root/'lib/libddsc.so',ROOT/'test/cyclone_comparison/benchmark.cc',out/'sample.idl']
files += sorted((out/'generated').rglob('*.hpp'))
(out/'build.json').write_text(json.dumps({'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
    'commands':commands,'sha256':{str(x.resolve()):hashlib.sha256(x.read_bytes()).hexdigest() for x in files}},ensure_ascii=False,indent=2)+'\n')
