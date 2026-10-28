// io.hpp - one async API over IOCP (Windows) and io_uring (Linux).
//
// Written for the DEV.BG C/C++ talk, 28 Oct 2026.
//
// The whole argument of the talk is in this file, and it is short on purpose.
// Read the two await_suspend bodies side by side: they are the same four steps
// in two different dialects.
//
//   1. remember the coroutine_handle
//   2. leave a trail: somewhere the completion can find the handle again
//   3. hand the operation to the kernel
//   4. return true, which means "I have suspended, resume me later"
//
// The trail comes before the hand-over, never after: once the kernel has the
// operation it can complete at any moment, on any thread. On Windows steps 2
// and 3 are one call, WSARecv(..., &op.ov). On Linux they are set_data, then
// submit.
//
// And read where `io_op` lives: inside the awaitable, which is a temporary in
// the co_await expression, which the compiler puts in the coroutine frame.
// So the OVERLAPPED / the SQE user_data stays at a fixed address for exactly
// as long as the operation is in flight. Nobody had to arrange that.

#pragma once

#include <coroutine>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
using socket_t = SOCKET;
inline constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
#  include <liburing.h>
#  include <mutex>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
inline constexpr socket_t invalid_socket = -1;
#endif

// ---------------------------------------------------------------------------
// The two platform details that would otherwise leak into the server: how a
// socket is closed, and what the backend is called. They live here so that
// echo.cpp can hold no #ifdef at all, which is the claim the talk makes on
// stage. Two lines of preprocessor moved one file down is not a trick: the
// point is that the user's code never has to branch, not that branching
// disappears from the program.
// ---------------------------------------------------------------------------
inline void close_socket(socket_t s) noexcept {
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

inline const char* backend_name() noexcept {
#if defined(_WIN32)
    return "IOCP";
#else
    return "io_uring";
#endif
}

// ---------------------------------------------------------------------------
// One in-flight operation. One suspended coroutine.
// ---------------------------------------------------------------------------
struct io_op {
#if defined(_WIN32)
    // MUST be the first member. The completion loop gets an OVERLAPPED* back
    // from the kernel and casts it straight to io_op*.
    OVERLAPPED ov{};
#endif
    std::coroutine_handle<> h{};
    int result = 0;
};

// ---------------------------------------------------------------------------
// The context: owns the completion queue and the loop that drains it.
// ---------------------------------------------------------------------------
class io_context {
public:
    io_context();
    ~io_context();

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    void attach(socket_t s);  // no-op on Linux, required on Windows
    void run();               // drains completions until stop()
    void stop();

    // The two awaitables. Both are returned by value and co_await'ed
    // immediately, so both live in the caller's coroutine frame.
    struct recv_awaitable;
    struct send_awaitable;

    recv_awaitable recv(socket_t s, char* buf, int len) noexcept;
    send_awaitable send(socket_t s, const char* buf, int len) noexcept;

#if defined(_WIN32)
    HANDLE iocp_ = nullptr;
#else
    io_uring ring_{};
    bool stopping_ = false;

    // The submission queue is single-producer. Two threads push into it
    // here: the acceptor, which runs each new echo() up to its first
    // co_await, and the completion thread, which resumes everything after.
    // Without this lock both can be handed the SAME submission slot and
    // write two operations into it: a lost receive, or a buffer pointer that
    // belongs to another connection. ThreadSanitizer caught exactly that on
    // 29 Sep 2026 while the smoke test still passed. IOCP needs no lock, the
    // port is thread-safe. The completion side stays lock-free: only run()
    // reads it.
    std::mutex sq_mu_;
    io_uring_sqe* next_sqe();  // caller holds sq_mu_
#endif
};

// ===========================================================================
// Windows: I/O completion ports
// ===========================================================================
#if defined(_WIN32)

inline io_context::io_context() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        std::abort();
    }
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!iocp_) {
        std::fprintf(stderr, "CreateIoCompletionPort failed: %lu\n", GetLastError());
        std::abort();
    }
}

inline io_context::~io_context() {
    if (iocp_) CloseHandle(iocp_);
    WSACleanup();
}

inline void io_context::attach(socket_t s) {
    // Associating the socket is what makes its completions land in our queue.
    if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), iocp_, 0, 0)) {
        std::fprintf(stderr, "attach failed: %lu\n", GetLastError());
    }
}

struct io_context::recv_awaitable {
    socket_t s;
    char* buf;
    int len;
    io_op op{};

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        op.h = h;                                     // 1. remember the handle
        WSABUF wb{static_cast<ULONG>(len), buf};
        DWORD flags = 0;
        int r = WSARecv(s, &wb, 1, nullptr, &flags,   // 3. hand it over, and
                        &op.ov, nullptr);             // 2. the trail, &op.ov,
                                                      //    in the same call
        if (r == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            op.result = -1;
            return false;  // failed to start: do not suspend, resume inline
        }
        return true;       // 4. suspended
    }

    int await_resume() const noexcept { return op.result; }
};

