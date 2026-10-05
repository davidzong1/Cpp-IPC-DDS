#pragma once
#include "dzIPC/net/wire_protocol.h"
#include <algorithm>
#include <cstring>

// 仅供先完成长度校验的协议模块使用。所有整数显式端序，不解引用未对齐整数。
namespace dzIPC::net::codec
{
inline std::uint64_t get(const std::uint8_t *p, unsigned n) noexcept
{
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v = (v << 8) | p[i];
    return v;
}
inline void put(std::uint8_t *p, std::uint64_t v, unsigned n) noexcept
{
    for (unsigned i = 0; i < n; ++i)
        p[n - 1 - i] = static_cast<std::uint8_t>(v >> (i * 8));
}
inline void append(Bytes &b, std::uint64_t n, unsigned width)
{
    const auto start = b.size();
    b.resize(start + width);
    put(b.data() + start, n, width);
}
inline bool valid(ByteView b) noexcept
{
    return b.data || b.size == 0;
}
inline bool zeros(const std::uint8_t *p, std::size_t n) noexcept
{
    for (std::size_t i = 0; i < n; ++i)
        if (p[i])
            return false;
    return true;
}
template <std::size_t N> inline void copy(std::array<std::uint8_t, N> &out, const std::uint8_t *p)
{
    std::copy_n(p, N, out.begin());
}
inline ProtocolStatus error(ProtocolCode c)
{
    return {c};
}
// CRC 字段归零，不分配临时整包副本。
std::uint32_t packet_crc(ByteView packet, std::size_t zero_offset) noexcept;
inline bool utf8(ByteView b)
{
    for (std::size_t i = 0; i < b.size;)
    {
        const auto c = b.data[i++];
        if (c < 128)
            continue;
        unsigned n;
        std::uint32_t cp, minimum;
        if (c >= 0xc2 && c <= 0xdf)
        {
            n = 1;
            cp = c & 31;
            minimum = 128;
        }
        else if (c >= 0xe0 && c <= 0xef)
        {
            n = 2;
            cp = c & 15;
            minimum = 2048;
        }
        else if (c >= 0xf0 && c <= 0xf4)
        {
            n = 3;
            cp = c & 7;
            minimum = 65536;
        }
        else
            return false;
        if (n > b.size - i)
            return false;
        while (n--)
        {
            const auto d = b.data[i++];
            if ((d & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (d & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}
} // namespace dzIPC::net::codec
