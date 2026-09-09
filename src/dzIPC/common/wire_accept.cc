#include "dzIPC/common/wire_accept.h"

#include "ipc_msg/ipc_msg_base/dzflat.h"

namespace dzIPC {

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

}   // namespace dzIPC
