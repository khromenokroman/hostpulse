#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <chrono>
#include <nlohmann/json.hpp>

namespace hostpulse::collector {

/** @brief Датчик температуры (ID и значение в Цельсиях) */
struct TempSensor {
    std::string id;
    float celsius;
};

/** @brief Датчик вентилятора (ID и обороты в минуту) */
struct FanSensor {
    std::string id;
    int rpm;
};

/** @brief Данные температурных датчиков */
struct TempData {
    std::vector<TempSensor> package;
    std::vector<TempSensor> cores;
    std::vector<TempSensor> sensors;
    std::map<std::string, float> max_sensors;
    std::map<std::string, float> crit_sensors;
};

/** @brief Данные вентиляторов */
struct FanData {
    std::vector<FanSensor> fans;
};

/** @brief Информация о процессоре */
struct CpuInfo {
    std::string model;
    int logical_cores = 0;
    int physical_cores = 0;
    std::vector<float> current_mhz;
    float min_mhz = 0.0f;
    float max_mhz = 0.0f;
    std::string governor;
    std::string driver;
};

/** @brief Загрузка процессора (CPU load averages + jiffies) */
struct CpuLoad {
    uint64_t total = 0;
    uint64_t active = 0;
    float active_percent = 0.0f;
    float user_percent = 0.0f;
    float system_percent = 0.0f;
    float idle_percent = 0.0f;
    float iowait_percent = 0.0f;
    uint64_t user = 0;
    uint64_t nice = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t iowait = 0;
    uint64_t irq = 0;
    uint64_t softirq = 0;
    uint64_t steal = 0;
    float load1 = 0.0f;
    float load5 = 0.0f;
    float load15 = 0.0f;
    struct CoreLoad {
        int id = 0;
        uint64_t active = 0;
        uint64_t total = 0;
        float percent = 0.0f;
    };
    std::vector<CoreLoad> per_core;
};

/** @brief Информация об оперативной памяти */
struct MemInfo {
    uint64_t total = 0;
    uint64_t used = 0;
    float used_percent = 0.0f;
    uint64_t free = 0;
    uint64_t available = 0;
    uint64_t buffers = 0;
    uint64_t cached = 0;
    uint64_t swap_total = 0;
    uint64_t swap_free = 0;
    uint64_t swap_used = 0;
    float swap_percent = 0.0f;
};

/** @brief Информация о дисках */
struct DiskInfo {
    struct Disk {
        std::string name;
        uint64_t size_bytes = 0;
        uint64_t read_bytes = 0;
        uint64_t write_bytes = 0;
        uint64_t read_ios = 0;
        uint64_t write_ios = 0;
        uint64_t read_sectors = 0;
        uint64_t write_sectors = 0;
    };

    struct Mount {
        std::string mountpoint;
        std::string device;
        std::string fstype;
        uint64_t total_bytes = 0;
        uint64_t used_bytes = 0;
        uint64_t read_bytes = 0;
        uint64_t write_bytes = 0;
    };

