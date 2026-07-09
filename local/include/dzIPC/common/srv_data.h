#ifndef SRV_DATA_H
#define SRV_DATA_H
#include <memory>
#include "dzIPC/common/data_base.h"
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"

namespace dzIPC {
class ServiceData : public DataBase
{
public:
    ServiceData(const std::shared_ptr<IpcMsgBase>& request, const std::shared_ptr<IpcMsgBase>& response,
                uint32_t msg_id = 0)
        : msg_id_(msg_id)
    {
        request_.reset(request->clone());
        response_.reset(response->clone());
        request_->set_msg_id(msg_id);
        response_->set_msg_id(msg_id);
        msg_method = 1;
    }

    ServiceData(std::shared_ptr<IpcMsgBase>&& request, std::shared_ptr<IpcMsgBase>&& response, uint32_t msg_id = 0)
        : request_(std::move(request))
        , response_(std::move(response))
        , msg_id_(msg_id)
    {
        request_->set_msg_id(msg_id);
        response_->set_msg_id(msg_id);
        msg_method = 1;
    }

    std::shared_ptr<IpcMsgBase>& request() { return request_; };

    std::shared_ptr<IpcMsgBase>& response() { return response_; }

    ServiceData(const std::shared_ptr<ServiceData>& other)
        : request_(other->request_->clone())
        , response_(other->response_->clone())
        , msg_id_(other->msg_id_)
    {
        msg_method = other->msg_method;
    }

    ServiceData* clone() { return new ServiceData(*this); }

    bool check_msg_id(const ipc::buffer& data) { return request_->check_id(data); }

private:
    ServiceData(const ServiceData& other)
    {
        request_.reset(other.request_->clone());
        response_.reset(other.response_->clone());
        msg_id_ = other.msg_id_;
        msg_method = other.msg_method;
    }

    ServiceData() = default;
    std::shared_ptr<IpcMsgBase> request_;
    std::shared_ptr<IpcMsgBase> response_;
    uint32_t msg_id_;
};
}   // namespace dzIPC

#endif   // DATA_H