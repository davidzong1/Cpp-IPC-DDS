// 跨进程工装：应用载荷每条重新填充；控制文件仅传就绪/停止/信用，不传用户数据。
#include <dds/dds.h>
#include <dds/ddsc/dds_loan_api.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include "types_select.hpp"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "dzIPC/socket_ser_cli_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "dzIPC/common/wire_accept.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"

using u64 = uint64_t;
using Clock = std::chrono::steady_clock;
using Img = dzIPC::Msg::StdImage;
using Flat = dzIPC::Msg::StdImageFlat;
using Req = dzIPC::Srv::RequestResponseTestRequest;
using Res = dzIPC::Srv::RequestResponseTestResponse;
using ReqFlat = dzIPC::Srv::RequestResponseTestRequestFlat;
using ResFlat = dzIPC::Srv::RequestResponseTestResponseFlat;
constexpr size_t MAX_TOPICS = 1000;
u64 now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
void nap() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
struct Control {
    std::atomic<u64> ready{0}, done{0}, start{UINT64_MAX}, end{UINT64_MAX};
    std::atomic<u64> ack[MAX_TOPICS]{};
    std::atomic<u64> expected[MAX_TOPICS]{}, delivered[MAX_TOPICS]{}, final_seq[MAX_TOPICS]{};
    std::atomic<u64> drained{0};
};
static_assert(std::atomic<u64>::is_always_lock_free, "跨进程控制需要无锁原子量");
struct Config {
    std::string backend, mode, role, control, out, token;
    size_t bytes=64, topics=1;
    unsigned domain=173,lanes=1;
    double seconds=1;
    bool full=false;
};
struct Metrics {
    u64 received=0, bad=0, duplicate=0, gaps=0, last=0, in_window=0;
    std::vector<double> latency;
    std::vector<u64> seen;
    std::mutex mu;
};
struct DDSMeta { u64 seq, stamp; };
bool in_shared_mapping(const void* pointer) {
    static const auto ranges=[] {
        std::vector<std::pair<uintptr_t,uintptr_t>> out;
        std::ifstream f("/proc/self/maps");std::string line;
        while(std::getline(f,line)) if(line.find("/dev/shm/")!=std::string::npos) {
            std::istringstream is(line);std::string address;is>>address;
            auto dash=address.find('-');
            out.emplace_back(std::stoull(address.substr(0,dash),nullptr,16),std::stoull(address.substr(dash+1),nullptr,16));
        }
        return out;
    }();
    auto p=reinterpret_cast<uintptr_t>(pointer);
    for(auto r:ranges) if(p>=r.first && p<r.second) return true;
    return false;
}
double cpu_seconds() {
    rusage r{}; getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_utime.tv_usec/1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec/1e6;
}
double main_cpu_seconds() {
    rusage r{}; getrusage(RUSAGE_THREAD,&r);
    return r.ru_utime.tv_sec+r.ru_utime.tv_usec/1e6+r.ru_stime.tv_sec+r.ru_stime.tv_usec/1e6;
}
int threads() {
    int n=0; for (auto& unused : std::filesystem::directory_iterator("/proc/self/task")) { (void)unused; ++n; } return n;
}
// 全字节正确性档使用固定种子的非均匀内容；普通速度档保留 memset 工作量。
uint8_t payload_byte(u64 seq, size_t offset) {
    u64 x=seq*0x9e3779b97f4a7c15ULL+u64(offset)*0xbf58476d1ce4e5b9ULL;
    x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;
    x=(x^(x>>27))*0x94d049bb133111ebULL;
    return uint8_t(x^(x>>31));
}
void fill_payload(uint8_t* data,size_t len,u64 seq,bool full) {
    if(full) for(size_t k=0;k<len;++k) data[k]=payload_byte(seq,k);
    else std::memset(data,uint8_t(seq),len);
}
bool check_payload(const uint8_t* data,size_t len,u64 seq,bool full) {
    if(!len) return true;
    if(full) {
        for(size_t k=0;k<len;++k) if(data[k]!=payload_byte(seq,k)) return false;
        return true;
    }
    return data[0]==uint8_t(seq) && data[len-1]==uint8_t(seq);
}
struct Histogram {
    // 精确保存速度样本；压力由确定性每百条抽样，避免逐消息计时记录成为主要负载。
    static void write(std::ostream& o, std::vector<double> v, const std::string& key) {
        std::sort(v.begin(),v.end());
        auto q=[&](double p){return v.empty()?0:v[std::min(v.size()-1,size_t(std::ceil(p*v.size())-1))];};
        o << ",\"" << key << "_n\":" << v.size()
          << ",\"" << key << "_mean_us\":" << (v.empty()?0:std::accumulate(v.begin(),v.end(),0.0)/v.size())
          << ",\"" << key << "_p50_us\":" << q(.5)
          << ",\"" << key << "_p95_us\":" << q(.95)
          << ",\"" << key << "_p99_us\":" << q(.99)
          << ",\"" << key << "_max_us\":" << (v.empty()?0:v.back());
    }
};
struct Bench {
    Config c;
    Control* ctl;
    std::vector<std::unique_ptr<Metrics>> metrics;
    std::vector<std::shared_ptr<dzIPC::pub_ipc_base>> pubs;
    std::vector<std::shared_ptr<dzIPC::sub_ipc_base>> subs;
    std::vector<std::shared_ptr<dzIPC::TopicData>> td;
    dds_entity_t dp=0;
    std::vector<dds_entity_t> writers, readers;
    struct Ctx { Bench* b; size_t i; bool reply; };
    std::vector<std::unique_ptr<Ctx>> contexts;
    std::unique_ptr<dzIPC::ser_ipc_base> server;
    std::unique_ptr<dzIPC::cli_ipc_base> client;
    std::shared_ptr<dzIPC::ServiceData> sd;
    u64 shm_available=0, loan_available=0;
    std::atomic<u64> reply_ok{0},reply_bad{0},request_views{0},response_views{0};
    std::atomic<u64> data_in_shm{0},data_on_heap{0},loan_in_shm{0},loan_on_heap{0};
    std::atomic<u64> response_seq{0};
    u64 attempts=0, sent=0, failed=0, plan=0, warmup_retries=0;
    u64 window=8, offered_rate=0, idle_ns=0, message_limit=0, progress_deadline_ns=1000000000ULL;
    bool no_retry=false;
    std::vector<u64> topic_sent;
    u64 stalled=0;
    std::atomic<u64> loan_failed{0};
    std::mutex timing_mutex;
    std::vector<double> send_times, prepare_times, rpc_times;
    double cpu0=0, main_cpu0=0, elapsed=0;
    u64 counter_tlv=0,counter_a=0,counter_b=0,counter_pre=0;
    bool isdds() const { return c.backend.rfind("dds",0)==0; }
    bool isiox() const { return c.backend=="dds-iox"; }
    bool issocket() const { return c.backend=="socket"; }
    bool pressure() const { return c.mode=="stress"; }
    bool rpc() const { return c.mode=="rpc"; }
    Bench(Config v, Control* p):c(std::move(v)),ctl(p) {
        for(size_t i=0;i<c.topics;++i) metrics.emplace_back(new Metrics);
        topic_sent.resize(c.topics);
        if(const char* v=std::getenv("BREAKDOWN_WINDOW")) window=std::stoull(v);
        if(const char* v=std::getenv("BREAKDOWN_RATE")) offered_rate=std::stoull(v);
        if(const char* v=std::getenv("COMPARISON_WINDOW")) window=std::stoull(v);
        if(const char* v=std::getenv("COMPARISON_RATE")) offered_rate=std::stoull(v);
        if(const char* v=std::getenv("COMPARISON_IDLE_NS")) idle_ns=std::stoull(v);
        if(const char* v=std::getenv("COMPARISON_COUNT")) message_limit=std::stoull(v);
        if(idle_ns && (window!=1 || offered_rate)) throw std::runtime_error("静默档需要单条在途且不叠加限速");
        no_retry=std::getenv("COMPARISON_NO_RETRY")!=nullptr;
        if(!window) throw std::runtime_error("在途窗口必须大于零");
    }
    std::string topic(size_t i) const {return "cmp_"+c.token+"_"+std::to_string(i);}
    void check(dds_entity_t e,const char* stage) {
        if(e<0) throw std::runtime_error(std::string(stage)+": "+dds_strretcode(-e));
    }
    void consume(size_t i,u64 seq,u64 stamp,const uint8_t* data,size_t len) {
        const u64 arrival=now_ns();
        auto& m=*metrics[i];
        std::lock_guard<std::mutex> lock(m.mu);
        if(stamp>=ctl->start.load(std::memory_order_acquire)) {
            ++m.received;
            if(arrival<ctl->end.load()) ++m.in_window;
            const u64 first=pressure()?2:33;
            bool duplicate=false;
            if(seq<first) { ++m.bad; duplicate=true; }
            else {
                const u64 bit=seq-first, word=bit/64, mask=1ULL<<(bit%64);
                if(word>=m.seen.size()) m.seen.resize(word+1);
                duplicate=(m.seen[word]&mask)!=0;
                m.seen[word]|=mask;
            }
            if(duplicate) ++m.duplicate;
            else {
                const u64 previous=m.last?m.last:first-1;
                if(seq>previous+1) m.gaps+=seq-previous-1;
                else if(seq<previous && m.gaps) --m.gaps;
                m.last=std::max(seq,m.last);
                ctl->delivered[i].fetch_add(1,std::memory_order_release);
            }
            const bool valid=len==c.bytes && check_payload(data,len,seq,c.full);
            if(!valid) ++m.bad;
            if(!pressure() || seq%100==0) m.latency.push_back((arrival-stamp)/1000.0);
        }
        ctl->ack[i].store(seq,std::memory_order_release);
    }
    static void listener(dds_entity_t rd,void* raw) {
        auto* x=static_cast<Ctx*>(raw);
        x->b->take(rd,x->i,x->reply);
    }
    void take(dds_entity_t rd,size_t i,bool reply) {
        for(;;) {
            void* samples[32]{}; dds_sample_info_t info[32]{};
            const int n=dds_take(rd,samples,info,32,32);
            if(n<=0) return;
            for(int j=0;j<n;++j) if(info[j].valid_data) {
                auto* meta=static_cast<DDSMeta*>(samples[j]);
                auto* data=reinterpret_cast<uint8_t*>(samples[j])+sizeof(DDSMeta);
                if(meta->seq==1 || meta->seq%100==0) {
                    if(in_shared_mapping(data)) ++data_in_shm; else ++data_on_heap;
                }
                if(rpc()) {
                    if(reply) {
                        u64 success=0; std::memcpy(&success,data,8);
                        if(success==1) response_seq.store(meta->seq,std::memory_order_release);
                    } else {
                        // 服务回调回复 8 B success=1；正确性轮完整遍历请求。
                        if(c.full && !check_payload(data,c.bytes,meta->seq,true)) ++reply_bad;
                        u64 res[3]={meta->seq,now_ns(),1};
                        if(dds_write(writers[0],res)==0) ++reply_ok; else ++reply_bad;
                    }
                } else consume(i,meta->seq,meta->stamp,data,c.bytes);
            }
            dds_return_loan(rd,samples,n);
        }
    }
    dds_entity_t endpoint(const std::string& name,size_t bytes,bool writer,size_t i,bool reply=false) {
        auto tp=dds_create_topic(dp,descriptor(bytes),name.c_str(),nullptr,nullptr);check(tp,"topic");
        auto* q=dds_create_qos();
        dds_qset_reliability(q,DDS_RELIABILITY_RELIABLE,DDS_MSECS(100));
        dds_qset_history(q,DDS_HISTORY_KEEP_LAST,pressure()?64:16);
        dds_qset_durability(q,DDS_DURABILITY_VOLATILE);
        dds_entity_t e;
        if(writer) e=dds_create_writer(dp,tp,q,nullptr);
        else {
            auto cx=std::make_unique<Ctx>(); cx->b=this;cx->i=i;cx->reply=reply;
            auto* l=dds_create_listener(cx.get());
            dds_lset_data_available(l,listener);
            e=dds_create_reader(dp,tp,q,l);
            dds_delete_listener(l);
            contexts.push_back(std::move(cx));
        }
        dds_delete_qos(q); check(e,writer?"writer":"reader");
        shm_available+=dds_is_shared_memory_available(e);
        loan_available+=dds_is_loan_available(e);
        return e;
    }
    void init() {
        dzIPC::EnableNodelet(false);
        dzIPC::EnableDzFlat(c.backend=="a"||c.backend=="b"||c.backend=="prebuilt");
        const bool sender=c.role=="pub";
        if(isdds()) {
            dp=dds_create_participant(c.domain,nullptr,nullptr);check(dp,"participant");
            if(rpc()) {
                if(sender) {
                    writers.push_back(endpoint(topic(0)+"_req",c.bytes,true,0));
                    readers.push_back(endpoint(topic(0)+"_res",8,false,0,true));
                } else {
                    writers.push_back(endpoint(topic(0)+"_res",8,true,0));
                    readers.push_back(endpoint(topic(0)+"_req",c.bytes,false,0));
                }
            } else for(size_t i=0;i<c.topics;++i) {
                if(sender) writers.push_back(endpoint(topic(i),c.bytes,true,i));
                else readers.push_back(endpoint(topic(i),c.bytes,false,i));
            }
        } else if(rpc()) {
            sd=std::make_shared<dzIPC::ServiceData>(std::make_shared<Req>(),std::make_shared<Res>());
            auto cb=[this](std::shared_ptr<dzIPC::ServiceData>& x) {
                if(x->request_is_view()) ++request_views;
                if(c.full) {
                    bool valid=true;
                    if(x->request_is_view()) {
                        auto v=x->request_view<ReqFlat>();
                        valid=v.valid() && v.request().size()==c.bytes/8;
                        if(valid) for(size_t k=0;k<v.request().size();++k)
                            if(v.request()[k]!=v.request()[0]+double(k)*0.5) {valid=false;break;}
                    } else {
                        auto q=std::static_pointer_cast<Req>(x->request());
                        valid=q->request.size()==c.bytes/8;
                        if(valid) for(size_t k=0;k<q->request.size();++k)
                            if(q->request[k]!=q->request[0]+double(k)*0.5) {valid=false;break;}
                    }
                    if(!valid) ++reply_bad;
                }
                auto r=std::static_pointer_cast<Res>(x->response());
                r->response.assign(1,1.0);
                ++reply_ok;
            };
            if(sender) {
                if(issocket()) client.reset(new dzIPC::socket::socket_cli_ipc(topic(0),sd,c.domain));
                else client.reset(new dzIPC::shm::shm_cli_ipc(topic(0),sd,c.domain));
                client->InitChannel("");
            } else {
                if(issocket()) server.reset(new dzIPC::socket::socket_ser_ipc(topic(0),sd,cb,c.domain));
                else server.reset(new dzIPC::shm::shm_ser_ipc(topic(0),sd,cb,c.domain));
                server->InitChannel("");
            }
        } else for(size_t i=0;i<c.topics;++i) {
            auto msg=std::make_shared<Img>();
            msg->height=1;msg->width=0;msg->step=0;msg->header.stamp=0;
            td.push_back(std::make_shared<dzIPC::TopicData>(msg,77));
            if(sender) {
                if(issocket()) pubs.emplace_back(new dzIPC::socket::socket_pub_ipc(td.back(),topic(i),c.domain));
                else pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td.back(),topic(i),c.domain));
                pubs.back()->InitChannel("");
            } else {
                if(issocket()) subs.emplace_back(new dzIPC::socket::socket_sub_ipc(td.back(),topic(i),c.domain,64));
                else subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td.back(),topic(i),c.domain,64));
                subs.back()->InitChannel("");
            }
        }
    }
    bool matched() {
        if(isdds()) {
            for(auto w:writers) {
                dds_publication_matched_status_t st{};
                if(dds_get_publication_matched_status(w,&st)<0 || st.current_count<1) return false;
            }
            return true;
        }
        if(rpc()) return client->handshake_completed();
        for(auto& p:pubs) if(!p->has_subscribed()) return false;
        return true;
    }
    bool send_one(size_t i,u64 seq,bool measured) {
        u64 begin=now_ns(), entered=begin,finished=0;
        bool ok=false;
        if(isdds()) {
            std::vector<uint64_t> buf;
            void* sample=nullptr;
            if(isiox()) {
                if(!dds_is_loan_available(writers[i]) || dds_loan_sample(writers[i],&sample)!=0) {
                    ++loan_failed; return false;
                }
            } else {buf.resize((c.bytes+sizeof(DDSMeta)+7)/8);sample=buf.data();}
            if(isiox() && (seq==1 || seq%100==0)) {
                if(in_shared_mapping(sample)) ++loan_in_shm;else ++loan_on_heap;
            }
            auto* m=static_cast<DDSMeta*>(sample);m->seq=seq;m->stamp=begin;
            fill_payload(static_cast<uint8_t*>(sample)+sizeof(DDSMeta),c.bytes,seq,c.full);
            entered=now_ns();
            ok=dds_write(writers[i],sample)==DDS_RETCODE_OK;finished=now_ns();
        } else if(c.backend=="b") {
            auto p=std::static_pointer_cast<dzIPC::shm::shm_pub_ipc>(pubs[i]);
            auto lo=p->loan<Flat>(c.bytes+32);
            if(lo.valid()) {
                lo->set_width(seq);lo->set_height(1);lo->set_step(c.bytes);lo->header().set_stamp(double(begin));
                auto data=lo->alloc_data(c.bytes);
                if(data.size()==c.bytes) fill_payload(data.data(),data.size(),seq,c.full);
                entered=now_ns();ok=p->publish_loaned(std::move(lo));
            } else ++loan_failed;
        } else {
            auto m=std::static_pointer_cast<Img>(td[i]->topic());
            m->data.resize(c.bytes);m->width=seq;m->height=1;m->step=c.bytes;m->header.stamp=double(begin);
            fill_payload(m->data.data(),m->data.size(),seq,c.full);
            if(c.backend=="prebuilt") {
                std::vector<uint8_t> segment(m->dzflat_size());
                if(!m->dzflat_write(segment.data(),segment.size())) throw std::runtime_error("预构造段失败");
                entered=now_ns();ok=pubs[i]->publish_prebuilt_segment(segment.data(),segment.size());finished=now_ns();
            } else { entered=now_ns();ok=pubs[i]->publish(m); }
        }
        if(!finished) finished=now_ns();
        if(measured && (!pressure()||seq%100==0)) {
            std::lock_guard<std::mutex> guard(timing_mutex);
            prepare_times.push_back((entered-begin)/1000.0);
            send_times.push_back((finished-entered)/1000.0);
        }
        return ok;
    }
    bool rpc_one(u64 seq,bool measured) {
        auto begin=now_ns();bool ok=false;
        if(isdds()) {
            ok=send_one(0,seq,false);
            auto deadline=now_ns()+1000000000ULL;
            while(ok && response_seq.load(std::memory_order_acquire)<seq && now_ns()<deadline) std::this_thread::yield();
            ok=ok && response_seq.load()>=seq;
        } else {
            auto req=std::static_pointer_cast<Req>(sd->request());
            req->request.assign(c.bytes/8,double(seq));
            if(c.full) for(size_t k=0;k<req->request.size();++k) req->request[k]+=double(k)*0.5;
            // 原生 rev_tm 单位为毫秒，防止失败组合永久阻塞。
            ok=client->send_request(sd,1000);
            if(ok) {
                if(sd->response_is_view()) {
                    ++response_views;
                    auto v=sd->response_view<ResFlat>();
                    ok=v.valid() && v.response().size()==1 && v.response()[0]==1.0;
                } else {
                    auto r=std::static_pointer_cast<Res>(sd->response());
                    ok=r->response.size()==1 && r->response[0]==1.0;
                }
            }
        }
        if(measured && ok) rpc_times.push_back((now_ns()-begin)/1000.0);
        return ok;
    }
    void baseline() {
        dzIPC::ResetDzFlatCounters();
        auto s=dzIPC::measure::CounterRegistry::instance().snapshot();
        counter_tlv=s.values[size_t(dzIPC::measure::CounterId::tlv_messages)];
        counter_a=s.values[size_t(dzIPC::measure::CounterId::dzflat_a_messages)];
        counter_b=s.values[size_t(dzIPC::measure::CounterId::dzflat_b_messages)];
        counter_pre=dzIPC::DzFlatPrebuiltSegmentCount();
    }
    void run_pub() {
        const u64 limit=now_ns()+90000000000ULL;
        while((!ctl->ready.load() || !matched()) && now_ns()<limit) nap();
        if(!ctl->ready.load() || !matched()) throw std::runtime_error("90 秒内未完成全部端点匹配");
        if(isiox() && loan_available==0) throw std::runtime_error("DDS 共享内存借样不可用，拒绝退回 UDP");
        // 每个话题至少一次全链路握手；速度试验 32 次预热。
        std::vector<u64> seq(c.topics,0);
        const size_t rounds=pressure()?1:32;
        for(size_t r=0;r<rounds;++r) for(size_t i=0;i<c.topics;++i) {
            const u64 s=++seq[i];
            bool ok=rpc()?rpc_one(s,false):send_one(i,s,false);
            if(!ok) throw std::runtime_error("预热发送失败");
            if(!rpc()) {
                auto dl=now_ns()+2000000000ULL;
                while(ctl->ack[i].load()<s && now_ns()<dl) std::this_thread::yield();
                for(unsigned retry=0;ctl->ack[i].load()<s && retry<(no_retry?0u:3u);++retry) {
                    ++warmup_retries;
                    if(!send_one(i,s,false)) throw std::runtime_error("预热重试发送失败");
                    auto retry_end=now_ns()+200000000ULL;
                    while(ctl->ack[i].load()<s && now_ns()<retry_end) std::this_thread::yield();
                }
                if(ctl->ack[i].load()<s) throw std::runtime_error("预热接收超时");
            }
        }
        baseline();cpu0=cpu_seconds();main_cpu0=main_cpu_seconds();
        const u64 start=now_ns()+100000000ULL;
        const u64 end=start+u64(c.seconds*1e9);
        ctl->end.store(end);ctl->start.store(start,std::memory_order_release);
        while(now_ns()<start) nap();
        if(pressure()) {
            // 每个线程拥有不重叠的话题子集，每话题仍按同一绝对时间轴每毫秒发一条。
            plan=u64(std::llround(c.seconds*1000))*c.topics;
            struct Lane {u64 attempts=0,sent=0,failed=0;};
            const unsigned lanes=std::min<size_t>(c.lanes,c.topics);
            std::vector<Lane> stats(lanes);
            std::vector<std::thread> producers;
            for(unsigned lane=0;lane<lanes;++lane) producers.emplace_back([&,lane] {
                auto& count=stats[lane];
                for(u64 tick=0;tick<u64(std::llround(c.seconds*1000)) && now_ns()<end;++tick) {
                    const u64 due=start+tick*1000000ULL;
                    while(now_ns()<due) {
                        if(due-now_ns()>100000) std::this_thread::sleep_for(std::chrono::microseconds(30));
                    }
                    for(size_t i=lane;i<c.topics && now_ns()<end;i+=lanes) {
                        ++count.attempts;
                        if(send_one(i,++seq[i],true)) {++count.sent;++topic_sent[i];}else ++count.failed;
                    }
                }
            });
            for(auto& t:producers) t.join();
            for(auto count:stats) {attempts+=count.attempts;sent+=count.sent;failed+=count.failed;}
        } else {
            u64 last_ack=ctl->ack[0].load(), last_progress=now_ns(), next_send=start+idle_ns;
            const u64 rate_plan=offered_rate?u64(std::llround(c.seconds*offered_rate)):0;
            while(now_ns()<end && (!offered_rate || attempts<rate_plan) && (!message_limit || attempts<message_limit)) {
                const auto ack=ctl->ack[0].load(std::memory_order_acquire);
                if(ack!=last_ack) {last_ack=ack;last_progress=now_ns();if(idle_ns) next_send=last_progress+idle_ns;}
                if(!rpc() && seq[0]>=ack+window) {
                    if(now_ns()-last_progress>progress_deadline_ns) {++stalled;break;}
                    std::this_thread::yield();continue;
                }
                if(idle_ns && now_ns()<next_send) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));continue;
                }
                if(offered_rate && now_ns()<start+attempts*1000000000ULL/offered_rate) {std::this_thread::yield();continue;}
                ++attempts;
                bool ok=rpc()?rpc_one(++seq[0],true):send_one(0,++seq[0],true);
                if(ok) {++sent;++topic_sent[0];}else ++failed;
                next_send=now_ns()+idle_ns;
            }
            plan=message_limit?message_limit:(offered_rate?u64(std::llround(c.seconds*offered_rate)):attempts);
        }
        elapsed=(now_ns()-start)/1e9;
        const double measured_cpu=cpu_seconds()-cpu0;
        // 先公布每话题成功数量，再让接收端决定排空；两端均存活到确认或硬上限。
        for(size_t i=0;i<c.topics;++i) {
            ctl->expected[i].store(topic_sent[i],std::memory_order_relaxed);
            ctl->final_seq[i].store(seq[i],std::memory_order_relaxed);
        }
        ctl->done.store(1,std::memory_order_release);
        if(!rpc()) {
            const u64 drain_limit=now_ns()+6000000000ULL;
            while(!ctl->drained.load(std::memory_order_acquire) && now_ns()<drain_limit) nap();
        }
        output(measured_cpu);
    }
    void run_sub() {
        ctl->ready.store(1,std::memory_order_release);
        u64 hard=now_ns()+150000000000ULL;
        bool started=false;
        u64 finished_at=0;
        dzIPC::threepools::RecvWorkerStats before{};
        while(now_ns()<hard) {
            if(!started && now_ns()>=ctl->start.load()) {
                started=true;cpu0=cpu_seconds();main_cpu0=main_cpu_seconds();
                before=dzIPC::threepools::RecvWorkerPool::instance().stats();
            }
            if(!isdds() && !rpc()) {
                for(size_t i=0;i<c.topics;++i) for(int k=0;k<32;++k) {
                    dzIPC::Sample sample;
                    if(subs[i]->try_get(sample)) {
                        auto v=sample.view<Flat>();
                        if(v.valid()) consume(i,v.width(),u64(v.header().stamp()),v.data().data(),v.data().size());
                    } else if(subs[i]->try_get_clone(td[i])) {
                        auto m=std::static_pointer_cast<Img>(td[i]->topic());
                        consume(i,m->width,u64(m->header.stamp),m->data.data(),m->data.size());
                    } else break;
                }
                if(pressure()) std::this_thread::yield();
            } else nap(); // DDS 回调在原生接收线程执行，应用不建接收工作线程。
            if(ctl->done.load(std::memory_order_acquire)) {
                if(!finished_at) finished_at=now_ns();
                bool complete=true;
                if(!rpc()) for(size_t i=0;i<c.topics;++i)
                    complete=complete && ctl->delivered[i].load(std::memory_order_acquire)==ctl->expected[i].load();
                if(complete || now_ns()-finished_at>=5000000000ULL) break;
            }
        }
        if(!ctl->done.load()) throw std::runtime_error("发布端未完成或运行超时");
        ctl->drained.store(1,std::memory_order_release);
        // 先停止回调，使聚合读取与回调写入不并发。
        for(auto r:readers) dds_set_listener(r,nullptr);
        elapsed=(now_ns()-ctl->start.load())/1e9;
        const auto after=dzIPC::threepools::RecvWorkerPool::instance().stats();
        worker_delta=after;
        worker_delta.route_count=std::max(before.route_count,after.route_count);
        worker_delta.messages_received-=before.messages_received;
        worker_delta.wait_wakeups-=before.wait_wakeups;
        worker_delta.wait_timeouts-=before.wait_timeouts;
        worker_delta.scan_rounds-=before.scan_rounds;
        worker_delta.scanned_routes_total-=before.scanned_routes_total;
        worker_delta.recv_once_calls-=before.recv_once_calls;
        worker_delta.recv_once_over_budget-=before.recv_once_over_budget;
        worker_delta.budget_yields-=before.budget_yields;
        output(started?cpu_seconds()-cpu0:0);
    }
    dzIPC::threepools::RecvWorkerStats worker_delta{};
    void output(double cpu) {
        std::ofstream o(c.out);
        o.precision(12);
        rusage usage{};getrusage(RUSAGE_SELF,&usage);
        o<<"{\"main_cpu_seconds\":"<<main_cpu_seconds()-main_cpu0<<",\"max_rss_kib\":"<<usage.ru_maxrss
         <<",\"voluntary_context_switches\":"<<usage.ru_nvcsw<<",\"involuntary_context_switches\":"<<usage.ru_nivcsw
         <<",\"backend\":\""<<c.backend<<"\",\"mode\":\""<<c.mode<<"\",\"role\":\""<<c.role
         <<"\",\"bytes\":"<<c.bytes<<",\"topics\":"<<c.topics<<",\"duration\":"<<c.seconds
         <<",\"elapsed\":"<<elapsed<<",\"attempts\":"<<attempts<<",\"sent\":"<<sent<<",\"failed\":"<<failed
         <<",\"publish_lanes\":"<<c.lanes<<",\"warmup_retries\":"<<warmup_retries
         <<",\"plan\":"<<plan<<",\"loan_failed\":"<<loan_failed.load()<<",\"cpu_seconds\":"<<cpu
         <<",\"threads\":"<<threads()<<",\"dds_shm_endpoints\":"<<shm_available<<",\"dds_loan_endpoints\":"<<loan_available;
        u64 received=0,bad=0,duplicate=0,gaps=0,covered=0,win=0,min_rx=UINT64_MAX,max_rx=0;
        std::vector<double> lat;
        for(auto& ptr:metrics) {
            auto& m=*ptr;
            received+=m.received;bad+=m.bad;duplicate+=m.duplicate;gaps+=m.gaps;covered+=m.received>0;
            win+=m.in_window;min_rx=std::min(min_rx,m.received);max_rx=std::max(max_rx,m.received);
            lat.insert(lat.end(),m.latency.begin(),m.latency.end());
        }
        o<<",\"received\":"<<received<<",\"received_in_window\":"<<win<<",\"bad\":"<<bad<<",\"duplicate\":"<<duplicate
         <<",\"gaps\":"<<gaps<<",\"topics_covered\":"<<covered<<",\"topic_rx_min\":"<<min_rx<<",\"topic_rx_max\":"<<max_rx;
        Histogram::write(o,std::move(lat),"latency");Histogram::write(o,send_times,"send");
        Histogram::write(o,prepare_times,"prepare");Histogram::write(o,rpc_times,"rpc");
        o<<",\"stalled\":"<<stalled<<",\"window\":"<<window<<",\"offered_rate\":"<<offered_rate;
        o<<",\"topic_delivery\":[";
        for(size_t i=0;i<c.topics;++i) {
            if(i) o<<",";
            o<<"{\"expected\":"<<ctl->expected[i].load()<<",\"delivered\":"<<ctl->delivered[i].load()
             <<",\"final_seq\":"<<ctl->final_seq[i].load()<<",\"last_received\":"<<metrics[i]->last
             <<",\"gaps\":"<<metrics[i]->gaps<<"}";
        }
        o<<"]";
        auto s=dzIPC::measure::CounterRegistry::instance().snapshot();
        o<<",\"queue_evicted\":"<<s.values[size_t(dzIPC::measure::CounterId::queue_evicted)];
        const auto rx=dzIPC::DzFlatRxCounters();
        o<<",\"wire_tlv_accepted_total\":"<<rx.tlv_accepted
         <<",\"wire_tlv_id_skipped_total\":"<<rx.tlv_id_skipped
         <<",\"wire_tlv_corrupt_total\":"<<rx.tlv_corrupt_drop
         <<",\"wire_dzflat_accepted_total\":"<<rx.dzflat_accepted
         <<",\"wire_dzflat_defects_total\":"<<rx.defects()
         <<",\"wakeup_artifacts_total\":"<<dzIPC::WakeupArtifactCount();
        o<<",\"tlv\":"<<s.values[size_t(dzIPC::measure::CounterId::tlv_messages)]-counter_tlv
         <<",\"a\":"<<s.values[size_t(dzIPC::measure::CounterId::dzflat_a_messages)]-counter_a
         <<",\"b\":"<<s.values[size_t(dzIPC::measure::CounterId::dzflat_b_messages)]-counter_b
         <<",\"prebuilt\":"<<dzIPC::DzFlatPrebuiltSegmentCount()-counter_pre
         <<",\"dzflat\":"<<dzIPC::DzFlatPublishCount()<<",\"fallback\":"<<dzIPC::DzFlatFallbackCount();
        const auto& w=worker_delta;
        o<<",\"pool_workers\":"<<dzIPC::threepools::RecvWorkerPool::instance().worker_count()
         <<",\"pool_routes\":"<<w.route_count<<",\"pool_messages\":"<<w.messages_received
         <<",\"pool_wakeups\":"<<w.wait_wakeups<<",\"pool_wait_timeouts\":"<<w.wait_timeouts
         <<",\"pool_scans\":"<<w.scan_rounds<<",\"pool_scanned_routes\":"<<w.scanned_routes_total
         <<",\"pool_recv_calls\":"<<w.recv_once_calls<<",\"pool_over_budget\":"<<w.recv_once_over_budget
         <<",\"pool_budget_yields\":"<<w.budget_yields
         <<",\"reply_ok\":"<<reply_ok.load()<<",\"reply_bad\":"<<reply_bad.load()
         <<",\"request_views\":"<<request_views.load()<<",\"response_views\":"<<response_views.load()
         <<",\"loan_in_shm\":"<<loan_in_shm.load()<<",\"loan_on_heap\":"<<loan_on_heap.load()
         <<",\"data_in_shm\":"<<data_in_shm.load()<<",\"data_on_heap\":"<<data_on_heap.load()<<"}\n";
        o.flush();
        if(!o) throw std::runtime_error("结果写入失败");
    }
    ~Bench() { if(dp>0) dds_delete(dp); }
};
int main(int argc,char**argv) {
    try {
        if(argc!=12) throw std::runtime_error("参数：backend mode role bytes topics seconds domain control out token full");
        Config c;
        c.backend=argv[1];c.mode=argv[2];c.role=argv[3];c.bytes=std::stoul(argv[4]);c.topics=std::stoul(argv[5]);
        c.seconds=std::stod(argv[6]);c.domain=std::stoul(argv[7]);c.control=argv[8];c.out=argv[9];c.token=argv[10];c.full=std::stoi(argv[11]);
        if(c.mode=="stress") {
            const char* lanes=std::getenv("COMPARISON_PUBLISHERS");
            c.lanes=lanes?std::stoul(lanes):4;
        }
        if(!c.lanes || c.lanes>32) throw std::runtime_error("发布线程数必须为 1..32");
        if(!c.topics||c.topics>MAX_TOPICS||c.seconds<=0) throw std::runtime_error("非法规模或时长");
        if(c.mode=="rpc"&&(c.backend=="b"||c.backend=="prebuilt")) throw std::runtime_error("原生服务接口不支持该路径");
        int fd=open(c.control.c_str(),O_RDWR);
        if(fd<0) throw std::runtime_error("无法打开控制文件");
        void* map=mmap(nullptr,sizeof(Control),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
        if(map==MAP_FAILED) throw std::runtime_error("控制文件映射失败");
        auto* ctl=static_cast<Control*>(map);
        Bench b(c,ctl);b.init();
        if(c.role=="pub") b.run_pub();else b.run_sub();
        return 0;
    } catch(const std::exception& e) {std::cerr<<"测试失败："<<e.what()<<std::endl;return 2;}
}
