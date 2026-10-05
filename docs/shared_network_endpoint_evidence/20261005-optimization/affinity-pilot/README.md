# 固定CPU诊断（非正式验收）

每窗口8秒、预热2秒、100Hz。publisher=2、gateway=0、首个subscriber=4；完整CPU映射见result.json。这4个窗口仍有p50退化，不能声称绑核解决了问题。其二进制为127e9d3加ACK就绪立即处理修复，未含后续“解锁后notify”修复。没有并行构建或压力任务；保留全部样本。正式验收使用最终冻结版本与原all-CPU配置。
