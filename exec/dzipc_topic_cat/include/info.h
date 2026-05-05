#pragma once
#include "libipc/buffer.h"

namespace dzIPC {
struct sniffer_info
{
    explicit sniffer_info(ipc::buffer&& req, ipc::buffer&& res)
        : request(std::move(req))
        , response(std::move(res))
    {}

    ipc::buffer request;
    ipc::buffer response;
};
}   // namespace dzIPC