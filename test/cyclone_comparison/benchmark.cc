// 固定速率、本机跨进程：各后端都使用事件等待，原始样本在应用层取得后计时。
#include "dzIPC/dzipc.h"
#include "dzIPC/common/sample_message.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include <dds/dds.h>
#include <dds/ddsc/dds_loan_api.h>
#include "types_select.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
using u64 = std::uint64_t;
static u64 now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
static void check(int rc, const char* what) { if (rc < 0) throw std::runtime_error(std::string(what)+": "+dds_strretcode(-rc)); }
static bool shared_pointer(const void* ptr) {
    std::ifstream maps("/proc/self/maps"); std::string line;
    const auto address = reinterpret_cast<std::uintptr_t>(ptr);
    while (std::getline(maps, line)) {
        std::uintptr_t lo=0, hi=0;
        if (std::sscanf(line.c_str(), "%lx-%lx", &lo, &hi)==2 && lo<=address && address<hi)
            return line.find("/dev/shm/")!=std::string::npos;
    }
    return false;
}

struct DDS {
    dds_entity_t participant=0, endpoint=0, waitset=0;
    bool shm=false, loan=false;
    DDS(bool publisher, const std::string& topic, size_t bytes) {
        participant=dds_create_participant(181,nullptr,nullptr); check(participant,"participant");
        auto t=dds_create_topic(participant,descriptor(bytes),topic.c_str(),nullptr,nullptr); check(t,"topic");
        auto* q=dds_create_qos();
        dds_qset_reliability(q,DDS_RELIABILITY_RELIABLE,DDS_MSECS(100));
        dds_qset_history(q,DDS_HISTORY_KEEP_LAST,64);
        dds_qset_durability(q,DDS_DURABILITY_VOLATILE);
        endpoint=publisher?dds_create_writer(participant,t,q,nullptr):dds_create_reader(participant,t,q,nullptr);
        dds_delete_qos(q); check(endpoint,"endpoint");
        shm=dds_is_shared_memory_available(endpoint); loan=dds_is_loan_available(endpoint);
        if (!publisher) {
            auto condition=dds_create_readcondition(endpoint,DDS_ANY_STATE); check(condition,"readcondition");
            waitset=dds_create_waitset(participant); check(waitset,"waitset");
            check(dds_waitset_attach(waitset,condition,1),"attach");
        }
    }
    ~DDS() { if (participant>0) dds_delete(participant); }
};

