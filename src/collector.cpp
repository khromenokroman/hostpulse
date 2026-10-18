#include "collector.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <array>
#include <chrono>
#include <thread>
#include <algorithm>
#include <mutex>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <sys/statvfs.h>

namespace fs = std::filesystem;

namespace sys_info::collector {

// ------------------------------------------------------------------
// Утилиты
// ------------------------------------------------------------------

static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::string s;
    f >> s;
    return s;
}

static std::string read_line(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line);
    return line;
}

static std::string read_full(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::string s;
    std::getline(f, s);
    return s;
}

static float celsius_from_millideg(int32_t millideg) {
    return millideg / 1000.0f;
}

static uint64_t read_uint(const fs::path& p) {
    auto s = read_file(p);
    if (s.empty()) return 0;
    return std::stoull(s);
}

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    auto end = s.find_last_not_of(" \t\n\r");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

// ------------------------------------------------------------------
// Температура
// ------------------------------------------------------------------

TempData read_temperatures() {
    TempData data;
    fs::path hwmon_dir("/sys/class/hwmon");
    if (!fs::exists(hwmon_dir)) return data;

    for (const auto& entry : fs::directory_iterator(hwmon_dir)) {
        auto name = entry.path().filename().string();
        if (name.rfind("hwmon", 0) != 0) continue;

        fs::path base = entry.path();
        std::string hwmon_name = read_full(base / "name");
        if (hwmon_name.empty()) continue;

        for (int i = 1; ; ++i) {
            auto temp_file = base / ("temp" + std::to_string(i) + "_input");
            auto temp_max_file = base / ("temp" + std::to_string(i) + "_max");
            auto temp_crit_file = base / ("temp" + std::to_string(i) + "_crit");

            auto val_str = read_file(temp_file);
            if (val_str.empty()) break;

            try {
                int32_t raw = std::stoi(val_str);
                float c = celsius_from_millideg(raw);

              std::string label;
                std::string source;
                if (hwmon_name == "coretemp") {
                    if (i == 1) {
                        label = "Температура пакета";
                        data.package.emplace_back(label, c);
                        source = "coretemp";
                    } else {
                        label = "Ядро " + std::to_string(i - 1);
                        data.cores.emplace_back(label, c);
                        source = "coretemp";
                    }
                } else if (hwmon_name == "nvme") {
                    label = "NVMe";
                    data.sensors.emplace_back(label, c);
                    source = "nvme";
                } else if (hwmon_name == "acpitz") {
                    label = "Материнская плата";
                    data.sensors.emplace_back(label, c);
                    source = "acpitz";
                } else if (hwmon_name == "nct6687") {
                    if (i == 1) {
                        label = "Чипсет";
                        data.sensors.emplace_back(label, c);
                        source = "nct6687";
                    } else if (i >= 2 && i <= 7) {
                        label = "Датчик " + std::to_string(i);
                        data.sensors.emplace_back(label, c);
                        source = "nct6687";
                    } else {
                        source = "";
                    }
                } else if (hwmon_name == "drivetemp") {
                    label = "SSD/SDD";
                    data.sensors.emplace_back(label, c);
                    source = "drivetemp";
                } else {
                    source = "";
                }

                if (!source.empty()) {
                    auto max_str = read_file(temp_max_file);
                    if (!max_str.empty()) {
                        data.max_sensors[label] = std::stof(max_str) / 1000.0f;
                    }

                    auto crit_str = read_file(temp_crit_file);
                    if (!crit_str.empty()) {
                        data.crit_sensors[label] = std::stof(crit_str) / 1000.0f;
                    }
                }

                auto max_str = read_file(temp_max_file);
                if (!max_str.empty()) {
                    data.max_sensors[label] = std::stof(max_str) / 1000.0f;
                }

                auto crit_str = read_file(temp_crit_file);
                if (!crit_str.empty()) {
                    data.crit_sensors[label] = std::stof(crit_str) / 1000.0f;
                }

            } catch (...) {
                break;
            }
        }
    }

    // NVMe из thermal zones
    fs::path thermal_dir("/sys/class/thermal");
    if (fs::exists(thermal_dir)) {
        for (const auto& tz : fs::directory_iterator(thermal_dir)) {
            auto type = read_full(tz.path() / "type");
            auto temp_str = read_file(tz.path() / "temp");
            if (type.rfind("nvme", 0) == 0 && !temp_str.empty()) {
                try {
                    float c = std::stoi(temp_str) / 1000.0f;
                    data.sensors.emplace_back(type, c);
                } catch (...) {}
            }
        }
    }

    std::sort(data.cores.begin(), data.cores.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(data.sensors.begin(), data.sensors.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });

    return data;
}

