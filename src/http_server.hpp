#pragma once

#include <filesystem>
#include <iostream>
#include <stop_token>
#include <thread>

#include "httplib.h"

namespace fs = std::filesystem;

// RAII HTTP server that serves a static output directory.
//
// Runs on a std::jthread so it never blocks the main thread.
// When the jthread is destroyed (or stop() is called), httplib::Server::stop()
// is signalled and the thread joins automatically.

class HttpServer {
public:
    HttpServer(fs::path root, int port)
        : root_(std::move(root)), port_(port) {}

    // Start serving in the background.  Prints the local URL on success.
    void start() {
        // Mount the output directory at the site root.
        if (!svr_.set_mount_point("/", root_.string())) {
            std::cerr << "[serve] error: cannot mount directory: " << root_ << "\n";
            return;
        }

        // Redirect bare "/" to the index page if one exists.
        svr_.Get("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_redirect("/content/index.html");
        });

        server_thread_ = std::jthread([this](std::stop_token st) {
            // Register a stop callback so httplib shuts down when the
            // jthread's stop is requested (e.g. on normal program exit).
            std::stop_callback on_stop(st, [this] { svr_.stop(); });

            std::cout << "[serve] http://localhost:" << port_ << "\n";
            std::cout << "[serve] press Ctrl-C to quit\n";
            svr_.listen("localhost", port_);
        });
    }

    // Request shutdown explicitly (jthread destructor will also do this).
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
