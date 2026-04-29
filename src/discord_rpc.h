#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <ctime>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#endif

class DiscordRPC {
public:
    static DiscordRPC& instance() {
        static DiscordRPC inst;
        return inst;
    }

    void init(const std::string& appId) {
        m_appId = appId;
        m_running = true;
        m_startTime = static_cast<int64_t>(std::time(nullptr));
        m_thread = std::thread(&DiscordRPC::loop, this);
    }

    void shutdown() {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
        disconnect();
    }

private:
    std::string  m_appId;
    std::atomic<bool> m_running{false};
    std::thread  m_thread;
    int64_t      m_startTime = 0;
    int          m_nonce = 1;

#ifdef _WIN32
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
#else
    int m_sock = -1;
#endif

    bool connected() {
#ifdef _WIN32
        return m_pipe != INVALID_HANDLE_VALUE;
#else
        return m_sock >= 0;
#endif
    }

    bool connect() {
#ifdef _WIN32
        for (int i = 0; i < 10; i++) {
            std::string path = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
            m_pipe = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (m_pipe != INVALID_HANDLE_VALUE) return true;
        }
        return false;
#else
        const char* tmpDirs[] = { getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), "/tmp" };
        for (auto dir : tmpDirs) {
            if (!dir) continue;
            for (int i = 0; i < 10; i++) {
                std::string path = std::string(dir) + "/discord-ipc-" + std::to_string(i);
                int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0) continue;
                sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
                if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0) {
                    m_sock = fd;
                    return true;
                }
                close(fd);
            }
        }
        return false;
#endif
    }

    void disconnect() {
#ifdef _WIN32
        if (m_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
#else
        if (m_sock >= 0) {
            close(m_sock);
            m_sock = -1;
        }
#endif
    }

    bool write(const void* data, size_t len) {
#ifdef _WIN32
        DWORD written = 0;
        return WriteFile(m_pipe, data, (DWORD)len, &written, nullptr) && written == (DWORD)len;
#else
        return send(m_sock, data, len, 0) == (ssize_t)len;
#endif
    }

    bool read(void* buf, size_t len) {
#ifdef _WIN32
        DWORD got = 0;
        return ReadFile(m_pipe, buf, (DWORD)len, &got, nullptr) && got == (DWORD)len;
#else
        size_t got = 0;
        while (got < len) {
            ssize_t r = recv(m_sock, (char*)buf + got, len - got, 0);
            if (r <= 0) return false;
            got += r;
        }
        return true;
#endif
    }

    bool sendFrame(uint32_t op, const std::string& json) {
        uint32_t header[2] = { op, (uint32_t)json.size() };
        if (!write(header, 8)) return false;
        return write(json.data(), json.size());
    }

    bool readFrame(uint32_t& op, std::string& json) {
        uint32_t header[2];
        if (!read(header, 8)) return false;
        op = header[0];
        json.resize(header[1]);
        return header[1] == 0 || read(&json[0], header[1]);
    }

    bool doHandshake() {
        std::string hs = "{\"v\":1,\"client_id\":\"" + m_appId + "\"}";
        if (!sendFrame(0, hs)) return false;
        uint32_t op; std::string resp;
        return readFrame(op, resp);
    }

    bool sendPresence() {
        std::string nonce = std::to_string(m_nonce++);
        std::string payload =
            "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
#ifdef _WIN32
            std::to_string((int)GetCurrentProcessId()) +
#else
            std::to_string((int)getpid()) +
#endif
        ",\"activity\":{"
        "\"timestamps\":{\"start\":" + std::to_string(m_startTime) + "},"
        "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Compact Launcher\"}"
        "}},\"nonce\":\"" + nonce + "\"}";

        if (!sendFrame(1, payload)) return false;
        uint32_t op; std::string resp;
        return readFrame(op, resp);
    }

    void loop() {
        while (m_running) {
            if (!connected()) {
                if (!connect() || !doHandshake()) {
                    for (int i = 0; i < 50 && m_running; i++)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
            }
            if (!sendPresence()) {
                disconnect();
                continue;
            }
            for (int i = 0; i < 150 && m_running; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    DiscordRPC() = default;
    ~DiscordRPC() { shutdown(); }
    DiscordRPC(const DiscordRPC&) = delete;
    DiscordRPC& operator=(const DiscordRPC&) = delete;
};
