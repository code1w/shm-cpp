// server.cpp — Multi-client server using RingChannel + EventLoop

#include <shm_ipc/ring_channel.hpp>
#include <shm_ipc/event_loop.hpp>

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <unordered_map>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr int kTimerIntervalMs = 300;
constexpr int kMaxTicks        = 40;
constexpr int kMaxClients      = 16;

using Channel = shm_ipc::DefaultRingChannel;

struct ClientState {
    int                   id;
    shm_ipc::UniqueFd     socket_fd;
    Channel               channel;
    int                   timer_fd = -1;
    int                   tick = 0;
    int                   total_read = 0;
    std::unique_ptr<char[]> read_buf; // heap-allocated read buffer
};

shm_ipc::EventLoop *g_loop = nullptr;

void signal_handler(int /*signo*/) {
    if (g_loop) g_loop->stop();
}

shm_ipc::UniqueFd create_listen_socket(const char *path) {
    shm_ipc::UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    ::unlink(path);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::bind(sfd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    if (::listen(sfd.get(), 5) < 0)
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));

    return sfd;
}

} // anonymous namespace

int main() {
    try {
        struct sigaction sa{};
        sa.sa_handler = signal_handler;
        ::sigemptyset(&sa.sa_mask);
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);

        shm_ipc::EventLoop loop;
        g_loop = &loop;

        std::unordered_map<int, std::unique_ptr<ClientState>> clients;
        int next_client_id = 1;

        auto listen_fd = create_listen_socket(shm_ipc::kDefaultSocketPath);
        std::printf("server: listening on %s (max %d clients, ring=%zuMB, max_msg=%zuB)\n",
                    shm_ipc::kDefaultSocketPath, kMaxClients,
                    Channel::Ring::capacity / (1024 * 1024),
                    Channel::max_msg_size);

        // --- Accept callback ---
        loop.add_fd(listen_fd.get(), [&](int lfd, short /*revents*/) {
            if (static_cast<int>(clients.size()) >= kMaxClients) {
                std::fprintf(stderr, "server: max clients reached, rejecting\n");
                shm_ipc::UniqueFd rejected{::accept(lfd, nullptr, nullptr)};
                return;
            }

            shm_ipc::UniqueFd cfd{::accept(lfd, nullptr, nullptr)};
            if (!cfd) return;

            int cid = next_client_id++;
            std::printf("server: client #%d connected (fd=%d)\n", cid, cfd.get());

            auto cs = std::make_unique<ClientState>();
            cs->id = cid;
            cs->channel = Channel::accept(cfd.get());
            cs->read_buf = std::make_unique<char[]>(Channel::max_msg_size);
            std::printf("server: client #%d ring channel established\n", cid);

            int client_fd = cfd.get();
            cs->socket_fd = std::move(cfd);

            ClientState *csp = cs.get();
            int tfd = loop.add_timer(kTimerIntervalMs, [&loop, &clients, client_fd, csp](int tfd, short /*rev*/) {
                shm_ipc::EventLoop::drain_timerfd(tfd);

                if (++csp->tick > kMaxTicks) {
                    std::printf("server: client #%d max ticks reached\n", csp->id);
                    loop.remove_timer(tfd);
                    loop.remove_fd(client_fd);
                    clients.erase(client_fd);
                    return;
                }

                // Write a response message
                auto now = std::time(nullptr);
                std::array<char, 32> ts{};
                std::strftime(ts.data(), ts.size(), "%H:%M:%S", std::localtime(&now));

                std::array<char, 4096> msg{};
                int msg_len = std::snprintf(msg.data(), msg.size(),
                    "[server seq=%03d time=%s client=#%d] tick=%d",
                    csp->tick, ts.data(), csp->id, csp->tick);

                if (csp->channel.try_write(msg.data(),
                        static_cast<uint32_t>(msg_len + 1), csp->tick) == 0) {
                    char notify = 'S';
                    ::write(client_fd, &notify, 1);
                }

                // Batch-read all available client messages
                uint32_t len{}, seq{};
                int batch = 0;
                while (csp->channel.try_read(csp->read_buf.get(), &len, &seq) == 0) {
                    std::printf("Tserver: client #%d [seq=%03u len=%u]: %.*s\n",
                                csp->id, seq, len,
                                static_cast<int>(len), csp->read_buf.get());
                    ++csp->total_read;
                    ++batch;
                }
                if (batch > 0) {
                    std::printf("Tserver: client #%d batch=%d total=%d\n",
                                csp->id, batch, csp->total_read);
                }
            });
            csp->timer_fd = tfd;

            loop.add_fd(client_fd, [&loop, &clients, client_fd, csp](int /*fd*/, short revents) {
                if (revents & (POLLHUP | POLLERR)) {
                    std::printf("server: client #%d disconnected (HUP/ERR)\n", csp->id);
                    loop.remove_timer(csp->timer_fd);
                    loop.remove_fd(client_fd);
                    clients.erase(client_fd);
                    return;
                }
                if (revents & POLLIN) {
                    char notify{};
                    auto n = ::read(client_fd, &notify, 1);
                    if (n <= 0 || notify == 0) {
                        uint32_t len{}, seq{};
                        while (csp->channel.try_read(csp->read_buf.get(), &len, &seq) == 0) {
                            std::printf("server: client #%d [seq=%03u len=%u]: %.*s\n",
                                        csp->id, seq, len,
                                        static_cast<int>(len), csp->read_buf.get());
                            ++csp->total_read;
                        }
                        std::printf("server: client #%d disconnected, total read=%d\n",
                                    csp->id, csp->total_read);
                        loop.remove_timer(csp->timer_fd);
                        loop.remove_fd(client_fd);
                        clients.erase(client_fd);
                        return;
                    }
                }
            });

            clients[client_fd] = std::move(cs);
        });

        loop.run();

        std::printf("\nserver: shutting down (%zu clients remaining)\n", clients.size());
        clients.clear();
        ::unlink(shm_ipc::kDefaultSocketPath);

    } catch (const std::exception &e) {
        std::fprintf(stderr, "server: error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return 0;
}
