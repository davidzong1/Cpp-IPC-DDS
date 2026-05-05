#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ROS2风格消息文件到C++头文件生成器
生成包含序列化和反序列化函数的C++类
"""

import os
import sys
import re
from typing import List, Dict, Tuple, Optional
from dataclasses import dataclass


@dataclass
class FieldInfo:
    """字段信息"""

    field_type: str  # 原始类型，如 int32, string[], float64[]
    field_name: str  # 字段名
    cpp_type: str  # C++类型
    is_array: bool  # 是否为数组
    is_string: bool  # 是否为字符串


class MessageGenerator:
    """消息生成器"""

    # ROS2类型到C++类型的映射
    TYPE_MAPPING = {
        "bool": "bool",
        "int8": "int8_t",
        "uint8": "uint8_t",
        "int16": "int16_t",
        "uint16": "uint16_t",
        "int32": "int32_t",
        "uint32": "uint32_t",
        "int64": "int64_t",
        "uint64": "uint64_t",
        "float32": "float",
        "float64": "double",
        "string": "std::string",
    }

    TYPE_INDEX = {
        "bool": 1,
        "int8": 2,
        "uint8": 3,
        "int16": 4,
        "uint16": 5,
        "int32": 6,
        "uint32": 7,
        "int64": 8,
        "uint64": 9,
        "float32": 10,
        "float64": 11,
        "string": 12,
        "bool[]": 13,
        "int8[]": 14,
        "uint8[]": 15,
        "int16[]": 16,
        "uint16[]": 17,
        "int32[]": 18,
        "uint32[]": 19,
        "int64[]": 20,
        "uint64[]": 21,
        "float32[]": 22,
        "float64[]": 23,
        "string[]": 24,
    }

    def __init__(self):
        self.fields: List[FieldInfo] = []

    def _normalize_indent(self, content: str, indent_size: int = 4) -> str:
        normalized_lines = []
        for line in content.split("\n"):
            if not line:
                normalized_lines.append("")
                continue
            stripped = line.lstrip(" ")
            leading_spaces = len(line) - len(stripped)
            normalized_leading = (leading_spaces // indent_size) * indent_size
            normalized_lines.append(" " * normalized_leading + stripped)
        return "\n".join(normalized_lines)

    def _get_type_index(self, field_type: str) -> int:
        if field_type not in self.TYPE_INDEX:
            raise ValueError(f"Unsupported type index: {field_type}")
        return self.TYPE_INDEX[field_type]

    def parse_msg_file(self, msg_file_path: str) -> None:
        """解析.msg文件"""
        with open(msg_file_path, "r", encoding="utf-8") as f:
            lines = f.readlines()

        for line in lines:
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            # 解析字段定义
            parts = line.split()
            if len(parts) >= 2:
                field_type = parts[0]
                field_name = parts[1]

                # 检查是否为数组
                is_array = field_type.endswith("[]")
                if is_array:
                    base_type = field_type[:-2]  # 移除[]
                else:
                    base_type = field_type

                # 检查是否为字符串
                is_string = base_type == "string"

                # 转换为C++类型
                if base_type in self.TYPE_MAPPING:
                    cpp_base_type = self.TYPE_MAPPING[base_type]
                else:
                    raise ValueError(f"Unsupported type: {base_type}")

                if is_array:
                    cpp_type = f"std::vector<{cpp_base_type}>"
                else:
                    cpp_type = cpp_base_type

                field_info = FieldInfo(
                    field_type=field_type,
                    field_name=field_name,
                    cpp_type=cpp_type,
                    is_array=is_array,
                    is_string=is_string,
                )
                self.fields.append(field_info)

    def generate_class_header(self, class_name: str) -> str:
        """生成类头部"""
        return f"""#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
