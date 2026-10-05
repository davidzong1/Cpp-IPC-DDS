#pragma once
#include "dzIPC/net/wire_protocol.h"
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
namespace shared_net_test
{
inline dzIPC::net::Bytes vector(const std::string &key)
{
    std::ifstream file(DZIPC_GOLDEN_VECTORS);
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    const auto prefix = '"' + key + "\": \"";
    const auto pos = text.find(prefix);
    if (pos == std::string::npos)
        throw std::runtime_error("缺少参考向量：" + key);
    const auto start = pos + prefix.size();
    const auto end = text.find('"', start);
    dzIPC::net::Bytes bytes;
    for (auto i = start; i < end; i += 2)
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return bytes;
}
} // namespace shared_net_test
