/* 步骤② 归因量测: chunk 池 32 块的占用到底是谁钉的
 * (docs/shm_chunk_pool_occupancy_plan.md §3 步骤②)
 *
 * 上位文档把这一步的产物定成一个**归因结论**: 池空发生时, 三个持有者谁占主导 ——
 *   ① 用户态队列 `view_queue_` / `msg_queue_`(每个未消费 Sample 钉一块)
 *   ② 环内在飞(已 loan 但订阅线程还没 recv 的)
 *   ③ 订阅线程回调中(瞬态)
 * 并给了硬闸门: **这一步之前不许动架构** —— 若主导者是②, 步骤④(解钉)全部白做。
 *
 * ⛔ 为什么必须扫 `queue_size` 而不是只跑默认值
 *    `CircularQueue::push_owned` 满时 drop-oldest **且永不阻塞**
 *    (include/dzIPC/common/circularqueue.h:91-113), 丢弃的 MsgPtr 析构即还池。
 *    于是队列是个**能吸收一切的汇**: `queue_size >= 32` 时 L 恒为 32, 队列主导与
 *    环主导给出**同一个读数**。唯一能分开两者的是 `queue_size < 32` —— 队列主导
 *    预测 L ≈ queue_size, 环主导预测 L ≈ 32 且与 queue_size 无关。
 *    ⇒ `--queue` 是主变量, 单点读数没有判别力。
 *
 * ⛔ 为什么必须在生产进行中连续采样, 不能读终态
 *    停发之后再读, 订阅线程会把环里剩下的条目继续 recv 进队列(drop-oldest 也在
 *    还池), 环必然被读空 —— 那是"我停了"造成的, 不是稳态。本基准在生产持续的
 *    采样窗内逐点取数, 报分布(p50/p95/max), 不报单点终值。
 *
 * ⛔ 尺寸档怎么算(2026-09-19 实测订正, 决定读哪个段)
 *    借样 chunk 的档位**不是** `calc_chunk_size(dzflat_size())` —— 借样路径有**两层**取整:
 *      ipc.cpp:1530  `cap = loan_size_class(size)`     // 先按 large_msg_align(1024) 向上取整
 *      ipc.cpp:510   `chunk_size = calc_chunk_size(cap)` // 再走一次 calc_chunk_size(又 +1024)
 *    ⇒ **档位 = calc_chunk_size(loan_size_class(dzflat_size()))**。
 *    实拍坐实: dzflat_size() = 7600 ⇒ cap = 8192 ⇒ 档位 = 9216。而按
 *    `calc_chunk_size(7600)` 会算成 8192 —— 那其实是 **TLV 回落**用的档
 *    (`no_member_send` 报 `size = 7716, chunk_size = 8192`), 读它会读到另一个池。
 *    两个池在同一个进程里同时存在, 读错了不会报错, 只会安静地量错对象。
 *
 * ⛔ 为什么默认 payload 取 11000
 *    档位 = ceil1024(D) + 1024(见上), 故 payload=D 要落在独占档就得避开别的用例:
 *    test_pool_exhaust_observability(7168/5120/3072/4096)、test_chunk_hold(5120/9216/13312)、
 *    test_uf007(13312) 已占。D = 11000 ⇒ dzflat_size ≈ 11096 ⇒ 档位 12288, 独占。
 *    同档池被别的用例钉干时, 本基准会把别人的占用读成自己的 —— 所以这条是硬要求。
 *
 * ── 两条独立通道(交叉验证, 不是"两条都非零"就算过)────────────────────────────
 *   通道 A(外部, 零产品码改动): 直读池段文件, 走一遍空闲链得 free, L = 32 - free。
 *   通道 B(插桩, 临时):         订阅循环里 store 的 view_queue_->size() / msg_queue_->size()。
 *   判据见文件末尾 report() 里的三条断言 —— 其中 Q_B <= min(queue_size,32) 用的是
 *   **静态属性**(队列容量), 与两条读法都无关, 所以它是一条真判据而不是自证。
 *
 * ⛔ 通道 A 的读法订正(2026-09-19): 上位文档 §2 写的 `L = 32 - cursor_` **符号反了**。
 *    `id_pool::cursor_` 是**空闲链表头**不是借出计数(src/libipc/utility/id_pool.h:51-88):
 *    fresh 池 `init()` 令 next_[i] = i+1(末尾 next_[31] = 32)且 cursor_ = 0 ⇒ fresh 态
 *    `32 - cursor_` 会给出 32(满占), 恰好读反; 而 release(id) 把 cursor_ 设成被释放的
 *    id, 于是它连"近似计数"都不是。正解是**沿 next_ 走链数空闲块**:
 *    `free = 从 cursor_ 出发走到 >= 32 的步数`, `L = 32 - free`。
 *    (docs/probe_teardown.cpp 的 `cursor_ == 32` 只在"一口气钉干、从不归还"的单调场景
 *     下与 free == 0 等价 —— 步骤① 的探针正好只在该场景用过它, 所以没暴露。)
 *
 * 用法: pool_attribution_benchmark --queue=2 --msgs=200000 --drain=0 --pairs=1
 * 输出: 一行机器可读的 `RESULT ...`, 供 docs/pool_attribution_run.sh 汇总成表。
 */
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/sample_message.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/def.h"
#include "libipc/shm.h"

