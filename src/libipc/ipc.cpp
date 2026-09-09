
#include <type_traits>
#include <cstring>
#include <algorithm>
#include <utility> // std::pair, std::move, std::forward
#include <atomic>
#include <type_traits> // aligned_storage_t
#include <string>
#include <vector>
#include <array>
#include <cassert>
#include <mutex>

#include "libipc/ipc.h"
#include "libipc/def.h"
#include "libipc/shm.h"
#include "libipc/pool_alloc.h"
#include "libipc/queue.h"
#include "libipc/policy.h"
#include "libipc/rw_lock.h"
#include "libipc/waiter.h"

#include "libipc/utility/log.h"
#include "libipc/utility/id_pool.h"
#include "libipc/utility/scope_guard.h"
#include "libipc/utility/utility.h"

#include "libipc/memory/resource.h"
#include "libipc/platform/detail.h"
#include "libipc/circ/elem_array.h"

namespace
{

  using msg_id_t = std::uint32_t;
  using acc_t = std::atomic<msg_id_t>;

  template <std::size_t DataSize, std::size_t AlignSize>
  struct msg_t;

  template <std::size_t AlignSize>
  struct msg_t<0, AlignSize>
  {
    msg_id_t cc_id_;
    msg_id_t id_;
    std::int32_t remain_;
    bool storage_;
  };

  template <std::size_t DataSize, std::size_t AlignSize>
  struct msg_t : msg_t<0, AlignSize>
  {
    std::aligned_storage_t<DataSize, AlignSize> data_{};

    msg_t() = default;
    msg_t(msg_id_t cc_id, msg_id_t id, std::int32_t remain, void const *data,
          std::size_t size)
        : msg_t<0, AlignSize>{cc_id, id, remain,
                              (data == nullptr) || (size == 0)}
    {
      if (this->storage_)
      {
        if (data != nullptr)
        {
          // copy storage-id
          *reinterpret_cast<ipc::storage_id_t *>(&data_) =
              *static_cast<ipc::storage_id_t const *>(data);
        }
      }
      else
        std::memcpy(&data_, data, size);
    }
  };

  template <typename T>
  ipc::buff_t make_cache(T &data, std::size_t size)
  {
    auto ptr = ipc::mem::alloc(size);
    std::memcpy(ptr, &data, (ipc::detail::min)(sizeof(data), size));
    return {ptr, size, ipc::mem::free};
  }

  acc_t *cc_acc(ipc::string const &pref)
  {
    static ipc::unordered_map<ipc::string, ipc::shm::handle> handles;
    static std::mutex lock;
    std::lock_guard<std::mutex> guard{lock};
    auto it = handles.find(pref);
    if (it == handles.end())
    {
      ipc::string shm_name{ipc::make_prefix(pref, {"CA_CONN__"})};
      ipc::shm::handle h;
      if (!h.acquire(shm_name.c_str(), sizeof(acc_t)))
      {
        ipc::error("[cc_acc] acquire failed: %s\n", shm_name.c_str());
        return nullptr;
      }
      it = handles.emplace(pref, std::move(h)).first;
    }
    return static_cast<acc_t *>(it->second.get());
  }

  struct cache_t
  {
    std::size_t fill_;
    ipc::buff_t buff_;

    cache_t(std::size_t f, ipc::buff_t &&b) : fill_(f), buff_(std::move(b)) {}

    void append(void const *data, std::size_t size)
    {
      if (fill_ >= buff_.size() || data == nullptr || size == 0)
        return;
      auto new_fill = (ipc::detail::min)(fill_ + size, buff_.size());
      std::memcpy(static_cast<ipc::byte_t *>(buff_.data()) + fill_, data,
                  new_fill - fill_);
      fill_ = new_fill;
    }
  };

  struct conn_info_head
  {
    ipc::string prefix_;
    ipc::string name_;
    msg_id_t cc_id_; // connection-info id
    ipc::detail::waiter cc_waiter_, wt_waiter_, rd_waiter_;
    ipc::shm::handle acc_h_;

    conn_info_head(char const *prefix, char const *name)
        : prefix_{ipc::make_string(prefix)},
          name_{ipc::make_string(name)},
          cc_id_{} {}

    void init()
    {
      if (!cc_waiter_.valid())
        cc_waiter_.open(ipc::make_prefix(prefix_, {"CC_CONN__", name_}).c_str());
      if (!wt_waiter_.valid())
        wt_waiter_.open(ipc::make_prefix(prefix_, {"WT_CONN__", name_}).c_str());
      if (!rd_waiter_.valid())
        rd_waiter_.open(ipc::make_prefix(prefix_, {"RD_CONN__", name_}).c_str());
      if (!acc_h_.valid())
        acc_h_.acquire(ipc::make_prefix(prefix_, {"AC_CONN__", name_}).c_str(),
                       sizeof(acc_t));
      if (cc_id_ != 0)
      {
        return;
      }
      acc_t *pacc = cc_acc(prefix_);
      if (pacc == nullptr)
      {
        // Failed to obtain the global accumulator.
        return;
      }
      cc_id_ = pacc->fetch_add(1, std::memory_order_relaxed) + 1;
      if (cc_id_ == 0)
      {
        // The identity cannot be 0.
        cc_id_ = pacc->fetch_add(1, std::memory_order_relaxed) + 1;
      }
    }

    void clear() noexcept
    {
      cc_waiter_.clear();
      wt_waiter_.clear();
      rd_waiter_.clear();
      acc_h_.clear();
    }

    static void clear_storage(char const *prefix, char const *name) noexcept
    {
      auto p = ipc::make_string(prefix);
      auto n = ipc::make_string(name);
      ipc::detail::waiter::clear_storage(
          ipc::make_prefix(p, {"CC_CONN__", n}).c_str());
      ipc::detail::waiter::clear_storage(
          ipc::make_prefix(p, {"WT_CONN__", n}).c_str());
      ipc::detail::waiter::clear_storage(
          ipc::make_prefix(p, {"RD_CONN__", n}).c_str());
      ipc::shm::handle::clear_storage(
          ipc::make_prefix(p, {"AC_CONN__", n}).c_str());
    }

    void quit_waiting()
    {
      cc_waiter_.quit_waiting();
      wt_waiter_.quit_waiting();
      rd_waiter_.quit_waiting();
    }

    auto acc() { return static_cast<acc_t *>(acc_h_.get()); }

    auto &recv_cache()
    {
      thread_local ipc::unordered_map<msg_id_t, cache_t> tls;
      return tls;
    }
  };

  IPC_CONSTEXPR_ std::size_t align_chunk_size(std::size_t size) noexcept
  {
    return (((size - 1) / ipc::large_msg_align) + 1) * ipc::large_msg_align;
  }

  IPC_CONSTEXPR_ std::size_t calc_chunk_size(std::size_t size) noexcept
  {
    return ipc::make_align(
        alignof(std::max_align_t),
        align_chunk_size(ipc::make_align(alignof(std::max_align_t),
                                         sizeof(std::atomic<ipc::circ::cc_t>)) +
                         size));
  }