// ------------------------------------------------------------------
// Вентиляторы
// ------------------------------------------------------------------

FanData read_fans() {
    FanData data;
    fs::path hwmon_dir("/sys/class/hwmon");
    if (!fs::exists(hwmon_dir)) return data;

    for (const auto& entry : fs::directory_iterator(hwmon_dir)) {
        auto name = entry.path().filename().string();
        if (name.rfind("hwmon", 0) != 0) continue;

        fs::path base = entry.path();
        std::string hwmon_name = read_full(base / "name");
        if (hwmon_name.empty()) continue;

        for (int i = 1; ; ++i) {
            auto fan_file = base / ("fan" + std::to_string(i) + "_input");
            auto val_str = read_file(fan_file);
            if (val_str.empty()) break;

            try {
                int rpm = std::stoi(val_str);
                if (rpm > 0) {
                    std::string label;
                    if (hwmon_name == "nct6687") {
                        if (i == 1) label = "Вентилятор 1";
                        else if (i == 2) label = "Вентилятор 2";
                        else if (i == 3) label = "Вентилятор 3";
                        else label = "Вентилятор " + std::to_string(i);
                    } else if (hwmon_name == "coretemp") {
                        label = "Вентилятор CPU";
                    } else {
                        label = hwmon_name + " fan" + std::to_string(i);
                    }
                    data.fans.emplace_back(label, rpm);
                }
            } catch (...) {
                break;
            }
        }
    }

    return data;
}

// ------------------------------------------------------------------
// CPU
// ------------------------------------------------------------------

static void read_cpuinfo(CpuInfo& info) {
    std::ifstream f("/proc/cpuinfo");
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);

        auto eq = line.find(":");
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "model name" && info.model.empty()) {
            info.model = val;
        } else if (key == "cpu cores") {
            if (info.physical_cores == 0) {
                info.physical_cores = std::stoi(val);
            }
        } else if (key == "cpu MHz") {
            info.current_mhz.push_back(std::stof(val));
        } else if (key == "processor") {
            info.logical_cores++;
        }
    }
}

static void read_scaling_freq(CpuInfo& info) {
    fs::path cpu0("/sys/devices/system/cpu/cpu0/cpufreq");
    if (!fs::exists(cpu0)) return;

    auto cur = read_file(cpu0 / "scaling_cur_freq");
    if (!cur.empty()) {
        int freq_khz = std::stoi(cur);
        info.current_mhz.push_back(freq_khz / 1000.0f);
    }

    auto max_f = read_file(cpu0 / "scaling_max_freq");
    if (!max_f.empty()) {
        info.max_mhz = std::stoi(max_f) / 1000.0f;
    }

    auto min_f = read_file(cpu0 / "scaling_min_freq");
    if (!min_f.empty()) {
        info.min_mhz = std::stoi(min_f) / 1000.0f;
    }

    auto gov = read_full(cpu0 / "scaling_governor");
    if (!gov.empty()) {
        info.governor = gov;
    }

    auto drv = read_full(cpu0 / "scaling_driver");
    if (!drv.empty()) {
        info.driver = drv;
    }
}

CpuInfo read_cpu_info() {
    CpuInfo info;
    read_cpuinfo(info);
    read_scaling_freq(info);
    return info;
}

// ------------------------------------------------------------------
// Загрузка CPU
// ------------------------------------------------------------------

static CpuLoad read_cpu_load_raw() {
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return {};

    std::string line;
    std::getline(f, line);

    std::istringstream iss(line);
    std::string cpu_tag;
    iss >> cpu_tag;

    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

    CpuLoad load{};
    load.user = user;
    load.nice = nice;
    load.system = system;
    load.idle = idle;
    load.iowait = iowait;
    load.irq = irq;
    load.softirq = softirq;
    load.steal = steal;

    uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
    load.total = total;
    load.active = total - idle - iowait;

    return load;
}

