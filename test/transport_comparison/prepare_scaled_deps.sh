#!/usr/bin/env bash
# 同版本测试依赖，只调整 iceoryx 容量；安装与下载均留在给定的仓库外目录。
set -euo pipefail
target="${1:?用法：prepare_scaled_deps.sh 仓库外临时目录}"
mkdir -p "$target/sources" "$target/prefix"
curl -fL --retry 3 --retry-all-errors https://codeload.github.com/eclipse-iceoryx/iceoryx/tar.gz/refs/tags/v2.0.5 -o "$target/sources/iceoryx.tar.gz"
curl -fL --retry 3 --retry-all-errors https://codeload.github.com/eclipse-cyclonedds/cyclonedds/tar.gz/refs/tags/0.10.2 -o "$target/sources/cyclonedds.tar.gz"
sha256sum "$target/sources/iceoryx.tar.gz" "$target/sources/cyclonedds.tar.gz" > "$target/source-sha256.txt"
tar -xf "$target/sources/iceoryx.tar.gz" -C "$target/sources"
tar -xf "$target/sources/cyclonedds.tar.gz" -C "$target/sources"
# 2.0.5 的通知器容量被上游硬编码为 256，普通 -D 参数不生效。
# 同时重建 RouDi、C binding 与 CycloneDDS，禁止与原安装的 ABI 混用。
python3 - "$target/sources/iceoryx-2.0.5/iceoryx_posh/cmake/IceoryxPoshDeployment.cmake" <<'PY'
import pathlib,sys
p=pathlib.Path(sys.argv[1]); s=p.read_text()
old="set(IOX_INTERNAL_MAX_NUMBER_OF_NOTIFIERS 256)"
assert s.count(old)==1
p.write_text(s.replace(old,"set(IOX_INTERNAL_MAX_NUMBER_OF_NOTIFIERS 2048)"))
PY
cmake -S "$target/sources/iceoryx-2.0.5/iceoryx_meta" -B "$target/iox-build" \
  -DCMAKE_INSTALL_PREFIX="$target/prefix" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DIOX_MAX_PUBLISHERS=2048 -DIOX_MAX_SUBSCRIBERS=2048 \
  -DBUILD_TEST=OFF
cmake --build "$target/iox-build" -j8
cmake --install "$target/iox-build"
cmake -S "$target/sources/cyclonedds-0.10.2" -B "$target/dds-build" \
  -DCMAKE_INSTALL_PREFIX="$target/prefix" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$target/prefix" -DBUILD_SHARED_LIBS=ON \
  -DENABLE_TOPIC_DISCOVERY=ON -DENABLE_TYPE_DISCOVERY=ON \
  -DENABLE_SECURITY=NO -DENABLE_SSL=NO -DENABLE_SHM=ON \
  -DBUILD_DOCS=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$target/dds-build" -j8
cmake --install "$target/dds-build"
