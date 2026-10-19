#include <gtest/gtest.h>
#include "collector.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

namespace sys_info::collector::test {

TEST(CollectorTest, ReadTemperatures) {
    auto temp = read_temperatures();
    // Должна найтись хотя бы одна температура (в виртуалках hwmon бывает пуст)
    if (temp.sensors.empty() && temp.cores.empty() && temp.package.empty()) {
        GTEST_SKIP() << "в /sys/class/hwmon нет датчиков температуры";
    }
    for (const auto& s : temp.cores) {
        EXPECT_GT(s.celsius, 0.0f) << "ядро '" << s.id << "'";
    }
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

/** @brief Метки tempN_label всех hwmon, кроме переименуемых сборщиком (пакет CPU, nct6687) */
static std::vector<std::string> sysfs_temp_labels(const std::string& only_hwmon = {}) {
    namespace fs = std::filesystem;
    std::vector<std::string> labels;
    for (const auto& hw : fs::directory_iterator("/sys/class/hwmon")) {
        std::ifstream name_file(hw.path() / "name");
        std::string name;
        std::getline(name_file, name);
        if (name == "nct6687" || (!only_hwmon.empty() && name != only_hwmon)) continue;
        for (const auto& f : fs::directory_iterator(hw.path())) {
            auto fn = f.path().filename().string();
            if (!fn.starts_with("temp") || !fn.ends_with("_label")) continue;
            std::ifstream lf(f.path());
            std::string label;
            std::getline(lf, label);
            if (!label.empty() && !label.starts_with("Package")) labels.push_back(label);
        }
    }
    return labels;
}

/** Имена датчиков читаются целой строкой: раньше "Sensor 1" обрезалось до "Sensor". */
TEST(CollectorTest, SensorLabelsNotTruncated) {
    auto labels = sysfs_temp_labels();
    if (labels.empty()) GTEST_SKIP() << "в hwmon нет tempN_label";

    auto temp = read_temperatures();
    std::set<std::string> ids;
    for (const auto& s : temp.sensors) ids.insert(s.id);
    for (const auto& s : temp.cores) ids.insert(s.id);
    for (const auto& label : labels) {
        EXPECT_TRUE(ids.contains(label)) << "метка '" << label << "' из sysfs не найдена в прочитанных датчиках";
    }
}

/**
 * coretemp нумерует temp-файлы с пропусками (temp1, temp10, temp14, ...):
 * ядер должно найтись столько же, сколько файлов Core N в hwmon.
 */
TEST(CollectorTest, AllCoreTemperaturesFound) {
    size_t expected = 0;
    for (const auto& label : sysfs_temp_labels("coretemp")) {
        if (label.starts_with("Core")) ++expected;
    }
    if (expected == 0) GTEST_SKIP() << "нет coretemp";

    auto temp = read_temperatures();
    EXPECT_EQ(temp.cores.size(), expected) << "ядер в coretemp: " << expected << ", прочитано: " << temp.cores.size();
}

/**
 * Sampler: второй замер считает загрузку за интервал, а не с момента загрузки системы;
 * все проценты в пределах 0..100.
 */
TEST(SamplerTest, LoadPercentIsDeltaInRange) {
    Sampler sampler;
    sampler.sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto st = sampler.sample();

    auto in_range = [](float v) { return v >= 0.0f && v <= 100.0f; };
    EXPECT_PRED1(in_range, st.load.active_percent);
    EXPECT_PRED1(in_range, st.load.user_percent);
    EXPECT_PRED1(in_range, st.load.system_percent);
    EXPECT_PRED1(in_range, st.load.iowait_percent);
    ASSERT_FALSE(st.load.per_core.empty());
    for (const auto& c : st.load.per_core) {
        SCOPED_TRACE("CPU" + std::to_string(c.id));
        EXPECT_PRED1(in_range, c.percent);
    }
}

/**
 * Sampler: скорость сети считается по реальному интервалу между замерами;
 * на первом замере предыдущих счётчиков нет — скорость 0.
 */
TEST(SamplerTest, NetworkRateNonNegative) {
    Sampler sampler;
    auto first = sampler.sample();
    for (const auto& [name, n] : first.hw.network) {
        EXPECT_EQ(n.rate_rx_mbps, 0.0f) << name << ": первый замер без предыдущего";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto second = sampler.sample();
    for (const auto& [name, n] : second.hw.network) {
        SCOPED_TRACE(name);
        EXPECT_GE(n.rate_rx_mbps, 0.0f);
        EXPECT_GE(n.rate_tx_mbps, 0.0f);
        EXPECT_GE(n.speed_mbps, 0) << "speed -1 у опущенных интерфейсов должен стать 0";
    }
}

} // namespace sys_info::collector::test
