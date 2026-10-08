#!/usr/bin/env python3
"""串行执行全部冻结阶段，离线复算后逐阶段提交；完整阶段可恢复而不重采。"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from run import ROOT,HERE,dump

p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--resume',action='store_true');p.add_argument('--commit',action='store_true');p.add_argument('--start-stage');a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
dds='/home/zwc/branch/lejulab_platform/src/lejusdk/3rd_party/x86_64/cyclonedds-0.10.2'
roudi='/home/zwc/branch/lejulab_platform/src/lejusdk/3rd_party/x86_64/iceoryx/bin/iox-roudi'
build='/var/tmp/dzipc-cyclone-build-20261007'
scaled='/var/tmp/dzipc-cyclone-scaled-20261007/prefix'
regular=[sys.executable,str(HERE/'run.py'),'--build',build,'--dds-root',dds,'--roudi',roudi]
legacy=[sys.executable,str(HERE/'legacy_campaign.py'),'--gateway',build+'/bin/dzipc_gateway']
backends=['--backends','shared','dds-udp','dds-iox']
stages=[
 ('path-probe',regular+['--subscribers','1','--sizes','4096','--backends','dds-udp','dds-iox','--rounds','1','--seconds','1','--trace','--purpose','path-probe'],None),
 ('latency',regular+['--seconds','10'], 'report.py'),
 ('multi-per-publisher',regular+backends+['--publishers','1','4','8','--subscribers','1','8','--seconds','5'], 'multi_report.py'),
 ('multi-fixed-total',regular+backends+['--publishers','1','4','8','--subscribers','8','--sizes','64','4096','--seconds','5','--rate-policy','aggregate','--rate','800'], 'multi_report.py'),
 ('multi-32sub',regular+backends+['--publishers','4','8','--subscribers','32','--sizes','64','--seconds','5'], 'multi_report.py'),
 ('speed',legacy+['--kind','speed','--comparison-build','/var/tmp/dzipc-cyclone-throughput-20261007/build','--dds-root',dds,'--roudi',roudi,'--seconds','1'], 'legacy_report.py'),
 ('stress-installed',legacy+['--kind','stress','--comparison-build','/var/tmp/dzipc-cyclone-throughput-20261007/build','--dds-root',dds,'--roudi',roudi,'--seconds','10','--backends','shm','shared','dds-udp','dds-iox'], 'legacy_report.py'),
 ('stress-scaled',legacy+['--kind','stress','--comparison-build','/var/tmp/dzipc-cyclone-throughput-scaled-20261007/build','--dds-root',scaled,'--roudi',scaled+'/bin/iox-roudi','--seconds','10','--backends','shm','shared','dds-udp','dds-iox'], 'legacy_report.py'),
]
plan=[{'name':name,'command':cmd+['--output',str(out/name)],'report':report} for name,cmd,report in stages]
if (out/'execution-plan.json').exists():
    assert a.resume and json.loads((out/'execution-plan.json').read_text())==plan, '不能覆盖或改变已冻结的计划'
else:dump(out/'execution-plan.json',plan)
if a.start_stage: assert a.start_stage in [step['name'] for step in plan]
started=not a.start_stage
for step in plan:
    target=out/step['name']; manifest=target/'manifest.json'
    if not started and step['name']!=a.start_stage:
        m=json.loads(manifest.read_text()); assert len(m['windows'])==len(m['jobs']), '先前阶段不完整'
        continue
    started=True
    if target.exists():
        assert a.resume and manifest.exists(), '阶段已存在但不能恢复'
        m=json.loads(manifest.read_text());assert len(m['windows'])==len(m['jobs']), '阶段未完成，保留原批次并另行处理，不拼窗'
    else:
        print('开始阶段：'+step['name'],flush=True)
        with (out/(step['name']+'.log')).open('w') as log:subprocess.run(step['command'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    if step['report'] and not (a.resume and (target/'audit.json').exists() and (target/'results.md').exists()):
        subprocess.run([sys.executable,str(HERE/step['report']),str(target)],cwd=ROOT,check=True)
    if a.commit:
        relative=target.relative_to(ROOT)
        subprocess.run(['git','add','-f','--',str(relative),str((out/(step['name']+'.log')).relative_to(ROOT))],cwd=ROOT,check=True)
        subprocess.run(['git','-c','core.whitespace=cr-at-eol','diff','--cached','--check','--','test/cyclone_comparison','test/transport_comparison'],cwd=ROOT,check=True)
        changed=subprocess.run(['git','diff','--cached','--quiet'],cwd=ROOT)
        if changed.returncode:subprocess.run(['git','-c','gc.auto=0','-c','maintenance.auto=false','commit','-m','bench: 记录 Cyclone 对照 '+step['name']+' 全部结果'],cwd=ROOT,check=True)
    print('完成阶段：'+step['name'],flush=True)