/* ── 通道 B 的插桩符号 ──────────────────────────────────────────────────────
 * 由步骤② 的临时插桩提供(src/dzIPC/shm_pub_sub_ipc.cc, 见 docs/pool_attribution_instr.py)。
 * 声明成 **weak**: 干净构建下解析为 nullptr, 基准自动降级为"只有通道 A"并在 RESULT
 * 里打 instr=0 —— 这样同一个二进制在两种构建下都能跑, 不需要两套基准。
 * ⛔ 降级必须**可见**: 静默降级会让"通道 B 缺失"被读成"Q 恒为 0"。
 * 命名空间与插桩侧一致: dzIPC::shm::pa_instr —— 插桩侧**不能**放进 dzIPC::detail
 * 的同级名字里(会遮蔽该 TU 内既有的 detail::NoteDzFlatRx 调用), 详见驱动脚本
 * docs/pool_attribution_instr.py 里 HUNKS 的注释。 */
namespace dzIPC { namespace shm { namespace pa_instr {
extern std::atomic<std::size_t> g_pa_view_q __attribute__((weak));
extern std::atomic<std::size_t> g_pa_msg_q  __attribute__((weak));
}}}  // namespace dzIPC::shm::pa_instr

namespace {

using namespace std::chrono_literals;

/* 每档 chunk 池容量 = ipc::large_msg_cache(当前 40, include/libipc/def.h)。
 * ⛔ 由常量导出而非写死: 容量一变, 写死魔数会让判据失真(同 test_uf007 的教训)。
 * 池空即 cursor_ 走到底 —— 见 id_pool::empty()。 */
constexpr int kPoolCap = static_cast<int>(ipc::large_msg_cache);
constexpr std::size_t kLargeMsgAlign = 1024;
constexpr std::uint32_t kMsgId = 31;

/* 其他回归用例占用的尺寸类: test_pool_exhaust_observability(7168/5120/3072/4096)、
 * test_chunk_hold(5120/9216/13312)、test_uf007(13312)。本基准必须落在别的档位,
 * 否则同档池被那些用例钉干时会读成"本基准把池用满了"。 */
constexpr std::size_t kForeignClasses[] = {3072, 4096, 5120, 7168, 9216, 13312};

/* calc_chunk_size / loan_size_class 的本地副本(照抄 src/libipc/ipc.cpp:229-241 与 :402-419)。
 * ⛔ 抄写而非调用: 那两个函数在 ipc.cpp 的**匿名 namespace** 里, 没有对外链接。
 * 用途只是把 dzflat_size() 映射到段名。一旦上游改了取整规则, 这里算出的档位会指向
 * 一个不存在的段(read_pool_snapshot 的 free 恒为满), 于是 L 恒 0 —— 所以下面
 * **不靠**"段存不存在"兜底, 而是起始 fresh 检查 + 稳态 L_max 两道断言把它逼出来。 */
constexpr std::size_t align_up_to(std::size_t x, std::size_t a) noexcept
{
    return ((x + a - 1) / a) * a;
}
constexpr std::size_t calc_chunk_size(std::size_t size) noexcept
{
    return align_up_to(align_up_to(16 + size, kLargeMsgAlign),
                       alignof(std::max_align_t));
}
/* 借样路径的第一层取整(ipc.cpp:402, 只覆盖 <= 64KB 那一支 —— 本基准的负载远小于此)。 */
constexpr std::size_t loan_size_class(std::size_t size) noexcept
{
    return align_up_to(size, kLargeMsgAlign);
}

/* 借样 chunk 的档位 = 两层取整叠加。见文件头注释的实测订正。 */
constexpr std::size_t borrowed_chunk_class(std::size_t dzflat_size) noexcept
{
    return calc_chunk_size(loan_size_class(dzflat_size));
}

std::string pool_segment_path(std::size_t chunk_class)
{
    /* ⛔ 与 ipc.cpp get_info 的段名构造同步(含容量分量 __C<cap>)。 */
    return "/dev/shm/__IPC_SHM__CHUNK_INFO__" + std::to_string(chunk_class) +
           "__C" + std::to_string(static_cast<std::size_t>(ipc::large_msg_cache));
}

/* ── 通道 A: 直读池段, 走一遍空闲链 ──────────────────────────────────────────
 * 段是 tmpfs 文件, 进程 mmap 的同时可按文件读同一份内存(步骤① 已实测)。
 * 段内布局: chunk_info_t 首成员 id_pool<>, 其 next_[40] 占偏移 0..39(每项 1 字节,
 * 见 id_type<0,AlignSize>), cursor_ 在偏移 40 ⇒ 读 [0,41) 就是读整条空闲链。
 *
 * 段不存在 ⇒ 从未取过块 ⇒ 全空闲(libipc 的段是首次 acquire 时懒创建的, 建 route
 * 本身不建段 —— 步骤① §5 环境事实 1)。 */
struct PoolSnap
{
    int  free_n = kPoolCap;
    bool ok     = true;
    /* ⛔ 失败分两类, 必须分开数 —— 它们指向完全不同的结论:
     *   kLoop:     某次走链步数超过容量 ⇒ 链上存在环(自环或互环), 是**可证的坏了**。
     *   kUnstable: 所有尝试都没有超步, 只是"两次连续读不一致" ⇒ 链在动(churn 下的
     *              正常现象), 是**读法的采样窗口太短**, 不是被测对象有毛病。
     * 混成一个 badsnap 时, "读不到"会被读成"池坏了" —— 与步骤① 那条
     * "读数必须先证非退化" 同型。 */
    enum class Bad { kNone, kShort, kLoop, kUnstable };
    Bad  bad = Bad::kNone;
};

PoolSnap read_pool_snapshot(const std::string& path)
{
    /* ⛔ (容量+1) 字节的 fread 不是原子读, 而读写方在并发改这条链。为了把两种完全不同的
     * 情况分开, 这里重试并要求**连续两次结果一致**才采信:
     *   - 瞬时撕裂: 下一轮就一致了 ⇒ 正常采到数;
     *   - 持续性自环: 每一轮都失败 ⇒ 那不是读法问题, 是链本身坏了
     *     (`release(id)` 在 `cursor_ == id` 时会写出 `next_[id] = id`, 即同一 id 被
     *      释放两次 —— test_chunk_hold 头注释登记的"套圈重读致 id 重复入池")。
     * 不加重试的话这两种会被混成一个 badsnap 计数, 谁也说不清读数为什么没了。 */
    /* 尝试次数从 16 提到 64: churn 重时"两次连续一致"本身就要多试几次, 16 次不够
     * 会把"链在动"误判成失败(实测 queue=8/pub-extra=2 那几组 badsnap=60 里混着这种)。
     * (容量+1) 字节的读极便宜, 64 次的开销可忽略。 */
    constexpr int kAttempts = 64;
    PoolSnap last;
    bool have_last = false;
    int  loop_attempts = 0;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        PoolSnap s;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) return s;   // 段不存在 ⇒ fresh, 全空闲
        unsigned char b[kPoolCap + 1];
        const std::size_t got = std::fread(b, 1, sizeof(b), f);
        std::fclose(f);
        if (got < sizeof(b)) { s.ok = false; s.bad = PoolSnap::Bad::kShort; }
        else {
            /* 沿链走。⛔ 必须限步: 自环时 cursor_ 永远 < 容量, 不限步就是死循环 ——
             * 量具把被测进程挂住比读错更糟。步数超过容量即判本次读作废。 */
            unsigned cursor = b[kPoolCap];
            int n = 0;
            while (cursor < static_cast<unsigned>(kPoolCap) && n <= kPoolCap) {
                cursor = b[cursor];
                ++n;
            }
            if (n > kPoolCap) { s.ok = false; s.bad = PoolSnap::Bad::kLoop; ++loop_attempts; }
            else { s.free_n = n; s.ok = true; s.bad = PoolSnap::Bad::kNone; }
        }
        if (have_last && s.ok && last.ok && s.free_n == last.free_n) return s;
        last = s;
        have_last = true;
    }
    /* 走到这里 = 64 次都没采到"连续两次一致"。
     * 判据: 过半数尝试都超步 ⇒ 链是**持续**带环的(瞬时撕裂不可能连续多数次都成环);
     * 否则只是链在动。 */
    if (!last.ok) last.bad = (loop_attempts * 2 >= kAttempts) ? PoolSnap::Bad::kLoop
                                                              : PoolSnap::Bad::kUnstable;
    else last.bad = PoolSnap::Bad::kUnstable;   /* 读到过, 只是没连续一致 */
    last.ok = false;
    return last;
}

