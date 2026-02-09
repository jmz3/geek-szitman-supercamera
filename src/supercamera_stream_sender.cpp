#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "supercamera_core.hpp"

namespace {

constexpr uint32_t STREAM_MAGIC = 0x47535643; // GSVC
constexpr uint8_t STREAM_VERSION = 1;
constexpr uint8_t STREAM_CODEC_JPEG = 1;
constexpr size_t STREAM_HEADER_SIZE = 28;
constexpr uint32_t MAX_PAYLOAD_SIZE = 1024 * 1024;

std::atomic_bool g_stop = false;

struct SenderOptions {
    std::string transport;
    std::string bind_ip = "0.0.0.0";
    uint16_t port = 9000;
    uint16_t camera_count = 1;
    uint32_t max_fps = 0;
    uint32_t log_every = 120;
    bool transport_set = false;
};

uint64_t host_to_be64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(value);
#else
    return value;
#endif
}

uint64_t be64_to_host(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(value);
#else
    return value;
#endif
}

std::array<uint8_t, STREAM_HEADER_SIZE> serialize_header(const supercamera::CapturedFrame &frame) {
    std::array<uint8_t, STREAM_HEADER_SIZE> out{};

    const uint32_t magic_be = htonl(STREAM_MAGIC);
    const uint16_t flags_be = htons(0);
    const uint16_t source_id_be = htons(frame.source_id);
    const uint16_t reserved_be = htons(0);
    const uint32_t frame_id_be = htonl(frame.frame_id);
    const uint64_t timestamp_be = host_to_be64(frame.timestamp_us);
    const uint32_t payload_size_be = htonl(static_cast<uint32_t>(frame.jpeg.size()));

    std::memcpy(out.data() + 0, &magic_be, sizeof(magic_be));
    out[4] = STREAM_VERSION;
    out[5] = STREAM_CODEC_JPEG;
    std::memcpy(out.data() + 6, &flags_be, sizeof(flags_be));
    std::memcpy(out.data() + 8, &source_id_be, sizeof(source_id_be));
    std::memcpy(out.data() + 10, &reserved_be, sizeof(reserved_be));
    std::memcpy(out.data() + 12, &frame_id_be, sizeof(frame_id_be));
    std::memcpy(out.data() + 16, &timestamp_be, sizeof(timestamp_be));
    std::memcpy(out.data() + 24, &payload_size_be, sizeof(payload_size_be));

    return out;
}

struct DecodedHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t codec;
    uint16_t flags;
    uint16_t source_id;
    uint16_t reserved;
    uint32_t frame_id;
    uint64_t timestamp_us;
    uint32_t payload_size;
};

bool decode_and_validate_header(std::span<const uint8_t, STREAM_HEADER_SIZE> data, DecodedHeader *out) {
    uint32_t magic_be = 0;
    uint16_t flags_be = 0;
    uint16_t source_id_be = 0;
    uint16_t reserved_be = 0;
    uint32_t frame_id_be = 0;
    uint64_t timestamp_be = 0;
    uint32_t payload_size_be = 0;

    std::memcpy(&magic_be, data.data() + 0, sizeof(magic_be));
    std::memcpy(&flags_be, data.data() + 6, sizeof(flags_be));
    std::memcpy(&source_id_be, data.data() + 8, sizeof(source_id_be));
    std::memcpy(&reserved_be, data.data() + 10, sizeof(reserved_be));
    std::memcpy(&frame_id_be, data.data() + 12, sizeof(frame_id_be));
    std::memcpy(&timestamp_be, data.data() + 16, sizeof(timestamp_be));
    std::memcpy(&payload_size_be, data.data() + 24, sizeof(payload_size_be));

    const DecodedHeader parsed = {
        .magic = ntohl(magic_be),
        .version = data[4],
        .codec = data[5],
        .flags = ntohs(flags_be),
        .source_id = ntohs(source_id_be),
        .reserved = ntohs(reserved_be),
        .frame_id = ntohl(frame_id_be),
        .timestamp_us = be64_to_host(timestamp_be),
        .payload_size = ntohl(payload_size_be),
    };

    if (parsed.magic != STREAM_MAGIC) {
        return false;
    }
    if (parsed.version != STREAM_VERSION) {
        return false;
    }
    if (parsed.codec != STREAM_CODEC_JPEG) {
        return false;
    }
    if (parsed.payload_size > MAX_PAYLOAD_SIZE) {
        return false;
    }

    *out = parsed;
    return true;
}

