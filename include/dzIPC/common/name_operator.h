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

/* ---------------------------------------------------------------------------
 * SHM 段名规则 —— **唯一出处**。
 *
 * 段名以前是各处各自拼字符串: 传输层一份、C++ sniffer 一份(其注释明写"sniffer 必须
 * 复刻完全相同的方案")、dzplot 一份、集成测试一份、单测里还硬编码了字面量。规则一改
 * 就得同时改五处, 漏掉哪处都是**静默失效**(sniffer 打不开就是一片空白, 不报错)。
 *
 * 所以规则收在这里, 谁要段名谁调这几个函数。Python 侧无法直接调 C++, 但至少可以对着
 * 这一处去核对, 而不是对着传输层实现去猜。
 *
 * 段名含 domain_id: 不含就等于 SHM 上没有 domain 隔离 —— 见 docs/shm_defect_fixes.md
 * 第 1 条。
 * ------------------------------------------------------------------------- */

/// pub/sub 数据段: "dz_ipc_d<domain>_<sanitized>_topic"
std::string shm_topic_segment_name(const std::string& topic_name, size_t domain_id);

/// 服务通道段名前缀: "dz_ipc_d<domain>_<topic>"; 请求/响应各自追加 "_ser_r"/"_ser_w"
std::string shm_service_prefix(const std::string& topic_name, size_t domain_id);

/* pub/sub 控制面段名(数据段名 + "_control2")。
 *
 * 与 shm_ser_cli_ipc.h 的 ser_service_control_name() 对称: 服务侧的控制面段名早就
 * "为占用判定"导出过一份, pub/sub 侧的对应字符串却一直锁在 shm_pub_sub_ipc.cc 的
 * 匿名 namespace 里(control_name_for, static)。结果是**每一处需要一个 pub/sub 控制面
 * 段名的代码都只能自己复刻这个后缀**, 而漏掉后缀 "2" 的代价不是报错而是静默失效:
 * 打开一个不存在的段不失败, 只是得到一个恒 Empty/generation==0 的空壳
 * (docs/shm_defect_fixes.md 第 1 条)。
 *
 * 已知的复刻点: exec/dzipc_topic_cat/include/control_plane_naming.h、
 * tools/dzplot/main.py::_control_plane_name_for_topic、test/test_shm_receiver_cap.cpp
 * 以及工具侧的文档注释。规则一改就得同时改这几处, 漏掉哪处都是静默失效 ——
 * 与数据段名当初收进本文件是同一个理由。 */
std::string shm_topic_control_name(const std::string& topic_name, size_t domain_id);
