# 安装与回滚复跑

```bash
cmake --build build-shared-net --target ipc dzipc_gateway dzipc_list dzipc_topic_cat --parallel 4
cmake --install build-shared-net --prefix /tmp/dzipc-shared-install
c++ -O2 -g -std=c++17 test/shared_net/installed_roundtrip.cc -I/tmp/dzipc-shared-install/include -L/tmp/dzipc-shared-install/lib -Wl,-rpath,/tmp/dzipc-shared-install/lib -lipc -pthread -o /tmp/dzipc-installed-roundtrip
python3 test/shared_net/rollback.py --gateway /tmp/dzipc-shared-install/bin/dzipc_gateway --example /tmp/dzipc-installed-roundtrip
c++ -O2 -g -std=c++17 exec/dzipc_gateway/src/shared_net_probe.cc -I/tmp/dzipc-shared-install/include -L/tmp/dzipc-shared-install/lib -Wl,-rpath,/tmp/dzipc-shared-install/lib -lipc -pthread -o /tmp/dzipc-installed-probe
python3 test/shared_net/cli.py --gateway /tmp/dzipc-shared-install/bin/dzipc_gateway
python3 test/shared_net/topic_cat.py --gateway /tmp/dzipc-shared-install/bin/dzipc_gateway --probe /tmp/dzipc-installed-probe --cat /tmp/dzipc-shared-install/bin/dzipc_topic_cat --list /tmp/dzipc-shared-install/bin/dzipc_list
cmake --build build-shared-net-off --target test_shared_net_config test_shared_net_shm_capacity --parallel 4
ctest --test-dir build-shared-net-off -L shared_net --output-on-failure
# 先构建以下旧 target，再执行：
env -u DZIPC_NET_BACKEND ctest --test-dir build-shared-net -R '^test_(udp_port_boundary|shm_control_scheduler|shm_mpmc_channel|shm_mpmc_types|shm_mpmc_config|shm_mpmc_control_plane|shm_multi_publisher|shm_route_session)$' --output-on-failure
cmake --build build-shared-net-python --target _dzipc_core dzipc_gateway --parallel 4
unshare --user --map-root-user --mount --ipc bash -c 'mount -t tmpfs -o size=768m tmpfs /dev/shm && /usr/bin/python3.10 test/shared_net/public_python.py --gateway "$PWD/build-shared-net-python/bin/dzipc_gateway"'
git -c core.whitespace=cr-at-eol diff --check
```

上述路径均为本次实际构建/安装路径，rollback.py 内部使用独占 IPC/mount 命名空间；不调用团队 MCP。