  struct chunk_t
  {
    std::atomic<ipc::circ::cc_t> &conns() noexcept
    {
      return *reinterpret_cast<std::atomic<ipc::circ::cc_t> *>(this);
    }

    void *data() noexcept
    {
      return reinterpret_cast<ipc::byte_t *>(this) +
             ipc::make_align(alignof(std::max_align_t),
                             sizeof(std::atomic<ipc::circ::cc_t>));
    }
  };

  struct chunk_info_t
  {
    ipc::id_pool<> pool_;
    ipc::spin_lock lock_;

    IPC_CONSTEXPR_ static std::size_t chunks_mem_size(
        std::size_t chunk_size) noexcept
    {
      return ipc::id_pool<>::max_count * chunk_size;
    }

    ipc::byte_t *chunks_mem() noexcept
    {
      return reinterpret_cast<ipc::byte_t *>(this + 1);
    }

    chunk_t *at(std::size_t chunk_size, ipc::storage_id_t id) noexcept
    {
      if (id < 0)
        return nullptr;
      return reinterpret_cast<chunk_t *>(chunks_mem() + (chunk_size * id));
    }
  };

  /* chunk 负载区的 8 字节对齐契约。
   *
   * chunk->data() = 共享段基址 + sizeof(chunk_info_t) + chunk_size * id + 16。
   * 段基址来自 mmap(页对齐), chunk_size 是 large_msg_align(1024) 的整数倍, 头部
   * 偏移 16 亦为 8 的倍数 —— 因此只要 sizeof(chunk_info_t) 是 8 的倍数, 负载区
   * 就一定 8 字节对齐。
   *
   * 这个前提原本只是"恰好成立"(id_pool 的 alignof 为 1, spin_lock 为 4), 没有任何
   * 东西保证它。DZFlat 会在负载区里原地读写 double/uint64(见 docs/dzflat_shm.md
   * §3.6), x86 容忍非对齐访问而 ARM 不一定, 故在此钉死: 一旦 chunk_info_t 的成员
   * 变化导致对齐塌掉, 这里编译期就会失败, 而不是在 ARM 上运行期出错。
   */
  static_assert(sizeof(chunk_info_t) % 8 == 0,
                "chunk payload must stay 8-byte aligned: see docs/dzflat_shm.md 3.6");

  auto &chunk_storages()
  {
    class chunk_handle_t
    {
      ipc::unordered_map<ipc::string, ipc::shm::handle> handles_;
      std::mutex lock_;

      static bool make_handle(ipc::shm::handle &h, ipc::string const &shm_name,
                              std::size_t chunk_size)
      {
        if (!h.valid() &&
            !h.acquire(shm_name.c_str(),
                       sizeof(chunk_info_t) +
                           chunk_info_t::chunks_mem_size(chunk_size)))
        {
          ipc::error(
              "[chunk_storages] chunk_shm.id_info_.acquire failed: chunk_size = "
              "%zd\n",
              chunk_size);
          return false;
        }
        return true;
      }

    public:
      chunk_info_t *get_info(conn_info_head *inf, std::size_t chunk_size)
      {
        ipc::string pref{(inf == nullptr) ? ipc::string{} : inf->prefix_};
        ipc::string shm_name{
            ipc::make_prefix(pref, {"CHUNK_INFO__", ipc::to_string(chunk_size)})};
        ipc::shm::handle *h;
        {
          std::lock_guard<std::mutex> guard{lock_};
          h = &(handles_[pref]);
          if (!make_handle(*h, shm_name, chunk_size))
          {
            return nullptr;
          }
        }
        auto *info = static_cast<chunk_info_t *>(h->get());
        if (info == nullptr)
        {
          ipc::error(
              "[chunk_storages] chunk_shm.id_info_.get failed: chunk_size = "
              "%zd\n",
              chunk_size);
          return nullptr;
        }
        return info;
      }
    };
    using deleter_t = void (*)(chunk_handle_t *);
    using chunk_handle_ptr_t = std::unique_ptr<chunk_handle_t, deleter_t>;
    static ipc::map<std::size_t, chunk_handle_ptr_t> chunk_hs;
    return chunk_hs;
  }

  chunk_info_t *chunk_storage_info(conn_info_head *inf, std::size_t chunk_size)
  {
    auto &storages = chunk_storages();
    std::decay_t<decltype(storages)>::iterator it;
    {
      static ipc::rw_lock lock;
      IPC_UNUSED_ std::shared_lock<ipc::rw_lock> guard{lock};
      if ((it = storages.find(chunk_size)) == storages.end())
      {
        using chunk_handle_ptr_t =
            std::decay_t<decltype(storages)>::value_type::second_type;
        using chunk_handle_t = chunk_handle_ptr_t::element_type;
        guard.unlock();
        IPC_UNUSED_ std::lock_guard<ipc::rw_lock> guard{lock};
        it = storages
                 .emplace(chunk_size,
                          chunk_handle_ptr_t{
                              ipc::mem::alloc<chunk_handle_t>(),
                              [](chunk_handle_t *p)
                              { ipc::mem::destruct(p); }})
                 .first;
      }
    }
    return it->second->get_info(inf, chunk_size);
  }

  /* 借样(loan)的容量档位。
   *
   * 为什么不能按精确长度借: 共享段是按 chunk_size 分段命名的
   * (CHUNK_INFO__<chunk_size>, 见 chunk_storage_info), 而 calc_chunk_size 只按
   * large_msg_align(1024) 取整。变长负载(压缩图 / 点云)每 1KB 就会开一个新段,
   * 映射对象数无界增长 —— 借样把 chunk 的持有期拉长后, 同时活着的段会更多。
   *
   * 档位设计: 小消息按 1KB 台阶(≤64KB 共 64 档, 与既有 send 路径同粒度, 不引入
   * 新段), 大消息按 2 的幂(段数对数增长, 最多再加十几档)。代价是大消息最坏浪费
   * 接近一半容量 —— 但 chunk 是 tmpfs 上的稀疏映射, 只有真正写到的页才占物理内存,
   * 而我们只写 total_size 那一段。
   */
  IPC_CONSTEXPR_ std::size_t loan_size_class(std::size_t size) noexcept
  {
    if (size <= 64 * 1024)
    {
      return ((size + ipc::large_msg_align - 1) / ipc::large_msg_align) *
             ipc::large_msg_align;
    }
    std::size_t c = 128 * 1024;
    while (c < size)
    {
      std::size_t nxt = c << 1;
      if (nxt < c)
        return size; // 溢出: 退回精确值, 后续 acquire 会失败
      c = nxt;
    }
    return c;
  }

