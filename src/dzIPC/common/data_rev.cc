#include "dzIPC/common/data_rev.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <vector>
#include "ipc_msg/ipc_msg_base/udp_rtps_ack_msg.hpp"

namespace dzIPC {
namespace socket {
namespace {
constexpr std::size_t UDP_MAX_SIZE = 1'472;
constexpr std::size_t TAIL_SIZE = 12;
constexpr std::size_t MAX_RECV_TOTAL_SIZE = 64 * 1'024 * 1'024;
constexpr int SEND_RETRY_MAX = 10;
constexpr int RTPS_MAX_NACK_ROUND = 5;
constexpr int SEND_BURST_BEFORE_YIELD = 64;
constexpr uint64_t ACK_FAST_WAIT_MS = 5;

struct chunk_meta
{
    uint16_t page_cnt{0};
    uint32_t total_size{0};
    uint32_t msg_id{0};
};

uint32_t local_node_id()
{
    static const uint32_t id = []
    {
        const auto now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return static_cast<uint32_t>((now >> 32) ^ now ^ 0xA5'3C'9E'17u);
    }();
    return id;
}

uint64_t elapsed_ms(const std::chrono::steady_clock::time_point& begin)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count());
}

bool parse_tail(const ipc::buffer& buf, ipc_tail_msg& tail)
{
    if (buf.size() < TAIL_SIZE)
    {
        return false;
    }

    const auto* p = static_cast<const uint8_t*>(buf.data());
    const std::size_t n = buf.size();
    tail.page_cnt = static_cast<uint16_t>(p[n - 12]) << 8 | static_cast<uint16_t>(p[n - 11]);
    tail.now_page = static_cast<uint16_t>(p[n - 10]) << 8 | static_cast<uint16_t>(p[n - 9]);
    tail.total_size = static_cast<uint32_t>(p[n - 8]) << 24 | static_cast<uint32_t>(p[n - 7]) << 16
                      | static_cast<uint32_t>(p[n - 6]) << 8 | static_cast<uint32_t>(p[n - 5]);
    tail.dz_ipc_msg_id = static_cast<uint32_t>(p[n - 4]) << 24 | static_cast<uint32_t>(p[n - 3]) << 16
                         | static_cast<uint32_t>(p[n - 2]) << 8 | static_cast<uint32_t>(p[n - 1]);
    return true;
}

void write_now_page(ipc::buffer& chunk, uint16_t now_page)
{
    if (chunk.size() < TAIL_SIZE)
    {
        return;
    }
    auto* p = static_cast<uint8_t*>(chunk.data());
    const std::size_t n = chunk.size();
    p[n - 10] = static_cast<uint8_t>(now_page >> 8);
    p[n - 9] = static_cast<uint8_t>(now_page & 0xFF);
}

bool valid_chunk_meta(const ipc_tail_msg& tail)
{
    if (tail.page_cnt == 0 || tail.total_size < TAIL_SIZE)
    {
        return false;
    }
    if (tail.total_size > MAX_RECV_TOTAL_SIZE)
    {
        return false;
    }

    const std::size_t page_cnt = static_cast<std::size_t>(tail.page_cnt);
    const std::size_t total_size = static_cast<std::size_t>(tail.total_size);
    const std::size_t max_total = page_cnt * UDP_MAX_SIZE;
    const std::size_t min_total = (page_cnt == 0) ? TAIL_SIZE : ((page_cnt - 1) * UDP_MAX_SIZE + TAIL_SIZE);
    return total_size >= min_total && total_size <= max_total;
}

std::size_t expected_page_size(const chunk_meta& meta, uint16_t now_page)
{
    if (now_page == meta.page_cnt)
    {
        const std::size_t offset = static_cast<std::size_t>(now_page - 1) * UDP_MAX_SIZE;
        return static_cast<std::size_t>(meta.total_size) - offset;
    }
    return UDP_MAX_SIZE;
}

bool place_page(const ipc::buffer& page_buf, const chunk_meta& meta, std::vector<uint8_t>& assembled,
                std::vector<uint8_t>& received, std::size_t& received_cnt)
{
    ipc_tail_msg tail;
    if (!parse_tail(page_buf, tail))
    {
        return false;
    }
    if (tail.page_cnt != meta.page_cnt || tail.total_size != meta.total_size || tail.dz_ipc_msg_id != meta.msg_id)
    {
        return false;
    }
    if (tail.now_page == 0 || tail.now_page > meta.page_cnt)
    {
        return false;
    }

    const std::size_t payload = page_buf.size();
    const std::size_t expected = expected_page_size(meta, tail.now_page);
    if (payload != expected)
    {
        return false;
    }

    if (received[tail.now_page] != 0)
    {
        return true;
    }

    const std::size_t offset = static_cast<std::size_t>(tail.now_page - 1) * UDP_MAX_SIZE;
    std::memcpy(assembled.data() + offset, page_buf.data(), payload);
    received[tail.now_page] = 1;
    ++received_cnt;
    return true;
}

uint64_t calc_round_wait_ms(uint16_t page_cnt, uint64_t tm)
{
    const uint64_t by_pages = std::max<uint64_t>(20, std::min<uint64_t>(200, static_cast<uint64_t>(page_cnt) * 4));
    if (tm == ipc::invalid_value)
    {
        return by_pages;
    }
    const uint64_t by_budget = std::max<uint64_t>(10, tm / static_cast<uint64_t>(RTPS_MAX_NACK_ROUND + 1));
    return std::min(by_pages, by_budget);
}

uint64_t calc_nack_wait_ms(uint16_t page_cnt)
{
    return std::max<uint64_t>(20, std::min<uint64_t>(200, static_cast<uint64_t>(page_cnt) * 4));
}

bool send_chunk_with_retry(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& chunk)
{
    int retry = 0;
    while (!node->send(chunk))
    {
        if (++retry >= SEND_RETRY_MAX)
        {
            return false;
        }
        if (retry <= 2)
        {
            std::this_thread::yield();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return true;
}

bool send_all_chunks(std::shared_ptr<ipc::socket::UDPNode>& node, std::vector<ipc::buffer>& chunks)
{
    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        if (!send_chunk_with_retry(node, chunks[i]))
        {
            return false;
        }
        if (((i + 1) % SEND_BURST_BEFORE_YIELD) == 0 && (i + 1) < chunks.size())
        {
            std::this_thread::yield();
        }
    }
    return true;
}

void send_ack(std::shared_ptr<ipc::socket::UDPNode>& node, const chunk_meta& meta)
{
    IpcRtpsAckMsg ack_msg;
    ack_msg.page_cnt = meta.page_cnt;
    ack_msg.total_size = meta.total_size;
    ack_msg.data_msg_id = meta.msg_id;
    ack_msg.receiver_id = local_node_id();
    ipc::buffer ack_buf = ack_msg.serialize();
    send_chunk_with_retry(node, ack_buf);
}

bool wait_first_data_chunk(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<IpcMsgBase>& msg_ptr,
                           chunk_meta& meta, ipc::buffer& first_page,
                           const std::chrono::steady_clock::time_point& begin, uint64_t tm)
{
    while (true)
    {
        uint64_t wait_ms = tm;
        if (tm != ipc::invalid_value)
        {
            const uint64_t used = elapsed_ms(begin);
            if (used >= tm)
            {
                return false;
            }
            wait_ms = tm - used;
        }

        ipc::buffer buf = node->receive(wait_ms);
        if (buf.empty())
        {
            return false;
        }
        if (!msg_ptr->check_id(buf))
        {
            continue;
        }

        ipc_tail_msg tail;
        if (!parse_tail(buf, tail) || !valid_chunk_meta(tail))
        {
            continue;
        }

        if (tail.now_page == 0 || tail.now_page > tail.page_cnt)
        {
            continue;
        }

        chunk_meta tentative{tail.page_cnt, tail.total_size, tail.dz_ipc_msg_id};
        if (buf.size() != expected_page_size(tentative, tail.now_page))
        {
            continue;
        }

        meta = tentative;
        first_page = std::move(buf);
        return true;
    }
}

void send_nack_for_missing(std::shared_ptr<ipc::socket::UDPNode>& node, const chunk_meta& meta,
                           const std::vector<uint8_t>& received, IpcRtpsNackMsg& nack_msg)
{
    nack_msg.page_cnt = meta.page_cnt;
    nack_msg.total_size = meta.total_size;
    nack_msg.data_msg_id = meta.msg_id;
    nack_msg.receiver_id = local_node_id();
    nack_msg.missing_pages.clear();

    for (uint16_t i = 1; i <= meta.page_cnt; ++i)
    {
        if (received[i] == 0)
        {
            nack_msg.missing_pages.push_back(i);
            if (nack_msg.missing_pages.size() >= IpcRtpsNackMsg::kMaxMissingPages)
            {
                break;
            }
        }
    }

    if (nack_msg.missing_pages.empty())
    {
        return;
    }

    ipc::buffer nack_buf = nack_msg.serialize();
    send_chunk_with_retry(node, nack_buf);
}

ipc::buffer make_owned_copy(const void* src, std::size_t n)
{
    auto* mem = new uint8_t[n];
    std::memcpy(mem, src, n);
    return ipc::buffer(mem, n, [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
}

bool recv_chunk_common(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<IpcMsgBase>& msg_ptr, uint64_t tm)
{
    const auto begin = std::chrono::steady_clock::now();

    chunk_meta meta{};
    ipc::buffer first_page;
    if (!wait_first_data_chunk(node, msg_ptr, meta, first_page, begin, tm))
    {
        return false;
    }

    if (meta.page_cnt == 1)
    {
        if (first_page.size() != meta.total_size)
        {
            return false;
        }
        msg_ptr->deserialize(first_page);
        return true;
    }

    std::vector<uint8_t> assembled(meta.total_size);
    std::vector<uint8_t> received(static_cast<std::size_t>(meta.page_cnt) + 1, 0);
    std::size_t received_cnt = 0;
    if (!place_page(first_page, meta, assembled, received, received_cnt))
    {
        return false;
    }

    const uint64_t round_wait_ms = calc_round_wait_ms(meta.page_cnt, tm);
    IpcRtpsNackMsg nack_msg;

    for (int round = 0; round < RTPS_MAX_NACK_ROUND && received_cnt < meta.page_cnt; ++round)
    {
        const auto round_begin = std::chrono::steady_clock::now();
        while (received_cnt < meta.page_cnt)
        {
            const uint64_t round_used = elapsed_ms(round_begin);
            if (round_used >= round_wait_ms)
            {
                break;
            }

            uint64_t wait_ms = round_wait_ms - round_used;
            if (tm != ipc::invalid_value)
            {
                const uint64_t used = elapsed_ms(begin);
                if (used >= tm)
                {
                    return false;
                }
                wait_ms = std::min(wait_ms, tm - used);
            }

            ipc::buffer page = node->receive(wait_ms);
            if (page.empty())
            {
                break;
            }
            if (!msg_ptr->check_id(page))
            {
                continue;
            }

            place_page(page, meta, assembled, received, received_cnt);
        }

        if (received_cnt >= meta.page_cnt)
        {
            break;
        }

        if (tm != ipc::invalid_value && elapsed_ms(begin) >= tm)
        {
            return false;
        }

        send_nack_for_missing(node, meta, received, nack_msg);
    }

    if (received_cnt < meta.page_cnt)
    {
        return false;
    }

    ipc::buffer assembled_view(assembled.data(), meta.total_size);
    msg_ptr->deserialize(assembled_view);
    send_ack(node, meta);
    return true;
}
}   // namespace

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool chunk_rev_topic(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<TopicData>& rev_msg, uint64_t tm)
{
    std::shared_ptr<IpcMsgBase> msg_ptr = rev_msg->topic();
    return recv_chunk_common(node, msg_ptr, tm);
}

bool chunk_rev_server(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<ServiceData>& rev_msg, uint64_t tm,
                      bool ser_or_cli)
{
    std::shared_ptr<IpcMsgBase> msg_ptr = ser_or_cli ? rev_msg->request() : rev_msg->response();
    return recv_chunk_common(node, msg_ptr, tm);
}

ipc::buffer chunk_rev_sniff(ipc::socket::UDPNode& node, uint64_t tm)
{
    const auto first_begin = std::chrono::steady_clock::now();

    chunk_meta meta{};
    bool have_meta = false;
    std::vector<uint8_t> assembled;
    std::vector<uint8_t> received;
    std::size_t received_cnt = 0;
    std::chrono::steady_clock::time_point assembly_begin{};
    uint64_t assembly_budget_ms = 0;
    ipc::buffer pending;

    auto begin_assembly = [&](const ipc::buffer& first_page)
    {
        assembled.assign(meta.total_size, 0);
        received.assign(static_cast<std::size_t>(meta.page_cnt) + 1, 0);
        received_cnt = 0;
        assembly_begin = std::chrono::steady_clock::now();
        // Bounded internal deadline so a stuck assembly doesn't hold the sniffer
        // thread past the next sender cycle. Tied to page_cnt so big frames get
        // proportionally more time, but capped to keep stop-responsiveness sane.
        assembly_budget_ms = std::min<uint64_t>(500, std::max<uint64_t>(50, static_cast<uint64_t>(meta.page_cnt) * 2));
        place_page(first_page, meta, assembled, received, received_cnt);
    };

    while (true)
    {
        uint64_t wait_ms;
        if (have_meta)
        {
            const uint64_t used = elapsed_ms(assembly_begin);
            if (used >= assembly_budget_ms)
            {
                return ipc::buffer{};
            }
            wait_ms = assembly_budget_ms - used;
        }
        else
        {
            if (tm == ipc::invalid_value)
            {
                wait_ms = ipc::invalid_value;
            }
            else
            {
                const uint64_t used = elapsed_ms(first_begin);
                if (used >= tm)
                {
                    return ipc::buffer{};
                }
                wait_ms = tm - used;
            }
        }

        ipc::buffer page;
        if (pending.size() > 0)
        {
            page = std::move(pending);
        }
        else
        {
            try
            {
                page = node.receive(wait_ms);
            }
            catch (...)
            {
                return ipc::buffer{};
            }
            if (page.empty())
            {
                return ipc::buffer{};
            }
        }

        ipc_tail_msg tail;
        if (!parse_tail(page, tail) || !valid_chunk_meta(tail))
        {
            continue;
        }
        if (tail.now_page == 0 || tail.now_page > tail.page_cnt)
        {
            continue;
        }

        chunk_meta tentative{tail.page_cnt, tail.total_size, tail.dz_ipc_msg_id};
        if (page.size() != expected_page_size(tentative, tail.now_page))
        {
            continue;
        }

        if (!have_meta)
        {
            // Only kick off assembly on a true first page. This is what keeps us
            // from latching onto the tail of a message whose head we missed —
            // i.e., the case where the previous frame was still being verified
            // on the wire while we were already moving on to "the next packet".
            if (tail.now_page != 1)
            {
                continue;
            }
            meta = tentative;
            have_meta = true;
            if (meta.page_cnt == 1)
            {
                if (page.size() != meta.total_size)
                {
                    have_meta = false;
                    continue;
                }
                return make_owned_copy(page.data(), page.size());
            }
            begin_assembly(page);
            if (received_cnt >= meta.page_cnt)
            {
                return make_owned_copy(assembled.data(), meta.total_size);
            }
            continue;
        }

        if (tail.dz_ipc_msg_id == meta.msg_id && tail.page_cnt == meta.page_cnt && tail.total_size == meta.total_size)
        {
            place_page(page, meta, assembled, received, received_cnt);
            if (received_cnt >= meta.page_cnt)
            {
                return make_owned_copy(assembled.data(), meta.total_size);
            }
            continue;
        }

        // A chunk with a different msg_id arrived mid-assembly — the sender has
        // already moved on, so the in-flight frame is unrecoverable.
        if (tail.now_page == 1)
        {
            // Fresh first page of a new message: discard the old frame and
            // restart cleanly on this one.
            meta = tentative;
            if (meta.page_cnt == 1)
            {
                if (page.size() != meta.total_size)
                {
                    have_meta = false;
                    continue;
                }
                return make_owned_copy(page.data(), page.size());
            }
            begin_assembly(page);
            if (received_cnt >= meta.page_cnt)
            {
                return make_owned_copy(assembled.data(), meta.total_size);
            }
            continue;
        }
        // Mid-page of a message whose start we never saw — we missed the boat.
        // Drop and let the caller try again from a clean state.
        return ipc::buffer{};
    }
}

bool chunk_send(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data)
{
    if (publish_data.empty() || publish_data.size() < TAIL_SIZE)
    {
        return false;
    }

    std::vector<ipc::buffer> chunks;
    chunks.reserve((publish_data.size() + UDP_MAX_SIZE - 1) / UDP_MAX_SIZE);
    for (std::size_t offset = 0; offset < publish_data.size(); offset += UDP_MAX_SIZE)
    {
        const std::size_t sz = std::min(UDP_MAX_SIZE, publish_data.size() - offset);
        chunks.emplace_back(static_cast<uint8_t*>(publish_data.data()) + offset, sz);
    }

    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        if (chunks[i].size() < TAIL_SIZE)
        {
            return false;
        }
        write_now_page(chunks[i], static_cast<uint16_t>(i + 1));
    }

    ipc_tail_msg first_tail;
    if (!parse_tail(chunks.front(), first_tail) || !valid_chunk_meta(first_tail))
    {
        return false;
    }
    if (static_cast<std::size_t>(first_tail.page_cnt) != chunks.size())
    {
        return false;
    }

    if (!send_all_chunks(node, chunks))
    {
        return false;
    }

    if (chunks.size() <= 1)
    {
        return true;
    }

    const chunk_meta meta{first_tail.page_cnt, first_tail.total_size, first_tail.dz_ipc_msg_id};
    const uint64_t nack_wait_ms = calc_nack_wait_ms(meta.page_cnt);
    IpcRtpsNackMsg nack_msg;
    IpcRtpsAckMsg ack_msg;

    for (int round = 0; round < RTPS_MAX_NACK_ROUND; ++round)
    {
        bool got_nack = false;
        bool got_ack = false;
        std::unordered_set<uint16_t> missing_union;
        const auto round_begin = std::chrono::steady_clock::now();
        const uint64_t round_wait_ms = (round == 0) ? ACK_FAST_WAIT_MS : nack_wait_ms;

        while (true)
        {
            const uint64_t used = elapsed_ms(round_begin);
            if (used >= round_wait_ms)
            {
                break;
            }
            ipc::buffer recv_buf = node->receive(round_wait_ms - used);
            if (recv_buf.empty())
            {
                break;
            }
            if (ack_msg.check_ak_id(recv_buf))
            {
                ack_msg.deserialize(recv_buf);
                if (ack_msg.page_cnt == meta.page_cnt && ack_msg.total_size == meta.total_size
                    && ack_msg.data_msg_id == meta.msg_id)
                {
                    got_ack = true;
                    break;
                }
                continue;
            }
            if (!nack_msg.check_id(recv_buf))
            {
                continue;
            }

            nack_msg.deserialize(recv_buf);
            if (nack_msg.page_cnt != meta.page_cnt || nack_msg.total_size != meta.total_size
                || nack_msg.data_msg_id != meta.msg_id)
            {
                continue;
            }

            got_nack = true;
            for (uint16_t page_id : nack_msg.missing_pages)
            {
                if (page_id > 0 && page_id <= chunks.size())
                {
                    missing_union.insert(page_id);
                }
            }
        }

        if (got_ack)
        {
            return true;
        }

        if (!got_nack || missing_union.empty())
        {
            break;
        }

        std::size_t resent = 0;
        for (uint16_t miss : missing_union)
        {
            if (!send_chunk_with_retry(node, chunks[miss - 1]))
            {
                return false;
            }
            if ((++resent % SEND_BURST_BEFORE_YIELD) == 0 && resent < missing_union.size())
            {
                std::this_thread::yield();
            }
        }
    }

    return true;
}
}   // namespace socket
}   // namespace dzIPC
