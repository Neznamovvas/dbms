#include <iostream>
#include <string>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

class DBMSClient {
private:
    int sock_;
    std::string server_ip_;
    int port_;

public:
    DBMSClient(const std::string& server_ip = "127.0.0.1", int port = 8080)
        : sock_(-1), server_ip_(server_ip), port_(port) {}

    ~DBMSClient() {
        disconnect();
    }

    bool connect() {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) {
            std::cerr << "Failed to create socket" << std::endl;
            return false;
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(static_cast<uint16_t>(port_));

        if (inet_pton(AF_INET, server_ip_.c_str(), &server_addr.sin_addr) <= 0) {
            std::cerr << "Invalid server address" << std::endl;
            close(sock_);
            sock_ = -1;
            return false;
        }

        if (::connect(sock_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
            std::cerr << "Failed to connect to server" << std::endl;
            close(sock_);
            sock_ = -1;
            return false;
        }

        std::cout << "Connected to server at " << server_ip_ << ":" << port_ << std::endl;
        return true;
    }

    void disconnect() {
        if (sock_ >= 0) {
            close(sock_);
            sock_ = -1;
        }
    }

    std::string send_query(const std::string& query) {
        if (sock_ < 0) {
            return "Not connected to server";
        }

        std::string query_with_newline = query + "\n";
        send(sock_, query_with_newline.c_str(), query_with_newline.size(), 0);

        char buffer[65536];
        memset(buffer, 0, sizeof(buffer));

        ssize_t bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0) {
            return "Connection closed by server";
        }

        return std::string(buffer, static_cast<size_t>(bytes_received));
    }

    void run_interactive() {
        if (!connect()) {
            return;
        }

        std::cout << "========================================" << std::endl;
        std::cout << "  DBMS Client - Connected to Server" << std::endl;
        std::cout << "  Type 'exit;' to quit" << std::endl;
        std::cout << "========================================" << std::endl;

        std::string line;
        std::string buffer;

        while (true) {
            std::cout << "\ndbms> ";
            std::cout.flush();

            if (!std::getline(std::cin, line)) {
                break;
            }

            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t") + 1);

            if (line == "exit" || line == "exit;") {
                break;
            }

            buffer += line + " ";

            if (line.find(';') != std::string::npos) {
                std::string response = send_query(buffer);
                std::cout << response << std::endl;
                buffer.clear();
            }
        }

        std::cout << "Goodbye!" << std::endl;
        disconnect();
    }

    void run_script(const std::string& filename) {
        if (!connect()) {
            return;
        }

        std::ifstream file(filename);
        if (!file) {
            std::cerr << "Cannot open file: " << filename << std::endl;
            return;
        }

        std::stringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();

        std::string current;

        auto trim_query = [](std::string q) {
            const auto not_space = [](unsigned char c) { return !std::isspace(c); };
            q.erase(q.begin(), std::find_if(q.begin(), q.end(), not_space));
            q.erase(std::find_if(q.rbegin(), q.rend(), not_space).base(), q.end());
            return q;
        };

        for (char c : content) {
            current += c;
            if (c == ';') {
                std::string response = send_query(trim_query(current));
                std::cout << response << std::endl;
                current.clear();
            }
        }

        disconnect();
    }
};

int main(int argc, char* argv[]) {
    std::string server_ip = "127.0.0.1";
    int port = 8080;

    if (argc > 1 && std::string(argv[1]) == "--help") {
        std::cout << "Usage:" << std::endl;
        std::cout << "  dbms_client                     - Interactive mode (localhost:8080)" << std::endl;
        std::cout << "  dbms_client script.txt          - Batch mode" << std::endl;
        std::cout << "  dbms_client -h <host> -p <port> - Connect to specific server" << std::endl;
        return 0;
    }

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-h" && i + 1 < argc) {
            server_ip = argv[++i];
        } else if (std::string(argv[i]) == "-p" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        }
    }

    DBMSClient client(server_ip, port);

    if (argc > 1 && argv[1][0] != '-') {
        client.run_script(argv[1]);
    } else {
        client.run_interactive();
    }

    return 0;
}
