// 资源探针：仅在 isolated.sh 中运行。首尾触页，不做全载荷填充吞吐测试。
// 编译与调用方式见 topic_pool_results.md。
#include "libipc/ipc.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
struct Topic {
    std::unique_ptr<ipc::route> tx, rx;
    std::vector<ipc::loan_t> loans;
    ~Topic() { if (tx) for (const auto& lo : loans) tx->discard_loan(lo); }
};
void snapshot(const std::string& stage, const std::string& token) {
    std::uint64_t count=0, logical=0, allocated=0, pool_count=0, pool_logical=0, pool_allocated=0;
    for (const auto& entry : fs::directory_iterator("/dev/shm")) {
        const auto name=entry.path().filename().string();
        if (name.find(token)==std::string::npos) continue;
        struct stat st{};
        if (::stat(entry.path().c_str(),&st)) throw std::runtime_error("共享段 stat 失败");
        ++count;logical+=st.st_size;allocated+=st.st_blocks*512;
        if (name.find("DZFLAT_TOPIC_V1__")!=std::string::npos && name.find("CHUNK_INFO__")!=std::string::npos) {
            ++pool_count;pool_logical+=st.st_size;pool_allocated+=st.st_blocks*512;
        }
    }
    std::ifstream status("/proc/self/status");std::string line;
    std::uint64_t rss=0,vms=0;
    while(std::getline(status,line)) {
        if(line.rfind("VmRSS:",0)==0)rss=std::stoull(line.substr(6));
        if(line.rfind("VmSize:",0)==0)vms=std::stoull(line.substr(7));
    }
    std::cout<<"{\"stage\":\""<<stage<<"\",\"segments\":"<<count
             <<",\"logical_bytes\":"<<logical<<",\"allocated_bytes\":"<<allocated
             <<",\"pools\":"<<pool_count<<",\"pool_logical_bytes\":"<<pool_logical
             <<",\"pool_allocated_bytes\":"<<pool_allocated
             <<",\"rss_kib\":"<<rss<<",\"vms_kib\":"<<vms<<"}"<<std::endl;
}
int main(int argc,char** argv) {
    if(argc!=3) {std::cerr<<"用法：探针 话题数 每条请求字节数\n";return 2;}
    const unsigned topics=std::stoul(argv[1]);const std::size_t bytes=std::stoull(argv[2]);
    if(!topics || topics>1000 || !bytes || bytes>1048577)return 2;
    const auto token="topic_resource_"+std::to_string(getpid());
    std::vector<std::unique_ptr<Topic>> items;
    snapshot("before",token);
    for(unsigned i=0;i<topics;++i) {
        auto t=std::make_unique<Topic>();const auto name=token+"_"+std::to_string(i);
        t->tx=std::make_unique<ipc::route>(name.c_str(),ipc::sender,false);
        t->rx=std::make_unique<ipc::route>(name.c_str(),ipc::receiver,false);
        if(!t->tx->wait_for_recv(1,1000))return 3;
        for(unsigned j=0;j<10;++j) {
            auto lo=t->tx->loan_topic(bytes);
            if(!lo.valid())return 4;
            auto* p=static_cast<volatile unsigned char*>(lo.data);
            p[0]=static_cast<unsigned char>(i);p[bytes-1]=static_cast<unsigned char>(j);
            t->loans.push_back(lo);
        }
        ipc::loan_status reason{};
        if(t->tx->loan_topic(bytes,reason).valid() || reason!=ipc::loan_status::pool_exhausted)return 5;
        items.push_back(std::move(t));
    }
    snapshot("held_ten_each",token);
    for(auto& t:items) {
        for(auto& lo:t->loans)t->tx->discard_loan(lo);
        t->loans.clear();
        auto lo=t->tx->loan_topic(bytes);
        if(!lo.valid())return 6;
        t->tx->discard_loan(lo);
    }
    snapshot("returned",token);
    items.clear();
    snapshot("routes_destroyed",token);
    return 0;
}
