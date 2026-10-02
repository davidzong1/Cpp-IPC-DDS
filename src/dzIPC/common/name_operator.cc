#include "dzIPC/common/channel_scope.h"
#include "dzIPC/common/name_operator.h"

std::string extract_last_segment(const std::string& full_name)
{
    // 查找最后一个 "::" 的位置
    size_t pos = full_name.rfind("::");

    // 如果找到 "::"，返回其后部分；否则返回原字符串
    return (pos != std::string::npos) ? full_name.substr(pos + 2)   // 跳过 "::" 的 2 个字符
                                      : full_name;
}

std::string sanitize_topic_name(const std::string& topic_name)
{
    std::string name;
    name.reserve(topic_name.size());
    for (char ch : topic_name)
    {
        const bool is_alnum = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        name.push_back(is_alnum || ch == '_' || ch == '-' || ch == '.' ? ch : '_');
    }
    return name;
}

std::string shm_topic_segment_name(const std::string& topic_name, size_t domain_id)
{
    return "dz_ipc_d" + std::to_string(domain_id) + "_s2_" + dzIPC::common::channel_scope_suffix(topic_name, domain_id, dzIPC::common::ScopeKind::PubSub)
           + "_topic";
}

std::string shm_service_prefix(const std::string& topic_name, size_t domain_id)
{
    // 原始名字参与身份，避免斜杠/下划线别名共享同一服务通道。
    return "dz_ipc_d" + std::to_string(domain_id) + "_s2_" + dzIPC::common::channel_scope_suffix(topic_name, domain_id, dzIPC::common::ScopeKind::Service);
}

std::string shm_service_legacy_prefix(const std::string& topic_name, size_t domain_id)
{
    /* F1 **之前**的规则(不清洗)。存在的唯一理由: 让"回滚 / 旧名探测"可表达。
     * ⛔ 诚实前提: 对含 '/' 的 topic, 本函数算出的名字必带内层 '/' ⇒ shm_open 恒
     * EINVAL ⇒ 它**永远不可能指向一个真实存在的段**。所以它**不是**迁移路径, 而是
     * 回归护栏 + 回滚基准: 若规则将来再变, 用它算旧名仍能与新名比对。 */
    return "dz_ipc_d" + std::to_string(domain_id) + "_" + topic_name;
}

std::string shm_topic_control_name(const std::string& topic_name, size_t domain_id)
{
    // 控制面由相同作用域的数据段名派生，二者的 domain 与原始话题身份一致。
    return shm_topic_segment_name(topic_name, domain_id) + "_control2";
}
