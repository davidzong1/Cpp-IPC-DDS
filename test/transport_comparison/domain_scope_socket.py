#!/usr/bin/env python3
"""作用域改造后的 UDP 大包诊断；在私有 namespace 中运行，可与改造前二进制对照。"""
import argparse
import json
import os
from pathlib import Path
import run as runner

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True)
    p.add_argument('--baseline',action='store_true')
    a=p.parse_args();w=a.work.resolve()
    if w==runner.ROOT or runner.ROOT in w.parents:p.error('工作目录必须在仓库外')
    rows=[]
    for window,rate in [(8,0),(1,30)]:
        for repeat in range(1,4):
            for variant in (['baseline','final'] if a.baseline else ['final']):
                folder=w/variant;(folder/'cases').mkdir(parents=True,exist_ok=True)
                os.environ.update(COMPARISON_WINDOW=str(window),COMPARISON_RATE=str(rate),
                                  COMPARISON_COUNT='0',COMPARISON_IDLE_NS='0',COMPARISON_NO_RETRY='1')
                args=argparse.Namespace(suite=f'scope-w{window}-r{rate}',work=folder,
                                       rerun=False,workers=32,publishers=4,domain=0)
                r=runner.run_case(args,'socket','pubsub',1048576,1,repeat,3,full=True)
                r['variant']=variant;rows.append(r)
                (w/'scope-socket-results.json').write_text(json.dumps(rows,indent=2))
    # 窗口 8 为不可靠突发诊断；窗口 1/30 msg/s 是无故障链路下的正确性检查。
    return any(r['status']!='ok' for r in rows if '-w1-' in r['id'])
if __name__=='__main__':raise SystemExit(main())