class MultiCameraFrameBuffer {
public:
    explicit MultiCameraFrameBuffer(uint16_t camera_count)
        : slots_(camera_count) {}

    void push(const supercamera::CapturedFrame &frame) {
        std::lock_guard lock(mtx_);
        if (frame.source_id >= slots_.size()) {
            return;
        }

        Slot &slot = slots_[frame.source_id];
        if (slot.pending) {
            ++slot.dropped_count;
            ++dropped_total_;
        } else {
            slot.pending = true;
            pending_ids_.push_back(frame.source_id);
        }
        slot.latest = frame;
        cv_.notify_one();
    }

    bool wait_next(supercamera::CapturedFrame *out_frame) {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [&] { return stopped_ || !pending_ids_.empty(); });
        if (stopped_) {
            return false;
        }

        const uint16_t source_id = pending_ids_.front();
        pending_ids_.pop_front();

        Slot &slot = slots_[source_id];
        slot.pending = false;
        if (!slot.latest.has_value()) {
            return false;
        }

        *out_frame = *slot.latest;
        return true;
    }

    void stop() {
        std::lock_guard lock(mtx_);
        stopped_ = true;
        cv_.notify_all();
    }

    uint64_t dropped_count() const {
        std::lock_guard lock(mtx_);
        return dropped_total_;
    }

private:
    struct Slot {
        std::optional<supercamera::CapturedFrame> latest;
        uint64_t dropped_count = 0;
        bool pending = false;
    };

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<Slot> slots_;
    std::deque<uint16_t> pending_ids_;
    uint64_t dropped_total_ = 0;
    bool stopped_ = false;
};

void print_help(const char *argv0) {
    std::cout << "Usage: " << argv0 << " --transport <tcp|udp> [options]\n"
              << "\n"
              << "Options:\n"
              << "  --transport <tcp|udp>  Transport protocol. Only tcp is implemented.\n"
              << "  --bind <ip>            Bind address (default: 0.0.0.0).\n"
              << "  --port <n>             TCP port (default: 9000).\n"
              << "  --camera-count <n>     Number of USB cameras to stream (default: 1).\n"
              << "  --max-fps <n>          Max send frame rate, 0 for unlimited (default: 0).\n"
              << "  --log-every <n>        Print stats every N sent frames (default: 120).\n"
              << "  --help                 Show this help.\n";
}

