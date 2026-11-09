#include "collector.h"

#include <sys/statvfs.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

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

static float celsius_from_millideg(int32_t millideg) { return millideg / 1000.0f; }

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

/** @brief Запустить команду и вернуть stdout; пусто при ошибке */
static std::string run_cmd(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string result;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

// ------------------------------------------------------------------
// Видеокарты
// ------------------------------------------------------------------

/** @brief PCI-адрес из /sys/class/drm/cardN/device: "00:02.0" из "0000:00:02.0" */
static std::string pci_address(const fs::path& device_dir) {
    auto target = fs::canonical(device_dir);
    std::string name = target.filename().string();
    if (name.rfind("0000:", 0) == 0) name = name.substr(5);
    return name;
}

/** @brief Модель из lspci по PCI-адресу; пусто, если lspci нет или адрес не найден */
static std::string lspci_name(const std::string& pci_addr) {
    auto out = run_cmd("lspci 2>/dev/null");
    if (out.empty()) return {};
    std::istringstream stream(out);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find(pci_addr) == 0) {
            auto colon = line.find(":");
            return (colon == std::string::npos) ? line : trim(line.substr(colon + 1));
        }
    }
    return {};
}

static void fill_gpu_hwmon(GpuInfo& gpu, const fs::path& device) {
    // hwmon-каталог драйвера (amdgpu, radeon): самая высокая температура, самый быстрый вентилятор
    fs::path hwmon = device / "hwmon";
    if (fs::exists(hwmon)) {
        for (const auto& entry : fs::directory_iterator(hwmon)) {
            for (int i : hwmon_indices(entry.path(), "temp")) {
                auto val_str = read_word(entry.path() / ("temp" + std::to_string(i) + "_input"));
                if (val_str.empty()) continue;
                try {
                    float c = celsius_from_millideg(std::stoi(val_str));
                    if (c > gpu.temp_celsius) gpu.temp_celsius = c;
                } catch (...) {
                }
            }
            for (int i : hwmon_indices(entry.path(), "fan")) {
                auto val_str = read_word(entry.path() / ("fan" + std::to_string(i) + "_input"));
                if (val_str.empty()) continue;
                try {
                    int rpm = std::stoi(val_str);
                    if (rpm > 0 && rpm != 255 && rpm != 127 && rpm != 31 && rpm != 15) {
                        // 255/127/31/15: драйвер не может прочитать обороты (AIB-карты без SMBus)
                        gpu.fan_rpm = rpm;
                    }
                } catch (...) {
                }
            }
        }
    }
}

