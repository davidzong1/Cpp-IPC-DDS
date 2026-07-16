#include "dzIPC/common/crc32c.h"

#include <array>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <nmmintrin.h>  // SSE4.2 _mm_crc32_u64
#endif

namespace dzIPC {
namespace common {
namespace {

constexpr uint32_t kCrc32cPolynomial = 0x82F63B78u;

std::array<uint32_t, 256> make_table()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i)
    {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1u) ? kCrc32cPolynomial : 0u);
        }
        table[i] = crc;
    }
    return table;
}

const std::array<uint32_t, 256>& crc32c_table()
{
    static const std::array<uint32_t, 256> table = make_table();
    return table;
}

uint32_t crc32c_sw(const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    const auto& table = crc32c_table();
    uint32_t crc = 0xFFFF'FFFFu;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc = (crc >> 8) ^ table[(crc ^ bytes[i]) & 0xFFu];
    }
    return ~crc;
}

// ---- Hardware-accelerated SSE4.2 path ----
// Uses _mm_crc32_u8 / _mm_crc32_u64 intrinsics which map directly to the
// CRC32C instruction (polynomial 0x1EDC6F41, same as Castagnoli).
// Benchmark: ~8-12x faster than SW LUT for payloads > 1KB on modern x86.

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)

// Clang & GCC support function multiversioning / target attributes.
// Separate the HW path so the compiler can emit SSE4.2 instructions
// without requiring -msse4.2 globally.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sse4.2")))
#endif
uint32_t crc32c_hw(const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFF'FFFFu;

    // Process 8-byte chunks with _mm_crc32_u64 for maximum throughput.
    std::size_t offset = 0;
    for (; offset + 8 <= size; offset += 8)
    {
        uint64_t word;
        std::memcpy(&word, bytes + offset, sizeof(word));
        crc = static_cast<uint32_t>(_mm_crc32_u64(crc, word));
    }

    // Process remaining bytes with _mm_crc32_u8.
    for (; offset < size; ++offset)
    {
        crc = _mm_crc32_u8(crc, bytes[offset]);
    }

    return ~crc;
}

bool has_sse42()
{
    // __builtin_cpu_supports is available since GCC 4.8 / Clang 3.6.
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("sse4.2");
#else
    return false;
#endif
}

#endif // x86/x64

// Select the best available implementation once at startup.
using crc32c_fn_t = uint32_t (*)(const void*, std::size_t);

crc32c_fn_t resolve_crc32c_impl()
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if (has_sse42())
    {
        return crc32c_hw;
    }
#endif
    return crc32c_sw;
}

}   // namespace

uint32_t crc32c(const void* data, std::size_t size)
{
    if (data == nullptr && size != 0)
    {
        return 0;
    }

    static const crc32c_fn_t impl = resolve_crc32c_impl();
    return impl(data, size);
}

}   // namespace common
}   // namespace dzIPC
