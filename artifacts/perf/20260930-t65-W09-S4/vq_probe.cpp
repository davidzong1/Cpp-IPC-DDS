#include <cstdio>
#include "dzIPC/common/nodelet_config.h"
#include "libipc/def.h"
int main(){
  std::printf("ViewQueueCap()=%zu  (large_msg_cache=%d / 4)\n", dzIPC::ViewQueueCap(), (int)ipc::large_msg_cache);
  std::printf("IsViewQueuePinEnabled()=%d\n", (int)dzIPC::IsViewQueuePinEnabled());
  return 0;
}
