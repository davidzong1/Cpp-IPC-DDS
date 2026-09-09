/* DZFlat 离线基准 (docs/dzflat_shm.md Step 1 的验收门)
 *
 * 只比较**布局本身**的代价, 不碰传输层: 这是在承诺 libipc/dzIPC 改造之前先把
 * §1.3 的数量级坐实。三项指标: wire 字节数 / 堆分配次数 / 编解码耗时。
 *
 * 口径说明(重要, 否则数字会被误读):
 *   编码 —— TLV 的 serialize() 自带一次整包 new; DZFlat 写进调用方给的缓冲(生产里
 *           那就是 chunk), 所以用预分配缓冲, 不计入分配。
 *   解码 —— 分三项列出, 别只看一项:
 *             TLV deserialize   : 把整包拆进 owning struct(一次全量拷贝);
 *             DZFlat bind       : 只校验段头, O(1);
 *             DZFlat copy_to    : 拷回 owning struct, 与 TLV deserialize 同口径;
 *           另单列 "遍历全量" —— 消费者真正读一遍数据的代价。TLV 消费者要付
 *           "deserialize + 遍历私有内存", DZFlat 消费者只付 "bind + 遍历共享内存"。
 *
 * 每项跑 kReps 次取平均; 分配次数按次归一。
 */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_msg/std_msgs/std_point_cloud.hpp"

/* ---------------------------------------------------------------- 分配计数器 */
namespace {
std::size_t g_allocs = 0;
bool g_counting = false;
constexpr int kReps = 5;
}   // namespace

void* operator new(std::size_t n)
{
    if (g_counting) ++g_allocs;
    void* p = std::malloc(n ? n : 1);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n)
{
    if (g_counting) ++g_allocs;
    void* p = std::malloc(n ? n : 1);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

/* 优化屏障: 遍历循环的结果必须"被用掉", 否则 -O2 会把整个循环消掉, 于是"读全量"
 * 一项测出 0 us, 端到端比值变成无意义的天文数字。 */
template<typename T>
inline void keep(const T& v)
{
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "r,m"(v) : "memory");
#else
    static volatile T sink_;
    sink_ = v;
#endif
}

struct Metric
{
    double us = 0.0;      /* 每次平均 */
    double allocs = 0.0;  /* 每次平均 */
};

/* 把 fn 跑 kReps 次, 返回单次平均耗时与单次平均分配次数。
 * 计数器不嵌套: 每个 measure() 自己开关, 调用方不得在 fn 内再 measure。 */
template<typename F>
Metric measure(F&& fn)
{
    fn();   /* 预热一次, 不计入 */
    g_allocs = 0;
    g_counting = true;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kReps; ++i) fn();
    const auto t1 = std::chrono::steady_clock::now();
    g_counting = false;
    Metric m;
    m.us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kReps;
    m.allocs = double(g_allocs) / kReps;
    return m;
}

void row(const char* label, const Metric& m)
{
    std::printf("  %-30s %11.1f us   %9.1f 次分配\n", label, m.us, m.allocs);
}

double ratio(double a, double b) { return b > 0.0 ? a / b : 0.0; }

