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

namespace hostpulse::collector {

// ------------------------------------------------------------------
// Утилиты
// ------------------------------------------------------------------

/** @brief Первое слово файла (для числовых значений sysfs) */
static std::string read_word(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::string s;
    f >> s;
    return s;
}

/** @brief Первая строка файла целиком (для имён и меток, в них бывают пробелы) */
static std::string read_line(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line);
    return line;
}

/** @brief Целое из файла; 0, если файла нет или там не число */
static int read_int(const fs::path& p) {
    try {
        return std::stoi(read_word(p));
    } catch (...) {
        return 0;
    }
}

static float celsius_from_millideg(int32_t millideg) {
    return millideg / 1000.0f;
}

/**
 * @brief Номера N всех файлов <prefix>N_input в каталоге hwmon, по возрастанию
 *
 * Номера идут не подряд (у coretemp: temp1, temp10, temp14, ...),
 * поэтому перебирать 1, 2, 3... до первого пропуска нельзя.
 */
static std::vector<int> hwmon_indices(const fs::path& base, const std::string& prefix) {
    std::vector<int> result;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(base, ec)) {
        auto name = e.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || !name.ends_with("_input")) continue;
        auto num = name.substr(prefix.size(), name.size() - prefix.size() - 6);
        if (num.empty() || !std::all_of(num.begin(), num.end(), ::isdigit)) continue;
        result.push_back(std::stoi(num));
    }
    std::sort(result.begin(), result.end());
    return result;
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
        std::string hwmon_name = read_line(base / "name");
        if (hwmon_name.empty()) continue;

        for (int i : hwmon_indices(base, "temp")) {
            auto idx = "temp" + std::to_string(i);
            auto val_str = read_word(base / (idx + "_input"));
            if (val_str.empty()) continue;

            try {
                float c = celsius_from_millideg(std::stoi(val_str));
                auto temp_label = read_line(base / (idx + "_label"));
                std::string label;

                if (hwmon_name == "coretemp") {
                    if (temp_label.rfind("Package", 0) == 0) {
                        label = "Температура пакета";
                        data.package.emplace_back(label, c);
                    } else {
                        label = temp_label.empty() ? ("Ядро " + std::to_string(i - 1)) : temp_label;
                        data.cores.emplace_back(label, c);
                    }
                } else if (hwmon_name == "nvme") {
                    label = temp_label.empty() ? "NVMe" : temp_label;
                    data.sensors.emplace_back(label, c);
                } else if (hwmon_name == "acpitz") {
                    label = "Материнская плата";
                    data.sensors.emplace_back(label, c);
                } else if (hwmon_name == "nct6687") {
                    if (i == 1) {
                        label = temp_label.empty() ? "Чипсет" : temp_label;
                    } else if (i <= 7) {
                        label = temp_label.empty() ? ("Датчик " + std::to_string(i)) : temp_label;
                    } else {
                        continue;
                    }
                    data.sensors.emplace_back(label, c);
                } else if (hwmon_name == "drivetemp") {
                    label = temp_label.empty() ? "SSD/SDD" : temp_label;
                    data.sensors.emplace_back(label, c);
                } else {
                    label = temp_label.empty() ? hwmon_name : temp_label;
                    data.sensors.emplace_back(label, c);
                }

                auto max_str = read_word(base / (idx + "_max"));
                if (!max_str.empty()) {
                    data.max_sensors[label] = std::stof(max_str) / 1000.0f;
                }

                auto crit_str = read_word(base / (idx + "_crit"));
                if (!crit_str.empty()) {
                    data.crit_sensors[label] = std::stof(crit_str) / 1000.0f;
                }
            } catch (...) {
                continue;
            }
        }
    }

    // NVMe из thermal zones
    fs::path thermal_dir("/sys/class/thermal");
    if (fs::exists(thermal_dir)) {
        for (const auto& tz : fs::directory_iterator(thermal_dir)) {
            auto type = read_line(tz.path() / "type");
            auto temp_str = read_word(tz.path() / "temp");
            if (type.rfind("nvme", 0) == 0 && !temp_str.empty()) {
                try {
                    float c = std::stoi(temp_str) / 1000.0f;
                    data.sensors.emplace_back(type, c);
                } catch (...) {}
            }
        }
    }

    std::sort(data.cores.begin(), data.cores.end(), [](const auto& a, const auto& b) {
        return a.id.size() != b.id.size() ? a.id.size() < b.id.size() : a.id < b.id;
    });
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
        std::string hwmon_name = read_line(base / "name");
        if (hwmon_name.empty()) continue;

        for (int i : hwmon_indices(base, "fan")) {
            auto idx = "fan" + std::to_string(i);
            try {
                int rpm = std::stoi(read_word(base / (idx + "_input")));
                if (rpm <= 0) continue;

                auto fan_label = read_line(base / (idx + "_label"));
                std::string label;
                if (hwmon_name == "nct6687") {
                    label = fan_label.empty() ? ("Вентилятор " + std::to_string(i)) : fan_label;
                } else if (hwmon_name == "coretemp") {
                    label = "Вентилятор CPU";
                } else {
                    label = fan_label.empty() ? (hwmon_name + " fan" + std::to_string(i)) : fan_label;
                }
                data.fans.emplace_back(label, rpm);
            } catch (...) {
                continue;
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

    auto cur = read_word(cpu0 / "scaling_cur_freq");
    if (!cur.empty()) {
        int freq_khz = std::stoi(cur);
        info.current_mhz.push_back(freq_khz / 1000.0f);
    }

    auto max_f = read_word(cpu0 / "scaling_max_freq");
    if (!max_f.empty()) {
        info.max_mhz = std::stoi(max_f) / 1000.0f;
    }

    auto min_f = read_word(cpu0 / "scaling_min_freq");
    if (!min_f.empty()) {
        info.min_mhz = std::stoi(min_f) / 1000.0f;
    }

    auto gov = read_line(cpu0 / "scaling_governor");
    if (!gov.empty()) {
        info.governor = gov;
    }

    auto drv = read_line(cpu0 / "scaling_driver");
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

    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
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

        uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

        uint64_t t = user + nice + system + idle + iowait + irq + softirq + steal;
        int core_id = std::stoi(tag.substr(3));
        uint64_t active = t - idle - iowait;
        float percent = t > 0 ? static_cast<float>(active) / static_cast<float>(t) * 100.0f : 0.0f;
        total.per_core.push_back({core_id, active, t, percent});
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
            uint32_t major = 0, minor = 0;
            std::string name;
            uint64_t reads = 0, merged_reads = 0, sectors_read = 0, read_time = 0;
            uint64_t writes = 0, merged_writes = 0, sectors_written = 0, write_time = 0;

            iss >> major >> minor >> name;
            if (name.empty()) continue;

            iss >> reads >> merged_reads >> sectors_read >> read_time
                >> writes >> merged_writes >> sectors_written >> write_time;

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
            auto size_str = read_word(size_file);
            if (!size_str.empty()) {
                try {
                    uint64_t sectors = std::stoull(size_str);
                    info.disks[disk_name].size_bytes = sectors * 512;
                    info.disks[disk_name].name = disk_name;
                } catch (...) {}
            }

            // физический диск: есть ссылка device на контроллер (у loop, dm, zram её нет)
            if (fs::exists(disk.path() / "device")) {
                auto& d = info.disks[disk_name];
                d.name = disk_name;
                d.model = trim(read_line(disk.path() / "device/model"));
                if (disk_name.rfind("nvme", 0) == 0) {
                    d.kind = "NVMe";
                } else {
                    d.kind = read_word(disk.path() / "queue/rotational") == "1" ? "HDD" : "SSD";
                }
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
        return read_line(p);
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
        const char* kind = fs::exists(bat_dir / "energy_full") ? "energy" : "charge";
        info.battery.capacity = read_int(bat_dir / "capacity");
        info.battery.full_design = read_int(bat_dir / (std::string(kind) + "_full_design"));
        info.battery.full_now = read_int(bat_dir / (std::string(kind) + "_full"));
        info.battery.status = read_line(bat_dir / "status");
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
            n.type = read_line(iface.path() / "type");
            n.mac = read_line(iface.path() / "address");

            // у Wi-Fi и опущенных интерфейсов speed не читается или равен -1
            n.speed_mbps = std::max(read_int(iface.path() / "speed"), 0);

            try {
                n.rx_bytes = std::stoull(read_word(iface.path() / "statistics/rx_bytes"));
                n.tx_bytes = std::stoull(read_word(iface.path() / "statistics/tx_bytes"));
            } catch (...) {}
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

// ------------------------------------------------------------------
// Sampler
// ------------------------------------------------------------------

static float delta_percent(uint64_t cur, uint64_t prev, uint64_t total_delta) {
    if (total_delta == 0 || cur < prev) return 0.0f;
    return static_cast<float>(cur - prev) / static_cast<float>(total_delta) * 100.0f;
}

static void apply_load_delta(CpuLoad& cur, const CpuLoad& prev) {
    if (cur.total > prev.total) {
        uint64_t dt = cur.total - prev.total;
        cur.active_percent = delta_percent(cur.active, prev.active, dt);
        cur.user_percent = delta_percent(cur.user + cur.nice, prev.user + prev.nice, dt);
        cur.system_percent = delta_percent(cur.system, prev.system, dt);
        cur.idle_percent = delta_percent(cur.idle, prev.idle, dt);
        cur.iowait_percent = delta_percent(cur.iowait, prev.iowait, dt);
    }

    for (auto& core : cur.per_core) {
        auto it = std::find_if(prev.per_core.begin(), prev.per_core.end(),
                               [&](const auto& p) { return p.id == core.id; });
        if (it == prev.per_core.end() || core.total <= it->total) continue;
        core.percent = delta_percent(core.active, it->active, core.total - it->total);
    }
}

SystemStatus Sampler::sample() {
    auto status = read_all();
    auto now = std::chrono::steady_clock::now();

    if (has_prev_) {
        apply_load_delta(status.load, prev_load_);

        double seconds = std::chrono::duration<double>(now - prev_time_).count();
        for (auto& [name, n] : status.hw.network) {
            auto it = prev_net_.find(name);
            if (seconds <= 0 || it == prev_net_.end()) continue;
            // счётчики сбрасываются при пересоздании интерфейса
            if (n.rx_bytes < it->second.first || n.tx_bytes < it->second.second) continue;
            n.prev_rx = it->second.first;
            n.prev_tx = it->second.second;
            n.rate_rx_mbps = static_cast<float>((n.rx_bytes - n.prev_rx) * 8.0 / seconds / 1e6);
            n.rate_tx_mbps = static_cast<float>((n.tx_bytes - n.prev_tx) * 8.0 / seconds / 1e6);
        }
    }

    prev_load_ = status.load;
    prev_net_.clear();
    for (const auto& [name, n] : status.hw.network) {
        prev_net_[name] = {n.rx_bytes, n.tx_bytes};
    }
    prev_time_ = now;
    has_prev_ = true;
    return status;
}

} // namespace hostpulse::collector
