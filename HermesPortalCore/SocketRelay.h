#pragma once
// SocketRelay — single-client socket IPC for HermesPortal v1.4.1
// See implementation in src/SocketRelay.cpp

#include <string>
#include <atomic>
#include <cstdint>

class SocketRelay {
public:
    struct Config {
        std::string bind_addr = "127.0.0.1";
        uint16_t port = 0;                      // 0 => ephemeral port auto-selected
        std::string auth_token;                 // mandatory when using socket output
        size_t max_queue = 4096;                // messages (drop-oldest)
        size_t batch_bytes = 16 * 1024;         // batch bytes per send
        bool replace_client = true;             // replace existing client on new connect
        bool verbose = false;                   // print small logs to stderr
    };

    explicit SocketRelay(const Config& cfg);
    ~SocketRelay();

    // start background accept/worker thread (throws on bind error)
    void start();

    // stop threads and close sockets (blocks until exit)
    void stop();

    // notify relay of a new CSV line (called from parser path).
    // Behavior:
    //  - if client is connected, enqueues line (bounded queue, drop-oldest)
    //  - if no client connected, does nothing (no buffering)
    // This call is fast/lock-protected.
    void notify(const std::string& csvLine);

    // get the listening port (0 if not started or error)
    uint16_t listening_port() const;

private:
    void* impl_; // opaque pointer to implementation
};
