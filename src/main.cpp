#include "server.h"
#include "collector.h"
#include <iostream>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <string>

static void signal_handler(int sig) {
    (void)sig;
    hostpulse::server::stop_server();
}

static void print_usage(const char* prog) {
    std::cout << "hostpulse — состояние компьютера в браузере: нагрузка, температуры, память, диски, сеть\n"
                 "\n"
                 "Использование:\n"
                 "  " << prog << " [ПОРТ]    запустить веб-дашборд (порт по умолчанию 8080)\n"
                 "  " << prog << " --dump    вывести все данные в JSON и выйти\n"
                 "  " << prog << " --help    показать эту справку\n";
}

/** @brief Номер порта 1..65535 из строки; 0, если строка не порт */
static int parse_port(const std::string& s) {
    if (s.empty() || s.size() > 5 || s.find_first_not_of("0123456789") != std::string::npos) return 0;
    int port = std::stoi(s);
    return port <= 65535 ? port : 0;
}

int main(int argc, char* argv[]) {
    int port = 8080;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage("hostpulse");
            return 0;
        }
        if (arg == "--dump") {
            auto status = hostpulse::collector::read_all();
            nlohmann::json j;
            nlohmann::to_json(j, status);
            std::cout << j.dump(2) << std::endl;
            return 0;
        }
        port = parse_port(arg);
        if (!port) {
            std::cerr << "Ошибка: неверный аргумент «" << arg << "». Ожидается номер порта от 1 до 65535.\n"
                      << "Справка: hostpulse --help" << std::endl;
            return 2;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "hostpulse: запуск сервера на порту " << port << "\n"
              << "Откройте в браузере: http://localhost:" << port
              << " (с других компьютеров сети — по IP этой машины)\n"
              << "Остановить: Ctrl+C" << std::endl;

    if (!hostpulse::server::start_server(port)) {
        std::cerr << "Ошибка: не удалось запустить сервер на порту " << port
                  << " — порт занят другой программой или нет прав (порты ниже 1024 требуют root)." << std::endl;
        return 1;
    }

    std::cout << "hostpulse: сервер остановлен" << std::endl;
    return 0;
}
