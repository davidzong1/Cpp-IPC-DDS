#include <cstring>
#include <cstdint>
#include <thread>

#include "libipc/shm.h"

#include "test.h"

using namespace ipc::shm;

namespace {

TEST(SHM, acquire) {
    handle shm_hd;
    EXPECT_FALSE(shm_hd.valid());

    EXPECT_TRUE(shm_hd.acquire("my-test-1", 1024));
    EXPECT_TRUE(shm_hd.valid());
    EXPECT_STREQ(shm_hd.name(), "my-test-1");

    EXPECT_TRUE(shm_hd.acquire("my-test-2", 2048));
    EXPECT_TRUE(shm_hd.valid());
    EXPECT_STREQ(shm_hd.name(), "my-test-2");

    EXPECT_TRUE(shm_hd.acquire("my-test-3", 4096));
    EXPECT_TRUE(shm_hd.valid());
    EXPECT_STREQ(shm_hd.name(), "my-test-3");
}

TEST(SHM, release) {
    handle shm_hd;
    EXPECT_FALSE(shm_hd.valid());
    shm_hd.release();
    EXPECT_FALSE(shm_hd.valid());
    EXPECT_TRUE(shm_hd.acquire("release-test-1", 512));
    EXPECT_TRUE(shm_hd.valid());
    shm_hd.release();
    EXPECT_FALSE(shm_hd.valid());
}

TEST(SHM, get) {
    handle shm_hd;
    EXPECT_TRUE(shm_hd.get() == nullptr);
    EXPECT_TRUE(shm_hd.acquire("get-test", 2048));

    auto mem = shm_hd.get();
    EXPECT_TRUE(mem != nullptr);
    EXPECT_TRUE(mem == shm_hd.get());

    std::uint8_t buf[1024] = {};
    EXPECT_TRUE(memcmp(mem, buf, sizeof(buf)) == 0);

    handle shm_other(shm_hd.name(), shm_hd.size());
    EXPECT_TRUE(shm_other.get() != shm_hd.get());
}

TEST(SHM, hello) {
    handle shm_hd;
    EXPECT_TRUE(shm_hd.acquire("hello-test", 128));
    auto mem = shm_hd.get();
    EXPECT_TRUE(mem != nullptr);

    constexpr char hello[] = "hello!";
    std::memcpy(mem, hello, sizeof(hello));
    EXPECT_STREQ((char const *)shm_hd.get(), hello);

    shm_hd.release();
    EXPECT_TRUE(shm_hd.get() == nullptr);
    EXPECT_TRUE(shm_hd.acquire("hello-test", 1024));

    mem = shm_hd.get();
    EXPECT_TRUE(mem != nullptr);
    std::uint8_t buf[1024] = {};
    EXPECT_TRUE(memcmp(mem, buf, sizeof(buf)) == 0);

    std::memcpy(mem, hello, sizeof(hello));
    EXPECT_STREQ((char const *)shm_hd.get(), hello);
}

TEST(SHM, mt) {
    handle shm_hd;
    EXPECT_TRUE(shm_hd.acquire("mt-test", 256));
    constexpr char hello[] = "hello!";
    std::memcpy(shm_hd.get(), hello, sizeof(hello));

    std::thread {
        [&shm_hd] {
            handle shm_mt(shm_hd.name(), shm_hd.size());
            shm_hd.release();
            constexpr char hello[] = "hello!";
            EXPECT_STREQ((char const *)shm_mt.get(), hello);
        }
    }.join();

    EXPECT_TRUE(shm_hd.get() == nullptr);
    EXPECT_FALSE(shm_hd.valid());

    EXPECT_TRUE(shm_hd.acquire("mt-test", 1024));
    std::uint8_t buf[1024] = {};
    EXPECT_TRUE(memcmp(shm_hd.get(), buf, sizeof(buf)) == 0);
}

TEST(SHM, remove) {
    {
        auto id = ipc::shm::acquire("hello-remove", 111);
        EXPECT_TRUE(ipc_ut::expect_exist("hello-remove", true));
        ipc::shm::remove(id);
        EXPECT_TRUE(ipc_ut::expect_exist("hello-remove", false));
    }
    {
        auto id = ipc::shm::acquire("hello-remove", 111);
        EXPECT_TRUE(ipc_ut::expect_exist("hello-remove", true));
        ipc::shm::release(id);
        EXPECT_TRUE(ipc_ut::expect_exist("hello-remove", true));
        ipc::shm::remove("hello-remove");
        EXPECT_TRUE(ipc_ut::expect_exist("hello-remove", false));
    }
    {
        handle shm_hd;
        EXPECT_TRUE(shm_hd.acquire("mt-test", 256));
        EXPECT_TRUE(ipc_ut::expect_exist("mt-test", true));
        shm_hd.clear();
        EXPECT_TRUE(ipc_ut::expect_exist("mt-test", false));
    }
    {
        handle shm_hd;
        EXPECT_TRUE(shm_hd.acquire("mt-test", 256));
        EXPECT_TRUE(ipc_ut::expect_exist("mt-test", true));
        shm_hd.clear_storage("mt-test");
        EXPECT_TRUE(ipc_ut::expect_exist("mt-test", false));
    }
}

TEST(SHM, release_no_unlink_preserves_name) {
    handle shm_hd;
    EXPECT_TRUE(shm_hd.acquire("rnl-test", 1024));
    EXPECT_TRUE(ipc_ut::expect_exist("rnl-test", true));

    // release_no_unlink should detach but NOT unlink the SHM name
    shm_hd.release_no_unlink();
    EXPECT_FALSE(shm_hd.valid());
    EXPECT_TRUE(ipc_ut::expect_exist("rnl-test", true));

    // Clean up
    ipc::shm::remove("rnl-test");
    EXPECT_TRUE(ipc_ut::expect_exist("rnl-test", false));
}

TEST(SHM, release_no_unlink_allows_reacquire) {
    // Simulates the sniffer close + publisher restart scenario:
    // 1. Create SHM (like a publisher)
    // 2. Another handle opens it (like a sniffer)
    // 3. Sniffer releases without unlink
    // 4. Original is removed and recreated (publisher restart)
    // 5. New sniffer can still open the same name

    handle pub_hd;
    EXPECT_TRUE(pub_hd.acquire("rnl-reopen", 1024));
    EXPECT_TRUE(ipc_ut::expect_exist("rnl-reopen", true));

    // Sniffer acquires the same SHM
    handle sniff_hd;
    EXPECT_TRUE(sniff_hd.acquire("rnl-reopen", 1024, ipc::shm::open));
    EXPECT_TRUE(sniff_hd.valid());

    // Sniffer closes — must NOT unlink the publisher's SHM
    sniff_hd.release_no_unlink();
    EXPECT_FALSE(sniff_hd.valid());
    EXPECT_TRUE(ipc_ut::expect_exist("rnl-reopen", true));

    // Publisher restarts: remove old, create new
    pub_hd.release(); // this WILL unlink (publisher owns the name)
    EXPECT_FALSE(ipc_ut::expect_exist("rnl-reopen", true));

    handle new_pub_hd;
    EXPECT_TRUE(new_pub_hd.acquire("rnl-reopen", 2048));
    EXPECT_TRUE(ipc_ut::expect_exist("rnl-reopen", true));

    // New sniffer can open the recreated SHM
    handle new_sniff_hd;
    EXPECT_TRUE(new_sniff_hd.acquire("rnl-reopen", 2048, ipc::shm::open));
    EXPECT_TRUE(new_sniff_hd.valid());

    // Clean up
    new_sniff_hd.release_no_unlink();
    new_pub_hd.release();
    EXPECT_FALSE(ipc_ut::expect_exist("rnl-reopen", true));
}

} // internal-linkage
