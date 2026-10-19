#include <csignal>
#include <iostream>
#include <string>

#include "collector.h"
#include "server.h"

static void signal_handler(int sig) {
    (void)sig;
    hostpulse::server::stop_server();
}

static void print_usage() {
    std::cout << "hostpulse " HOSTPULSE_VERSION
                 "\nСостояние компьютера в браузере: нагрузка, температуры, память, диски, сеть.\n"
                 "\n"
                 "Использование: hostpulse [ПАРАМЕТРЫ] [ПОРТ]\n"
                 "\n"
                 "  --port ПОРТ    порт веб-дашборда (по умолчанию 8080)\n"
                 "  --host АДРЕС   откуда принимать подключения: 0.0.0.0 из сети (по умолчанию),\n"
                 "                 127.0.0.1 только с этой машины\n"
                 "  --dump         вывести все данные в JSON и выйти\n"
                 "  --version      показать версию\n"
                 "  --help         показать эту справку\n";
}

/** @brief Номер порта 1..65535 из строки; 0, если строка не порт */
static int parse_port(const std::string& s) {
    if (s.empty() || s.size() > 5 || s.find_first_not_of("0123456789") != std::string::npos) return 0;
    int port = std::stoi(s);
    return port <= 65535 ? port : 0;
}

/** @brief Сообщение о неверных аргументах в stderr; код выхода 2 */
static int usage_error(const std::string& message) {
    std::cerr << "Ошибка: " << message << "\nСправка: hostpulse --help" << std::endl;
    return 2;
}

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string host = "0.0.0.0";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--version") {
            std::cout << "hostpulse " HOSTPULSE_VERSION << std::endl;
            return 0;
        }
        if (arg == "--dump") {
            nlohmann::json j = hostpulse::collector::read_all();
            std::cout << j.dump(2) << std::endl;
            return 0;
        }
        if (arg == "--host" || arg == "--port") {
            if (i + 1 >= argc) return usage_error("после " + arg + " нужно значение");
            std::string value = argv[++i];
            if (arg == "--host") {
                host = value;
                continue;
            }
            arg = value;
        }
        port = parse_port(arg);
        if (!port) return usage_error("неверный порт «" + arg + "», ожидается число от 1 до 65535");
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "hostpulse " HOSTPULSE_VERSION ": запуск сервера на " << host << ":" << port << "\n"
              << "Откройте в браузере: http://localhost:" << port;
    if (host == "0.0.0.0") std::cout << " (с других компьютеров сети: по IP этой машины)";
    std::cout << std::endl;

    if (!hostpulse::server::start_server(port, {}, host)) {
        std::cerr << "Ошибка: не удалось запустить сервер на " << host << ":" << port
                  << ": порт занят другой программой, неверный адрес или нет прав (порты ниже 1024 требуют root)." << std::endl;
        return 1;
    }

    std::cout << "hostpulse: сервер остановлен" << std::endl;
    return 0;
}
