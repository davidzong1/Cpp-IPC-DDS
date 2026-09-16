/* DZFlat 传输层基准 (docs/dzflat_shm.md Step 2 验收门)
 *
 * 与 dzflat_benchmark.cpp 的区别: 那个是**离线**比较布局本身(不碰传输), 这个走真实
 * 的 shm_pub_ipc / shm_sub_ipc 链路, 测的是收益进入传输路径之后还剩多少。
 *
 * 口径:
 *   publish 列 —— 发布方把一条消息交出去的成本。TLV = serialize() 整包 new + 页尾
 *                 分段拷贝 + send() 再 memcpy 进 chunk; DZFlat = 借一块 chunk 并
 *                 就地按平坦布局写一遍。这是 Step 2 的主指标。
 *   取出 列   —— 只是从订阅队列 pop 一个 shared_ptr。**解码发生在订阅者的后台线程**
 *                 (见 shm_sub_ipc::InitChannel 的 subscribe_thread_), 不在这里,
 *                 所以这一列不能当作解码成本的对比, 仅用于确认消息确实送达。
 *   计数器    —— dzflat/fallback 证明每一轮确实走了预期的路径而非静默回退。
 *
 * 发一条取一条, 不能连发不取: chunk 池只有 32 块/尺寸档位, 堆满后大消息会退化成
 * 64 字节分片打进 256 槽的环, 触发既有的重组缺陷(与 DZFlat 无关, 基线同样崩 ——
 * 见 docs/dzflat_shm.md §9.4)。
 *
 * 自带 main(), 不是 gtest。
 */
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_msg/std_msgs/std_point_cloud.hpp"
using namespace std::chrono_literals;

template<typename Msg>
void bench(const char* label, const Msg& src, const char* topic_base, int iters){
  for (int mode=0; mode<2; ++mode){
    const bool on = (mode==1);
    dzIPC::EnableDzFlat(on);
    dzIPC::ResetDzFlatCounters();
    std::string topic = std::string(topic_base) + (on?"_flat":"_tlv");
    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<Msg>(), 90);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<Msg>(), 90);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 64};
    pub.InitChannel(); sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    // 预热
    for (int i=0;i<5;++i){ auto m=std::make_shared<Msg>(src); m->set_msg_id(90); pub.publish(m); }
    std::this_thread::sleep_for(100ms);
    while (sub.try_get_clone(sub_td)) {}
    dzIPC::Sample _s;
    while (sub.try_get(_s)) {}   /* DZFlat 段在视图队列 */

    /* 发一条取一条。不能连发不取: chunk 池只有 32 块/档位, 堆满后大消息会退化成
     * 64 字节分片打进 256 槽的环, 触发既有的重组缺陷(与 DZFlat 无关, 基线同样崩)。 */
    double pub_us=0, sub_us=0; int got=0;
    for (int i=0;i<iters;++i){
      auto m = std::make_shared<Msg>(src); m->set_msg_id(90);
      auto a = std::chrono::steady_clock::now();
      pub.publish(m);
      auto b = std::chrono::steady_clock::now();
      pub_us += std::chrono::duration<double,std::micro>(b-a).count();
      auto deadline = std::chrono::steady_clock::now() + 200ms;
      while (std::chrono::steady_clock::now() < deadline){
        auto c = std::chrono::steady_clock::now();
        dzIPC::Sample smp;
        if (sub.try_get_clone(sub_td) || sub.try_get(smp)) {
          auto d = std::chrono::steady_clock::now();
          sub_us += std::chrono::duration<double,std::micro>(d-c).count();
          ++got; break;
        }
        std::this_thread::sleep_for(200us);
      }
    }
    pub_us /= iters; sub_us = got? sub_us/got : 0;

    printf("  %-10s %-6s publish %8.1f us/条   收到 %3d 条 (取出 %6.1f us/条)  dzflat=%llu fallback=%llu\n",
      label, on?"DZFlat":"TLV", pub_us, got, sub_us,
      (unsigned long long)dzIPC::DzFlatPublishCount(),
      (unsigned long long)dzIPC::DzFlatFallbackCount());
    fflush(stdout);
  }
}

/* B 级: 就地构造。与 A 级的差别是负载不再经过调用方的堆 —— 这里用"填充一块像素
 * 缓冲"模拟相机, A 级填到 std::vector 再 publish, B 级直接填到 chunk。 */
