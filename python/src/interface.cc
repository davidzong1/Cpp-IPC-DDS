#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/dzipc.h"
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;
namespace info = dzIPC::info_pool;

#define DZIPC_STRINGIFY_IMPL(value) #value
#define DZIPC_STRINGIFY(value) DZIPC_STRINGIFY_IMPL(value)

namespace {

constexpr const char* kPybind11Version =
    DZIPC_STRINGIFY(PYBIND11_VERSION_MAJOR) "."
    DZIPC_STRINGIFY(PYBIND11_VERSION_MINOR) "."
    DZIPC_STRINGIFY(PYBIND11_VERSION_PATCH);

std::vector<uint8_t> BytesToVector(py::bytes data)
{
    char* raw = nullptr;
    Py_ssize_t size = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &raw, &size) != 0)
    {
        throw py::error_already_set();
    }
    if (size <= 0)
    {
        return {};
    }
    const auto* first = reinterpret_cast<const uint8_t*>(raw);
    return {first, first + static_cast<std::size_t>(size)};
}

void DeserializeBytes(dzIPC::GenericMessage& self, py::bytes data)
{
    auto payload = BytesToVector(data);
    ipc::buffer buf(payload.empty() ? nullptr : payload.data(), payload.size());
    self.deserialize(buf);
}

template<typename T>
T CheckedCast(std::uint64_t value, const char* name)
{
    if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
    {
        throw py::value_error(std::string(name) + " is out of range");
    }
    return static_cast<T>(value);
}

}   // namespace