  std::pair<ipc::storage_id_t, void *> acquire_storage(conn_info_head *inf,
                                                       std::size_t size,
                                                       ipc::circ::cc_t conns)  {
    std::size_t chunk_size = calc_chunk_size(size);
    auto info = chunk_storage_info(inf, chunk_size);
    if (info == nullptr)
      return {};

    info->lock_.lock();
    info->pool_.prepare();
    // got an unique id
    auto id = info->pool_.acquire();
    info->lock_.unlock();

    auto chunk = info->at(chunk_size, id);
    if (chunk == nullptr)
      return {};
    chunk->conns().store(conns, std::memory_order_relaxed);
    return {id, chunk->data()};
  }

  void *find_storage(ipc::storage_id_t id, conn_info_head *inf,
                     std::size_t size)
  {
    if (id < 0)
    {
      ipc::error("[find_storage] id is invalid: id = %ld, size = %zd\n", (long)id,
                 size);
      return nullptr;
    }
    std::size_t chunk_size = calc_chunk_size(size);
    auto info = chunk_storage_info(inf, chunk_size);
    if (info == nullptr)
      return nullptr;
    return info->at(chunk_size, id)->data();
  }

  void release_storage(ipc::storage_id_t id, conn_info_head *inf,
                       std::size_t size)
  {
    if (id < 0)
    {
      ipc::error("[release_storage] id is invalid: id = %ld, size = %zd\n",
                 (long)id, size);
      return;
    }
    std::size_t chunk_size = calc_chunk_size(size);
    auto info = chunk_storage_info(inf, chunk_size);
    if (info == nullptr)
      return;
    info->lock_.lock();
    info->pool_.release(id);
    info->lock_.unlock();
  }

  template <ipc::relat Rp, ipc::relat Rc>
  bool sub_rc(ipc::wr<Rp, Rc, ipc::trans::unicast>,
              std::atomic<ipc::circ::cc_t> & /*conns*/,
              ipc::circ::cc_t /*curr_conns*/,
              ipc::circ::cc_t /*conn_id*/) noexcept
  {
    return true;
  }

  template <ipc::relat Rp, ipc::relat Rc>
  bool sub_rc(ipc::wr<Rp, Rc, ipc::trans::broadcast>,
              std::atomic<ipc::circ::cc_t> &conns, ipc::circ::cc_t curr_conns,
              ipc::circ::cc_t conn_id) noexcept
  {
    auto last_conns = curr_conns & ~conn_id;
    for (unsigned k = 0;;)
    {
      auto chunk_conns = conns.load(std::memory_order_acquire);
      if (conns.compare_exchange_weak(chunk_conns, chunk_conns & last_conns,
                                      std::memory_order_release))
      {
        return (chunk_conns & last_conns) == 0;
      }
      ipc::yield(k);
    }
  }

  template <typename Flag>
  void recycle_storage(ipc::storage_id_t id, conn_info_head *inf,
                       std::size_t size, ipc::circ::cc_t curr_conns,
                       ipc::circ::cc_t conn_id)
  {
    if (id < 0)
    {
      ipc::error("[recycle_storage] id is invalid: id = %ld, size = %zd\n",
                 (long)id, size);
      return;
    }
    std::size_t chunk_size = calc_chunk_size(size);
    auto info = chunk_storage_info(inf, chunk_size);
    if (info == nullptr)
      return;

    auto chunk = info->at(chunk_size, id);
    if (chunk == nullptr)
      return;

    if (!sub_rc(Flag{}, chunk->conns(), curr_conns, conn_id))
    {
      return;
    }
    info->lock_.lock();
    info->pool_.release(id);
    info->lock_.unlock();
  }

  /* 覆写槽位时对被丢弃消息的 chunk 做条件归还。
   *
   * rem_cc = force_push 覆写前仍未 pop 该槽位的接收方位图(见 prod_cons.h 的
   * broadcast force_push), 也就是"永远看不到被覆写这条消息"的那一批。
   *
   *   清掉 rem_cc 的位后 == 0 → 没有任何接收方还握着这块 → 立即归还 id;
   *                      != 0 → 仍有接收方 pop 过而尚未释放其 buff_t →
   *                              **不归还**, 交由最后一个持有者的 buff_t 析构
   *                              (recycle_storage)归还。
   *
   * 为什么必须清 rem_cc 的位: chunk 的 conns 位图在发送时被初始化为当时的接收方
   * 集合, 只有"pop 到该消息并释放 buff_t"才会清位。被覆写的消息那些接收方永远
   * 读不到, 其位若不在此清掉, 位图永不归零 → chunk 永久泄漏(32 槽/尺寸类, 见
   * id_pool::max_count)。旧实现正是为此才无条件 release_storage。
   *
   * 为什么不能无条件 release: 无条件归还会把"正被接收方持有的 chunk id"直接放回
   * 池子, 下一帧 acquire 到同一 id 就会覆写持有者正在读的内存, 且持有者析构时会
   * 造成同一 id 二次入池(两条消息拿到同一块)。今天接收方拿到 buff_t 后立刻拷出
   * 就丢, 窗口是微秒级; 一旦让接收方长期持有 chunk(DZFlat 的 Sample), 该窗口会
   * 被拉成秒级并必现。
   *
   * 残余窗口(本函数未关闭): pop() 先把槽位数据拷出、之后才清自己的 rc 位, 所以
   * 一个"已读到 storage id 但尚未清位"的接收方仍会被算进 rem_cc。这与 prod_cons.h
   * force_push 注释里已登记的"覆写与 pop 无互斥 → 数据撕裂"同源, 需在 pop() 侧
   * 增加覆写检测才能根除。
   */
  void discard_storage(ipc::storage_id_t id, conn_info_head *inf,
                       std::size_t size, ipc::circ::cc_t rem_cc)
  {
    if (id < 0)
    {
      ipc::error("[discard_storage] id is invalid: id = %ld, size = %zd\n",
                 (long)id, size);
      return;
    }
    /* rem_cc == 0 ⇒ 该消息**所有**本该收到它的读方都已经 pop 过, 于是这块 chunk 的
     * 生命周期完全归它们的 buff_t 所有 —— 写方在此无权归还。
     *
     * 不加这道闸的后果是双重入池: 最后一个持有者的 recycle_storage 已经把 id 放回池子,
     * 写方再放一次, id_pool::release 的头插就把空闲链表接成自环
     * (next_[id] = cursor_ 而 cursor_ 已是 id), 此后每次 acquire 都返回同一个 id ——
     * 所有大消息共用一块 chunk, 内容互相踩踏。实测表现为吞吐用例直接挂死。
     *
     * 这也让 unicast 策略天然安全: 它的 rem_cc 恒为 0(其 push 只取读方已放行的格子),
     * 于是这里恒早返回。 */
    if (rem_cc == 0)
      return;

    std::size_t chunk_size = calc_chunk_size(size);
    auto info = chunk_storage_info(inf, chunk_size);
    if (info == nullptr)
      return;

    auto chunk = info->at(chunk_size, id);
    if (chunk == nullptr)
      return;

    auto &conns = chunk->conns();
    for (unsigned k = 0;;)
    {
      auto cur_conns = conns.load(std::memory_order_acquire);
      auto nxt_conns = static_cast<ipc::circ::cc_t>(cur_conns & ~rem_cc);
      if (conns.compare_exchange_weak(cur_conns, nxt_conns,
                                      std::memory_order_release))
      {
        if (nxt_conns != 0)
        {
          return; // 仍有持有者, 由其 buff_t 析构归还
        }
        break;
      }
      ipc::yield(k);
    }
    info->lock_.lock();
    info->pool_.release(id);
    info->lock_.unlock();
  }

