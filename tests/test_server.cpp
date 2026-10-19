#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "server.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

namespace sys_info::server::test {

using ::testing::HasSubstr;
using ::testing::Not;

/**
 * @brief Поднимает настоящий сервер в отдельном потоке на время теста
 *
 * start_server() блокирует, поэтому он живёт в своём потоке; в конструкторе ждём,
 * пока сервер начнёт отвечать, в деструкторе останавливаем и дожидаемся потока.
 */
class ServerTest : public ::testing::Test {
protected:
    static constexpr int kPort = 18080;

    void SetUp() override {
        thread_ = std::thread([this] { listen_ok_ = start_server(kPort, {
            {"/extra", [] { return std::string(R"({"extra":true})"); }},
        }); });

        client_.set_connection_timeout(std::chrono::seconds(1));
        for (int i = 0; i < 100; ++i) {
            if (client_.Get("/api/health")) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        FAIL() << "сервер на порту " << kPort << " не ответил за 5 секунд";
    }

    void TearDown() override {
        stop_server();
        thread_.join();
        EXPECT_TRUE(listen_ok_) << "start_server() вернул false: порт " << kPort << " занят?";
    }

    /** @brief GET с проверкой статуса 200; возвращает тело */
    std::string get(const std::string& path, const std::string& expected_type = "application/json") {
        auto res = client_.Get(path);
        if (!res) {
            ADD_FAILURE() << "GET " << path << ": нет ответа (" << httplib::to_string(res.error()) << ")";
            return {};
        }
        EXPECT_EQ(res->status, 200) << "GET " << path;
        EXPECT_THAT(res->get_header_value("Content-Type"), HasSubstr(expected_type)) << "GET " << path;
        return res->body;
    }

    nlohmann::json get_json(const std::string& path) {
        auto body = get(path);
        try {
            return nlohmann::json::parse(body);
        } catch (const std::exception& e) {
            ADD_FAILURE() << "GET " << path << ": невалидный JSON (" << e.what() << "):\n" << body.substr(0, 300);
            return {};
        }
    }

    httplib::Client client_{"127.0.0.1", kPort};

private:
    std::thread thread_;
    bool listen_ok_ = false;
};

/** Главная страница: HTML с дашбордом, который сам забирает /api/all. */
TEST_F(ServerTest, IndexPageServesDashboard) {
    auto html = get("/", "text/html");
    EXPECT_THAT(html, HasSubstr("<!DOCTYPE html>"));
    EXPECT_THAT(html, HasSubstr("<title>System Info</title>"));
    EXPECT_THAT(html, HasSubstr("fetch('/api/all'")) << "дашборд должен забирать данные из /api/all";
    EXPECT_THAT(html, HasSubstr("</script>"));
    EXPECT_THAT(html, Not(HasSubstr("__DATA__"))) << "данные не должны вшиваться в страницу";
}

/** /api/all: все разделы, которые рисует дашборд, на месте. */
TEST_F(ServerTest, ApiAllHasEverySection) {
    auto j = get_json("/api/all");
    for (const char* key : {"cpu", "load", "temp", "fans", "mem", "disk", "hw", "timestamp"}) {
        EXPECT_TRUE(j.contains(key)) << "в /api/all нет раздела '" << key << "'";
    }
    // снимок обновляется раз в секунду, поэтому сравниваем состав, а не значения
    auto keys = [](const nlohmann::json& o) {
        std::vector<std::string> k;
        for (const auto& [key, v] : o.items()) k.push_back(key);
        return k;
    };
    EXPECT_EQ(keys(get_json("/api/health")), keys(j)) << "/api/health должен отдавать то же, что /api/all";
}

/** Отдельные эндпоинты отдают свой раздел снимка, а не весь снимок. */
TEST_F(ServerTest, SectionEndpoints) {
    auto cpu = get_json("/api/cpu");
    EXPECT_TRUE(cpu.contains("model"));
    EXPECT_GT(cpu.value("logical_cores", 0), 0);

    auto mem = get_json("/api/mem");
    EXPECT_GT(mem.value("total", uint64_t{0}), 0u);

    for (const char* path : {"/api/temp", "/api/fans", "/api/disk", "/api/hw"}) {
        SCOPED_TRACE(path);
        auto j = get_json(path);
        EXPECT_TRUE(j.is_object()) << "ожидался объект, получено: " << j.dump().substr(0, 200);
        EXPECT_FALSE(j.contains("cpu")) << "эндпоинт вернул весь снимок вместо раздела";
    }
}

/** Загрузка по ядрам — проценты за интервал между замерами, в пределах 0..100. */
TEST_F(ServerTest, LoadPerCorePercent) {
    // ждём второй замер фонового потока, чтобы проценты были уже за интервал
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto load = get_json("/api/load");
    ASSERT_TRUE(load.contains("per_core") && load["per_core"].is_array()) << load.dump().substr(0, 300);
    ASSERT_FALSE(load["per_core"].empty());
    for (const auto& c : load["per_core"]) {
        SCOPED_TRACE("CPU" + std::to_string(c.value("id", -1)));
        ASSERT_TRUE(c.contains("percent"));
        double p = c["percent"].get<double>();
        EXPECT_GE(p, 0.0);
        EXPECT_LE(p, 100.0);
    }
    double active = load.value("active_percent", -1.0);
    EXPECT_GE(active, 0.0);
    EXPECT_LE(active, 100.0);
}

/** Сеть: у каждого интерфейса есть поля, которые рисует плитка «Сеть». */
TEST_F(ServerTest, NetworkHasRateFields) {
    auto hw = get_json("/api/hw");
    ASSERT_TRUE(hw.contains("network"));
    for (const auto& [name, net] : hw["network"].items()) {
        SCOPED_TRACE(name);
        for (const char* key : {"name", "mac", "speed_mbps", "rate_rx_mbps", "rate_tx_mbps"}) {
            EXPECT_TRUE(net.contains(key)) << "нет поля '" << key << "'";
        }
        EXPECT_GE(net.value("speed_mbps", -1), 0);
    }
}

/** Дополнительные маршруты из start_server() подключаются. */
TEST_F(ServerTest, ExtraRoute) {
    auto j = get_json("/extra");
    EXPECT_EQ(j, nlohmann::json({{"extra", true}}));
}

/** Неизвестный путь — 404, а не дашборд или пустой JSON. */
TEST_F(ServerTest, UnknownPathIs404) {
    auto res = client_.Get("/api/nope");
    ASSERT_TRUE(res) << "нет ответа";
    EXPECT_EQ(res->status, 404);
}

} // namespace sys_info::server::test
