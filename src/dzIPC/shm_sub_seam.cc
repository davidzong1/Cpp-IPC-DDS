/* 阶段 2 收包路径内部测试缝的实现。语义与设计约束见 shm_sub_seam.h。
 *
 * 落点在 src/dzIPC/ 下 ⇒ 被现有 aux_source_directory(${...}/src/dzIPC) 收编,
 * 无需改 src/CMakeLists.txt(与 shm_route_session.cc 同一条落点理由)。
 */
#include "dzIPC/detail/shm_sub_seam.h"

#include <atomic>

namespace dzIPC {
namespace detail {

namespace {

/* 用 std::atomic<SeamHook> 而不是裸指针: 测试可能在收包线程运行期间安装/卸载钩子
 * (析构用例就是在订阅端跑起来之后才装钩子的)。relaxed 足够 —— 钩子与事件之间没有
 * 需要跨线程同步的数据, 钩子自己负责它的同步(测试用例里是 mutex + condition_variable)。 */
std::atomic<SeamHook> g_hook{nullptr};

}   // namespace

void SetSeamHook(SeamHook hook) noexcept
{
    g_hook.store(hook, std::memory_order_release);
}

SeamHook GetSeamHook() noexcept
{
    return g_hook.load(std::memory_order_relaxed);
}

}   // namespace detail
}   // namespace dzIPC
