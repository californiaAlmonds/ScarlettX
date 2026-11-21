// Exercises the XTaskQueue / XAsync / XAsyncProvider slots of ApiImpl {073B7DCB}
// the way a guest does: through the raw vtable, with the integer values of the
// public GDK headers written out literally (never the repo's own enum names).

#include "scarlettx/gdk/xgameruntime_abi.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

#define CHECK(...)                                                                    \
    do {                                                                              \
        if (!(__VA_ARGS__)) {                                                         \
            std::fprintf(stderr, "[test_xtaskqueue_xasync] FAILED: %s at line %d\n",  \
                         #__VA_ARGS__, __LINE__);                                     \
            std::fflush(stderr);                                                      \
            return false;                                                             \
        }                                                                             \
    } while (0)

namespace {

// Literal values from the public headers.
constexpr uint32_t kManual = 0;      // XTaskQueueDispatchMode::Manual
constexpr uint32_t kThreadPool = 1;  // XTaskQueueDispatchMode::ThreadPool
constexpr uint32_t kSerialized = 2;  // XTaskQueueDispatchMode::SerializedThreadPool
constexpr uint32_t kImmediate = 3;   // XTaskQueueDispatchMode::Immediate
constexpr uint32_t kWorkPort = 0;        // XTaskQueuePort::Work
constexpr uint32_t kCompletionPort = 1;  // XTaskQueuePort::Completion
constexpr uint32_t kOpBegin = 0, kOpDoWork = 1, kOpGetResult = 2, kOpCancel = 3, kOpCleanup = 4;

constexpr int32_t kOk = 0;
constexpr int32_t kEPending = static_cast<int32_t>(0x8000000Au);
constexpr int32_t kEAbort = static_cast<int32_t>(0x80004004u);
constexpr int32_t kEFail = static_cast<int32_t>(0x80004005u);
constexpr int32_t kEInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kEUnexpected = static_cast<int32_t>(0x8000FFFFu);
constexpr int32_t kENotSupported = static_cast<int32_t>(0x80070032u);
constexpr int32_t kEInsufficientBuffer = static_cast<int32_t>(0x8007007Au);
constexpr int32_t kENoTaskQueue = static_cast<int32_t>(0x800701ABu);

// Vtable slot indices of ApiImpl {073B7DCB}.
enum Slot : size_t {
    kAsyncGetStatus = 3, kAsyncGetResultSize = 4, kAsyncCancel = 5, kAsyncRun = 6,
    kAsyncBegin = 7, kAsyncSchedule = 9, kAsyncComplete = 10, kAsyncGetResult = 11,
    kQueueCreate = 12, kQueueCreateComposite = 13, kQueueGetPort = 14, kQueueDuplicate = 15,
    kQueueDispatch = 16, kQueueClose = 17, kQueueSubmit = 18, kQueueSubmitDelayed = 19,
    kQueueRegisterWaiter = 20, kQueueUnregisterWaiter = 21, kQueueTerminate = 22,
    kQueueRegisterMonitor = 23, kQueueUnregisterMonitor = 24, kQueueGetProcess = 25,
    kQueueSetProcess = 26,
};

struct Guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};
constexpr Guid kGuid073B7DCB = {
    0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}};

void* g_api = nullptr;

template <typename R, typename... A>
R Call(size_t slot, A... args) {
    using Fn = R(SCARLETTX_GDK_CALLBACK*)(void*, A...);
    void** vtbl = *reinterpret_cast<void***>(g_api);
    return reinterpret_cast<Fn>(vtbl[slot])(g_api, args...);
}

using Queue = void*;
using TaskCallback = void(SCARLETTX_GDK_CALLBACK*)(void*, bool);

int32_t CreateQueue(uint32_t work, uint32_t completion, Queue* out) {
    return Call<int32_t, uint32_t, uint32_t, Queue*>(kQueueCreate, work, completion, out);
}
void CloseQueue(Queue q) { Call<void, Queue>(kQueueClose, q); }
int32_t Submit(Queue q, uint32_t port, void* ctx, TaskCallback cb) {
    return Call<int32_t, Queue, uint32_t, void*, TaskCallback>(kQueueSubmit, q, port, ctx, cb);
}
int32_t SubmitDelayed(Queue q, uint32_t port, uint32_t ms, void* ctx, TaskCallback cb) {
    return Call<int32_t, Queue, uint32_t, uint32_t, void*, TaskCallback>(
        kQueueSubmitDelayed, q, port, ms, ctx, cb);
}
bool Dispatch(Queue q, uint32_t port, uint32_t timeout_ms) {
    return Call<bool, Queue, uint32_t, uint32_t>(kQueueDispatch, q, port, timeout_ms);
}
int32_t Terminate(Queue q, bool wait, void* ctx, void(SCARLETTX_GDK_CALLBACK* cb)(void*)) {
    return Call<int32_t, Queue, bool, void*, void(SCARLETTX_GDK_CALLBACK*)(void*)>(
        kQueueTerminate, q, wait, ctx, cb);
}

bool WaitFor(const std::function<bool()>& pred, int timeout_ms = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();  // sleep granularity on Windows is ~15 ms
    }
    return true;
}

struct Counters {
    std::atomic<int> ran{0};
    std::atomic<int> canceled{0};
    std::atomic<int> terminated{0};
};

void SCARLETTX_GDK_CALLBACK CountCb(void* ctx, bool canceled) {
    auto* c = static_cast<Counters*>(ctx);
    if (canceled) {
        c->canceled.fetch_add(1);
    }
    c->ran.fetch_add(1);
}

void SCARLETTX_GDK_CALLBACK TerminatedCb(void* ctx) {
    static_cast<Counters*>(ctx)->terminated.fetch_add(1);
}

// --- Dispatch modes, by their header integers --------------------------------

struct SerialState {
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    std::atomic<int> next{0};
    std::atomic<int> out_of_order{0};
    std::atomic<int> ran{0};
};
struct SerialItem {
    SerialState* state;
    int index;
};

