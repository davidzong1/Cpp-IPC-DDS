#include "dzIPC/dzipc.h"
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
// AUTO_GENERATED_MSG_SRV_INCLUDES_BEGIN
#include "ipc_msg/supervisor_info/supervisor.hpp"
#include "ipc_msg/test_messages/complex_message.hpp"
#include "ipc_msg/test_msg2/test_msg.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"
// AUTO_GENERATED_MSG_SRV_INCLUDES_END

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <limits>
#include <memory>
#include <string>

namespace py = pybind11;
namespace info = dzIPC::info_pool;

PYBIND11_MODULE(dzipc, m)
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

    py::class_<dzIPC::TopicData, std::shared_ptr<dzIPC::TopicData>>(m, "TopicData")
        .def(py::init<const std::shared_ptr<IpcMsgBase>&, size_t>(), py::arg("topic"), py::arg("msg_id") = 0)
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
        .def(py::init<const std::shared_ptr<IpcMsgBase>&, const std::shared_ptr<IpcMsgBase>&, uint32_t>(),
             py::arg("request"), py::arg("response"), py::arg("msg_id") = 0)
        .def("request", [](dzIPC::ServiceData& self) { return self.request(); })
        .def("response", [](dzIPC::ServiceData& self) { return self.response(); });

    py::class_<dzIPC::pimpl::server_ipc_impl, std::shared_ptr<dzIPC::pimpl::server_ipc_impl>>(m, "ServerIPC")
        // InitChannel 会建立 socket/shm 通道并可能等待对端，可能阻塞 → 释放 GIL
        .def("InitChannel", &dzIPC::pimpl::server_ipc_impl::InitChannel, py::arg("extra_info") = "",
             py::call_guard<py::gil_scoped_release>())
        .def("reset_message", &dzIPC::pimpl::server_ipc_impl::reset_message)
        .def("reset_callback", &dzIPC::pimpl::server_ipc_impl::reset_callback);

    py::class_<dzIPC::pimpl::client_ipc_impl, std::shared_ptr<dzIPC::pimpl::client_ipc_impl>>(m, "ClientIPC")
        // InitChannel 同上
        .def("InitChannel", &dzIPC::pimpl::client_ipc_impl::InitChannel, py::arg("extra_info") = "",
             py::call_guard<py::gil_scoped_release>())
        .def("reset_message", &dzIPC::pimpl::client_ipc_impl::reset_message)
        // send_request 会阻塞等待 server 回包（带 rev_tm 超时）→ 必须释放 GIL
        .def(
            "send_request",
            [](dzIPC::pimpl::client_ipc_impl& self, std::shared_ptr<dzIPC::ServiceData> request, uint64_t rev_tm)
            { return self.send_request(request, rev_tm); }, py::arg("request"),
            py::arg("rev_tm") = std::numeric_limits<uint32_t>::max(), py::call_guard<py::gil_scoped_release>());

    py::class_<dzIPC::pimpl::publisher_ipc_impl, std::shared_ptr<dzIPC::pimpl::publisher_ipc_impl>>(m, "PublisherIPC")
        // InitChannel 同上
        .def("InitChannel", &dzIPC::pimpl::publisher_ipc_impl::InitChannel, py::arg("extra_info") = "",
             py::call_guard<py::gil_scoped_release>())
        .def("reset_message", &dzIPC::pimpl::publisher_ipc_impl::reset_message)
        // publish 涉及 socket/shm 写入，可能阻塞在内核发送缓冲或共享内存锁 → 释放 GIL
        .def("publish", &dzIPC::pimpl::publisher_ipc_impl::publish, py::call_guard<py::gil_scoped_release>())
        // has_subscribed 通常是轻量原子判断，不释放 GIL
        .def("has_subscribed", &dzIPC::pimpl::publisher_ipc_impl::has_subscribed);

    py::class_<dzIPC::pimpl::subscriber_ipc_impl, std::shared_ptr<dzIPC::pimpl::subscriber_ipc_impl>>(m,
                                                                                                      "SubscriberIPC")
        // InitChannel 同上
        .def("InitChannel", &dzIPC::pimpl::subscriber_ipc_impl::InitChannel, py::arg("extra_info") = "",
             py::call_guard<py::gil_scoped_release>())
        .def("reset_message", &dzIPC::pimpl::subscriber_ipc_impl::reset_message)
        // get 是阻塞型：等到消息才返回。**必须**释放 GIL，否则同进程发布线程会被饿死。
        // 注意：只在 C++ IO 调用期间释放 GIL，返回值（Python 对象）的构造必须在持有 GIL 时进行，
        // 否则 pybind11 会触发 "inc_ref() PyGILState_Check() failure" 断言。
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
        // try_get 非阻塞，但仍涉及 socket/shm 读、互斥锁；同样仅在 C++ 调用期间释放 GIL。
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

    m.def(
        "make_topic_data", [](const std::shared_ptr<IpcMsgBase>& msg, int msg_id)
        { return std::make_shared<dzIPC::TopicData>(msg, static_cast<size_t>(msg_id)); }, py::arg("msg"),
        py::arg("msg_id") = 0);

    m.def(
        "make_service_data",
        [](const std::shared_ptr<IpcMsgBase>& request, const std::shared_ptr<IpcMsgBase>& response, int msg_id)
        { return std::make_shared<dzIPC::ServiceData>(request, response, static_cast<uint32_t>(msg_id)); },
        py::arg("request"), py::arg("response"), py::arg("msg_id") = 0);

    // 4 个 *PtrMake 工厂在内部会建立 socket / 共享内存 / 多播订阅，可能阻塞 → 释放 GIL。
    // 注意 ServerIPCPtrMake 接受 Python callback：构造期只是存指针，不会回调；
    // 后续 server 工作线程触发 callback 时由 pybind11/functional.h 自行获取 GIL，安全。
    m.def("ServerIPCPtrMake", &dzIPC::ServerIPCPtrMake, py::arg("topic_name"), py::arg("msg"), py::arg("callback"),
          py::arg("domain_id"), py::arg("ipc_type"), py::arg("verbose") = false,
          py::call_guard<py::gil_scoped_release>());

    m.def("ClientIPCPtrMake", &dzIPC::ClientIPCPtrMake, py::arg("topic_name"), py::arg("msg"), py::arg("domain_id"),
          py::arg("ipc_type"), py::arg("verbose") = false, py::call_guard<py::gil_scoped_release>());

    m.def("PublisherIPCPtrMake", &dzIPC::PublisherIPCPtrMake, py::arg("msg"), py::arg("topic_name"),
          py::arg("domain_id"), py::arg("ipc_type"), py::arg("verbose") = false,
          py::call_guard<py::gil_scoped_release>());

    m.def("SubscriberIPCPtrMake", &dzIPC::SubscriberIPCPtrMake, py::arg("msg"), py::arg("topic_name"),
          py::arg("domain_id"), py::arg("queue_size"), py::arg("ipc_type"), py::arg("verbose") = false,
          py::call_guard<py::gil_scoped_release>());

    m.def("StartShutdownMonitor", &dzIPC::StartShutdownMonitor);
    m.def("RequestShutdown", &dzIPC::RequestShutdown);
    m.def("IsShutdownRequested", &dzIPC::IsShutdownRequested);
    m.def("CleanupIpcInstances", &dzIPC::CleanupIpcInstances);

    // AUTO_GENERATED_MSG_SRV_BINDINGS_BEGIN
    py::class_<dzIPC::Msg::Supervisor, IpcMsgBase, std::shared_ptr<dzIPC::Msg::Supervisor>>(m, "Supervisor")
        .def(py::init<>())
        .def_readwrite("update_time", &dzIPC::Msg::Supervisor::update_time)
        .def_readwrite("additional_info", &dzIPC::Msg::Supervisor::additional_info);

    py::class_<dzIPC::Msg::ComplexMessage, IpcMsgBase, std::shared_ptr<dzIPC::Msg::ComplexMessage>>(m, "ComplexMessage")
        .def(py::init<>())
        .def_readwrite("status", &dzIPC::Msg::ComplexMessage::status)
        .def_readwrite("tiny_int", &dzIPC::Msg::ComplexMessage::tiny_int)
        .def_readwrite("tiny_uint", &dzIPC::Msg::ComplexMessage::tiny_uint)
        .def_readwrite("small_int", &dzIPC::Msg::ComplexMessage::small_int)
        .def_readwrite("small_uint", &dzIPC::Msg::ComplexMessage::small_uint)
        .def_readwrite("normal_int", &dzIPC::Msg::ComplexMessage::normal_int)
        .def_readwrite("normal_uint", &dzIPC::Msg::ComplexMessage::normal_uint)
        .def_readwrite("big_int", &dzIPC::Msg::ComplexMessage::big_int)
        .def_readwrite("big_uint", &dzIPC::Msg::ComplexMessage::big_uint)
        .def_readwrite("single_precision", &dzIPC::Msg::ComplexMessage::single_precision)
        .def_readwrite("double_precision", &dzIPC::Msg::ComplexMessage::double_precision)
        .def_readwrite("message", &dzIPC::Msg::ComplexMessage::message)
        .def_readwrite("status_array", &dzIPC::Msg::ComplexMessage::status_array)
        .def_readwrite("tiny_int_array", &dzIPC::Msg::ComplexMessage::tiny_int_array)
        .def_readwrite("tiny_uint_array", &dzIPC::Msg::ComplexMessage::tiny_uint_array)
        .def_readwrite("small_int_array", &dzIPC::Msg::ComplexMessage::small_int_array)
        .def_readwrite("small_uint_array", &dzIPC::Msg::ComplexMessage::small_uint_array)
        .def_readwrite("normal_int_array", &dzIPC::Msg::ComplexMessage::normal_int_array)
        .def_readwrite("normal_uint_array", &dzIPC::Msg::ComplexMessage::normal_uint_array)
        .def_readwrite("big_int_array", &dzIPC::Msg::ComplexMessage::big_int_array)
        .def_readwrite("big_uint_array", &dzIPC::Msg::ComplexMessage::big_uint_array)
        .def_readwrite("single_precision_array", &dzIPC::Msg::ComplexMessage::single_precision_array)
        .def_readwrite("double_precision_array", &dzIPC::Msg::ComplexMessage::double_precision_array)
        .def_readwrite("message_array", &dzIPC::Msg::ComplexMessage::message_array);

    py::class_<dzIPC::Msg::TestMsg, IpcMsgBase, std::shared_ptr<dzIPC::Msg::TestMsg>>(m, "TestMsg")
        .def(py::init<>())
        .def_readwrite("data1", &dzIPC::Msg::TestMsg::data1)
        .def_readwrite("data2", &dzIPC::Msg::TestMsg::data2)
        .def_readwrite("data3", &dzIPC::Msg::TestMsg::data3)
        .def_readwrite("data4", &dzIPC::Msg::TestMsg::data4);

    py::class_<dzIPC::Srv::RequestResponseTestRequest, IpcMsgBase, std::shared_ptr<dzIPC::Srv::RequestResponseTestRequest>>(m, "RequestResponseTestRequest")
        .def(py::init<>())
        .def_readwrite("request", &dzIPC::Srv::RequestResponseTestRequest::request);

    py::class_<dzIPC::Srv::RequestResponseTestResponse, IpcMsgBase, std::shared_ptr<dzIPC::Srv::RequestResponseTestResponse>>(m, "RequestResponseTestResponse")
        .def(py::init<>())
        .def_readwrite("response", &dzIPC::Srv::RequestResponseTestResponse::response);

    m.def("message_types", []() {
        return std::vector<std::string>{
            "Supervisor",
            "ComplexMessage",
            "TestMsg",
        };
    });

    m.def("create_message", [](const std::string &type_name) -> std::shared_ptr<IpcMsgBase> {
        if (type_name == "Supervisor") return std::make_shared<dzIPC::Msg::Supervisor>();
        if (type_name == "ComplexMessage") return std::make_shared<dzIPC::Msg::ComplexMessage>();
        if (type_name == "TestMsg") return std::make_shared<dzIPC::Msg::TestMsg>();
        throw py::value_error("Unknown message type: " + type_name);
    });

    m.def("service_types", []() {
        return std::vector<std::string>{
            "RequestResponseTest",
        };
    });

    m.def("create_service_request", [](const std::string &service_name) -> std::shared_ptr<IpcMsgBase> {
        if (service_name == "RequestResponseTest") return std::make_shared<dzIPC::Srv::RequestResponseTestRequest>();
        throw py::value_error("Unknown service type: " + service_name);
    });

    m.def("create_service_response", [](const std::string &service_name) -> std::shared_ptr<IpcMsgBase> {
        if (service_name == "RequestResponseTest") return std::make_shared<dzIPC::Srv::RequestResponseTestResponse>();
        throw py::value_error("Unknown service type: " + service_name);
    });

// AUTO_GENERATED_MSG_SRV_BINDINGS_END
}