/* 该池段是否还有**别的进程**持有。清池(shm_unlink)会打断正在用它的进程, 所以
 * --reset 之前必须先过这一关(协议同 unfixed_defects.md §4)。 */
bool segment_has_live_holder(const std::string& segname)
{
    const pid_t self = ::getpid();
    for (int pid = 1; pid < 4194304; ++pid) {
        if (pid == self) continue;
        char dir[64];
        std::snprintf(dir, sizeof(dir), "/proc/%d/maps", pid);
        std::FILE* f = std::fopen(dir, "r");
        if (f == nullptr) continue;
        char line[4096];
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            /* maps 行: addr perms offset dev inode pathname [ (deleted)]
             * 取第 6 段做 basename 精确比较 —— 用子串匹配会让 8192 命中 81920。 */
            char* p = line;
            int field = 0;
            while (*p != '\0' && field < 5) {
                while (*p == ' ') ++p;
                while (*p != '\0' && *p != ' ' && *p != '\n') ++p;
                ++field;
            }
            while (*p == ' ') ++p;
            char* e = p;
            while (*e != '\0' && *e != '\n') ++e;
            *e = '\0';
            char* slash = std::strrchr(p, '/');
            const char* base = (slash != nullptr) ? slash + 1 : p;
            if (std::strcmp(base, segname.c_str()) == 0) {
                std::fclose(f);
                return true;
            }
        }
        std::fclose(f);
    }
    return false;
}