PYBIND11_MODULE(_dzipc_core, m)
{
    m.doc() = "pybind11 bindings for cpp-ipc (dzIPC)";
    m.attr("__python_version__") = PY_VERSION;
    m.attr("__pybind11_version__") = kPybind11Version;

    py::enum_<dzIPC::IPCType>(m, "IPCType")
        .value("Shm", dzIPC::IPCType::Shm)
        .value("Socket", dzIPC::IPCType::Socket)
        .export_values();

    m.attr("IPC_SHM") = py::cast(dzIPC::IPC_SHM);
    m.attr("IPC_SOCKET") = py::cast(dzIPC::IPC_SOCKET);

    py::class_<IpcMsgBase, std::shared_ptr<IpcMsgBase>>(m, "IpcMsgBase")
        .def("set_msg_id", &IpcMsgBase::set_msg_id)
        .def("total_size", &IpcMsgBase::total_size)
        .def("total_page_cnt", &IpcMsgBase::total_page_cnt);

    // ---- GenericMessage: 动态消息，替代所有具体 msg/srv 类型的绑定 ----
    py::class_<dzIPC::GenericMessage, IpcMsgBase, std::shared_ptr<dzIPC::GenericMessage>>(
        m, "GenericMessage")
        .def(py::init<>())
        .def("clear", &dzIPC::GenericMessage::clear)
        /* ---- DZFlat 段直通 (docs/dzflat_shm.md §9.6) ----
         * C++ 侧没有 schema 无法解析定长布局, 所以把段原样交给 Python, 由
         * python/dzipc/dzflat.py 按生成的 schema 解码。 */
        .def("has_dzflat", &dzIPC::GenericMessage::has_dzflat,
             "是否持有一个 DZFlat 段(而非 TLV 字段)")
        .def("dzflat_schema_hash_rx", &dzIPC::GenericMessage::dzflat_seg_schema_hash,
             "所持 DZFlat 段的 schema 指纹; 用于查 dzipc.dzflat 的 schema 注册表")
        .def(
            "dzflat_memoryview",
            [](dzIPC::GenericMessage& self)
            {
                /* 零拷贝视图: 生命周期与本 GenericMessage 绑定。Python 侧的解码器在
                 * 消息存活期间读它; 需要留存就自己 copy。这与 C++ 侧 View 的契约一致。 */
                const auto& v = self.dzflat_seg();
                return py::memoryview::from_memory(
                    const_cast<std::uint8_t*>(v.data()), static_cast<py::ssize_t>(v.size()),
                    /*readonly=*/true);
            },
            "所持 DZFlat 段的只读零拷贝视图(生命周期随本对象)")
        .def(
            "dzflat_bytes",
            [](dzIPC::GenericMessage& self)
            {
                const auto& v = self.dzflat_seg();
                return py::bytes(reinterpret_cast<const char*>(v.data()), v.size());
            },
            "所持 DZFlat 段的字节副本(需要跨消息留存时用它)")
        .def("field_count", &dzIPC::GenericMessage::field_count)
        .def("field_name", &dzIPC::GenericMessage::field_name)
        .def("field_type", &dzIPC::GenericMessage::field_type)
        // 标量 setters
        .def("set_bool", &dzIPC::GenericMessage::set_bool,
             py::arg("name"), py::arg("val"))
        .def("set_int8", &dzIPC::GenericMessage::set_int8,
             py::arg("name"), py::arg("val"))
        .def("set_uint8", &dzIPC::GenericMessage::set_uint8,
             py::arg("name"), py::arg("val"))
        .def("set_int16", &dzIPC::GenericMessage::set_int16,
             py::arg("name"), py::arg("val"))
        .def("set_uint16", &dzIPC::GenericMessage::set_uint16,
             py::arg("name"), py::arg("val"))
        .def("set_int32", &dzIPC::GenericMessage::set_int32,
             py::arg("name"), py::arg("val"))
        .def("set_uint32", &dzIPC::GenericMessage::set_uint32,
             py::arg("name"), py::arg("val"))
        .def("set_int64", &dzIPC::GenericMessage::set_int64,
             py::arg("name"), py::arg("val"))
        .def("set_uint64", &dzIPC::GenericMessage::set_uint64,
             py::arg("name"), py::arg("val"))
        .def("set_float32", &dzIPC::GenericMessage::set_float32,
             py::arg("name"), py::arg("val"))
        .def("set_float64", &dzIPC::GenericMessage::set_float64,
             py::arg("name"), py::arg("val"))
        .def("set_string", &dzIPC::GenericMessage::set_string,
             py::arg("name"), py::arg("val"))
        .def("set_nested", &dzIPC::GenericMessage::set_nested,
             py::arg("name"), py::arg("val"))
        // 标量 getters
        .def("get_bool", &dzIPC::GenericMessage::get_bool,
             py::arg("name"))
        .def("get_int8", &dzIPC::GenericMessage::get_int8,
             py::arg("name"))
        .def("get_uint8", &dzIPC::GenericMessage::get_uint8,
             py::arg("name"))
        .def("get_int16", &dzIPC::GenericMessage::get_int16,
             py::arg("name"))
        .def("get_uint16", &dzIPC::GenericMessage::get_uint16,
             py::arg("name"))
        .def("get_int32", &dzIPC::GenericMessage::get_int32,
             py::arg("name"))
        .def("get_uint32", &dzIPC::GenericMessage::get_uint32,
             py::arg("name"))
        .def("get_int64", &dzIPC::GenericMessage::get_int64,
             py::arg("name"))
        .def("get_uint64", &dzIPC::GenericMessage::get_uint64,
             py::arg("name"))
        .def("get_float32", &dzIPC::GenericMessage::get_float32,
             py::arg("name"))
        .def("get_float64", &dzIPC::GenericMessage::get_float64,
             py::arg("name"))
        .def("get_string", &dzIPC::GenericMessage::get_string,
             py::arg("name"))
        .def("get_nested", &dzIPC::GenericMessage::get_nested,
             py::arg("name"))
        // 数组 setters
        .def("set_bool_array", &dzIPC::GenericMessage::set_bool_array,
             py::arg("name"), py::arg("arr"))
        .def("set_int8_array", &dzIPC::GenericMessage::set_int8_array,
             py::arg("name"), py::arg("arr"))
        .def("set_uint8_array", &dzIPC::GenericMessage::set_uint8_array,
             py::arg("name"), py::arg("arr"))
        .def("set_int16_array", &dzIPC::GenericMessage::set_int16_array,
             py::arg("name"), py::arg("arr"))
        .def("set_uint16_array", &dzIPC::GenericMessage::set_uint16_array,
             py::arg("name"), py::arg("arr"))
        .def("set_int32_array", &dzIPC::GenericMessage::set_int32_array,
             py::arg("name"), py::arg("arr"))
        .def("set_uint32_array", &dzIPC::GenericMessage::set_uint32_array,
             py::arg("name"), py::arg("arr"))
        .def("set_int64_array", &dzIPC::GenericMessage::set_int64_array,
             py::arg("name"), py::arg("arr"))
        .def("set_uint64_array", &dzIPC::GenericMessage::set_uint64_array,
             py::arg("name"), py::arg("arr"))
        .def("set_float32_array", &dzIPC::GenericMessage::set_float32_array,
             py::arg("name"), py::arg("arr"))
        .def("set_float64_array", &dzIPC::GenericMessage::set_float64_array,
             py::arg("name"), py::arg("arr"))
        .def("set_string_array", &dzIPC::GenericMessage::set_string_array,
             py::arg("name"), py::arg("arr"))
        .def("set_nested_array", &dzIPC::GenericMessage::set_nested_array,
             py::arg("name"), py::arg("arr"))
        // 数组 getters
        .def("get_bool_array", &dzIPC::GenericMessage::get_bool_array,
             py::arg("name"))
        .def("get_int8_array", &dzIPC::GenericMessage::get_int8_array,
             py::arg("name"))
        .def("get_uint8_array", &dzIPC::GenericMessage::get_uint8_array,
             py::arg("name"))
        .def("get_int16_array", &dzIPC::GenericMessage::get_int16_array,
             py::arg("name"))
        .def("get_uint16_array", &dzIPC::GenericMessage::get_uint16_array,
             py::arg("name"))
        .def("get_int32_array", &dzIPC::GenericMessage::get_int32_array,
             py::arg("name"))
        .def("get_uint32_array", &dzIPC::GenericMessage::get_uint32_array,
             py::arg("name"))
        .def("get_int64_array", &dzIPC::GenericMessage::get_int64_array,
             py::arg("name"))
        .def("get_uint64_array", &dzIPC::GenericMessage::get_uint64_array,
             py::arg("name"))
        .def("get_float32_array", &dzIPC::GenericMessage::get_float32_array,
             py::arg("name"))
        .def("get_float64_array", &dzIPC::GenericMessage::get_float64_array,
             py::arg("name"))
        .def("get_string_array", &dzIPC::GenericMessage::get_string_array,
             py::arg("name"))
        .def("get_nested_array", &dzIPC::GenericMessage::get_nested_array,
             py::arg("name"))
        .def("serialize",
             [](dzIPC::GenericMessage& self) -> py::bytes
             {
                 auto buf = self.serialize();
                 if (buf.empty()) return py::bytes();
                 return py::bytes(static_cast<const char*>(buf.data()), buf.size());
             })
        .def("serialize_bytes",
             [](dzIPC::GenericMessage& self) -> py::bytes
             {
                 auto buf = self.serialize();
                 if (buf.empty()) return py::bytes();
                 return py::bytes(static_cast<const char*>(buf.data()), buf.size());
             })
        .def("deserialize",
             [](dzIPC::GenericMessage& self, py::bytes data) { DeserializeBytes(self, data); },
             py::arg("data"))
        .def("deserialize_bytes",
             [](dzIPC::GenericMessage& self, py::bytes data) { DeserializeBytes(self, data); },
             py::arg("data"));

    // ---- Control Plane (read-only) for publisher-restart detection ----
    // Mirrors exec/dzipc_topic_cat/src/shm_sniffer.cc:140-158.
    // Only exposes read-only methods: open, valid, generation, state.
    // Write operations (begin_rebuild, set_ready, set_stopping, etc.)
    // are deliberately NOT bound — they belong to the publisher side.

    py::enum_<dzIPC::control_plane_shm::TopicState>(m, "TopicState")
        .value("Empty", dzIPC::control_plane_shm::TopicState::Empty)
        .value("Clearing", dzIPC::control_plane_shm::TopicState::Clearing)
        .value("Ready", dzIPC::control_plane_shm::TopicState::Ready)
        .value("Stopping", dzIPC::control_plane_shm::TopicState::Stopping)
        .export_values();

    py::class_<dzIPC::control_plane_shm::TopicControlPlane>(m, "TopicControlPlane")
        .def(py::init<>())
        .def("open", &dzIPC::control_plane_shm::TopicControlPlane::open, py::arg("name"))
        .def("valid", &dzIPC::control_plane_shm::TopicControlPlane::valid)
        .def("generation", &dzIPC::control_plane_shm::TopicControlPlane::generation)
        .def("state", &dzIPC::control_plane_shm::TopicControlPlane::state);

    // ---- IPC 基础设施 ----

    py::class_<dzIPC::TopicData, std::shared_ptr<dzIPC::TopicData>>(m, "TopicData")
        .def(py::init<const std::shared_ptr<IpcMsgBase>&, size_t>(),
             py::arg("topic"), py::arg("msg_id") = 0)
        .def("topic", [](dzIPC::TopicData& self) { return self.topic(); })
        .def("update", &dzIPC::TopicData::update)
        .def("swap",
             [](dzIPC::TopicData& self)
             {
                 auto msg = std::shared_ptr<IpcMsgBase>();
                 self.swap(msg);
                 return msg;
             });

    py::class_<dzIPC::ServiceData, std::shared_ptr<dzIPC::ServiceData>>(m, "ServiceData")
        .def(py::init<const std::shared_ptr<IpcMsgBase>&, const std::shared_ptr<IpcMsgBase>&,
                      uint32_t>(),
             py::arg("request"), py::arg("response"), py::arg("msg_id") = 0)
        .def("request", [](dzIPC::ServiceData& self) { return self.request(); })
        .def("response", [](dzIPC::ServiceData& self) { return self.response(); });

    py::class_<dzIPC::pimpl::server_ipc_impl, std::shared_ptr<dzIPC::pimpl::server_ipc_impl>>(
        m, "ServerIPC")
        .def("InitChannel", &dzIPC::pimpl::server_ipc_impl::InitChannel, py::arg("extra_info") = "",
             py::call_guard<py::gil_scoped_release>())
        .def("reset_message", &dzIPC::pimpl::server_ipc_impl::reset_message)
        .def("reset_callback", &dzIPC::pimpl::server_ipc_impl::reset_callback);

    py::class_<dzIPC::pimpl::client_ipc_impl, std::shared_ptr<dzIPC::pimpl::client_ipc_impl>>(
        m, "ClientIPC")
        .def("InitChannel", &dzIPC::pimpl::client_ipc_impl::InitChannel, py::arg("extra_info") = "",
             py::call_guard<py::gil_scoped_release>())
        .def("reset_message", &dzIPC::pimpl::client_ipc_impl::reset_message)
        .def(
            "send_request",
            [](dzIPC::pimpl::client_ipc_impl& self, std::shared_ptr<dzIPC::ServiceData> request,
               uint64_t rev_tm)
            { return self.send_request(request, rev_tm); },
            py::arg("request"),
            py::arg("rev_tm") = std::numeric_limits<uint32_t>::max(),
            py::call_guard<py::gil_scoped_release>());

    py::class_<dzIPC::pimpl::publisher_ipc_impl, std::shared_ptr<dzIPC::pimpl::publisher_ipc_impl>>(
        m, "PublisherIPC")
        .def("InitChannel", &dzIPC::pimpl::publisher_ipc_impl::InitChannel, py::arg("extra_info") = "",
             py::call_guard<py::gil_scoped_release>())
        .def("reset_message", &dzIPC::pimpl::publisher_ipc_impl::reset_message)
        .def("publish", &dzIPC::pimpl::publisher_ipc_impl::publish,
             py::call_guard<py::gil_scoped_release>())
        .def("publish_best_effort", &dzIPC::pimpl::publisher_ipc_impl::publish_best_effort,
             py::call_guard<py::gil_scoped_release>())
        .def("publish_blocking", &dzIPC::pimpl::publisher_ipc_impl::publish_blocking,
             py::arg("msg"), py::arg("tm"), py::call_guard<py::gil_scoped_release>())
        .def("publish_for_sniffer", &dzIPC::pimpl::publisher_ipc_impl::publish_for_sniffer,
             py::call_guard<py::gil_scoped_release>())
        .def("has_subscribed", &dzIPC::pimpl::publisher_ipc_impl::has_subscribed);

    py::class_<dzIPC::pimpl::subscriber_ipc_impl,
               std::shared_ptr<dzIPC::pimpl::subscriber_ipc_impl>>(m, "SubscriberIPC")
        .def("InitChannel", &dzIPC::pimpl::subscriber_ipc_impl::InitChannel, py::arg("extra_info") = "",
             py::call_guard<py::gil_scoped_release>())
        .def("reset_message", &dzIPC::pimpl::subscriber_ipc_impl::reset_message)
        /* C++ 侧 get/try_get 已翻转为零拷贝 Sample 视图(见 pub_sub_base.h)。
         * Python 的 GenericMessage 载体是 schema-less 直通体, 无 C++ 类型可 bind 成
         * XxxView; Python 的零拷贝视图(borrowed bytes + memoryview)是后续独立改动。
         * 因此这里把 get/try_get 指到 clone 路径, 保留 Python 今日的行为。 */
        .def(
            "get",
            [](dzIPC::pimpl::subscriber_ipc_impl& self, std::shared_ptr<dzIPC::TopicData> msg)
            {
                {
                    py::gil_scoped_release release;
                    self.get_clone(msg);
                }
                return msg;
            },
            py::arg("msg"))
        .def(
            "try_get",
            [](dzIPC::pimpl::subscriber_ipc_impl& self, std::shared_ptr<dzIPC::TopicData> msg)
            {
                bool ok;
                {
                    py::gil_scoped_release release;
                    ok = self.try_get_clone(msg);
                }
                return py::make_tuple(ok, msg);
            },
            py::arg("msg"))
        .def(
            "get_clone",
            [](dzIPC::pimpl::subscriber_ipc_impl& self, std::shared_ptr<dzIPC::TopicData> msg)
            {
                {
                    py::gil_scoped_release release;
                    self.get_clone(msg);
                }
                return msg;
            },
            py::arg("msg"))
        .def(
            "try_get_clone",
            [](dzIPC::pimpl::subscriber_ipc_impl& self, std::shared_ptr<dzIPC::TopicData> msg)
            {
                bool ok;
                {
                    py::gil_scoped_release release;
                    ok = self.try_get_clone(msg);
                }
                return py::make_tuple(ok, msg);
            },
            py::arg("msg"));

    // 工厂函数
    m.def(
        "make_topic_data",
        [](const std::shared_ptr<IpcMsgBase>& msg, std::uint64_t msg_id)
        { return std::make_shared<dzIPC::TopicData>(msg, CheckedCast<std::size_t>(msg_id, "msg_id")); },
        py::arg("msg"), py::arg("msg_id") = 0);

    m.def(
        "make_service_data",
        [](const std::shared_ptr<IpcMsgBase>& request, const std::shared_ptr<IpcMsgBase>& response,
           std::uint64_t msg_id)
        { return std::make_shared<dzIPC::ServiceData>(request, response, CheckedCast<std::uint32_t>(msg_id, "msg_id")); },
        py::arg("request"), py::arg("response"), py::arg("msg_id") = 0);

    m.def("ServerIPCPtrMake", &dzIPC::ServerIPCPtrMake, py::arg("topic_name"), py::arg("msg"),
          py::arg("callback"), py::arg("domain_id"), py::arg("ipc_type"),
          py::arg("verbose") = false, py::arg("enable_thread_qos") = false,
          py::arg("cpu_id") = -1, py::arg("thread_priority") = 0,
          py::call_guard<py::gil_scoped_release>());

    m.def("ClientIPCPtrMake", &dzIPC::ClientIPCPtrMake, py::arg("topic_name"), py::arg("msg"),
          py::arg("domain_id"), py::arg("ipc_type"), py::arg("verbose") = false,
          py::arg("enable_thread_qos") = false, py::arg("cpu_id") = -1,
          py::arg("thread_priority") = 0,
          py::call_guard<py::gil_scoped_release>());

    m.def("PublisherIPCPtrMake", &dzIPC::PublisherIPCPtrMake, py::arg("msg"), py::arg("topic_name"),
          py::arg("domain_id"), py::arg("ipc_type"), py::arg("verbose") = false,
          py::arg("enable_thread_qos") = false, py::arg("cpu_id") = -1,
          py::arg("thread_priority") = 0,
          py::call_guard<py::gil_scoped_release>());

    m.def("SubscriberIPCPtrMake", &dzIPC::SubscriberIPCPtrMake, py::arg("msg"), py::arg("topic_name"),
          py::arg("domain_id"), py::arg("queue_size"), py::arg("ipc_type"),
          py::arg("verbose") = false, py::arg("enable_thread_qos") = false,
          py::arg("cpu_id") = -1, py::arg("thread_priority") = 0,
          py::call_guard<py::gil_scoped_release>());

    // ---- DZFlat: 开关与观测量 (docs/dzflat_shm.md) ----
    //
    // Python 进程需要这些, 理由和 C++ 一样但更迫切: Python 侧的载体是 GenericMessage,
    // 它是**直通体**(无 schema, 见 generic_message.hpp), 于是对任何格式合法的段都返回
    // 成功 —— C++ 层的 dzflat_schema_drop 在 Python 进程里永远是 0。版本错配到了 Python
    // 才被发现(按指纹查不到 schema), 所以两侧的计数要一起看才完整:
    //   C++ 侧 rx_counters()          —— 判别、msg_id、TLV 越界
    //   Python 侧 dzipc.dzflat.rx_stats() —— 指纹查不到 schema
    py::class_<dzIPC::DzFlatRxStats>(m, "DzFlatRxStats")
        .def_readonly("dzflat_accepted", &dzIPC::DzFlatRxStats::dzflat_accepted)
        .def_readonly("dzflat_id_skipped", &dzIPC::DzFlatRxStats::dzflat_id_skipped)
        .def_readonly("dzflat_header_bad", &dzIPC::DzFlatRxStats::dzflat_header_bad)
        .def_readonly("dzflat_schema_drop", &dzIPC::DzFlatRxStats::dzflat_schema_drop)
        .def_readonly("tlv_accepted", &dzIPC::DzFlatRxStats::tlv_accepted)
        .def_readonly("tlv_id_skipped", &dzIPC::DzFlatRxStats::tlv_id_skipped)
        .def_readonly("tlv_corrupt_drop", &dzIPC::DzFlatRxStats::tlv_corrupt_drop)
        .def_property_readonly("defects", &dzIPC::DzFlatRxStats::defects,
                               "真实缺陷合计(不含正常的 msg_id 过滤); 非 0 就该去查")
        .def("__repr__", [](const dzIPC::DzFlatRxStats& s) {
            return "<DzFlatRxStats dzflat=" + std::to_string(s.dzflat_accepted)
                   + "/skip" + std::to_string(s.dzflat_id_skipped)
                   + "/hdr_bad" + std::to_string(s.dzflat_header_bad)
                   + "/schema_drop" + std::to_string(s.dzflat_schema_drop)
                   + " tlv=" + std::to_string(s.tlv_accepted)
                   + "/skip" + std::to_string(s.tlv_id_skipped)
                   + "/corrupt" + std::to_string(s.tlv_corrupt_drop) + ">";
        });

    m.def("EnableDzFlat", &dzIPC::EnableDzFlat, py::arg("enabled"),
          "开启/关闭 DZFlat 发布(进程级, 默认关)。只影响发布侧; 接收侧永远同时认两种 wire");
    m.def("IsDzFlatEnabled", &dzIPC::IsDzFlatEnabled);
    m.def("DzFlatPublishCount", &dzIPC::DzFlatPublishCount, "走 DZFlat 发出的条数");
    m.def("DzFlatFallbackCount", &dzIPC::DzFlatFallbackCount, "尝试后回落整包序列化的条数");
    m.def("ResetDzFlatCounters", &dzIPC::ResetDzFlatCounters);
    m.def("DzFlatRxCounters", &dzIPC::DzFlatRxCounters, "接收侧计数(含各类拒收)");
    m.def("ResetDzFlatRxCounters", &dzIPC::ResetDzFlatRxCounters);

    m.def("StartShutdownMonitor", &dzIPC::StartShutdownMonitor);
    m.def("RequestShutdown", &dzIPC::RequestShutdown);
    m.def("IsShutdownRequested", &dzIPC::IsShutdownRequested);

    // ---- Passive sniffer (ipc::sniffer) ----
    // Read-only hook into SHM channels — does NOT register as receiver,
    // does not affect publisher behaviour.  Same sniffer used by
    // dzipc_topic_cat.

    py::enum_<ipc::sniffer::topology>(m, "SnifferTopology")
        .value("server", ipc::sniffer::topology::server)
        .value("route", ipc::sniffer::topology::route)
        .value("channel", ipc::sniffer::topology::channel)
        .export_values();

    py::class_<ipc::sniffer>(m, "Sniffer")
        .def(py::init<>())
        .def("open",
             [](ipc::sniffer& self, const char* name, ipc::sniffer::topology t) {
                 return self.open(name, t);
             },
             py::arg("name"), py::arg("topology") = ipc::sniffer::topology::route,
             "Attach to a channel by name. Returns True on success.")
        .def("open_prefixed",
             [](ipc::sniffer& self, const char* pref, const char* name,
                ipc::sniffer::topology t) {
                 return self.open(ipc::prefix{pref}, name, t);
             },
             py::arg("pref"), py::arg("name"),
             py::arg("topology") = ipc::sniffer::topology::route,
             "Attach with an explicit SHM prefix (e.g. domain id string).")
        .def("close", &ipc::sniffer::close)
        .def("valid", &ipc::sniffer::valid)
        .def("name", &ipc::sniffer::name,
             "Channel name, or empty string if not open.")
        .def("dropped", &ipc::sniffer::dropped,
             "Total dropped messages since open().")
        .def("skip_to_latest", &ipc::sniffer::skip_to_latest,
             "Discard backlog, resume from publisher's current write index.")
        .def("receiver_connections", &ipc::sniffer::receiver_connections,
             "Bit-set of currently-registered receivers (lower 32 bits).")
        .def("try_recv",
             [](ipc::sniffer& self) -> py::object {
                 ipc::sniffer::meta m{};
                 auto buf = self.try_recv(&m);
                 if (buf.empty()) return py::none();
                 return py::dict(
                     py::arg("data") = py::bytes(
                         static_cast<const char*>(buf.data()), buf.size()),
                     py::arg("cc_id") = m.cc_id,
                     py::arg("msg_id") = m.msg_id,
                     py::arg("dropped") = m.dropped);
             },
             "Non-blocking read. Returns None if no message, else dict "
             "with keys: data (bytes), cc_id, msg_id, dropped.")
        .def("recv",
             [](ipc::sniffer& self, std::uint64_t timeout_ms) -> py::object {
                 ipc::sniffer::meta m{};
                 auto buf = self.recv(timeout_ms, &m);
                 if (buf.empty()) return py::none();
                 return py::dict(
                     py::arg("data") = py::bytes(
                         static_cast<const char*>(buf.data()), buf.size()),
                     py::arg("cc_id") = m.cc_id,
                     py::arg("msg_id") = m.msg_id,
                     py::arg("dropped") = m.dropped);
             },
             py::arg("timeout_ms") = static_cast<std::uint64_t>(ipc::invalid_value),
             "Blocking read with timeout. Returns None on timeout, else "
             "dict with keys: data (bytes), cc_id, msg_id, dropped.");
}
