/* 制造「死进程未发布借样」：借满 132096 档的 N 块并**不发布**，然后由外部 SIGKILL。
 * 用于反证 reclaim_dead_chunks 的 published==0 跳过路径。 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <unistd.h>
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_point_cloud.hpp"

int main(int argc, char** argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 40;
    const std::uint32_t kN = 3000;   /* 与 DzFlatBuilder.Tier0 同一 budget ⇒ 同一 chunk 尺寸档 */
    const int do_publish = (argc > 2 && std::string(argv[2]) == "publish") ? 1 : 0;   /* 反事实开关 */
    dzIPC::EnableDzFlat(true);
    const std::string topic = (argc > 3) ? std::string(argv[3]) : (std::string("leak_") + std::to_string(getpid()));
    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdPointCloud>(), 61);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdPointCloud>(), 61);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel(); sub.InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int ok = 0;
    std::vector<decltype(pub.loan<dzIPC::Msg::StdPointCloudFlat>(131072))> held;
    (void)held;
    for (int i = 0; i < n; ++i) {
        const std::uint32_t budget = kN * sizeof(dzIPC::Msg::StdVector3dRoot) + kN * sizeof(double) + 512;
        auto lo = pub.loan<dzIPC::Msg::StdPointCloudFlat>(budget);
        if (!lo.valid()) break;
        (void)lo;
        auto pts = lo->alloc_points(kN); (void)pts;
        if (do_publish) { (void)pub.publish_loaned(std::move(lo)); }
        else { held.push_back(std::move(lo)); }   /* 故意不发布 */
        ++ok;
    }
    std::printf("loaned=%d mode=%s\n", ok, do_publish ? "published" : "unpublished"); std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(30));   /* 等外部 SIGKILL */
    std::printf("NOT_KILLED\n");
    return 0;
}
