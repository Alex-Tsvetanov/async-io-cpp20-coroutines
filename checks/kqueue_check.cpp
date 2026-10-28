// kqueue_check.cpp - does the slide 14 kqueue card hold, under libkqueue on Linux?
//
// Part 1 compiles the card's await_suspend / await_resume bodies VERBATIM.
// Part 2 runs the contract the slide depends on, on a real socket pair:
//   a. registering EVFILT_READ with EV_ONESHOT and udata = &op does not fire early
//   b. when data arrives, the event comes back carrying the same &op in udata
//   c. EV_ONESHOT disarms: more data does NOT fire again until re-registered
//   d. re-registering (the card's "EV_ADD | EV_ONESHOT" again) fires again
//   e. the buffer is only touched by recv() in await_resume, after the event
//
// Build (after: sudo apt install libkqueue-dev), all on one line:
//   g++ -std=c++20 -Wall -Wextra -Werror kqueue_check.cpp $(pkg-config --cflags --libs libkqueue)

#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>

#include <coroutine>
#include <cstdio>
#include <cstring>

struct io_op { std::coroutine_handle<> h{}; int result = 0; };

// ---- Part 1: the slide card, verbatim ------------------------------------
struct recv_awaitable {
    int kq; int s; char* buf; int len;
    io_op op{};

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        // await_suspend
        op.h = h;
        struct kevent ev;
        EV_SET(&ev, s, EVFILT_READ,
               EV_ADD | EV_ONESHOT,
               0, 0, &op);   // find us here
        kevent(kq, &ev, 1, nullptr, 0, nullptr);
        return true;   // suspended
    }

    int await_resume() noexcept {
        // await_resume: the I/O is here
        return ::recv(s, buf, len, 0);
    }
};

// ---- Part 2: the runtime contract ----------------------------------------
static int failures = 0;
static void check(const char* what, bool ok) {
    std::printf("  %-58s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
}

static int poll_once(int kq, struct kevent* out, int ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    return kevent(kq, nullptr, 0, out, 1, &ts);
}

int main() {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { std::perror("socketpair"); return 2; }
    int kq = kqueue();
    if (kq < 0) { std::perror("kqueue"); return 2; }

    char buf[64];
    std::memset(buf, 'x', sizeof buf);
    recv_awaitable aw{kq, sv[0], buf, (int)sizeof buf};
    aw.await_suspend(std::noop_coroutine());   // registers, exactly as the card does

    struct kevent out;
    check("a. nothing fires before data arrives", poll_once(kq, &out, 100) == 0);
    check("e. buffer untouched while registered", buf[0] == 'x');

    (void)!write(sv[1], "ping", 4);
    int n = poll_once(kq, &out, 1000);
    check("b. data arrives: one event", n == 1);
    check("b. udata is &op, the trail comes back", n == 1 && out.udata == (void*)&aw.op);
    check("e. buffer still untouched until await_resume", buf[0] == 'x');
    int r = aw.await_resume();
    check("e. await_resume recv() reads the data", r == 4 && std::memcmp(buf, "ping", 4) == 0);

    (void)!write(sv[1], "more", 4);
    check("c. EV_ONESHOT: no second event without re-arming", poll_once(kq, &out, 300) == 0);

    aw.await_suspend(std::noop_coroutine());   // re-arm, as the next co_await would
    n = poll_once(kq, &out, 1000);
    check("d. re-armed: fires again", n == 1 && out.udata == (void*)&aw.op);

    std::printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
