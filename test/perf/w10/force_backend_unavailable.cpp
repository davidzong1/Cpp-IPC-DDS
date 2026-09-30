/* R2/t42 故障注入器（**工装侧，不是产品代码**）：把 `ipc::recv_wait_set::add` 强制返回 false。
 *
 * 为什么能构成「后端不可用」而不是"改产品代码"：
 *   libipc 对 `add` 的调用走**它自己的 PLT**（`objdump -d build/lib/libipc.so.1.3.0` 可见
 *   `call _ZN3ipc13recv_wait_set3addERKNS_15recv_wait_tokenE@plt`），而该符号在动态符号表里是
 *   `GLOBAL DEFAULT`（`readelf --dyn-syms` 第 268 行）、**无符号版本脚本约束**（`.gnu.version`
 *   中非 `*global*`）。⇒ `LD_PRELOAD` 的全局定义可抢先解析。
 *   ⛔ 不改任何产品源文件、不改 ABI、不改库。
 *
 * 构建（见 w10_r2_driver.sh）：
 *   g++ -std=c++17 -O2 -fPIC -shared -I include test/perf/w10/force_backend_unavailable.cpp \
 *       -o build/bin/libforce_backend.so
 * 使用：LD_PRELOAD=<...>/libforce_backend.so build/bin/w10_r2_minimal --case 4 --arm inject
 */
#include <atomic>
namespace ipc {
class recv_wait_token;
class recv_wait_set
{
public:
    bool add(const recv_wait_token&);
};
bool recv_wait_set::add(const recv_wait_token&)
{
    return false;   /* 强制"后端不可用" */
}
}  // namespace ipc
