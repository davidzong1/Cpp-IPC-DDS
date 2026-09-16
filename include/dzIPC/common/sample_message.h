#pragma once
/* Sample —— DZFlat 接收侧的借样视图持有者 (docs/dzflat_shm.md §4.3 的 Sample<T>)
 *
 * 发布侧有 LoanedMessage(生产者借 chunk 就地写), 这里是它的**接收镜像**: 订阅线程把
 * recv() 得到的 buff_t(它让 chunk 的 conns 引用计数在共享内存里保持非零)原样移进
 * Sample, 于是共享 chunk 在用户读字段期间不会被归还池中 —— 直到 Sample 析构, buff_t
 * 的删除器才跑 recycle_storage 归还。类型化读走生成的 XxxView(见 view<T>()), 全段零拷贝。
 *
 * 三条契约:
 *
 * ① **生命周期必须覆盖读取**。view<T>() 返回的 XxxView / span / string_view 都指向
 *    Sample 持有的共享内存。Sample 析构 → chunk 归还池 → 这些视图全部悬空。所以用户要
 *    在 Sample 活着的时候读完, 或者先 copy_to 到 owning 对象再放 Sample 走。
 *
 * ② **只在 DZFlat wire 上产生**。Sample 只能承载 DZFlat 段; TLV 消息没有可借的平坦段,
 *    走 get_clone()/try_get_clone() 物化。订阅线程按段首 magic 分流, 见 shm_pub_sub_ipc.cc
 *    的 subscribe 循环。因此 get()/try_get()(视图路径)与 get_clone()/try_get_clone()
 *    (物化路径)是**互补的两条队列**, 一个 wire 上通常只喂其中一条; 混合 wire(灰度期)
 *    需要调用方两条都 drain。
 *
 * ③ **move-only**。buff_t 独占 chunk 引用, 拷贝会双归。析构 / move 赋值都走默认实现。
 */
#include <cstdint>
#include <cstddef>
#include <utility>

#include "libipc/buffer.h"

namespace dzIPC {

class Sample
{
public:
    Sample() = default;

    /* 由订阅线程构造: 把 recv() 的 buff_t 连同段头信息移进来。 */
    Sample(ipc::buffer buf, std::uint32_t msg_id, std::uint32_t schema_hash) noexcept
        : buf_(std::move(buf))
        , msg_id_(msg_id)
        , schema_hash_(schema_hash)
    {
    }

    Sample(const Sample&) = delete;
    Sample& operator=(const Sample&) = delete;
    Sample(Sample&&) noexcept = default;
    Sample& operator=(Sample&&) noexcept = default;

    /// 是否持有一个借样的 DZFlat 段。
    bool valid() const noexcept { return !buf_.empty(); }
    explicit operator bool() const noexcept { return valid(); }

    /// DZFlat 段基址(共享内存, 生命周期随本对象)。
    const void* data() const noexcept { return buf_.data(); }

    /// 借到的 chunk 容量。段内有效数据是 SegHeader.total_size, 一般小于本值。
    std::size_t size() const noexcept { return buf_.size(); }

    /// 段头的 msg_id(已由订阅线程与话题比对通过)。
    std::uint32_t msg_id() const noexcept { return msg_id_; }

    /// 段头的 schema_hash(已由订阅线程与话题比对通过)。
    std::uint32_t schema_hash() const noexcept { return schema_hash_; }

    /// 把本段 bind 成该话题的只读 XxxView。类型不符(不应发生)时返回空 view。
    /// 例: auto v = s.view<dzIPC::Msg::StdImageFlat>();
    template<typename Flat>
    typename Flat::view_t view() const
    {
        return Flat::view_t::bind(data(), size());
    }

private:
    ipc::buffer buf_;
    std::uint32_t msg_id_ = 0;
    std::uint32_t schema_hash_ = 0;
};

}   // namespace dzIPC