struct Args
{
    std::size_t queue     = 1024;
    std::size_t msgs      = 200000;  /* 所有发布线程合计的条数上限 */
    std::size_t pairs     = 1;       /* 独立 pub/sub 对的数量 */
    std::size_t payload   = 11000;   /* 图像 data 字节数(决定尺寸档, 见文件头) */
    std::size_t sample_ms = 5;
    std::size_t samples   = 60;
    std::size_t warmup_ms = 300;
    int         drain     = 0;
    std::size_t hold      = 0;       /* 应用侧长期持有的 Sample 数(通道 B 的灵敏度正对照) */
    std::size_t pub_extra = 0;       /* 额外的发布线程(环反臂: 生产快于消费时环内在飞是否上升) */
    int         no_pin    = 0;       /* 步骤③ 的 A/B: 关掉 view 队列容量钉, 复现"钉前"行为 */
    bool        reset     = false;
};

void usage()
{
    std::fprintf(stderr,
        "用法: pool_attribution_benchmark [选项]\n"
        "  --queue=N       订阅队列深度(主变量, 默认 1024)\n"
        "  --msgs=M        发布条数上限(所有发布线程合计, 默认 200000)\n"
        "  --drain=0|1     0=应用从不 try_get(队列当纯汇) 1=持续 drain(默认 0)\n"
        "  --pub-extra=N   在每对自带 1 个发布线程之外再加 N 个(默认 0)\n"
        "                  环反臂: 生产端压过订阅端时, R = L - Qv 是否上升\n"
        "  --hold=N        持续 drain, 但把前 N 个 Sample 一直留在应用侧(默认 0)\n"
        "                  用来证明 L - Qv 是活量: 应用持有的 chunk 不在任何队列里\n"
        "  --pairs=K       独立 pub/sub 对的数量(共享同一档池, 默认 1)\n"
        "  --no-pin=1      关掉步骤③ 的 view 队列容量钉\n"
        "                  钉默认 ON: view_queue 容量 = min(queue_size, ViewQueueCap()=8)。\n"
        "                  本开关复现\"钉前\"行为, 使 steps ③ 的 A/B 出自同一个二进制。\n"        "  --payload=N     图像 data 字节数(默认 7500)\n"
        "  --sample-ms=K   采样周期毫秒(默认 5)\n"
        "  --samples=K     采样点数(默认 60)\n"
        "  --warmup-ms=K   采样前预热毫秒(默认 300)\n"
        "  --reset=1       起始非 fresh 时清池(先过 /proc 活持有者检查)\n");
}