  template <typename MsgT, typename Flag>
  bool clear_message(conn_info_head *inf, void *p, ipc::circ::cc_t rem_cc)
  {
    auto msg = static_cast<MsgT *>(p);
    /* rem_cc 是判定的**唯一依据**: 它非空才说明"有读方本该收到这条消息却永远不会来取",
     * 也只有那时写方才有权归还其 chunk。
     *
     * rem_cc == 0 时必须什么都不做 —— 该消息所有该收的读方都已 pop 过, chunk 的生命
     * 周期完全归它们的 buff_t。此时若再归还一次, 就是双重入池: id_pool::release 的头插
     * 会把空闲链表接成自环(next_[id] = cursor_ 而 cursor_ 已是 id), 此后每次 acquire
     * 都返回同一个 id, 所有大消息共用一块 chunk 互相踩踏。实测表现为吞吐用例直接挂死。
     *
     * 这条也让 unicast 天然安全: 它的 push 只取读方已放行的格子(rem_cc 恒 0), 而它的
     * force_push 从不调用本回调, 于是恒早返回。
     *
     * 详见 docs/dzflat_known_issues.md 第 4 条。 */
    if (msg->storage_ && (rem_cc != 0))
    {
      std::int32_t r_size =
          static_cast<std::int32_t>(ipc::data_length) + msg->remain_;
      if (r_size <= 0)
      {
        ipc::error("[clear_message] invalid msg size: %d\n", (int)r_size);
        return true;
      }
      auto id = *reinterpret_cast<ipc::storage_id_t *>(&msg->data_);
      auto sz = static_cast<std::size_t>(r_size);
      if constexpr (ipc::relat_trait<Flag>::is_broadcast)
      {
        discard_storage(id, inf, sz, rem_cc);
      }
      else
      {
        release_storage(id, inf, sz);
      }
    }
    return true;
  }

  template <typename W, typename F>
  bool wait_for(W &waiter, F &&pred, std::uint64_t tm)
  {
    if (tm == 0)
      return !pred();
    for (unsigned k = 0; pred();)
    {
      bool ret = true;
      ipc::sleep(k, [&k, &ret, &waiter, &pred, tm]
                 {
      ret = waiter.wait_if(std::forward<F>(pred), tm);
      k = 0; });
      if (!ret)
        return false; // timeout or fail
      if (k == 0)
        break; // k has been reset
    }
    return true;
  }

  template <typename Policy, std::size_t DataSize = ipc::data_length,
            std::size_t AlignSize =
                (ipc::detail::min)(DataSize, alignof(std::max_align_t))>
  struct queue_generator
  {
    using queue_t = ipc::queue<msg_t<DataSize, AlignSize>, Policy>;

    struct conn_info_t : conn_info_head
    {
      queue_t que_;

      conn_info_t(char const *pref, char const *name)
          : conn_info_head{pref, name}
      {
        init();
      }

      void init()
      {
        conn_info_head::init();
        if (!que_.valid())
        {
          que_.open(ipc::make_prefix(prefix_, {"QU_CONN__", this->name_, "__",
                                               ipc::to_string(DataSize), "__",
                                               ipc::to_string(AlignSize)})
                        .c_str());
        }
      }

      void clear() noexcept
      {
        que_.clear();
        conn_info_head::clear();
      }

      static void clear_storage(char const *prefix, char const *name) noexcept
      {
        queue_t::clear_storage(
            ipc::make_prefix(
                ipc::make_string(prefix),
                {"QU_CONN__", ipc::make_string(name), "__",
                 ipc::to_string(DataSize), "__", ipc::to_string(AlignSize)})
                .c_str());
        conn_info_head::clear_storage(prefix, name);
      }

      void disconnect_receiver()
      {
        bool dis = que_.disconnect();
        this->quit_waiting();
        if (dis)
        {
          this->recv_cache().clear();
        }
      }
    };
  };

  template <typename Policy>
  struct detail_impl
  {
    using policy_t = Policy;
    using flag_t = typename policy_t::flag_t;
    using queue_t = typename queue_generator<policy_t>::queue_t;
    using conn_info_t = typename queue_generator<policy_t>::conn_info_t;

    constexpr static conn_info_t *info_of(ipc::handle_t h) noexcept
    {
      return static_cast<conn_info_t *>(h);
    }

    constexpr static queue_t *queue_of(ipc::handle_t h) noexcept
    {
      return (info_of(h) == nullptr) ? nullptr : &(info_of(h)->que_);
    }

    static void notify_readers(conn_info_t *info, std::true_type)
    {
      info->rd_waiter_.broadcast();
    }

    static void notify_readers(conn_info_t *info, std::false_type)
    {
      info->rd_waiter_.notify();
    }

    static void notify_readers(conn_info_t *info)
    {
      notify_readers(
          info,
          std::integral_constant<bool,
                                 ipc::relat_trait<flag_t>::is_broadcast>{});
    }

    static void notify_writers(conn_info_t *info, std::true_type)
    {
      info->wt_waiter_.broadcast();
    }

    static void notify_writers(conn_info_t *info, std::false_type)
    {
      info->wt_waiter_.notify();
    }

    static void notify_writers(conn_info_t *info)
    {
      notify_writers(
          info,
          std::integral_constant<bool,
                                 ipc::relat_trait<flag_t>::is_multi_producer>{});
    }

    /* API implementations */

    static bool connect(ipc::handle_t *ph, ipc::prefix pref, char const *name,
                        bool start_to_recv)
    {
      assert(ph != nullptr);
      if (*ph == nullptr)
      {
        *ph = ipc::mem::alloc<conn_info_t>(pref.str, name);
      }
      return reconnect(ph, start_to_recv);
    }

    static bool connect(ipc::handle_t *ph, char const *name, bool start_to_recv)
    {
      return connect(ph, {nullptr}, name, start_to_recv);
    }

    static void disconnect(ipc::handle_t h)
    {
      auto que = queue_of(h);
      if (que == nullptr)
      {
        return;
      }
      que->shut_sending();
      assert(info_of(h) != nullptr);
      info_of(h)->disconnect_receiver();
    }

    static bool reconnect(ipc::handle_t *ph, bool start_to_recv)
    {
      assert(ph != nullptr);
      assert(*ph != nullptr);
      auto que = queue_of(*ph);
      if (que == nullptr)
      {
        return false;
      }
      info_of(*ph)->init();
      if (start_to_recv)
      {
        que->shut_sending();
        if (que->connect())
        { // wouldn't connect twice
          info_of(*ph)->cc_waiter_.broadcast();
          return true;
        }
        return false;
      }
      // start_to_recv == false
      if (que->connected())
      {
        info_of(*ph)->disconnect_receiver();
      }
      return que->ready_sending();
    }