namespace dzIPC::Msg {{
class {class_name} : public IpcMsgBase
{{
public:
    /* 构造函数和析构函数 */
    {class_name}() = default;
    ~{class_name}() = default;
"""

    def generate_member_variables(self) -> str:
        """生成成员变量"""
        lines = []
        for field in self.fields:
            comment = f"    /* {field.field_name} */"
            lines.append(f"     {field.cpp_type} {field.field_name};{comment}")
        return "\n".join(lines)

    def generate_constructors(self, class_name: str) -> str:
        """生成构造函数"""
        return f"""
    /* 成员变量 */"""

    def generate_serialize_function(self) -> str:
        """生成序列化函数"""
        lines = [
            "",
            "        /* 序列化函数 */",
            "        ipc::buffer serialize() override",
            "        {",
        ]

        # 计算各个字段的大小变量定义
        for i, field in enumerate(self.fields):
            if field.is_string and not field.is_array:
                lines.append(
                    f"          int32_t {field.field_name}_size = {field.field_name}.size() ;"
                )
            elif field.is_array and field.is_string:
                lines.append(
                    f"          int32_t {field.field_name}_count = {field.field_name}.size();"
                )
                lines.append(f"         int32_t {field.field_name}_total_size_ = 0;")
                lines.append(f"         for (const auto& str : {field.field_name}) {{")
                lines.append(
                    f"              {field.field_name}_total_size_ += sizeof(int32_t) + str.size() ;"
                )
                lines.append(f"         }}")
            elif field.is_array:
                base_type = field.cpp_type[12:-1]  # 从 std::vector<type> 中提取 type
                if base_type == "bool":
                    lines.append(
                        f"          int32_t {field.field_name}_count = {field.field_name}.size();"
                    )
                    lines.append(
                        f"          int32_t {field.field_name}_size = {field.field_name}_count * sizeof(bool);"
                    )
                else:
                    lines.append(
                        f"          int32_t {field.field_name}_count = {field.field_name}.size();"
                    )
                    lines.append(
                        f"          int32_t {field.field_name}_size = {field.field_name}_count * sizeof({base_type});"
                    )
            else:
                lines.append(
                    f"          int32_t {field.field_name}_size = sizeof({field.field_name});"
                )

        lines.append("")

        # 计算总缓冲区大小
        lines.append("          // 计算总缓冲区大小")
        lines.append("          size_t total_size_ = 0;")
        for field in self.fields:
            if field.is_string and not field.is_array:
                lines.append(
                    f"          total_size_ += sizeof(int32_t) + {len(field.field_name)};"
                )
                lines.append("          total_size_ += sizeof(uint8_t);")
                lines.append(
                    f"          total_size_ += sizeof({field.field_name}_size) + {field.field_name}_size;"
                )
            elif field.is_array and field.is_string:
                lines.append(
                    f"          total_size_ += sizeof(int32_t) + {len(field.field_name)};"
                )
                lines.append("          total_size_ += sizeof(uint8_t);")
                lines.append(
                    f"          total_size_ += sizeof({field.field_name}_count) + {field.field_name}_total_size_;"
                )
            elif field.is_array:
                lines.append(
                    f"          total_size_ += sizeof(int32_t) + {len(field.field_name)};"
                )
                lines.append("          total_size_ += sizeof(uint8_t);")
                lines.append(
                    f"          total_size_ += sizeof({field.field_name}_count) + {field.field_name}_size;"
                )
            else:
                lines.append(
                    f"          total_size_ += sizeof(int32_t) + {len(field.field_name)};"
                )
                lines.append("          total_size_ += sizeof(uint8_t);")
                lines.append(f"          total_size_ += {field.field_name}_size;")

        lines.append("")
        lines.append("          // 一次性分配缓冲区")
        lines.append(
            "           ipc::buffer buffer = std::move(this->serialize_data_cut(total_size_));"
        )
        lines.append("          uint32_t offset = 0;")
        lines.append("          uint16_t page = 1;")
        lines.append("")

        # 序列化每个字段
        for field in self.fields:
            if field.is_string and not field.is_array:
                # 单个字符串
                lines.extend(
                    [
                        f"          // 序列化 {field.field_name}",
                        f"          int32_t {field.field_name}_name_size = {len(field.field_name)};",
                        f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_name_size), page, offset, sizeof({field.field_name}_name_size));",
                        f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(\"{field.field_name}\"), page, offset, {field.field_name}_name_size);",
                        f"          uint8_t {field.field_name}_type = {self._get_type_index(field.field_type)};",
                        f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_type), page, offset, sizeof({field.field_name}_type));",
                        f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_size), page, offset, sizeof({field.field_name}_size));"
                        f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>({field.field_name}.data()), page, offset, {field.field_name}_size);",
                        "",
                    ]
                )
            elif field.is_array and field.is_string:
                # 字符串数组
                lines.extend(
                    [
                        f"          // 序列化 {field.field_name}",
                        f"          int32_t {field.field_name}_name_size = {len(field.field_name)};",
                        f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_name_size), page, offset, sizeof({field.field_name}_name_size));",
                        f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(\"{field.field_name}\"), page, offset, {field.field_name}_name_size);",
                        f"          uint8_t {field.field_name}_type = {self._get_type_index(field.field_type)};",
                        f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_type), page, offset, sizeof({field.field_name}_type));",
                        f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_count), page, offset, sizeof({field.field_name}_count));",
                        f"          for (const auto& str : {field.field_name}) {{",
                        f"              int32_t str_size = str.size();",
                        f"              this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&str_size), page, offset, sizeof(str_size));",
                        f"              this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(str.data()), page, offset, str_size);",
                        f"          }}",
                        "",
                    ]
                )
            elif field.is_array:
                # 基本类型数组
                base_type = field.cpp_type[12:-1]  # 从 std::vector<type> 中提取 type
                # 特殊处理 std::vector<bool>
                if base_type == "bool":
                    lines.extend(
                        [
                            f"          // 序列化 {field.field_name} (bool数组特殊处理)",
                            f"          int32_t {field.field_name}_name_size = {len(field.field_name)};",
                            f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_name_size), page, offset, sizeof({field.field_name}_name_size));",
                            f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(\"{field.field_name}\"), page, offset, {field.field_name}_name_size);",
                            f"          uint8_t {field.field_name}_type = {self._get_type_index(field.field_type)};",
                            f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_type), page, offset, sizeof({field.field_name}_type));",
                            f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_count), page, offset, sizeof({field.field_name}_count));",
                            f"          std::vector<uint8_t> bool_byte({field.field_name}_count, 0);",
                            f"          for (int32_t i = 0; i < {field.field_name}_count; ++i) {{",
                            f"              bool_byte[i] = {field.field_name}[i] ? 1 : 0;",
                            f"          }}",
                            f"          if ({field.field_name}_count > 0) {{",
                            f"              this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(bool_byte.data()), page, offset, {field.field_name}_count);",
                            f"          }}",
                            "",
                        ]
                    )
                else:
                    lines.extend(
                        [
                            f"          // 序列化 {field.field_name}",
                            f"          int32_t {field.field_name}_name_size = {len(field.field_name)};",
                            f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_name_size), page, offset, sizeof({field.field_name}_name_size));",
                            f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(\"{field.field_name}\"), page, offset, {field.field_name}_name_size);",
                            f"          uint8_t {field.field_name}_type = {self._get_type_index(field.field_type)};",
                            f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_type), page, offset, sizeof({field.field_name}_type));",
                            f"          this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_count), page, offset, sizeof({field.field_name}_count));",
                            f"          if ({field.field_name}_count > 0) {{",
                            f"              this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>({field.field_name}.data()), page, offset, {field.field_name}_size);",
                            f"          }}",
                            "",
                        ]
                    )
            else:
                # 基本类型(非数组)
                base_type = field.cpp_type[12:-1]  # 从 std::vector<type> 中提取 type
                # bool类型需要特殊处理，确保以1字节形式存储
                if base_type == "bool":
                    lines.extend(
                        [
                            f"        // 序列化 {field.field_name} (bool类型特殊处理)",
                            f"        int32_t {field.field_name}_name_size = {len(field.field_name)};",
                            f"        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_name_size), page, offset, sizeof({field.field_name}_name_size));",
                            f"        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(\"{field.field_name}\"), page, offset, {field.field_name}_name_size);",
                            f"        uint8_t {field.field_name}_type = {self._get_type_index(field.field_type)};",
                            f"        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_type), page, offset, sizeof({field.field_name}_type));",
                            f"        uint8_t {field.field_name}_byte = {field.field_name} ? 1 : 0;",
                            f"        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_byte), page, offset, sizeof({field.field_name}_byte));",
                            "",
                        ]
                    )
                else:
                    lines.extend(
                        [
                            f"        // 序列化 {field.field_name}",
                            f"        int32_t {field.field_name}_name_size = {len(field.field_name)};",
                            f"        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_name_size), page, offset, sizeof({field.field_name}_name_size));",
                            f"        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(\"{field.field_name}\"), page, offset, {field.field_name}_name_size);",
                            f"        uint8_t {field.field_name}_type = {self._get_type_index(field.field_type)};",
                            f"        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}_type), page, offset, sizeof({field.field_name}_type));",
                            f"        this->adapt_memcpy_tos(static_cast<uint8_t *>(buffer.data()), reinterpret_cast<const uint8_t *>(&{field.field_name}), page, offset, sizeof({field.field_name}));",
                            "",
                        ]
                    )
        # 添加尾部标识符
        lines.extend(
            [
                "         this->add_tail_msg(static_cast<uint8_t *>(buffer.data()) + offset, page);"
            ]
        )
        lines.extend(["         return buffer;", "      }"])

        return self._normalize_indent("\n".join(lines))

    def generate_deserialize_function(self) -> str:
        """生成反序列化函数"""
        lines = [
            "",
            "    /* 反序列化函数 */",
            "    void deserialize(const ipc::buffer& buffer) override",
            "    {",
            "           uint32_t offset = 0;",
            "           deserialize_data_cut(buffer.size());",
        ]

        # 反序列化每个字段
        for field in self.fields:
            if field.is_string and not field.is_array:
                # 单个字符串
                lines.extend(
                    [
                        f"          // 反序列化 {field.field_name}",
                        f"          int32_t {field.field_name}_name_size;",
                        f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_name_size));",
                        f"          offset += {field.field_name}_name_size;",
                        f"          uint8_t {field.field_name}_type;",
                        f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_type), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_type));",
                        f"          (void){field.field_name}_type;",
                        f"          int32_t {field.field_name}_size;",
                        f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_size));",
                        f"          {field.field_name}.resize({field.field_name}_size );",
                        f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>({field.field_name}.data()), static_cast<const uint8_t *>(buffer.data()), offset, {field.field_name}_size);",
                        "",
                    ]
                )
            elif field.is_array and field.is_string:
                # 字符串数组
                lines.extend(
                    [
                        f"        // 反序列化 {field.field_name}",
                        f"        int32_t {field.field_name}_name_size;",
                        f"        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_name_size));",
                        f"        offset += {field.field_name}_name_size;",
                        f"        offset += sizeof(uint8_t); // 跳过类型标识",
                        f"        int32_t {field.field_name}_count;",
                        f"        this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_count), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_count));",
                        f"        {field.field_name}.clear();",
                        f"        {field.field_name}.reserve({field.field_name}_count);",
                        f"        for (int32_t i = 0; i < {field.field_name}_count; ++i) {{",
                        f"            int32_t str_size;",
                        f"            this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&str_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof(str_size));",
                        f"            std::string str(str_size, '\\0');",
                        f"            this->adapt_memcpy_tods(reinterpret_cast<uint8_t*>(str.data()), static_cast<const uint8_t *>(buffer.data()), offset, str_size);",
                        f"            {field.field_name}.emplace_back(std::move(str));",
                        f"        }}",
                        "",
                    ]
                )
            elif field.is_array:
                # 基本类型数组
                base_type = field.cpp_type[12:-1]  # 从 std::vector<type> 中提取 type
                # 特殊处理 std::vector<bool>
                if base_type == "bool":
                    lines.extend(
                        [
                            f"          // 反序列化 {field.field_name} (bool数组特殊处理)",
                            f"          int32_t {field.field_name}_name_size;",
                            f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_name_size));",
                            f"          offset += {field.field_name}_name_size;",
                            f"          offset += sizeof(uint8_t); // 跳过类型标识",
                            f"          int32_t {field.field_name}_count;",
                            f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_count), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_count));",
                            f"          {field.field_name}.clear();",
                            f"          {field.field_name}.resize({field.field_name}_count);",
                            f"          if ({field.field_name}_count > 0) {{",
                            f"              std::vector<uint8_t> bool_byte({field.field_name}_count, 0);",
                            f"              this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(bool_byte.data()), static_cast<const uint8_t *>(buffer.data()), offset, {field.field_name}_count);",
                            f"              for (int32_t i = 0; i < {field.field_name}_count; ++i) {{",
                            f"                  {field.field_name}[i] = (bool_byte[i] != 0);",
                            f"              }}",
                            f"          }}",
                            "",
                        ]
                    )
                else:
                    lines.extend(
                        [
                            f"          // 反序列化 {field.field_name}",
                            f"          int32_t {field.field_name}_name_size;",
                            f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_name_size));",
                            f"          offset += {field.field_name}_name_size;",
                            f"          offset += sizeof(uint8_t); // 跳过类型标识",
                            f"          int32_t {field.field_name}_count;",
                            f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_count), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_count));",
                            f"          {field.field_name}.resize({field.field_name}_count);",
                            f"          if ({field.field_name}_count > 0) {{",
                            f"              this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>({field.field_name}.data()), static_cast<const uint8_t *>(buffer.data()), offset, {field.field_name}_count * sizeof({base_type}));",
                            f"          }}",
                            "",
                        ]
                    )
            else:
                # 基本类型
                base_type = field.cpp_type[12:-1]  # 从 std::vector<type> 中提取 type
                # bool类型需要特殊处理，确保以1字节形式存储
                if base_type == "bool":
                    lines.extend(
                        [
                            f"          // 反序列化 {field.field_name} (bool类型特殊处理)",
                            f"          int32_t {field.field_name}_name_size;",
                            f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_name_size));",
                            f"          offset += {field.field_name}_name_size;",
                            f"          offset += sizeof(uint8_t); // 跳过类型标识",
                            f"          uint8_t {field.field_name}_byte;",
                            f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_byte), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_byte));",
                            f"          {field.field_name} = ({field.field_name}_byte != 0);",
                            "",
                        ]
                    )
                else:
                    lines.extend(
                        [
                            f"          // 反序列化 {field.field_name}",
                            f"          int32_t {field.field_name}_name_size;",
                            f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}_name_size), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}_name_size));",
                            f"          offset += {field.field_name}_name_size;",
                            f"          offset += sizeof(uint8_t); // 跳过类型标识",
                            f"          this->adapt_memcpy_tods(reinterpret_cast<uint8_t *>(&{field.field_name}), static_cast<const uint8_t *>(buffer.data()), offset, sizeof({field.field_name}));",
                            "",
                        ]
                    )

        lines.append("      }")

        return self._normalize_indent("\n".join(lines))

    def generate_clone_function(self, class_name: str) -> str:
        """生成克隆函数"""
        return f"""
        /* 克隆函数 */
        {class_name}* clone() const override
        {{
            return new {class_name}(*this);
        }}"""

    def generate_class_footer(self) -> str:
        """生成类尾部"""
        return "\n};\n} // namespace dzIPC::Msg\n"

    def generate_hpp_file(self, class_name: str) -> str:
        """生成完整的.hpp文件"""
        parts = [
            self.generate_class_header(class_name),
            self.generate_constructors(class_name),
            "",
            self.generate_member_variables(),
            self.generate_serialize_function(),
            self.generate_deserialize_function(),
            self.generate_clone_function(class_name),
            self.generate_class_footer(),
        ]
        return self._normalize_indent("\n".join(parts))

    def process_msg_file(
        self, msg_file_path: str, output_dir: Optional[str] = None
    ) -> str:
        """处理.msg文件并生成.hpp文件"""
        # 解析消息文件
        self.parse_msg_file(msg_file_path)

        # 获取类名
        base_name = os.path.splitext(os.path.basename(msg_file_path))[0]
        if not re.fullmatch(r"[a-z_]+", base_name):
            raise ValueError("Msg file name must be lowercase letters and underscores only")

        class_name = "".join([part.capitalize() for part in base_name.split("_") if part])

        # 生成.hpp文件内容
        hpp_content = self.generate_hpp_file(class_name)

        # 确定输出路径
        if output_dir is None:
            output_dir = os.path.dirname(msg_file_path)

        output_file_path = os.path.join(output_dir, f"{base_name}.hpp")

        # 写入文件
        with open(output_file_path, "w", encoding="utf-8") as f:
            f.write(hpp_content)

        return output_file_path


def main():
    """主函数"""
    if len(sys.argv) != 2:
        print("用法: python3 msg_generator.py <msg_file_path>")
        print("示例: python3 msg_generator.py /path/to/message.msg")
        sys.exit(1)

    msg_file_path = sys.argv[1]

    if not os.path.exists(msg_file_path):
        print(f"错误: 文件 {msg_file_path} 不存在")
        sys.exit(1)

    if not msg_file_path.endswith(".msg"):
        print("错误: 输入文件必须是.msg格式")
        sys.exit(1)

    try:
        generator = MessageGenerator()
        output_file = generator.process_msg_file(msg_file_path)

    except Exception as e:
        print(f"生成失败: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
