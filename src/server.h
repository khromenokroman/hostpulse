#pragma once

#include <functional>
#include <string>
#include <vector>

namespace hostpulse::server {

using Handler = std::function<std::string()>;

struct Route {
    std::string path;
    Handler handler;
    bool is_html = false;
};

/**
 * @brief Запустить HTTP-сервер (блокирует до stop_server())
 *
 * Вместе с сервером стартует фоновый поток, который раз в секунду снимает
 * данные через collector::Sampler; API отдаёт последний снимок.
 */
bool start_server(int port = 8080, const std::vector<Route>& routes = {}, const std::string& host = "0.0.0.0");

/** @brief Остановить сервер; можно вызывать из обработчика сигнала */
void stop_server();

} // namespace hostpulse::server
