# Async I/O with C++20 coroutines

**One API over epoll, kqueue, IOCP and io_uring.**

Slides and demo code from the talk at the
[DEV.BG C/C++ user group](https://dev.bg/groups/c-c-plus-plus/), 28 October 2026, by Alex Tsvetanov.

## What is here

| Path | What it is |
|---|---|
| `slides/Async-IO-with-Cpp20-Coroutines.pdf` | The deck. Read this one. |
| `slides/Async-IO-with-Cpp20-Coroutines.pptx` | The same deck, editable. |
| `src/io.hpp` | The argument of the talk: IOCP and io_uring behind identical call sites. |
| `src/echo.cpp` | A TCP echo server. No `#ifdef` at all. The buffer lives in the coroutine frame. |
| `src/naive.cpp` | The same handler with one line changed: the buffer comes from a shared pool. |
| `src/server.hpp` | Listener, accept thread, `--trace`. Shared, so the two servers differ only where it matters. |
| `src/task.hpp` | The smallest coroutine type that works. Fire and forget. |
| `smoke_test.py` | Four correctness checks, or one concurrency check at N clients. |
| `client.py` | Sends a few lines over one connection. Stands in for `nc` on Windows. |
| `checks/kqueue_check.cpp` | Compiles the kqueue card from slide 14 and checks the contract it relies on. |

The call site is the same on both platforms:

```cpp
int n = co_await ctx.recv(s, buf, kBufSize);
int w = co_await ctx.send(s, buf + sent, n - sent);
```

## The four steps

Every `await_suspend` in `io.hpp` does the same four things, in this order:

```
1. remember the coroutine_handle
2. leave a trail: somewhere the completion can find the handle again
3. hand the operation to the kernel
4. return true, meaning "I have suspended, resume me later"
```

The trail comes before the hand-over, never after. Once the kernel has the operation it can
complete at any moment, on any thread. On Windows steps 2 and 3 are one call, with `&op.ov` as
the trail. On Linux they are `io_uring_sqe_set_data`, then `io_uring_submit`.

## Build

### Windows

MinGW g++:

```bash
g++ -std=c++20 -Wall -Wextra -O2 -o echo.exe src/echo.cpp -lws2_32
```

```bash
g++ -std=c++20 -Wall -Wextra -O2 -o naive.exe src/naive.cpp -lws2_32
```

MSVC:

```
cl /std:c++20 /EHsc /W4 /Fe:echo.exe src\echo.cpp ws2_32.lib
```

### Linux

Needs liburing:

```bash
sudo apt install liburing-dev cmake g++
```

```bash
g++ -std=c++20 -Wall -Wextra -O2 -o echo src/echo.cpp $(pkg-config --cflags --libs liburing) -pthread
```

```bash
g++ -std=c++20 -Wall -Wextra -O2 -o naive src/naive.cpp $(pkg-config --cflags --libs liburing) -pthread
```

### CMake, either platform

Builds both `echo` and `naive`.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

## Run the demo

Commands are for Windows. On Linux drop the `.exe`.

**1. The frame, visible.** Start `echo` with `--trace` and connect one client:

```bash
./echo.exe 9009 --trace
```

```bash
python client.py 9009 "hello" "again"
```

Each message shows the handler starting on the acceptor thread and resuming on the completion
thread, with the same buffer address. Different thread, same buffer. That is the coroutine frame
doing the work.

Any raw TCP client works too. Each line you type comes straight back:

```bash
curl telnet://localhost:9009
```

**2. The frame holds up under load.**

```bash
python smoke_test.py 9009 100
```

**3. The naive wrapper breaks.**

```bash
./naive.exe 9010
```

```bash
python smoke_test.py 9010 1
```

```bash
python smoke_test.py 9010 100
```

At 1 client it is correct. At 100 the test reports clients that got another client's bytes.
Nothing crashes. If a run comes back clean, run it again: it is a race, and the kernel decides
the timing. Typing lines by hand never breaks it, because hand-typed lines never overlap.

Measured on 29 September 2026, 5 runs each at 100 clients:

| | `echo` | `naive` |
|---|---|---|
| **Windows, IOCP** | 100/100 every run | failed 5 of 5, 17 to 27 clients got another client's bytes |
| **Linux, io_uring** | 100/100 every run | failed 5 of 5, 7 to 13 clients got another client's bytes |

It breaks less often on Linux. IOCP writes into the buffer the moment data arrives. io_uring
arms a poll for a socket receive and copies when the completion is processed, so the window is
narrower. It is never closed.

## Deliberately missing

Each one is a fair question, and the honest answer is that it would not fit on a slide.

- **Async accept.** `AcceptEx` on Windows, `IORING_OP_ACCEPT` on Linux. Accept runs on its own
  thread with a blocking call.
- **epoll and kqueue backends.** They are on slide 14 as code. The demo runs the two completion
  kernels, because that is where buffer ownership bites.
- **Cancellation, timeouts, back-pressure, error mapping**, a correct buffer pool, more than one
  completion thread.
- **A result type for `task`.** It is fire and forget: no result, no continuation, no exceptions.

## Useful links

### C++20 coroutines

- [Coroutines (C++20)](https://en.cppreference.com/w/cpp/language/coroutines), cppreference.
  The language rules, including where exceptions go across a suspend.
- [`std::coroutine_handle`](https://en.cppreference.com/w/cpp/coroutine/coroutine_handle) and
  [`std::suspend_always` / `std::suspend_never`](https://en.cppreference.com/w/cpp/coroutine/suspend_always),
  cppreference.
- Lewis Baker's series:
  [Coroutine Theory](https://lewissbaker.github.io/2017/09/25/coroutine-theory),
  [Understanding operator co_await](https://lewissbaker.github.io/2017/11/17/understanding-operator-co-await),
  [Understanding the promise type](https://lewissbaker.github.io/2018/09/05/understanding-the-promise-type).

### Coroutine thread pools and libraries

- [cppcoro](https://github.com/lewissbaker/cppcoro), Lewis Baker. See `static_thread_pool`, and
  `io_service` for coroutine I/O.
- [libcoro](https://github.com/jbaldwin/libcoro), Josh Baldwin. See `coro::thread_pool`. Its
  README also warns about `thread_local` across a `co_await`, which is the thread hop from the talk.
- [P2300, `std::execution`](https://wg21.link/p2300). Schedulers, senders and receivers, adopted
  for C++26. It composes async work. It does not talk to the kernel.

### Windows: IOCP

- [I/O Completion Ports](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports),
  Microsoft Learn.
- [`GetQueuedCompletionStatus`](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getqueuedcompletionstatus)
  and [`WSARecv`](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsarecv).
- [I/O ring, `ioringapi.h`](https://learn.microsoft.com/en-us/windows/win32/api/ioringapi/).
  Windows' io_uring-style API. Documented for files, not sockets.
- [One year to I/O ring: what changed](https://windows-internals.com/one-year-to-i-o-ring-what-changed/),
  Windows Internals.

### Linux: io_uring

- [Efficient IO with io_uring](https://kernel.dk/io_uring.pdf), Jens Axboe. The design paper.
- [`io_uring(7)`](https://man7.org/linux/man-pages/man7/io_uring.7.html), the man page.
- [liburing](https://github.com/axboe/liburing), the library the demo uses.
- [Lord of the io_uring](https://unixism.net/loti/), a guide with worked examples.

### Readiness: epoll and kqueue

- [`epoll(7)`](https://man7.org/linux/man-pages/man7/epoll.7.html).
- [`kqueue(2)`](https://man.freebsd.org/cgi/man.cgi?query=kqueue), FreeBSD.
- [libkqueue](https://github.com/mheily/libkqueue), the kqueue API on Linux. `checks/kqueue_check.cpp`
  runs against it. That proves the API contract, not macOS kernel behaviour.

### From the Q&A list

- **io_uring in Docker** is blocked by the default seccomp profile since Docker Engine 25.0:
  [moby/moby#46762](https://github.com/moby/moby/pull/46762). Workaround, a custom profile:
  [Docker seccomp](https://docs.docker.com/engine/security/seccomp/).
- **Rust**: [mio](https://github.com/tokio-rs/mio), under tokio, is readiness based on every
  platform. [tokio-uring](https://docs.rs/tokio-uring/latest/) takes the buffer by value and hands
  it back, because ownership passes to the kernel. The same problem as the talk, solved by moving
  the buffer instead of keeping it in the frame.
- **C++26**: [Herb Sutter's St. Louis 2024 trip report](https://herbsutter.com/2024/07/02/trip-report-summer-iso-c-standards-meeting-st-louis-mo-usa/),
  where P2300 was adopted.

### By the author

- *Optimizing Asynchronous Event Dispatch in Modern C++ Publish/Subscribe Systems*, TELECOM 2025.
  [DOI](https://doi.org/10.1109/TELECOM66943.2025.11304088).
- *Modern C++ Publish/Subscribe Pattern*, ICEST 2025, with Assoc. Prof. Ivan Stankov.
  [DOI](https://doi.org/10.1109/icest66328.2025.11098436).

## License

The code is under the [MIT License](LICENSE). The slides are shared for reference: the
template, the DEV.BG logo and the partner logos belong to their owners.

## Contact

[linkedin.com/in/alex-tsvetanov](https://www.linkedin.com/in/alex-tsvetanov) ·
[github.com/Alex-Tsvetanov](https://github.com/Alex-Tsvetanov)