struct io_context::send_awaitable {
    socket_t s;
    const char* buf;
    int len;
    io_op op{};

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        op.h = h;
        WSABUF wb{static_cast<ULONG>(len), const_cast<CHAR*>(buf)};
        int r = WSASend(s, &wb, 1, nullptr, 0, &op.ov, nullptr);
        if (r == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            op.result = -1;
            return false;
        }
        return true;
    }

    int await_resume() const noexcept { return op.result; }
};

inline void io_context::run() {
    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = nullptr;
        BOOL ok = GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov, INFINITE);

        if (!ov) break;  // stop() posts a null OVERLAPPED

        // ov is the first member of io_op, so this points back at the
        // awaitable sitting in the suspended coroutine's frame.
        auto* op = reinterpret_cast<io_op*>(ov);
        op->result = ok ? static_cast<int>(bytes) : -1;
        op->h.resume();
    }
}

inline void io_context::stop() {
    PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
}

// ===========================================================================
// Linux: io_uring
// ===========================================================================
#else

inline io_context::io_context() {
    if (io_uring_queue_init(256, &ring_, 0) < 0) {
        std::fprintf(stderr, "io_uring_queue_init failed\n");
        std::abort();
    }
}

inline io_context::~io_context() { io_uring_queue_exit(&ring_); }

inline void io_context::attach(socket_t) {
    // Nothing to do. io_uring takes the fd per operation rather than
    // registering it up front. One of the small asymmetries that does not
    // abstract away cleanly.
}

struct io_context::recv_awaitable {
    io_context* ctx;
    socket_t s;
    char* buf;
    int len;
    io_op op{};

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        op.h = h;                                        // 1. remember
        std::lock_guard<std::mutex> lock(ctx->sq_mu_);
        io_uring_sqe* sqe = ctx->next_sqe();
        io_uring_prep_recv(sqe, s, buf, len, 0);         //    describe it
        io_uring_sqe_set_data(sqe, &op);                 // 2. leave a trail
        io_uring_submit(&ctx->ring_);                    // 3. hand over
        return true;                                     // 4. suspended
        // Nothing may touch *this after submit: the completion thread can
        // resume, and even destroy, this frame before we return.
    }

    int await_resume() const noexcept { return op.result; }
};

struct io_context::send_awaitable {
    io_context* ctx;
    socket_t s;
    const char* buf;
    int len;
    io_op op{};

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        op.h = h;
        std::lock_guard<std::mutex> lock(ctx->sq_mu_);
        io_uring_sqe* sqe = ctx->next_sqe();
        io_uring_prep_send(sqe, s, buf, len, 0);
        io_uring_sqe_set_data(sqe, &op);
        io_uring_submit(&ctx->ring_);
        return true;
    }

    int await_resume() const noexcept { return op.result; }
};

inline void io_context::run() {
    while (!stopping_) {
        io_uring_cqe* cqe = nullptr;
        if (io_uring_wait_cqe(&ring_, &cqe) < 0) break;

        auto* op = static_cast<io_op*>(io_uring_cqe_get_data(cqe));
        if (op) {
            op->result = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);
            op->h.resume();
        } else {
            io_uring_cqe_seen(&ring_, cqe);  // the stop() wakeup
        }
    }
}

inline io_uring_sqe* io_context::next_sqe() {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {                    // queue full: flush it and try once more
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    if (!sqe) {
        std::fprintf(stderr, "io_uring submission queue full\n");
        std::abort();
    }
    return sqe;
}

inline void io_context::stop() {
    stopping_ = true;
    std::lock_guard<std::mutex> lock(sq_mu_);
    io_uring_sqe* sqe = next_sqe();
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, nullptr);
    io_uring_submit(&ring_);
}

#endif

// ---------------------------------------------------------------------------
// Factory functions. Identical call site on both platforms, which is the
// entire point of the exercise.
// ---------------------------------------------------------------------------
inline io_context::recv_awaitable
io_context::recv(socket_t s, char* buf, int len) noexcept {
#if defined(_WIN32)
    return recv_awaitable{s, buf, len};
#else
    return recv_awaitable{this, s, buf, len};
#endif
}

inline io_context::send_awaitable
io_context::send(socket_t s, const char* buf, int len) noexcept {
#if defined(_WIN32)
    return send_awaitable{s, buf, len};
#else
    return send_awaitable{this, s, buf, len};
#endif
}
