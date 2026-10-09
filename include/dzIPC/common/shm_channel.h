#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>

#include "libipc/ipc.h"

namespace dzIPC {
namespace shm {

/* Runtime-erased SHM channel used only by the SHM pub/sub implementation.
 * Keeping the two concrete chan_wrapper instantiations behind one object lets
 * the legacy route ABI and the opt-in MPMC layout coexist in one process. */
class ShmChannel
{
public:
    ShmChannel(const char* name, unsigned mode, bool verbose, bool mpmc)
        : mpmc_(mpmc)
        , channel_(mpmc ? channel_variant{std::in_place_type<ipc::mpmc_channel>, name, mode, verbose}
                       : channel_variant{std::in_place_type<ipc::route>, name, mode, verbose})
    {}

    bool is_mpmc() const noexcept { return mpmc_; }
    /* Legacy receive seam compatibility.  The seam predates the runtime
     * channel variant and only knows about ipc::route.  Returning nullptr for
     * MPMC keeps that diagnostic hook honest without exposing a wrong cast. */
    ipc::route* legacy_route() noexcept
    {
        return std::get_if<ipc::route>(&channel_);
    }
    const ipc::route* legacy_route() const noexcept
    {
        return std::get_if<ipc::route>(&channel_);
    }
    bool valid() const noexcept
    {
        return std::visit([](const auto& channel) { return channel.valid(); }, channel_);
    }
    std::size_t recv_count() const
    {
        return std::visit([](const auto& channel) { return channel.recv_count(); }, channel_);
    }
    std::uint32_t connected_id() const
    {
        return std::visit([](const auto& channel) { return channel.connected_id(); }, channel_);
    }
    ipc::recv_wait_token read_wait_token() const noexcept
    {
        return std::visit([](const auto& channel) { return channel.read_wait_token(); }, channel_);
    }
    void disconnect_receivers(std::uint32_t ids)
    {
        std::visit([ids](auto& channel) { channel.disconnect_receivers(ids); }, channel_);
    }
    bool wait_for_recv(std::size_t count, std::uint64_t timeout)
    {
        return std::visit([count, timeout](const auto& channel) {
            return channel.wait_for_recv(count, timeout);
        }, channel_);
    }
    bool send(const void* data, std::size_t size, std::uint64_t timeout)
    {
        return std::visit([&](auto& channel) { return channel.send(data, size, timeout); }, channel_);
    }
    bool no_member_try_send(const void* data, std::size_t size, std::uint64_t timeout)
    {
        return std::visit([&](auto& channel) { return channel.no_member_try_send(data, size, timeout); }, channel_);
    }
    bool try_send(const void* data, std::size_t size, std::uint64_t timeout)
    {
        return std::visit([&](auto& channel) { return channel.try_send(data, size, timeout); }, channel_);
    }
    ipc::buff_t recv(std::uint64_t timeout)
    {
        return std::visit([timeout](auto& channel) { return channel.recv(timeout); }, channel_);
    }
    ipc::buff_t try_recv()
    {
        return std::visit([](auto& channel) { return channel.try_recv(); }, channel_);
    }
    ipc::loan_t loan(std::size_t size)
    {
        return std::visit([size](auto& channel) { return channel.loan(size); }, channel_);
    }
    ipc::loan_t loan(std::size_t size, std::uint64_t timeout, ipc::loan_status& status)
    {
        return std::visit([&](auto& channel) { return channel.loan(size, timeout, status); }, channel_);
    }
    ipc::loan_t loan(std::size_t size, std::uint64_t timeout)
    {
        ipc::loan_status status;
        return loan(size, timeout, status);
    }
    bool inspect_pool(std::size_t size, ipc::pool_snapshot& out) const
    {
        return std::visit([&](const auto& channel) { return channel.inspect_pool(size, out); }, channel_);
    }
    bool publish_loan(const ipc::loan_t& loan, std::uint64_t timeout)
    {
        return std::visit([&](auto& channel) { return channel.publish_loan(loan, timeout); }, channel_);
    }
    bool publish_loan_size(const ipc::loan_t& loan, std::size_t used)
    {
        return std::visit([&](auto& channel) { return channel.publish_loan_size(loan, used); }, channel_);
    }
    void discard_loan(const ipc::loan_t& loan)
    {
        std::visit([&](auto& channel) { channel.discard_loan(loan); }, channel_);
    }
    void disconnect()
    {
        std::visit([](auto& channel) { channel.disconnect(); }, channel_);
    }
    void release() noexcept
    {
        std::visit([](auto& channel) { channel.release(); }, channel_);
    }
    void clear() noexcept
    {
        std::visit([](auto& channel) { channel.clear(); }, channel_);
    }

    static void clear_storage(const char* name, bool mpmc) noexcept
    {
        if (mpmc)
            ipc::mpmc_channel::clear_storage(name);
        else
            ipc::route::clear_storage(name);
    }

private:
    using channel_variant = std::variant<ipc::route, ipc::mpmc_channel>;

    bool mpmc_{false};
    channel_variant channel_;
};

} // namespace shm
} // namespace dzIPC
