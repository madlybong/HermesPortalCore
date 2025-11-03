// src/SocketRelay.cpp
// Implementation for SocketRelay (HermesPortal v1.4.1)
// Uses opaque impl_ pointer in header to avoid nested-private-access issues.

#include "SocketRelay.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <string>
#include <iostream>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using sock_t = SOCKET;
static const sock_t INVALID_SOCK = INVALID_SOCKET;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
using sock_t = int;
static const sock_t INVALID_SOCK = -1;
#endif

namespace {

    // Local implementation type (hidden)
    struct Impl {
        SocketRelay::Config cfg;

        // listen socket and chosen port
        sock_t listen_sock = INVALID_SOCK;
        uint16_t listen_port = 0;

        std::thread accept_thread;
        std::thread worker_thread;
        std::atomic<bool> running{ false };

        // client state (single client)
        std::mutex client_mtx;
        sock_t client_sock = INVALID_SOCK;
        std::string client_peer;
        std::atomic<bool> client_connected{ false };

        // queue while connected
        std::mutex queue_mtx;
        std::condition_variable queue_cv;
        std::deque<std::string> q;

        Impl(const SocketRelay::Config& c) : cfg(c) {}
        ~Impl() {}
    };

    // Helper to close socket portably
    inline void close_sock(sock_t s) {
#ifdef _WIN32
        if (s != INVALID_SOCK) closesocket(s);
#else
        if (s != INVALID_SOCK) close(s);
#endif
    }

    // Set SO_REUSEADDR (best-effort)
    inline void set_reuse_addr(sock_t s) {
#ifdef _WIN32
        BOOL opt = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
        int opt = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    }

    // Blocking recv-a-line with optional timeout (ms). Returns true if a line (without newline) was read.
    bool recv_line_with_timeout(sock_t s, std::string& out, int timeout_ms = 5000) {
        out.clear();
#ifdef _WIN32
        DWORD tv = (timeout_ms >= 0) ? static_cast<DWORD>(timeout_ms) : 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        if (timeout_ms >= 0) {
            timeval tv{ static_cast<long>(timeout_ms / 1000), static_cast<long>((timeout_ms % 1000) * 1000) };
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
#endif

        char ch;
        while (true) {
#ifdef _WIN32
            int n = recv(s, &ch, 1, 0);
            if (n <= 0) return false;
#else
            ssize_t n = ::recv(s, &ch, 1, 0);
            if (n <= 0) return false;
#endif
            if (ch == '\n') break;
            if (ch == '\r') continue;
            out.push_back(ch);
            if (out.size() > 64 * 1024) return false; // protective limit
        }
        return true;
    }

    // Blocking send-all (no message splitting); returns false on any send error
    bool send_all(sock_t s, const char* buf, size_t len, int timeout_ms = 5000) {
#ifdef _WIN32
        DWORD tv = (timeout_ms >= 0) ? static_cast<DWORD>(timeout_ms) : 0;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
        size_t sent = 0;
        while (sent < len) {
#ifdef _WIN32
            int n = send(s, buf + sent, static_cast<int>(len - sent), 0);
            if (n == SOCKET_ERROR || n == 0) return false;
            sent += static_cast<size_t>(n);
#else
            ssize_t n = ::send(s, buf + sent, len - sent, 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
#endif
        }
        return true;
    }

    // Get peer address as string (best-effort)
    std::string peer_to_string(sock_t s) {
        sockaddr_storage ss{};
        socklen_t sl = sizeof(ss);
        if (getpeername(s, reinterpret_cast<sockaddr*>(&ss), &sl) == 0) {
            char host[NI_MAXHOST];
            char serv[NI_MAXSERV];
            if (getnameinfo(reinterpret_cast<sockaddr*>(&ss), sl, host, sizeof(host), serv, sizeof(serv),
                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
                return std::string(host) + ":" + std::string(serv);
            }
        }
        return std::string("<unknown>");
    }

    // Helper: create listen socket and bind/listen. Returns chosen port in out_port.
    static sock_t create_listen_socket(const std::string& bind_addr, uint16_t port, uint16_t& out_port) {
        sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCK) return INVALID_SOCK;
        set_reuse_addr(s);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) <= 0) {
            // fallback to INADDR_ANY on failure
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        }

        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_sock(s);
            return INVALID_SOCK;
        }

