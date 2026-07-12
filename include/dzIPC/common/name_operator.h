#pragma once
#include <string>

std::string extract_last_segment(const std::string& full_name);

/**
 * @brief 将 topic 名称中的非法字符替换为 '_', 使其适合用作共享内存文件名
 *
 * 与 shm_pub_sub_ipc 中使用的清洗逻辑完全一致:
 * 只保留字母数字、'_'、'-'、'.', 其余字符替换为 '_'
 *
 * @param topic_name 原始 topic 名称 (如 "/demo/depth_image")
 * @return 清洗后不含前缀的名称 (如 "_demo_depth_image")
 */
std::string sanitize_topic_name(const std::string& topic_name);
