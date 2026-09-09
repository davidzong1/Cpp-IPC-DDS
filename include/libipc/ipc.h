#pragma once

#include <string>

#include "libipc/export.h"
#include "libipc/def.h"
#include "libipc/buffer.h"
#include "libipc/shm.h"
#include "libipc/sniffer.h"

namespace ipc
{

  using handle_t = void *;
  using buff_t = buffer;

  enum : unsigned
  {
    sender,
    receiver
  };

  /**
   * \brief 一块已借出的共享 chunk。
   *
   * 常规 send() 是「调用方缓冲 → memcpy 进 chunk」; loan() 把 chunk 直接交给调用方,
   * 让它就地把负载写进共享内存, 省掉那一次 memcpy。配合 DZFlat 平坦布局使用时,
   * 「序列化」与「送进共享内存」合并成一次写入(见 docs/dzflat_shm.md)。
   *
   * \note size 是**借到的容量**, 不是负载长度。容量按尺寸档位向上取整(见
   *       loan_size_class), 因为共享段是按 chunk_size 分段命名的, 逐字节取整会让
   *       每个长度都开一个新段。接收侧据此还原 chunk 位置, 所以投递时travel 的
   *       正是这个容量值 —— 真实负载长度由负载自身的头部承载。
   *
   * \note 生命周期: loan() 成功后, 要么 publish_loan()(所有权转移给队列, 由最后一个
   *       接收方的 buff_t 析构归还), 要么 discard_loan()(立刻归还)。两者都不调用即为
   *       泄漏 —— 每个尺寸档位只有 32 块。
   */
  struct loan_t
  {
    ipc::storage_id_t id = -1;
    void *data = nullptr;
    std::size_t size = 0;   ///< 借到的容量(>= 请求值)

    bool valid() const noexcept { return (id >= 0) && (data != nullptr); }
  };

  template <typename Flag>
  struct IPC_EXPORT chan_impl
  {
    static ipc::handle_t init_first();

    static bool connect(ipc::handle_t *ph, char const *name, unsigned mode);
    static bool connect(ipc::handle_t *ph, prefix, char const *name,
                        unsigned mode);
    static bool reconnect(ipc::handle_t *ph, unsigned mode);
    static void disconnect(ipc::handle_t h);
    static void destroy(ipc::handle_t h);

    static char const *name(ipc::handle_t h);

    // Release memory without waiting for the connection to disconnect.
    static void release(ipc::handle_t h) noexcept;

    // Force cleanup of all shared memory storage that handles depend on.
    static void clear(ipc::handle_t h) noexcept;
    static void clear_storage(char const *name) noexcept;
    static void clear_storage(prefix, char const *name) noexcept;

    static std::size_t recv_count(ipc::handle_t h);
    static bool wait_for_recv(ipc::handle_t h, std::size_t r_count,
                              std::uint64_t tm);

    /**
     * \brief This handle's bit in the queue's receiver connection bitmap.
     * \return 0 when the handle is not connected as a receiver.
     *
     * Pair with disconnect_receivers() to let a supervising party reap a
     * specific dead reader.
     */
    static std::uint32_t connected_id(ipc::handle_t h);

    /**
     * \brief Forcibly clear specific receiver connection bits.
     *
     * The queue cannot tell a dead reader from a merely slow one — a reader
     * that has not released a slot may simply be behind. Callers MUST
     * establish liveness by other means (dzIPC drives this from the topic
     * control plane's per-subscriber heartbeats) and pass only the bits of
     * readers known to be gone. Disconnecting a live reader silently stops
     * its delivery with no notification to it.
     *
     * \param cc_ids Bitwise-OR of the connection ids to remove; 0 is a no-op.
     */
    static void disconnect_receivers(ipc::handle_t h, std::uint32_t cc_ids);

    static bool send(ipc::handle_t h, void const *data, std::size_t size,
                     std::uint64_t tm, bool verbose);
    static buff_t recv(ipc::handle_t h, std::uint64_t tm, bool verbose);
    static bool no_member_try_send(ipc::handle_t h, void const *data, std::size_t size,
                                   std::uint64_t tm, bool verbose);
    static bool try_send(ipc::handle_t h, void const *data, std::size_t size,
                         std::uint64_t tm, bool verbose);
    static buff_t try_recv(ipc::handle_t h, bool verbose);