/* ------------------------------------------------------------------ std_image */
void bench_image(std::size_t w, std::size_t h)
{
    dzIPC::Msg::StdImage img;
    img.header.frame_id = "camera_optical_frame";
    img.header.stamp = 1234.5678;
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.step = static_cast<std::uint32_t>(w * 3);
    img.encoding = "rgb8";
    img.data.resize(w * h * 3);
    for (std::size_t i = 0; i < img.data.size(); ++i)
        img.data[i] = static_cast<std::uint8_t>(i & 0xFF);
    const std::size_t payload = img.data.size();

    std::printf("\n=== StdImage %zux%zu rgb8 (负载 %.2f MB) ===\n", w, h,
                double(payload) / (1024.0 * 1024.0));

    /* ---- TLV ---- */
    ipc::buffer tlv;
    const Metric enc_tlv = measure([&] { tlv = img.serialize(); });
    const std::size_t tlv_bytes = tlv.size();

    dzIPC::Msg::StdImage tlv_back;
    const Metric dec_tlv = measure([&] { tlv_back.deserialize(tlv); });

    const Metric trav_owned = measure([&] {
        std::uint64_t s = 0;
        for (std::size_t i = 0; i < tlv_back.data.size(); ++i) s += tlv_back.data[i];
        keep(s);
    });

    /* ---- DZFlat ---- */
    const std::uint32_t cap = dzIPC::Msg::StdImageFlat::size(img);
    std::vector<std::uint8_t> seg(cap);   /* 生产里这块就是 chunk */
    const Metric enc_flat =
        measure([&] { dzIPC::Msg::StdImageFlat::write(img, seg.data(), cap); });

    const auto* sh = reinterpret_cast<const dzflat::SegHeader*>(seg.data());
    const std::uint32_t flat_bytes = sh->total_size;

    const Metric dec_bind = measure([&] {
        auto v = dzIPC::Msg::StdImageView::bind(seg.data(), flat_bytes);
        keep(v.valid());
    });

    auto view = dzIPC::Msg::StdImageView::bind(seg.data(), flat_bytes);
    if (!view.valid())
    {
        std::printf("  !! DZFlat bind 失败\n");
        return;
    }
    const Metric trav_view = measure([&] {
        auto px = view.data();
        std::uint64_t s = 0;
        for (std::uint32_t i = 0; i < px.size(); ++i) s += px[i];
        keep(s);
    });

    dzIPC::Msg::StdImage flat_back;
    const Metric dec_copy = measure([&] { view.copy_to(flat_back); });

    /* ---- 正确性 ---- */
    const bool ok_tlv = (tlv_back.data == img.data && tlv_back.encoding == img.encoding
                         && tlv_back.header.frame_id == img.header.frame_id);
    const bool ok_flat = (flat_back.data == img.data && flat_back.encoding == img.encoding
                          && flat_back.header.frame_id == img.header.frame_id
                          && flat_back.header.stamp == img.header.stamp
                          && flat_back.width == img.width && flat_back.step == img.step);
    const bool ok_view = (view.width() == img.width && view.encoding() == img.encoding
                          && view.header().frame_id() == img.header.frame_id
                          && view.data().size() == payload);
    std::printf(" 往返一致: TLV=%s  DZFlat.copy_to=%s  DZFlat.view=%s\n",
                ok_tlv ? "ok" : "**FAIL**", ok_flat ? "ok" : "**FAIL**",
                ok_view ? "ok" : "**FAIL**");

    std::printf(" 编码:\n");
    row("TLV serialize", enc_tlv);
    row("DZFlat write", enc_flat);
    std::printf(" 解码:\n");
    row("TLV deserialize", dec_tlv);
    row("DZFlat bind (O(1))", dec_bind);
    row("DZFlat copy_to (同口径)", dec_copy);
    std::printf(" 消费者读全量:\n");
    row("遍历 owning struct (TLV 后)", trav_owned);
    row("遍历 view (共享内存原地)", trav_view);
    std::printf(" wire: TLV %zu B  vs  DZFlat %u B   (膨胀 %.4fx vs %.4fx, 负载 %zu B)\n",
                tlv_bytes, flat_bytes, ratio(double(tlv_bytes), double(payload)),
                ratio(double(flat_bytes), double(payload)), payload);
    std::printf(" 编码 %.1fx 更快; 消费者端到端(解码+读全量) TLV %.1f us vs DZFlat %.1f us"
                " → %.2fx\n",
                ratio(enc_tlv.us, enc_flat.us), dec_tlv.us + trav_owned.us,
                dec_bind.us + trav_view.us,
                ratio(dec_tlv.us + trav_owned.us, dec_bind.us + trav_view.us));
    std::fflush(stdout);
}