static CpuLoad read_cpu_load_per_core() {
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return {};

    std::string line;
    std::getline(f, line); // skip first line

    CpuLoad total;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string tag;
        iss >> tag;
        if (tag.rfind("cpu", 0) != 0 || tag == "cpu") continue;

        uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
        iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

        uint64_t t = user + nice + system + idle + iowait + irq + softirq + steal;
        total.per_core.emplace_back(t - idle - iowait, t);
    }

    return total;
}

CpuLoad read_cpu_load() {
    auto load = read_cpu_load_raw();

    if (load.total > 0) {
        load.active_percent = static_cast<float>(load.active) / static_cast<float>(load.total) * 100.0f;
        load.user_percent = static_cast<float>(load.user + load.nice) / static_cast<float>(load.total) * 100.0f;
        load.system_percent = static_cast<float>(load.system) / static_cast<float>(load.total) * 100.0f;
        load.idle_percent = static_cast<float>(load.idle) / static_cast<float>(load.total) * 100.0f;
        load.iowait_percent = static_cast<float>(load.iowait) / static_cast<float>(load.total) * 100.0f;
    }

    load.per_core = read_cpu_load_per_core().per_core;

    std::ifstream avg("/proc/loadavg");
    if (avg) {
        avg >> load.load1 >> load.load5 >> load.load15;
    }

    return load;
}

// ------------------------------------------------------------------
// Память
// ------------------------------------------------------------------

MemInfo read_memory() {
    MemInfo info;
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return info;

    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find(":");
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val_str = trim(line.substr(eq + 1));

        auto kb_pos = val_str.find("kB");
        if (kb_pos != std::string::npos) val_str = val_str.substr(0, kb_pos);

        try {
            uint64_t kb = std::stoull(val_str);
            uint64_t bytes = kb * 1024;

            if (key == "MemTotal") info.total = bytes;
            else if (key == "MemFree") info.free = bytes;
            else if (key == "MemAvailable") info.available = bytes;
            else if (key == "Buffers") info.buffers = bytes;
            else if (key == "Cached") info.cached = bytes;
            else if (key == "SwapTotal") info.swap_total = bytes;
            else if (key == "SwapFree") info.swap_free = bytes;
        } catch (...) {}
    }

    info.used = info.total - info.free - info.buffers - info.cached;
    if (info.used > info.total) info.used = 0;
    info.swap_used = info.swap_total - info.swap_free;
    info.available = info.available > 0 ? info.available : (info.free + info.buffers + info.cached);

    if (info.total > 0) {
        info.used_percent = static_cast<float>(info.used) / static_cast<float>(info.total) * 100.0f;
    }
    if (info.swap_total > 0) {
        info.swap_percent = static_cast<float>(info.swap_used) / static_cast<float>(info.swap_total) * 100.0f;
    }

    return info;
}

// ------------------------------------------------------------------
// Диск
// ------------------------------------------------------------------

DiskInfo read_disk_info() {
    DiskInfo info;

    std::ifstream f("/proc/diskstats");
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream iss(line);
            uint32_t major, minor;
            std::string name;
            uint64_t reads, merged_reads, sectors_read, write_time;
            uint64_t writes, merged_writes, sectors_written, io_in_progress;
            uint64_t io_time, io_time2;

            iss >> major >> minor >> name;
            if (name.empty()) continue;

            iss >> reads >> merged_reads >> sectors_read >> write_time
                >> writes >> merged_writes >> sectors_written >> io_in_progress
                >> io_time >> io_time2;

            if (reads == 0 && writes == 0) continue;

            auto& d = info.disks[name];
            d.name = name;
            d.read_sectors = sectors_read;
            d.write_sectors = sectors_written;
            d.read_ios = reads;
            d.write_ios = writes;
            d.read_bytes = sectors_read * 512;
            d.write_bytes = sectors_written * 512;
        }
    }

    std::ifstream mounts("/proc/mounts");
    if (mounts.is_open()) {
        std::string line;
        while (std::getline(mounts, line)) {
            std::istringstream iss(line);
            std::string device, mountpoint, fstype;
            iss >> device >> mountpoint >> fstype;

            if (fstype != "ext4" && fstype != "xfs" && fstype != "btrfs" &&
                fstype != "f2fs" && fstype != "ntfs" && fstype != "vfat" &&
                fstype != "exfat" && fstype != "zfs") {
                continue;
            }

            auto dev_name = device;
            if (dev_name.rfind("/dev/", 0) == 0) {
                dev_name = dev_name.substr(5);
            }

            auto& m = info.mounts[mountpoint];
            m.mountpoint = mountpoint;
            m.device = device;
            m.fstype = fstype;

            struct statvfs sv;
            if (statvfs(mountpoint.c_str(), &sv) == 0) {
                m.total_bytes = sv.f_frsize * sv.f_blocks;
                m.used_bytes = sv.f_frsize * (sv.f_blocks - sv.f_bfree);
            }

            if (auto it = info.disks.find(dev_name); it != info.disks.end()) {
                m.read_bytes = it->second.read_bytes;
                m.write_bytes = it->second.write_bytes;
            }
        }
    }

    fs::path block_dir("/sys/block");
    if (fs::exists(block_dir)) {
        for (const auto& disk : fs::directory_iterator(block_dir)) {
            auto disk_name = disk.path().filename().string();
            auto size_file = disk.path() / "size";
            auto size_str = read_file(size_file);
            if (!size_str.empty()) {
                try {
                    uint64_t sectors = std::stoull(size_str);
                    info.disks[disk_name].size_bytes = sectors * 512;
                    info.disks[disk_name].name = disk_name;
                } catch (...) {}
            }
        }
    }

    return info;
}