        if (listen(s, 1) < 0) {
            close_sock(s);
            return INVALID_SOCK;
        }

        // determine actual port (for ephemeral)
        sockaddr_in sa{};
        socklen_t len = static_cast<socklen_t>(sizeof(sa));
        if (getsockname(s, reinterpret_cast<sockaddr*>(&sa), &len) == 0) {
            out_port = ntohs(sa.sin_port);
        }
        else {
            out_port = port;
        }

        return s;
    }

    // Accept loop: accepts connections, performs AUTH handshake, installs client (replace policy).
    static void accept_loop(Impl* I) {
        while (I->running.load()) {
            if (I->listen_sock == INVALID_SOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            sockaddr_in peer{};
            socklen_t plen = static_cast<socklen_t>(sizeof(peer));
            sock_t s = accept(I->listen_sock, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (s == INVALID_SOCK) {
                // transient error
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            std::string peerstr = peer_to_string(s);
            if (I->cfg.verbose) std::cerr << "[SOCKET] incoming connection from " << peerstr << "\n";

            // read AUTH line
            std::string line;
            bool got = recv_line_with_timeout(s, line, 3000);
            if (!got) {
                if (I->cfg.verbose) std::cerr << "[SOCKET] auth read failed from " << peerstr << "\n";
                close_sock(s);
                continue;
            }

            bool ok = false;
            if (line.size() >= 6 && line.rfind("AUTH ", 0) == 0) {
                std::string tok = line.substr(5);
                if (tok == I->cfg.auth_token) ok = true;
            }

            if (!ok) {
                const char* err = "ERR auth\n";
                send_all(s, err, strlen(err));
                close_sock(s);
                if (I->cfg.verbose) std::cerr << "[SOCKET] auth failed from " << peerstr << "\n";
                continue;
            }

            // send OK
            const char* okmsg = "OK\n";
            send_all(s, okmsg, strlen(okmsg));

            {
                std::lock_guard<std::mutex> lk(I->client_mtx);
                if (I->client_connected.load() && I->client_sock != INVALID_SOCK) {
                    if (I->cfg.replace_client) {
                        if (I->cfg.verbose) std::cerr << "[SOCKET] replacing existing client\n";
                        close_sock(I->client_sock);
                    }
                    else {
                        const char* busy = "ERR busy\n";
                        send_all(s, busy, strlen(busy));
                        close_sock(s);
                        continue;
                    }
                }
                I->client_sock = s;
                I->client_peer = peerstr;
                I->client_connected.store(true);
            }

            // wake worker if waiting
            I->queue_cv.notify_one();

            if (I->cfg.verbose) std::cerr << "[SOCKET] client accepted: " << peerstr << "\n";
        }
    }

    // Worker loop: wait for queued messages and send to client in batches
    static void worker_loop(Impl* I) {
        while (I->running.load()) {
            // Wait until there is a client and data
            std::unique_lock<std::mutex> qlk(I->queue_mtx);
            I->queue_cv.wait(qlk, [&] {
                return !I->running.load() || (I->client_connected.load() && !I->q.empty());
                });

            if (!I->running.load()) break;

            // copy a batch
            std::vector<std::string> batch;
            size_t bytes = 0;
            while (!I->q.empty() && bytes < I->cfg.batch_bytes) {
                batch.emplace_back(std::move(I->q.front()));
                bytes += batch.back().size() + 1; // newline
                I->q.pop_front();
            }
            qlk.unlock();

            if (batch.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // build send buffer
            std::string out;
            out.reserve(bytes);
            for (auto& m : batch) {
                out.append(m);
                if (m.empty() || m.back() != '\n') out.push_back('\n');
            }

            // send under client lock
            bool ok = true;
            {
                std::lock_guard<std::mutex> lk(I->client_mtx);
                if (!I->client_connected.load() || I->client_sock == INVALID_SOCK) {
                    ok = false;
                }
                else {
                    if (I->cfg.verbose) {
                        std::cerr << "[SOCKET] worker: sending bytes=" << out.size() << " to " << I->client_peer << "\n";
                    }
                    if (!send_all(I->client_sock, out.data(), out.size())) {
                        if (I->cfg.verbose) std::cerr << "[SOCKET] failed to send to client " << I->client_peer << "\n";
                        close_sock(I->client_sock);
                        I->client_sock = INVALID_SOCK;
                        I->client_connected.store(false);
                        ok = false;
                    }
                    else {
                        if (I->cfg.verbose) std::cerr << "[SOCKET] worker: send OK\n";
                    }
                }
            }


            // per requirement: if send failed, we DROP the batch (no preservation)
            (void)ok;
        }
    }

} // namespace anon

// Public SocketRelay methods

SocketRelay::SocketRelay(const Config& cfg) {
    impl_ = new Impl(cfg);
}

SocketRelay::~SocketRelay() {
    try { stop(); }
    catch (...) {}
    if (impl_) { delete reinterpret_cast<Impl*>(impl_); impl_ = nullptr; }
}

void SocketRelay::start() {
    if (!impl_) return;
    Impl* I = reinterpret_cast<Impl*>(impl_);
    if (I->running.load()) return;

    if (I->cfg.auth_token.empty()) {
        throw std::runtime_error("SocketRelay: auth_token required");
    }

    // create listen socket
    uint16_t chosen = 0;
    sock_t ls = create_listen_socket(I->cfg.bind_addr, I->cfg.port, chosen);
    if (ls == INVALID_SOCK) {
        throw std::runtime_error("SocketRelay: failed to bind/listen on " + I->cfg.bind_addr);
    }
    I->listen_sock = ls;
    I->listen_port = chosen;

    I->running.store(true);
    // accept thread
    I->accept_thread = std::thread([I]() { accept_loop(I); });
    // worker thread
    I->worker_thread = std::thread([I]() { worker_loop(I); });

    if (I->cfg.verbose) {
        std::cerr << "[SOCKET] listening " << I->cfg.bind_addr << ":" << I->listen_port << "\n";
    }
}

void SocketRelay::stop() {
    if (!impl_) return;
    Impl* I = reinterpret_cast<Impl*>(impl_);
    if (!I->running.load()) return;

    I->running.store(false);
    // Wake worker
    I->queue_cv.notify_all();

    // Close listen socket to break accept
    if (I->listen_sock != INVALID_SOCK) {
        close_sock(I->listen_sock);
        I->listen_sock = INVALID_SOCK;
    }

    // Close client
    {
        std::lock_guard<std::mutex> lk(I->client_mtx);
        if (I->client_sock != INVALID_SOCK) {
            close_sock(I->client_sock);
            I->client_sock = INVALID_SOCK;
            I->client_connected.store(false);
        }
    }

    if (I->accept_thread.joinable()) I->accept_thread.join();
    if (I->worker_thread.joinable()) I->worker_thread.join();
}

void SocketRelay::notify(const std::string& csvLine) {
    if (!impl_) return;
    Impl* I = reinterpret_cast<Impl*>(impl_);
    if (!I->client_connected.load()) {
        if (I->cfg.verbose) {
            std::cerr << "[SOCKET] notify: no client connected, dropping line\n";
        }
        return; // drop while disconnected
    }

    std::unique_lock<std::mutex> lk(I->queue_mtx);
    if (I->q.size() >= I->cfg.max_queue) {
        // drop oldest
        if (I->cfg.verbose) {
            std::cerr << "[SOCKET] notify: queue full (" << I->q.size() << "), dropping oldest\n";
        }
        I->q.pop_front();
    }
    I->q.emplace_back(csvLine);
    if (I->cfg.verbose) {
        std::cerr << "[SOCKET] notify: enqueued, qsize=" << I->q.size() << "\n";
    }
    lk.unlock();
    I->queue_cv.notify_one();
}


uint16_t SocketRelay::listening_port() const {
    if (!impl_) return 0;
    Impl* I = reinterpret_cast<Impl*>(impl_);
    return I->listen_port;
}