/** @brief NVIDIA: энергия, память, обороты через nvidia-smi. fan.speed у пассивных карт (SXM, Tesla) = N/A: пропускаем. */
static void fill_gpu_nvidia(GpuInfo& gpu, const fs::path& device) {
    // PCI-адрес из sysfs: /sys/class/drm/cardN/device/uevent, строка PCI_SLOT_NAME=XXXX:XX:XX.X
    std::string pci_slot;
    std::ifstream uevent(device / "uevent");
    std::string line;
    while (std::getline(uevent, line)) {
        if (line.rfind("PCI_SLOT_NAME=", 0) == 0) {
            pci_slot = trim(line.substr(14));
            break;
        }
    }
    if (pci_slot.empty()) return;

    // Запрашиваем PCI-адреса всех NVIDIA GPU и ищем совпадение
    auto bus_ids = run_cmd("nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader 2>/dev/null");
    if (bus_ids.empty()) return;

    std::istringstream bs(bus_ids);
    int idx = 0;
    int nvidia_idx = -1;
    while (std::getline(bs, line)) {
        auto bus_id = trim(line);
        // нормализация: 00000000:01:00.0 -> 0000:01:00.0
        auto colon = bus_id.find(":");
        if (colon != std::string::npos && colon > 4) {
            bus_id = "0000:" + bus_id.substr(colon + 1);
        }
        if (bus_id == pci_slot) {
            nvidia_idx = idx;
            break;
        }
        idx++;
    }
    if (nvidia_idx < 0) return;

    auto one = run_cmd("nvidia-smi -i " + std::to_string(nvidia_idx) +
                       " --query-gpu=power.draw,power.limit,utilization.gpu,"
                       "memory.used,memory.total,fan.speed,temperature.gpu --format=csv,noheader,nounits 2>/dev/null");
    if (one.empty()) return;

    // Разделяем по запятым, сохраняя исходные индексы
    std::vector<std::pair<int, std::string>> fields;
    std::istringstream ls(one);
    std::string field;
    int fidx = 0;
    while (std::getline(ls, field, ',')) {
        field = trim(field);
        if (!field.empty() && field != "N/A" && field != "[N/A]") {
            fields.push_back({fidx, field});
        }
        fidx++;
    }

    for (const auto& [i, val] : fields) {
        try {
            if (i == 0)
                gpu.power_watts = std::stof(val);
            else if (i == 1)
                gpu.power_limit_watts = std::stof(val);
            else if (i == 2)
                gpu.busy_percent = std::stof(val);
            else if (i == 3)
                gpu.vram_used = static_cast<int64_t>(std::stod(val)) * 1024 * 1024;
            else if (i == 4)
                gpu.vram_total = static_cast<int64_t>(std::stod(val)) * 1024 * 1024;
            else if (i == 5)
                gpu.fan_rpm = std::stoi(val);
            else if (i == 6)
                gpu.temp_celsius = std::stof(val);
        } catch (...) {
        }
    }
}

/** @brief Debug: возвращает сырые результаты nvidia-smi для диагностики. */
std::string debug_nvidia_info() {
    nlohmann::json result;
    result["nvidia_bus_ids"] = run_cmd("nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader 2>&1");
    result["nvidia_list"] = run_cmd("nvidia-smi -L 2>&1");
    return result.dump();
}

std::vector<GpuInfo> read_gpu_info() {
    std::vector<GpuInfo> gpus;

    // /sys/class/drm/cardN: дисплейные устройства. computeN (NVIDIA MIG, Intel cGPU) не показываем.
    // NVIDIA в виртуалках может быть только в /dev/nvidiactl: сейчас не обрабатываем.
    fs::path drm_dir("/sys/class/drm");
    if (!fs::exists(drm_dir)) return gpus;

    for (const auto& entry : fs::directory_iterator(drm_dir)) {
        auto name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0) continue;
        if (!std::all_of(name.begin() + 4, name.end(), ::isdigit)) continue;

        fs::path card = entry.path();
        fs::path device = card / "device";
        if (!fs::exists(device)) continue; // виртуальные (virtio, vbox): только cardN без PCI

        GpuInfo g;
        g.index = std::stoi(name.substr(4));

        // драйвер: имя ссылки device/driver
        std::error_code ec;
        auto driver = fs::read_symlink(device / "driver", ec);
        if (!ec) g.driver = driver.filename().string();

        // модель из lspci по PCI-адресу; если lspci нет: имя драйвера
        auto pci = pci_address(device);
        g.name = lspci_name(pci);
        if (g.name.empty()) g.name = g.driver + " (card" + std::to_string(g.index) + ")";

        // данные по драйверу: /sys/class/drm/cardN/device/...
        auto read_file = [&](const char* file) -> std::string { return read_word(device / file); };
        auto read_int64 = [&](const char* file) -> int64_t {
            auto s = read_file(file);
            try {
                return s.empty() ? 0 : std::stoll(s);
            } catch (...) {
                return 0;
            }
        };

        if (g.driver == "amdgpu" || g.driver == "radeon") {
            g.vram_total = read_int64("mem_info_vram_total");
            g.vram_used = read_int64("mem_info_vram_used");
            auto power = read_file("power_power_consumption");
            if (!power.empty()) {
                try {
                    g.power_watts = std::stoll(power) / 1000.0f; // мВт
                } catch (...) {
                }
            }
            auto busy = read_file("gpu_busy_percent");
            if (!busy.empty()) {
                try {
                    g.busy_percent = std::stof(busy);
                } catch (...) {
                }
            }
            // режим питания: power_dpm_force_performance_level (amdgpu)
            auto profile = read_line(device / "power_dpm_force_performance_level");
            if (!profile.empty() && profile != "default") g.power_profile = profile;
        }

        if (g.driver == "i915" || g.driver == "xe") {
            auto busy = read_file("gpu_busy_percent");
            if (!busy.empty()) {
                try {
                    g.busy_percent = std::stof(busy);
                } catch (...) {
                }
            }
            // i915 не отдаёт vram_total: читаем из /sys/firmware/acpi (Intel HD/Audio)
            // Для Intel встроенной графики VRAM общий с RAM: показываем только если есть данные
        }

        if (g.driver == "nvidia") {
            fill_gpu_nvidia(g, device);
            // температура: /sys/class/thermal/thermal_zoneN (type=nvidia0)
            fs::path thermal_dir("/sys/class/thermal");
            if (fs::exists(thermal_dir)) {
                for (const auto& tz : fs::directory_iterator(thermal_dir)) {
                    auto type = read_line(tz.path() / "type");
                    if (type.rfind("nvidia", 0) != 0) continue;
                    auto val_str = read_word(tz.path() / "temp");
                    if (val_str.empty()) continue;
                    try {
                        float c = std::stoi(val_str) / 1000.0f;
                        if (c > g.temp_celsius) g.temp_celsius = c;
                    } catch (...) {
                    }
                }
            }
        }

        // температура и вентилятор: hwmon драйвера (cardN/device/hwmon/hwmon*/...).
        // Это общий путь для i915/xe/amdgpu/radeon; у NVIDIA hwmon под drm/device не регистрируется,
        // его температура идёт через /sys/class/thermal (см. выше, в блоке nvidia).
        fill_gpu_hwmon(g, device);

        gpus.push_back(std::move(g));
    }

    // по возрастанию индекса cardN
    std::sort(gpus.begin(), gpus.end(), [](const auto& a, const auto& b) { return a.index < b.index; });

    return gpus;
}

