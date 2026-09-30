/* t46 要求 5 的影子验证：**用补丁后的头文件**（唯一改动 = 应用调用点）+ 我的 libipc
 * 原因出口，走真实 B 借样路径，证明 `borrow_failed_pool_exhausted` 0 → 非 0，
 * 且传输层 A/TLV 腿的 `chunk_exhausted` **不被污染**（三类互不冒充）。
 * ⛔ shadow/ 只是**证据副本**：真树未被改动（交付 §6 有范围声明与 diff 核对）。 */
#include <cstdio>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/measure/counters.h"
#include "ipc_msg/std_msgs/std_string.hpp"
using dzIPC::measure::CounterId;
using dzIPC::measure::CounterRegistry;
static unsigned long long g(CounterId i){ return (unsigned long long)CounterRegistry::instance().get(i); }
static void dump(const char* t){
  std::printf("%s io_pool=%llu io_oversized=%llu io_norx=%llu io_publish=%llu io_unknown=%llu "
              "| cap chunk_ex=%llu chunk_af=%llu\n", t,
    g(CounterId::borrow_failed_pool_exhausted), g(CounterId::borrow_failed_oversized),
    g(CounterId::borrow_failed_no_receiver), g(CounterId::borrow_failed_publish),
    g(CounterId::borrow_failed_reason_unknown),
    g(CounterId::chunk_exhausted), g(CounterId::chunk_alloc_failed));
  std::fflush(stdout);
}
static std::shared_ptr<dzIPC::TopicData> td()
{ return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 71); }
int main(){
  dzIPC::EnableDzFlat(true);
  auto a=td(), b=td();
  dzIPC::shm::shm_pub_ipc pub{a,"patchshadow2",0,false};
  dzIPC::shm::shm_sub_ipc sub{b,"patchshadow2",0,8,false};
  pub.InitChannel(); sub.InitChannel();
  /* ⛔ 必须等握手完成：`loan<Flat>()` 不加 recv_count 前置（`try_publish_dzflat` 才加），
   * 未握手时它直接落 no_receiver —— 那是**正确**归因而非池耗尽。本包第一版即被这一点
   * 抓到（io_norx=120 而 io_pool=0），记入交付 §3.3 的构造陷阱。 */
  for (int i=0;i<60;++i) {
    auto m=std::make_shared<dzIPC::Msg::StdString>(); m->set_msg_id(71); m->str="w";
    pub.publish(m);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  dump("BEFORE");
  std::vector<dzIPC::LoanedMessage<dzIPC::Msg::StdStringFlat>> held;
  int ok=0, rejected=0;
  for (int i=0;i<120;++i){
    auto lo = pub.loan<dzIPC::Msg::StdStringFlat>(64*1024);
    if(!lo.valid()){ ++rejected; continue; }
    held.push_back(std::move(lo)); ++ok;
  }
  std::printf("loaned=%d rejected=%d\n", ok, rejected);
  dump("AFTER ");
  held.clear();
  std::printf("PATCH_SHADOW_DONE\n");
  return 0;
}
