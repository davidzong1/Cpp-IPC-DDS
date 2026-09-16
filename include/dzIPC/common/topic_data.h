#ifndef TOPIC_DATA_H
#define TOPIC_DATA_H
#include <memory>
#include "dzIPC/common/data_base.h"
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"

namespace dzIPC {
class TopicData : public DataBase
{
public:
    TopicData(const std::shared_ptr<IpcMsgBase>& topic, uint32_t msg_id = 0)
    {
        topic_.reset(topic->clone());
        topic_cache.reset(topic->clone());
        topic_->set_msg_id(msg_id);
        topic_cache->set_msg_id(msg_id);
        msg_id_ = msg_id;
        msg_method = 0;
    }

    TopicData(std::shared_ptr<IpcMsgBase>&& topic, uint32_t msg_id = 0)
        : topic_(std::move(topic))
        , msg_id_(msg_id)
    {
        topic_cache.reset(topic_->clone());
        topic_->set_msg_id(msg_id);
        topic_cache->set_msg_id(msg_id);
        msg_method = 0;
    }

    TopicData(const std::shared_ptr<TopicData>& other)
    {
        topic_.reset(other->topic_->clone());
        topic_cache.reset(other->topic_cache->clone());
        msg_id_ = other->msg_id_;
        msg_method = 0;
    }

    std::shared_ptr<IpcMsgBase>& topic() { return topic_; };

    TopicData* clone() { return new TopicData(*this); }

    void update(std::shared_ptr<IpcMsgBase>& other) { topic_ = std::move(other); }

    void swap(std::shared_ptr<IpcMsgBase>& other)
    {
        other = std::move(topic_);
        topic_.reset(topic_cache->clone());
    }

    bool check_msg_id(const ipc::buffer& data) override { return topic_cache->check_id(data); }

    /* DZFlat 段头里的 msg_id 校验。与 check_msg_id 一样走 topic_cache —— topic_ 会被
     * swap() 移走, 只有 cache 始终持有本话题的类型与 msg_id。 */
    bool check_dzflat_msg_id(const ipc::buffer& data) const
    {
        return topic_cache->check_dzflat_id(data);
    }

    /// 本话题的 msg_id。供 AcceptWire 校验用 —— topic_ 会被 swap() 移走, 这里始终有效。
    uint32_t msg_id() const noexcept { return msg_id_; }

private:
    TopicData(const TopicData& other)
    {
        topic_.reset(other.topic_->clone());
        topic_cache.reset(other.topic_cache->clone());
        msg_id_ = other.msg_id_;
        msg_method = 0;
    }

    TopicData() = default;
    std::shared_ptr<IpcMsgBase> topic_, topic_cache;
    uint32_t msg_id_{0};
};
}   // namespace dzIPC

#endif   // DATA_H