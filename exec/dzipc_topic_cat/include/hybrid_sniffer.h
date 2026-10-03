#pragma once
#include "sniffer_base.h"
#include "dzIPC/hybrid_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"

namespace dzIPC {
// 只订阅一个逻辑端点，由混合订阅者合并两腿；不再另建原始 UDP/SHM 嗅探腿。
class hybrid_sniffer final : public sniffer_base {
public:
    hybrid_sniffer(const std::string& topic,std::uint64_t domain,std::uint32_t id){create_sniffer(topic,domain,false,id);}
    void create_sniffer(const std::string& topic,std::uint64_t domain,bool,std::uint32_t id) override {
        data_=std::make_shared<TopicData>(std::make_shared<GenericMessage>(),id);
        sub_=std::make_unique<hybrid::Subscriber>(data_,topic,domain,10);
        sub_->InitChannel("topic_cat observer");
    }
    sniffer_info try_recv() noexcept override {return recv_inner(0);}
protected:
    sniffer_info recv_inner(std::uint64_t) noexcept override {
        try {
            if(!sub_->try_get_clone(data_))return {};
            auto msg=std::static_pointer_cast<GenericMessage>(data_->topic());
            if(msg->has_dzflat())return {ipc::buffer(const_cast<std::uint8_t*>(msg->dzflat_data()),msg->dzflat_len()),{}};
            return {msg->serialize(),{}};
        } catch(...) {return {};}
    }
private:
    std::shared_ptr<TopicData> data_;std::unique_ptr<hybrid::Subscriber> sub_;
};
}
