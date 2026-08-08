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
