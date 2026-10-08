#!/usr/bin/env python3
"""Git 后台维护监测补正后，完整重跑两个潜在受干扰阶段；不替换原批次。"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from run import ROOT, HERE, dump

p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--commit',action='store_true');a=p.parse_args()
out=a.output.resolve();original=json.loads((out/'execution-plan.json').read_text());plan=[]
for name in ('multi-fixed-total','multi-32sub'):
    old=next(s for s in original if s['name']==name); target=out/('verification-'+name)
    command=list(old['command']);command[command.index('--output')+1]=str(target)
    command+=['--purpose','git-maintenance-verification']
    plan.append({'original':name,'name':target.name,'command':command})
manifest=out/'verification-plan.json'
if manifest.exists():assert json.loads(manifest.read_text())==plan
else:dump(manifest,plan)
for s in plan:
    target=out/s['name']; print('开始环境复测：'+s['name'],flush=True)
    if target.exists():
        m=json.loads((target/'manifest.json').read_text());assert len(m['windows'])==len(m['jobs']), '保留中断批次，不拼窗'
    else:
        with (out/(s['name']+'.log')).open('w') as log:
            subprocess.run(s['command'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    subprocess.run([sys.executable,str(HERE/'multi_report.py'),str(target)],cwd=ROOT,check=True)
    if a.commit:
        subprocess.run(['git','add','-f','--',str(target.relative_to(ROOT)),str((out/(s['name']+'.log')).relative_to(ROOT))],cwd=ROOT,check=True)
        subprocess.run(['git','-c','gc.auto=0','-c','maintenance.auto=false','commit','-m','bench: 记录环境监测补正后的 '+s['name']],cwd=ROOT,check=True)
    print('完成环境复测：'+s['name'],flush=True)
