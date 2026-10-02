#include "trace.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include <array>
#include <vector>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <ctime>
namespace breakdown {
namespace {
constexpr std::size_t Capacity = 262144;
std::array<std::vector<std::uint64_t>, Count> columns;
bool active=false;
std::uint64_t stride=64, phase=1;
thread_local std::size_t selected=Capacity;
thread_local std::uint64_t worker_enter=0;
const char* names[]={"produced","publish_enter","submit_begin","publish_return","received",
                     "lease_released","validated","allocated","queue_visible",
                     "take_begin","take_end","app","worker_enter"};
}
bool enabled() noexcept { return active; }
std::uint64_t now() noexcept {
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC,&ts);
    return std::uint64_t(ts.tv_sec)*1000000000ULL+ts.tv_nsec;
}
void init() {
    const char* on=std::getenv("BREAKDOWN_TRACE");
    active=on && std::atoi(on)!=0;
    if(const char* p=std::getenv("BREAKDOWN_PHASE")) phase=std::strtoull(p,nullptr,10)%stride;
    if(active) for(auto& c:columns) c.assign(Capacity,0); // 预触页，避免首次采样分配。
}
void begin(std::uint64_t seq) noexcept {
    selected=active && seq%stride==phase && seq/stride<Capacity ? seq/stride : Capacity;
}
void stamp(Stage s,std::uint64_t ns) noexcept {
    if(selected<Capacity) columns[s][selected]=ns;
}
void mark(Stage s) noexcept { if(selected<Capacity) columns[s][selected]=now(); }
void worker_begin(std::uint64_t ns) noexcept { if(active) worker_enter=ns; }
void received(const void* data,std::size_t size) noexcept {
    selected=Capacity;
    if(!active || !data || !size) return;
    auto v=dzIPC::Msg::StdImageView::bind(data,size);
    if(!v.valid()) return;
    begin(v.width());
    stamp(WorkerEnter,worker_enter);
    mark(Received); // 包含探针用于识别序号的只读视图绑定成本。
}
void dump(const char* path) {
    if(!active) return;
    std::ofstream out(path);
    if(!out) throw std::runtime_error("无法写入分段计时数据");
    out<<"seq";
    for(const auto* name:names) out<<','<<name;
    out<<'\n';
    for(std::size_t i=0;i<Capacity;++i) {
        if(!columns[Produced][i] && !columns[Received][i] && !columns[App][i]) continue;
        out<<i*stride+phase;
        for(const auto& c:columns) out<<','<<c[i];
        out<<'\n';
    }
}
}
