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

namespace {

/* F1 最小清洗: 段名里的**内层** '/' 会让 POSIX shm_open 直接 EINVAL(22)
 * (POSIX 只接受 /somename 形; libipc 的 object_name() 只补**前导** '/' 不做校验)。
 *
 * 只清 '/' 而**不是**复用 sanitize_topic_name(), 是刻意的取舍 —— 后者把
 * [0-9A-Za-z_.-] 之外的**所有**字符映射成 '_', 那会**改名**含 ':' 等"合法文件名字符、
 * 但不在该集合"的**现行可用** topic: "rt:chatter" 今天的段名 dz_ipc_d3_rt:chatter 是
 * 合法文件名、**能建段**, 加全清洗后变成 dz_ipc_d3_rt_chatter ⇒ 新旧版本各建一段、
 * **静默不互通**。只清 '/' 的恒等性: topic 不含 '/' ⇒ 输出与本改动前**逐字节相同**
 * ⇒ 零改名、零迁移。
 *
 * 平台谓词: '/' 非法是 **POSIX** 的约束; Windows 的段名没有这条规则, 故保持原样,
 * 不改既有行为。 */
std::string minimal_segment_sanitize(const std::string& name)
{
#if defined(_WIN32)
    return name;
#else
    std::string out;
    out.reserve(name.size());
    for (char ch : name)
    {
        out.push_back(ch == '/' ? '_' : ch);
    }
    return out;
#endif
}

}   // namespace

std::string shm_service_prefix(const std::string& topic_name, size_t domain_id)
{
    /* ⛔ 此处旧注释称"传的已是处理过的名字"—— **对本函数的 ser-cli 调用方是错的**:
     * shm_ser_cli_ipc.cc 的 service_prefix_for()(:120-123) 原样转发**裸 topic**,
     * 所以清洗只能在这里做, 不能指望调用方。 */
    return "dz_ipc_d" + std::to_string(domain_id) + "_" + minimal_segment_sanitize(topic_name);
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
    /* "_control2": TopicControl 增加 PeerSlot 表后结构体变大, 沿用旧名会让
     * ipc::shm::handle::acquire() 在已存在的小段上 mmap 出超出文件长度的区域,
     * 访问越界部分直接 SIGBUS。换名等于强制新建一段, 同时也隔离了新旧版本进程。
     *
     * 这里传的是**原始** topic 名: sanitize 由 shm_topic_segment_name 负责, 且必须
     * 由它负责 —— 传 sanitize 之后的名字会二次清洗(幂等, 结果相同)但语义上多此一举,
     * 真正要防的是漏掉清洗。 */
    return shm_topic_segment_name(topic_name, domain_id) + "_control2";
}
