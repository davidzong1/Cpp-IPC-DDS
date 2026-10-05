#pragma once
#include "dzIPC/net/client_runtime.h"
#include "dzIPC/net/wire_blob.h"

namespace dzIPC::net
{
class ShmWireWriter
{
  public:
    explicit ShmWireWriter(RouteDescriptor, bool internal = false);
    ~ShmWireWriter();
    ShmWireWriter(const ShmWireWriter &) = delete;
    ShmWireWriter &operator=(const ShmWireWriter &) = delete;
    SubmitState try_commit(const WireBlob &);
    SubmitState try_commit_local(IpcMsgBase &, bool prefer_dzflat = true,
                                 std::uint64_t deadline_ns = 0);
    SubmitState try_commit_prebuilt(ByteView, std::uint64_t deadline_ns = 0);
    bool has_subscribers() const;
    void close_after_quiescent();
    const RouteDescriptor &descriptor() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class ShmWireBridge
{
  public:
    explicit ShmWireBridge(RouteDescriptor descriptor) : writer_(std::move(descriptor), true)
    {
    }
    SubmitState try_commit(const WireBlob &blob)
    {
        return writer_.try_commit(blob);
    }
    bool has_subscribers() const
    {
        return writer_.has_subscribers();
    }
    void close_after_quiescent()
    {
        writer_.close_after_quiescent();
    }

  private:
    ShmWireWriter writer_;
};
} // namespace dzIPC::net
