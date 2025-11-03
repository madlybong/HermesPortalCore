///*
//HermesPortal v1.4.1 (core, 7202 + 7208 + SHM via XMemoryRing v2 + Socket single-client IPC)
//- 7 files total (SocketRelay added)
//- Output modes: console (default) | shm | file | socket
//- Socket mode: single local client, AUTH <token> handshake, drop-oldest queue when client connected,
//  no buffering while client disconnected.
//*/
//
//#include "includes/hermes_core.h"
//#include "XMemoryRing.hpp"
//#include "FileWriter.h"
//#include "SocketRelay.h"
//
//#include <thread>
//#include <chrono>
//#include <atomic>
//#include <string_view>
//
//#include <iostream>
//#include <iomanip>
//#include <sstream>
//#include <set>
//#include <vector>
//#include <csignal>
//
//#ifdef _WIN32
//#define WIN32_LEAN_AND_MEAN
//#include <winsock2.h>
//#include <ws2tcpip.h>
//#pragma comment(lib, "Ws2_32.lib")
//#endif
//
//// ======================= VERSION =======================
//static const char* kVersion = "HermesPortal v1.4.1";
//
//// ---------------- small helpers ----------------
//static std::set<int> parseEnabled(const std::string& s) {
//    std::set<int> out;
//    size_t pos = 0, prev = 0;
//    while ((pos = s.find(',', prev)) != std::string::npos) {
//        try { out.insert(std::stoi(s.substr(prev, pos - prev))); }
//        catch (...) {}
//        prev = pos + 1;
//    }
//    if (prev < s.size()) { try { out.insert(std::stoi(s.substr(prev))); } catch (...) {} }
//    return out;
//}
//
//// Lightweight CSV splitter used for extracting token & type from emitted CSV lines.
//static inline void split_csv_fields_simple(const std::string& line, std::vector<std::string>& out) {
//    out.clear();
//    size_t i = 0;
//    while (true) {
//        size_t j = line.find(',', i);
//        if (j == std::string::npos) { out.emplace_back(line.substr(i)); break; }
//        out.emplace_back(line.substr(i, j - i));
//        i = j + 1;
//    }
//}
//
//// ---------------- graceful shutdown ----------------
//static std::atomic<bool> g_running{ true };
//
//#ifdef _WIN32
//BOOL WINAPI console_ctrl_handler(DWORD ctrlType) {
//    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT ||
//        ctrlType == CTRL_CLOSE_EVENT) {
//        g_running.store(false);
//        return TRUE;
//    }
//    return FALSE;
//}
//#else
//static void signal_handler(int) {
//    g_running.store(false);
//}
//#endif
//
//// ---------------- main ----------------
//int main(int argc, char* argv[]) {
//    std::cout.setf(std::ios::unitbuf);
//
//#ifdef _WIN32
//    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
//    WSADATA wsaData;
//    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
//        std::cerr << "[FATAL] WSAStartup failed\n";
//        return 1;
//    }
//#else
//    std::signal(SIGINT, signal_handler);
//    std::signal(SIGTERM, signal_handler);
//#endif
//
//    //executable safety (unchanged)
//    const std::string fixed = "2025-10-20";
//    auto now = std::chrono::system_clock::now();
//    std::time_t t = std::chrono::system_clock::to_time_t(now);
//    std::tm local{};
//#ifdef _WIN32
//    localtime_s(&local, &t);
//#else
//    localtime_r(&t, &local);
//#endif
//    std::ostringstream oss;
//    oss << std::put_time(&local, "%Y-%m-%d");
//    std::string today = oss.str();
////    if (today != fixed) {
////        std::cerr << "Illegal usage detected.\n";
////#ifdef _WIN32
////        WSACleanup();
////#endif
////        return 1;
////    }
//
//    if (argc < 2) {
//        std::cerr
//            << "Usage: " << (argc ? argv[0] : "HermesPortal") << " <tokens_csv>\n"
//            << "  [--enable 7202,7208] [--market all]\n"
//            << "  [--out console|shm|file|socket] [--ring-name <name>] [--token <auth>] [--ring-cap <bytes>]\n"
//            << "  [--socket-port <port>] [--socket-token <token>] [--socket-maxq <n>] [--socket-batch-bytes <bytes>]\n"
//            << "  [--file-base <path>] [--debug] [--debug-schema]\n";
//#ifdef _WIN32
//        WSACleanup();
//#endif
//        return 1;
//    }
//
//    // CLI parse
//    std::string tokensCsv = argv[1];
//    std::set<int> enabledCodes;
//    bool marketAll = false;
//    bool debugMirror = false;
//    bool debugSchema = false;
//
//    std::string outMode = "console";
//    std::string ringName, ringToken; // ring naming for shm (existing flags)
//    uint64_t ringCap = (4ull << 20);
//    std::string fileBase;
//
//    // socket options
//    uint16_t socket_port = 0;
//    std::string socket_auth_token;
//    size_t socket_maxq = 4096;
//    size_t socket_batch_bytes = 16 * 1024;
//
//    for (int i = 2; i < argc; ++i) {
//        std::string a = argv[i];
//        if (a == "--enable" && i + 1 < argc) {
//            enabledCodes = parseEnabled(argv[++i]);
//        }
//        else if (a == "--market" && i + 1 < argc) {
//            if (std::string(argv[++i]) == "all") marketAll = true;
//        }
//        else if (a == "--out" && i + 1 < argc) {
//            outMode = argv[++i];
//        }
//        else if (a == "--ring-name" && i + 1 < argc) {
//            ringName = argv[++i];
//        }
//        else if (a == "--token" && i + 1 < argc) {
//            ringToken = argv[++i];
//        }
//        else if (a == "--ring-cap" && i + 1 < argc) {
//            try { ringCap = std::stoull(argv[++i]); }
//            catch (...) {}
//        }
//        else if (a == "--file-base" && i + 1 < argc) {
//            fileBase = argv[++i];
//        }
//        else if (a == "--socket-port" && i + 1 < argc) {
//            try { socket_port = static_cast<uint16_t>(std::stoi(argv[++i])); }
//            catch (...) {}
//        }
//        else if (a == "--socket-token" && i + 1 < argc) {
//            socket_auth_token = argv[++i];
//        }
//        else if (a == "--socket-maxq" && i + 1 < argc) {
//            try { socket_maxq = static_cast<size_t>(std::stoul(argv[++i])); }
//            catch (...) {}
//        }
//        else if (a == "--socket-batch-bytes" && i + 1 < argc) {
//            try { socket_batch_bytes = static_cast<size_t>(std::stoul(argv[++i])); }
//            catch (...) {}
//        }
//        else if (a == "--debug") {
//            debugMirror = true;
//        }
//        else if (a == "--debug-schema") {
//            debugSchema = true;
//        }
//    }
//
//    // LZO init
//    if (!Lzo::Init()) {
//        std::cerr << "[FATAL] LZO init failed\n";
//#ifdef _WIN32
//        WSACleanup();
//#endif
//        return 1;
//    }
//
//    // tokens
//    StrikeList strikes;
//    if (!strikes.loadFromArgs(tokensCsv)) {
//        std::cerr << "[FATAL] No tokens parsed from input.\n";
//#ifdef _WIN32
//        WSACleanup();
//#endif
//        return 1;
//    }
//
//    // Console sink
//    ConsoleSink sink;
//    ConsoleSink::setConsoleMirror(debugMirror);
//
//#ifdef _WIN32
//    xmr::Writer shmWriter;
//#endif
//    static FileWriter g_file_writer;
//    bool file_writer_enabled = false;
//
//    // SocketRelay pointer (only used if outMode == "socket")
//    std::unique_ptr<SocketRelay> socketRelay;
//
//    // Setup outputs
//    if (outMode == "shm") {
//#ifdef _WIN32
//        try {
//            xmr::Config cfg;
//            cfg.nameW = std::wstring(ringName.begin(), ringName.end());
//            cfg.tokenW = std::wstring(ringToken.begin(), ringToken.end());
//            cfg.capacity_bytes = ringCap ? ringCap : (4ull << 20);
//            cfg.drop_policy = xmr::DropPolicy::DropOldest;
//            cfg.frame_mode = xmr::FrameMode::Newline;
//            cfg.heartbeat_interval = std::chrono::nanoseconds(500'000'000);
//
//            if (debugMirror) {
//                cfg.on_event = [](xmr::EventType ev, const std::string& msg) {
//                    std::cerr << "[XMR event] " << (int)ev << " " << msg << "\n";
//                    };
//            }
//
//            shmWriter.open(cfg);
//            ConsoleSink::setExternal([&](const std::string& line) -> bool {
//                return shmWriter.write(line);
//                });
//
//            std::cout << "[INFO] Writing to SHM ring '" << ringName << "'"
//                << " (cap " << cfg.capacity_bytes << " bytes, drop=DropOldest)\n";
//        }
//        catch (const std::exception& e) {
//            std::cerr << "[FATAL] SHM open failed: " << e.what() << "\n";
//#ifdef _WIN32
//            WSACleanup();
//#endif
//            return 1;
//        }
//#else
//        std::cerr << "[WARN] --out shm requested but SHM is only implemented on Windows\n";
//#endif
//    }
//    else if (outMode == "file") {
//        try {
//            g_file_writer.start(fileBase);
//            file_writer_enabled = true;
//            ConsoleSink::setExternal([&](const std::string& line) -> bool {
//                std::vector<std::string> f;
//                split_csv_fields_simple(line, f);
//                uint32_t token = 0; uint16_t type = 0;
//                try { token = static_cast<uint32_t>(std::stoul(f.size() ? f[0] : "0")); }
//                catch (...) {}
//                try { type = static_cast<uint16_t>(std::stoi(f.size() > 1 ? f[1] : "0")); }
//                catch (...) {}
//                std::string market;
//                if (type == 7202 && f.size() > 2) market = f[2];
//                g_file_writer.enqueue(type, token, line, market);
//                if (debugMirror) std::cout << line << '\n';
//                return true;
//                });
//            std::cout << "[INFO] Writing to files under base=" << (fileBase.empty() ? "<exe-dir>/data" : fileBase) << "\n";
//        }
//        catch (const std::exception& e) {
//            std::cerr << "[FATAL] FileWriter start failed: " << e.what() << "\n";
//            return 1;
//        }
//    }
//    else if (outMode == "socket") {
//        // socket mode selected
//        if (socket_auth_token.empty()) {
//            std::cerr << "[FATAL] --socket-token is mandatory when using --out socket\n";
//#ifdef _WIN32
//            WSACleanup();
//#endif
//            return 1;
//        }
//
//        // create config and start relay
//        SocketRelay::Config cfg;
//        cfg.bind_addr = "127.0.0.1";
//        cfg.port = socket_port;
//        cfg.auth_token = socket_auth_token;
//        cfg.max_queue = socket_maxq;
//        cfg.batch_bytes = socket_batch_bytes;
//        cfg.replace_client = true;
//        cfg.verbose = debugMirror;
//
//        socketRelay.reset(new SocketRelay(cfg));
//        try {
//            socketRelay->start();
//        }
//        catch (const std::exception& e) {
//            std::cerr << "[FATAL] SocketRelay start failed: " << e.what() << "\n";
//#ifdef _WIN32
//            WSACleanup();
//#endif
//            return 1;
//        }
//        uint16_t p = socketRelay->listening_port();
//        // <-- changed from std::cerr to std::cout so parent/spawner doesn't treat this as an error line
//        std::cout << "[SOCKET] listening 127.0.0.1:" << p << " (token=" << (socket_auth_token.size() ? socket_auth_token.substr(0, 4) + "..." : "<none>") << ")\n";
//
//        // Set external sink to forward to file/shm as well as socket relay:
//        ConsoleSink::setExternal([&](const std::string& line) -> bool {
//            // forward to file writer if enabled
//            if (file_writer_enabled) {
//                std::vector<std::string> f;
//                split_csv_fields_simple(line, f);
//                uint32_t token = 0; uint16_t type = 0;
//                try { token = static_cast<uint32_t>(std::stoul(f.size() ? f[0] : "0")); }
//                catch (...) {}
//                try { type = static_cast<uint16_t>(std::stoi(f.size() > 1 ? f[1] : "0")); }
//                catch (...) {}
//                std::string market;
//                if (type == 7202 && f.size() > 2) market = f[2];
//                g_file_writer.enqueue(type, token, line, market);
//            }
//#ifdef _WIN32
//            // forward to shm if opened
//            if (shmWriter.is_open()) shmWriter.write(line);
//#endif
//            // notify socket relay (non-blocking; will drop if no client)
//            if (socketRelay) socketRelay->notify(line);
//            if (debugMirror) std::cout << line << "\n";
//            return true;
//            });
//
//        std::cout << "[INFO] Socket output enabled\n";
//    }
//    else {
//        std::cout << "[INFO] Output: console (core sink)\n";
//    }
//
//    // Dispatcher & handlers
//    PacketDispatcher dispatcher;
//    auto want = [&](int code) {
//        if (marketAll) return true;
//        return enabledCodes.empty() ? (code == 7202 || code == 7208)
//            : (enabledCodes.count(code) > 0);
//        };
//    if (want(7202)) dispatcher.registerHandler(std::make_unique<Handler7202>());
//    if (want(7208)) dispatcher.registerHandler(std::make_unique<Handler7208>());
//    if (debugSchema) PrintSchemas();
//
//    PacketParser parser(dispatcher);
//    InstrumentDirectory instDir;
//
//    // Multicast
//    const char* MULTICAST_IP = "233.1.2.5";
//    const int   MULTICAST_PORT = 34330;
//
//#ifdef _WIN32
//    SOCKET sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
//    if (sockfd == INVALID_SOCKET) { std::cerr << "[FATAL] socket\n"; WSACleanup(); return 1; }
//    BOOL reuse = TRUE;
//    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
//#else
//    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
//    if (sockfd < 0) { perror("[FATAL] socket"); return 1; }
//    int reuse = 1;
//    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
//#endif
//
//    sockaddr_in addr{};
//    addr.sin_family = AF_INET;
//    addr.sin_addr.s_addr = htonl(INADDR_ANY);
//    addr.sin_port = htons(MULTICAST_PORT);
//
//    if (bind(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
//        perror("[FATAL] bind");
//#ifdef _WIN32
//        closesocket(sockfd); WSACleanup();
//#else
//        close(sockfd);
//#endif
//        return 1;
//    }
//
//    ip_mreq mreq{};
//    in_addr multi{};
//#ifdef _WIN32
//    InetPtonA(AF_INET, MULTICAST_IP, &multi);
//#else
//    inet_pton(AF_INET, MULTICAST_IP, &multi);
//#endif
//    mreq.imr_multiaddr = multi;
//    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
//    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
//        reinterpret_cast<const char*>(&mreq), sizeof(mreq)) < 0) {
//        perror("[WARN] IP_ADD_MEMBERSHIP");
//    }
//
//    std::cout << "[INFO] Listening multicast " << MULTICAST_IP << ":" << MULTICAST_PORT << "\n";
//
//    static char recv_buf[65536];
//    while (g_running.load()) {
//#ifdef _WIN32
//        int recv_len = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);
//        if (recv_len > 0) {
//            parser.parse(recv_buf, recv_len, sink, &instDir, strikes);
//        }
//        else {
//            int err = WSAGetLastError();
//            if (err == WSAEINTR || err == WSAEWOULDBLOCK) continue;
//            std::cerr << "[WARN] recvfrom error " << err << "\n";
//        }
//#else
//        ssize_t recv_len = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);
//        if (recv_len > 0) {
//            parser.parse(recv_buf, static_cast<int>(recv_len), sink, &instDir, strikes);
//        }
//        else {
//            if (errno == EINTR) continue;
//            perror("[WARN] recvfrom");
//        }
//#endif
//    }
//
//    std::cout << "[INFO] Shutting down...\n";
//
//    // cleanup
//#ifdef _WIN32
//    closesocket(sockfd);
//    WSACleanup();
//#else
//    close(sockfd);
//#endif
//
//    if (socketRelay) {
//        socketRelay->stop();
//        socketRelay.reset();
//    }
//
//    if (file_writer_enabled) {
//        g_file_writer.stop();
//    }
//
//#ifdef _WIN32
//    if (shmWriter.is_open()) shmWriter.close();
//#endif
//
//    return 0;
//}



