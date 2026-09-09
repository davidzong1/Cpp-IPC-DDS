/// Tests for the dzipc_pub CLI tool's building blocks:
///   - .msg schema parsing and recursive schema registry (msg_schema.h)
///   - default message construction and field overrides   (msg_builder.h)
///   - end-to-end publish → subscribe over SHM using the same code path
///     the tool uses (GenericMessage + TopicData + InitChannel).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/topic_data.h"
#include "dzIPC/dzipc.h"
#include "dzIPC/ipc_info_pool.h"
#include "exec/dzipc_pub/include/msg_builder.h"
#include "exec/dzipc_pub/include/msg_schema.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"

#if defined(_WIN32)
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

namespace fs = std::filesystem;
using dzIPC::GenericMessage;
using dzipc_pub::MsgSchema;
using dzipc_pub::SchemaRegistry;

namespace {

/// Creates a temporary msg directory with a small type hierarchy:
///   vec3.msg            float64 x/y/z + string frame
///   robot_pose.msg      string name, vec3 position, vec3[] waypoints,
///                       int32 status, int32[] path_ids
///   inner_mid.msg       vec3 leaf
///   deep_outer.msg      inner_mid mid
///   sensor_reading.msg  every primitive scalar + array type
class DzipcPubTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        msg_dir_ = fs::temp_directory_path()
                   / ("dzipc_pub_test_" + std::to_string(GETPID()));
        fs::create_directories(msg_dir_ / "sub");

        write_file(msg_dir_ / "vec3.msg",
                   "# simple 3d vector\n"
                   "float64 x\n"
                   "float64 y\n"
                   "float64 z\n"
                   "string frame\n");
        // in a subdirectory to exercise recursive search
        write_file(msg_dir_ / "sub" / "robot_pose.msg",
                   "string name\n"
                   "vec3 position\n"
                   "vec3[] waypoints\n"
                   "int32 status\n"
                   "int32[] path_ids\n");
        write_file(msg_dir_ / "inner_mid.msg", "vec3 leaf\n");
        write_file(msg_dir_ / "deep_outer.msg", "inner_mid mid\n");
        write_file(msg_dir_ / "sensor_reading.msg",
                   "bool ok\n"
                   "int8 a8\n"
                   "uint8 b8\n"
                   "int16 a16\n"
                   "uint16 b16\n"
                   "int32 a32\n"
                   "uint32 b32\n"
                   "int64 a64\n"
                   "uint64 b64\n"
                   "float32 f32\n"
                   "float64 f64\n"
                   "string label\n"
                   "bool[] flags\n"
                   "int32[] samples\n"
                   "float64[] weights\n"
                   "string[] tags\n");
        // references a type that has no .msg file
        write_file(msg_dir_ / "broken.msg", "no_such_type ghost\n");
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(msg_dir_, ec);
    }

    static void write_file(const fs::path& path, const std::string& content)
    {
        std::ofstream f(path);
        ASSERT_TRUE(f.is_open()) << "cannot create " << path;
        f << content;
    }

    std::string dir() const { return msg_dir_.string(); }

    fs::path msg_dir_;
};

}   // namespace

// =====================================================================
// Schema parsing
// =====================================================================

TEST(DzipcPubUtil, SnakeToPascal)
{
    EXPECT_EQ(dzipc_pub::snake_to_pascal("std_vector"), "StdVector");
    EXPECT_EQ(dzipc_pub::snake_to_pascal("pose"), "Pose");
    EXPECT_EQ(dzipc_pub::snake_to_pascal("robot_state_2d"), "RobotState2d");
    // Already-PascalCase names pass through unchanged
    EXPECT_EQ(dzipc_pub::snake_to_pascal("StdHeader"), "StdHeader");
    EXPECT_EQ(dzipc_pub::snake_to_pascal(""), "");
}

TEST_F(DzipcPubTest, ParseMsgFile)
{
    MsgSchema schema = dzipc_pub::parse_msg_file((msg_dir_ / "sub" / "robot_pose.msg").string());
    EXPECT_EQ(schema.type_name, "RobotPose");
    ASSERT_EQ(schema.fields.size(), 5u);

    EXPECT_EQ(schema.fields[0].name, "name");
    EXPECT_EQ(schema.fields[0].type, dzIPC::FT_STRING);
    EXPECT_TRUE(schema.fields[0].nested_type.empty());

    EXPECT_EQ(schema.fields[1].name, "position");
    EXPECT_EQ(schema.fields[1].type, dzIPC::FT_NESTED);
    EXPECT_EQ(schema.fields[1].nested_type, "Vec3");   // snake_case declaration normalized

    EXPECT_EQ(schema.fields[2].name, "waypoints");
    EXPECT_EQ(schema.fields[2].type, dzIPC::FT_NESTED_ARRAY);
    EXPECT_EQ(schema.fields[2].nested_type, "Vec3");

    EXPECT_EQ(schema.fields[3].name, "status");
    EXPECT_EQ(schema.fields[3].type, dzIPC::FT_INT32);

    EXPECT_EQ(schema.fields[4].name, "path_ids");
    EXPECT_EQ(schema.fields[4].type, dzIPC::FT_INT32_ARRAY);
}

