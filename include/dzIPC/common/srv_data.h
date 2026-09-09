#ifndef SRV_DATA_H
#define SRV_DATA_H
#include <memory>
#include "dzIPC/common/data_base.h"
#include "dzIPC/common/sample_message.h"
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

    /* ---- 接收侧零拷贝视图(docs/dzflat_shm.md; 与 pub/sub 的 Sample 同机制) ----
     *
     * DZFlat 的 request/response 到达时不再物化进上面的 owning 槽, 而是把借来的 chunk
     * 存在 *_sample_ 里, 用 request_view<T>()/response_view<T>() 只读零拷贝访问。owning
     * request()/response() 在视图模式下保持模板克隆(未填充) —— 所以:
     *   request_is_view()==true  ⟹ 读 request_view<T>(); request() 此刻不可读物化数据;
     *   request_is_view()==false ⟹ 走 TLV / 快速路径物化, request() 是收到的 owning 对象。
     * clone()/拷贝构造**丢弃**借样 —— 借的 chunk 不能深拷贝, 复制出的必须是全新 owning 壳。 */
    std::shared_ptr<Sample>& request_sample() noexcept { return req_sample_; }
    std::shared_ptr<Sample>& response_sample() noexcept { return resp_sample_; }
    bool request_is_view() const noexcept { return req_sample_ != nullptr; }
    bool response_is_view() const noexcept { return resp_sample_ != nullptr; }

    /// 收到的 DZFlat request 的类型化只读视图; 非 DZFlat(或类型不支持)时返回空 view。
    template<typename Flat>
    typename Flat::view_t request_view() const
    {
        using V = typename Flat::view_t;
        return req_sample_ ? V::bind(req_sample_->data(), req_sample_->size()) : V{};
    }

    /// 收到的 DZFlat response 的类型化只读视图; 非 DZFlat(或类型不支持)时返回空 view。
    template<typename Flat>
    typename Flat::view_t response_view() const
    {
        using V = typename Flat::view_t;
        return resp_sample_ ? V::bind(resp_sample_->data(), resp_sample_->size()) : V{};
    }

    /// 无论 wire 都返回一份 owning 拷贝(供日志等需要物化字节的场合)。
    /// TLV/快速路径已 owning, 直接 clone(); DZFlat 从借样 copy_to 一份。返回空表示都不可。
    std::shared_ptr<IpcMsgBase> request_owned_copy() const;
    std::shared_ptr<IpcMsgBase> response_owned_copy() const;

    ServiceData(const std::shared_ptr<ServiceData>& other)
        : request_(other->request_->clone())
        , response_(other->response_->clone())
        , msg_id_(other->msg_id_)
    {
        msg_method = other->msg_method;
        /* 借样不参与拷贝 —— 复制出的必须是全新 owning 壳(见类顶注释)。 */
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
        /* 借样不参与拷贝。 */
    }

    ServiceData() = default;
    std::shared_ptr<IpcMsgBase> request_;
    std::shared_ptr<IpcMsgBase> response_;
    /* 接收侧借来的 DZFlat 段(可选)。非空 = 该槽以只读视图形式交付, owning 槽未填充。 */
    std::shared_ptr<Sample> req_sample_;
    std::shared_ptr<Sample> resp_sample_;
    uint32_t msg_id_;
};

/* 从借样段物化一份 owning(走 IpcMsgBase::dzflat_read 虚接口, 无需知道具体类型)。
 * 视图模式下 owning 槽是模板克隆, clone() 一份后 dzflat_read 把段填进去即成真身。 */
inline std::shared_ptr<IpcMsgBase> ServiceData::request_owned_copy() const
{
    if (!request_)
    {
        return nullptr;
    }
    auto out = std::shared_ptr<IpcMsgBase>(request_->clone());
    if (req_sample_)
    {
        out->dzflat_read(req_sample_->data(), req_sample_->size());
    }
    return out;
}

inline std::shared_ptr<IpcMsgBase> ServiceData::response_owned_copy() const
{
    if (!response_)
    {
        return nullptr;
    }
    auto out = std::shared_ptr<IpcMsgBase>(response_->clone());
    if (resp_sample_)
    {
        out->dzflat_read(resp_sample_->data(), resp_sample_->size());
    }
    return out;
}
}   // namespace dzIPC

#endif   // DATA_H