    std::map<std::string, Disk> disks;
    std::map<std::string, Mount> mounts;
};

/** @brief Информация о батарее */
struct BatteryInfo {
    int capacity = 0;
    int full_design = 0;
    int full_now = 0;
    std::string status;
};

/** @brief Информация о сетевом интерфейсе */
struct NetworkInfo {
    std::string name;
    std::string type;
    std::string mac;
    int speed_mbps = 0;
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
    uint64_t prev_rx = 0;
    uint64_t prev_tx = 0;
    float rate_rx_mbps = 0.0f;
    float rate_tx_mbps = 0.0f;
};

/** @brief Информация об оборудовании (DMI, BIOS, GPU, сеть) */
struct HardwareInfo {
    std::string board_vendor;
    std::string board_name;
    std::string bios_vendor;
    std::string bios_version;
    std::string bios_date;
    std::string product_name;
    std::string serial_number;
    BatteryInfo battery;
    std::vector<std::string> gpu;
    std::map<std::string, NetworkInfo> network;
    int pci_device_count = 0;
};

/** @brief Полный статус системы */
struct SystemStatus {
    CpuInfo cpu;
    CpuLoad load;
    TempData temp;
    FanData fans;
    MemInfo mem;
    DiskInfo disk;
    HardwareInfo hw;
    std::chrono::system_clock::time_point timestamp;
};

/** @brief Считать все данные системы */
SystemStatus read_all();

/**
 * @brief Сборщик с состоянием: загрузка CPU и скорость сети по разнице между замерами
 *
 * read_cpu_load() отдаёт проценты с момента загрузки системы, а скорость сети без
 * предыдущего замера не посчитать. Sampler помнит прошлый замер и пересчитывает
 * эти поля за интервал между вызовами sample(). Не потокобезопасен.
 */
class Sampler {
public:
    /** @brief Снять все данные и пересчитать загрузку CPU и скорость сети за интервал */
    SystemStatus sample();

private:
    CpuLoad prev_load_;
    std::map<std::string, std::pair<uint64_t, uint64_t>> prev_net_;
    std::chrono::steady_clock::time_point prev_time_;
    bool has_prev_ = false;
};

/** @brief Считать температуры (hwmon + thermal zones) */
TempData read_temperatures();

/** @brief Считать данные вентиляторов (hwmon) */
FanData read_fans();

/** @brief Считать информацию о процессоре (model, cores, MHz, governor) */
CpuInfo read_cpu_info();

/** @brief Считать загрузку процессора (loadavg + jiffies) */
CpuLoad read_cpu_load();

/** @brief Считать информацию об оперативной памяти */
MemInfo read_memory();

/** @brief Считать информацию о дисках и смонтированных разделах */
DiskInfo read_disk_info();

/** @brief Считать информацию об оборудовании (DMI, BIOS, GPU, сеть, батарея) */
HardwareInfo read_hardware_info();

// JSON сериализация
template<typename K, typename V>
inline void to_json(nlohmann::json& j, const std::map<K, V>& m) {
    j = nlohmann::json::object();
    for (auto& [k, v] : m) j[k] = v;
}

inline void to_json(nlohmann::json& j, const TempSensor& s) {
    j = nlohmann::json{{"id", s.id}, {"celsius", s.celsius}};
}

inline void to_json(nlohmann::json& j, const FanSensor& f) {
    j = nlohmann::json{{"id", f.id}, {"rpm", f.rpm}};
}

inline void to_json(nlohmann::json& j, const TempData& t) {
    j = nlohmann::json{
        {"package", t.package},
        {"cores", t.cores},
        {"sensors", t.sensors},
        {"max_sensors", t.max_sensors},
        {"crit_sensors", t.crit_sensors}
    };
}

inline void to_json(nlohmann::json& j, const FanData& f) {
    j = nlohmann::json{{"fans", f.fans}};
}

inline void to_json(nlohmann::json& j, const CpuInfo& c) {
    j = nlohmann::json{
        {"model", c.model},
        {"logical_cores", c.logical_cores},
        {"physical_cores", c.physical_cores},
        {"current_mhz", c.current_mhz},
        {"min_mhz", c.min_mhz},
        {"max_mhz", c.max_mhz},
        {"governor", c.governor},
        {"driver", c.driver}
    };
}

inline void to_json(nlohmann::json& j, const CpuLoad::CoreLoad& c) {
    j = nlohmann::json{
        {"id", c.id},
        {"active", c.active},
        {"total", c.total},
        {"percent", c.percent}
    };
}

inline void to_json(nlohmann::json& j, const CpuLoad& l) {
    j = nlohmann::json{
        {"total", l.total},
        {"active", l.active},
        {"active_percent", l.active_percent},
        {"user_percent", l.user_percent},
        {"system_percent", l.system_percent},
        {"idle_percent", l.idle_percent},
        {"iowait_percent", l.iowait_percent},
        {"user", l.user},
        {"nice", l.nice},
        {"system", l.system},
        {"idle", l.idle},
        {"iowait", l.iowait},
        {"irq", l.irq},
        {"softirq", l.softirq},
        {"steal", l.steal},
        {"load1", l.load1},
        {"load5", l.load5},
        {"load15", l.load15},
        {"per_core", l.per_core}
    };
}

inline void to_json(nlohmann::json& j, const MemInfo& m) {
    j = nlohmann::json{
        {"total", m.total},
        {"used", m.used},
        {"used_percent", m.used_percent},
        {"free", m.free},
        {"available", m.available},
        {"buffers", m.buffers},
        {"cached", m.cached},
        {"swap_total", m.swap_total},
        {"swap_used", m.swap_used},
        {"swap_percent", m.swap_percent}
    };
}

inline void to_json(nlohmann::json& j, const DiskInfo::Disk& d) {
    j = nlohmann::json{
        {"name", d.name},
        {"size_bytes", d.size_bytes},
        {"read_bytes", d.read_bytes},
        {"write_bytes", d.write_bytes},
        {"read_ios", d.read_ios},
        {"write_ios", d.write_ios},
        {"read_sectors", d.read_sectors},
        {"write_sectors", d.write_sectors}
    };
}

inline void to_json(nlohmann::json& j, const DiskInfo::Mount& m) {
    j = nlohmann::json{
        {"mountpoint", m.mountpoint},
        {"device", m.device},
        {"fstype", m.fstype},
        {"total_bytes", m.total_bytes},
        {"used_bytes", m.used_bytes},
        {"read_bytes", m.read_bytes},
        {"write_bytes", m.write_bytes}
    };
}

inline void to_json(nlohmann::json& j, const DiskInfo& d) {
    j = nlohmann::json{
        {"disks", d.disks},
        {"mounts", d.mounts}
    };
}

inline void to_json(nlohmann::json& j, const BatteryInfo& b) {
    j = nlohmann::json{
        {"capacity", b.capacity},
        {"full_design", b.full_design},
        {"full_now", b.full_now},
        {"status", b.status}
    };
}

inline void to_json(nlohmann::json& j, const NetworkInfo& n) {
    j = nlohmann::json{
        {"name", n.name},
        {"type", n.type},
        {"mac", n.mac},
        {"speed_mbps", n.speed_mbps},
        {"rx_bytes", n.rx_bytes},
        {"tx_bytes", n.tx_bytes},
        {"rate_rx_mbps", n.rate_rx_mbps},
        {"rate_tx_mbps", n.rate_tx_mbps}
    };
}

inline void to_json(nlohmann::json& j, const HardwareInfo& h) {
    j = nlohmann::json{
        {"board_vendor", h.board_vendor},
        {"board_name", h.board_name},
        {"bios_vendor", h.bios_vendor},
        {"bios_version", h.bios_version},
        {"bios_date", h.bios_date},
        {"product_name", h.product_name},
        {"serial_number", h.serial_number},
        {"battery", h.battery},
        {"gpu", h.gpu},
        {"network", h.network},
        {"pci_device_count", h.pci_device_count}
    };
}

inline void to_json(nlohmann::json& j, const SystemStatus& s) {
    j = nlohmann::json{
        {"cpu", s.cpu},
        {"load", s.load},
        {"temp", s.temp},
        {"fans", s.fans},
        {"mem", s.mem},
        {"disk", s.disk},
        {"hw", s.hw},
        {"timestamp", std::chrono::system_clock::to_time_t(s.timestamp)}
    };
}

} // namespace hostpulse::collector
