#include "gtest/gtest.h"
#include "dzIPC/common/shm_mpmc_config.h"

TEST(ShmMpmcConfig, OnlyExplicitOneEnablesMpmc)
{
    EXPECT_FALSE(dzIPC::shm_mpmc_enabled_value(nullptr));
    EXPECT_FALSE(dzIPC::shm_mpmc_enabled_value(""));
    EXPECT_FALSE(dzIPC::shm_mpmc_enabled_value("0"));
    EXPECT_FALSE(dzIPC::shm_mpmc_enabled_value("true"));
    EXPECT_FALSE(dzIPC::shm_mpmc_enabled_value("01"));
    EXPECT_TRUE(dzIPC::shm_mpmc_enabled_value("1"));
}
