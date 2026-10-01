/* T06 / W12 §8 复核探针（纯读取器，⛔ 不链接 libipc、⛔ 不修改任何产品代码）。
 *
 * 用途：在**不调用任何产品 API** 的前提下，直接量出被测段的真实状态，作为
 * 「被测路径确实执行 / 未执行」的**独立证据**（方案 §5.4 的教训：不得每轮
 * clear_storage 导致被测路径首行早退 → 连负控都绿）。
 *
 * 段内布局（与 docs/shm_chunk_pool_occupancy_plan.md §3 步骤② 一致）：
 *   chunk_info_t 的首成员 = id_pool<>（alignof = 1）⇒ 文件头 capacity 字节是
 *   next_[0..capacity-1]，紧随其后 1 字节是 cursor_。
 *   · 全新段（zero-filled）：next_[i] == 0 (i≠0) 且 next_[0] == 0、cursor_ == 0
 *     ⇒ 这正是 id_pool::invalid() 的判据（与本进程 static 零值实例逐字节相等）
 *     ⇒ reclaim_orphan_segment 首行 `if (!orphan_candidate)` 早退。
 *   · 已用过的段：空闲链被头插改动过，或 cursor_ != 0 ⇒ 非素净。
 *
 * 子命令：
 *   zero   <path>            逐字节判"是否全 0"，并打印前 48 字节
 *   state  <path>            打印 cursor_、链长、i==next_[i] 自环项、前 48 字节
 *   probe  <path> <off> <len> 按 8 字节解析 [off, off+len) 里的 u64(小端)与 u32(id)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) { printf("PROBE_ERR open_failed path=%s errno=%d\n", path, errno); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *b = (unsigned char *)malloc((size_t)n);
    if (b == NULL) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(b); return NULL; }
    *out_n = (size_t)n;
    return b;
}

static unsigned long long u64le(const unsigned char *p)
{
    unsigned long long v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
static unsigned long u32le(const unsigned char *p)
{
    unsigned long v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

static void hex48(const unsigned char *b, size_t n)
{
    size_t m = n < 48 ? n : 48;
    printf("PROBE_HEAD48");
    for (size_t i = 0; i < m; ++i) printf(" %02x", b[i]);
    printf("\n");
}

static int cmd_zero(const char *path)
{
    size_t n = 0;
    unsigned char *b = slurp(path, &n);
    if (b == NULL) return 1;
    size_t first = n;
    for (size_t i = 0; i < n; ++i) if (b[i] != 0) { first = i; break; }
    printf("PROBE_ZERO path=%s size=%zu all_zero=%d first_nonzero_off=%zd\n", path, n,
           (first == n) ? 1 : 0, (first == n) ? (long)-1 : (long)first);
    hex48(b, n);
    free(b);
    return 0;
}

static int cmd_state(const char *path, size_t cap)
{
    size_t n = 0;
    unsigned char *b = slurp(path, &n);
    if (b == NULL) return 1;
    if (n < cap + 1) { printf("PROBE_ERR too_short size=%zu need=%zu\n", n, cap + 1); free(b); return 1; }
    unsigned cursor = b[cap];
    size_t steps = 0;
    unsigned cur = cursor;
    size_t self_loops = 0;
    int broken = 0;
    while (cur < cap && steps <= cap) { cur = b[cur]; ++steps; }
    if (steps > cap) broken = 1;
    for (size_t i = 0; i < cap; ++i) if (b[i] == (unsigned char)i) ++self_loops;
    int all_zero = 1;
    for (size_t i = 0; i < n; ++i) if (b[i] != 0) { all_zero = 0; break; }
    /* chain_len 上限截到 cap：自环时 steps 会到 cap+1，那只是哨兵值，
     * 真实信息在 chain_broken 上（否则读数会被误读成"41 个空闲块 > 容量 40"）。 */
    printf("PROBE_STATE path=%s size=%zu cap=%zu cursor=%u chain_len=%zu chain_broken=%d "
           "self_loops=%zu all_zero=%d idle_when_exhausted=%d\n",
           path, n, cap, cursor, (steps > cap) ? cap : steps, broken, self_loops, all_zero,
           (steps == cap) ? 1 : 0);
    hex48(b, n);
    free(b);
    return 0;
}

/* 打印空闲链的完整字节镜像（可看出自环/短缺/重复可达 id）。 */
static int cmd_chain(const char *path, size_t cap)
{
    size_t n = 0;
    unsigned char *b = slurp(path, &n);
    if (b == NULL) return 1;
    if (n < cap + 1) { printf("PROBE_ERR too_short\n"); free(b); return 1; }
    printf("PROBE_CHAIN path=%s cap=%zu cursor=%u next=[", path, cap, b[cap]);
    for (size_t i = 0; i < cap; ++i) printf("%s%u", i ? "," : "", b[i]);
    printf("]\n");
    /* 可达集：沿链走，记录访问到的 id（用于判断"是否所有 40 个 id 都可达"） */
    unsigned cur = b[cap];
    size_t steps = 0;
    unsigned char seen[256] = {0};
    while (cur < cap && steps <= cap + 1 && !seen[cur]) { seen[cur] = 1; cur = b[cur]; ++steps; }
    size_t reach = 0;
    for (size_t i = 0; i < cap; ++i) reach += seen[i] ? 1 : 0;
    printf("PROBE_CHAIN_STATS steps=%zu reachable_ids=%zu cycle_closed=%d\n", steps, reach,
           (cur < cap && seen[cur]) ? 1 : 0);
    free(b);
    return 0;
}

static int cmd_probe(const char *path, long off, long len)
{
    size_t n = 0;
    unsigned char *b = slurp(path, &n);
    if (b == NULL) return 1;
    if (off < 0 || (size_t)off >= n) { printf("PROBE_ERR off_out_of_range off=%ld size=%zu\n", off, n); free(b); return 1; }
    long end = off + len;
    if (end > (long)n) end = (long)n;
    printf("PROBE_RANGE path=%s off=%ld len=%ld size=%zu\n", path, off, (long)(end - off), n);
    for (long o = off; o + 16 <= end; o += 16) {
        unsigned long long a = u64le(b + o);
        unsigned long long c = u64le(b + o + 8);
        unsigned long ida = u32le(b + o);
        printf("PROBE_U64 off=%ld u64=%llu id32=%lu u64b=%llu\n", o, a, ida, c);
    }
    free(b);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: seg_probe zero|state|chain <path> [cap]\n       seg_probe probe <path> <off> <len>\n");
        return 2;
    }
    if (strcmp(argv[1], "zero") == 0) return cmd_zero(argv[2]);
    if (strcmp(argv[1], "state") == 0) return cmd_state(argv[2], argc > 3 ? (size_t)atoi(argv[3]) : 40);
    if (strcmp(argv[1], "chain") == 0) return cmd_chain(argv[2], argc > 3 ? (size_t)atoi(argv[3]) : 40);
    if (strcmp(argv[1], "probe") == 0 && argc >= 5) return cmd_probe(argv[2], atol(argv[3]), atol(argv[4]));
    printf("PROBE_ERR bad_args\n");
    return 2;
}