    static void destroy(ipc::handle_t h) noexcept { ipc::mem::free(info_of(h)); }

    static std::size_t recv_count(ipc::handle_t h) noexcept
    {
      auto que = queue_of(h);
      if (que == nullptr)
      {
        return ipc::invalid_value;
      }
      return que->conn_count();
    }

    static std::uint32_t connected_id(ipc::handle_t h) noexcept
    {
      auto que = queue_of(h);
      if (que == nullptr)
      {
        return 0;
      }
      return static_cast<std::uint32_t>(que->connected_id());
    }

    static void disconnect_receivers(ipc::handle_t h,
                                     std::uint32_t cc_ids) noexcept
    {
      if (cc_ids == 0)
      {
        return;
      }
      auto que = queue_of(h);
      if (que == nullptr || que->elems() == nullptr)
      {
        return;
      }
      // Caller-supplied liveness: only bits of readers known to be gone.
      que->elems()->disconnect_receiver(static_cast<ipc::circ::cc_t>(cc_ids));
    }

    static bool wait_for_recv(ipc::handle_t h, std::size_t r_count,
                              std::uint64_t tm)
    {
      auto que = queue_of(h);
      if (que == nullptr)
      {
        return false;
      }
      return wait_for(
          info_of(h)->cc_waiter_,
          [que, r_count]
          { return que->conn_count() < r_count; },
          tm);
    }

    template <typename F>
    static bool send(F &&gen_push, ipc::handle_t h, void const *data,
                     std::size_t size, bool verbose)
    {
      if (data == nullptr || size == 0)
      {
        if (verbose)
          ipc::error("fail: send(%p, %zd)\n", data, size);
        return false;
      }
      auto que = queue_of(h);
      if (que == nullptr)
      {
        if (verbose)
          ipc::error("fail: send, queue_of(h) == nullptr\n");
        return false;
      }
      if (que->elems() == nullptr)
      {
        if (verbose)
          ipc::error("fail: send, queue_of(h)->elems() == nullptr\n");
        return false;
      }
      if (!que->ready_sending())
      {
        if (verbose)
          ipc::error("fail: send, que->ready_sending() == false\n");
        return false;
      }
      ipc::circ::cc_t conns =
          que->elems()->connections(std::memory_order_relaxed);
      if (conns == 0)
      {
        if (verbose)
          ipc::error("fail: send, there is no receiver on this connection.\n");
        return false;
      }
      // calc a new message id
      conn_info_t *inf = info_of(h);
      auto acc = inf->acc();
      if (acc == nullptr)
      {
        if (verbose)
          ipc::error("fail: send, info_of(h)->acc() == nullptr\n");
        return false;
      }
      auto msg_id = acc->fetch_add(1, std::memory_order_relaxed);
      auto try_push = std::forward<F>(gen_push)(inf, que, msg_id);
      if (size > ipc::large_msg_limit)
      {
        auto dat = acquire_storage(inf, size, conns);
        void *buf = dat.second;
        if (buf != nullptr)
        {
          std::memcpy(buf, data, size);
          return try_push(static_cast<std::int32_t>(size) -
                              static_cast<std::int32_t>(ipc::data_length),
                          &(dat.first), 0);
        }
        // try using message fragment
        // ipc::log("fail: shm::handle for big message. msg_id: %zd, size: %zd\n",
        // msg_id, size);
      }
      // push message fragment
      std::int32_t offset = 0;
      for (std::int32_t i = 0;
           i < static_cast<std::int32_t>(size / ipc::data_length);
           ++i, offset += ipc::data_length)
      {
        if (!try_push(static_cast<std::int32_t>(size) - offset -
                          static_cast<std::int32_t>(ipc::data_length),
                      static_cast<ipc::byte_t const *>(data) + offset,
                      ipc::data_length))
        {
          return false;
        }
      }
      // if remain > 0, this is the last message fragment
      std::int32_t remain = static_cast<std::int32_t>(size) - offset;
      if (remain > 0)
      {
        if (!try_push(remain - static_cast<std::int32_t>(ipc::data_length),
                      static_cast<ipc::byte_t const *>(data) + offset,
                      static_cast<std::size_t>(remain)))
        {
          return false;
        }
      }
      return true;
    }

    static bool send(ipc::handle_t h, void const *data, std::size_t size,
                     std::uint64_t tm, bool verbose)
    {
      return send(
          [tm](auto *info, auto *que, auto msg_id)
          {
            return [tm, info, que, msg_id](std::int32_t remain, void const *data,
                                           std::size_t size)
            {
              if (!wait_for(
                      info->wt_waiter_,
                      [&]
                      {
                        /* push 也会覆写被套圈的格子(见 prod_cons.h 里
                         * <single,multi,broadcast>::push 的注释), 所以它和 force_push
                         * 一样必须归还被丢弃消息的 chunk。旧实现这里是空回调, 于是每次
                         * 这种覆写都漏一块。 */
                        return !que->push(
                            [info](void *p, ipc::circ::cc_t rem_cc)
                            {
                              return clear_message<typename queue_t::value_t,
                                                   flag_t>(info, p, rem_cc);
                            },
                            info->cc_id_, msg_id, remain, data, size);
                      },
                      tm))
              {
                ipc::log("force_push: msg_id = %zd, remain = %d, size = %zd\n",
                         msg_id, remain, size);
                if (!que->force_push(
                        [info](void *p, ipc::circ::cc_t rem_cc)
                        {
                          return clear_message<typename queue_t::value_t,
                                               flag_t>(info, p, rem_cc);
                        },
                        info->cc_id_, msg_id, remain, data, size))
                {
                  return false;
                }
              }
              notify_readers(info);
              return true;
            };
          },
          h, data, size, verbose);
    }
    template <typename F>
    static bool no_member_send(F &&gen_push, ipc::handle_t h, void const *data,
                               std::size_t size, bool verbose)
    {
      if (data == nullptr || size == 0)
      {
        if (verbose)
          ipc::error("fail: send(%p, %zd)\n", data, size);
        return 0;
      }
      auto que = queue_of(h);
      if (que == nullptr)
      {
        if (verbose)
          ipc::error("fail: send, queue_of(h) == nullptr\n");
        return 0;
      }
      if (que->elems() == nullptr)
      {
        if (verbose)
          ipc::error("fail: send, queue_of(h)->elems() == nullptr\n");
        return 0;
      }
      if (!que->ready_sending())
      {
        if (verbose)
          ipc::error("fail: send, que->ready_sending() == false\n");
        return 0;
      }
      ipc::circ::cc_t conns =
          que->elems()->connections(std::memory_order_relaxed);
      const bool sniffer_only = conns == 0;
      if (sniffer_only && size > ipc::sniffer_payload_limit)
      {
        if (verbose)
          ipc::error("fail: send, sniffer-only payload is too large: size = %zd, limit = %zd\n",
                     size, static_cast<std::size_t>(ipc::sniffer_payload_limit));
        return 0;
      }
      // calc a new message id
      conn_info_t *inf = info_of(h);
      auto acc = inf->acc();
      if (acc == nullptr)
      {
        if (verbose)
          ipc::error("fail: send, info_of(h)->acc() == nullptr\n");
        return 0;
      }
      auto msg_id = acc->fetch_add(1, std::memory_order_relaxed);
      auto try_push = std::forward<F>(gen_push)(inf, que, msg_id);
      if (!sniffer_only && size > ipc::large_msg_limit)
      {
        auto dat = acquire_storage(inf, size, conns);
        void *buf = dat.second;
        if (buf != nullptr)
        {
          std::memcpy(buf, data, size);
          return try_push(static_cast<std::int32_t>(size) -
                              static_cast<std::int32_t>(ipc::data_length),
                          &(dat.first), 0);
        }
        // try using message fragment
        // ipc::log("fail: shm::handle for big message. msg_id: %zd, size: %zd\n",
        // msg_id, size);
      }
      // push message fragment
      std::int32_t offset = 0;
      for (std::int32_t i = 0;
           i < static_cast<std::int32_t>(size / ipc::data_length);
           ++i, offset += ipc::data_length)
      {
        if (!try_push(static_cast<std::int32_t>(size) - offset -
                          static_cast<std::int32_t>(ipc::data_length),
                      static_cast<ipc::byte_t const *>(data) + offset,
                      ipc::data_length))
        {
          return 0;
        }
      }
      // if remain > 0, this is the last message fragment
      std::int32_t remain = static_cast<std::int32_t>(size) - offset;
      if (remain > 0)
      {
        if (!try_push(remain - static_cast<std::int32_t>(ipc::data_length),
                      static_cast<ipc::byte_t const *>(data) + offset,
                      static_cast<std::size_t>(remain)))
        {
          return 0;
        }
      }
      return 1;
    }

