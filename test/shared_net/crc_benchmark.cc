// 同一二进制通过LD_LIBRARY_PATH分别加载优化前后库；仅测CRC调用，不替代端到端。
#include "dzIPC/net/wire_protocol.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>
int main() {
    using namespace dzIPC::net;
    volatile std::uint32_t sink = 0;
    std::cout << "{\"samples\":["; bool first = true;
    for (std::size_t size : {144u,1184u,1048656u}) {
        Bytes bytes(size); for (std::size_t i=0;i<size;++i) bytes[i] = (i * 17 + 13) & 255;
        const auto expected = crc32c(ByteView(bytes));
        const auto iterations = std::max<std::size_t>(64, std::min<std::size_t>(10000, (64u * 1024 * 1024) / size));
        for (unsigned warm=0;warm<16;++warm) sink ^= crc32c(ByteView(bytes));
        for (unsigned repeat=0;repeat<5;++repeat) {
            const auto start = std::chrono::steady_clock::now();
            for (std::size_t i=0;i<iterations;++i) sink ^= crc32c(ByteView(bytes));
            const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
            if (!first) std::cout << ','; first = false;
            std::cout << "{\"bytes\":" << size << ",\"iterations\":" << iterations << ",\"repeat\":" << repeat
                      << ",\"elapsed_ns\":" << ns << ",\"ns_per_call\":" << double(ns)/iterations << ",\"crc\":" << expected << '}';
        }
    }
    std::cout << "],\"sink\":" << sink << "}\n";
}
