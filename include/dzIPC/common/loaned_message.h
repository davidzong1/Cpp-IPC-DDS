#pragma once
/* LoanedMessage —— DZFlat B 级借样发布的持有者 (docs/dzflat_shm.md §4.2)
 *
 * A 级(publish(shared_ptr<IpcMsgBase>))是「用户容器 → chunk」一次拷贝; B 级把 chunk
 * 直接交给调用方就地构造, 大负载**一次拷贝都没有**:
 *
 *     auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(h * step + 64);
 *     if (!lo.valid()) { ... 回退 A 级 publish ... }
 *     lo->set_width(w);
 *     lo->set_height(h);
 *     lo->header().set_frame_id("camera");
 *     auto px = lo->alloc_data(h * step);      // span 指向共享内存本身
 *     camera.read_into(px.data(), px.size());  // 相机直接写进 chunk
 *     pub.publish_loaned(std::move(lo));
 *
 * 三条契约:
 *
 * ① **容量必须由调用方给上界**。变长负载是就地写的, 借样时还不知道最终长度, 所以
 *    loan(varlen_budget) 的参数是"变长区最多要多少字节"。超预算时 alloc_* 返回空
 *    span、Writer 转入 !ok, publish_loaned 会失败并归还 chunk —— 不会写出坏段。
 *
 * ② **生命周期 RAII**。借到的 chunk 要么被 publish_loaned 交给队列(由最后一个接收方
 *    的 buff_t 析构归还), 要么在本对象析构时 discard。每个尺寸档位只有 32 块, 漏一块
 *    就少一块, 所以这里不给"忘记归还"留口子。move-only, 禁止拷贝。
 *
 * ③ **借样会失败, 且失败是常态**。无接收方 / chunk 池耗尽都会得到无效 LoanedMessage,
 *    调用方必须能回退到普通 publish。这不是异常路径, 是背压。
 */

#include <cstdint>
#include <memory>
#include <utility>

#include "ipc_msg/ipc_msg_base/dzflat.h"
#include "libipc/ipc.h"

namespace dzIPC {

/// \tparam Flat 由 generator 发射的 XxxFlat(提供 root_t / builder_t / kSchemaHash)
template<typename Flat>
class LoanedMessage
{
public:
    using Builder = typename Flat::builder_t;
    using Root = typename Flat::root_t;

    LoanedMessage() = default;

    /* 由 shm_pub_ipc::loan 调用。ch 需在本对象存活期间有效 —— 发布者持有它。 */
    LoanedMessage(std::shared_ptr<ipc::route> ch, ipc::loan_t lo, std::uint32_t msg_id)
        : ch_(std::move(ch)), lo_(lo), msg_id_(msg_id)
    {
        if (!ch_ || !lo_.valid())
        {
            return;
        }
        auto* seg = static_cast<std::uint8_t*>(lo_.data);
        const std::uint32_t varlen_off =
            dzflat::varlen_start(static_cast<std::uint32_t>(sizeof(Root)));
        if (lo_.size < varlen_off)
        {
            return;   /* 容量连 Root 都放不下, 保持无效 */
        }
        /* Root 区一次清零: chunk 是复用的共享内存, 成员间与尾部的填充字节若不清,
         * 会把上一条消息的内容泄漏给订阅方(与 Flat::write 同一处理)。 */
        std::memset(seg + sizeof(dzflat::SegHeader), 0, varlen_off - sizeof(dzflat::SegHeader));
        w_ = dzflat::Writer{seg, static_cast<std::uint32_t>(lo_.size), varlen_off};
        b_ = Builder(reinterpret_cast<Root*>(seg + sizeof(dzflat::SegHeader)), &w_);
        armed_ = true;
    }

    ~LoanedMessage() { discard(); }

    LoanedMessage(const LoanedMessage&) = delete;
    LoanedMessage& operator=(const LoanedMessage&) = delete;

    /* move 后必须重建 Builder 里的 Writer 指针: Writer 是本对象的成员, 地址随对象走。
     * 直接拷 b_ 会让它指向已移动对象的 w_ —— 悬垂。 */
    LoanedMessage(LoanedMessage&& rhs) noexcept { adopt(std::move(rhs)); }

    LoanedMessage& operator=(LoanedMessage&& rhs) noexcept
    {
        if (this != &rhs)
        {
            discard();
            adopt(std::move(rhs));
        }
        return *this;
    }

    bool valid() const noexcept { return armed_; }
    explicit operator bool() const noexcept { return armed_; }

    Builder* operator->() noexcept { return &b_; }
    Builder& operator*() noexcept { return b_; }

    /// 构造过程是否仍然健康(false = 超出变长预算)。
    bool ok() const noexcept { return armed_ && w_.ok(); }

    /// 已写入的段字节数(封口后即 SegHeader.total_size)。
    std::uint32_t size() const noexcept { return armed_ ? w_.size() : 0; }

    /* --- 以下两个只给 shm_pub_ipc 用 --- */

    /// 封口段头。失败(超预算)时不投递。
    bool finalize() { return armed_ && Flat::finalize(lo_.data, w_, msg_id_); }

    const ipc::loan_t& loan() const noexcept { return lo_; }

    /// 所有权交给队列后调用: 之后析构不再归还 chunk。
    void release() noexcept { armed_ = false; }

    /// 立刻归还 chunk(未投递时)。幂等。
    void discard() noexcept
    {
        if (armed_ && ch_)
        {
            ch_->discard_loan(lo_);
        }
        armed_ = false;
    }

private:
    void adopt(LoanedMessage&& rhs) noexcept
    {
        ch_ = std::move(rhs.ch_);
        lo_ = rhs.lo_;
        msg_id_ = rhs.msg_id_;
        w_ = rhs.w_;
        armed_ = rhs.armed_;
        /* Builder 只持 (Root*, Writer*)。Root* 指向 chunk(不动), Writer* 必须重指
         * 到**本对象**的 w_。 */
        if (armed_)
        {
            auto* seg = static_cast<std::uint8_t*>(lo_.data);
            b_ = Builder(reinterpret_cast<Root*>(seg + sizeof(dzflat::SegHeader)), &w_);
        }
        else
        {
            b_ = Builder{};
        }
        rhs.armed_ = false;
    }

    std::shared_ptr<ipc::route> ch_;
    ipc::loan_t lo_{};
    std::uint32_t msg_id_ = 0;
    dzflat::Writer w_{nullptr, 0, 0};
    Builder b_{};
    bool armed_ = false;
};

}   // namespace dzIPC