    static bool no_member_try_send(ipc::handle_t h, void const *data, std::size_t size,
                                   std::uint64_t tm, bool verbose)
    {
      return no_member_send(
          [tm, verbose](auto *info, auto *que, auto msg_id)
          {
            const bool sniffer_only = que->elems()->connections(std::memory_order_relaxed) == 0;
            return [tm, info, que, msg_id, verbose, sniffer_only](std::int32_t remain, void const *data,
                                           std::size_t size)
            {
              if (sniffer_only)
              {
                /* sniffer 环从不承载 chunk(no_member_send 在 sniffer_only 下不走
                 * acquire_storage), 所以这里无需归还; 但回调签名要与 push 统一。 */
                if (!que->push_sniffer(
                        [](void *, ipc::circ::cc_t)
                        { return true; },
                        info->cc_id_, msg_id, remain, data, size))
                {
                  return false;
                }
                notify_readers(info);
                return true;
              }
              if (!wait_for(
                      info->wt_waiter_,
                      [&]
                      {
                        /* push 也会覆写被套圈的格子(见 prod_cons.h 里
                         * <single,multi,broadcast>::push 的注释), 所以它和 force_push
                         * 一样必须归还被丢弃消息的 chunk。旧实现这里是空回调, 于是每次
                         * 这种覆写都漏一块。 */
                        return !que->push(
                            [info](void *p, ipc::circ::cc_t rem_cc)
                            {
                              return clear_message<typename queue_t::value_t,
                                                   flag_t>(info, p, rem_cc);
                            },
                            info->cc_id_, msg_id, remain, data, size);
                      },
                      tm))
              {
                // push() timed out — fall back to force_push(), which bumps the
                // epoch and overwrites the oldest slot instead of waiting.
                // Slow readers lose the lapped messages but stay connected;
                // force_push no longer disconnects them (see prod_cons.h).
                // This prevents a single slow reader from permanently
                // blocking the publisher (mirrors send() behavior).
                if (verbose)
                  ipc::log("no_member_try_send force_push: msg_id = %zd, "
                           "remain = %d, size = %zd\n",
                           msg_id, remain, size);
                if (!que->force_push(
                        [info](void *p, ipc::circ::cc_t rem_cc)
                        {
                          return clear_message<typename queue_t::value_t,
                                               flag_t>(info, p, rem_cc);
                        },
                        info->cc_id_, msg_id, remain, data, size))
                {
                  return false;
                }
              }
              notify_readers(info);
              return true;
            };
          },
          h, data, size, verbose);
    }

    static bool try_send(ipc::handle_t h, void const *data, std::size_t size,
                         std::uint64_t tm, bool verbose)
    {
      return send(
          [tm](auto *info, auto *que, auto msg_id)
          {
            return [tm, info, que, msg_id](std::int32_t remain, void const *data,
                                           std::size_t size)
            {
              if (!wait_for(
                      info->wt_waiter_,
                      [&]
                      {
                        /* push 也会覆写被套圈的格子(见 prod_cons.h 里
                         * <single,multi,broadcast>::push 的注释), 所以它和 force_push
                         * 一样必须归还被丢弃消息的 chunk。旧实现这里是空回调, 于是每次
                         * 这种覆写都漏一块。 */
                        return !que->push(
                            [info](void *p, ipc::circ::cc_t rem_cc)
                            {
                              return clear_message<typename queue_t::value_t,
                                                   flag_t>(info, p, rem_cc);
                            },
                            info->cc_id_, msg_id, remain, data, size);
                      },
                      tm))
              {
                return false;
              }
              notify_readers(info);
              return true;
            };
          },
          h, data, size, verbose);
    }

