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
    return "dz_ipc_d" + std::to_string(domain_id) + "_" + sanitize_topic_name(topic_name)
           + "_topic";
}

std::string shm_service_prefix(const std::string& topic_name, size_t domain_id)
{
    /* 注意: 服务侧历史上没有对 topic 名做 sanitize(传的已是处理过的名字), 这里保持原样,
     * 只补 domain 前缀 —— 改清洗规则会动到段名, 属于另一件事。 */
    return "dz_ipc_d" + std::to_string(domain_id) + "_" + topic_name;
}

std::string shm_topic_control_name(const std::string& topic_name, size_t domain_id)
{
    /* "_control2": TopicControl 增加 PeerSlot 表后结构体变大, 沿用旧名会让
     * ipc::shm::handle::acquire() 在已存在的小段上 mmap 出超出文件长度的区域,
     * 访问越界部分直接 SIGBUS。换名等于强制新建一段, 同时也隔离了新旧版本进程。
     *
     * 这里传的是**原始** topic 名: sanitize 由 shm_topic_segment_name 负责, 且必须
     * 由它负责 —— 传 sanitize 之后的名字会二次清洗(幂等, 结果相同)但语义上多此一举,
     * 真正要防的是漏掉清洗。 */
    return shm_topic_segment_name(topic_name, domain_id) + "_control2";
}
