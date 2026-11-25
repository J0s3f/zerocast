/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "stream_config.h"

#include <functional>
#include <string>

namespace stream {

// Optional LAN web page for entering stream keys from a phone or PC.
//
// Safety properties, all deliberate:
//  - It never runs unless the user starts it in the app (start()).
//  - It stops by itself after ttl_sec, 30 s after a successful save, after too many wrong
//    passwords, or when stop() is called (leaving the screen, Esc).
//  - Every activation gets a fresh random password that is shown only on the device screen.
//  - Saved stream keys are never sent back to the browser; a blank key field means "unchanged".
//  - State-changing requests need a per-activation CSRF token.
// The page is plain HTTP on the LAN, so the password and keys are not encrypted in transit:
// that is why the window is short and the password single-use.
class WebConfigServer {
public:
    using ApplyFn = std::function<void(const Config&)>;

    WebConfigServer();
    ~WebConfigServer();

    WebConfigServer(const WebConfigServer&) = delete;
    WebConfigServer& operator=(const WebConfigServer&) = delete;

    // Binds the port and starts the countdown. Returns false (see error()) on failure.
    bool start(const Config& current, ApplyFn apply, int port = 8080, int ttl_sec = 300);
    void stop();
    // Serves pending connections without blocking. Call about every 100 ms.
    void poll();

    bool running() const { return listen_fd_ >= 0; }
    const std::string& password() const { return password_; }
    const std::string& error() const { return error_; }
    // Why it last stopped: "expired", "saved", "locked", "stopped" ("" while running).
    const std::string& stop_reason() const { return stop_reason_; }
    std::string url() const;
    int seconds_left() const;
    int port() const { return port_; }

    // Handles one raw HTTP request and returns the raw HTTP response. Public so the logic can
    // be tested without sockets.
    std::string respond(const std::string& raw_request);

private:
    std::string render_page(const std::string& notice) const;
    bool apply_form(const std::string& body, std::string* notice);

    int listen_fd_ = -1;
    int port_ = 8080;
    std::string password_;
    std::string csrf_;
    std::string error_;
    std::string stop_reason_;
    Config config_;
    ApplyFn apply_;
    long expires_at_ = 0;
    int failed_attempts_ = 0;
    bool locked_ = false;
    bool saved_ = false;
};

}  // namespace stream