/*
HermesPortal v1.4.1 (core, 7202 + 7208 + SHM via XMemoryRing v2 + Socket single-client IPC)
- 7 files total (SocketRelay added)
- Output modes: console (default) | shm | file | socket
- Socket mode: single local client, AUTH <token> handshake, drop-oldest queue when client connected,
  no buffering while client disconnected.
*/

#include "includes/hermes_core.h"
#include "XMemoryRing.hpp"
#include "FileWriter.h"
#include "SocketRelay.h"

#include <thread>
#include <chrono>
#include <atomic>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <set>
#include <vector>
#include <string>
#include <algorithm>
#include <csignal>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

// ======================= VERSION =======================
static const char* kVersion = "HermesPortal v1.4.1";

// ---------------- small helpers ----------------
static std::set<int> parseEnabled(const std::string& s) {
    std::set<int> out;
    size_t pos = 0, prev = 0;
    while ((pos = s.find(',', prev)) != std::string::npos) {
        try { out.insert(std::stoi(s.substr(prev, pos - prev))); }
        catch (...) {}
        prev = pos + 1;
    }
    if (prev < s.size()) { try { out.insert(std::stoi(s.substr(prev))); } catch (...) {} }
    return out;
}

// Lightweight CSV splitter used for extracting token & type from emitted CSV lines.
static inline void split_csv_fields_simple(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    size_t i = 0;
    while (true) {
        size_t j = line.find(',', i);
        if (j == std::string::npos) { out.emplace_back(line.substr(i)); break; }
        out.emplace_back(line.substr(i, j - i));
        i = j + 1;
    }
}

