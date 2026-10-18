#include <gtest/gtest.h>
#include "collector.h"
#include <nlohmann/json.hpp>

namespace sys_info::collector::test {

TEST(CollectorTest, ReadTemperatures) {
    auto temp = read_temperatures();
    // Должна хотя бы одна температура найтись
    EXPECT_FALSE(temp.sensors.empty() || temp.cores.empty() || temp.package.empty());
}

TEST(CollectorTest, ReadFans) {
    auto fans = read_fans();
    // Могут отсутствовать на некоторых системах
    (void)fans;
}

TEST(CollectorTest, ReadCpuInfo) {
    auto cpu = read_cpu_info();
    EXPECT_FALSE(cpu.model.empty());
    EXPECT_GT(cpu.logical_cores, 0);
    EXPECT_GT(cpu.physical_cores, 0);
}

TEST(CollectorTest, ReadCpuLoad) {
    auto load = read_cpu_load();
    EXPECT_GT(load.total, 0);
    EXPECT_GT(load.load1, -1.0f);
}

TEST(CollectorTest, ReadMemory) {
    auto mem = read_memory();
    EXPECT_GT(mem.total, 0);
    EXPECT_GT(mem.free, 0);
}

TEST(CollectorTest, ReadDiskInfo) {
    auto disk = read_disk_info();
    EXPECT_FALSE(disk.disks.empty());
}

TEST(CollectorTest, ReadHardwareInfo) {
    auto hw = read_hardware_info();
    EXPECT_FALSE(hw.board_vendor.empty());
}

TEST(CollectorTest, ReadAll) {
    auto status = read_all();
    EXPECT_FALSE(status.cpu.model.empty());
    EXPECT_GT(status.mem.total, 0);
    EXPECT_FALSE(status.disk.disks.empty());
}

TEST(CollectorTest, JsonSerialization) {
    auto temp = read_temperatures();
    nlohmann::json j;
    nlohmann::to_json(j, temp);
    // Должна сериализоваться без ошибок
    std::string s = j.dump();
    EXPECT_FALSE(s.empty());
}

TEST(CollectorTest, JsonCpuInfo) {
    auto cpu = read_cpu_info();
    nlohmann::json j;
    nlohmann::to_json(j, cpu);
    EXPECT_EQ(j["model"], cpu.model);
    EXPECT_EQ(j["logical_cores"], cpu.logical_cores);
}

TEST(CollectorTest, JsonMemInfo) {
    auto mem = read_memory();
    nlohmann::json j;
    nlohmann::to_json(j, mem);
    EXPECT_EQ(j["total"], mem.total);
    EXPECT_EQ(j["used"], mem.used);
}

} // namespace sys_info::collector::test
