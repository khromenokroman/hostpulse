#pragma once

#include <string>
#include <functional>

namespace sys_info::server {

using Handler = std::function<std::string()>;

struct Route {
    std::string path;
    Handler handler;
    bool is_html = false;
};

bool start_server(int port = 8080, const std::vector<Route>& routes = {});
void stop_server();

} // namespace sys_info::server
