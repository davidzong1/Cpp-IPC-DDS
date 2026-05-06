#pragma once
#include "libipc/buffer.h"

namespace dzIPC {
struct sniffer_info
{
    sniffer_info() {}

    sniffer_info(sniffer_info&& other) noexcept = default;
    sniffer_info(const sniffer_info&) = delete;

    sniffer_info(ipc::buffer&& req, ipc::buffer&& res)
        : request(std::move(req))
        , response(std::move(res))
    {}

    sniffer_info& operator=(sniffer_info&& other) noexcept
    {
        if (this != &other)
        {
            request = std::move(other.request);
            response = std::move(other.response);
        }
        return *this;
    }

    sniffer_info& operator=(const sniffer_info&) = delete;

    ipc::buffer request{};
    ipc::buffer response{};
};
}   // namespace dzIPC