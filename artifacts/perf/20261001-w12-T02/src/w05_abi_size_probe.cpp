/* T02 §4.3.5 ABI 探针：在**修复前**（dcaa0d9 前缀树）与**修复后**（工作区）两棵头文件树上
 * 各编译一次，输出导出抽象基类的 sizeof / alignof。
 *
 * ⛔ 本探针只读；不写段、不起线程、不调用任何产品函数。
 * 用法: g++ -std=c++17 -I <tree>/include -I <tree>/src ... abi_size_probe.cpp
 */
#include <cstdio>

#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/shm_control_scheduler.h"

int main()
{
    using dzIPC::shm_control::PubControlState;
    using dzIPC::shm_control::SubControlState;
    std::printf("sizeof(PubControlState)=%zu\n", sizeof(PubControlState));
    std::printf("alignof(PubControlState)=%zu\n", alignof(PubControlState));
    std::printf("sizeof(SubControlState)=%zu\n", sizeof(SubControlState));
    std::printf("sizeof(shm_pub_ipc)=%zu\n", sizeof(dzIPC::shm::shm_pub_ipc));
    std::printf("sizeof(ControlTiming)=%zu\n", sizeof(dzIPC::shm_control::ControlTiming));
    std::printf("T02_ABI_SIZE_DONE\n");
    return 0;
}
