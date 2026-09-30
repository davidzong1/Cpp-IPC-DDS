#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "dzIPC/common/path_switch.h"
#include "dzIPC/common/srv_data.h"

namespace dzIPC {
class IPC_EXPORT ser_ipc_base
{
public:
    explicit ser_ipc_base(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                          std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id, bool verbose)
    {}

    virtual ~ser_ipc_base() = 0;
    virtual void reset_message(const std::shared_ptr<ServiceData>& msg) = 0;
    virtual void reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback) = 0;
    virtual void InitChannel(std::string extra_info) = 0;
    virtual bool handshake_completed() const = 0;

    /* 当下**实际**承载数据的传输 (T2 §5 / T2 §7 R5)。
     *
     * ⛔ 与构造期的 IPCType 不是一回事: IPCType 在构造期一次定死, 路径切换之后
     * 不再等于实际传输, 而它仍被用来记日志的 TransportKind —— 于是排查时会被
     * 日志误导。任何"当下真正走哪条路"的记录/判定都必须读这里。
     *
     * 默认 None = 本实现不汇报, 调用方应回落到构造期 IPCType。 */
    virtual path::Kind transport_current() const { return path::Kind::None; }
    /* 可观测状态; 默认是"什么都没发生"。 */
    virtual const path::Status& status() const { return default_status_; }

    std::atomic<bool> exit_flag{false};

private:
    path::Status default_status_;
};

class IPC_EXPORT cli_ipc_base
{
public:
    explicit cli_ipc_base(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                          bool verbose)
    {}

    virtual ~cli_ipc_base() = 0;
    virtual void InitChannel(std::string extra_info) = 0;
    virtual void reset_message(const std::shared_ptr<ServiceData>& msg) = 0;
    virtual bool send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm) = 0;
    virtual bool handshake_completed() const = 0;

    /* 语义同 ser_ipc_base::transport_current。 */
    virtual path::Kind transport_current() const { return path::Kind::None; }
    virtual const path::Status& status() const { return default_status_; }

    std::atomic<bool> exit_flag{false};

private:
    path::Status default_status_;
};

inline ser_ipc_base::~ser_ipc_base() = default;
inline cli_ipc_base::~cli_ipc_base() = default;
}   // namespace dzIPC
