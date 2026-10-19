#include "server.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

#include "collector.h"
#include "web_ui.h"

namespace hostpulse::server {

static std::atomic<bool> g_running{false};
static std::unique_ptr<httplib::Server> g_server;

static std::mutex g_snapshot_mutex;
static nlohmann::json g_snapshot;

/** @brief Интервал замеров; от него зависит окно усреднения загрузки CPU и скорости сети */
static constexpr auto kSampleInterval = std::chrono::seconds(1);

static void take_sample(collector::Sampler& sampler) {
    nlohmann::json j = sampler.sample();
    std::lock_guard lock(g_snapshot_mutex);
    g_snapshot = std::move(j);
}

static void sampler_loop(collector::Sampler sampler) {
    while (g_running) {
        // спим короткими шагами, чтобы stop_server() не ждал целый интервал
        auto wake = std::chrono::steady_clock::now() + kSampleInterval;
        while (g_running && std::chrono::steady_clock::now() < wake) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!g_running) break;
        try {
            take_sample(sampler);
        } catch (const std::exception& e) {
            // оставляем прошлый снимок, следующий замер попробует снова
            std::cerr << "Ошибка сбора данных: " << e.what() << std::endl;
        }
    }
}

/** @brief Часть снимка по ключу ("cpu", "mem", ...) или весь снимок для пустого ключа */
static std::string snapshot_part(const std::string& key) {
    std::lock_guard lock(g_snapshot_mutex);
    if (key.empty()) return g_snapshot.dump(2);
    auto it = g_snapshot.find(key);
    return it == g_snapshot.end() ? "null" : it->dump(2);
}

bool start_server(int port, const std::vector<Route>& extra_routes, const std::string& host) {
    g_server = std::make_unique<httplib::Server>();
    // httplib по умолчанию ставит SO_REUSEPORT: тогда второй экземпляр молча слушает
    // тот же порт и делит с первым запросы. Оставляем только SO_REUSEADDR.
    g_server->set_socket_options([](socket_t sock) {
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    });
    g_running = true;

    // первый замер до listen(), чтобы API сразу отдавал данные
    collector::Sampler sampler;
    take_sample(sampler);
    std::thread sampler_thread(sampler_loop, std::move(sampler));

    static const std::pair<const char*, const char*> kApi[] = {
        {"/api/cpu", "cpu"},   {"/api/load", "load"}, {"/api/temp", "temp"}, {"/api/fans", "fans"}, {"/api/mem", "mem"},
        {"/api/disk", "disk"}, {"/api/hw", "hw"},     {"/api/all", ""},      {"/api/health", ""},
    };
    for (const auto& [path, key] : kApi) {
        g_server->Get(path, [key = std::string(key)](const httplib::Request&, httplib::Response& res) {
            res.set_content(snapshot_part(key), "application/json");
        });
    }

    g_server->Get("/", [](const httplib::Request&, httplib::Response& res) { res.set_content(std::string(kIndexHtml), "text/html; charset=utf-8"); });

    for (auto& r : extra_routes) {
        g_server->Get(r.path, [h = r.handler, html = r.is_html](const httplib::Request&, httplib::Response& res) {
            res.set_content(h(), html ? "text/html; charset=utf-8" : "application/json");
        });
    }

    bool ok = g_running && g_server->listen(host, port);

    g_running = false;
    sampler_thread.join();
    return ok;
}

void stop_server() {
    g_running = false;
    if (g_server) {
        g_server->stop();
    }
}

} // namespace hostpulse::server