// ------------------------------------------------------------------
// Температура
// ------------------------------------------------------------------

/**
 * @brief Понятное имя датчика по драйверу hwmon и его метке
 *
 * Метки драйверов ("Composite", "Sensor 1", "edge") простому человеку ничего не говорят,
 * поэтому называем по устройству. Пустая строка: датчик не показываем.
 */
static std::string sensor_name(const std::string& hwmon, const std::string& label, int index) {
    if (hwmon == "nvme") {
        // "Composite": общая температура накопителя, "Sensor N": отдельные точки на плате
        if (label.empty() || label == "Composite") return "SSD";
        if (label.rfind("Sensor ", 0) == 0) return "SSD, датчик " + label.substr(7);
        return "SSD, " + label;
    }
    if (hwmon == "drivetemp") return "Диск";
    if (hwmon == "acpitz") return "Материнская плата";
    if (hwmon.rfind("iwlwifi", 0) == 0) return "Wi-Fi адаптер";
    if (hwmon.rfind("pch_", 0) == 0) return "Чипсет";
    if (hwmon == "amdgpu" || hwmon == "radeon" || hwmon == "nouveau" || hwmon == "i915" || hwmon == "xe") {
        if (label == "junction") return "Видеокарта, самое горячее место";
        if (label == "mem") return "Видеопамять";
        return "Видеокарта";
    }
    if (hwmon == "nct6687") {
        if (index == 1) return label.empty() ? "Чипсет" : label;
        if (index <= 7) return label.empty() ? ("Датчик " + std::to_string(index)) : label;
        return {};
    }
    return label.empty() ? hwmon : label;
}

