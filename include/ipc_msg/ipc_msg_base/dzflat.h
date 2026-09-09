#pragma once
/* DZFlat —— SHM 专属平坦布局的运行时支撑 (设计见 docs/dzflat_shm.md)
 *
 * 布局(layout_ver = 1):
 *
 *   ┌──────────────────────────────────────────────────────────┐
 *   │ SegHeader (32 B, 8 对齐)                                  │
 *   ├──────────────────────────────────────────────────────────┤
 *   │ Root record (定长, 编译期已知)                             │
 *   │   标量 / 定长数组 / 嵌套消息的 Root  → 原地内联             │
 *   │   string / T[] / string[] / Msg[]   → VarRef{off, cnt}    │
 *   ├──────────────────────────────────────────────────────────┤
 *   │ Varlen area (追加式, 每块 8 对齐)                          │
 *   └──────────────────────────────────────────────────────────┘
 *
 * 三条性质:
 *   ① 位置无关 —— 全段无指针, 只有相对段首的 u32 偏移。可整体 memcpy 到任意地址、
 *      写文件、发 socket, 语义不变;
 *   ② 嵌套零间接 —— 嵌套消息的 Root 一律内联(任意深度), 只有它自己的变长负载落到
 *      变长区。所以读一个嵌套标量是一次指针加法, 不存在逐层解析;
 *   ③ 无字段名、无 per-field type tag —— 版本由 SegHeader.schema_hash 一次性兜住。
 *
 * 读端不信任段内容: 段是由另一个进程写进共享内存的, 所有 VarRef 在解引用前都要
 * 对 total_size 做边界校验(越界 → 返回空视图, 不是未定义行为)。
 */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace dzflat {

/* 'D','Z','F','L' 小端。TLV wire 的首 4 字节是首字段名的长度(int32, 实测 1~64),
 * 与本值结构上不可能碰撞 —— 这就是两套 wire 的判别式(docs/dzflat_shm.md §3.7)。 */
constexpr std::uint32_t kMagic = 0x4C465A44u;
constexpr std::uint16_t kLayoutVer = 1;

/* 变长区每块的对齐。取 8 是为了让 double / uint64 / 元素 Root 都能原地读:
 * x86 容忍非对齐访问, ARM 不一定。 */
constexpr std::uint32_t kAlign = 8;

constexpr std::uint32_t align_up(std::uint32_t n) noexcept
{
    return (n + (kAlign - 1)) & ~(kAlign - 1);
}

/* 供 generator 发射的布局自证式使用: 按"声明顺序 + 自然对齐"推导期望偏移, 与编译器
 * 的实际 offsetof 比对。任何意外填充都会变成编译错误而不是运行期错数据。 */
constexpr std::size_t align_to(std::size_t n, std::size_t a) noexcept
{
    return ((n + a - 1) / a) * a;
}

struct SegHeader
{
    std::uint32_t magic;
    std::uint32_t schema_hash;
    std::uint32_t root_off;    /* = sizeof(SegHeader) */
    std::uint32_t root_size;   /* = sizeof(XxxRoot) */
    std::uint32_t total_size;  /* 全段字节数, 含本头 */
    std::uint16_t layout_ver;
    std::uint16_t flags;
    std::uint32_t msg_id;      /* 沿用 dz_ipc_msg_id 语义 */
    std::uint32_t reserved;
};
static_assert(sizeof(SegHeader) == 32, "SegHeader 必须是 32 字节");
static_assert(alignof(SegHeader) == 4, "SegHeader 只含 u32/u16, 对齐应为 4");

/* 变长字段的唯一间接层。off 相对段首(SegHeader 起点); off == 0 表示空。
 * cnt 的含义随字段类型而定: string 是字节数, 数组是元素个数。 */
struct VarRef
{
    std::uint32_t off;
    std::uint32_t cnt;
};
static_assert(sizeof(VarRef) == 8, "VarRef 必须是 8 字节");

/* ---------------------------------------------------------------- schema hash
 *
 * FNV-1a 32。规范化串由 generator 按声明顺序拼出(见 docs/dzflat_shm.md §3.5),
 * 嵌套类型在串里占位 "{}", 其自身的 hash 作为可变参混入 —— 于是父类型的 hash 可
 * 以只依赖"本地字段串 + 各嵌套类型的 hash 常量", 全部在编译期算完, 运行期零成本。
 */
constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;

constexpr std::uint32_t fnv1a(const char* s, std::uint32_t h = kFnvOffset) noexcept
{
    return (*s == '\0') ? h
                        : fnv1a(s + 1, (h ^ static_cast<std::uint8_t>(*s)) * kFnvPrime);
}

constexpr std::uint32_t fnv1a_u32(std::uint32_t v, std::uint32_t h) noexcept
{
    h = (h ^ (v & 0xFFu)) * kFnvPrime;
    h = (h ^ ((v >> 8) & 0xFFu)) * kFnvPrime;
    h = (h ^ ((v >> 16) & 0xFFu)) * kFnvPrime;
    h = (h ^ ((v >> 24) & 0xFFu)) * kFnvPrime;
    return h;
}

constexpr std::uint32_t mix_nested(std::uint32_t h) noexcept { return h; }

template<typename... Rest>
constexpr std::uint32_t mix_nested(std::uint32_t h, std::uint32_t first, Rest... rest) noexcept
{
    return mix_nested(fnv1a_u32(first, h), rest...);
}

/* canon = 本地字段规范化串; nested = 各嵌套类型的 kSchemaHash, 顺序与串中 "{}" 一致。 */
template<typename... H>
constexpr std::uint32_t schema_hash(const char* canon, H... nested) noexcept
{
    return mix_nested(fnv1a(canon), static_cast<std::uint32_t>(nested)...);
}

/* ---------------------------------------------------------------------- span
 *
 * C++17 没有 std::span, 这里只要一个只读的 {指针, 长度}。 */
template<typename T>
class span
{
public:
    span() = default;
    span(T* p, std::uint32_t n) : p_(p), n_(n) {}

    T* data() const noexcept { return p_; }
    std::uint32_t size() const noexcept { return n_; }
    bool empty() const noexcept { return n_ == 0; }
    T* begin() const noexcept { return p_; }
    T* end() const noexcept { return p_ + n_; }
    T& operator[](std::uint32_t i) const noexcept { return p_[i]; }

private:
    T* p_ = nullptr;
    std::uint32_t n_ = 0;
};

/* ------------------------------------------------------------------- Writer
 *
 * 变长区的追加器。reserve 只做"占位并返回段内偏移", 内容由调用方写。
 * 返回 0 表示失败(容量不足) —— 0 同时是 VarRef 的空值, 于是失败会被读端当作空
 * 字段, 而 ok() 让写端能把失败上报。 */
class Writer
{
public:
    Writer(std::uint8_t* seg, std::uint32_t cap, std::uint32_t cur)
        : seg_(seg), cap_(cap), cur_(cur)
    {
    }

    std::uint32_t reserve(std::uint32_t bytes)
    {
        const std::uint32_t at = align_up(cur_);
        if (at < cur_ || bytes > cap_ || at > cap_ - bytes)
        {
            ok_ = false;
            return 0;
        }
        /* 对齐填充的字节保持确定值, 避免把未初始化的共享内存写进 wire。 */
        if (at > cur_) std::memset(seg_ + cur_, 0, at - cur_);
        cur_ = at + bytes;
        return at;
    }

    std::uint8_t* at(std::uint32_t off) noexcept { return seg_ + off; }
    std::uint32_t size() const noexcept { return cur_; }
    bool ok() const noexcept { return ok_; }

private:
    std::uint8_t* seg_;
    std::uint32_t cap_;
    std::uint32_t cur_;
    bool ok_ = true;
};

/* ------------------------------------------------------------------- Reader
 *
 * 段的只读句柄 + 全部边界校验。段来自别的进程, 一律不信任。 */
class Reader
{
public:
    Reader() = default;
    Reader(const std::uint8_t* seg, std::uint32_t size) : seg_(seg), size_(size) {}

    bool valid() const noexcept { return seg_ != nullptr; }
    const std::uint8_t* base() const noexcept { return seg_; }
    std::uint32_t size() const noexcept { return size_; }

    /* 校验 [off, off + cnt * stride) 落在段内。stride 由调用方给出元素大小。 */
    bool in_bounds(std::uint32_t off, std::uint32_t cnt, std::uint32_t stride) const noexcept
    {
        if (seg_ == nullptr || off == 0) return false;
        if (off >= size_) return false;
        if (stride != 0 && cnt > (size_ - off) / stride) return false;
        return true;
    }

    template<typename T>
    span<const T> as_span(VarRef r) const noexcept
    {
        if (r.cnt == 0) return {};
        if (!in_bounds(r.off, r.cnt, static_cast<std::uint32_t>(sizeof(T)))) return {};
        return span<const T>(reinterpret_cast<const T*>(seg_ + r.off), r.cnt);
    }

    std::string_view as_string(VarRef r) const noexcept
    {
        if (r.cnt == 0) return {};
        if (!in_bounds(r.off, r.cnt, 1)) return {};
        return std::string_view(reinterpret_cast<const char*>(seg_ + r.off), r.cnt);
    }

    /* 取 Msg[] / string[] 的第 i 个元素记录的地址; 越界返回 nullptr。 */
    template<typename T>
    const T* elem_at(VarRef r, std::uint32_t i) const noexcept
    {
        if (i >= r.cnt) return nullptr;
        if (!in_bounds(r.off, r.cnt, static_cast<std::uint32_t>(sizeof(T)))) return nullptr;
        return reinterpret_cast<const T*>(seg_ + r.off) + i;
    }

private:
    const std::uint8_t* seg_ = nullptr;
    std::uint32_t size_ = 0;
};

/* ------------------------------------------------- B 级就地构造的通用原语
 *
 * generator 发射的 XxxBuilder 只是这几个函数的类型化包装。放在这里而不是生成到每个
 * 头文件里, 是为了让"占位 + 回填 VarRef"这段逻辑只有一份实现。
 *
 * 全部函数的失败语义一致: 容量不足时把 VarRef 置空、Writer 标记为 !ok, 返回空/false。
 * 于是调用方可以一路写下去不检查, 最后由 finalize 统一失败 —— 但**不能**把失败当成
 * "写成功了一部分", 段要么整体可用要么整体作废。 */

/// 在变长区占 n 个 T 并回填 ref; 返回**可写** span(B 级零拷贝的落点)。
template<typename T>
span<T> alloc_array(Writer& w, std::uint32_t n, VarRef& ref)
{
    if (n == 0)
    {
        ref = VarRef{0, 0};
        return {};
    }
    /* n * sizeof(T) 的溢出必须挡掉: n 可能来自调用方的计算结果。 */
    if (n > (0xFFFFFFFFu / static_cast<std::uint32_t>(sizeof(T))))
    {
        ref = VarRef{0, 0};
        return {};
    }
    const std::uint32_t bytes = n * static_cast<std::uint32_t>(sizeof(T));
    const std::uint32_t off = w.reserve(bytes);
    if (off == 0)
    {
        ref = VarRef{0, 0};
        return {};
    }
    ref = VarRef{off, n};
    return span<T>(reinterpret_cast<T*>(w.at(off)), n);
}

/// 把 sv 的字节拷进变长区并回填 ref。string 短且源在调用方堆上, 这一跳无法消除。
inline bool put_string(Writer& w, std::string_view sv, VarRef& ref)
{
    const std::uint32_t n = static_cast<std::uint32_t>(sv.size());
    if (n == 0)
    {
        ref = VarRef{0, 0};
        return true;
    }
    const std::uint32_t off = w.reserve(n);
    if (off == 0)
    {
        ref = VarRef{0, 0};
        return false;
    }
    std::memcpy(w.at(off), sv.data(), n);
    ref = VarRef{off, n};
    return true;
}

/// string[] 的两步写法第一步: 占 n 个 VarRef 的表(表项先置空)。
inline bool alloc_string_table(Writer& w, std::uint32_t n, VarRef& ref)
{
    auto tbl = alloc_array<VarRef>(w, n, ref);
    if (n != 0 && tbl.empty()) return false;
    for (std::uint32_t i = 0; i < tbl.size(); ++i) tbl[i] = VarRef{0, 0};
    return true;
}

/// string[] 第二步: 写第 i 项。i 越界或表未分配时返回 false。
inline bool put_string_at(Writer& w, VarRef table, std::uint32_t i, std::string_view sv)
{
    if (i >= table.cnt || table.off == 0) return false;
    auto* slot = reinterpret_cast<VarRef*>(w.at(table.off)) + i;
    return put_string(w, sv, *slot);
}

/// 取 Msg[] 第 i 个元素 Root 的**可写**地址; 越界返回 nullptr。
template<typename T>
T* elem_ptr(Writer& w, VarRef ref, std::uint32_t i)
{
    if (i >= ref.cnt || ref.off == 0) return nullptr;
    return reinterpret_cast<T*>(w.at(ref.off)) + i;
}

/// Root 区结束(= 变长区起点)。SegHeader 与 Root 都在这之前。
inline std::uint32_t varlen_start(std::uint32_t root_size) noexcept
{
    return align_up(static_cast<std::uint32_t>(sizeof(SegHeader)) + root_size);
}

/* 只看段首 4 字节是不是 magic。用来区分"这压根不是 DZFlat 段"(正常, TLV 缓冲就是这样)
 * 和"是 DZFlat 段但段头坏了"(截断 / layout_ver 不认识) —— looks_like_dzflat 对两者都返回
 * false, 而两者的处置完全不同。 */
inline bool has_dzflat_magic(const void* p, std::size_t size) noexcept
{
    if (p == nullptr || size < sizeof(std::uint32_t)) return false;
    std::uint32_t m = 0;
    std::memcpy(&m, p, sizeof(m));
    return m == kMagic;
}

/* 段首判别: 是 DZFlat 还是既有 TLV wire。 */
inline bool looks_like_dzflat(const void* p, std::size_t size) noexcept{
    if (p == nullptr || size < sizeof(SegHeader)) return false;
    SegHeader h{};
    std::memcpy(&h, p, sizeof(h));
    return h.magic == kMagic && h.layout_ver == kLayoutVer && h.total_size <= size
           && h.root_off == sizeof(SegHeader);
}

/* 绑定一个段: 校验 magic / 版本 / schema_hash / Root 落在段内。
 * 返回的 Reader 无效表示不可用(理由由调用方按需区分)。 */
inline Reader bind_segment(const void* p, std::size_t size, std::uint32_t expect_hash,
                           std::uint32_t root_size) noexcept
{
    if (!looks_like_dzflat(p, size)) return {};
    SegHeader h{};
    std::memcpy(&h, p, sizeof(h));
    if (h.schema_hash != expect_hash) return {};
    if (h.root_size != root_size) return {};
    if (h.total_size < sizeof(SegHeader) + root_size) return {};
    return Reader(static_cast<const std::uint8_t*>(p), h.total_size);
}

inline void write_header(std::uint8_t* seg, std::uint32_t schema_hash,
                         std::uint32_t root_size, std::uint32_t total_size,
                         std::uint32_t msg_id) noexcept
{
    SegHeader h{};
    h.magic = kMagic;
    h.schema_hash = schema_hash;
    h.root_off = sizeof(SegHeader);
    h.root_size = root_size;
    h.total_size = total_size;
    h.layout_ver = kLayoutVer;
    h.flags = 0;
    h.msg_id = msg_id;
    h.reserved = 0;
    std::memcpy(seg, &h, sizeof(h));
}

}   // namespace dzflat
