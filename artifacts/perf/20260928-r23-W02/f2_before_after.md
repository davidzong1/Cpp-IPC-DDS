# W02-F2 修复前后同 case 列对照（r20 → r23）

| case（路径/尺寸/工作负载） | r20 samples `wire_bytes` | r23 samples `wire_bytes` | r20 summary `wire_bytes_per_msg` | r23 summary `wire_bytes_per_msg` | r23 `wire_bytes_source` |
|---|---|---|---|---|---|
| tlv / 64 B / full | 163 | 163 | 163 | 163 | 对象 serialize() 长度 |
| tlv / 1048576 B / full | 1063885 | 1063885 | 1063885 | 1063885 | 对象 serialize() 长度 |
| dzflat-a / 65536 B / full | 66448 | 66448 | 66448 | 66448 | 对象 dzflat_size() |
| dzflat-b / 64 B / full | 166 | 166 | 166 | 166 | B 借样 chunk 容量 |
| cyclonedds-udp / 64 B / full | 0 | 空字段 | 0 | 空字段 | not_collected(DDS 侧无 dzIPC wire 计数) |
| cyclonedds-udp / 1048576 B / full | 0 | 空字段 | 0 | 空字段 | not_collected(DDS 侧无 dzIPC wire 计数) |
| cyclonedds-iox / 64 B / full | 0 | 空字段 | 0 | 空字段 | not_collected(DDS 侧无 dzIPC wire 计数) |
| cyclonedds-iox / 1048576 B / full | 0 | 空字段 | 0 | 空字段 | not_collected(DDS 侧无 dzIPC wire 计数) |
