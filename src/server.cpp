// Cross-platform TCP server: uses BSD sockets on macOS/Linux and Winsock on
// Windows behind a small compatibility shim, so the rest of the file (and
// all request-handling logic) stays platform-independent.
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using socket_t = SOCKET;
    static const socket_t INVALID_SOCK = INVALID_SOCKET;
    static const int SOCK_ERR = SOCKET_ERROR;
    static inline void close_socket(socket_t s) { closesocket(s); }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    using socket_t = int;
    static const socket_t INVALID_SOCK = -1;
    static const int SOCK_ERR = -1;
    static inline void close_socket(socket_t s) { close(s); }
#endif

#include "../include/logger.hpp"
#include "../include/executor.hpp"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <cstring>
#include <ctime>

using namespace dbms;

const int BUFFER_SIZE = 65536;

class LoggedDBServer {
private:
    int port_;
    socket_t server_socket_;
    std::atomic<bool> running_;
    AccessLogger logger_;
    Executor executor_;
    
    #ifdef _WIN32
    WSADATA wsa_data_;
    #endif
    
public:
    LoggedDBServer(int port = 8080, const std::string& log_file = "access.log")
        : port_(port), server_socket_(INVALID_SOCK), running_(true),
          logger_(log_file, true, LogLevel::L_INFO) {
        
        std::cout << "Logging to: " << log_file << std::endl;
        
        #ifdef _WIN32
        WSAStartup(MAKEWORD(2, 2), &wsa_data_);
        #endif
    }
    
    ~LoggedDBServer() {
        #ifdef _WIN32
        WSACleanup();
        #endif
    }
    
    void start() {
        create_socket();
        bind_socket();
        listen_for_connections();
        
        std::cout << "Logged DB Server started on port " << port_ << std::endl;
        std::cout << "Access log: access.log" << std::endl;
        std::cout << "Statistics: STATS command" << std::endl;
        std::cout << "Rotate log: ROTATE command" << std::endl;
        
        accept_clients();
    }
    
    void stop() {
        running_ = false;
        close_socket(server_socket_);
    }
    
private:
    void create_socket() {
        server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket_ == INVALID_SOCK) {
            throw std::runtime_error("Failed to create socket");
        }

        // Allow quick restarts of the server on the same port (common need
        // on macOS/Linux where TIME_WAIT can otherwise block a rebind).
        int opt = 1;
        #ifdef _WIN32
        setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
        #else
        setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        #endif
    }
    
    void bind_socket() {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);
        
        if (bind(server_socket_, (struct sockaddr*)&addr, sizeof(addr)) == SOCK_ERR) {
            throw std::runtime_error("Failed to bind socket");
        }
    }
    
    void listen_for_connections() {
        if (listen(server_socket_, 5) == SOCK_ERR) {
            throw std::runtime_error("Failed to listen");
        }
    }
    
    void accept_clients() {
        while (running_) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            
            socket_t client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
            if (client_socket == INVALID_SOCK) continue;
        
            std::string client_id = IDGenerator::generate_client_id();
            std::string handler_id = IDGenerator::generate_handler_id();
            
            std::cout << "New client connected: " << client_id << " (handler: " << handler_id << ")" << std::endl;
            
            std::thread(&LoggedDBServer::handle_client, this, client_socket, client_id, handler_id).detach();
        }
    }
    
    void send_response(socket_t client_socket, const std::string& response) {
        std::string resp = response + "\n";
        send(client_socket, resp.c_str(), (int)resp.size(), 0);
    }
    
    void handle_client(socket_t client_socket, const std::string& client_id, const std::string& handler_id) {
        char buffer[BUFFER_SIZE];
        
        while (running_) {
            memset(buffer, 0, sizeof(buffer));
            
            int bytes = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
            if (bytes <= 0) break;
            
            std::string query(buffer);
            if (!query.empty() && query.back() == '\n') {
                query.pop_back();
            }
            
            std::cout << "[" << client_id << "] Processing: " << query << std::endl;
            
            AccessLogger::QueryLogger query_logger(&logger_, query, client_id, handler_id);
            
            int status_code = 200;
            std::string response;
            std::string error_message;
            
            try {
                if (query == "STATS") {
                    response = logger_.get_stats();
                } else if (query == "ROTATE") {
                    logger_.rotate();
                    json j;
                    j["status"] = "success";
                    j["message"] = "Log rotated";
                    response = j.dump();
                } else {
                    auto result = executor_.execute(query);
                    response = result.to_json();
                }
            } catch (const std::exception& e) {
                status_code = 500;
                error_message = e.what();
                json j;
                j["error"] = e.what();
                response = j.dump();
            }
            
            if (status_code == 200) {
                query_logger.success();
            } else {
                query_logger.error(error_message, status_code);
            }
            
            send_response(client_socket, response);
        }
        
        close_socket(client_socket);
        
        std::cout << "Client disconnected: " << client_id << std::endl;
    }
};

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string log_file = "access.log";
    
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    if (argc > 2) {
        log_file = argv[2];
    }
    
    try {
        LoggedDBServer server(port, log_file);
        server.start();
        
        std::cout << "Press Enter to stop..." << std::endl;
        std::cin.get();
        
        server.stop();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