TEST_F(DzipcPubTest, ParseMsgFileSkipsCommentsAndBlanks)
{
    MsgSchema schema = dzipc_pub::parse_msg_file((msg_dir_ / "vec3.msg").string());
    EXPECT_EQ(schema.type_name, "Vec3");
    ASSERT_EQ(schema.fields.size(), 4u);   // comment line ignored
    EXPECT_EQ(schema.fields[3].name, "frame");
}

TEST_F(DzipcPubTest, FindMsgFileRecursive)
{
    std::string found = dzipc_pub::find_msg_file(dir(), "RobotPose");
    EXPECT_FALSE(found.empty());
    EXPECT_NE(found.find("robot_pose.msg"), std::string::npos);

    EXPECT_TRUE(dzipc_pub::find_msg_file(dir(), "DoesNotExist").empty());
}

TEST_F(DzipcPubTest, SchemaRegistryLoadsNestedRecursively)
{
    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("RobotPose");
    ASSERT_NE(schema, nullptr);
    // nested Vec3 got cached as a side effect
    EXPECT_NE(registry.get("Vec3"), nullptr);
    // unknown type
    EXPECT_EQ(registry.load("DoesNotExist"), nullptr);
}

// =====================================================================
// Default construction
// =====================================================================

TEST_F(DzipcPubTest, BuildDefaultsRecursive)
{
    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("RobotPose");
    ASSERT_NE(schema, nullptr);

    GenericMessage msg;
    EXPECT_TRUE(dzipc_pub::build_default_message(msg, *schema, registry));

    EXPECT_EQ(msg.get_string("name"), "");
    EXPECT_EQ(msg.get_int32("status"), 0);
    EXPECT_TRUE(msg.get_int32_array("path_ids").empty());
    EXPECT_TRUE(msg.get_nested_array("waypoints").empty());

    // nested defaults built from vec3.msg, not left empty
    GenericMessage pos = msg.get_nested("position");
    EXPECT_EQ(pos.field_count(), 4u);
    EXPECT_DOUBLE_EQ(pos.get_float64("x"), 0.0);
    EXPECT_EQ(pos.get_string("frame"), "");
}

TEST_F(DzipcPubTest, BuildDefaultsReportsMissingNestedSchema)
{
    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("Broken");
    ASSERT_NE(schema, nullptr);

    GenericMessage msg;
    EXPECT_FALSE(dzipc_pub::build_default_message(msg, *schema, registry));
    // field still present (empty) so the message stays publishable
    EXPECT_EQ(msg.get_nested("ghost").field_count(), 0u);
}

// =====================================================================
// Field overrides
// =====================================================================

TEST_F(DzipcPubTest, OverrideScalarsWithEqualsSyntax)
{
    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("SensorReading");
    ASSERT_NE(schema, nullptr);

    GenericMessage msg;
    ASSERT_TRUE(dzipc_pub::build_default_message(msg, *schema, registry));

    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "ok=true"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "a8=-128"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "b8=255"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "a16=-32768"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "b16=65535"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "a32=42"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "b32=4294967295"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "a64=-9999999999"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "b64=18446744073709551615"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "f32=1.5"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "f64=2.718281828"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "label=hello world"), "");

    EXPECT_TRUE(msg.get_bool("ok"));
    EXPECT_EQ(msg.get_int8("a8"), -128);
    EXPECT_EQ(msg.get_uint8("b8"), 255);
    EXPECT_EQ(msg.get_int16("a16"), -32768);
    EXPECT_EQ(msg.get_uint16("b16"), 65535);
    EXPECT_EQ(msg.get_int32("a32"), 42);
    EXPECT_EQ(msg.get_uint32("b32"), 4294967295u);
    EXPECT_EQ(msg.get_int64("a64"), -9999999999LL);
    EXPECT_EQ(msg.get_uint64("b64"), 18446744073709551615ULL);
    EXPECT_FLOAT_EQ(msg.get_float32("f32"), 1.5f);
    EXPECT_DOUBLE_EQ(msg.get_float64("f64"), 2.718281828);
    EXPECT_EQ(msg.get_string("label"), "hello world");
}