    static ipc::buff_t recv(ipc::handle_t h, std::uint64_t tm, bool verbose)
    {
      auto que = queue_of(h);
      if (que == nullptr)
      {
        ipc::error("fail: recv, queue_of(h) == nullptr\n");
        return {};
      }
      if (!que->connected())
      {
        // hasn't connected yet, just return.
        return {};
      }
      conn_info_t *inf = info_of(h);
      auto &rc = inf->recv_cache();
      for (;;)
      {
        // pop a new message
        typename queue_t::value_t msg{};
        bool writable = false;
        if (!wait_for(
                inf->rd_waiter_,
                [que, &msg, &h, &writable]
                {
                  if (!que->connected())
                  {
                    reconnect(&h, true);
                  }
                  writable = false;
                  return !que->pop(msg, [&writable](bool out)
                                   { writable = out; });
                },
                tm))
        {
          // pop failed, just return.
          return {};
        }
        if (writable)
        {
          notify_writers(inf);
        }
        if ((inf->acc() != nullptr) && (msg.cc_id_ == inf->cc_id_))
        {
          continue; // ignore message to self
        }
        // msg.remain_ may minus & abs(msg.remain_) < data_length
        std::int32_t r_size =
            static_cast<std::int32_t>(ipc::data_length) + msg.remain_;
        if (r_size <= 0)
        {
          if (verbose)
            ipc::error("fail: recv, r_size = %d\n", (int)r_size);
          return {};
        }
        std::size_t msg_size = static_cast<std::size_t>(r_size);
        // large message
        if (msg.storage_)
        {
          ipc::storage_id_t buf_id =
              *reinterpret_cast<ipc::storage_id_t *>(&msg.data_);
          void *buf = find_storage(buf_id, inf, msg_size);
          if (buf != nullptr)
          {
            struct recycle_t
            {
              ipc::storage_id_t storage_id;
              conn_info_t *inf;
              ipc::circ::cc_t curr_conns;
              ipc::circ::cc_t conn_id;
            } *r_info = ipc::mem::alloc<recycle_t>(recycle_t{
                buf_id, inf, que->elems()->connections(std::memory_order_relaxed),
                que->connected_id()});
            if (r_info == nullptr)
            {
              ipc::log("fail: ipc::mem::alloc<recycle_t>.\n");
              return ipc::buff_t{buf, msg_size}; // no recycle
            }
            else
            {
              return ipc::buff_t{
                  buf, msg_size,
                  [](void *p_info, std::size_t size)
                  {
                    auto r_info = static_cast<recycle_t *>(p_info);
                    IPC_UNUSED_ auto finally =
                        ipc::guard([r_info]
                                   { ipc::mem::free(r_info); });
                    recycle_storage<flag_t>(r_info->storage_id, r_info->inf, size,
                                            r_info->curr_conns, r_info->conn_id);
                  },
                  r_info};
            }
          }
          else
          {
            ipc::log(
                "fail: shm::handle for large message. msg_id: %zd, buf_id: %zd, "
                "size: %zd\n",
                msg.id_, buf_id, msg_size);
            continue;
          }
        }
        // find cache with msg.id_
        auto cac_it = rc.find(msg.id_);
        if (cac_it == rc.end())
        {
          if (msg_size <= ipc::data_length)
          {
            return make_cache(msg.data_, msg_size);
          }
          // gc
          if (rc.size() > 1024)
          {
            std::vector<msg_id_t> need_del;
            for (auto const &pair : rc)
            {
              auto cmp = std::minmax(msg.id_, pair.first);
              if (cmp.second - cmp.first > 8192)
              {
                need_del.push_back(pair.first);
              }
            }
            for (auto id : need_del)
              rc.erase(id);
          }
          // cache the first message fragment
          rc.emplace(msg.id_,
                     cache_t{ipc::data_length, make_cache(msg.data_, msg_size)});
        }
        // has cached before this message
        else
        {
          auto &cac = cac_it->second;
          // this is the last message fragment
          if (msg.remain_ <= 0)
          {
            cac.append(&(msg.data_), msg_size);
            // finish this message, erase it from cache
            auto buff = std::move(cac.buff_);
            rc.erase(cac_it);
            return buff;
          }
          // there are remain datas after this message
          cac.append(&(msg.data_), ipc::data_length);
        }
      }
    }

    static ipc::buff_t try_recv(ipc::handle_t h, bool verbose)
    {
      return recv(h, 0, verbose);
    }

    /* ---------------------------------------------------------------- 借样 */

    static ipc::loan_t loan(ipc::handle_t h, std::size_t size, bool verbose)
    {
      auto que = queue_of(h);
      if (que == nullptr || que->elems() == nullptr)
      {
        if (verbose)
          ipc::error("fail: loan, invalid queue\n");
        return {};
      }
      if (!que->ready_sending())
      {
        if (verbose)
          ipc::error("fail: loan, que->ready_sending() == false\n");
        return {};
      }
      /* 无接收方时不存在 chunk 语义: send() 在这种情况下也不走 chunk(改用 sniffer
       * 环或分片), 强行借了没人回收。让调用方回退。 */
      ipc::circ::cc_t conns =
          que->elems()->connections(std::memory_order_relaxed);
      if (conns == 0)
      {
        if (verbose)
          ipc::error("fail: loan, there is no receiver on this connection.\n");
        return {};
      }
      /* 接收侧靠 msg.storage_ 判定大消息, 而 storage_ 只在 size > large_msg_limit
       * 的分支被设置。借样必须落在那条路径上。 */
      if (size <= ipc::large_msg_limit)
      {
        size = ipc::large_msg_limit + 1;
      }
      const std::size_t cap = loan_size_class(size);
      conn_info_t *inf = info_of(h);
      auto dat = acquire_storage(inf, cap, conns);
      if (dat.second == nullptr)
      {
        /* chunk 池耗尽(每档位 32 块)。不是错误, 是背压信号 —— 调用方回退整包路径。 */
        return {};
      }
      ipc::loan_t lo;
      lo.id = dat.first;
      lo.data = dat.second;
      lo.size = cap;
      return lo;
    }

    static bool publish_loan(ipc::handle_t h, ipc::loan_t const &lo,
                             std::uint64_t tm, bool verbose)
    {
      if (!lo.valid())
        return false;
      auto que = queue_of(h);
      conn_info_t *inf = info_of(h);
      if (que == nullptr || inf == nullptr || que->elems() == nullptr)
      {
        if (verbose)
          ipc::error("fail: publish_loan, invalid queue\n");
        discard_loan(h, lo);
        return false;
      }
      auto acc = inf->acc();
      if (acc == nullptr)
      {
        if (verbose)
          ipc::error("fail: publish_loan, info_of(h)->acc() == nullptr\n");
        discard_loan(h, lo);
        return false;
      }
      auto msg_id = acc->fetch_add(1, std::memory_order_relaxed);
      /* 与 send() 的大消息分支同构: 槽位里写的是 chunk id(整数), 不是数据。
       * 传 size = 0 让 msg_t 置 storage_ = true 并拷贝 id;
       * remain 编码的是**借到的容量**, 因为接收侧要用它反推 chunk_size 才能定位
       * 共享段(见 recv 的 find_storage(buf_id, inf, msg_size))。真实负载长度由负载
       * 自身的头部承载, 不走这里。 */
      const std::int32_t remain = static_cast<std::int32_t>(lo.size) -
                                  static_cast<std::int32_t>(ipc::data_length);
      auto id = lo.id;
      bool pushed = wait_for(
          inf->wt_waiter_,
          [&]
          {
            /* 同上: push 覆写被套圈的格子时也要归还被丢弃消息的 chunk。 */
            return !que->push(
                [inf](void *p, ipc::circ::cc_t rem_cc)
                {
                  return clear_message<typename queue_t::value_t, flag_t>(inf, p,
                                                                          rem_cc);
                },
                inf->cc_id_, msg_id, remain, &id, 0);
          },
          tm);
      if (!pushed)
      {
        if (verbose)
          ipc::log("publish_loan force_push: msg_id = %zd, cap = %zd\n", msg_id,
                   lo.size);
        pushed = que->force_push(
            [inf](void *p, ipc::circ::cc_t rem_cc)
            {
              return clear_message<typename queue_t::value_t, flag_t>(inf, p,
                                                                      rem_cc);
            },
            inf->cc_id_, msg_id, remain, &id, 0);
      }
      if (!pushed)
      {
        /* 没能进队列 = 没有任何接收方会回收它, 必须自己还回去。 */
        if (verbose)
          ipc::error("fail: publish_loan, push failed; chunk returned\n");
        discard_loan(h, lo);
        return false;
      }
      notify_readers(inf);
      return true;
    }