// ------------------------------------------------------------------
// Оборудование
// ------------------------------------------------------------------

HardwareInfo read_hardware_info() {
    HardwareInfo info;

    auto read_dmi = [](const char* file) -> std::string {
        fs::path p("/sys/class/dmi/id/");
        p /= file;
        return read_full(p);
    };

    info.board_vendor = read_dmi("board_vendor");
    info.board_name = read_dmi("board_name");
    info.bios_vendor = read_dmi("bios_vendor");
    info.bios_version = read_dmi("bios_version");
    info.bios_date = read_dmi("bios_date");
    info.product_name = read_dmi("product_name");
    info.serial_number = read_dmi("serial_number");

    fs::path bat_dir("/sys/class/power_supply/BAT0");
    if (fs::exists(bat_dir)) {
        info.battery.capacity = std::stoi(read_file(bat_dir / "capacity"));
        info.battery.full_design = std::stoi(read_file(bat_dir / "energy_full_design"));
        info.battery.full_now = std::stoi(read_file(bat_dir / "energy_full"));
        info.battery.status = read_full(bat_dir / "status");
    }

    auto run_lspci = []() -> std::string {
        FILE* pipe = popen("lspci 2>/dev/null", "r");
        if (!pipe) return {};
        std::string result;
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) {
            result += buf;
        }
        pclose(pipe);
        return result;
    }();

    std::istringstream gpu_stream(run_lspci);
    std::string line;
    while (std::getline(gpu_stream, line)) {
        if (line.find("VGA") != std::string::npos || line.find("3D") != std::string::npos ||
            line.find("Display") != std::string::npos) {
            info.gpu.push_back(trim(line.substr(line.find(":") + 1)));
        }
    }

    fs::path net_dir("/sys/class/net");
    if (fs::exists(net_dir)) {
        for (const auto& iface : fs::directory_iterator(net_dir)) {
            std::string name = iface.path().filename().string();
            if (name == "lo") continue;

            auto& n = info.network[name];
            n.name = name;
            n.type = read_full(iface.path() / "type");
            n.mac = read_full(iface.path() / "address");

            auto speed = read_file(iface.path() / "speed");
            if (!speed.empty()) {
                n.speed_mbps = std::stoi(speed);
            }

            auto rx = read_file(iface.path() / "statistics/rx_bytes");
            auto tx = read_file(iface.path() / "statistics/tx_bytes");
            if (!rx.empty()) n.rx_bytes = std::stoull(rx);
            if (!tx.empty()) n.tx_bytes = std::stoull(tx);
        }
    }

    info.pci_device_count = std::count(run_lspci.begin(), run_lspci.end(), '\n');

    return info;
}

// ------------------------------------------------------------------
// Сбор всех данных
// ------------------------------------------------------------------

SystemStatus read_all() {
    SystemStatus status;
    status.cpu = read_cpu_info();
    status.load = read_cpu_load();
    status.temp = read_temperatures();
    status.fans = read_fans();
    status.mem = read_memory();
    status.disk = read_disk_info();
    status.hw = read_hardware_info();
    status.timestamp = std::chrono::system_clock::now();
    return status;
}

} // namespace sys_info::collector
