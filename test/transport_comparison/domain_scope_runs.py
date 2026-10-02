#!/usr/bin/env python3
"""socket/SHM 作用域回归：私有共享内存，构建和证据全部放仓库外。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from topic_pool_lifecycle import isolated
ROOT=Path(__file__).resolve().parents[2]
TESTS='''test_channel_scope test_socket_reliable_crc test_socket_borrow test_socket_recv_worker
 test_socket_ser_concurrency test_udp_port_boundary test_shm_domain_isolation
 test_shm_sniffer_control_name test_dzflat_transport test_dzflat_sercli test_sercli_auto_path
 test_topic_chunk_pool test_ipc_info_pool test_ipc_info_pool_layout test_ipc_info_pool_version
 test_socket_endpoint_split test_handshake_probe test_loan test_lifecycle_contract
 test_shm_route_session test_dzflat_rx test_dzflat_fallback_semantics test_recv_fragment_isolation
 test_socket_wait_set test_socket_nodelet test_socket_topic_isolation test_shm_ser_backpressure'''.split()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True)
    p.add_argument('--skip-build',action='store_true')
    p.add_argument('--tests',nargs='+',default=TESTS)
    a=p.parse_args();w=a.work.resolve()
    if w==ROOT or ROOT in w.parents:p.error('构建和日志目录必须在仓库外')
    out=w/'scope-results';out.mkdir(parents=True,exist_ok=False)
    if not a.skip_build:
        subprocess.run(['python3','-B',str(ROOT/'test/transport_comparison/repair_units.py'),
                        '--work',str(w/'units'),'--targets',*a.tests],check=True)
    rows=[];hashes={}
    for t in a.tests:
        binary=w/'units/build/bin'/t
        hashes[t]=hashlib.sha256(binary.read_bytes()).hexdigest()
        with (out/(t+'.log')).open('w') as log:
            try:code=subprocess.run(isolated([binary]),stdout=log,stderr=subprocess.STDOUT,timeout=180).returncode
            except subprocess.TimeoutExpired:code=124
        rows.append({'test':t,'exit':code})
        (out/'results.json').write_text(json.dumps(rows,indent=2))
        print(t,code,flush=True)
    hashes['libipc.so']=hashlib.sha256((w/'units/build/lib/libipc.so').read_bytes()).hexdigest()
    (out/'hashes.json').write_text(json.dumps(hashes,indent=2))
    return any(r['exit'] for r in rows)
if __name__=='__main__':raise SystemExit(main())