bool parse_u64(const char* s, std::size_t& out)
{
    if (s == nullptr || *s == '\0') return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s || *end != '\0') return false;
    out = static_cast<std::size_t>(v);
    return true;
}

/* ⛔ 手写解析, 不用 argparser.h: 它的 BOOL 类型会吞掉下一个 token
 * (-w -f 4 ⇒ 开关静默关闭且频率丢失, 见 memory: argparser-bool-swallows-next-token)。
 * 本基准的开关全是 --key=value 形态, substr + strtoull 足够且没有静默失败面。 */
bool parse_args(int argc, char** argv, Args& a)
{
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--help" || s == "-h") { usage(); std::exit(0); }
        const std::size_t eq = s.find('=');
        if (eq == std::string::npos || s.rfind("--", 0) != 0) {
            std::fprintf(stderr, "无法解析的参数: %s\n", s.c_str());
            return false;
        }
        const std::string k = s.substr(2, eq - 2);
        const char* v = s.c_str() + eq + 1;
        std::size_t n = 0;
        if      (k == "queue")     { if (!parse_u64(v, n)) return false; a.queue = n; }
        else if (k == "msgs")      { if (!parse_u64(v, n)) return false; a.msgs = n; }
        else if (k == "pairs")     { if (!parse_u64(v, n)) return false; a.pairs = n ? n : 1; }
        else if (k == "payload")   { if (!parse_u64(v, n)) return false; a.payload = n; }
        else if (k == "sample-ms") { if (!parse_u64(v, n)) return false; a.sample_ms = n ? n : 1; }
        else if (k == "samples")   { if (!parse_u64(v, n)) return false; a.samples = n ? n : 1; }
        else if (k == "warmup-ms") { if (!parse_u64(v, n)) return false; a.warmup_ms = n; }
        else if (k == "drain")     { if (!parse_u64(v, n)) return false; a.drain = n ? 1 : 0; }
        else if (k == "hold")      { if (!parse_u64(v, n)) return false; a.hold = n; }
        else if (k == "pub-extra") { if (!parse_u64(v, n)) return false; a.pub_extra = n; }
        else if (k == "no-pin")    { if (!parse_u64(v, n)) return false; a.no_pin = (n != 0) ? 1 : 0; }
        else if (k == "reset")     { if (!parse_u64(v, n)) return false; a.reset = (n != 0); }
        else { std::fprintf(stderr, "未知参数: %s\n", s.c_str()); return false; }
    }
    return true;
}

dzIPC::Msg::StdImage make_image(std::size_t data_bytes, std::uint8_t seed)
{
    dzIPC::Msg::StdImage img;
    img.header.frame_id = "camera";
    const std::size_t w = 64;
    img.width    = static_cast<std::uint32_t>(w);
    img.step     = static_cast<std::uint32_t>(w * 3);
    img.height   = static_cast<std::uint32_t>(data_bytes / (w * 3));
    img.encoding = "rgb8";
    img.data.assign(data_bytes, seed);   /* data.size() 恰为 data_bytes */
    return img;
}

struct Pair
{
    std::unique_ptr<dzIPC::shm::shm_pub_ipc> p;
    std::unique_ptr<dzIPC::shm::shm_sub_ipc> s;
    /* 声明的先后 = 析构的倒序: s 先亡, 订阅线程在 pub 还在时就停干净。 */
};

int pct(const std::vector<int>& sorted, double p)
{
    if (sorted.empty()) return -1;
    std::size_t i = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
    if (i >= sorted.size()) i = sorted.size() - 1;
    return sorted[i];
}

}  // namespace

