#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ROS2风格服务文件到C++头文件生成器
生成包含客户端请求类和服务端响应类的C++头文件
"""

import os
import sys
import re
from typing import Dict, List, Optional

from msg_generator import FieldInfo, MessageGenerator, NestedTypeInfo


class ServiceGenerator(MessageGenerator):
    """服务生成器"""

    def __init__(self, nested_types: Optional[Dict[str, NestedTypeInfo]] = None):
        super().__init__(nested_types=nested_types)
        self.request_fields: List[FieldInfo] = []
        self.response_fields: List[FieldInfo] = []

    def parse_field_definition(self, line: str) -> Optional[FieldInfo]:
        """解析字段定义"""
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("//"):
            return None

        parts = line.split()
        if len(parts) >= 2:
            return self._resolve_field_info(parts[0], parts[1])
        return None

    def parse_srv_file(self, srv_file_path: str) -> None:
        """解析.srv文件"""
        self.current_base_name = os.path.splitext(os.path.basename(srv_file_path))[0]
        with open(srv_file_path, "r", encoding="utf-8") as f:
            content = f.read()

        parts = content.split("---")
        if len(parts) != 2:
            raise ValueError(
                "Invalid .srv file format. Expected request and response sections separated by '---'"
            )

        for line in parts[0].strip().split("\n"):
            field_info = self.parse_field_definition(line)
            if field_info:
                self.request_fields.append(field_info)

        for line in parts[1].strip().split("\n"):
            field_info = self.parse_field_definition(line)
            if field_info:
                self.response_fields.append(field_info)

    def generate_file_header(self, fields: List[FieldInfo]) -> str:
        nested_includes = []
        for field in fields:
            if field.is_nested and field.nested_info:
                include_path = field.nested_info.include_path
                if include_path not in nested_includes:
                    nested_includes.append(include_path)
        nested_include_lines = "".join([f'#include "{path}"\n' for path in nested_includes])

        return f"""#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
{nested_include_lines}
namespace dzIPC::Srv {{
"""

    def generate_class(self, class_name: str, fields: List[FieldInfo]) -> str:
        """生成完整的类"""
        old_fields = self.fields
        self.fields = fields
        try:
            parts = [
                f"class {class_name} : public IpcMsgBase",
                "{",
                "public:",
                "    /* 构造函数和析构函数 */",
                f"    {class_name}() = default;",
                f"    ~{class_name}() = default;",
                "",
                "    /* 成员变量 */",
                self.generate_member_variables(),
                self.generate_serialize_function(),
                self.generate_deserialize_function(),
                self.generate_clone_function(class_name),
                "};",
                "",
            ]
            return self._normalize_indent("\n".join(parts))
        finally:
            self.fields = old_fields

    def generate_hpp_file(self, base_name: str) -> str:
        """生成完整的.hpp文件"""
        if not re.fullmatch(r"[a-z][a-z0-9_]*", base_name):
            raise ValueError("Srv file name must start with a lowercase letter and contain only lowercase letters, digits, and underscores")

        class_name = self._snake_to_pascal(base_name)
        request_class_name = f"{class_name}Request"
        response_class_name = f"{class_name}Response"
        all_fields = self.request_fields + self.response_fields

        parts = [
            self.generate_file_header(all_fields),
            "// 请求类",
            self.generate_class(request_class_name, self.request_fields),
            "// 响应类",
            self.generate_class(response_class_name, self.response_fields),
            "} // namespace dzIPC::Srv",
        ]

        return self._normalize_indent("\n".join(parts)) + "\n"

    def process_srv_file(self, srv_file_path: str, output_dir: Optional[str] = None) -> str:
        """处理.srv文件并生成.hpp文件"""
        self.parse_srv_file(srv_file_path)

        base_name = os.path.splitext(os.path.basename(srv_file_path))[0]
        hpp_content = self.generate_hpp_file(base_name)

        if output_dir is None:
            output_dir = os.path.dirname(srv_file_path)

        output_file_path = os.path.join(output_dir, f"{base_name}.hpp")
        with open(output_file_path, "w", encoding="utf-8") as f:
            f.write(hpp_content)

        return output_file_path


def main():
    """主函数"""
    if len(sys.argv) != 2:
        print("用法: python3 srv_generator.py <srv_file_path>")
        print("示例: python3 srv_generator.py /path/to/service.srv")
        sys.exit(1)

    srv_file_path = sys.argv[1]

    if not os.path.exists(srv_file_path):
        print(f"错误: 文件 {srv_file_path} 不存在")
        sys.exit(1)

    if not srv_file_path.endswith(".srv"):
        print("错误: 输入文件必须是.srv格式")
        sys.exit(1)

    try:
        generator = ServiceGenerator()
        generator.process_srv_file(srv_file_path)
    except Exception as e:
        print(f"生成失败: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