    /**
     * \brief 借一块共享 chunk 供调用方就地写入。
     * \return 无效 loan_t 表示失败(无接收方 / chunk 池耗尽 / size 过小)。
     *
     * 失败时**必须**回退到 send/try_send —— 借样不是必成的, 池子只有 32 块/档位。
     * 成功后必须以 publish_loan 或 discard_loan 之一结束, 否则泄漏。
     */
    static ipc::loan_t loan(ipc::handle_t h, std::size_t size, bool verbose);

    /// \brief 把已借出的 chunk 作为一条消息投递(单条, 不拆包)。
    /// 失败时 chunk 已被本函数归还, 调用方不得再 discard_loan。
    static bool publish_loan(ipc::handle_t h, ipc::loan_t const &lo,
                             std::uint64_t tm, bool verbose);

    /// \brief 放弃一块未投递的 chunk, 立刻归还池子。幂等于无效 loan。
    static void discard_loan(ipc::handle_t h, ipc::loan_t const &lo);
  };

  template <typename Flag>
  class chan_wrapper
  {
  private:
    using detail_t = chan_impl<Flag>;

    ipc::handle_t h_ = detail_t::init_first();
    unsigned mode_ = ipc::sender;
    bool connected_ = false;
    bool verbose_ = true;

  public:
    chan_wrapper() noexcept = default;

    explicit chan_wrapper(char const *name, unsigned mode = ipc::sender,
                          bool verbose = true)
        : connected_{this->connect(name, mode)}, verbose_{verbose} {}

    chan_wrapper(prefix pref, char const *name, unsigned mode = ipc::sender,
                 bool verbose = true)
        : connected_{this->connect(pref, name, mode)}, verbose_{verbose} {}

    chan_wrapper(chan_wrapper &&rhs) noexcept : chan_wrapper{} { swap(rhs); }

    ~chan_wrapper() { detail_t::destroy(h_); }

    void swap(chan_wrapper &rhs) noexcept
    {
      std::swap(h_, rhs.h_);
      std::swap(mode_, rhs.mode_);
      std::swap(connected_, rhs.connected_);
      std::swap(verbose_, rhs.verbose_);
    }

    chan_wrapper &operator=(chan_wrapper rhs) noexcept
    {
      swap(rhs);
      return *this;
    }

    char const *name() const noexcept { return detail_t::name(h_); }

    // Release memory without waiting for the connection to disconnect.
    void release() noexcept
    {
      detail_t::release(h_);
      h_ = nullptr;
    }

    // Clear shared memory files under opened handle.
    void clear() noexcept
    {
      detail_t::clear(h_);
      h_ = nullptr;
    }

    // Clear shared memory files under a specific name.
    static void clear_storage(char const *name) noexcept
    {
      detail_t::clear_storage(name);
    }

    // Clear shared memory files under a specific name with a prefix.
    static void clear_storage(prefix pref, char const *name) noexcept
    {
      detail_t::clear_storage(pref, name);
    }

    ipc::handle_t handle() const noexcept { return h_; }

    bool valid() const noexcept { return (handle() != nullptr); }

    unsigned mode() const noexcept { return mode_; }

    chan_wrapper clone() const { return chan_wrapper{name(), mode_}; }

    /**
     * Building handle, then try connecting with name & mode flags.
     */
    bool connect(char const *name, unsigned mode = ipc::sender | ipc::receiver)
    {
      if (name == nullptr || name[0] == '\0')
        return false;
      detail_t::disconnect(h_); // clear old connection
      return connected_ = detail_t::connect(&h_, name, mode_ = mode);
    }
    bool connect(prefix pref, char const *name,
                 unsigned mode = ipc::sender | ipc::receiver)
    {
      if (name == nullptr || name[0] == '\0')
        return false;
      detail_t::disconnect(h_); // clear old connection
      return connected_ = detail_t::connect(&h_, pref, name, mode_ = mode);
    }

    /**
     * Try connecting with new mode flags.
     */
    bool reconnect(unsigned mode)
    {
      if (!valid())
        return false;
      if (connected_ && (mode_ == mode))
        return true;
      return connected_ = detail_t::reconnect(&h_, mode_ = mode);
    }

    void disconnect()
    {
      if (!valid())
        return;
      detail_t::disconnect(h_);
      connected_ = false;
    }

    std::size_t recv_count() const { return detail_t::recv_count(h_); }

