#include "index/mcp_http_serial.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

namespace qt::index {
namespace {

namespace net = boost::asio;
using json = nlohmann::json;

std::size_t query_size(const HttpRequest& request, const std::string& key, std::size_t fallback) {
    const auto found = request.query.find(key);
    if (found == request.query.end() || found->second.empty()) {
        return fallback;
    }
    return static_cast<std::size_t>(std::strtoull(found->second.c_str(), nullptr, 10));
}

bool query_bool(const HttpRequest& request, const std::string& key, bool fallback) {
    const auto found = request.query.find(key);
    if (found == request.query.end() || found->second.empty()) {
        return fallback;
    }
    return found->second != "0" && found->second != "false";
}

std::string query_value(const HttpRequest& request, const std::string& key, const std::string& fallback = {}) {
    const auto found = request.query.find(key);
    return found == request.query.end() || found->second.empty() ? fallback : found->second;
}

struct SerialSession {
    std::string id;
    std::string port_name;
    unsigned int baud = 115200;
    net::io_context io;
    std::unique_ptr<net::serial_port> port;
    std::thread reader;
    std::mutex mutex;
    std::string log;
    std::size_t bytes_read = 0;
    std::atomic<bool> running{false};
};

class SerialManager {
public:
    ~SerialManager() {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, _] : sessions_) {
                ids.push_back(id);
            }
        }
        for (const auto& id : ids) {
            close(id);
        }
    }

    json list(const HttpRequest& request = {}) const {
        json ports = json::array();
        const std::vector<std::string> patterns{"/dev/ttyUSB", "/dev/ttyACM", "/dev/ttyS"};
        const auto filter = query_value(request, "pattern");
        const auto limit = query_size(request, "limit", 0);
        const auto add_port = [&](const std::filesystem::path& path) {
            const auto text = path.string();
            if (!filter.empty() && text.find(filter) == std::string::npos) {
                return;
            }
            if (limit != 0 && ports.size() >= limit) {
                return;
            }
            ports.push_back({{"path", text}});
        };
        std::error_code ec;
        for (const auto& prefix : patterns) {
            for (int index = 0; index < 64; ++index) {
                const auto path = std::filesystem::path{prefix + std::to_string(index)};
                if (std::filesystem::exists(path, ec)) {
                    add_port(path);
                }
            }
        }
        const auto serial_by_id = std::filesystem::path{"/dev/serial/by-id"};
        if (std::filesystem::is_directory(serial_by_id, ec)) {
            for (std::filesystem::directory_iterator it(serial_by_id, ec), end; !ec && it != end; it.increment(ec)) {
                add_port(it->path());
            }
        }
        return {{"ports", ports}, {"query", request.query}};
    }

    json open(const HttpRequest& request) {
        const auto port_name = query_value(request, "port");
        if (!allowed_port_path(port_name)) {
            throw std::invalid_argument("serial port must be under /dev/tty*, /dev/serial/, or /dev/pts/");
        }
        const auto baud = static_cast<unsigned int>(query_size(request, "baud", 115200));
        const auto id = query_value(request, "session", "serial-" + std::to_string(next_id_++));
        auto session = std::make_shared<SerialSession>();
        session->id = id;
        session->port_name = port_name;
        session->baud = baud;
        session->port = std::make_unique<net::serial_port>(session->io);
        session->port->open(port_name);
        session->port->set_option(net::serial_port_base::baud_rate(baud));
        session->port->set_option(net::serial_port_base::character_size(8));
        session->port->set_option(net::serial_port_base::parity(net::serial_port_base::parity::none));
        session->port->set_option(net::serial_port_base::stop_bits(net::serial_port_base::stop_bits::one));
        const auto native = session->port->native_handle();
        const int flags = ::fcntl(native, F_GETFL, 0);
        if (flags == -1 || ::fcntl(native, F_SETFL, flags | O_NONBLOCK) == -1) {
            throw std::runtime_error("serial nonblocking setup failed: " + std::string(std::strerror(errno)));
        }
        session->running.store(true);
        session->reader = std::thread([session]() { read_loop(session); });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto replaced = sessions_.find(id);
            if (replaced != sessions_.end()) {
                close_locked(replaced->second);
                sessions_.erase(replaced);
            }
            sessions_[id] = session;
        }
        return {{"session", id}, {"port", port_name}, {"baud", baud}, {"open", true}};
    }

    json close(const std::string& id) {
        std::shared_ptr<SerialSession> session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = sessions_.find(id);
            if (found == sessions_.end()) {
                return {{"session", id}, {"closed", false}, {"message", "session not found"}};
            }
            session = found->second;
            sessions_.erase(found);
        }
        close_locked(session);
        return {{"session", id}, {"closed", true}};
    }

    json write(const HttpRequest& request) {
        const auto session = require_session(query_value(request, "session"));
        auto data = query_value(request, "data");
        if (query_bool(request, "newline", false)) {
            data.push_back('\n');
        }
        if (data.empty()) {
            throw std::invalid_argument("serial write requires data");
        }
        boost::system::error_code ec;
        const auto written = net::write(*session->port, net::buffer(data), ec);
        if (ec) {
            throw std::runtime_error("serial write failed: " + ec.message());
        }
        return {{"session", session->id}, {"bytes_written", written}};
    }

    json read(const HttpRequest& request) {
        const auto session = require_session(query_value(request, "session"));
        const auto offset = query_size(request, "offset", 0);
        const auto max_bytes = query_size(request, "max_bytes", 8192);
        std::lock_guard<std::mutex> lock(session->mutex);
        const auto start = std::min(offset, session->bytes_read);
        const auto buffered_start = session->bytes_read >= session->log.size()
                                        ? session->bytes_read - session->log.size()
                                        : 0;
        const auto local_start = start > buffered_start ? start - buffered_start : 0;
        auto text = local_start < session->log.size() ? session->log.substr(local_start) : std::string{};
        bool truncated = false;
        if (max_bytes != 0 && text.size() > max_bytes) {
            text.resize(max_bytes);
            truncated = true;
        }
        return {{"session", session->id},
                {"port", session->port_name},
                {"offset", start},
                {"next_offset", session->bytes_read},
                {"data", text},
                {"truncated", truncated},
                {"open", session->running.load()}};
    }

    json clear(const HttpRequest& request) {
        const auto session = require_session(query_value(request, "session"));
        std::lock_guard<std::mutex> lock(session->mutex);
        session->log.clear();
        return {{"session", session->id}, {"offset", session->bytes_read}, {"cleared", true}};
    }

    json transact(const HttpRequest& request) {
        const auto session = require_session(query_value(request, "session"));
        std::size_t offset = 0;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            offset = session->bytes_read;
        }
        write(request);
        std::this_thread::sleep_for(std::chrono::milliseconds(query_size(request, "wait_ms", 250)));
        HttpRequest read_request = request;
        read_request.query["offset"] = std::to_string(offset);
        return read(read_request);
    }