TEST_F(DzipcPubTest, OverrideArrays)
{
    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("SensorReading");
    ASSERT_NE(schema, nullptr);

    GenericMessage msg;
    ASSERT_TRUE(dzipc_pub::build_default_message(msg, *schema, registry));

    // bracketed and bare forms both work
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "samples=[1, 2, 3]"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "weights=0.5,1.5,-2.25"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "tags=[alpha, beta gamma]"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "flags=[true,false,1,0]"), "");

    EXPECT_EQ(msg.get_int32_array("samples"), (std::vector<int32_t>{1, 2, 3}));
    EXPECT_EQ(msg.get_float64_array("weights"), (std::vector<double>{0.5, 1.5, -2.25}));
    EXPECT_EQ(msg.get_string_array("tags"), (std::vector<std::string>{"alpha", "beta gamma"}));
    EXPECT_EQ(msg.get_bool_array("flags"), (std::vector<uint8_t>{1, 0, 1, 0}));

    // empty array literal clears the field
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "samples=[]"), "");
    EXPECT_TRUE(msg.get_int32_array("samples").empty());
}

TEST_F(DzipcPubTest, OverrideNestedDottedPath)
{
    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("RobotPose");
    ASSERT_NE(schema, nullptr);

    GenericMessage msg;
    ASSERT_TRUE(dzipc_pub::build_default_message(msg, *schema, registry));

    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "position.x=1.25"), "");
    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "position.frame=map"), "");

    GenericMessage pos = msg.get_nested("position");
    EXPECT_DOUBLE_EQ(pos.get_float64("x"), 1.25);
    EXPECT_DOUBLE_EQ(pos.get_float64("y"), 0.0);   // untouched sibling keeps default
    EXPECT_EQ(pos.get_string("frame"), "map");
}

TEST_F(DzipcPubTest, OverrideTwoLevelNesting)
{
    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("DeepOuter");
    ASSERT_NE(schema, nullptr);

    GenericMessage msg;
    ASSERT_TRUE(dzipc_pub::build_default_message(msg, *schema, registry));

    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "mid.leaf.z=-3.5"), "");
    EXPECT_DOUBLE_EQ(msg.get_nested("mid").get_nested("leaf").get_float64("z"), -3.5);
}

TEST_F(DzipcPubTest, OverrideLegacyColonSyntax)
{
    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("SensorReading");
    ASSERT_NE(schema, nullptr);

    GenericMessage msg;
    ASSERT_TRUE(dzipc_pub::build_default_message(msg, *schema, registry));

    EXPECT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "a32:int32:77"), "");
    EXPECT_EQ(msg.get_int32("a32"), 77);

    // declared type must match the schema
    std::string err = dzipc_pub::apply_field_override(msg, *schema, registry, "a32:float64:1.0");
    EXPECT_NE(err.find("declared as 'int32'"), std::string::npos) << err;
}

TEST_F(DzipcPubTest, OverrideRejectsInvalidSpecs)
{
    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("SensorReading");
    ASSERT_NE(schema, nullptr);

    GenericMessage msg;
    ASSERT_TRUE(dzipc_pub::build_default_message(msg, *schema, registry));
    size_t field_count_before = msg.field_count();

    // unknown field must be rejected, not silently added
    EXPECT_NE(dzipc_pub::apply_field_override(msg, *schema, registry, "typo_field=1"), "");
    EXPECT_EQ(msg.field_count(), field_count_before);

    // malformed values
    EXPECT_NE(dzipc_pub::apply_field_override(msg, *schema, registry, "a32=notanumber"), "");
    EXPECT_NE(dzipc_pub::apply_field_override(msg, *schema, registry, "a8=129"), "");        // out of range
    EXPECT_NE(dzipc_pub::apply_field_override(msg, *schema, registry, "b8=-1"), "");         // negative unsigned
    EXPECT_NE(dzipc_pub::apply_field_override(msg, *schema, registry, "ok=maybe"), "");      // bad bool
    EXPECT_NE(dzipc_pub::apply_field_override(msg, *schema, registry, "samples=[1,x,3]"), "");
    EXPECT_NE(dzipc_pub::apply_field_override(msg, *schema, registry, "no_separator"), "");

    // dotted path through a non-nested field
    EXPECT_NE(dzipc_pub::apply_field_override(msg, *schema, registry, "a32.x=1"), "");
}

TEST_F(DzipcPubTest, OverrideNestedFieldDirectlyIsRejected)
{
    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("RobotPose");
    ASSERT_NE(schema, nullptr);

    GenericMessage msg;
    ASSERT_TRUE(dzipc_pub::build_default_message(msg, *schema, registry));

    std::string err = dzipc_pub::apply_field_override(msg, *schema, registry, "position=1");
    EXPECT_NE(err.find("dotted paths"), std::string::npos) << err;
}

// =====================================================================
// Wire-format round trip
// =====================================================================

