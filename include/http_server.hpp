#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

namespace SmartDehum {

class SimpleHttpServer {
public:
    using TriggerCallback = std::function<void()>;

    SimpleHttpServer(int port, TriggerCallback on_trigger)
        : port_(port), on_trigger_(on_trigger), running_(false), server_fd_(-1) {}

    ~SimpleHttpServer() {
        stop();
    }

    bool start() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) return false;

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
            close(server_fd_);
            return false;
        }

        if (listen(server_fd_, 5) < 0) {
            close(server_fd_);
            return false;
        }

        running_ = true;
        server_thread_ = std::thread(&SimpleHttpServer::worker, this);
        return true;
    }

    void stop() {
        if (running_) {
            running_ = false;
            if (server_fd_ >= 0) {
                shutdown(server_fd_, SHUT_RDWR);
                close(server_fd_);
                server_fd_ = -1;
            }
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
        }
    }

    void update_status_json(const std::string& json_str) {
        std::lock_guard<std::mutex> lock(json_mutex_);
        cached_json_ = json_str;
    }

private:
    int port_;
    TriggerCallback on_trigger_;
    std::atomic<bool> running_;
    int server_fd_;
    std::thread server_thread_;
    std::mutex json_mutex_;
    std::string cached_json_{"{}"};

    void worker() {
        while (running_) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                if (!running_) break;
                continue;
            }

            char buffer[1024] = {0};
            ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
            if (bytes_read > 0) {
                std::string req(buffer);
                if (req.find("GET /dehum_status.json") != std::string::npos) {
                    std::string payload;
                    {
                        std::lock_guard<std::mutex> lock(json_mutex_);
                        payload = cached_json_;
                    }
                    std::string resp = "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Access-Control-Allow-Origin: *\r\n"
                                       "Content-Length: " + std::to_string(payload.size()) + "\r\n"
                                       "\r\n" + payload;
                    write(client_fd, resp.c_str(), resp.size());
                } else if (req.find("POST /trigger") != std::string::npos) {
                    if (on_trigger_) on_trigger_();
                    std::string payload = "{\"status\":\"ok\",\"msg\":\"triggered\"}";
                    std::string resp = "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Access-Control-Allow-Origin: *\r\n"
                                       "Content-Length: " + std::to_string(payload.size()) + "\r\n"
                                       "\r\n" + payload;
                    write(client_fd, resp.c_str(), resp.size());
                } else {
                    std::string resp = "HTTP/1.1 404 Not Found\r\n\r\n";
                    write(client_fd, resp.c_str(), resp.size());
                }
            }
            close(client_fd);
        }
    }
};

} // namespace SmartDehum
