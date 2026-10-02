#pragma once
// 仅用于仓库外诊断构建；采样结果在各进程本地保存，退出后按序号关联。
#include <cstddef>
#include <cstdint>
namespace breakdown {
enum Stage { Produced, PublishEnter, SubmitBegin, PublishReturn, Received,
             LeaseReleased, Validated, Allocated, QueueVisible, TakeBegin, TakeEnd,
             App, WorkerEnter, Count };
void init();
void begin(std::uint64_t seq) noexcept;
void mark(Stage stage) noexcept;
void stamp(Stage stage, std::uint64_t ns) noexcept;
void worker_begin(std::uint64_t ns) noexcept;
void received(const void* data, std::size_t size) noexcept;
void dump(const char* path);
bool enabled() noexcept;
std::uint64_t now() noexcept;
}
