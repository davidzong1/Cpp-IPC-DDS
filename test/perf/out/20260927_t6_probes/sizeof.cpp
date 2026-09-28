#include <dzIPC/socket_pub_sub_ipc.h>
#include <dzIPC/socket_ser_cli_ipc.h>
#include <dzIPC/shm_ser_cli_ipc.h>
#include <libipc/udp.h>
#include <cstdio>
int main(){
  std::printf("socket_sub_ipc=%zu\n", sizeof(dzIPC::socket::socket_sub_ipc));
  std::printf("socket_pub_ipc=%zu\n", sizeof(dzIPC::socket::socket_pub_ipc));
  std::printf("socket_ser_ipc=%zu\n", sizeof(dzIPC::socket::socket_ser_ipc));
  std::printf("shm_ser_ipc=%zu\n", sizeof(dzIPC::shm::shm_ser_ipc));
  std::printf("UDPNode=%zu\n", sizeof(ipc::socket::UDPNode));
  return 0;
}
