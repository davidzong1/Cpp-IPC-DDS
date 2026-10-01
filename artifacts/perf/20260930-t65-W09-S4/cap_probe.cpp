/* t65：运行时直读每档块数 + 归属键，⛔ 不采信文档 */
#include <cstdio>
#include "libipc/def.h"
#include "libipc/utility/id_pool.h"
int main(){
  std::printf("large_msg_cache=%d\n", (int)ipc::large_msg_cache);
  std::printf("id_pool<>::max_count=%zu\n", (size_t)ipc::id_pool<>::max_count);

  return 0;
}
