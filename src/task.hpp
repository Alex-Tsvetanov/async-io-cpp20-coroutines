// task.hpp - the smallest coroutine type this talk needs.
//
// Written for the DEV.BG C/C++ talk, 28 Oct 2026. MIT licensed, see LICENSE.
//
// Deliberately fire-and-forget: no result, no continuation, no cancellation.
// A real framework needs all three. A 50-minute talk does not, and every extra
// member here is a member the audience has to read instead of the io layer.
#pragma once

#include <coroutine>
#include <exception>

struct task {
    struct promise_type {
        task get_return_object() noexcept { return {}; }

        // suspend_never: the coroutine starts running on whichever thread
        // called it, right up to its first co_await. That matters, and it is
        // the thing section 5 is about: the connection handler *begins* on the
        // acceptor thread and *continues* on the completion thread.
        std::suspend_never initial_suspend() noexcept { return {}; }

        // suspend_never: the frame destroys itself when the coroutine returns.
        // Nobody is waiting on it, so nobody has to clean it up.
        std::suspend_never final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        // A demo has no error policy. A real one must have one.
        void unhandled_exception() { std::terminate(); }
    };
};