// ---------------- graceful shutdown ----------------
static std::atomic<bool> g_running{ true };

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT ||
        ctrlType == CTRL_CLOSE_EVENT) {
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}
#else
static void signal_handler(int) {
    g_running.store(false);
}
#endif

// ---------------- CLI helpers and defaults ----------------
// Default multicast settings: default instrument = FO -> port 34330
static std::string MULTICAST_IP = "233.1.2.5";
static int         MULTICAST_PORT = 34330; // FO default. CM = 34074
// debug control (defined here; declared extern in the header)
bool g_cm_debug = false;


static std::string to_lowercopy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

static void print_usage_and_exit(const char* prog) {
    std::cerr
        << "Usage: " << (prog ? prog : "HermesPortal") << " <tokens_csv>\n"
        << "  [--enable 7202,7208] [--market all]\n"
        << "  [--out console|shm|file|socket] [--ring-name <name>] [--token <auth>] [--ring-cap <bytes>]\n"
        << "  [--socket-port <port>] [--socket-token <token>] [--socket-maxq <n>] [--socket-batch-bytes <bytes>]\n"
        << "  [--file-base <path>] [--debug] [--debug-schema]\n"
        << "\nAdditional multicast flags:\n"
        << "  --inst <cm|fo>          Choose instrument type (cm -> port 34074, fo -> port 34330). Default = fo\n"
        << "  --mcast-ip <ip>         Override multicast IP (default 233.1.2.5)\n"
        << "  --mcast-port <port>     Override multicast port (overrides --inst default)\n"
        << "  -h, --help              Show this help\n"
        ;
    std::exit(1);
}