/** @brief Одинаковым именам добавляет номер: "SSD", "SSD (2)": например, при двух накопителях */
static void make_unique_names(std::vector<TempSensor>& sensors) {
    std::map<std::string, int> count;
    for (auto& s : sensors) {
        int n = ++count[s.id];
        if (n > 1) s.id += " (" + std::to_string(n) + ")";
    }
}

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
                    // Intel: "Package id 0": весь процессор, "Core N": ядро
                    if (temp_label.rfind("Package", 0) == 0) {
                        label = "Процессор в целом";
                        data.package.emplace_back(label, c);
                    } else {
                        label = "Ядро " + (temp_label.rfind("Core ", 0) == 0 ? temp_label.substr(5) : std::to_string(i - 1));
                        data.cores.emplace_back(label, c);
                    }
                } else if (hwmon_name == "k10temp") {
                    // AMD: "Tctl": весь процессор, "TccdN": блок ядер (CCD), "Tdie" дублирует Tctl
                    if (temp_label.rfind("Tccd", 0) == 0) {
                        label = "Блок ядер " + temp_label.substr(4);
                        data.cores.emplace_back(label, c);
                    } else if (temp_label.empty() || temp_label == "Tctl") {
                        label = "Процессор в целом";
                        data.package.emplace_back(label, c);
                    } else {
                        continue;
                    }
                } else {
                    label = sensor_name(hwmon_name, temp_label, i);
                    if (label.empty()) continue;
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
                    data.sensors.emplace_back("SSD", c);
                } catch (...) {
                }
            }
        }
    }

    std::sort(data.cores.begin(), data.cores.end(),
              [](const auto& a, const auto& b) { return a.id.size() != b.id.size() ? a.id.size() < b.id.size() : a.id < b.id; });
    std::sort(data.sensors.begin(), data.sensors.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    // номера ядер Intel идут с пропусками (0, 4, 8, ... у гибридных): нумеруем по порядку с 1
    for (size_t k = 0; k < data.cores.size(); ++k) {
        if (data.cores[k].id.rfind("Ядро ", 0) == 0) data.cores[k].id = "Ядро " + std::to_string(k + 1);
    }
    make_unique_names(data.package);
    make_unique_names(data.cores);
    make_unique_names(data.sensors);

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
                if (hwmon_name == "coretemp") {
                    label = "Вентилятор процессора";
                } else {
                    label = fan_label.empty() ? ("Вентилятор " + std::to_string(i)) : fan_label;
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

            if (key == "MemTotal")
                info.total = bytes;
            else if (key == "MemFree")
                info.free = bytes;
            else if (key == "MemAvailable")
                info.available = bytes;
            else if (key == "Buffers")
                info.buffers = bytes;
            else if (key == "Cached")
                info.cached = bytes;
            else if (key == "SwapTotal")
                info.swap_total = bytes;
            else if (key == "SwapFree")
                info.swap_free = bytes;
        } catch (...) {
        }
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

            iss >> reads >> merged_reads >> sectors_read >> read_time >> writes >> merged_writes >> sectors_written >> write_time;

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

            if (fstype != "ext4" && fstype != "xfs" && fstype != "btrfs" && fstype != "f2fs" && fstype != "ntfs" && fstype != "vfat" &&
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
                } catch (...) {
                }
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
        if (line.find("VGA") != std::string::npos || line.find("3D") != std::string::npos || line.find("Display") != std::string::npos) {
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
            // физический адаптер: есть ссылка device на контроллер
            if (fs::exists(iface.path() / "device")) {
                n.kind = fs::exists(iface.path() / "wireless") ? "Wi-Fi" : "Ethernet";
            }

            // у Wi-Fi и опущенных интерфейсов speed не читается или равен -1
            n.speed_mbps = std::max(read_int(iface.path() / "speed"), 0);

            try {
                n.rx_bytes = std::stoull(read_word(iface.path() / "statistics/rx_bytes"));
                n.tx_bytes = std::stoull(read_word(iface.path() / "statistics/tx_bytes"));
            } catch (...) {
            }
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
    status.gpu = read_gpu_info();
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
        auto it = std::find_if(prev.per_core.begin(), prev.per_core.end(), [&](const auto& p) { return p.id == core.id; });
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