int main(int argc, char** argv) try {
    if(argc!=10) throw std::runtime_error("参数：pub|sub shm|shared|dds-udp|dds-iox topic bytes output rate subscribers publishers publisher_id");
    const std::string role=argv[1], backend=argv[2], topic=argv[3];
    const size_t bytes=std::stoull(argv[4]); const unsigned rate=std::stoul(argv[6]), subscribers=std::stoul(argv[7]);
    const u64 publishers=std::stoull(argv[8]),publisher_id=std::stoull(argv[9]);
    if ((role!="pub"&&role!="sub") || (backend!="shm"&&backend!="shared"&&backend!="dds-udp"&&backend!="dds-iox") || !rate || !subscribers || bytes<64)
        throw std::runtime_error("不支持的参数");
    const bool isdds=backend.rfind("dds-",0)==0, isiox=backend=="dds-iox", publisher=role=="pub";
    dzIPC::EnableDzFlat(true); dzIPC::EnableNodelet(false);
    std::unique_ptr<DDS> dds;
    auto model=std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(),71);
    auto transport=backend=="shared"?dzIPC::IPC_SOCKET:dzIPC::IPC_SHM;
    dzIPC::PublisherIPCPtr pub;
    dzIPC::SubscriberIPCPtr sub;
    if(isdds) {
        dds.reset(new DDS(publisher,topic,bytes));
        if(isiox && (!dds->shm || (publisher&&!dds->loan))) throw std::runtime_error("DDS 共享内存/借样不可用");
        if(!isiox && dds->shm) throw std::runtime_error("UDP 对照意外启用了共享内存");
    } else if(publisher) {
        pub=dzIPC::PublisherIPCPtrMake(model,topic,0,transport); pub->InitChannel();
    } else {
        sub=dzIPC::SubscriberIPCPtrMake(model,topic,0,64,transport); sub->InitChannel();
    }
    std::ofstream csv(argv[5]); if(!csv) throw std::runtime_error("无法创建 CSV");
    if(!publisher) {
        csv<<"publisher_id,sequence,read_ns,elapsed_ns,bytes,payload_bytes\n";
        std::atomic<bool> running{true}; u64 count=0,invalid=0; bool receive_ptr_shm=false,probed=false;
        std::exception_ptr error;
        std::thread reader([&] { try {
            while(running.load()) {
                dzIPC::Sample sample; void* data_sample=nullptr; dds_sample_info_t info{};
                const unsigned char* data=nullptr; size_t sample_bytes=bytes; u64 received=0;
                if(isdds) {
                    int n=dds_take(dds->endpoint,&data_sample,&info,1,1); check(n,"take");
                    if(!n) { dds_attach_t triggered; check(dds_waitset_wait(dds->waitset,&triggered,1,DDS_MSECS(20)),"wait"); continue; }
                    received=now_ns();
                    if(!info.valid_data) { check(dds_return_loan(dds->endpoint,&data_sample,1),"return loan"); continue; }
                    data=static_cast<unsigned char*>(data_sample);
                } else {
                    if(!sub->get(sample,20)) continue;
                    received=now_ns(); sample_bytes=sample.size();
                    auto view=sample.view<dzIPC::Msg::StdImageFlat>();
                    if(!view.valid()||view.data().size()!=bytes) { ++invalid; continue; }
                    data=view.data().data();
                }
                u64 stamp=0,sequence=0,source=0; std::memcpy(&stamp,data,8); std::memcpy(&sequence,data+8,8); std::memcpy(&source,data+17,8);
                bool valid=received>=stamp && source<publishers;
                for(size_t i=25;i<bytes;++i) valid&=data[i]==0xa5;
                if(!valid) ++invalid;
                if(!probed) { receive_ptr_shm=shared_pointer(data); probed=true; }
                if(data[16]) { ++count; csv<<source<<','<<sequence<<','<<received<<','<<(received-stamp)<<','<<sample_bytes<<','<<bytes<<'\n'; }
                if(isdds) check(dds_return_loan(dds->endpoint,&data_sample,1),"return loan");
            }
        } catch(...) { error=std::current_exception(); } });
        std::cout<<"{\"ready\":true,\"pid\":"<<getpid()<<",\"shm_endpoint\":"<<(dds&&dds->shm?"true":"false")<<"}"<<std::endl;
        std::string command; std::getline(std::cin,command); running.store(false); reader.join();
        if(error) std::rethrow_exception(error);
        csv.close();
        std::cout<<"{\"received\":"<<count<<",\"invalid\":"<<invalid<<",\"receive_pointer_in_shm\":"<<(receive_ptr_shm?"true":"false")<<"}"<<std::endl;
        return invalid?1:0;
    }
    const auto match_deadline=Clock::now()+std::chrono::seconds(15);
    bool matched=false;
    while(Clock::now()<match_deadline) {
        if(isdds) { dds_publication_matched_status_t st{}; check(dds_get_publication_matched_status(dds->endpoint,&st),"matched"); matched=st.current_count>=subscribers; }
        else matched=pub->has_subscribed();
        if(matched) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if(!matched) throw std::runtime_error("发布订阅匹配超时");
    bool loan_in_shm=false;
    if(isiox) { void* loan=nullptr; check(dds_loan_sample(dds->endpoint,&loan),"probe loan"); loan_in_shm=shared_pointer(loan); check(dds_return_loan(dds->endpoint,&loan,1),"probe return"); if(!loan_in_shm) throw std::runtime_error("DDS 借样不在共享内存"); }
    std::vector<unsigned char> blob; size_t offset=0;
    if(isdds) blob.assign(bytes,0xa5);
    else {
        dzIPC::Msg::StdImage message; message.set_msg_id(71); message.width=bytes; message.height=1; message.data.assign(bytes,0xa5);
        blob.resize(message.dzflat_size());
        if(!message.dzflat_write(blob.data(),blob.size())) throw std::runtime_error("预构造失败");
        offset=dzIPC::Msg::StdImageFlat::view_t::bind(blob.data(),blob.size()).data().data()-blob.data();
    }
    std::cout<<"{\"ready\":true,\"pid\":"<<getpid()<<",\"loan_in_shm\":"<<(loan_in_shm?"true":"false")<<",\"sample_bytes\":"<<blob.size()<<"}"<<std::endl;
    unsigned seconds=0; u64 scheduled_start=0; std::cin>>seconds>>scheduled_start; if(!seconds || !publishers || publisher_id>=publishers) throw std::runtime_error("时长/发布者身份不合法");
    const auto start=Clock::time_point(std::chrono::nanoseconds(scheduled_start)); const u64 total=u64(seconds+2)*rate;
    u64 accepted=0,rejected=0,late=0,max_late=0;
    csv<<"publisher_id,sequence,start_ns,elapsed_ns,api_ns,success,bytes,schedule_late_ns,payload_bytes\n";
    for(u64 sequence=0;sequence<total;++sequence) {
        const auto scheduled=start+std::chrono::nanoseconds(sequence*1000000000ULL/rate);
        std::this_thread::sleep_until(scheduled);
        const u64 stamp=now_ns(),due=std::chrono::duration_cast<std::chrono::nanoseconds>(scheduled.time_since_epoch()).count();
        const bool measured=sequence>=u64(2)*rate;
        std::memcpy(blob.data()+offset,&stamp,8); std::memcpy(blob.data()+offset+8,&sequence,8); blob[offset+16]=measured;
        std::memcpy(blob.data()+offset+17,&publisher_id,8);
        u64 entered=now_ns(); bool ok=false;
        if(isdds) {
            void* sample=blob.data();
            if(isiox) { check(dds_loan_sample(dds->endpoint,&sample),"loan"); std::memcpy(sample,blob.data(),blob.size()); }
            entered=now_ns(); ok=dds_write(dds->endpoint,sample)==DDS_RETCODE_OK;
        } else ok=pub->publish_prebuilt_segment(blob.data(),blob.size());
        const u64 returned=now_ns();
        if(measured) {
            ok?++accepted:++rejected; const u64 lateness=stamp>due?stamp-due:0;
            if(lateness>=1000000000ULL/rate) ++late;
            if(lateness>max_late) max_late=lateness;
            csv<<publisher_id<<','<<sequence<<','<<stamp<<','<<returned-stamp<<','<<returned-entered<<','<<ok<<','<<blob.size()<<','<<lateness<<','<<bytes<<'\n';
        }
    }
    std::this_thread::sleep_until(start+std::chrono::seconds(seconds+2)); csv.close();
    const u64 origin=std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count();
    std::cout<<"{\"accepted\":"<<accepted<<",\"rejected\":"<<rejected<<",\"late_periods\":"<<late<<",\"max_late_ns\":"<<max_late
             <<",\"window_start_ns\":"<<origin+2000000000ULL<<",\"window_end_ns\":"<<origin+u64(seconds+2)*1000000000ULL<<",\"finish_ns\":"<<now_ns()<<"}"<<std::endl;
    std::string command; std::getline(std::cin,command); std::getline(std::cin,command);
    return rejected?1:0;
} catch(const std::exception& e) { std::cerr<<"基准失败："<<e.what()<<'\n'; return 1; }