/* ------------------------------------------------------------ std_point_cloud */
void bench_cloud(std::size_t n)
{
    dzIPC::Msg::StdPointCloud pc;
    pc.header.frame_id = "lidar_top";
    pc.header.stamp = 42.0;
    pc.points.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        pc.points[i].data = {double(i), double(i) * 2.0, double(i) * 3.0};
    pc.channel_names = {"intensity", "ring"};
    pc.channels.resize(n);
    for (std::size_t i = 0; i < n; ++i) pc.channels[i] = double(i) * 0.5;
    const std::size_t payload = n * 3 * sizeof(double) + n * sizeof(double);

    std::printf("\n=== StdPointCloud %zu 点 (StdVector3d[] 的元素是 Tier-0) ===\n", n);

    ipc::buffer tlv;
    const Metric enc_tlv = measure([&] { tlv = pc.serialize(); });
    const std::size_t tlv_bytes = tlv.size();

    dzIPC::Msg::StdPointCloud tlv_back;
    const Metric dec_tlv = measure([&] { tlv_back.deserialize(tlv); });

    const Metric trav_owned = measure([&] {
        double s = 0.0;
        for (std::size_t i = 0; i < tlv_back.points.size(); ++i)
            s += tlv_back.points[i].data[0] + tlv_back.points[i].data[1]
                 + tlv_back.points[i].data[2];
        keep(s);
    });

    const std::uint32_t cap = dzIPC::Msg::StdPointCloudFlat::size(pc);
    std::vector<std::uint8_t> seg(cap);
    const Metric enc_flat =
        measure([&] { dzIPC::Msg::StdPointCloudFlat::write(pc, seg.data(), cap); });

    const auto* sh = reinterpret_cast<const dzflat::SegHeader*>(seg.data());
    const std::uint32_t flat_bytes = sh->total_size;

    const Metric dec_bind = measure([&] {
        auto v = dzIPC::Msg::StdPointCloudView::bind(seg.data(), flat_bytes);
        keep(v.valid());
    });

    auto view = dzIPC::Msg::StdPointCloudView::bind(seg.data(), flat_bytes);
    if (!view.valid())
    {
        std::printf("  !! DZFlat bind 失败\n");
        return;
    }
    /* Tier-0 元素数组在变长区里就是一段连续的 C 数组, 整体零拷贝可用。 */
    const Metric trav_view = measure([&] {
        auto raw = view.points_raw();
        double s = 0.0;
        for (std::uint32_t i = 0; i < raw.size(); ++i)
            s += raw[i].data[0] + raw[i].data[1] + raw[i].data[2];
        keep(s);
    });

    dzIPC::Msg::StdPointCloud flat_back;
    const Metric dec_copy = measure([&] { view.copy_to(flat_back); });

    const bool ok_tlv = (tlv_back.points.size() == n
                         && tlv_back.points[n / 2].data == pc.points[n / 2].data
                         && tlv_back.channel_names == pc.channel_names
                         && tlv_back.channels == pc.channels);
    const bool ok_flat = (flat_back.points.size() == n
                          && flat_back.points[n / 2].data == pc.points[n / 2].data
                          && flat_back.channel_names == pc.channel_names
                          && flat_back.channels == pc.channels
                          && flat_back.header.frame_id == pc.header.frame_id);
    const bool ok_view = (view.points_count() == n && view.points_raw().size() == n
                          && view.points(n / 2).data().data()[1] == pc.points[n / 2].data[1]
                          && view.channel_names_count() == pc.channel_names.size()
                          && view.channel_names(1) == pc.channel_names[1]);
    std::printf(" 往返一致: TLV=%s  DZFlat.copy_to=%s  DZFlat.view=%s\n",
                ok_tlv ? "ok" : "**FAIL**", ok_flat ? "ok" : "**FAIL**",
                ok_view ? "ok" : "**FAIL**");

    std::printf(" 编码:\n");
    row("TLV serialize", enc_tlv);
    row("DZFlat write", enc_flat);
    std::printf(" 解码:\n");
    row("TLV deserialize", dec_tlv);
    row("DZFlat bind (O(1))", dec_bind);
    row("DZFlat copy_to (同口径)", dec_copy);
    std::printf(" 消费者读全量:\n");
    row("遍历 owning struct (TLV 后)", trav_owned);
    row("遍历 view (共享内存原地)", trav_view);
    std::printf(" wire: TLV %zu B  vs  DZFlat %u B   (膨胀 %.4fx vs %.4fx, 负载 %zu B)\n",
                tlv_bytes, flat_bytes, ratio(double(tlv_bytes), double(payload)),
                ratio(double(flat_bytes), double(payload)), payload);
    std::printf(" 编码 %.1fx 更快; 编码分配次数 %.0f → %.0f\n",
                ratio(enc_tlv.us, enc_flat.us), enc_tlv.allocs, enc_flat.allocs);
    std::printf(" 消费者端到端(解码+读全量) TLV %.1f us vs DZFlat %.1f us → %.2fx\n",
                dec_tlv.us + trav_owned.us, dec_bind.us + trav_view.us,
                ratio(dec_tlv.us + trav_owned.us, dec_bind.us + trav_view.us));
    std::fflush(stdout);
}

}   // namespace

int main(int argc, char** argv)
{
    std::size_t points = 100000;
    if (argc > 1) points = std::strtoul(argv[1], nullptr, 10);

    std::printf("DZFlat 离线基准 (docs/dzflat_shm.md Step 1), 每项 %d 次取平均\n", kReps);
    std::printf("Root 尺寸: StdHeaderRoot=%zu StdImageRoot=%zu StdVector3dRoot=%zu "
                "StdPointCloudRoot=%zu\n",
                sizeof(dzIPC::Msg::StdHeaderRoot), sizeof(dzIPC::Msg::StdImageRoot),
                sizeof(dzIPC::Msg::StdVector3dRoot), sizeof(dzIPC::Msg::StdPointCloudRoot));
    std::printf("schema_hash: image=%08X cloud=%08X   kRootTight: image=%d cloud=%d v3=%d\n",
                dzIPC::Msg::StdImageFlat::kSchemaHash,
                dzIPC::Msg::StdPointCloudFlat::kSchemaHash,
                int(dzIPC::Msg::StdImageFlat::kRootTight),
                int(dzIPC::Msg::StdPointCloudFlat::kRootTight),
                int(dzIPC::Msg::StdVector3dFlat::kRootTight));

    bench_image(640, 480);
    bench_image(1920, 1080);
    bench_cloud(points);
    std::printf("\n");
    return 0;
}
