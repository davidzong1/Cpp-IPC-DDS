# 发布结果身份边界修复

`PublisherEndpoint::deliver()` 原来填写 `PublishOutcome.sequence` 和 `remote.publisher_id/sequence`，但本机直达/网关离线的 `publish()`、`prebuilt()` 仅填写最外层序号。这导致同一发布者在路径切换后，调用者看到的身份字段变成全零，无法一致关联返回结果。

新增内部 `identify()`，三个原有序号分配点统一填写身份。仍按发布对象独立分配一个序号；不修改 wire、可靠确认、配额、发送成功语义或错误重试。无网络提交时保留身份仅用于关联结果，不代表收到远端确认。

新增测试在同话题两个发布者上，分别验证本机直达/网关离线、预构造 DZFlat/对象发布，以及第一个发布者关闭后第二个发布者继续使用自己的身份与序号。每次提交接收恰好一次。修复前测试明确因返回身份/序号为零失败；修复后 partial_submit、public_api、prebuilt、local_direct 四组回归通过。详见 identity-red.log、identity-green.log。

这是一项确定的身份一致性修复，不是剩余三个延迟失败的关闭证据。