void SCARLETTX_GDK_CALLBACK SerialCb(void* ctx, bool /*canceled*/) {
    auto* item = static_cast<SerialItem*>(ctx);
    SerialState* s = item->state;
    const int now = s->in_flight.fetch_add(1) + 1;
    int prev = s->max_in_flight.load();
    while (now > prev && !s->max_in_flight.compare_exchange_weak(prev, now)) {
    }
    if (s->next.fetch_add(1) != item->index) {
        s->out_of_order.fetch_add(1);
    }
    std::this_thread::yield();
    s->in_flight.fetch_sub(1);
    s->ran.fetch_add(1);
}

struct ThreadProbe {
    std::atomic<int> ran{0};
    std::thread::id thread;
};
void SCARLETTX_GDK_CALLBACK ThreadProbeCb(void* ctx, bool /*canceled*/) {
    auto* p = static_cast<ThreadProbe*>(ctx);
    p->thread = std::this_thread::get_id();
    p->ran.fetch_add(1);
}

bool TestDispatchModes() {
    Queue q = nullptr;
    CHECK(CreateQueue(4, kManual, &q) == kEInvalidArg && q == nullptr);

    // (1, 0): the work port runs on the pool by itself, the completion port only
    // inside XTaskQueueDispatch on the calling thread.
    CHECK(CreateQueue(kThreadPool, kManual, &q) == kOk && q != nullptr);
    ThreadProbe work;
    CHECK(Submit(q, kWorkPort, &work, ThreadProbeCb) == kOk);
    CHECK(WaitFor([&] { return work.ran.load() == 1; }));
    CHECK(work.thread != std::this_thread::get_id());
    CHECK(!Dispatch(q, kWorkPort, 0));

    ThreadProbe completion;
    CHECK(Submit(q, kCompletionPort, &completion, ThreadProbeCb) == kOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(completion.ran.load() == 0);
    CHECK(Dispatch(q, kCompletionPort, 0));
    CHECK(completion.ran.load() == 1 && completion.thread == std::this_thread::get_id());
    CHECK(!Dispatch(q, kCompletionPort, 0));
    CloseQueue(q);

    // (0, 0): nothing runs until dispatched.
    CHECK(CreateQueue(kManual, kManual, &q) == kOk);
    ThreadProbe manual;
    CHECK(Submit(q, kWorkPort, &manual, ThreadProbeCb) == kOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(manual.ran.load() == 0);
    CHECK(Dispatch(q, kWorkPort, 0) && manual.ran.load() == 1);
    CloseQueue(q);

    // (2, 2): pool threads, one callback at a time, in submission order.
    CHECK(CreateQueue(kSerialized, kSerialized, &q) == kOk);
    SerialState serial;
    std::vector<SerialItem> items(400);
    for (int i = 0; i < 400; ++i) {
        items[i] = {&serial, i};
        CHECK(Submit(q, kWorkPort, &items[i], SerialCb) == kOk);
    }
    CHECK(WaitFor([&] { return serial.ran.load() == 400; }));
    CHECK(serial.max_in_flight.load() == 1);
    CHECK(serial.out_of_order.load() == 0);
    CloseQueue(q);

    // (3, 3): runs inside the submit call.
    CHECK(CreateQueue(kImmediate, kImmediate, &q) == kOk);
    ThreadProbe immediate;
    CHECK(Submit(q, kWorkPort, &immediate, ThreadProbeCb) == kOk);
    CHECK(immediate.ran.load() == 1 && immediate.thread == std::this_thread::get_id());
    CloseQueue(q);
    return true;
}

// --- Process queue -----------------------------------------------------------

int32_t SCARLETTX_GDK_CALLBACK NoopWork(XAsyncBlock* /*async*/) { return kOk; }

bool TestProcessQueue() {
    // The getter returns bool, not HRESULT: 1 with a handle the caller must close.
    Queue q = nullptr;
    CHECK(Call<uint8_t, Queue*>(kQueueGetProcess, &q) == 1 && q != nullptr);
    CloseQueue(q);

    Call<void, Queue>(kQueueSetProcess, nullptr);
    q = reinterpret_cast<Queue>(static_cast<uintptr_t>(0x1234));
    CHECK(Call<uint8_t, Queue*>(kQueueGetProcess, &q) == 0 && q == nullptr);

    // An async call without a queue has nothing to run on.
    XAsyncBlock block{};
    CHECK(Call<int32_t, XAsyncBlock*, int32_t(SCARLETTX_GDK_CALLBACK*)(XAsyncBlock*)>(
              kAsyncRun, &block, NoopWork) == kENoTaskQueue);

    Queue mine = nullptr;
    CHECK(CreateQueue(kThreadPool, kThreadPool, &mine) == kOk);
    Call<void, Queue>(kQueueSetProcess, mine);
    CHECK(Call<uint8_t, Queue*>(kQueueGetProcess, &q) == 1 && q == mine);
    CloseQueue(q);
    CloseQueue(mine);  // the process slot keeps its own reference

    XAsyncBlock block2{};
    CHECK(Call<int32_t, XAsyncBlock*, int32_t(SCARLETTX_GDK_CALLBACK*)(XAsyncBlock*)>(
              kAsyncRun, &block2, NoopWork) == kOk);
    CHECK(Call<int32_t, XAsyncBlock*, bool>(kAsyncGetStatus, &block2, true) == kOk);
    return true;
}

// --- Delayed callbacks, monitors, composite queues, waiters ------------------

std::atomic<int> g_monitor_calls{0};
void SCARLETTX_GDK_CALLBACK MonitorCb(void* /*ctx*/, void* /*queue*/, uint32_t /*port*/) {
    g_monitor_calls.fetch_add(1);
}

bool TestDelayedCompositeWaiter() {
    Queue q = nullptr;
    CHECK(CreateQueue(kManual, kManual, &q) == kOk);

    uint64_t monitor_token = 0;
    CHECK(Call<int32_t, Queue, void*, void(SCARLETTX_GDK_CALLBACK*)(void*, void*, uint32_t),
               uint64_t*>(kQueueRegisterMonitor, q, nullptr, MonitorCb, &monitor_token) == kOk);

    Counters delayed;
    const auto start = std::chrono::steady_clock::now();
    CHECK(SubmitDelayed(q, kWorkPort, 40, &delayed, CountCb) == kOk);
    CHECK(!Dispatch(q, kWorkPort, 0));
    CHECK(Dispatch(q, kWorkPort, 2000));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    CHECK(delayed.ran.load() == 1 && delayed.canceled.load() == 0 && elapsed >= 30);
    CHECK(WaitFor([] { return g_monitor_calls.load() == 1; }));
    Call<void, Queue, uint64_t>(kQueueUnregisterMonitor, q, monitor_token);
    CHECK(Submit(q, kWorkPort, &delayed, CountCb) == kOk);
    CHECK(Dispatch(q, kWorkPort, 0));
    CHECK(g_monitor_calls.load() == 1);

    // A composite queue shares the ports it was built from, but terminating it
    // only cancels its own callbacks.
    void* work_port = nullptr;
    CHECK(Call<int32_t, Queue, uint32_t, void**>(kQueueGetPort, q, kWorkPort, &work_port) == kOk);
    CHECK(work_port != nullptr);
    Queue composite = nullptr;
    CHECK(Call<int32_t, void*, void*, Queue*>(kQueueCreateComposite, work_port, work_port,
                                              &composite) == kOk);
    Counters own, shared;
    CHECK(Submit(q, kWorkPort, &own, CountCb) == kOk);
    CHECK(Submit(composite, kCompletionPort, &shared, CountCb) == kOk);
    CHECK(Dispatch(q, kWorkPort, 0) && own.ran.load() == 1);
    CHECK(Dispatch(q, kWorkPort, 0) && shared.ran.load() == 1 && shared.canceled.load() == 0);

    CHECK(Submit(q, kWorkPort, &own, CountCb) == kOk);
    CHECK(Submit(composite, kWorkPort, &shared, CountCb) == kOk);
    CHECK(Terminate(composite, true, nullptr, nullptr) == kOk);
    CHECK(shared.ran.load() == 2 && shared.canceled.load() == 1);
    CHECK(own.ran.load() == 1);
    CHECK(Dispatch(q, kWorkPort, 0) && own.ran.load() == 2 && own.canceled.load() == 0);
    CloseQueue(composite);

#if defined(_WIN32)
    // A waiter submits its callback each time the handle is signaled.
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(event != nullptr);
    Counters waited;
    uint64_t waiter_token = 0;
    CHECK(Call<int32_t, Queue, uint32_t, void*, void*, TaskCallback, uint64_t*>(
              kQueueRegisterWaiter, q, kWorkPort, event, &waited, CountCb, &waiter_token) == kOk);
    CHECK(!Dispatch(q, kWorkPort, 30));
    CHECK(Call<int32_t, Queue, void*, void(SCARLETTX_GDK_CALLBACK*)(void*, void*, uint32_t),
               uint64_t*>(kQueueRegisterMonitor, q, nullptr, MonitorCb, &monitor_token) == kOk);
    const int monitor_calls = g_monitor_calls.load();
    SetEvent(event);
    CHECK(Dispatch(q, kWorkPort, 2000) && waited.ran.load() == 1);
    CHECK(WaitFor([&] { return g_monitor_calls.load() == monitor_calls + 1; }));
    Call<void, Queue, uint64_t>(kQueueUnregisterMonitor, q, monitor_token);
    SetEvent(event);
    CHECK(Dispatch(q, kWorkPort, 2000) && waited.ran.load() == 2);
    Call<void, Queue, uint64_t>(kQueueUnregisterWaiter, q, waiter_token);
    SetEvent(event);
    CHECK(!Dispatch(q, kWorkPort, 50) && waited.ran.load() == 2);
    CloseHandle(event);
#endif
    CloseQueue(q);
    return true;
}

// --- Termination -------------------------------------------------------------

void SCARLETTX_GDK_CALLBACK SlowCb(void* ctx, bool canceled) {
    std::this_thread::yield();
    CountCb(ctx, canceled);
}

struct SelfTerminate {
    Queue queue = nullptr;
    Counters counters;
    std::atomic<int32_t> result{1};
    std::atomic<int> done{0};
    std::atomic<int> go{1};
};
void SCARLETTX_GDK_CALLBACK SelfTerminateCb(void* ctx, bool /*canceled*/) {
    auto* s = static_cast<SelfTerminate*>(ctx);
    while (s->go.load() == 0) {
        std::this_thread::yield();
    }
    s->result.store(Terminate(s->queue, true, &s->counters, TerminatedCb));
    s->done.store(1);
}

// Signals that it started, then spins until released.
void SCARLETTX_GDK_CALLBACK GateCb(void* ctx, bool /*canceled*/) {
    auto* s = static_cast<SelfTerminate*>(ctx);
    s->done.store(1);
    while (s->go.load() == 0) {
        std::this_thread::yield();
    }
}

bool TestTerminate() {
    // Pending and delayed callbacks are delivered as canceled; later submits fail.
    Queue q = nullptr;
    CHECK(CreateQueue(kManual, kManual, &q) == kOk);
    Counters c;
    for (int i = 0; i < 3; ++i) {
        CHECK(Submit(q, i == 2 ? kCompletionPort : kWorkPort, &c, CountCb) == kOk);
    }
    CHECK(SubmitDelayed(q, kWorkPort, 60000, &c, CountCb) == kOk);
    CHECK(Terminate(q, true, &c, TerminatedCb) == kOk);
    CHECK(c.ran.load() == 4 && c.canceled.load() == 4 && c.terminated.load() == 1);
    CHECK(Submit(q, kWorkPort, &c, CountCb) == kEAbort);
    CHECK(Terminate(q, true, &c, TerminatedCb) == kEAbort);
    CHECK(!Dispatch(q, kWorkPort, 0));
    CloseQueue(q);
    CHECK(c.ran.load() == 4 && c.terminated.load() == 1);

    // Stress: terminate while pool threads are busy with the queue.
    for (int i = 0; i < 300; ++i) {
        const bool wait = (i % 2) == 0;
        CHECK(CreateQueue(kThreadPool, (i % 3) == 0 ? kSerialized : kThreadPool, &q) == kOk);
        Counters s;
        for (int j = 0; j < 32; ++j) {
            CHECK(Submit(q, (j % 2) ? kCompletionPort : kWorkPort, &s, SlowCb) == kOk);
        }
        CHECK(Terminate(q, wait, &s, TerminatedCb) == kOk);
        if (wait) {
            CHECK(s.ran.load() == 32 && s.terminated.load() == 1);
        } else {
            CHECK(WaitFor([&] { return s.terminated.load() == 1; }));
            CHECK(s.ran.load() == 32);
        }
        CloseQueue(q);
        CHECK(s.terminated.load() == 1);
    }

    // Terminate(wait = true) from one of the queue's own callbacks must not deadlock.
    for (int i = 0; i < 50; ++i) {
        SelfTerminate st;
        CHECK(CreateQueue(kThreadPool, kThreadPool, &st.queue) == kOk);
        CHECK(Submit(st.queue, kWorkPort, &st, SelfTerminateCb) == kOk);
        CHECK(WaitFor([&] { return st.done.load() == 1; }));
        CHECK(st.result.load() == kOk && st.counters.terminated.load() == 1);
        CloseQueue(st.queue);
    }

    // The same on a serialized port with another callback queued behind the caller:
    // only the calling thread can deliver it.
    for (int i = 0; i < 50; ++i) {
        SelfTerminate st;
        st.go.store(0);
        Counters behind;
        CHECK(CreateQueue(kSerialized, kSerialized, &st.queue) == kOk);
        CHECK(Submit(st.queue, kWorkPort, &st, SelfTerminateCb) == kOk);
        CHECK(Submit(st.queue, kWorkPort, &behind, CountCb) == kOk);
        st.go.store(1);
        CHECK(WaitFor([&] { return st.done.load() == 1; }));
        CHECK(st.result.load() == kOk && st.counters.terminated.load() == 1);
        CHECK(behind.ran.load() == 1 && behind.canceled.load() == 1);
        CloseQueue(st.queue);
    }

    // A dispatcher blocked with an infinite timeout returns when the queue is
    // terminated, even while one of the queue's callbacks is still running.
    {
        CHECK(CreateQueue(kManual, kThreadPool, &q) == kOk);
        SelfTerminate busy;  // used as a gate: its callback spins until go is set
        busy.go.store(0);
        busy.queue = q;
        std::atomic<int> dispatch_result{-1};
        Counters t;
        CHECK(Submit(q, kCompletionPort, &busy, GateCb) == kOk);
        CHECK(WaitFor([&] { return busy.done.load() == 1; }));
        std::thread dispatcher([&] {
            dispatch_result.store(Dispatch(q, kWorkPort, 0xFFFFFFFFu) ? 1 : 0);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(Terminate(q, false, &t, TerminatedCb) == kOk);
        const bool returned = WaitFor([&] { return dispatch_result.load() == 0; }, 5000);
        busy.go.store(1);
        if (!returned) {
            dispatcher.detach();
        } else {
            dispatcher.join();
        }
        CHECK(returned);
        CHECK(WaitFor([&] { return t.terminated.load() == 1; }));
        CloseQueue(q);
    }
    return true;
}

// --- Closing the last handle from the queue's own callback -------------------

struct CloseCtx {
    Queue queue = nullptr;
    std::atomic<int> done{0};
    volatile uint64_t scratch[16] = {};
};
void SCARLETTX_GDK_CALLBACK CloseSelfCb(void* ctx, bool /*canceled*/) {
    auto* c = static_cast<CloseCtx*>(ctx);
    CloseQueue(c->queue);  // last handle: the queue must stay usable until we return
    for (auto& v : c->scratch) {
        v = v + 1;
    }
    c->done.store(1);
}

bool TestCloseFromCallback() {
    for (int i = 0; i < 500; ++i) {
        CloseCtx ctx;
        const uint32_t mode = (i % 2) ? kSerialized : kThreadPool;
        CHECK(CreateQueue(mode, mode, &ctx.queue) == kOk);
        CHECK(Submit(ctx.queue, (i % 4) < 2 ? kWorkPort : kCompletionPort, &ctx, CloseSelfCb) == kOk);
        CHECK(WaitFor([&] { return ctx.done.load() == 1; }));
    }
    // Closing the last handle right after submitting: pool callbacks still run.
    for (int i = 0; i < 200; ++i) {
        Queue q = nullptr;
        CHECK(CreateQueue(kThreadPool, kSerialized, &q) == kOk);
        Counters c;
        for (int j = 0; j < 8; ++j) {
            CHECK(Submit(q, (j % 2) ? kCompletionPort : kWorkPort, &c, SlowCb) == kOk);
        }
        CloseQueue(q);
        CHECK(WaitFor([&] { return c.ran.load() == 8; }));
        CHECK(c.canceled.load() == 0);
    }
    return true;
}

// --- XAsyncProvider lifecycle -------------------------------------------------

struct ProviderData {  // XAsyncProviderData
    XAsyncBlock* async;
    size_t bufferSize;
    void* buffer;
    void* context;
};
using ProviderFn = int32_t(SCARLETTX_GDK_CALLBACK*)(uint32_t, const ProviderData*);

int32_t AsyncBegin(XAsyncBlock* block, void* ctx, const void* identity, ProviderFn provider) {
    return Call<int32_t, XAsyncBlock*, void*, const void*, const char*, ProviderFn>(
        kAsyncBegin, block, ctx, identity, "test", provider);
}
int32_t AsyncSchedule(XAsyncBlock* block, uint32_t delay_ms) {
    return Call<int32_t, XAsyncBlock*, uint32_t>(kAsyncSchedule, block, delay_ms);
}
void AsyncComplete(XAsyncBlock* block, int32_t hr, size_t size) {
    Call<void, XAsyncBlock*, int32_t, size_t>(kAsyncComplete, block, hr, size);
}
int32_t AsyncStatus(XAsyncBlock* block, bool wait) {
    return Call<int32_t, XAsyncBlock*, bool>(kAsyncGetStatus, block, wait);
}
int32_t AsyncGetResult(XAsyncBlock* block, const void* identity, size_t size, void* buffer,
                       size_t* used) {
    return Call<int32_t, XAsyncBlock*, const void*, size_t, void*, size_t*>(
        kAsyncGetResult, block, identity, size, buffer, used);
}

struct Op {
    std::mutex mutex;
    std::vector<uint32_t> ops;
    std::atomic<int> cleanups{0};
    std::atomic<int> callbacks{0};
    int32_t begin_result = kOk;
    int32_t work_result = kEPending;  // E_PENDING: DoWork completes with a payload itself
    uint32_t payload = 0xC0FFEE42u;
    int32_t in_callback_wrong_identity = 1;
    int32_t in_callback_small_buffer = 1;
    int32_t in_callback_result = 1;
    uint32_t fetched = 0;
    size_t used = 0;
    bool fetch_in_callback = false;
    bool complete_in_work = false;  // DoWork completes without payload, then keeps going
    int cleanups_seen_after_complete = -1;

    std::vector<uint32_t> Ops() {
        std::lock_guard<std::mutex> lock(mutex);
        return ops;
    }
};
const char kIdentity[] = "test identity";
const char kOtherIdentity[] = "other identity";

int32_t SCARLETTX_GDK_CALLBACK TestProvider(uint32_t op, const ProviderData* data) {
    auto* o = static_cast<Op*>(data->context);
    {
        std::lock_guard<std::mutex> lock(o->mutex);
        o->ops.push_back(op);
    }
    switch (op) {
        case kOpBegin:
            return o->begin_result;
        case kOpDoWork:
            if (o->complete_in_work) {
                AsyncComplete(data->async, kOk, 0);
                o->cleanups_seen_after_complete = o->cleanups.load();
                return kEPending;
            }
            if (o->work_result == kEPending) {
                AsyncComplete(data->async, kOk, sizeof(uint32_t));
            }
            return o->work_result;
        case kOpGetResult:
            if (data->bufferSize < sizeof(uint32_t)) {
                return kEFail;
            }
            *static_cast<uint32_t*>(data->buffer) = o->payload;
            return kOk;
        case kOpCancel:
            AsyncComplete(data->async, kEAbort, 0);
            return kOk;
        case kOpCleanup:
            o->cleanups.fetch_add(1);
            return kOk;
        default:
            return kEFail;
    }
}

void SCARLETTX_GDK_CALLBACK OpCompletion(XAsyncBlock* block) {
    auto* o = static_cast<Op*>(block->context);
    if (o->fetch_in_callback) {
        uint8_t tiny = 0;
        o->in_callback_wrong_identity =
            AsyncGetResult(block, kOtherIdentity, sizeof(o->fetched), &o->fetched, &o->used);
        o->in_callback_small_buffer = AsyncGetResult(block, kIdentity, sizeof(tiny), &tiny, nullptr);
        o->in_callback_result =
            AsyncGetResult(block, kIdentity, sizeof(o->fetched), &o->fetched, &o->used);
    }
    o->callbacks.fetch_add(1);
}

struct Tally {
    std::atomic<int> completions{0};
    std::atomic<int> cleanups{0};
    std::atomic<int> work{0};
};
int32_t SCARLETTX_GDK_CALLBACK QuickProvider(uint32_t op, const ProviderData* data) {
    if (op == kOpCleanup) {
        static_cast<Tally*>(data->context)->cleanups.fetch_add(1);
    }
    return kOk;  // DoWork returning S_OK completes the call without a payload
}
void SCARLETTX_GDK_CALLBACK FreeBlockCompletion(XAsyncBlock* block) {
    auto* t = static_cast<Tally*>(block->context);
    std::memset(static_cast<void*>(block), 0xDD, sizeof(*block));
    delete block;  // guests do this; the runtime must not touch the block afterwards
    t->completions.fetch_add(1);
}

int32_t SCARLETTX_GDK_CALLBACK RunWork(XAsyncBlock* block) {
    static_cast<Tally*>(block->context)->work.fetch_add(1);
    return kOk;
}

XAsyncBlock MakeBlock(Queue q, void* context, XAsyncCompletionRoutine* callback) {
    XAsyncBlock block{};
    block.queue = static_cast<XTaskQueueHandle>(q);
    block.context = context;
    block.callback = callback;
    return block;
}

bool TestProviderLifecycle() {
    Queue q = nullptr;
    CHECK(CreateQueue(kManual, kManual, &q) == kOk);

    XAsyncBlock never_begun{};
    CHECK(AsyncStatus(&never_begun, false) == kEInvalidArg);

    // Payload: Begin, DoWork, GetResult, then Cleanup exactly once.
    Op op;
    op.fetch_in_callback = true;
    XAsyncBlock block = MakeBlock(q, &op, OpCompletion);
    CHECK(AsyncBegin(&block, &op, kIdentity, TestProvider) == kOk);
    CHECK(AsyncStatus(&block, false) == kEPending);
    CHECK(AsyncSchedule(&block, 0) == kOk);
    CHECK(AsyncSchedule(&block, 0) == kEUnexpected);
    CHECK(Dispatch(q, kWorkPort, 0));
    CHECK(AsyncStatus(&block, false) == kOk);
    size_t size = 0;
    CHECK(Call<int32_t, XAsyncBlock*, size_t*>(kAsyncGetResultSize, &block, &size) == kOk);
    CHECK(size == sizeof(uint32_t));
    CHECK(op.callbacks.load() == 0 && op.cleanups.load() == 0);
    CHECK(Dispatch(q, kCompletionPort, 0));
    CHECK(op.callbacks.load() == 1);
    CHECK(op.in_callback_wrong_identity == kEInvalidArg);
    CHECK(op.in_callback_small_buffer == kEInsufficientBuffer);
    CHECK(op.in_callback_result == kOk && op.fetched == 0xC0FFEE42u && op.used == sizeof(uint32_t));
    CHECK((op.Ops() == std::vector<uint32_t>{kOpBegin, kOpDoWork, kOpGetResult, kOpCleanup}));
    CHECK(op.cleanups.load() == 1);
    CHECK(AsyncStatus(&block, false) == kOk);
    uint32_t again = 0;
    CHECK(AsyncGetResult(&block, kIdentity, sizeof(again), &again, nullptr) == kENotSupported);
    CHECK(!Dispatch(q, kWorkPort, 0) && !Dispatch(q, kCompletionPort, 0));

    // Cancel before the work ran: no DoWork, one Cleanup.
    Op cancel;
    XAsyncBlock cancel_block = MakeBlock(q, &cancel, OpCompletion);
    CHECK(AsyncBegin(&cancel_block, &cancel, kIdentity, TestProvider) == kOk);
    CHECK(AsyncSchedule(&cancel_block, 0) == kOk);
    Call<void, XAsyncBlock*>(kAsyncCancel, &cancel_block);
    CHECK(AsyncStatus(&cancel_block, false) == kEAbort);
    CHECK(Dispatch(q, kWorkPort, 0));
    CHECK(Dispatch(q, kCompletionPort, 0));
    CHECK(cancel.callbacks.load() == 1 && cancel.cleanups.load() == 1);
    CHECK((cancel.Ops() == std::vector<uint32_t>{kOpBegin, kOpCancel, kOpCleanup}));

    // A failing Begin completes the call with that failure.
    Op bad;
    bad.begin_result = kEFail;
    XAsyncBlock bad_block = MakeBlock(q, &bad, nullptr);
    CHECK(AsyncBegin(&bad_block, &bad, kIdentity, TestProvider) == kOk);
    CHECK(AsyncStatus(&bad_block, false) == kEFail);
    CHECK((bad.Ops() == std::vector<uint32_t>{kOpBegin, kOpCleanup}));
    CHECK(AsyncSchedule(&bad_block, 0) == kEInvalidArg);

    // Completing from inside DoWork must not clean up under the provider's feet.
    Op late;
    late.complete_in_work = true;
    XAsyncBlock late_block = MakeBlock(q, &late, nullptr);
    CHECK(AsyncBegin(&late_block, &late, kIdentity, TestProvider) == kOk);
    CHECK(AsyncSchedule(&late_block, 0) == kOk);
    CHECK(Dispatch(q, kWorkPort, 0));
    CHECK(late.cleanups_seen_after_complete == 0 && late.cleanups.load() == 1);
    CHECK(AsyncStatus(&late_block, false) == kOk);

    // XAsyncCancel before the work of an XAsyncRun started: the work never runs.
    Tally canceled_run;
    XAsyncBlock run_block = MakeBlock(q, &canceled_run, nullptr);
    CHECK(Call<int32_t, XAsyncBlock*, int32_t(SCARLETTX_GDK_CALLBACK*)(XAsyncBlock*)>(
              kAsyncRun, &run_block, RunWork) == kOk);
    Call<void, XAsyncBlock*>(kAsyncCancel, &run_block);
    CHECK(AsyncStatus(&run_block, false) == kEAbort);
    CHECK(Dispatch(q, kWorkPort, 0));
    CHECK(canceled_run.work.load() == 0);
    CloseQueue(q);

    // Terminating the queue aborts a call whose work is still queued.
    Queue tq = nullptr;
    CHECK(CreateQueue(kManual, kManual, &tq) == kOk);
    Op aborted;
    XAsyncBlock aborted_block = MakeBlock(tq, &aborted, OpCompletion);
    CHECK(AsyncBegin(&aborted_block, &aborted, kIdentity, TestProvider) == kOk);
    CHECK(AsyncSchedule(&aborted_block, 60000) == kOk);
    CHECK(Terminate(tq, true, nullptr, nullptr) == kOk);
    CHECK(AsyncStatus(&aborted_block, false) == kEAbort);
    CHECK((aborted.Ops() == std::vector<uint32_t>{kOpBegin, kOpCleanup}));
    CloseQueue(tq);

    // No payload: the block is freed inside the completion callback.
    Queue pool = nullptr;
    CHECK(CreateQueue(kThreadPool, kThreadPool, &pool) == kOk);
    Tally tally;
    for (int i = 0; i < 1000; ++i) {
        auto* heap_block = new XAsyncBlock(MakeBlock(pool, &tally, FreeBlockCompletion));
        CHECK(AsyncBegin(heap_block, &tally, kIdentity, QuickProvider) == kOk);
        CHECK(AsyncSchedule(heap_block, 0) == kOk);
    }
    CHECK(WaitFor([&] { return tally.completions.load() == 1000 && tally.cleanups.load() == 1000; }));

    // XAsyncGetStatus(wait = true) blocks until the status is final.
    Op waited;
    waited.work_result = kOk;
    XAsyncBlock waited_block = MakeBlock(pool, &waited, nullptr);
    CHECK(AsyncBegin(&waited_block, &waited, kIdentity, TestProvider) == kOk);
    CHECK(AsyncSchedule(&waited_block, 25) == kOk);
    CHECK(AsyncStatus(&waited_block, true) == kOk);
    CHECK(WaitFor([&] { return waited.cleanups.load() == 1; }));
    CloseQueue(pool);
    return true;
}

// --- XAsyncRun stress ---------------------------------------------------------

bool TestRunStress() {
    Queue q = nullptr;
    CHECK(CreateQueue(kThreadPool, kSerialized, &q) == kOk);
    Tally tally;
    for (int i = 0; i < 2000; ++i) {
        auto* block = new XAsyncBlock(MakeBlock(q, &tally, FreeBlockCompletion));
        CHECK(Call<int32_t, XAsyncBlock*, int32_t(SCARLETTX_GDK_CALLBACK*)(XAsyncBlock*)>(
                  kAsyncRun, block, RunWork) == kOk);
    }
    CHECK(WaitFor([&] { return tally.completions.load() == 2000; }));
    CHECK(tally.work.load() == 2000);

    // Cancel racing the start of the work: either the work ran and the call
    // succeeded, or it was aborted and the work never runs.
    for (int i = 0; i < 3000; ++i) {
        Tally one;
        XAsyncBlock block = MakeBlock(q, &one, nullptr);
        CHECK(Call<int32_t, XAsyncBlock*, int32_t(SCARLETTX_GDK_CALLBACK*)(XAsyncBlock*)>(
                  kAsyncRun, &block, RunWork) == kOk);
        if (i % 3) {
            std::this_thread::yield();
        }
        Call<void, XAsyncBlock*>(kAsyncCancel, &block);
        const int32_t status = AsyncStatus(&block, true);
        CHECK(status == kOk || status == kEAbort);
        CHECK(one.work.load() == (status == kOk ? 1 : 0));
    }
    CHECK(Terminate(q, true, nullptr, nullptr) == kOk);
    CloseQueue(q);
    return true;
}

// --- XTaskQueueCanDispatch & XAsyncBeginAlloc --------------------------------

struct AllocProviderCtx {
    uint32_t magic;
    uint32_t value;
    bool was_zero_init;
};

struct AllocTracker {
    std::atomic<int> begin_calls{0};
    std::atomic<int> work_calls{0};
    std::atomic<int> result_calls{0};
    std::atomic<int> cleanup_calls{0};
    std::atomic<int> zero_init_ok{0};
    std::atomic<int> cleanup_saw_magic{0};
    std::atomic<int> completions{0};
    int32_t fail_begin_hr{kOk};
};

AllocTracker* g_alloc_tracker = nullptr;
const char kAllocIdentity[] = "alloc_provider";

XGameResult SCARLETTX_GDK_CALLBACK AllocProvider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<AllocProviderCtx*>(data->context);
    AllocTracker* tr = g_alloc_tracker;
    switch (op) {
        case XAsyncOp_Begin: {
            tr->begin_calls.fetch_add(1);
            if (ctx) {
                const bool zeroed = (ctx->magic == 0 && ctx->value == 0 && !ctx->was_zero_init);
                ctx->was_zero_init = zeroed;
                ctx->magic = 0xA110C555u;
                ctx->value = 0x5CA91E77u;
                if (zeroed) {
                    tr->zero_init_ok.fetch_add(1);
                }
            }
            if (tr->fail_begin_hr < 0) {
                return tr->fail_begin_hr;
            }
            return XAsyncSchedule(data->async, 0);
        }
        case XAsyncOp_DoWork:
            tr->work_calls.fetch_add(1);
            if (ctx) {
                ctx->value ^= 0x00FF00FFu;
                XAsyncComplete(data->async, XGR_S_OK, sizeof(uint32_t));
                return XGR_E_PENDING;
            }
            return XGR_S_OK;
        case XAsyncOp_GetResult:
            tr->result_calls.fetch_add(1);
            if (!ctx || data->bufferSize < sizeof(uint32_t)) {
                return XGR_E_FAIL;
            }
            *static_cast<uint32_t*>(data->buffer) = ctx->value;
            return XGR_S_OK;
        case XAsyncOp_Cancel:
            XAsyncComplete(data->async, XGR_E_ABORT, 0);
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            tr->cleanup_calls.fetch_add(1);
            if (ctx && ctx->magic == 0xA110C555u) {
                tr->cleanup_saw_magic.fetch_add(1);
            }
            return XGR_S_OK;
        default:
            return XGR_E_FAIL;
    }
}

void SCARLETTX_GDK_CALLBACK AllocCompletion(XAsyncBlock* block) {
    static_cast<AllocTracker*>(block->context)->completions.fetch_add(1);
}

bool TestCanDispatchAndBeginAlloc() {
    // 1. XTaskQueueCanDispatch on manual, pool, immediate, delayed, and composite queues.
    CHECK(!XTaskQueueCanDispatch(nullptr, XTaskQueuePort_Work));

    XTaskQueueHandle manual = nullptr;
    CHECK(XTaskQueueCreate(XTaskQueueDispatchMode_Manual, XTaskQueueDispatchMode_Manual, &manual) ==
          XGR_S_OK);
    CHECK(!XTaskQueueCanDispatch(manual, XTaskQueuePort_Work));
    CHECK(!XTaskQueueCanDispatch(manual, XTaskQueuePort_Completion));
    CHECK(!XTaskQueueCanDispatch(manual, static_cast<XTaskQueuePort>(2)));

    Counters cnt;
    CHECK(XTaskQueueSubmitCallback(manual, XTaskQueuePort_Work, &cnt, CountCb) == XGR_S_OK);
    CHECK(XTaskQueueCanDispatch(manual, XTaskQueuePort_Work));
    CHECK(!XTaskQueueCanDispatch(manual, XTaskQueuePort_Completion));
    CHECK(XTaskQueueDispatch(manual, XTaskQueuePort_Work, 0));
    CHECK(cnt.ran.load() == 1);
    CHECK(!XTaskQueueCanDispatch(manual, XTaskQueuePort_Work));

    // Composite queue sharing the same manual port observes readiness.
    XTaskQueuePortHandle work_port = nullptr;
    CHECK(XTaskQueueGetPort(manual, XTaskQueuePort_Work, &work_port) == XGR_S_OK);
    XTaskQueueHandle comp = nullptr;
    CHECK(XTaskQueueCreateComposite(work_port, work_port, &comp) == XGR_S_OK);
    CHECK(!XTaskQueueCanDispatch(comp, XTaskQueuePort_Completion));
    CHECK(XTaskQueueSubmitCallback(comp, XTaskQueuePort_Completion, &cnt, CountCb) == XGR_S_OK);
    CHECK(XTaskQueueCanDispatch(manual, XTaskQueuePort_Work));
    CHECK(XTaskQueueCanDispatch(comp, XTaskQueuePort_Completion));
    CHECK(XTaskQueueDispatch(comp, XTaskQueuePort_Completion, 0));
    CHECK(cnt.ran.load() == 2);
    CHECK(!XTaskQueueCanDispatch(manual, XTaskQueuePort_Work));
    XTaskQueueCloseHandle(comp);

    // Delayed callback becomes dispatchable only after delay elapses.
    CHECK(XTaskQueueSubmitDelayedCallback(manual, XTaskQueuePort_Work, 30, &cnt, CountCb) ==
          XGR_S_OK);
    CHECK(!XTaskQueueCanDispatch(manual, XTaskQueuePort_Work));
    CHECK(WaitFor([&] { return XTaskQueueCanDispatch(manual, XTaskQueuePort_Work); }));
    CHECK(XTaskQueueDispatch(manual, XTaskQueuePort_Work, 0));
    CHECK(cnt.ran.load() == 3);
    CHECK(!XTaskQueueCanDispatch(manual, XTaskQueuePort_Work));

    // Non-manual ports are never manually dispatchable.
    XTaskQueueHandle pool = nullptr;
    CHECK(XTaskQueueCreate(XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_Immediate,
                           &pool) == XGR_S_OK);
    CHECK(!XTaskQueueCanDispatch(pool, XTaskQueuePort_Work));
    CHECK(!XTaskQueueCanDispatch(pool, XTaskQueuePort_Completion));
    XTaskQueueCloseHandle(pool);

    // 2. XAsyncBeginAlloc lifecycle with zero-initialized context + payload + automatic free.
    AllocTracker tracker;
    g_alloc_tracker = &tracker;

    XAsyncBlock invalid_block{};
    CHECK(XAsyncBeginAlloc(nullptr, kAllocIdentity, "alloc", AllocProvider,
                           sizeof(AllocProviderCtx)) == XGR_E_INVALIDARG);
    CHECK(XAsyncBeginAlloc(&invalid_block, kAllocIdentity, "alloc", nullptr,
                           sizeof(AllocProviderCtx)) == XGR_E_INVALIDARG);

    XAsyncBlock block{};
    block.queue = manual;
    block.context = &tracker;
    block.callback = AllocCompletion;
    CHECK(XAsyncBeginAlloc(&block, kAllocIdentity, "alloc", AllocProvider,
                           sizeof(AllocProviderCtx)) == XGR_S_OK);
    CHECK(tracker.begin_calls.load() == 1 && tracker.zero_init_ok.load() == 1);
    CHECK(XTaskQueueCanDispatch(manual, XTaskQueuePort_Work));
    CHECK(XTaskQueueDispatch(manual, XTaskQueuePort_Work, 0));
    CHECK(tracker.work_calls.load() == 1);
    CHECK(XAsyncGetStatus(&block, false) == XGR_S_OK);

    size_t res_size = 0;
    CHECK(XAsyncGetResultSize(&block, &res_size) == XGR_S_OK && res_size == sizeof(uint32_t));
    CHECK(XTaskQueueCanDispatch(manual, XTaskQueuePort_Completion));
    CHECK(XTaskQueueDispatch(manual, XTaskQueuePort_Completion, 0));
    CHECK(tracker.completions.load() == 1);

    uint32_t out_val = 0;
    size_t used = 0;
    CHECK(XAsyncGetResult(&block, kAllocIdentity, sizeof(out_val), &out_val, &used) == XGR_S_OK);
    CHECK(used == sizeof(uint32_t) && out_val == (0x5CA91E77u ^ 0x00FF00FFu));
    CHECK(tracker.cleanup_calls.load() == 1 && tracker.cleanup_saw_magic.load() == 1);

    // Failing Begin still runs Cleanup with valid allocated context and sets error status.
    tracker.fail_begin_hr = XGR_E_FAIL;
    XAsyncBlock fail_block{};
    fail_block.queue = manual;
    CHECK(XAsyncBeginAlloc(&fail_block, kAllocIdentity, "alloc_fail", AllocProvider,
                           sizeof(AllocProviderCtx)) == XGR_S_OK);
    CHECK(XAsyncGetStatus(&fail_block, false) == XGR_E_FAIL);
    CHECK(tracker.cleanup_calls.load() == 2 && tracker.cleanup_saw_magic.load() == 2);

    // Zero-size allocation passes null context and completes cleanly without payload.
    tracker.fail_begin_hr = kOk;
    XAsyncBlock zero_block{};
    zero_block.queue = manual;
    CHECK(XAsyncBeginAlloc(&zero_block, kAllocIdentity, "alloc_zero", AllocProvider, 0) ==
          XGR_S_OK);
    CHECK(XTaskQueueDispatch(manual, XTaskQueuePort_Work, 0));
    CHECK(XAsyncGetStatus(&zero_block, false) == XGR_S_OK);
    CHECK(tracker.cleanup_calls.load() == 3);

    g_alloc_tracker = nullptr;
    XTaskQueueCloseHandle(manual);
    return true;
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        bool (*fn)();
    };
    const Test tests[] = {
        {"dispatch modes", TestDispatchModes},
        {"process queue", TestProcessQueue},
        {"delayed/composite/waiter", TestDelayedCompositeWaiter},
        {"terminate", TestTerminate},
        {"close from callback", TestCloseFromCallback},
        {"provider lifecycle", TestProviderLifecycle},
        {"XAsyncRun stress", TestRunStress},
        {"CanDispatch & BeginAlloc", TestCanDispatchAndBeginAlloc},
    };
    // Two rounds: the second one runs after a full runtime shutdown and restart.
    for (int round = 0; round < 2; ++round) {
        if (InitializeApiImplEx(19041, 10002, 0) != XGR_S_OK ||
            QueryApiImpl(&kGuid073B7DCB, &kGuid073B7DCB, &g_api) != XGR_S_OK || !g_api) {
            std::fprintf(stderr, "[test_xtaskqueue_xasync] FAILED: runtime init\n");
            return 1;
        }
        g_monitor_calls.store(0);
        for (const auto& t : tests) {
            if (!t.fn()) {
                std::fprintf(stderr, "[test_xtaskqueue_xasync] FAILED in '%s' (round %d)\n",
                             t.name, round);
                return 1;
            }
        }
        Call<uint32_t>(2);  // Release
        g_api = nullptr;
        UninitializeApiImpl();
    }
    std::printf("[test_xtaskqueue_xasync] PASSED\n");
    return 0;
}
