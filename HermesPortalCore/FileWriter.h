#pragma once
// FileWriter: non-blocking file writer for HermesPortal
// Writes latest (overwrite) and historical (append) CSV lines per token.
// Default base directory is the executable directory + "/data" (unless start() is given another base).

#include <string>
#include <cstdint>
#include <cstddef>

class FileWriter {
public:
    FileWriter();
    ~FileWriter();

    // Start worker thread; baseDir empty => executable-dir + "/data"
    void start(const std::string& baseDir = "");

    // Stop worker thread and flush queue
    void stop();

    // Enqueue a CSV line. type should be 7208 or 7202. csv may include trailing newline.
    // market: optional string used to choose subfolder (if empty, FileWriter falls back to defaults:
    //         for 7208 -> "7208"; for 7202 -> "7202").
    // This function is thread-safe and returns immediately.
    void enqueue(uint16_t type, uint32_t token, const std::string& csvLine, const std::string& market = "");

    // Configure queue size (optional). Default 10000.
    void set_max_queue_size(size_t maxq);

private:
    // non-copyable
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;
};
