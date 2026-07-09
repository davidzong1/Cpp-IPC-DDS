#include "dzIPC/dzipc.h"
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace py = pybind11;
namespace info = dzIPC::info_pool;

PYBIND11_MODULE(_dzipc_core, m)
{
    m.doc() = "pybind11 bindings for cpp-ipc (dzIPC)";

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
             [](dzIPC::GenericMessage& self, py::bytes data)
             {
                 std::string s = data;
                 size_t sz = s.size();
                 auto* copy = new uint8_t[sz];
                 std::memcpy(copy, s.data(), sz);
                 ipc::buffer buf(copy, sz, [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
                 self.deserialize(buf);
             })
        .def("deserialize_bytes",
             [](dzIPC::GenericMessage& self, py::bytes data)
             {
                 std::string s = data;
                 size_t sz = s.size();
                 auto* copy = new uint8_t[sz];
                 std::memcpy(copy, s.data(), sz);
                 ipc::buffer buf(copy, sz, [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
                 self.deserialize(buf);
             });

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
        .def(
            "get",
            [](dzIPC::pimpl::subscriber_ipc_impl& self, std::shared_ptr<dzIPC::TopicData> msg)
            {
                {
                    py::gil_scoped_release release;
                    self.get(msg);
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
                    ok = self.try_get(msg);
                }
                return py::make_tuple(ok, msg);
            },
            py::arg("msg"));

    // 工厂函数
    m.def(
        "make_topic_data",
        [](const std::shared_ptr<IpcMsgBase>& msg, int msg_id)
        { return std::make_shared<dzIPC::TopicData>(msg, static_cast<size_t>(msg_id)); },
        py::arg("msg"), py::arg("msg_id") = 0);

    m.def(
        "make_service_data",
        [](const std::shared_ptr<IpcMsgBase>& request, const std::shared_ptr<IpcMsgBase>& response,
           int msg_id)
        { return std::make_shared<dzIPC::ServiceData>(request, response, static_cast<uint32_t>(msg_id)); },
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

    m.def("StartShutdownMonitor", &dzIPC::StartShutdownMonitor);
    m.def("RequestShutdown", &dzIPC::RequestShutdown);
    m.def("IsShutdownRequested", &dzIPC::IsShutdownRequested);
}