void bench_b_image(std::size_t w, std::size_t h, int iters){
  const std::uint32_t step = (std::uint32_t)(w*3), bytes = (std::uint32_t)(step*h);
  const char* topic="/b3/img";
  dzIPC::EnableDzFlat(true);
  auto pub_td=std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(),91);
  auto sub_td=std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(),91);
  dzIPC::shm::shm_pub_ipc pub{pub_td,topic,0};
  dzIPC::shm::shm_sub_ipc sub{sub_td,topic,0,64};
  pub.InitChannel(); sub.InitChannel();
  std::this_thread::sleep_for(300ms);

  for (int mode=0; mode<2; ++mode){
    const bool blevel = (mode==1);
    dzIPC::ResetDzFlatCounters();
    // 预热
    for (int i=0;i<3;++i){
      if (blevel){ auto lo=pub.loan<dzIPC::Msg::StdImageFlat>(bytes+256);
        if(lo.valid()){ lo->set_width((std::uint32_t)w); auto px=lo->alloc_data(bytes);
          for(std::uint32_t k=0;k<px.size();++k) px[k]=(std::uint8_t)k; pub.publish_loaned(std::move(lo)); } }
      else { auto m=std::make_shared<dzIPC::Msg::StdImage>(); m->set_msg_id(91);
        m->width=(std::uint32_t)w; m->data.resize(bytes);
        for(std::uint32_t k=0;k<bytes;++k) m->data[k]=(std::uint8_t)k; pub.publish(m); }
      std::this_thread::sleep_for(20ms); while(sub.try_get_clone(sub_td)){}
      { dzIPC::Sample _s2; while(sub.try_get(_s2)){} }
    }
    double us=0; int n=0;
    for (int i=0;i<iters;++i){
      auto a=std::chrono::steady_clock::now();
      bool okp=false;
      if (blevel){
        auto lo=pub.loan<dzIPC::Msg::StdImageFlat>(bytes+256);
        if (lo.valid()){
          lo->set_width((std::uint32_t)w); lo->set_height((std::uint32_t)h); lo->set_step(step);
          lo->set_encoding("rgb8");
          auto px=lo->alloc_data(bytes);
          for (std::uint32_t k=0;k<px.size();++k) px[k]=(std::uint8_t)(k&0xFF);  // "相机直写"
          okp = pub.publish_loaned(std::move(lo));
        }
      } else {
        auto m=std::make_shared<dzIPC::Msg::StdImage>(); m->set_msg_id(91);
        m->width=(std::uint32_t)w; m->height=(std::uint32_t)h; m->step=step; m->encoding="rgb8";
        m->data.resize(bytes);
        for (std::uint32_t k=0;k<bytes;++k) m->data[k]=(std::uint8_t)(k&0xFF); // 填到调用方堆
        okp = pub.publish(m);
      }
      auto b=std::chrono::steady_clock::now();
      if (okp){ us += std::chrono::duration<double,std::micro>(b-a).count(); ++n; }
      auto dl=std::chrono::steady_clock::now()+200ms;
      while (std::chrono::steady_clock::now()<dl){ dzIPC::Sample _s3;
        if (sub.try_get_clone(sub_td) || sub.try_get(_s3)) break; std::this_thread::sleep_for(200us); }
    }
    printf("  %-10s %-8s 构造+发布 %8.1f us/条  (成功 %d/%d)  dzflat=%llu fallback=%llu\n",
      "image", blevel?"B级就地":"A级拷贝", n?us/n:0, n, iters,
      (unsigned long long)dzIPC::DzFlatPublishCount(),(unsigned long long)dzIPC::DzFlatFallbackCount());
    fflush(stdout);
  }
}

int main(){
  printf("Step 2 传输层基准 (真实 SHM 链路)\n");
  dzIPC::Msg::StdImage img;
  img.header.frame_id="cam"; img.header.stamp=1.0;
  img.width=640; img.height=480; img.step=1920; img.encoding="rgb8";
  img.data.assign(640*480*3, 0x5A);
  bench("image640", img, "/b/img", 200);

  dzIPC::Msg::StdPointCloud pc;
  pc.header.frame_id="lidar"; pc.header.stamp=1.0;
  pc.points.resize(20000);
  for (size_t i=0;i<pc.points.size();++i) pc.points[i].data={double(i),double(i)*2,double(i)*3};
  pc.channel_names={"intensity","ring"}; pc.channels.assign(20000, 0.5);
  bench("cloud20k", pc, "/b/pc", 200);

  printf("\nStep 3: B 级就地构造 vs A 级(负载先填到调用方堆)\n");
  bench_b_image(640, 480, 150);
  return 0;
}