    static void discard_loan(ipc::handle_t h, ipc::loan_t const &lo)
    {
      if (!lo.valid())
        return;
      conn_info_t *inf = info_of(h);
      if (inf == nullptr)
        return;
      /* 尚未投递 ⇒ 没有任何接收方持有它 ⇒ 无条件归还是安全的(与被覆写消息的
       * discard_storage 不同, 那里必须先按 conns 位图判断)。 */
      release_storage(lo.id, inf, lo.size);
    }

  }; // detail_impl<Policy>

  template <typename Flag>
  using policy_t = ipc::policy::choose<ipc::circ::elem_array, Flag>;

} // namespace

namespace ipc
{

  template <typename Flag>
  ipc::handle_t chan_impl<Flag>::init_first()
  {
    ipc::detail::waiter::init();
    return nullptr;
  }

  template <typename Flag>
  bool chan_impl<Flag>::connect(ipc::handle_t *ph, char const *name,
                                unsigned mode)
  {
    return detail_impl<policy_t<Flag>>::connect(ph, name, mode & receiver);
  }

  template <typename Flag>
  bool chan_impl<Flag>::connect(ipc::handle_t *ph, prefix pref, char const *name,
                                unsigned mode)
  {
    return detail_impl<policy_t<Flag>>::connect(ph, pref, name, mode & receiver);
  }

  template <typename Flag>
  bool chan_impl<Flag>::reconnect(ipc::handle_t *ph, unsigned mode)
  {
    return detail_impl<policy_t<Flag>>::reconnect(ph, mode & receiver);
  }

  template <typename Flag>
  void chan_impl<Flag>::disconnect(ipc::handle_t h)
  {
    detail_impl<policy_t<Flag>>::disconnect(h);
  }

  template <typename Flag>
  void chan_impl<Flag>::destroy(ipc::handle_t h)
  {
    disconnect(h);
    detail_impl<policy_t<Flag>>::destroy(h);
  }

  template <typename Flag>
  void chan_impl<Flag>::release(ipc::handle_t h) noexcept
  {
    detail_impl<policy_t<Flag>>::destroy(h);
  }

  template <typename Flag>
  char const *chan_impl<Flag>::name(ipc::handle_t h)
  {
    auto *info = detail_impl<policy_t<Flag>>::info_of(h);
    return (info == nullptr) ? nullptr : info->name_.c_str();
  }

  template <typename Flag>
  void chan_impl<Flag>::clear(ipc::handle_t h) noexcept
  {
    disconnect(h);
    using conn_info_t = typename detail_impl<policy_t<Flag>>::conn_info_t;
    auto conn_info_p = static_cast<conn_info_t *>(h);
    if (conn_info_p == nullptr)
      return;
    conn_info_p->clear();
    destroy(h);
  }

  template <typename Flag>
  void chan_impl<Flag>::clear_storage(char const *name) noexcept
  {
    chan_impl<Flag>::clear_storage({nullptr}, name);
  }

  template <typename Flag>
  void chan_impl<Flag>::clear_storage(prefix pref, char const *name) noexcept
  {
    using conn_info_t = typename detail_impl<policy_t<Flag>>::conn_info_t;
    conn_info_t::clear_storage(pref.str, name);
  }

  template <typename Flag>
  std::size_t chan_impl<Flag>::recv_count(ipc::handle_t h)
  {
    return detail_impl<policy_t<Flag>>::recv_count(h);
  }

  template <typename Flag>
  std::uint32_t chan_impl<Flag>::connected_id(ipc::handle_t h)
  {
    return detail_impl<policy_t<Flag>>::connected_id(h);
  }

  template <typename Flag>
  void chan_impl<Flag>::disconnect_receivers(ipc::handle_t h,
                                             std::uint32_t cc_ids)
  {
    detail_impl<policy_t<Flag>>::disconnect_receivers(h, cc_ids);
  }

  template <typename Flag>
  bool chan_impl<Flag>::wait_for_recv(ipc::handle_t h, std::size_t r_count,
                                      std::uint64_t tm)
  {
    return detail_impl<policy_t<Flag>>::wait_for_recv(h, r_count, tm);
  }

  template <typename Flag>
  bool chan_impl<Flag>::send(ipc::handle_t h, void const *data, std::size_t size,
                             std::uint64_t tm, bool verbose)
  {
    return detail_impl<policy_t<Flag>>::send(h, data, size, tm, verbose);
  }

  template <typename Flag>
  buff_t chan_impl<Flag>::recv(ipc::handle_t h, std::uint64_t tm, bool verbose)
  {
    return detail_impl<policy_t<Flag>>::recv(h, tm, verbose);
  }

  template <typename Flag>
  bool chan_impl<Flag>::no_member_try_send(ipc::handle_t h, void const *data, std::size_t size,
                                           std::uint64_t tm, bool verbose)
  {
    return detail_impl<policy_t<Flag>>::no_member_try_send(h, data, size, tm, verbose);
  }

  template <typename Flag>
  bool chan_impl<Flag>::try_send(ipc::handle_t h, void const *data,
                                 std::size_t size, std::uint64_t tm,
                                 bool verbose)
  {
    return detail_impl<policy_t<Flag>>::try_send(h, data, size, tm, verbose);
  }

  template <typename Flag>
  buff_t chan_impl<Flag>::try_recv(ipc::handle_t h, bool verbose)
  {
    return detail_impl<policy_t<Flag>>::try_recv(h, verbose);
  }

  template <typename Flag>
  ipc::loan_t chan_impl<Flag>::loan(ipc::handle_t h, std::size_t size,
                                    bool verbose)
  {
    return detail_impl<policy_t<Flag>>::loan(h, size, verbose);
  }

  template <typename Flag>
  bool chan_impl<Flag>::publish_loan(ipc::handle_t h, ipc::loan_t const &lo,
                                     std::uint64_t tm, bool verbose)
  {
    return detail_impl<policy_t<Flag>>::publish_loan(h, lo, tm, verbose);
  }

  template <typename Flag>
  void chan_impl<Flag>::discard_loan(ipc::handle_t h, ipc::loan_t const &lo)
  {
    detail_impl<policy_t<Flag>>::discard_loan(h, lo);
  }

  template struct chan_impl<
      ipc::wr<relat::single, relat::single, trans::unicast>>;
  // template struct chan_impl<ipc::wr<relat::single, relat::multi ,
  // trans::unicast  >>; // TBD template struct chan_impl<ipc::wr<relat::multi ,
  // relat::multi , trans::unicast  >>; // TBD
  template struct chan_impl<
      ipc::wr<relat::single, relat::multi, trans::broadcast>>;
  template struct chan_impl<
      ipc::wr<relat::multi, relat::multi, trans::broadcast>>;

} // namespace ipc
