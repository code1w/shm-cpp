// client.cpp — Client using RingChannel + EventLoop

#include <shm_ipc/ring_channel.hpp>
#include <shm_ipc/event_loop.hpp>

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr int kTimerIntervalMs = 100;
constexpr int kMaxTicks        = 100;

shm_ipc::EventLoop *g_loop = nullptr;

void signal_handler(int /*signo*/) {
    if (g_loop) g_loop->stop();
}

shm_ipc::UniqueFd connect_to_server(const char *path) {
    shm_ipc::UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::connect(sfd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::string("connect: ") + std::strerror(errno));

    return sfd;
}

} // anonymous namespace

int main() {
    try {
        // Graceful shutdown
        struct sigaction sa{};
        sa.sa_handler = signal_handler;
        ::sigemptyset(&sa.sa_mask);
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);

        // Connect and establish bidirectional ring channel
        auto sfd = connect_to_server(shm_ipc::kDefaultSocketPath);
        std::printf("client: connected to %s\n", shm_ipc::kDefaultSocketPath);

        auto channel = shm_ipc::DefaultRingChannel::connect(sfd.get());
        std::printf("client: ring channel established\n\n");

        shm_ipc::EventLoop loop;
        g_loop = &loop;

        int tick = 0, write_ok = 0, write_full = 0;
        int socket_fd = sfd.get();

        // --- Timer: write messages to ring buffer ---
        loop.add_timer(kTimerIntervalMs, [&](int tfd, short /*rev*/) {
            shm_ipc::EventLoop::drain_timerfd(tfd);

            if (++tick > kMaxTicks) {
                loop.stop();
                return;
            }

            auto now = std::time(nullptr);
            std::array<char, 32> ts{};
            std::strftime(ts.data(), ts.size(), "%H:%M:%S", std::localtime(&now));

            std::array<char, 256> msg{};
            int msg_len = std::snprintf(msg.data(), msg.size(),
                "[client seq=%03d time=%s] tick=%d", tick, ts.data(), tick);

            if (channel.try_write(msg.data(),
                    static_cast<uint32_t>(msg_len + 1), tick) == 0) {
                ++write_ok;
                char notify = 'C';
                ::write(socket_fd, &notify, 1);
            } else {
                ++write_full;
                std::fprintf(stderr, "client: [seq=%03d] ring full! (ok=%d, full=%d)\n",
                             tick, write_ok, write_full);
            }

            if (tick % 10 == 0) {
                std::printf("client: --- tick=%d ok=%d full=%d readable=%lu ---\n",
                            tick, write_ok, write_full,
                            static_cast<unsigned long>(channel.readable()));
            }
        });

        // --- Socket: read server notifications ---
        loop.add_fd(socket_fd, [&](int /*fd*/, short revents) {
            if (revents & (POLLHUP | POLLERR)) {
                std::printf("client: server disconnected\n");
                loop.stop();
                return;
            }
            if (revents & POLLIN) {
                char notify{};
                auto n = ::read(socket_fd, &notify, 1);
                if (n <= 0 || notify == 0) {
                    std::printf("client: server disconnected\n");
                    loop.stop();
                    return;
                }
                if (notify == 'S') {
                    std::array<char, 256> data{};
                    uint32_t len{}, seq{};
                    while (channel.try_read(data.data(), &len, &seq) == 0) {
                        std::printf("client: server [seq=%03u]: %.*s\n",
                                    seq, static_cast<int>(len), data.data());
                    }
                }
            }
        });

        std::printf("client: timer=%dms, max_ticks=%d\n\n", kTimerIntervalMs, kMaxTicks);
        loop.run();

        // Shutdown
        std::printf("\nclient: done. ok=%d full=%d\n", write_ok, write_full);
        char end = 0;
        ::write(socket_fd, &end, 1);
        ::usleep(100000);

    } catch (const std::exception &e) {
        std::fprintf(stderr, "client: error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return 0;
}