private:
    static bool allowed_port_path(const std::string& path) {
        return path.rfind("/dev/tty", 0) == 0 || path.rfind("/dev/serial/", 0) == 0 ||
               path.rfind("/dev/pts/", 0) == 0;
    }

    static void read_loop(const std::shared_ptr<SerialSession>& session) {
        char buffer[256];
        const auto fd = session->port->native_handle();
        while (session->running.load()) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(fd, &read_fds);
            timeval timeout{};
            timeout.tv_usec = 100000;
            const int ready = ::select(fd + 1, &read_fds, nullptr, nullptr, &timeout);
            if (!session->running.load()) {
                break;
            }
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (ready == 0 || !FD_ISSET(fd, &read_fds)) {
                continue;
            }
            const auto count = ::read(fd, buffer, sizeof(buffer));
            if (count < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                break;
            }
            if (count == 0) {
                continue;
            }
            std::lock_guard<std::mutex> lock(session->mutex);
            session->log.append(buffer, buffer + count);
            session->bytes_read += static_cast<std::size_t>(count);
            constexpr std::size_t max_log = 256U * 1024U;
            if (session->log.size() > max_log) {
                session->log.erase(0, session->log.size() - max_log);
            }
        }
        session->running.store(false);
    }

    std::shared_ptr<SerialSession> require_session(const std::string& id) const {
        if (id.empty()) {
            throw std::invalid_argument("serial session is required");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            throw std::invalid_argument("serial session not found: " + id);
        }
        return found->second;
    }

    static void close_locked(const std::shared_ptr<SerialSession>& session) {
        session->running.store(false);
        if (session->port != nullptr && session->port->is_open()) {
            boost::system::error_code ec;
            session->port->cancel(ec);
            session->port->close(ec);
        }
        if (session->reader.joinable()) {
            session->reader.join();
        }
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SerialSession>> sessions_;
    std::atomic<std::size_t> next_id_{1};
};

SerialManager& serial_manager() {
    static SerialManager manager;
    return manager;
}

}  // namespace

json serial_list_payload(const HttpRequest& request) {
    return serial_manager().list(request);
}

json serial_open_payload(const HttpRequest& request) {
    return serial_manager().open(request);
}

json serial_close_payload(const HttpRequest& request) {
    return serial_manager().close(query_value(request, "session"));
}

json serial_write_payload(const HttpRequest& request) {
    return serial_manager().write(request);
}

json serial_read_payload(const HttpRequest& request) {
    return serial_manager().read(request);
}

json serial_clear_payload(const HttpRequest& request) {
    return serial_manager().clear(request);
}

json serial_transact_payload(const HttpRequest& request) {
    return serial_manager().transact(request);
}

}  // namespace qt::index
