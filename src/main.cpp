#include "server.h"
#include "collector.h"
#include <iostream>
#include <thread>
#include <csignal>
#include <cstdlib>

static void signal_handler(int sig) {
    (void)sig;
    sys_info::server::stop_server();
}

int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
        if (!port) port = 8080;
    }

    // Если есть флаг --dump, просто вывести данные и выйти
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--dump") {
            auto status = sys_info::collector::read_all();
            nlohmann::json j;
            nlohmann::to_json(j, status);
            std::cout << j.dump(2) << std::endl;
            return 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "sys_info server starting on port " << port << std::endl;
    std::cout << "Open http://localhost:" << port << " in browser" << std::endl;

    sys_info::server::start_server(port);

    return 0;
}
