// naive.cpp - echo.cpp with the buffer moved out of the frame.
//
// Written for the DEV.BG C/C++ talk, 28 Oct 2026. Run it next to echo.cpp:
//
//     python smoke_test.py 9009 1      one connection: passes
//     python smoke_test.py 9009 100    a hundred: clients get each other's bytes
//
// The change looks like a sensible optimisation. A 4 KB buffer per idle
// connection is real memory, so share a small pool of buffers instead. It is
// the readiness assumption carried over to a completion kernel: "a buffer is
// free again once the call that used it has returned". On epoll that holds,
// because the kernel only touches your buffer inside recv(). On IOCP and
// io_uring it is false. The kernel keeps the pointer for as long as the
// operation is in flight, and under load that is long enough for another
// connection to be handed the same slot.
//
// At one connection nothing ever overlaps, so it passes every test you would
// think to write on a laptop. That is why this bug ships.

#include "server.hpp"

#include <atomic>

constexpr int kBufSize = 4096;

// Two shared buffers for a hundred connections, instead of one each. That is
// fifty connections per buffer, the same ratio as a pool of 200 buffers
// behind 10,000 connections, which is an ordinary production shape. The ratio
// sets how often it breaks, not whether: measured on 29 Sep 2026, eight slots
// still broke on 8 runs in 10 on Linux, two broke on all 10.
struct buffer_pool {
    static constexpr unsigned kSlots = 2;
    char slot[kSlots][kBufSize];
    std::atomic<unsigned> next{0};
    char* lend() { return slot[next++ % kSlots]; }  // never given back: "it is free once we return"
};
buffer_pool pool;

task echo(io_context& ctx, socket_t s) {
    char* buf = pool.lend();                  // THE one line: the buffer is no longer in the frame
    trace("started", s, buf);

    for (;;) {
        int n = co_await ctx.recv(s, buf, kBufSize);
        trace("resumed", s, buf);
        if (n <= 0) break;

        int sent = 0;
        while (sent < n) {
            int w = co_await ctx.send(s, buf + sent, n - sent);
            if (w <= 0) goto done;
            sent += w;
        }
    }

done:
    close_socket(s);
    co_return;
}

int main(int argc, char** argv) { return serve(argc, argv, "naive", echo); }
