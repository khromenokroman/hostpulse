#pragma once

#include <string>
#include <functional>
#include <vector>

namespace sys_info::server {

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
bool start_server(int port = 8080, const std::vector<Route>& routes = {});

/** @brief Остановить сервер; можно вызывать из обработчика сигнала */
void stop_server();

} // namespace sys_info::server
