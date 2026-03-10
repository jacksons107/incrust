#pragma once

#include <filesystem>
#include <iostream>
#include <stop_token>
#include <thread>

#include "httplib.h"

namespace fs = std::filesystem;

// RAII HTTP server that serves a static output directory on a background jthread.

class HttpServer {
public:
    HttpServer(fs::path root, int port)
        : root_(std::move(root)), port_(port) {}

    void start() {
        if (!svr_.set_mount_point("/", root_.string())) {
            std::cerr << "[serve] error: cannot mount directory: " << root_ << "\n";
            return;
        }

        svr_.Get("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_redirect("/content/index.html");
        });

        server_thread_ = std::jthread([this](std::stop_token st) {
            std::stop_callback on_stop(st, [this] { svr_.stop(); });
            std::cout << "[serve] http://localhost:" << port_ << "\n";
            std::cout << "[serve] press Ctrl-C to quit\n";
            svr_.listen("localhost", port_);
        });
    }

    void stop() {
        svr_.stop();
        server_thread_.request_stop();
    }

    int  port() const { return port_; }
    bool is_running() const { return svr_.is_running(); }

private:
    fs::path         root_;
    int              port_;
    httplib::Server  svr_;
    std::jthread     server_thread_;
};
