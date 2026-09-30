/* t46 关键可达性核对：`loan<Flat>(varlen_budget)` 的预算类型是 **uint32_t**
 * ⇒ 它能请求到的最大档位远小于 `size_too_large` 阈值。必须如实测出，否则会造出
 * "有写入者但永不触发"的假接线。
 * 判据用 ipc::large_msg_cache(=池每档块数, def.h) 而不是内部 id_pool。 */
#include <cstdio>
#include <cstdint>
#include <limits>
#include "ipc_msg/std_msgs/std_string.hpp"
#include "libipc/def.h"
#include "libipc/ipc.h"
using Flat = dzIPC::Msg::StdStringFlat;
int main()
{
  const std::size_t max_count = static_cast<std::size_t>(ipc::large_msg_cache);
  const std::size_t kMaxChunkSize =
      ((std::numeric_limits<std::size_t>::max)() - 4096) / max_count;
  const std::uint32_t max_budget = (std::numeric_limits<std::uint32_t>::max)();
  const std::uint32_t sz_max = Flat::loan_size(max_budget);
  /* loan_size_class 会把 >64KiB 的档位取到 2 的幂 */
  std::uint64_t klass = 128 * 1024;
  while (klass < sz_max) klass <<= 1;
  std::printf("budget_type=uint32_t  max_budget=%u  Flat::loan_size(max)=%u  => loan_size_class≈%llu\n",
              max_budget, sz_max, (unsigned long long)klass);
  std::printf("kMaxChunkSize≈%zu  (criterion: cap > kMaxChunkSize ⇒ size_too_large)\n", kMaxChunkSize);
  std::printf("⇒ 本调用点能否触发 size_too_large: cap(%llu) > max(%zu) ? %s\n",
              (unsigned long long)klass, kMaxChunkSize,
              (klass > kMaxChunkSize) ? "能" : "**不能（算术上不可达）**");
  return 0;
}