int main(int argc, char** argv)
{
    Args a;
    if (!parse_args(argc, argv, a)) { usage(); return 2; }

    /* 步骤③ 的 A/B 开关: 必须在**任何订阅者构造之前**生效 —— 容量是构造时读的。
     * 放在这里而不是构造循环里, 是为了让"进程级开关"的语义显式且只有一处。 */
    if (a.no_pin != 0) { dzIPC::EnableViewQueuePin(false); }

    /* ── 尺寸档与段名 ──────────────────────────────────────────────────────── */
    const auto img = make_image(a.payload, 0x5A);
    std::shared_ptr<IpcMsgBase> probe =
        std::make_shared<dzIPC::Msg::StdImage>(img);
    const std::size_t need = probe->dzflat_size();
    if (need == 0) {
        std::fprintf(stderr, "dzflat_size() == 0 —— 本类型没走 DZFlat, 量不到借样占用\n");
        return 2;
    }
    const std::size_t cap = loan_size_class(need);
    const std::size_t cls = borrowed_chunk_class(need);
    for (std::size_t c : kForeignClasses) {
        if (c == cls) {
            std::fprintf(stderr,
                "尺寸档 %zu 被其他回归用例占用, 换 --payload"
                "(当前 need=%zu cap=%zu cls=%zu)\n", cls, need, cap, cls);
            return 2;
        }
    }
    const std::string segpath = pool_segment_path(cls);
    const std::string segname = "__IPC_SHM__CHUNK_INFO__" + std::to_string(cls) +
                                "__C" + std::to_string(static_cast<std::size_t>(ipc::large_msg_cache));

    /* ── 起始 fresh 检查(硬失败, 不许静默降级)──────────────────────────────
     * 残池会让"容量 32"这个前提消失, 之后所有读数都退化成恒真。
     * ⛔ 必须在任何 publish 之前做: 池段是懒创建的, 一旦本进程取过块, 这个检查就
     *    变成"自己刚建的空池必然 fresh" —— 那不是检查, 是自证。 */
    {
        PoolSnap s = read_pool_snapshot(segpath);
        if (!s.ok || s.free_n != kPoolCap) {
            if (!a.reset) {
                std::fprintf(stderr,
                    "池段非 fresh: %s free=%d%s\n"
                    "STALE_SEG=%s\n"
                    "(确认无活持有者后 rm 掉, 或加 --reset=1)\n",
                    segpath.c_str(), s.free_n, s.ok ? "" : "(读失败)", segpath.c_str());
                return 3;
            }
            if (segment_has_live_holder(segname)) {
                std::fprintf(stderr,
                    "⛔ 段 %s 仍有活持有者 —— 拒绝清池(会打断那些进程)\n", segname.c_str());
                return 5;
            }
            ipc::shm::handle::clear_storage(segname.c_str());
            PoolSnap s2 = read_pool_snapshot(segpath);
            if (!s2.ok || s2.free_n != kPoolCap) {
                std::fprintf(stderr, "清池后仍非 fresh(free=%d) —— 拒绝继续\n", s2.free_n);
                return 3;
            }
        }
    }

    /* ── 拓扑 ──────────────────────────────────────────────────────────────── */
    dzIPC::EnableDzFlat(true);
    dzIPC::ResetDzFlatCounters();
    dzIPC::ResetDzFlatRxCounters();

    std::vector<Pair> pairs(a.pairs);
    const std::string tag = std::to_string(::getpid());
    for (std::size_t i = 0; i < a.pairs; ++i) {
        const std::string topic = "/pool_attr/" + tag + "_" + std::to_string(i);
        auto pub_td = std::make_shared<dzIPC::TopicData>(
            std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        auto sub_td = std::make_shared<dzIPC::TopicData>(
            std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        /* ⚠️ 构造参数序陷阱: pub 第 4 参是 verbose, sub 第 4 参是 queue_size。 */
        pairs[i].p = std::make_unique<dzIPC::shm::shm_pub_ipc>(pub_td, topic, 0);
        pairs[i].s = std::make_unique<dzIPC::shm::shm_sub_ipc>(sub_td, topic, 0, a.queue);
        pairs[i].p->InitChannel();
        pairs[i].s->InitChannel();
    }

    /* 握手: 每对各自 pump 到收到第一个借样。同时把池段的懒创建触发掉。
     * 这里收到的 Sample 立刻释放 —— 预热期的占用不计入样本窗。 */
    for (std::size_t i = 0; i < a.pairs; ++i) {
        const auto deadline = std::chrono::steady_clock::now() + 4000ms;
        bool got = false;
        while (!got && std::chrono::steady_clock::now() < deadline) {
            auto m = std::make_shared<dzIPC::Msg::StdImage>(img);
            m->set_msg_id(kMsgId);
            pairs[i].p->publish(m);
            dzIPC::Sample s;
            got = pairs[i].s->try_get(s);
            std::this_thread::sleep_for(2ms);
        }
        if (!got) {
            std::fprintf(stderr, "第 %zu 对未在超时内收到借样 —— DZFlat 视图路径没通\n", i);
            return 4;
        }
    }
    if (dzIPC::DzFlatRxCounters().dzflat_accepted == 0) {
        std::fprintf(stderr, "dzflat_accepted == 0 —— 走的是 TLV 路径, 池占用不是借样造成的,"
                             "本次量测无效\n");
        return 4;
    }

    /* ── 并发三件: 发布 / (可选)drain / 采样 ─────────────────────────────── */
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> published{0};

    std::vector<std::thread> pub_threads;
    const std::size_t n_pub = a.pairs + a.pub_extra;
    pub_threads.reserve(n_pub);
    for (std::size_t i = 0; i < n_pub; ++i) {
        pub_threads.emplace_back([&, i] {
            auto* p = pairs[i % a.pairs].p.get();
            while (!stop.load(std::memory_order_relaxed)) {
                if (published.load(std::memory_order_relaxed) >= a.msgs) break;
                auto m = std::make_shared<dzIPC::Msg::StdImage>(img);
                m->set_msg_id(kMsgId);
                p->publish(m);
                published.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread consumer;
    /* 应用侧持有样本(通道 B 的灵敏度正对照, 见 usage)。held 是**应用**持有的
     * Sample, 它们钉住的 chunk **不在任何队列里** ⇒ 是 L - Qv 的直接来源。
     * 若 L - Qv 在 hold=N 下不动, 说明 R 是结构性零(量具看不到非队列持有),
     * 那样"R ≈ 0"就不是结论。 */
    std::vector<dzIPC::Sample> held;
    if (a.drain != 0 || a.hold != 0) {
        held.reserve(a.hold);
        consumer = std::thread([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                bool got = false;
                for (std::size_t i = 0; i < a.pairs; ++i) {
                    dzIPC::Sample s;   /* 未保留则本次迭代析构 ⇒ 立刻还池 */
                    if (pairs[i].s->try_get(s)) {
                        got = true;
                        if (held.size() < a.hold) held.push_back(std::move(s));
                    }
                }
                if (!got) std::this_thread::sleep_for(1ms);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(a.warmup_ms));

    const bool instr = (reinterpret_cast<const void*>(
                            &dzIPC::shm::pa_instr::g_pa_view_q) != nullptr);

    /* ⛔ 插桩的两个计数器是**进程级全局**, 每个订阅线程都往同一对里写。
     * pairs > 1 时 Q 变成"最后写的那个订阅者"的深度 —— 读出来毫无意义,
     * 而它看起来仍是个正常数字(不会报错)。这条必须硬失败, 不能静默降级。 */
    if (instr && a.pairs > 1) {
        std::fprintf(stderr, "⛔ 通道 B(插桩)只在 --pairs=1 下有效: 两个订阅线程写同一对\n"
                             "   进程级计数器, Q 会变成竞态里的最后一个值。请改用 --pairs=1。\n");
        return 2;
    }

    std::vector<int> Ls, Qs, Qms, Rs;
    Ls.reserve(a.samples);
    Qs.reserve(a.samples);
    Qms.reserve(a.samples);
    Rs.reserve(a.samples);
    int bad_snaps = 0;   /* 无效样本总数(bad_loop + bad_unstable) */
    int bad_loop  = 0;   /* 其中"链持续带环" —— 可证的坏链 */
    int bad_unstab= 0;   /* 其中"只是链在动" —— 采样窗口问题, 不是被测对象有毛病 */
    int q_gt_l    = 0;   /* 通道 B 读数 > 通道 A 读数的样本数(交叉验证的硬失败计数) */

    for (std::size_t k = 0; k < a.samples; ++k) {
        const PoolSnap s = read_pool_snapshot(segpath);
        if (!s.ok) {
            ++bad_snaps;
            if (s.bad == PoolSnap::Bad::kLoop) ++bad_loop;
            else                               ++bad_unstab;
        }
        else {
            const int L = kPoolCap - s.free_n;
            Ls.push_back(L);
            if (instr) {
                /* ⛔ Q 只取 **view 队列**。2026-09-19 实测(插桩)坐实 msg 队列不钉 chunk:
                 * 两条队列容量同为 queue_size, 若都钉则 L 应为 2*queue_size; 实测
                 * queue=16 ⇒ L=16(Qv=16, Qm=16) 而非 32 ⇒ 只有 view 队列钉。
                 * 机理: TLV 回落走 AcceptWire **物化**(段字节拷进消息自己的 vector),
                 * 而 dzflat_adopt 那条(schema-less 话题)才是借样 make_shared<ipc::buffer>。
                 * 本基准用 typed 话题 ⇒ 走 view 分支 ⇒ Qm 恒为纯计数。
                 * 把 Qm 计进 Q 会得到 Q = 2*queue_size > L, 触发假的 Q<=L 失败。 */
                const int qv = static_cast<int>(dzIPC::shm::pa_instr::g_pa_view_q
                                                    .load(std::memory_order_relaxed));
                const int qm = static_cast<int>(dzIPC::shm::pa_instr::g_pa_msg_q
                                                    .load(std::memory_order_relaxed));
                Qs.push_back(qv);
                Qms.push_back(qm);
                Rs.push_back(L - qv);
                /* 交叉验证判据 1(逐样本, 不是只比百分位): 队列持有的不可能多于池外总数。
                 * Qv > L ⇒ 必有一条通道读错(队列数错 / 段读错 / 相位错到不可能的程度)。 */
                if (qv > L) ++q_gt_l;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(a.sample_ms));
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : pub_threads) t.join();
    if (consumer.joinable()) consumer.join();

    const auto rx = dzIPC::DzFlatRxCounters();

    std::vector<int> Ls_s(Ls), Qs_s(Qs), Qms_s(Qms), Rs_s(Rs);
    std::sort(Ls_s.begin(), Ls_s.end());
    std::sort(Qs_s.begin(), Qs_s.end());
    std::sort(Qms_s.begin(), Qms_s.end());
    std::sort(Rs_s.begin(), Rs_s.end());

    char qbuf[224];
    if (instr && !Qs_s.empty()) {
        /* Qv = view 队列(钉 chunk); Qm = msg 队列(不钉, 仅信息, 见上面的 ⛔) */
        std::snprintf(qbuf, sizeof(qbuf),
                      "Q_min=%d Q_p50=%d Q_p95=%d Q_max=%d Qm_p50=%d",
                      Qs_s.front(), pct(Qs_s, 0.50), pct(Qs_s, 0.95), Qs_s.back(),
                      pct(Qms_s, 0.50));
    } else {
        std::snprintf(qbuf, sizeof(qbuf), "Q_min=NA Q_p50=NA Q_p95=NA Q_max=NA Qm_p50=NA");
    }
    char rbuf[96];
    if (instr && !Rs_s.empty()) {
        std::snprintf(rbuf, sizeof(rbuf), "R_p50=%d R_max=%d", pct(Rs_s, 0.50), Rs_s.back());
    } else {
        std::snprintf(rbuf, sizeof(rbuf), "R_p50=NA R_max=NA");
    }

    std::printf("RESULT queue=%zu drain=%d pairs=%zu payload=%zu need=%zu cap=%zu cls=%zu "
                "fresh=1 nsamp=%zu badsnap=%d badloop=%d badunstab=%d "
                "L_min=%d L_p50=%d L_p95=%d L_max=%d "
                "%s %s "
                "instr=%d qgtl=%d pin=%d vcap=%zu pubs=%llu fb=%llu rx_acc=%llu rx_def=%llu\n",
                a.queue, a.drain, a.pairs, a.payload, need, cap, cls,
                Ls_s.size(), bad_snaps, bad_loop, bad_unstab,
                Ls_s.empty() ? -1 : Ls_s.front(),
                pct(Ls_s, 0.50), pct(Ls_s, 0.95),
                Ls_s.empty() ? -1 : Ls_s.back(),
                qbuf, rbuf,
                instr ? 1 : 0, q_gt_l,
                dzIPC::IsViewQueuePinEnabled() ? 1 : 0,
                dzIPC::ViewQueueCap(),
                static_cast<unsigned long long>(published.load()),
                static_cast<unsigned long long>(dzIPC::DzFlatFallbackCount()),
                static_cast<unsigned long long>(rx.dzflat_accepted),
                static_cast<unsigned long long>(rx.defects()));

    /* ⛔ 量具自证: 样本太少说明读法或档位算错了(例如档位漂移后读到不存在的段 —— 那时
     * 每次 fopen 都失败, 会静默变成 L=0 而不是报错)。这里必须硬失败, 不许把
     * "读不到" 输出成 "池没被占"。 */
    if (Ls_s.size() * 2 < a.samples) {
        std::fprintf(stderr,
            "⛔ 有效样本 %zu/%zu 过少(badsnap=%d = 链带环 %d + 链在动 %d) ——\n"
            "   读法或尺寸档不可信, 本次读数作废\n",
            Ls_s.size(), a.samples, bad_snaps, bad_loop, bad_unstab);
        return 6;
    }
    /* ⛔ 交叉验证判据 1 的硬失败。单独 exit code 以便驱动脚本区分
     * "通道对不上" 与 "样本不够"。 */
    if (q_gt_l > 0) {
        std::fprintf(stderr, "⛔ 交叉验证失败: %d 个样本 Qv > L —— 两条通道对不上,"
                             "读数作废(不是结论)\n", q_gt_l);
        return 7;
    }
    return 0;
}
