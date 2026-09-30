#pragma once
/* W02 统一跨进程基准 · 逻辑载荷与时间戳头定义（单一事实来源）
 * ============================================================================
 * 交付依据：方案 §4 W02 第 2 条「默认使用同一逻辑载荷，记录实际应用字节与传输
 * 字节」、§10.7「每条消息使用可校验的数据模式或序号相关校验，避免重复使用固定
 * 载荷掩盖旧帧和内容损坏」。
 *
 * 同一份逻辑载荷定义被三条后端共用：
 *   · TLV        —— TestMsg 对象（data2 里内联 8 槽时间戳头）
 *   · DZFlat A/B —— TestMsgFlat（同一批字段、同一批模式字节）
 *   · CycloneDDS —— w02::Sample（IDL 显式时间戳头 + 同构 sequence 字段）
 *
 * 时间戳头固定 32 字节（8 个 int32 槽 / DDS 侧 8+4+4+8+8）：
 *   槽 0-1  seq (u64, 低/高 32 位)          —— 生产序号
 *   槽 2    idx_flags = idx | flags<<16     —— 逐消息内序号 + 相位标志(探针/预热/测量)
 *   槽 3    crc32c(载荷)                    —— 全量读校验值(仅 build/verify 工作负载写)
 *   槽 4-5  produced_ns (u64)               —— 生成数据之前
 *   槽 6-7  publish_enter_ns (u64)          —— 发布 API 入口
 *
 * **不要混用结束点**（§10.7 三实验组）：transport_done_ns 由发布进程本地记录后
 * 按 seq 与订阅侧逐样本表合并 —— 它不可能随同一条消息一起送出去(发送返回时消息
 * 已经离开发布进程)，所以本文件不携带它，避免"把上一帧的完成时刻当成这一帧"的
 * 经典错误。
 */
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace w02 {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

/* 时间戳头槽位（dzIPC 侧占用 data2 的前 8 个 int32；DDS 侧是显式结构体字段）。 */
enum HeaderSlot : u32
{
    kSlotSeqLo = 0,
    kSlotSeqHi = 1,
    kSlotIdxFlags = 2,
    kSlotCrc = 3,
    kSlotProducedLo = 4,
    kSlotProducedHi = 5,
    kSlotEnterLo = 6,
    kSlotEnterHi = 7,
};
constexpr u32 kHdrSlots = 8;
constexpr u32 kHdrBytes = kHdrSlots * 4;   /* 32 B */

/* 相位标志，打进 idx_flags 的高 16 位。 */
enum : u32
{
    kFlagProbe = 1u,     /* 连通性探针：不计入统计 */
    kFlagWarmup = 2u,    /* 预热：不计入统计 */
    kFlagMeasure = 4u,   /* 测量窗口内的样本 */
};

/* 应用可获得的视图/对象形态（决定消费侧读法与工作负载，见 Workload）。 */
enum class WireView
{
    tlv_object,      /* 物化对象：TestMsg（整包反序列化克隆） */
    dzflat_view,     /* DZFlat 段只读视图：TestMsgFlatView（零拷贝 span/string_view） */
    dds_sample,      /* CycloneDDS 样本（reader loan，iceoryx 时不整段拷贝） */
};

/* 逻辑载荷形态。**同一 target 在所有后端得到同一 Shape** —— 这是"同一逻辑载荷"
 * 的机器判据（shape 进 config_hash 与 manifest）。
 *   n1     : data1 的 double 元素数
 *   n2     : data2 的**运行负载** int32 元素数（不含 8 槽时间戳头）
 *   n3     : data3 的 string 元素数
 *   str_len: 每个 string 的字节数
 *   bytes() : 应用逻辑载荷字节 = 8*n1 + 4*n2 + str_len*n3 + 1(bool)
 *             （时间戳头 32 B 按 §12 口径单列为 header_bytes，不混进应用字节）
 */
struct Shape
{
    u32 n1{0};
    u32 n2{0};
    u32 n3{0};
    u32 str_len{0};

    u32 bytes() const noexcept { return n1 * 8u + n2 * 4u + n3 * str_len + 1u; }
    /* dzIPC 侧 data2 的总元素数（头 8 槽 + 运行负载）。 */
    u32 dzipc_data2_size() const noexcept { return kHdrSlots + n2; }
};

/* 由目标**应用载荷**字节数推形态。
 *
 * 口径：target 是应用逻辑载荷（`bytes()` 的结果），时间戳头那 32 B **不计入** ——
 * 头是基准自己的记账结构，且在 summary/manifest 里单列（`header_bytes`）。
 * 早先版本从 target 里先扣掉头，于是"64 B 档"实测只有 32 B 应用载荷，比目标少一半，
 * 与方案 §6.2 的尺寸档口径对不上（reviewer 会直接问"为什么 64B 档是 32B"）。
 *
 * 三次取整（double 段 / string 段 / int 段）后实际字节与目标只差个位数；
 * 调用方仍必须输出**实际值** `payload_bytes`，不把目标当实测。 */
inline Shape compute_shape(u64 target_bytes)
{
    Shape s;
    const u64 rest = (target_bytes > 1u) ? (target_bytes - 1u) : 16u;
    const u64 d1 = (rest * 60u / 100u) / 8u;
    s.n1 = static_cast<u32>(d1 > 0 ? d1 : 1);
    u64 d3bytes = rest * 10u / 100u;
    if (d3bytes == 0) d3bytes = 1;
    s.str_len = static_cast<u32>(d3bytes > 64u ? 64u : d3bytes);
    s.n3 = static_cast<u32>(d3bytes / s.str_len);
    if (s.n3 == 0) s.n3 = 1;
    const u64 used = static_cast<u64>(s.n1) * 8u + static_cast<u64>(s.n3) * s.str_len;
    const u64 left = (rest > used) ? (rest - used) : 0u;
    s.n2 = static_cast<u32>(left / 4u);
    if (s.n2 == 0) s.n2 = 1;
    return s;
}

/* ---------------------------------------------------------------- 模式生成
 * 序号相关：同一位置在不同 seq 上的值不同，于是"收到上一帧/旧帧"必然被逐元素
 * 比对抓到；同时 CRC 覆盖全部载荷字节，抓到段内损坏。
 */
inline double pat_double(u64 seq, u32 i) noexcept
{
    u64 x = (seq * 0x9E3779B97F4A7C15ull) ^ (static_cast<u64>(i) * 0xBF58476D1CE4E5B9ull);
    x ^= x >> 29;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 32;
    return static_cast<double>(x % 1000003ull) / 1000.0;
}

inline i32 pat_int(u64 seq, u32 i) noexcept
{
    u32 x = static_cast<u32>(seq * 2654435761ull) + i * 40503u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return static_cast<i32>(x);
}

inline void pat_string(u64 seq, u32 k, u32 len, char* out) noexcept
{
    /* 可打印 ASCII，长度精确 len（不足处补 'x'）。 */
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    constexpr u32 kAlpha = 36;
    for (u32 i = 0; i < len; ++i)
    {
        const u32 x = static_cast<u32>(seq * 1315423911ull) + k * 2654435761u + i * 2246822519u;
        out[i] = alphabet[(x ^ (x >> 11)) % kAlpha];
    }
}

/* 逐元素校验 + CRC 累计。写侧与读侧调用同一组函数，保证口径一致。 */
struct PatternChecker
{
    u32 n1{0}, n2{0}, n3{0}, str_len{0};
    u64 seq{0};
    u64 elements_checked{0};
    u64 mismatches{0};
    u32 crc{0};
    std::string first_mismatch;

    void expect(const Shape& s, u64 sequence) noexcept
    {
        n1 = s.n1; n2 = s.n2; n3 = s.n3; str_len = s.str_len; seq = sequence;
        elements_checked = 0; mismatches = 0; crc = 0;
    }
};

}   // namespace w02
