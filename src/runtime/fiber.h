#pragma once
#include <functional>
#include <vector>
#include <memory>
#include <cstdint>
#include <stdexcept>
#include "value.h"

#if defined(_WIN32)
    #define NOVA_FIBER_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
    #define NOVA_FIBER_UCONTEXT
    #include <ucontext.h>
#else
    #error "Unsupported platform for fibers"
#endif

namespace novac {

// forward
struct NovaValue;
// Val alias now from value.h (TVal)

// ════════════════════════════════════════════════════════════════════════════
//  Fiber — one cooperative execution context
// ════════════════════════════════════════════════════════════════════════════

struct Fiber {
    static constexpr size_t STACK_SIZE = 1024 * 1024; // 1MB per fiber

#if defined(NOVA_FIBER_WINDOWS)
    LPVOID handle        = nullptr;
    LPVOID callerHandle  = nullptr;
#else
    ucontext_t ctx;
    ucontext_t callerCtx;
    std::vector<char> stack;
#endif

    bool done    = false;
    bool started = false;

    Val  yieldedValue;   // fiber → caller  (yield/return value)
    Val  sentValue;      // caller → fiber  (next(val) / resolved await)

    std::exception_ptr thrownException; // propagate errors across context switch

    std::function<void(Fiber&)> body;   // set before first resume

    Fiber() {
#if defined(NOVA_FIBER_UCONTEXT)
        stack.resize(STACK_SIZE);
#endif
    }

    ~Fiber() {
#if defined(NOVA_FIBER_WINDOWS)
        if (handle) { DeleteFiber(handle); handle = nullptr; }
#endif
    }

    Fiber(const Fiber&)            = delete;
    Fiber& operator=(const Fiber&) = delete;

    // ── platform suspend/resume ──────────────────────────────────────────────

    // call from INSIDE fiber to suspend back to caller
    void suspend() {
#if defined(NOVA_FIBER_WINDOWS)
        SwitchToFiber(callerHandle);
#else
        swapcontext(&ctx, &callerCtx);
#endif
    }

    // call from OUTSIDE fiber to resume it
    void resume() {
#if defined(NOVA_FIBER_WINDOWS)
        // first resume: need caller fiber handle
        if (!callerHandle)
            callerHandle = GetCurrentFiber();
        if (!callerHandle || callerHandle == (LPVOID)0x1e00)
            callerHandle = ConvertThreadToFiber(nullptr);
        SwitchToFiber(handle);
#else
        swapcontext(&callerCtx, &ctx);
#endif
        // rethrow any exception the fiber left for us
        if (thrownException)
            std::rethrow_exception(thrownException);
    }
};

// ════════════════════════════════════════════════════════════════════════════
//  Fiber entry trampolines
// ════════════════════════════════════════════════════════════════════════════

#if defined(NOVA_FIBER_WINDOWS)

inline void CALLBACK fiberEntry(LPVOID param) {
    Fiber* f = static_cast<Fiber*>(param);
    try {
        f->body(*f);
    } catch (...) {
        f->thrownException = std::current_exception();
    }
    f->done = true;
    // return to caller — loop in case caller resumes a done fiber
    while (true) SwitchToFiber(f->callerHandle);
}

inline void fiberInit(Fiber& f) {
    f.handle = CreateFiber(Fiber::STACK_SIZE, fiberEntry, &f);
    if (!f.handle) throw std::runtime_error("CreateFiber failed");
}

#else

// ucontext trampoline — makecontext only passes ints so split pointer
inline void fiberEntry(uint32_t hi, uint32_t lo) {
    Fiber* f = reinterpret_cast<Fiber*>(
        ((uintptr_t)hi << 32) | (uintptr_t)lo
    );
    try {
        f->body(*f);
    } catch (...) {
        f->thrownException = std::current_exception();
    }
    f->done = true;
    swapcontext(&f->ctx, &f->callerCtx);
    // should never reach here
    while (true) swapcontext(&f->ctx, &f->callerCtx);
}

inline void fiberInit(Fiber& f) {
    getcontext(&f.ctx);
    f.ctx.uc_stack.ss_sp   = f.stack.data();
    f.ctx.uc_stack.ss_size = f.stack.size();
    f.ctx.uc_link          = nullptr;
    uintptr_t ptr = reinterpret_cast<uintptr_t>(&f);
    makecontext(&f.ctx, (void(*)())fiberEntry, 2,
                (uint32_t)(ptr >> 32),
                (uint32_t)(ptr & 0xffffffff));
}

#endif

} // namespace novac