bool parse_u16(const std::string &s, uint16_t *out) {
    try {
        const unsigned long v = std::stoul(s);
        if (v > 65535UL) {
            return false;
        }
        *out = static_cast<uint16_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_u32(const std::string &s, uint32_t *out) {
    try {
        const unsigned long v = std::stoul(s);
        if (v > 0xFFFFFFFFUL) {
            return false;
        }
        *out = static_cast<uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

int parse_args(int argc, char **argv, SenderOptions *opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            print_help(argv[0]);
            return 0;
        }

        auto need_value = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        try {
            if (arg == "--transport") {
                opts->transport = need_value("--transport");
                opts->transport_set = true;
            } else if (arg == "--bind") {
                opts->bind_ip = need_value("--bind");
            } else if (arg == "--port") {
                uint16_t port = 0;
                if (!parse_u16(need_value("--port"), &port)) {
                    throw std::runtime_error("invalid --port value");
                }
                opts->port = port;
            } else if (arg == "--camera-count") {
                uint16_t camera_count = 0;
                if (!parse_u16(need_value("--camera-count"), &camera_count) || camera_count == 0) {
                    throw std::runtime_error("invalid --camera-count value");
                }
                opts->camera_count = camera_count;
            } else if (arg == "--max-fps") {
                uint32_t max_fps = 0;
                if (!parse_u32(need_value("--max-fps"), &max_fps)) {
                    throw std::runtime_error("invalid --max-fps value");
                }
                opts->max_fps = max_fps;
            } else if (arg == "--log-every") {
                uint32_t log_every = 0;
                if (!parse_u32(need_value("--log-every"), &log_every)) {
                    throw std::runtime_error("invalid --log-every value");
                }
                opts->log_every = log_every;
            } else {
                throw std::runtime_error("unknown option: " + arg);
            }
        } catch (const std::exception &e) {
            std::cerr << e.what() << "\n";
            print_help(argv[0]);
            return -1;
        }
    }

    if (!opts->transport_set) {
        std::cerr << "--transport is required\n";
        print_help(argv[0]);
        return -1;
    }

    if (opts->transport == "udp") {
        std::cerr << "UDP transport is not implemented yet. Use --transport tcp.\n";
        return -1;
    }
    if (opts->transport != "tcp") {
        std::cerr << "unsupported transport: " << opts->transport << "\n";
        return -1;
    }

    return 1;
}

bool send_all(int fd, const uint8_t *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

void signal_handler(int) {
    g_stop = true;
}

int make_server_socket(const SenderOptions &opts) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket() failed\n";
        return -1;
    }

    const int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "setsockopt(SO_REUSEADDR) failed\n";
        close(fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opts.port);
    if (inet_pton(AF_INET, opts.bind_ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "invalid bind IP: " << opts.bind_ip << "\n";
        close(fd);
        return -1;
    }

    if (bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed on " << opts.bind_ip << ":" << opts.port << "\n";
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        std::cerr << "listen() failed\n";
        close(fd);
        return -1;
    }

    return fd;
}

bool run_self_tests() {
    {
        supercamera::CapturedFrame frame = {
            .jpeg = {1, 2, 3, 4},
            .source_id = 2,
            .frame_id = 99,
            .timestamp_us = 123456789ULL,
        };
        const auto header = serialize_header(frame);
        DecodedHeader decoded{};
        if (!decode_and_validate_header(std::span<const uint8_t, STREAM_HEADER_SIZE>(header), &decoded)) {
            std::cerr << "self-test failed: header round-trip decode\n";
            return false;
        }
        if (decoded.source_id != frame.source_id || decoded.frame_id != frame.frame_id
            || decoded.timestamp_us != frame.timestamp_us || decoded.payload_size != frame.jpeg.size()) {
            std::cerr << "self-test failed: header round-trip mismatch\n";
            return false;
        }
    }

    {
        supercamera::CapturedFrame frame = {
            .jpeg = {1, 2, 3},
            .source_id = 0,
            .frame_id = 1,
            .timestamp_us = 2,
        };
        auto header = serialize_header(frame);

        header[0] = 0x00;
        DecodedHeader decoded{};
        if (decode_and_validate_header(std::span<const uint8_t, STREAM_HEADER_SIZE>(header), &decoded)) {
            std::cerr << "self-test failed: bad magic accepted\n";
            return false;
        }

        header = serialize_header(frame);
        header[4] = 42;
        if (decode_and_validate_header(std::span<const uint8_t, STREAM_HEADER_SIZE>(header), &decoded)) {
            std::cerr << "self-test failed: bad version accepted\n";
            return false;
        }

        header = serialize_header(frame);
        const uint32_t oversized = htonl(MAX_PAYLOAD_SIZE + 1);
        std::memcpy(header.data() + 24, &oversized, sizeof(oversized));
        if (decode_and_validate_header(std::span<const uint8_t, STREAM_HEADER_SIZE>(header), &decoded)) {
            std::cerr << "self-test failed: oversized payload accepted\n";
            return false;
        }
    }

    {
        MultiCameraFrameBuffer buffer(2);
        const supercamera::CapturedFrame a1 = {
            .jpeg = {1},
            .source_id = 0,
            .frame_id = 1,
            .timestamp_us = 100,
        };
        const supercamera::CapturedFrame a2 = {
            .jpeg = {2},
            .source_id = 0,
            .frame_id = 2,
            .timestamp_us = 101,
        };
        const supercamera::CapturedFrame b1 = {
            .jpeg = {3},
            .source_id = 1,
            .frame_id = 1,
            .timestamp_us = 102,
        };

        buffer.push(a1);
        buffer.push(a2);
        buffer.push(b1);

        supercamera::CapturedFrame out1{};
        supercamera::CapturedFrame out2{};
        if (!buffer.wait_next(&out1) || !buffer.wait_next(&out2)) {
            std::cerr << "self-test failed: wait_next returned false\n";
            return false;
        }
        if (!(out1.source_id == 0 && out1.frame_id == 2 && out2.source_id == 1 && out2.frame_id == 1)) {
            std::cerr << "self-test failed: multi-camera ordering behavior\n";
            return false;
        }
        if (buffer.dropped_count() != 1) {
            std::cerr << "self-test failed: drop accounting\n";
            return false;
        }

        buffer.stop();
    }

    return true;
}

} // namespace

int main(int argc, char **argv) {
    SenderOptions opts;
    const int parse_result = parse_args(argc, argv, &opts);
    if (parse_result <= 0) {
        return parse_result == 0 ? 0 : 1;
    }

    if (!run_self_tests()) {
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const size_t available_devices = supercamera::SupercameraCapture::available_devices();
    if (available_devices == 0) {
        std::cerr << "no supported USB camera found\n";
        return 1;
    }

    uint16_t active_camera_count = opts.camera_count;
    if (active_camera_count > available_devices) {
        std::cerr << "requested " << active_camera_count << " cameras, but only "
                  << available_devices << " available; using " << available_devices << "\n";
        active_camera_count = static_cast<uint16_t>(available_devices);
    }

    MultiCameraFrameBuffer frame_buffer(active_camera_count);
    std::atomic_uint64_t captured_frames = 0;
    std::atomic_uint64_t sent_frames = 0;

    std::vector<std::unique_ptr<supercamera::SupercameraCapture>> captures;
    captures.reserve(active_camera_count);
    try {
        for (uint16_t source_id = 0; source_id < active_camera_count; ++source_id) {
            captures.emplace_back(std::make_unique<supercamera::SupercameraCapture>(source_id));
        }
    } catch (const std::exception &e) {
        std::cerr << "capture setup error: " << e.what() << "\n";
        return 1;
    }

    std::atomic_uint32_t active_capture_threads = active_camera_count;
    std::vector<std::thread> capture_threads;
    capture_threads.reserve(active_camera_count);

    for (uint16_t source_id = 0; source_id < active_camera_count; ++source_id) {
        capture_threads.emplace_back([&, source_id] {
            try {
                captures[source_id]->run([&](const supercamera::CapturedFrame &frame) {
                    ++captured_frames;
                    frame_buffer.push(frame);
                });
            } catch (const std::exception &e) {
                std::cerr << "capture error (camera " << source_id << "): " << e.what() << "\n";
            }

            if (active_capture_threads.fetch_sub(1) == 1) {
                frame_buffer.stop();
                g_stop = true;
            }
        });
    }

    const int server_fd = make_server_socket(opts);
    if (server_fd < 0) {
        for (auto &capture : captures) {
            capture->request_stop();
        }
        frame_buffer.stop();
        for (auto &thread : capture_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        return 1;
    }

    std::cout << "stream sender listening on " << opts.bind_ip << ":" << opts.port
              << " transport=tcp cameras=" << active_camera_count << "\n";

    while (!g_stop) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = accept(server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!g_stop) {
                std::cerr << "accept() failed\n";
            }
            break;
        }

        char client_ip[INET_ADDRSTRLEN] = "unknown";
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "client connected: " << client_ip << ":" << ntohs(client_addr.sin_port) << "\n";

        auto next_send_time = std::chrono::steady_clock::now();
        const auto frame_interval =
            (opts.max_fps > 0)
                ? std::chrono::microseconds(1000000 / opts.max_fps)
                : std::chrono::microseconds(0);

        while (!g_stop) {
            supercamera::CapturedFrame frame{};
            if (!frame_buffer.wait_next(&frame)) {
                break;
            }

            if (frame.jpeg.size() > MAX_PAYLOAD_SIZE) {
                std::cerr << "dropping oversized frame source=" << frame.source_id
                          << " frame_id=" << frame.frame_id
                          << " size=" << frame.jpeg.size() << "\n";
                continue;
            }

            if (opts.max_fps > 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now < next_send_time) {
                    std::this_thread::sleep_until(next_send_time);
                }
                next_send_time = std::chrono::steady_clock::now() + frame_interval;
            }

            const auto header = serialize_header(frame);
            if (!send_all(client_fd, header.data(), header.size())
                || !send_all(client_fd, frame.jpeg.data(), frame.jpeg.size())) {
                std::cout << "client disconnected\n";
                break;
            }

            const uint64_t total_sent = ++sent_frames;
            if (opts.log_every > 0 && total_sent % opts.log_every == 0) {
                std::cout << "stats: captured=" << captured_frames.load()
                          << " sent=" << total_sent
                          << " overwritten=" << frame_buffer.dropped_count() << "\n";
            }
        }

        close(client_fd);
    }

    close(server_fd);
    for (auto &capture : captures) {
        capture->request_stop();
    }
    frame_buffer.stop();
    for (auto &thread : capture_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    return 0;
}
