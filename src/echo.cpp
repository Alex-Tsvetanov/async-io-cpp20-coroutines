// echo.cpp - the same source on Linux and on Windows.
//
// Written for the DEV.BG C/C++ talk, 28 Oct 2026.
//
// Build and run it on both, point nc or smoke_test.py at it, and note that
// this file contains no #ifdef at all. Every platform difference is in
// io.hpp. Run it with --trace to watch the thread change and the buffer stay.
//
// naive.cpp has the same echo() with one line changed. Put the two side by side.

#include "server.hpp"

constexpr int kBufSize = 4096;

// One connection, one coroutine.
//
// `buf` is an ordinary local. Because this is a coroutine, it lives in the
// coroutine frame on the heap, not on a stack frame that is about to be
// reused. So it is still there, at the same address, when the kernel
// finishes writing into it and resumes us, on a different thread.
//
// That is the whole buffer-lifetime argument. There is nothing to see,
// which is the point: the language already had the right shape.
task echo(io_context& ctx, socket_t s) {
    char buf[kBufSize];                       // owned by this frame, and only this one
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

int main(int argc, char** argv) { return serve(argc, argv, "echo", echo); }