TEST_F(DzipcPubTest, SerializeDeserializeRoundtrip)
{
    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("RobotPose");
    ASSERT_NE(schema, nullptr);

    GenericMessage msg;
    ASSERT_TRUE(dzipc_pub::build_default_message(msg, *schema, registry));
    ASSERT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "name=roundtrip"), "");
    ASSERT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "status=-5"), "");
    ASSERT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "path_ids=[10,20,30]"), "");
    ASSERT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "position.y=9.75"), "");

    ipc::buffer wire = msg.serialize();
    ASSERT_FALSE(wire.empty());

    GenericMessage decoded;
    decoded.deserialize(wire);

    EXPECT_EQ(decoded.get_string("name"), "roundtrip");
    EXPECT_EQ(decoded.get_int32("status"), -5);
    EXPECT_EQ(decoded.get_int32_array("path_ids"), (std::vector<int32_t>{10, 20, 30}));
    EXPECT_DOUBLE_EQ(decoded.get_nested("position").get_float64("y"), 9.75);
}

// =====================================================================
// End-to-end publish → subscribe over SHM (mirrors the dzipc_pub flow:
// schema → defaults → overrides → TopicData(msg_id) → InitChannel →
// wait for handshake → publish).
// =====================================================================

TEST_F(DzipcPubTest, EndToEndShmPublishSubscribe)
{
    using namespace dzIPC;

    const std::string topic = "dzipc_pub_gtest_" + std::to_string(GETPID());
    constexpr uint32_t kMsgId = 4242;
    constexpr size_t kDomain = 1;

    SchemaRegistry registry(dir());
    const MsgSchema* schema = registry.load("RobotPose");
    ASSERT_NE(schema, nullptr);

    // --- subscriber side (what the target application would run) ---
    TopicDataPtr sub_data =
        std::make_shared<TopicData>(std::make_shared<GenericMessage>(), kMsgId);
    SubscriberIPCPtr subscriber = SubscriberIPCPtrMake(sub_data, topic, kDomain, 10, IPC_SHM, false);
    subscriber->InitChannel();

    // The fixed shm registration must expose the concrete message type name,
    // not "shared_ptr<IpcMsgBase>" — dzipc_pub relies on it to locate .msg files.
    {
        auto entries = dzIPC::info_pool::IpcInfoPool::instance().snapshot(false);
        bool found = false;
        for (const auto& e : entries)
        {
            if (e.topic_name == topic && e.kind == dzIPC::info_pool::EntryKind::ShmSub)
            {
                found = true;
                EXPECT_EQ(e.type_name, "GenericMessage");
            }
        }
        EXPECT_TRUE(found) << "subscriber did not register in IpcInfoPool";
    }

    // --- publisher side (dzipc_pub flow) ---
    GenericMessage msg;
    ASSERT_TRUE(dzipc_pub::build_default_message(msg, *schema, registry));
    ASSERT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "name=e2e"), "");
    ASSERT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "status=7"), "");
    ASSERT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "path_ids=[3,1,4]"), "");
    ASSERT_EQ(dzipc_pub::apply_field_override(msg, *schema, registry, "position.x=1.25"), "");

    auto topic_base = std::make_shared<GenericMessage>(msg);
    topic_base->set_msg_id(kMsgId);
    auto pub_data = std::make_shared<TopicData>(topic_base, kMsgId);
    PublisherIPCPtr publisher = PublisherIPCPtrMake(pub_data, topic, kDomain, IPC_SHM, false);
    publisher->InitChannel();

    // wait for the control-plane handshake so the first publish is deliverable
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!publisher->has_subscribed() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(publisher->has_subscribed()) << "shm handshake did not complete";

    // publish until the subscriber reports a message (retries tolerate startup races)
    std::atomic<bool> stop_publishing{false};
    std::thread pub_thread(
        [&]()
        {
            while (!stop_publishing.load())
            {
                auto out = std::make_shared<GenericMessage>(msg);
                out->set_msg_id(kMsgId);
                publisher->publish(out->msgcast<IpcMsgBase>());
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });

    bool received = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!received && std::chrono::steady_clock::now() < deadline)
    {
        if (subscriber->try_get_clone(sub_data))
        {
            received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stop_publishing.store(true);
    pub_thread.join();

    ASSERT_TRUE(received) << "subscriber did not receive any message";

    auto got = sub_data->topic()->msgcast<GenericMessage>();
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->get_string("name"), "e2e");
    EXPECT_EQ(got->get_int32("status"), 7);
    EXPECT_EQ(got->get_int32_array("path_ids"), (std::vector<int32_t>{3, 1, 4}));
    EXPECT_DOUBLE_EQ(got->get_nested("position").get_float64("x"), 1.25);
    // untouched fields arrive with their schema defaults
    EXPECT_TRUE(got->get_nested_array("waypoints").empty());
}
