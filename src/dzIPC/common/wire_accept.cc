#include "dzIPC/common/wire_accept.h"

#include <atomic>

#include "ipc_msg/ipc_msg_base/dzflat.h"

namespace dzIPC {

namespace {

/* 守门命中的条数。relaxed 足够: 它是诊断计数, 只要求不丢增量(不要求跨线程顺序)。 */
std::atomic<std::uint64_t> g_wakeup_artifacts{0};

}   // namespace

std::uint64_t WakeupArtifactCount() noexcept
{
    return g_wakeup_artifacts.load(std::memory_order_relaxed);
}

void ResetWakeupArtifactCount() noexcept
{
    g_wakeup_artifacts.store(0, std::memory_order_relaxed);
}

void NoteWakeupArtifact() noexcept
{
    g_wakeup_artifacts.fetch_add(1, std::memory_order_relaxed);
}

IPC_EXPORT bool AcceptWire(const ipc::buffer& raw, std::uint32_t expected_msg_id, IpcMsgBase& sink)
{
    using E = detail::DzFlatRxEvent;

    if (dzflat::looks_like_dzflat(raw.data(), raw.size()))
    {
        std::uint32_t id = 0;
        if (!IpcMsgBase::dzflat_peek_msg_id(raw.data(), raw.size(), id) || id != expected_msg_id)
        {
            detail::NoteDzFlatRx(E::kDzFlatIdSkipped);
            return false;
        }
        /* dzflat_read 内部 bind_segment 会比对 schema_hash / root_size / 段长。走到这里
         * 说明 magic 与 layout_ver 已经对上、msg_id 也对上, 所以它再失败只可能是**结构
         * 不一致** —— 一定是两端的 msg 定义不是同一份。 */
        if (!sink.dzflat_read(raw.data(), raw.size()))
        {
            detail::NoteDzFlatRx(E::kDzFlatSchemaDrop);
            return false;
        }
        detail::NoteDzFlatRx(E::kDzFlatAccepted);
        return true;
    }

    /* magic 对上却没通过判别 ⇒ 段头自相矛盾, 或 layout_ver 不认识。不能任其落进下面的
     * TLV 分支: 那里的尾部校验必然失配, 于是一个**损坏的 DZFlat 段**会被记成"这条不是我
     * 的话题"这种正常过滤, 真信号就此消失。 */
    if (dzflat::has_dzflat_magic(raw.data(), raw.size()))
    {
        detail::NoteDzFlatRx(E::kDzFlatHeaderBad);
        return false;
    }

    if (!sink.check_id(raw, expected_msg_id))
    {
        detail::NoteDzFlatRx(E::kTlvIdSkipped);
        return false;
    }
    sink.deserialize(raw);
    /* TLV 的偏移全由缓冲自身内容算出, 一个被截断/错乱的缓冲能把偏移推到任意远处。
     * check_id 只看尾部 4 字节, 挡不住这种缓冲(偶然通过的概率是 1/2^32 × 消息量,
     * 实测会命中)。越界已被 adapt_memcpy_tods 拦下并置位, 这里据此整条丢弃
     * —— 见 docs/dzflat_known_issues.md 第 3 条。 */
    if (!sink.deserialize_ok())
    {
        detail::NoteDzFlatRx(E::kTlvCorruptDrop);
        return false;
    }
    detail::NoteDzFlatRx(E::kTlvAccepted);
    return true;
}

IPC_EXPORT bool IsWakeupArtifact(const ipc::buffer& raw) noexcept
{
    /* 长度先挡: 伪影恒为 ipc::data_length (recv 里 r_size = data_length + remain_(0))。
     * 这一条也是快路径 —— 正常消息绝大多数不进循环。 */
    if (raw.size() != static_cast<std::size_t>(ipc::data_length))
    {
        return false;
    }
    const auto* p = static_cast<const unsigned char*>(raw.data());
    /* size() == 64 而 data() == nullptr 不是任何正常出口: make_cache 分配失败给回的是
     * size()==0 的空 buffer (ipc.cpp:92-100), 分片重组也不会产出这种形状。而它拿去分流
     * 只会读空指针 —— 按「不可用 ⇒ 丢弃」的保守方向判为伪影。 */
    if (p == nullptr)
    {
        return true;
    }
    /* 整段扫描 —— **不是**只扫尾部。
     * 只扫尾 12 字节会把 size() == ipc::data_length 的**真** DZFlat 段判成伪影:
     * TestMsg 的 dzflat_size 恰好 64, data4 == false 时尾 12 字节恰好全零(实测),
     * 于是真消息被静默丢掉 —— 比原缺陷更难查。判据的宽窄必须与 wire_accept.h 的
     * 「为什么判据是整段全零」一致, 由 RealDzFlatOfArtifactLengthIsNotFlagged 守门。 */
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        if (p[i] != 0)
        {
            return false;
        }
    }
    return true;
}

}   // namespace dzIPC
