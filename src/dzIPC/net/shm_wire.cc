#include "dzIPC/net/shm_wire.h"
#include "byte_codec.h"
#include "dzIPC/common/shm_mpmc_config.h"
#include "dzIPC/net/shared_config.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "local_control_linux.h"
#include <mutex>
#include <shared_mutex>

namespace dzIPC::net
{
struct ShmWireWriter::Impl
{
    const pid_t owner = getpid();
    RouteDescriptor descriptor;
    mutable std::shared_mutex gate;
    std::unique_ptr<shm::shm_pub_ipc> publisher;
    std::atomic<bool> poisoned{false};
    bool ready() const
    {
        return publisher && !poisoned.load();
    }
    bool subscribers() const
    {
        return ready() && publisher->publisher_->recv_count() != 0;
    }
    bool matches(Encoding encoding, std::uint32_t id, std::uint32_t schema) const
    {
        return id == descriptor.key.msg_id &&
               (encoding != Encoding::DzFlat || !descriptor.schema_hash ||
                schema == descriptor.schema_hash);
    }
    SubmitState publish(const ipc::loan_t &loan, std::size_t size)
    {
        try
        {
            return publisher->publisher_->publish_loan_size(loan, size) ? SubmitState::Committed
                                                                        : SubmitState::NotSubmitted;
        }
        catch (...)
        {
            poisoned.store(true);
            return SubmitState::Indeterminate;
        }
    }
    SubmitState commit(ByteView bytes, Encoding encoding, std::uint32_t id, std::uint32_t schema,
                       std::uint64_t deadline)
    {
        if (!ready() || !matches(encoding, id, schema) ||
            !validate_blob(bytes, encoding, id, schema))
            return SubmitState::NotSubmitted;
        if (!subscribers())
            return SubmitState::NotRequired;
        if (deadline && local::monotonic_ns() >= deadline)
            return SubmitState::NotSubmitted;
        auto loan = publisher->publisher_->loan(bytes.size);
        if (!loan.valid())
            return SubmitState::NotSubmitted;
        std::memcpy(loan.data, bytes.data, bytes.size);
        if (deadline && local::monotonic_ns() >= deadline)
            return SubmitState::NotSubmitted;
        return publish(loan, bytes.size);
    }
};
ShmWireWriter::ShmWireWriter(RouteDescriptor descriptor, bool internal) : impl_(new Impl)
{
    if (!shm_mpmc_enabled())
        throw ConfigError({ConfigCode::MpmcRequired, "原始 SHM 写入需要 DZIPC_SHM_MPMC=1"});
    Bytes checked;
    if (!encode_descriptor(descriptor, true, checked))
        throw std::invalid_argument("业务路由描述无效");
    impl_->descriptor = std::move(descriptor);
    auto model = std::make_shared<GenericMessage>();
    model->set_msg_id(impl_->descriptor.key.msg_id);
    auto topic = std::make_shared<TopicData>(model, impl_->descriptor.key.msg_id);
    auto pub = std::make_unique<shm::shm_pub_ipc>(
        topic, impl_->descriptor.topic, codec::get(impl_->descriptor.key.scope.data() + 8, 8));
    pub->set_internal(internal);
    pub->InitChannel("shared_network_raw");
    if (!pub->channel_ready() || !pub->topic_pool_lifetime_ || !pub->pub_control_state_)
        throw std::runtime_error("原始 SHM 发布端初始化失败");
    impl_->publisher = std::move(pub);
}
ShmWireWriter::~ShmWireWriter()
{
    if (impl_->owner != getpid())
    {
        impl_.release();
        return;
    }
    close_after_quiescent();
}
SubmitState ShmWireWriter::try_commit(const WireBlob &blob)
try
{
    if (impl_->owner != getpid())
        return SubmitState::NotSubmitted;
    std::shared_lock<std::shared_mutex> lock(impl_->gate);
    return impl_->commit(blob.view(), blob.encoding(), blob.msg_id(), blob.schema_hash(), 0);
}
catch (...)
{
    return SubmitState::NotSubmitted;
}
SubmitState ShmWireWriter::try_commit_prebuilt(ByteView bytes, std::uint64_t deadline)
try
{
    if (impl_->owner != getpid())
        return SubmitState::NotSubmitted;
    std::shared_lock<std::shared_mutex> lock(impl_->gate);
    if (!bytes.data || bytes.size < sizeof(dzflat::SegHeader))
        return SubmitState::NotSubmitted;
    dzflat::SegHeader h{};
    std::memcpy(&h, bytes.data, sizeof(h));
    if (h.total_size > bytes.size)
        return SubmitState::NotSubmitted;
    return impl_->commit({bytes.data, h.total_size}, Encoding::DzFlat, h.msg_id, h.schema_hash,
                         deadline);
}
catch (...)
{
    return SubmitState::NotSubmitted;
}
SubmitState ShmWireWriter::try_commit_local(IpcMsgBase &message, bool prefer_dzflat,
                                            std::uint64_t deadline)
try
{
    if (impl_->owner != getpid())
        return SubmitState::NotSubmitted;
    std::shared_lock<std::shared_mutex> lock(impl_->gate);
    if (!impl_->ready() || message.msg_id() != impl_->descriptor.key.msg_id ||
        (deadline && local::monotonic_ns() >= deadline))
        return SubmitState::NotSubmitted;
    if (!impl_->subscribers())
        return SubmitState::NotRequired;
    if (prefer_dzflat && message.dzflat_supported())
    {
        const auto need = message.dzflat_size();
        if (!need || need > kMaxMessageBytes ||
            !impl_->matches(Encoding::DzFlat, message.msg_id(), message.dzflat_schema_hash()))
            return SubmitState::NotSubmitted;
        auto loan = impl_->publisher->publisher_->loan(need);
        if (!loan.valid())
            return SubmitState::NotSubmitted;
        if (!message.dzflat_write(loan.data, loan.size) ||
            !validate_blob({loan.data, need}, Encoding::DzFlat, message.msg_id(),
                           message.dzflat_schema_hash()) ||
            (deadline && local::monotonic_ns() >= deadline))
            return SubmitState::NotSubmitted;
        return impl_->publish(loan, need);
    }
    WireBlob blob;
    if (!WireEncoder::encode(message, false, blob))
        return SubmitState::NotSubmitted;
    return impl_->commit(blob.view(), blob.encoding(), blob.msg_id(), blob.schema_hash(), deadline);
}
catch (...)
{
    return SubmitState::NotSubmitted;
}
bool ShmWireWriter::has_subscribers() const
{
    if (impl_->owner != getpid())
        return false;
    std::shared_lock<std::shared_mutex> lock(impl_->gate);
    return impl_->subscribers();
}
void ShmWireWriter::close_after_quiescent()
{
    if (impl_->owner != getpid())
        return;
    std::unique_lock<std::shared_mutex> lock(impl_->gate);
    impl_->publisher.reset();
}
const RouteDescriptor &ShmWireWriter::descriptor() const
{
    return impl_->descriptor;
}
} // namespace dzIPC::net