    /// This handle's bit in the receiver connection bitmap; 0 if not a receiver.
    std::uint32_t connected_id() const { return detail_t::connected_id(h_); }

    /// Reap specific dead receivers. See chan_impl::disconnect_receivers —
    /// liveness is the caller's responsibility.
    void disconnect_receivers(std::uint32_t cc_ids)
    {
      detail_t::disconnect_receivers(h_, cc_ids);
    }

    bool wait_for_recv(std::size_t r_count,
                       std::uint64_t tm = invalid_value) const
    {
      return detail_t::wait_for_recv(h_, r_count, tm);
    }

    static bool wait_for_recv(char const *name, std::size_t r_count,
                              std::uint64_t tm = invalid_value)
    {
      return chan_wrapper(name).wait_for_recv(r_count, tm);
    }

    /**
     * If timeout, this function would call 'force_push' to send the data
     * forcibly.
     */
    bool send(void const *data, std::size_t size,
              std::uint64_t tm = default_timeout)
    {
      return detail_t::send(h_, data, size, tm, verbose_);
    }
    bool send(buff_t const &buff, std::uint64_t tm = default_timeout)
    {
      return this->send(buff.data(), buff.size(), tm);
    }
    bool send(std::string const &str, std::uint64_t tm = default_timeout)
    {
      return this->send(str.c_str(), str.size() + 1, tm);
    }

    /**
     * If timeout, this function would just return false.
     */
    bool no_member_try_send(void const *data, std::size_t size,
                            std::uint64_t tm = default_timeout)
    {
      return detail_t::no_member_try_send(h_, data, size, tm, verbose_);
    }
    bool try_send(void const *data, std::size_t size,
                  std::uint64_t tm = default_timeout)
    {
      return detail_t::try_send(h_, data, size, tm, verbose_);
    }
    bool try_send(buff_t const &buff, std::uint64_t tm = default_timeout)
    {
      return this->try_send(buff.data(), buff.size(), tm);
    }
    bool try_send(std::string const &str, std::uint64_t tm = default_timeout)
    {
      return this->try_send(str.c_str(), str.size() + 1, tm);
    }

    buff_t recv(std::uint64_t tm = invalid_value)
    {
      return detail_t::recv(h_, tm, verbose_);
    }

    buff_t try_recv() { return detail_t::try_recv(h_, verbose_); }

    /**
     * \brief 借一块共享 chunk 就地写入, 省掉 send() 的那次 memcpy。
     *
     * 典型用法(失败必须能回退, 池子只有 32 块/档位):
     * \code
     *   auto lo = ch.loan(need);
     *   if (lo.valid()) {
     *     if (!write_payload_into(lo.data, lo.size)) { ch.discard_loan(lo); ... }
     *     else if (!ch.publish_loan(lo)) { ... }   // 失败时 chunk 已由内部归还
     *   } else {
     *     ch.try_send(buf, n);                     // 回退整包路径
     *   }
     * \endcode
     */
    loan_t loan(std::size_t size) { return detail_t::loan(h_, size, verbose_); }

    bool publish_loan(loan_t const &lo, std::uint64_t tm = default_timeout)
    {
      return detail_t::publish_loan(h_, lo, tm, verbose_);
    }

    void discard_loan(loan_t const &lo) { detail_t::discard_loan(h_, lo); }
  };

  template <relat Rp, relat Rc, trans Ts>
  using chan = chan_wrapper<ipc::wr<Rp, Rc, Ts>>;
  /**
   * @brief server mode
   * \note You could use one producer/writer for sending messages to a
   * server, then one consumer/reader which is receiving with this server, would
   * receive your sent messages. A server could only be used in 1 to 1 (one
   * producer/writer to one consumer/reader).
   */
  using server = chan<relat::single, relat::single, trans::unicast>;
  /**
   * \class route
   *
   * \note You could use one producer/server/sender for sending messages to a
   * route, then all the consumers/clients/receivers which are receiving with this
   * route, would receive your sent messages. A route could only be used in 1 to N
   * (one producer/writer to multi consumers/readers).
   */
  using route = chan<relat::single, relat::multi, trans::broadcast>;

  /**
   * \class channel
   *
   * \note You could use multi producers/writers for sending messages to a
   * channel, then all the consumers/readers which are receiving with this
   * channel, would receive your sent messages.
   */
  using channel = chan<relat::multi, relat::multi, trans::broadcast>;

} // namespace ipc
