
#include <type_traits>
#include <cstring>
#include <limits>
#include <dirent.h>   /* W09/D-14: 陈旧池段探活(扫 /proc/<pid>/maps) */
#include <cstdio>     /* std::snprintf / std::fopen */
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <utility> // std::pair, std::move, std::forward
#include <atomic>
#include <type_traits> // aligned_storage_t
#include <string>
#include <vector>
#include <array>
#include <cassert>
#include <mutex>
#include <sys/stat.h>

/* F1/t35：把池穷尽这个**产品侧事实**接到 W03 的计数注册表（只引用既有 ID）。
 * ⛔ 只读引用，不改 counters.h；counters.h 是 header-only，其单例是 inline 函数局部
 * static ⇒ 与 libipc.so 内既有写入点**同一份**（实测见 evidence 的 odr_unification_probe）。 */
#include "dzIPC/measure/counters.h"
#include "libipc/ipc.h"
#include "libipc/detail/publish_trace.h"
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

namespace ipc::detail {
namespace { std::atomic<PublishHook> publish_hook{nullptr}; }
void set_publish_hook(PublishHook hook) noexcept { publish_hook.store(hook, std::memory_order_relaxed); }
PublishHook get_publish_hook() noexcept { return publish_hook.load(std::memory_order_relaxed); }
struct loan_lifetime {
    std::mutex mutex;
    std::shared_ptr<ipc::topic_pool_context> pool;
    std::function<void()> give_back;
    bool finished = false;
    ~loan_lifetime() { if (give_back) give_back(); }
};
}

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

  /* 分配失败只报一次 —— 真的 OOM 时每个分片都会失败, 逐条打印会把日志淹掉,
   * 而这里要的是"看得见"而不是"看全"(消息本身已经按丢弃处理)。 */
  void report_cache_alloc_failure(std::size_t size)
  {
    static std::atomic<bool> reported{false};
    if (reported.exchange(true))
    {
      return;
    }
    ipc::error(
        "fail: make_cache, ipc::mem::alloc(%zu) returned nullptr. "
        "分片重组的缓冲区拿不到 —— 该条消息按丢包处理(不会再崩在写地址 0 上)。\n",
        size);
  }

  template <typename T>
  ipc::buff_t make_cache(T &data, std::size_t size)
  {
    auto ptr = ipc::mem::alloc(size);
    /* 分配失败 ⇒ 返回空 buff_t: 与 recv() 其余失败出口一致, 调用方按"没收到"
     * 丢弃该消息(见 docs/shm_defect_fixes.md 第 7 条)。 */
    if (ptr == nullptr)
    {
      report_cache_alloc_failure(size);
      return {};
    }
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
      /* buff_.data() 必须一起判: ipc::buffer 允许"size > 0 而 data() == nullptr"
       * (见 buffer::empty() 的判据), 而 make_cache 在分配失败时会给回一个空 buff_t。
       * 只看 size 的话, 这里就是又一个往地址 0 写的入口。 */
      if (buff_.data() == nullptr || fill_ >= buff_.size() || data == nullptr || size == 0)
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
    ipc::string topic_prefix_;
    std::shared_ptr<ipc::topic_pool_context> topic_pool_;
    msg_id_t cc_id_; // connection-info id
    ipc::detail::waiter cc_waiter_, wt_waiter_, rd_waiter_;
    ipc::shm::handle acc_h_;

    /* UF-003: 本路由的 owner 表(指向 elems 尾部内嵌表, 队列打开后有效)与
     * 路由标签(conn 段名的散列)。两者都是进程本地缓存, 不随段共享。 */
    ipc::circ::owner_table *owners_ = nullptr;
    std::uint32_t route_tag_ = 0;

    conn_info_head(char const *prefix, char const *name)
        : prefix_{ipc::make_string(prefix)},
          name_{ipc::make_string(name)},
          topic_prefix_{ipc::topic_pool_prefix(prefix_, name_)},
          topic_pool_{ipc::acquire_topic_pool(prefix_, name_)},
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

    // 缓存属于连接；不同话题的消息序号可以相同，不能共用线程级序号表。
    // disconnect 可以从控制线程调用，清理与收包的分片访问必须互斥。
    std::mutex recv_cache_mutex_;
    ipc::unordered_map<msg_id_t, cache_t> recv_cache_;
    auto &recv_cache() { return recv_cache_; }
  };

  IPC_CONSTEXPR_ std::size_t align_chunk_size(std::size_t size) noexcept
  {
    return (((size - 1) / ipc::large_msg_align) + 1) * ipc::large_msg_align;
  }

  std::size_t calc_chunk_size(std::size_t size) noexcept
  {
    size = ipc::pool_size_class(size);
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

    /* UF-003: 借出后是否已发布(=1)。落在 conns 与 data 之间的对齐空洞里
     * (偏移 4), 不改 chunk_size 与负载偏移契约(见下方静态断言)。 */
    std::atomic<std::uint8_t> &published() noexcept
    {
      return *reinterpret_cast<std::atomic<std::uint8_t> *>(
          reinterpret_cast<ipc::byte_t *>(this) +
          sizeof(std::atomic<ipc::circ::cc_t>));
    }

    /* UF-003: 借出者路由标签(0 = 未标记), 同样落在空洞内(偏移 8)。话题池里
     * 位号只在同路由 owner 表里有意义, 清扫方据此只处置本路由的块。 */
    std::atomic<std::uint32_t> &route_tag() noexcept
    {
      return *reinterpret_cast<std::atomic<std::uint32_t> *>(
          reinterpret_cast<ipc::byte_t *>(this) +
          sizeof(std::atomic<ipc::circ::cc_t>) + 4);
    }

    void *data() noexcept
    {
      return reinterpret_cast<ipc::byte_t *>(this) +
             ipc::make_align(alignof(std::max_align_t),
                             sizeof(std::atomic<ipc::circ::cc_t>));
    }
  };

  /* UF-003: published/route_tag 必须放得进 conns 之后的头部空洞, 否则下一个
   * 字段就会踩到负载区。头部长度 = make_align(alignof(max_align_t), sizeof(cc_t))。 */
  static_assert(sizeof(std::atomic<ipc::circ::cc_t>) + 4 +
                        sizeof(std::uint32_t) <=
                    ipc::make_align(alignof(std::max_align_t),
                                    sizeof(std::atomic<ipc::circ::cc_t>)),
                "UF-003: chunk header padding too small for published/route_tag");

  struct alignas(8) chunk_info_t
  {
    ipc::pool_identity_header identity_;
    ipc::id_pool<> pool_;
    ipc::spin_lock lock_;

    IPC_CONSTEXPR_ static std::size_t chunks_mem_size(
        std::size_t chunk_size, std::size_t count = ipc::topic_msg_cache) noexcept
    {
      return count * chunk_size;
    }

    ipc::byte_t *chunks_mem() noexcept
    {
      return reinterpret_cast<ipc::byte_t *>(this + 1);
    }

    chunk_t *at(std::size_t chunk_size, ipc::storage_id_t id,
                std::size_t count = ipc::topic_msg_cache) noexcept
    {
      if (id < 0 || static_cast<std::size_t>(id) >= count)
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

  chunk_info_t* chunk_storage_info(std::shared_ptr<ipc::topic_pool_context> const& pool,
                                    std::size_t chunk_size,
                                    std::size_t count = ipc::topic_msg_cache) {
    if (!pool || count != ipc::topic_msg_cache) return nullptr;
    return static_cast<chunk_info_t*>(pool->map_pool(chunk_size,
        sizeof(chunk_info_t) + chunk_info_t::chunks_mem_size(chunk_size)));
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
  std::size_t loan_size_class(std::size_t size) noexcept {
    return ipc::pool_size_class(size);
  }

  // 池满是背压：每话题每尺寸档固定十块，计数按进程聚合并节流。
  void note_pool_exhausted(char const *kind, std::size_t chunk_size,
                           std::size_t size, ipc::string const &prefix, std::size_t capacity = ipc::topic_msg_cache) {
    struct key_t {
      std::size_t chunk_size;
      char const *kind;
      std::size_t capacity;
    };
    struct stat_t {
      key_t key;
      std::uint64_t count;
    };
    static std::mutex lock;
    static std::vector<stat_t> stats;

    std::uint64_t n;
    {
      std::lock_guard<std::mutex> guard{lock};
      auto it = std::find_if(stats.begin(), stats.end(),
                             [&](stat_t const &s)
                             {
                               return (s.key.chunk_size == chunk_size) &&
                                      (std::strcmp(s.key.kind, kind) == 0) && s.key.capacity == capacity;
                             });
      if (it == stats.end()) {
        stats.push_back(stat_t{key_t{chunk_size, kind, capacity}, 0});
        it = stats.end() - 1;
      }
      n = ++(it->count);
    }

    /* ---- F1/t35：W03 计数接线（**本函数是全仓唯一的池穷尽出口**，故也是这两个
     * ID 的唯一写入点）。
     *
     * kind → ID 的映射沿用 W09-F5 冻结口径（docs/.../W09/容量与背压_交付.md:184）：
     *   `loan`                        ⇒ chunk_alloc_failed（B 借样被拒；调用方**必须**回退整包）
     *   `send` / `no_member_send`     ⇒ chunk_exhausted（A/TLV 降级为 64 B 分片，**仍交付**）
     * 两者是**不同**的失败类别（W09 §5.1：一个未交付、一个降级交付），⛔ 不得混算成一个。
     *
     * ⛔ 这两个 ID **不属于** t19 的三语义分离中的任何一组：`fallback_*` 是"DZFlat 尝试过
     *    但降级"，`borrow_failed_*` 是"应用 loan() 失败"。池穷尽是**传输层容量事实**，
     *    故只写 capacity 组，⛔ 不碰 fallback_total / borrow_failed_*（与本函数既有日志
     *    同一口径：日志里也从不说"回退"）。
     *
     * 位置：与首报/节流**同一函数**、在 `n` 已知之后 —— 记账与诊断共用同一次 (kind,
     * chunk_size) 归并，不可能出现"报了日志但没计数"或反之。 */
    if (std::strcmp(kind, "loan") == 0) {
      ::dzIPC::measure::CounterRegistry::instance().inc(
          ::dzIPC::measure::CounterId::chunk_alloc_failed);
    } else {
      ::dzIPC::measure::CounterRegistry::instance().inc(
          ::dzIPC::measure::CounterId::chunk_exhausted);
    }

    if ((n == 1) || ((n % 1024) == 0)) {
      ipc::error("chunk pool exhausted: kind = %s, chunk_size = %zu, size = %zu, "
                 "pool capacity = %zu, count = %llu (本进程), prefix = '%s'\n",
                 kind, chunk_size, size,
                 capacity,
                 static_cast<unsigned long long>(n), prefix.c_str());
    }
  }

  void note_reclaimed(char const *kind, std::size_t chunk_size,
                      std::size_t count, ipc::string const &prefix);

  /* UF-003: 池穷尽时的"死持有者清扫"。
   *
   * 回收一块 chunk 需要三个条件同时成立:
   *   ① 该块是**本路由**借出的(chunk->route_tag() == 本路由标签)—— 位号只在
   *      同路由 owner 表里有意义, 跨路由解释位图是错的;
   *   ② 该块**已发布**(published != 0)—— 借出但未发布的块由发布者自己处置
   *      (见 send 的 push 失败归还路径), 清扫方让路;
   *   ③ 位图里每一位的 owner 都"确定已死"—— 有活/不确定持有者就整块放弃。
   *
   * 动作: CAS 位图 → 0, 成功后持锁归还原 id。这与 discard_storage / recycle_storage
   * 共享同一条不变量: **位图归零是还池的唯一授权**, 于是并发双方只有一方能还。
   * 只收 broadcast: 单播的 cc 是计数语义, 位号无意义。 */
  bool reclaim_dead_chunks(conn_info_head *inf, chunk_info_t *info,
                           std::size_t chunk_size, bool bitmap_semantics,
                           char const *kind, ipc::string const& pref, std::size_t count) {
    if (inf == nullptr || info == nullptr) return false;
    if (!bitmap_semantics) return false;
    if (inf->owners_ == nullptr || inf->route_tag_ == 0) return false;

    /* 先对本路由的全部位做一次薄判(每块都探会让 /proc 读放大到池容量倍). */
    ipc::circ::cc_t const all =
        static_cast<ipc::circ::cc_t>(~static_cast<ipc::circ::cc_t>(0u));
    ipc::circ::cc_t const dead = inf->owners_->proven_dead_bits(all);
    if (dead == 0) return false;

    std::size_t reclaimed = 0;
    for (ipc::storage_id_t id = 0; id < static_cast<ipc::storage_id_t>(count); ++id) {
      auto *chunk = info->at(chunk_size, id, count);
      if (chunk == nullptr) continue;
      auto cur = chunk->conns().load(std::memory_order_acquire);
      if (cur == 0) continue; // 在池中或从未借出
      if (chunk->route_tag().load(std::memory_order_acquire) != inf->route_tag_)
        continue;
      if (chunk->published().load(std::memory_order_acquire) == 0) continue;
      if ((cur & ~dead) != 0) continue; // 有活/不确定持有者 ⇒ 不夺
      auto expect = cur;
      if (!chunk->conns().compare_exchange_strong(expect, 0,
                                                  std::memory_order_acq_rel))
        continue; // 有人并发动了位图: 让给它
      info->lock_.lock();
      info->pool_.release(id);
      info->lock_.unlock();
      ++reclaimed;
    }
    if (reclaimed != 0) note_reclaimed(kind, chunk_size, reclaimed, pref);
    return reclaimed != 0;
  }

  /* UF-003: 清扫归还计数(与 note_pool_exhausted 同款: 本进程计数 + 首报/节流). */
  void note_reclaimed(char const *kind, std::size_t chunk_size,
                      std::size_t count, ipc::string const &prefix) {
    struct key_t {
      std::size_t chunk_size;
      char const *kind;
    };
    struct stat_t {
      key_t key;
      std::uint64_t count;
    };
    static std::mutex lock;
    static std::vector<stat_t> stats;

    std::uint64_t n;
    {
      std::lock_guard<std::mutex> guard{lock};
      auto it = std::find_if(stats.begin(), stats.end(),
                             [&](stat_t const &s)
                             {
                               return (s.key.chunk_size == chunk_size) &&
                                      (std::strcmp(s.key.kind, kind) == 0);
                             });
      if (it == stats.end()) {
        stats.push_back(stat_t{key_t{chunk_size, kind}, 0});
        it = stats.end() - 1;
      }
      n = ++(it->count);
    }
    if ((n == 1) || ((n % 1024) == 0)) {
      ipc::log("chunk pool reclaim: kind = %s, chunk_size = %zu, reclaimed = %zu, "
               "count = %llu (本进程), prefix = '%s'\n",
               kind, chunk_size, count, (unsigned long long)n, prefix.c_str());
    }
  }

  /* UF-003: 发布成功"接管"标记 —— 置位后清扫方才有权对该块做验尸回收。 */
  void mark_published(conn_info_head *inf, std::size_t size,
                      ipc::storage_id_t id) {
    if (inf == nullptr || !ipc::detail::valid_storage(id)) return;
    std::size_t const chunk_size = calc_chunk_size(size);
    constexpr auto count = ipc::topic_msg_cache;
    auto info = chunk_storage_info(inf->topic_pool_, chunk_size, count);
    if (info == nullptr) return;
    auto *chunk = info->at(chunk_size, id, count);
    if (chunk == nullptr) return;
    chunk->published().store(1, std::memory_order_release);
  }

  /* UF-003: 未发布的借出块归还。push 失败 ⇒ 消息没进队列 ⇒ 没有任何接收方
   * 可能持有它 ⇒ 直接归零位图并还池(与 discard_storage 不同, 那里必须先按
   * 位图判断持有者)。这是既有漏点的顺带收口: 旧实现 push 失败即永久漏一块。 */
  void return_unpublished(conn_info_head *inf, std::size_t size,
                          ipc::storage_id_t id) {
    if (inf == nullptr || !ipc::detail::valid_storage(id)) return;
    std::size_t const chunk_size = calc_chunk_size(size);
    constexpr auto count = ipc::topic_msg_cache;
    auto info = chunk_storage_info(inf->topic_pool_, chunk_size, count);
    if (info == nullptr) return;
    auto *chunk = info->at(chunk_size, id, count);
    if (chunk == nullptr) return;
    chunk->published().store(0, std::memory_order_relaxed);
    chunk->conns().store(0, std::memory_order_release);
    info->lock_.lock();
    info->pool_.release(id);
    info->lock_.unlock();
  }

  /* t46：把"为什么没借到"逐出口带出去（`why` 可为空 ⇒ 与旧行为逐位相同）。
   * ⛔ 本函数在**匿名 namespace** 内（`ipc.cpp:41` 起），不是 libipc 的导出面
   *    ⇒ 加参数不改任何符号/布局（`nm -D` 对照见交付 §5）。 */
  std::pair<ipc::storage_id_t, void *> acquire_storage(conn_info_head *inf,
                                                       std::size_t size,
                                                       ipc::circ::cc_t conns,
                                                       char const *kind,
                                                       bool bitmap_semantics,
                                                       ipc::loan_status *why = nullptr)  {
    const auto fail = [why](ipc::loan_status w) -> std::pair<ipc::storage_id_t, void *> {
      if (why != nullptr)
        *why = w;
      return {};
    };
    if (inf == nullptr)
      return fail(ipc::loan_status::invalid_handle);
    std::size_t chunk_size = calc_chunk_size(size);
    constexpr auto count = ipc::topic_msg_cache;
    auto const& pref = inf->topic_prefix_;
    auto info = chunk_storage_info(inf->topic_pool_, chunk_size, count);
    if (info == nullptr)
      /* 段建不出来（mmap 失败 / 名字被占 / 尺寸算术越界）—— **不是**背压。 */
      return fail(ipc::loan_status::storage_unavailable);

    info->lock_.lock();
    info->pool_.prepare();
    // got an unique id
    auto id = info->pool_.acquire();
    info->lock_.unlock();

    /* 池空。**不能**和下面那个 chunk == nullptr 合并成一个分支: 那个还覆盖了
     * "段建不出来"和 id 越界, 混在一起报会把两种完全不同的故障源说成一个。
     * 这里的 id < 0 是 id_pool::acquire() 对空池的唯一返回值, 语义精确。 */
    if (id < 0) {
      /* UF-003: 池穷尽 ⇒ 先做一次"死持有者清扫", 再重试一次 acquire。
       * 清扫只回收"本路由借出、已发布、且全部持有者确定已死"的悬挂 chunk;
       * 任何不确定一律放弃(保守, 退化为原有背压路径)。 */
      if (reclaim_dead_chunks(inf, info, chunk_size, bitmap_semantics, kind, pref, count)) {
        info->lock_.lock();
        info->pool_.prepare();
        id = info->pool_.acquire();
        info->lock_.unlock();
      }

      if (id < 0) {
        /* prefix 按 const& 传、且只在本次调用内同步使用 —— 这里没有 get_info 上面
         * 那条"buff_t 析构时 conn_info 可能已 mem::free"的生存期问题: 本函数在
         * acquire 路径上, inf 是调用方自己活着的 conn_info。 */
        note_pool_exhausted(kind, chunk_size, size, pref, count);
        return fail(ipc::loan_status::pool_exhausted);
      }
    }

    auto chunk = info->at(chunk_size, id, count);
    if (chunk == nullptr)
      /* id 拿到了却定位不到块 = 池/段不一致（id 越界），与环境有关而与背压无关。 */
      return fail(ipc::loan_status::storage_unavailable);
    /* UF-003: 借出即标"未发布"并打上本路由标签。两处都必须在 conns.store 之前
     * 完成, 且 conns.store 用 release: 清扫方读到新位图时, 必然也已经看到
     * published == 0(在飞, 让路)与正确的路由标签。 */
    chunk->published().store(0, std::memory_order_relaxed);
    chunk->route_tag().store(inf->route_tag_, std::memory_order_relaxed);
    chunk->conns().store(conns, std::memory_order_release);
    return {id, chunk->data()};
  }

  void *find_storage(ipc::storage_id_t id, conn_info_head *inf,
                     std::size_t size)
  {
    if (!ipc::detail::valid_storage(id))
    {
      ipc::error("[find_storage] id is invalid: id = %ld, size = %zd\n", (long)id,
                 size);
      return nullptr;
    }
    if (inf == nullptr)
      return nullptr;
    std::size_t chunk_size = calc_chunk_size(size);
    constexpr auto count = ipc::topic_msg_cache;
    auto info = chunk_storage_info(inf->topic_pool_, chunk_size, count);
    if (info == nullptr)
      return nullptr;
    auto* chunk = info->at(chunk_size, id, count);
    return chunk == nullptr ? nullptr : chunk->data();
  }

  void release_storage(ipc::storage_id_t id, conn_info_head *inf,
                       std::size_t size)
  {
    if (!ipc::detail::valid_storage(id))
    {
      ipc::error("[release_storage] id is invalid: id = %ld, size = %zd\n",
                 (long)id, size);
      return;
    }
    if (inf == nullptr)
      return;
    std::size_t chunk_size = calc_chunk_size(size);
    constexpr auto count = ipc::topic_msg_cache;
    auto info = chunk_storage_info(inf->topic_pool_, chunk_size, count);
    if (info == nullptr)
      return;
    info->lock_.lock();
    info->pool_.release(id);
    info->lock_.unlock();
  }

  /* 归还幂等的**唯一依据**: 本次 CAS 到底有没有真的清掉"我自己那一位"。
   *
   * 为什么需要它 —— chunk->conns() 是发送时刻一次性置好的收方**位图**, 不是引用
   * 计数。清除它的地方有三处, 由不同线程/进程驱动同一张位图, 而位图本身没有仲裁者:
   *   ① 收方 buff_t 析构 → recycle_storage → sub_rc(正常路径);
   *   ② 写方覆写槽位 → discard_storage(被套圈的收方永远不会来取);
   *   ③ 缓冲被借走时的归还(adopt / loan 路径)。
   * 于是"同一个 conn 把同一块 chunk 归还两次"在协议上从未被排除 —— 而
   * id_pool::release 是 next_[id] = cursor_ 的**头插且不幂等**(id_pool.h:76-81):
   * 第二次 release 时 cursor_ 已经等于 id, 于是 next_[id] = id, 空闲链表接成自环,
   * 此后 acquire() 永远返回同一个 id、池里其余 id 永久不可达。实测指纹
   * next_[23] == 23 且 cursor_ == 23, 见 docs/shm_chunk_pool_occupancy_plan.md
   * §3 步骤③ 的副产品小节。
   *
   * 判据本身极便宜 —— CAS 的 expected 参数在**成功**那一轮被写回"内存里真正躺着
   * 的旧值", 所以:
   *     (旧值 & 我的位) == 0  ⇒ 本轮之前那一位**已经是 0** ⇒ 位不是我清的
   *                           ⇒ 这次归还不是我该做的 ⇒ 一律不还池。
   * 这把"谁有权还池"从**位置判据**(位图现在空不空)换成**动作判据**(这一位是不是
   * 我清掉的): 前者不幂等 —— 别人清完再轮到我就成了"位图空着, 那我清, 我该还";
   * 后者幂等, 与谁先谁后无关。
   *
   * ⛔ 它**不**证明载荷没被覆写, 也**不**让"归还早了"变得安全。位已清也可能是
   * "本格已被 force_push 覆写、旧内容已被 discard_storage 收回" —— 那条路径上
   * 读方仍可能持有一个已被回池复用的 id(ABA), 本守卫对它的射程是**零**。
   * 覆写那一侧的防护在 prod_cons.h 的 pop() 里(拷贝前后与进 clear 前各比一次
   * epoch), 与本判据各管一头, 不可互相替代。 */
  void note_double_return(char const *site, ipc::storage_id_t id,
                          ipc::circ::cc_t conn_mask) noexcept
  {
    static std::mutex lock;
    static std::uint64_t count = 0;
    std::uint64_t n;
    {
      std::lock_guard<std::mutex> guard{lock};
      n = ++count;
    }
    if ((n == 1) || ((n % 1024) == 0)) {
      ipc::error("chunk returned twice: site = %s, id = %ld, conn_mask = %u, "
                 "count = %llu (本进程); 已拦下, 未二次入池\n",
                 site, (long)id, static_cast<unsigned>(conn_mask),
                 static_cast<unsigned long long>(n));
    }
  }

  template <ipc::relat Rp, ipc::relat Rc>
  bool sub_rc(ipc::wr<Rp, Rc, ipc::trans::unicast>,
              std::atomic<ipc::circ::cc_t> & /*conns*/,
              ipc::circ::cc_t /*curr_conns*/,
              ipc::circ::cc_t /*conn_id*/,
              bool *out_dup = nullptr) noexcept
  {
    if (out_dup != nullptr)
      *out_dup = false;
    /* 无条件放行是**有意的**: unicast 的收方上限是 1, 而
     *   - push 只取"读方已放行"的格子(其 rem_cc 恒为 0) ⇒ 写方永远不会替它清位;
     *   - force_push 压根不调用归还回调。
     * 于是它的 conns 位图只有一个驱动者(收方自己), 同一块 chunk 不可能被归还两次
     * ⇒ 幂等守卫在这条路径上没有可拦的东西。若哪天 unicast 也接入了覆写/借样归还,
     * 这里必须补上与 broadcast 同款的判据, 而不是继续返回 true。 */
    return true;
  }

  template <ipc::relat Rp, ipc::relat Rc>
  bool sub_rc(ipc::wr<Rp, Rc, ipc::trans::broadcast>,
              std::atomic<ipc::circ::cc_t> &conns,
              ipc::circ::cc_t /*curr_conns*/, ipc::circ::cc_t conn_id,
              bool *out_dup = nullptr) noexcept
  {
    /* ── 归还的单一权威判据 ───────────────────────────────────────────────────
     * **谁把位图清空, 谁还池 —— 且每人只清自己那一位。**
     *
     * 旧实现清的是 `curr_conns & ~conn_id`(接收时在连的**所有**人), 于是"谁是最后
     * 一个归还者"有两个互斥的结局, 而两个都错:
     *   - 我先跑: 我顺手把别人(还在持有着的)的位也清了, 看到"有人没清完"就返回
     *     false —— 可那些位已经被我清掉, 对方后来再跑时 `mine == false`, 于是**谁也
     *     不还** ⇒ 那块 chunk 永久漏在池外;
     *   - 我不清: 双方都以为自己不是最后一个 ⇒ 同一块被还两次 ⇒ `next_[id] == id`
     *     自环, 池塌成一块且永久退化(实测: 40 轮 8/8 次复现)。
     *
     * 正解是让"清空"这件事**只可能被一个人观测到**: 每人只清自己那一位, 清完读到的
     * 结果就是权威 —— 为 0 说明自己是最后一个持有者, 由自己还; 不为 0 说明还有人,
     * 由那个人还。CAS 保证"清空"只有一个赢家, 于是"还池"也只有一个赢家。
     *
     * `mine == false`(自己那一位已经是 0)是**重复归还**: 本次调用没有清掉任何东西,
     * 无权还池, 并且说明上游把同一块 chunk 交付了两次(套圈重读等) —— 经 out_dup 报给
     * 调用方记一次诊断, 见 note_double_return。
     *
     * ⛔ 本判据**不**处理"归还得太早"(写方按 rem_cc 清掉了一个其实已 pop 的收方的位)。
     * 那一侧的防护在 prod_cons.h 的 pop() 里(拷贝前后与进 clear 前各比一次 epoch)。 */
    for (unsigned k = 0;;)
    {
      auto chunk_conns = conns.load(std::memory_order_acquire);
      if ((chunk_conns & conn_id) == 0)
      {
        if (out_dup != nullptr)
          *out_dup = true;   /* 我这一位已被清过 ⇒ 这次归还是重复的 */
        return false;
      }
      auto nxt_conns = static_cast<ipc::circ::cc_t>(chunk_conns & ~conn_id);
      if (conns.compare_exchange_weak(chunk_conns, nxt_conns,
                                      std::memory_order_release))
      {
        if (out_dup != nullptr)
          *out_dup = false;
        /* 清完为空 ⇒ 我是最后一个持有者 ⇒ 由我还池。非空 ⇒ 交给最后那个。 */
        return nxt_conns == 0;
      }
      ipc::yield(k);
    }
  }

  template <typename Flag>
  /* ⛔ 只收前缀(按值), 不收 conn_info_head*: 本函数由"大消息 buff_t 的析构器"
   * 调用, 而调用点上接收方的 conn_info 可能早已 mem::free —— 那就是
   * use-after-free(实测见 docs/shm_defect_fixes.md 第 7 条)。前缀是这条路径唯一
   * 需要的信息, 而且必须是值的拷贝(接收时拷好, 见 recv 里的 recycle_t)。 */
  void recycle_storage(std::shared_ptr<ipc::topic_pool_context> const& pool, ipc::storage_id_t id,
                       std::size_t size, ipc::circ::cc_t curr_conns,
                       ipc::circ::cc_t conn_id)
  {
    if (!ipc::detail::valid_storage(id))
    {
      ipc::error("[recycle_storage] id is invalid: id = %ld, size = %zd\n",
                 (long)id, size);
      return;
    }
    std::size_t chunk_size = calc_chunk_size(size);
    constexpr auto count = ipc::topic_msg_cache;
    auto info = chunk_storage_info(pool, chunk_size, count);
    if (info == nullptr)
      return;

    auto chunk = info->at(chunk_size, id, count);
    if (chunk == nullptr)
      return;

    bool dup = false;
    if (!sub_rc(Flag{}, chunk->conns(), curr_conns, conn_id, &dup))
    {
      /* 两种 false 都不还池, 但只有一种是异常:
       *   - dup = 我这一位本来就已是 0 ⇒ 之前已被别人清掉过(写方覆写归还, 或同一
       *     conn 归还了两次) ⇒ **这次归还是重复的** —— 旧实现照样落进
       *     id_pool::release, 于是 next_[id] == id 自环, 池里其余 id 永久不可达;
       *   - !dup = 我这一位刚被我清掉、但位图仍非空 ⇒ 仍有别的持有者, 由最后一个
       *     归还者还(正常并发路径, 广播下是常态, 报它就是刷屏)。 */
      if (dup)
      {
        note_double_return("recycle_storage", id, conn_id);
      }
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
   * 读不到, 其位若不在此清掉, 位图永不归零 → chunk 永久泄漏(每尺寸档 id_pool::
   * max_count 块, 见该常量)。旧实现正是为此才无条件 release_storage。
   *
   * 本函数**自己也要幂等**(见 note_double_return): rem_cc 是一个位图快照, 从捕获
   * 到清位之间这些位可能已被别人清掉(收方 buff_t 析构, 或另一次覆写) —— 若只看
   * "清完还剩几个"就还池, 那就是拿一个早已回池的 id 再还一次, 空闲链接成自环。
   * 判据是**这一位是不是我清掉的**, 不是"位图现在空不空"。
   *
   * 为什么不能无条件 release: 无条件归还会把"正被接收方持有的 chunk id"直接放回
   * 池子, 下一帧 acquire 到同一 id 就会覆写持有者正在读的内存, 且持有者析构时会
   * 造成同一 id 二次入池(两条消息拿到同一块)。今天接收方拿到 buff_t 后立刻拷出
   * 就丢, 窗口是微秒级; 一旦让接收方长期持有 chunk(DZFlat 的 Sample), 该窗口会
   * 被拉成秒级并必现。
   *
   * 残余窗口(本函数**只关了一半**): 上面那条幂等守卫拦的是"同一收方把同一块归还
   * 两次"。它拦不住的是**归还得太早**: pop() 先把槽位数据拷出、之后才清自己的 rc
   * 位, 所以一个"已读到 storage id 但尚未清位"的接收方仍会被算进 rem_cc, 被当成
   * "永远看不到这格"的收方而在此处把 chunk 收回 —— 那个读方随后仍会拿这个已回池
   * (可能已复用)的 id 去建 buff_t(ABA)。这一侧的正解在 prod_cons.h 的 pop() 里
   * (拷贝前后与进 clear 前各比一次 epoch, 见该处 ②③), ⛔ 不在这里, 也不要以为
   * 本守卫把它一并解决了。 */
  void discard_storage(ipc::storage_id_t id, conn_info_head *inf,
                       std::size_t size, ipc::circ::cc_t rem_cc)
  {
    if (!ipc::detail::valid_storage(id))
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

    if (inf == nullptr)
      return;
    std::size_t chunk_size = calc_chunk_size(size);
    constexpr auto count = ipc::topic_msg_cache;
    auto info = chunk_storage_info(inf->topic_pool_, chunk_size, count);
    if (info == nullptr)
      return;

    auto chunk = info->at(chunk_size, id, count);
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
        /* 幂等守卫, 与 sub_rc(broadcast) 同款: rem_cc 捕获的是一个**位图**而非计数,
         * 所以"这些位是我清掉的吗"必须看 CAS 成功那一轮的旧值。一个位都不剩说明
         * 这些收方已经各自归还过了(或已被另一次覆写清掉)—— 那块 chunk 早已回池,
         * 再还一次就是自环。见 note_double_return。 */
        if ((cur_conns & rem_cc) == 0)
        {
          note_double_return("discard_storage", id, rem_cc);
          return;
        }
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
      auto id = ipc::detail::storage_from_wire(*reinterpret_cast<ipc::storage_id_t *>(&msg->data_));
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

      /* ⛔ 段名同步点(必须逐字一致): 本函数 + clear_storage + sniffer.cpp 的
       * QU_CONN__ 构造。UF-003 在 elems 尾部追加了 owner 表, 布局变了 ⇒ 段名
       * 带 __V2 版本分量: 新旧二进制各建各段, 绝不混挂同一段(拍板文档 §1)。 */
      static ipc::string elems_name(char const *prefix, char const *name)
      {
        return ipc::make_prefix(
            ipc::make_string(prefix),
            {"QU_CONN__", ipc::make_string(name), "__", ipc::to_string(DataSize),
             "__", ipc::to_string(AlignSize), "__V4"});
      }

      void init()
      {
        conn_info_head::init();
        ipc::string const ename = elems_name(prefix_.c_str(), this->name_.c_str());
        if (!que_.valid())
        {
          que_.open(ename.c_str());
        }
        /* UF-003: 绑定本路由的 owner 表与路由标签(清扫方只处置本路由的块)。 */
        // 同名队列重建后可能复用连接位；inode 将旧 Sample 与新 owner 表隔离。
        struct stat generation{};
        const std::string path = "/dev/shm/" + std::string(ename.c_str());
        if (::stat(path.c_str(), &generation) == 0) {
          const auto tag_name = std::string(ename.c_str()) + "#" + std::to_string(generation.st_ino);
          route_tag_ = ipc::circ::route_tag_of(tag_name.c_str());
        } else route_tag_ = 0; // 无法确定代次时禁用自动死持有者清扫

        owners_ = que_.valid() ? &que_.elems()->owners() : nullptr;
      }

      void clear() noexcept
      {
        que_.clear();
        conn_info_head::clear();
      }

      static void clear_storage(char const *prefix, char const *name) noexcept
      {
        queue_t::clear_storage(elems_name(prefix, name).c_str());
        conn_info_head::clear_storage(prefix, name);
      }

      void disconnect_receiver()
      {
        bool dis = que_.disconnect();
        this->quit_waiting();
        if (dis)
        {
          std::lock_guard<std::mutex> lock(this->recv_cache_mutex_);
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

    static ipc::recv_wait_token read_wait_token(ipc::handle_t h) noexcept
    {
      auto* info = info_of(h);
      return (info == nullptr) ? ipc::recv_wait_token{} : info->rd_waiter_.read_wait_token();
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
        auto dat = acquire_storage(inf, size, conns, "send",
                                   ipc::relat_trait<flag_t>::is_broadcast);
        void *buf = dat.second;
        if (buf != nullptr)
        {
          std::memcpy(buf, data, size);
          auto wire_id = ipc::detail::storage_to_wire(dat.first);
          if (try_push(static_cast<std::int32_t>(size) -
                           static_cast<std::int32_t>(ipc::data_length),
                       &wire_id, 0))
          {
            /* UF-003: 已发布 ⇒ 清扫方自此才可接手处置该块。 */
            mark_published(inf, size, dat.first);
            return true;
          }
          /* 没进队列 ⇒ 没有任何接收方会持有它 ⇒ 立即归还(否则永久悬挂)。 */
          return_unpublished(inf, size, dat.first);
          return false;
        }
        // try using message fragment
        /* ⛔ 不要在这里重新打开逐条 log: 池空时每一条大消息都会走到这里, 打出来就是
         * 刷屏。池空已由 acquire_storage 的 note_pool_exhausted 统一收口
         * (首报 + 计数节流, kind = "send")。 */
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
        auto dat = acquire_storage(inf, size, conns, "no_member_send",
                                   ipc::relat_trait<flag_t>::is_broadcast);
        void *buf = dat.second;
        if (buf != nullptr)
        {
          std::memcpy(buf, data, size);
          auto wire_id = ipc::detail::storage_to_wire(dat.first);
          if (try_push(static_cast<std::int32_t>(size) -
                           static_cast<std::int32_t>(ipc::data_length),
                       &wire_id, 0))
          {
            mark_published(inf, size, dat.first);
            return 1;
          }
          return_unpublished(inf, size, dat.first);
          return 0;
        }
        // try using message fragment
        /* ⛔ 不要在这里重新打开逐条 log: 池空时每一条大消息都会走到这里, 打出来就是
         * 刷屏。池空已由 acquire_storage 的 note_pool_exhausted 统一收口
         * (首报 + 计数节流, kind = "no_member_send")。 */
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

    static ipc::buff_t recv(ipc::handle_t h, std::uint64_t tm, bool verbose, bool *consumed = nullptr)
    {
      if (consumed) *consumed = false;
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
        if (consumed) *consumed = true;
        if (consumed && !msg.storage_) return {};
        if (writable && !consumed)
        {
          notify_writers(inf);
        }
        if ((inf->acc() != nullptr) && (msg.cc_id_ == inf->cc_id_))
        {
          if (consumed) return {};
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
              ipc::detail::storage_from_wire(*reinterpret_cast<ipc::storage_id_t *>(&msg.data_));
          void *buf = find_storage(buf_id, inf, msg_size);
          if (buf != nullptr)
          {
            /* ⛔ 这里**必须拷前缀**, 不能拷 conn_info_t*: 这个 buff_t 会活过接收方
             * 本身(它由上层的 Sample / 消息对象持有), 而 conn_info 在接收方析构
             * (chan_impl::destroy)时就 mem::free 了 —— 旧实现下这种顺序就是
             * use-after-free, 现象是析构时 SIGSEGV at 0(内核日志只有一个 ip)。
             * 详见 docs/shm_defect_fixes.md 第 7 条。 */
            struct recycle_t
            {
              ipc::storage_id_t storage_id;
              std::shared_ptr<ipc::topic_pool_context> pool;
              ipc::circ::cc_t curr_conns;
              ipc::circ::cc_t conn_id;
            } *r_info = ipc::mem::alloc<recycle_t>(recycle_t{
                buf_id, inf->topic_pool_,
                que->elems()->connections(std::memory_order_relaxed),
                que->connected_id()});
            if (r_info == nullptr)
            {
              ipc::log("fail: ipc::mem::alloc<recycle_t>.\n");
              recycle_storage<flag_t>(inf->topic_pool_, buf_id, msg_size,
                  que->elems()->connections(std::memory_order_relaxed), que->connected_id());
              return {};
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
                    recycle_storage<flag_t>(r_info->pool, r_info->storage_id, size,
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
            if (consumed) return {};
            continue;
          }
        }
        // 分片缓存仅在本连接内查找；锁不跨共享队列等待，也不进入共享 chunk 快路径。
        std::lock_guard<std::mutex> cache_lock(inf->recv_cache_mutex_);
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

    /* t46：**唯一**的借样实现。`why == nullptr` 时与旧实现逐位相同（旧行为由下面
     * 那个薄转发保留）。所有失败出口都在这里逐条归因 —— 出口数 6，与 §"五出口"
     * 清点一一对应（交付 §2 的表）。 */
    static ipc::loan_t loan_impl(ipc::handle_t h, std::size_t size,
                                 ipc::loan_status *why, bool verbose)
    {
      const auto fail = [why](ipc::loan_status w) -> ipc::loan_t {
        if (why != nullptr)
          *why = w;
        return {};
      };
      auto que = queue_of(h);
      if (que == nullptr || que->elems() == nullptr)
      {
        if (verbose)
          ipc::error("fail: loan, invalid queue\n");
        return fail(ipc::loan_status::invalid_handle);
      }
      if (!que->ready_sending())
      {
        if (verbose)
          ipc::error("fail: loan, que->ready_sending() == false\n");
        return fail(ipc::loan_status::not_ready);
      }
      /* 无接收方时不存在 chunk 语义: send() 在这种情况下也不走 chunk(改用 sniffer
       * 环或分片), 强行借了没人回收。让调用方回退。 */
      ipc::circ::cc_t conns =
          que->elems()->connections(std::memory_order_relaxed);
      if (conns == 0)
      {
        if (verbose)
          ipc::error("fail: loan, there is no receiver on this connection.\n");
        return fail(ipc::loan_status::no_receiver);
      }
      /* 接收侧靠 msg.storage_ 判定大消息, 而 storage_ 只在 size > large_msg_limit
       * 的分支被设置。借样必须落在那条路径上。 */
      if (size <= ipc::large_msg_limit)
      {
        size = ipc::large_msg_limit + 1;
      }
      // 同时检查池容量乘法与队列 remain 的有符号编码上限。
      const std::size_t kMaxChunkSize =
          ((std::numeric_limits<std::size_t>::max)() - sizeof(chunk_info_t)) /
          static_cast<std::size_t>(ipc::id_pool<>::max_count);
      const std::size_t cap = loan_size_class(size);
      if (cap > kMaxChunkSize || cap > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()))
      {
        if (verbose)
          ipc::error("fail: loan, requested size too large (segment size arithmetic "
                     "would overflow): size = %zu, cap = %zu, max = %zu\n",
                     size, cap, kMaxChunkSize);
        return fail(ipc::loan_status::size_too_large);
      }
      conn_info_t *inf = info_of(h);
      ipc::loan_status w = ipc::loan_status::ok;
      auto dat = acquire_storage(inf, cap, conns, "loan",
                                 ipc::relat_trait<flag_t>::is_broadcast, &w);
      if (dat.second == nullptr)
      {
        /* chunk 池耗尽(每话题每档 10 块)。这是背压信号。
         * 计数与首报在 acquire_storage 的 note_pool_exhausted 里, 见该处注释。 */
        return fail(w);
      }
      ipc::loan_t lo;
      lo.id = dat.first;
      lo.data = dat.second;
      lo.size = cap;
      try {
        lo.lifetime = std::make_shared<ipc::detail::loan_lifetime>();
        lo.lifetime->pool = inf->topic_pool_;
        auto pool = inf->topic_pool_;
        const auto id = lo.id;
        lo.lifetime->give_back = [pool, id, cap] {
          auto* info = chunk_storage_info(pool, calc_chunk_size(cap));
          if (!info) return;
          auto* chunk = info->at(calc_chunk_size(cap), id);
          chunk->published().store(0, std::memory_order_relaxed);
          chunk->conns().store(0, std::memory_order_release);
          info->lock_.lock(); info->pool_.release(id); info->lock_.unlock();
        };
      } catch (...) {
        release_storage(lo.id, inf, cap);
        return fail(ipc::loan_status::storage_unavailable);
      }
      if (why != nullptr)
        *why = ipc::loan_status::ok;
      return lo;
    }

    static ipc::loan_t loan(ipc::handle_t h, std::size_t size, bool verbose)
    {
      return loan_impl(h, size, nullptr, verbose);
    }

    static ipc::loan_t loan(ipc::handle_t h, std::size_t size,
                            ipc::loan_status *st, bool verbose)
    {
      return loan_impl(h, size, st, verbose);
    }

    static bool publish_loan_impl(ipc::handle_t h, ipc::loan_t const &lo,
                             std::uint64_t tm, bool verbose, bool allow_overwrite, bool wake_readers, std::size_t used_size)
    {
      if (!lo.valid() || !ipc::detail::valid_storage(lo.id))
        return false;
      auto que = queue_of(h);
      conn_info_t *inf = info_of(h);
      if (que == nullptr || inf == nullptr || que->elems() == nullptr)
      {
        if (verbose)
          ipc::error("fail: publish_loan, invalid queue\n");
        return false;
      }
      auto acc = inf->acc();
      if (acc == nullptr)
      {
        if (verbose)
          ipc::error("fail: publish_loan, info_of(h)->acc() == nullptr\n");
        return false;
      }
      auto msg_id = acc->fetch_add(1, std::memory_order_relaxed);
      /* 与 send() 的大消息分支同构: 槽位里写的是 chunk id(整数), 不是数据。
       * 传 size = 0 让 msg_t 置 storage_ = true 并拷贝 id;
       * 默认 remain 编码借到的容量；publish_loan_size 可携带同档的真实长度。
       * 接收侧按尺寸档反推 chunk_size 定位共享段，因此已验证 used_size 与
       * lo.size 属于同一档。TLV 的历史页尾由此保持位于逻辑载荷末尾。 */
      const std::int32_t remain = static_cast<std::int32_t>(used_size ? used_size : lo.size) -
                                  static_cast<std::int32_t>(ipc::data_length);
      auto id = ipc::detail::storage_to_wire(lo.id);
      bool pushed = wait_for(
          inf->wt_waiter_,
          [&]
          {
            /* 同上: push 覆写被套圈的格子时也要归还被丢弃消息的 chunk。 */
            return !que->push(
                [inf, &lo](void *p, ipc::circ::cc_t rem_cc)
                {
                  const bool ready = clear_message<typename queue_t::value_t, flag_t>(inf, p, rem_cc);
                  if (ready) mark_published(inf, lo.size, lo.id);
                  return ready;
                },
                inf->cc_id_, msg_id, remain, &id, 0);
          },
          tm);
      if (!pushed && allow_overwrite)
      {
        if (verbose)
          ipc::log("publish_loan force_push: msg_id = %zd, cap = %zd\n", msg_id,
                   lo.size);
        pushed = que->force_push(
            [inf, &lo](void *p, ipc::circ::cc_t rem_cc)
            {
              const bool ready = clear_message<typename queue_t::value_t, flag_t>(inf, p, rem_cc);
              if (ready) mark_published(inf, lo.size, lo.id);
              return ready;
            },
            inf->cc_id_, msg_id, remain, &id, 0);
      }
      if (!pushed)
      {
        /* 没能进队列 = 没有任何接收方会回收它, 必须自己还回去。 */
        if (verbose)
          ipc::error("fail: publish_loan, push failed; chunk returned\n");
        return false;
      }
      /* 标记已在队列发布前的槽位回调内完成。接收者可能已归还甚至复用了
       * chunk；发布可见后绝不能再按旧 storage id 修改该块元数据。 */
      ipc::detail::trace_publish(ipc::detail::PublishPoint::BeforeNotify);
      if (wake_readers) notify_readers(inf);
      ipc::detail::trace_publish(ipc::detail::PublishPoint::AfterNotify);
      return true;
    }

    static bool publish_loan(ipc::handle_t h, ipc::loan_t const& lo,
                             std::uint64_t tm, bool verbose, bool allow_overwrite = true, bool wake_readers = true, std::size_t used_size = 0) {
      if (!lo.valid() || !lo.lifetime) return false;
      if (used_size && (used_size > lo.size || loan_size_class(std::max<std::size_t>(used_size, ipc::large_msg_limit + 1)) != lo.size)) return false;
      auto* inf = info_of(h);
      std::lock_guard<std::mutex> guard(lo.lifetime->mutex);
      if (lo.lifetime->finished || !inf || inf->topic_pool_ != lo.lifetime->pool) return false;
      bool okay;
      try { okay = publish_loan_impl(h, lo, tm, verbose, allow_overwrite, wake_readers, used_size); }
      catch (...) {
        // 可见性未知时封住本地归还；由独占会话关闭/接收所有者完成清理，不能归还仍可见 chunk。
        lo.lifetime->finished = true; lo.lifetime->give_back = {}; lo.lifetime->pool.reset(); throw;
      }
      lo.lifetime->finished = true;
      auto give_back = std::move(lo.lifetime->give_back);
      if (!okay && give_back) give_back();
      lo.lifetime->pool.reset();
      return okay;
    }

    static void discard_loan(ipc::handle_t, ipc::loan_t const& lo) {
      if (!lo.lifetime) return;
      std::lock_guard<std::mutex> guard(lo.lifetime->mutex);
      if (lo.lifetime->finished) return;
      lo.lifetime->finished = true;
      auto give_back = std::move(lo.lifetime->give_back);
      if (give_back) give_back();
      lo.lifetime->pool.reset();
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
    // destroy 先断开本句柄；活跃对端继续复用，最后租约负责清理。
    if (h != nullptr) destroy(h);
  }

  template <typename Flag>
  void chan_impl<Flag>::clear_storage(char const *name) noexcept
  {
    chan_impl<Flag>::clear_storage({nullptr}, name);
  }

  template <typename Flag>
  void chan_impl<Flag>::clear_storage(prefix pref, char const *name) noexcept
  {
    // 删除池与队列必须在同一个目录锁内完成，禁止清理与新建交错。
    ipc::clear_topic_pools(ipc::make_string(pref.str), ipc::make_string(name));
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
  ipc::recv_wait_token chan_impl<Flag>::read_wait_token(ipc::handle_t h) noexcept
  {
    return detail_impl<policy_t<Flag>>::read_wait_token(h);
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
  buff_t chan_impl<Flag>::try_recv_loan(ipc::handle_t h, bool &consumed, bool verbose)
  {
    return detail_impl<policy_t<Flag>>::recv(h, 0, verbose, &consumed);
  }

  template <typename Flag>
  ipc::loan_t chan_impl<Flag>::loan(ipc::handle_t h, std::size_t size,
                                    bool verbose)
  {
    return detail_impl<policy_t<Flag>>::loan(h, size, verbose);
  }

  template <typename Flag>
  ipc::loan_t chan_impl<Flag>::loan(ipc::handle_t h, std::size_t size,
                                    ipc::loan_status *st, bool verbose)
  {
    return detail_impl<policy_t<Flag>>::loan(h, size, st, verbose);
  }

  template <typename Flag>
  bool chan_impl<Flag>::publish_loan(ipc::handle_t h, ipc::loan_t const &lo,
                                     std::uint64_t tm, bool verbose)
  {
    return detail_impl<policy_t<Flag>>::publish_loan(h, lo, tm, verbose);
  }

  template <typename Flag>
  bool chan_impl<Flag>::try_publish_loan(ipc::handle_t h, ipc::loan_t const &lo, bool verbose, bool wake_readers)
  {
    return detail_impl<policy_t<Flag>>::publish_loan(h, lo, 0, verbose, false, wake_readers);
  }

  template <typename Flag>
  bool chan_impl<Flag>::publish_loan_size(ipc::handle_t h, ipc::loan_t const &lo, std::size_t used, bool verbose)
  {
    if (!used) return false;
    return detail_impl<policy_t<Flag>>::publish_loan(h, lo, 0, verbose, true, true, used);
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