// ---------------- main ----------------
int main(int argc, char* argv[]) {
    std::cout.setf(std::ios::unitbuf);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[FATAL] WSAStartup failed\n";
        return 1;
    }
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    if (argc < 2) {
#ifdef _WIN32
        WSACleanup();
#endif
        print_usage_and_exit(argc ? argv[0] : nullptr);
    }

    // CLI parse
    std::string tokensCsv = argv[1];
    std::set<int> enabledCodes;
    bool marketAll = false;
    bool debugMirror = false;
    bool debugSchema = false;

    std::string outMode = "console";
    std::string ringName, ringToken; // ring naming for shm (existing flags)
    uint64_t ringCap = (4ull << 20);
    std::string fileBase;

    // socket options
    uint16_t socket_port = 0;
    std::string socket_auth_token;
    size_t socket_maxq = 4096;
    size_t socket_batch_bytes = 16 * 1024;

    // selected feed (default FO)
    FeedType selectedFeed = FeedType::FO;

    // optional: dump first UDP packet to file and/or print hex (for debugging CM framing)
    std::string dump_pkt_path;
    bool dump_hex = false;


    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];

        // support --flag=value
        auto eqpos = a.find('=');
        std::string key = (eqpos == std::string::npos) ? a : a.substr(0, eqpos);
        std::string val = (eqpos == std::string::npos) ? std::string() : a.substr(eqpos + 1);

        if (key == "--enable") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            enabledCodes = parseEnabled(val);
        }
        else if (key == "--market") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            if (to_lowercopy(val) == "all") marketAll = true;
        }
        else if (key == "--out") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            outMode = val;
        }
        else if (key == "--ring-name") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            ringName = val;
        }
        else if (key == "--token") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            ringToken = val;
        }
        else if (key == "--ring-cap") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            try { ringCap = std::stoull(val); }
            catch (...) {}
        }
        else if (key == "--file-base") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            fileBase = val;
        }
        else if (key == "--socket-port") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            try { socket_port = static_cast<uint16_t>(std::stoi(val)); }
            catch (...) {}
        }
        else if (key == "--socket-token") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            socket_auth_token = val;
        }
        else if (key == "--socket-maxq") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            try { socket_maxq = static_cast<size_t>(std::stoul(val)); }
            catch (...) {}
        }
        else if (key == "--socket-batch-bytes") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            try { socket_batch_bytes = static_cast<size_t>(std::stoul(val)); }
            catch (...) {}
        }
        else if (key == "--debug") {
            debugMirror = true;
        }
        else if (key == "--debug-schema") {
            debugSchema = true;
        }
        else if (key == "--dump-pkt") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            dump_pkt_path = val;
        }
        else if (key == "--dump-hex") {
            dump_hex = true;
        }
        else if (key == "--inst") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            val = to_lowercopy(val);
            if (val == "cm") {
                MULTICAST_PORT = 34074;
                selectedFeed = FeedType::CM;
            }
            else if (val == "fo" || val.empty()) {
                MULTICAST_PORT = 34330;
                selectedFeed = FeedType::FO;
            }
            else {
                std::cerr << "[FATAL] Invalid value for --inst (use 'cm' or 'fo')\n";
#ifdef _WIN32
                WSACleanup();
#endif
                return 1;
            }
        }
        else if (key == "--mcast-ip") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            if (val.empty()) {
                std::cerr << "[FATAL] --mcast-ip requires an argument\n";
#ifdef _WIN32
                WSACleanup();
#endif
                return 1;
            }
            MULTICAST_IP = val;
        }
        else if (key == "--mcast-port") {
            if (val.empty() && i + 1 < argc) val = argv[++i];
            try {
                int p = std::stoi(val);
                if (p <= 0 || p > 65535) throw std::out_of_range("port");
                MULTICAST_PORT = p;
            }
            catch (...) {
                std::cerr << "[FATAL] Invalid value for --mcast-port: " << val << "\n";
#ifdef _WIN32
                WSACleanup();
#endif
                return 1;
            }
        }
        else {
            // ignore unknown args here (keeps compatibility)
        }
    }

    // Print chosen multicast settings early so logs show them
    std::cout << "[INFO] Multicast: " << MULTICAST_IP << ":" << MULTICAST_PORT
        << " (feed=" << (selectedFeed == FeedType::CM ? "CM" : "FO") << ")\n";

    // LZO init
    if (!Lzo::Init()) {
        std::cerr << "[FATAL] LZO init failed\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // tokens
    StrikeList strikes;
    if (!strikes.loadFromArgs(tokensCsv)) {
        std::cerr << "[FATAL] No tokens parsed from input.\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // Console sink
    ConsoleSink sink;
    ConsoleSink::setConsoleMirror(debugMirror);

#ifdef _WIN32
    xmr::Writer shmWriter;
#endif
    static FileWriter g_file_writer;
    bool file_writer_enabled = false;

    // SocketRelay pointer (only used if outMode == "socket")
    std::unique_ptr<SocketRelay> socketRelay;

    // Setup outputs
    if (outMode == "shm") {
#ifdef _WIN32
        try {
            xmr::Config cfg;
            cfg.nameW = std::wstring(ringName.begin(), ringName.end());
            cfg.tokenW = std::wstring(ringToken.begin(), ringToken.end());
            cfg.capacity_bytes = ringCap ? ringCap : (4ull << 20);
            cfg.drop_policy = xmr::DropPolicy::DropOldest;
            cfg.frame_mode = xmr::FrameMode::Newline;
            cfg.heartbeat_interval = std::chrono::nanoseconds(500'000'000);

            if (debugMirror) {
                cfg.on_event = [](xmr::EventType ev, const std::string& msg) {
                    std::cerr << "[XMR event] " << (int)ev << " " << msg << "\n";
                    };
            }

            shmWriter.open(cfg);
            ConsoleSink::setExternal([&](const std::string& line) -> bool {
                return shmWriter.write(line);
                });

            std::cout << "[INFO] Writing to SHM ring '" << ringName << "'"
                << " (cap " << cfg.capacity_bytes << " bytes, drop=DropOldest)\n";
        }
        catch (const std::exception& e) {
            std::cerr << "[FATAL] SHM open failed: " << e.what() << "\n";
#ifdef _WIN32
            WSACleanup();
#endif
            return 1;
        }
#else
        std::cerr << "[WARN] --out shm requested but SHM is only implemented on Windows\n";
#endif
    }
    else if (outMode == "file") {
        try {
            g_file_writer.start(fileBase);
            file_writer_enabled = true;
            ConsoleSink::setExternal([&](const std::string& line) -> bool {
                std::vector<std::string> f;
                split_csv_fields_simple(line, f);
                uint32_t token = 0; uint16_t type = 0;
                try { token = static_cast<uint32_t>(std::stoul(f.size() ? f[0] : "0")); }
                catch (...) {}
                try { type = static_cast<uint16_t>(std::stoi(f.size() > 1 ? f[1] : "0")); }
                catch (...) {}
                std::string market;
                if (type == 7202 && f.size() > 2) market = f[2];
                g_file_writer.enqueue(type, token, line, market);
                if (debugMirror) std::cout << line << '\n';
                return true;
                });
            std::cout << "[INFO] Writing to files under base=" << (fileBase.empty() ? "<exe-dir>/data" : fileBase) << "\n";
        }
        catch (const std::exception& e) {
            std::cerr << "[FATAL] FileWriter start failed: " << e.what() << "\n";
            return 1;
        }
    }
    else if (outMode == "socket") {
        // socket mode selected
        if (socket_auth_token.empty()) {
            std::cerr << "[FATAL] --socket-token is mandatory when using --out socket\n";
#ifdef _WIN32
            WSACleanup();
#endif
            return 1;
        }

        // create config and start relay
        SocketRelay::Config cfg;
        cfg.bind_addr = "127.0.0.1";
        cfg.port = socket_port;
        cfg.auth_token = socket_auth_token;
        cfg.max_queue = socket_maxq;
        cfg.batch_bytes = socket_batch_bytes;
        cfg.replace_client = true;
        cfg.verbose = debugMirror;

        socketRelay.reset(new SocketRelay(cfg));
        try {
            socketRelay->start();
        }
        catch (const std::exception& e) {
            std::cerr << "[FATAL] SocketRelay start failed: " << e.what() << "\n";
#ifdef _WIN32
            WSACleanup();
#endif
            return 1;
        }
        uint16_t p = socketRelay->listening_port();
        std::cout << "[SOCKET] listening 127.0.0.1:" << p << " (token=" << (socket_auth_token.size() ? socket_auth_token.substr(0, 4) + "..." : "<none>") << ")\n";

        // Set external sink to forward to file/shm as well as socket relay:
        ConsoleSink::setExternal([&](const std::string& line) -> bool {
            // forward to file writer if enabled
            if (file_writer_enabled) {
                std::vector<std::string> f;
                split_csv_fields_simple(line, f);
                uint32_t token = 0; uint16_t type = 0;
                try { token = static_cast<uint32_t>(std::stoul(f.size() ? f[0] : "0")); }
                catch (...) {}
                try { type = static_cast<uint16_t>(std::stoi(f.size() > 1 ? f[1] : "0")); }
                catch (...) {}
                std::string market;
                if (type == 7202 && f.size() > 2) market = f[2];
                g_file_writer.enqueue(type, token, line, market);
            }
#ifdef _WIN32
            // forward to shm if opened
            if (shmWriter.is_open()) shmWriter.write(line);
#endif
            // notify socket relay (non-blocking; will drop if no client)
            if (socketRelay) socketRelay->notify(line);
            if (debugMirror) std::cout << line << "\n";
            return true;
            });

        std::cout << "[INFO] Socket output enabled\n";
    }
    else {
        std::cout << "[INFO] Output: console (core sink)\n";
    }

    // Dispatcher & handlers
    PacketDispatcher dispatcher;
    auto want = [&](int code) {
        if (marketAll) return true;
        return enabledCodes.empty() ? (code == 7202 || code == 7208)
            : (enabledCodes.count(code) > 0);
        };
    if (want(7202)) dispatcher.registerHandler(std::make_unique<Handler7202>());
    if (want(7208)) dispatcher.registerHandler(std::make_unique<Handler7208>());

    // Register CM handlers (global implementations in HandlersMarket.cpp)
    if (selectedFeed == FeedType::CM) {
        dispatcher.registerHandler(std::make_unique<HandlerCM_CT>());
        dispatcher.registerHandler(std::make_unique<HandlerCM_PN>());
    }

    if (debugSchema) PrintSchemas();

    PacketParser parser(dispatcher);
    parser.setFeedType(selectedFeed);

    InstrumentDirectory instDir;

    // Multicast - now configurable via CLI flags above
#ifdef _WIN32
    SOCKET sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd == INVALID_SOCKET) { std::cerr << "[FATAL] socket\n"; WSACleanup(); return 1; }
    BOOL reuse = TRUE;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#else
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("[FATAL] socket"); return 1; }
    int reuse = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(MULTICAST_PORT);

    if (bind(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[FATAL] bind");
#ifdef _WIN32
        closesocket(sockfd); WSACleanup();
#else
        close(sockfd);
#endif
        return 1;
    }

    ip_mreq mreq{};
    in_addr multi{};
#ifdef _WIN32
    InetPtonA(AF_INET, MULTICAST_IP.c_str(), &multi);
#else
    inet_pton(AF_INET, MULTICAST_IP.c_str(), &multi);
#endif
    mreq.imr_multiaddr = multi;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
        reinterpret_cast<const char*>(&mreq), sizeof(mreq)) < 0) {
        perror("[WARN] IP_ADD_MEMBERSHIP");
    }

    std::cout << "[INFO] Listening multicast " << MULTICAST_IP << ":" << MULTICAST_PORT << "\n";

    static char recv_buf[65536];
    while (g_running.load()) {
#ifdef _WIN32
        int recv_len = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);
        if (recv_len > 0) {

            static bool dumped = false;
            if (!dumped && (!dump_pkt_path.empty() || dump_hex)) {
                // write binary file if requested
                if (!dump_pkt_path.empty()) {
                    std::ofstream ofs(dump_pkt_path, std::ios::binary);
                    if (ofs) {
#ifdef _WIN32
                        ofs.write(reinterpret_cast<const char*>(recv_buf), recv_len);
#else
                        ofs.write(recv_buf, recv_len);
#endif
                        ofs.close();
                        std::cout << "[DUMP] wrote " << recv_len << " bytes to " << dump_pkt_path << "\n";
                    }
                    else {
                        std::cerr << "[DUMP] failed to open " << dump_pkt_path << " for writing\n";
                    }
                }
                // print hex preview if requested
                if (dump_hex) {
                    std::ostringstream ss;
                    ss << "[DUMP HEX] first " << std::min<size_t>(256, static_cast<size_t>(recv_len)) << " bytes:\n";
                    size_t hex_print = std::min<size_t>(256, static_cast<size_t>(recv_len));
                    for (size_t ii = 0; ii < hex_print; ++ii) {
#ifdef _WIN32
                        unsigned char b = static_cast<unsigned char>(recv_buf[ii]);
#else
                        unsigned char b = static_cast<unsigned char>(recv_buf[ii]);
#endif
                        ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
                        if ((ii & 0x0F) == 0x0F) ss << '\n';
                        else ss << ' ';
                    }
                    ss << std::dec << '\n';
                    std::cout << ss.str();
                }
                dumped = true;
                std::cout << "[DUMP] finished; exiting to allow analysis. Remove --dump-pkt to resume normal operation.\n";
#ifdef _WIN32
                closesocket(sockfd);
                WSACleanup();
#else
                close(sockfd);
#endif
                return 0;
            }


            parser.parse(recv_buf, recv_len, sink, &instDir, strikes);
        }
        else {
            int err = WSAGetLastError();
            if (err == WSAEINTR || err == WSAEWOULDBLOCK) continue;
            std::cerr << "[WARN] recvfrom error " << err << "\n";
        }
#else
        ssize_t recv_len = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);
        if (recv_len > 0) {
            parser.parse(recv_buf, static_cast<int>(recv_len), sink, &instDir, strikes);
        }
        else {
            if (errno == EINTR) continue;
            perror("[WARN] recvfrom");
        }
#endif
    }

    std::cout << "[INFO] Shutting down...\n";

    // cleanup
#ifdef _WIN32
    closesocket(sockfd);
    WSACleanup();
#else
    close(sockfd);
#endif

    if (socketRelay) {
        socketRelay->stop();
        socketRelay.reset();
    }

    if (file_writer_enabled) {
        g_file_writer.stop();
    }

#ifdef _WIN32
    if (shmWriter.is_open()) shmWriter.close();
#endif

    return 0;
}
