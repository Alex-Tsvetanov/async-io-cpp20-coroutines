// server.hpp - the part of both demos that is not the point.
//
// A listening socket, an accept loop on its own thread, and a trace line that
// names the thread it runs on. echo.cpp and naive.cpp both use this, so the
// only difference between them is the one line the talk is about.

#pragma once

#include "io.hpp"
#include "task.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// Which thread are we on? Named, not numbered, so the trace reads on screen.
inline thread_local const char* t_thread = "main";
inline bool g_trace = false;

// One line per event, only with --trace. Watch two things in it: the thread
// changes between "started" and "resumed", and the buffer address does not.
inline void trace(const char* what, socket_t s, const void* buf) {
    if (!g_trace) return;
    std::printf("  conn %-4lld %-8s on the %-10s thread, buf at %p\n",
                static_cast<long long>(s), what, t_thread, buf);
    std::fflush(stdout);
}

inline socket_t make_listener(unsigned short port) {
    socket_t ls = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ls == invalid_socket) {
        std::fprintf(stderr, "socket() failed\n");
        std::exit(1);
    }

    int on = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&on), sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "bind() failed on port %u\n", port);
        std::exit(1);
    }
    if (::listen(ls, 512) != 0) {
        std::fprintf(stderr, "listen() failed\n");
        std::exit(1);
    }
    return ls;
}

// Usage: <program> [port] [--trace]
//
// Accept is deliberately synchronous and on its own thread. Async accept is
// AcceptEx on Windows and IORING_OP_ACCEPT on Linux, and wiring both would
// double the demo for no teaching benefit.
//
// Worth saying out loud: each handler starts running on the ACCEPTOR thread,
// up to its first co_await, and is resumed on the COMPLETION thread after
// that. That is section 5 of the talk, and --trace shows it happening.
template <class Handler>
int serve(int argc, char** argv, const char* name, Handler handler) {
    unsigned short port = 9009;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--trace") == 0) g_trace = true;
        else port = static_cast<unsigned short>(std::atoi(argv[i]));
    }

    io_context ctx;
    socket_t ls = make_listener(port);

    std::thread acceptor([&] {
        t_thread = "acceptor";
        for (;;) {
            socket_t s = ::accept(ls, nullptr, nullptr);
            if (s == invalid_socket) break;
            ctx.attach(s);
            handler(ctx, s);
        }
    });
    acceptor.detach();

    t_thread = "completion";
    std::printf("%s listening on port %u  (%s)%s\n", name, port, backend_name(),
                g_trace ? "  trace on" : "");
    std::fflush(stdout);

    ctx.run();
    return 0;
}
