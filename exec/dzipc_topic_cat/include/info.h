#pragma once
#include "handshake_probe.h"
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

    /* ⛔ 这个手写的移动赋值**必须**逐个列出成员: 它不会自动搬运新增字段。
     * 实测代价 —— 少了下面那行 `hs = ...`, 观测数据在 `info = std::move(sniffer->try_recv())`
     * 这一步被静默丢掉, 表现为"探针明明 open 成功、屏幕上却什么都没有"(而且**不报错**)。
     * 加字段时记得同步这里, 或者改成 `= default`。 */
    sniffer_info& operator=(sniffer_info&& other) noexcept
    {
        if (this != &other)
        {
            request = std::move(other.request);
            response = std::move(other.response);
            hs = other.hs;
        }
        return *this;
    }

    sniffer_info& operator=(const sniffer_info&) = delete;

    ipc::buffer request{};
    ipc::buffer response{};
    /* ser-cli 握手通道(base+2)的只读观测快照。默认 opened==false,
     * 也就是"没开观测" —— 所有既有调用点不看这个字段, 行为与历史一致。 */
    handshake_snapshot hs{};
};
}   // namespace dzIPC