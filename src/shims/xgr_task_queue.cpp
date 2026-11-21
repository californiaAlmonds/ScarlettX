// Self-contained implementation of the XTaskQueue / XAsync / XAsyncProvider
// contracts described by the public GDK headers. No third-party code.
//
// Locking: one engine mutex guards all queue, port, timer and waiter state.
// Guest callbacks are always invoked with that mutex released.

#include "xgr_task_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <new>
#include <thread>
#include <unordered_set>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kQueueSignature = 0x51545358;  // "XSTQ"
constexpr uint32_t kAsyncSignature = 0x41535358;  // "XSSA"
constexpr uint32_t kInfiniteTimeout = 0xFFFFFFFFu;

struct QueueCore;

struct Item {
    std::shared_ptr<QueueCore> owner;
    XTaskQueueCallback* callback = nullptr;
    void* context = nullptr;
    uint64_t waiter_id = 0;  // set when a waiter produced the item
};

struct Port {
    uint32_t mode = XTaskQueueDispatchMode_Manual;
    std::deque<Item> ready;
    std::condition_variable cv;  // Manual ports: wakes XTaskQueueDispatch
    bool serial_busy = false;    // SerializedThreadPool: an item is running or posted
    std::thread::id serial_thread;  // thread running the port's current item, if any
};

struct Monitor {
    uint64_t token = 0;
    XTaskQueueMonitorCallback* callback = nullptr;
    void* context = nullptr;
};

struct TerminationCallback {
    XTaskQueueTerminatedCallback* callback = nullptr;
    void* context = nullptr;
};

enum class QueueState { Active, Terminating, Terminated };

struct QueueCore {
    std::shared_ptr<Port> ports[2];
    QueueState state = QueueState::Active;
    uint32_t pending = 0;  // queued items (ready or delayed) owned by this queue
    uint32_t active = 0;   // callbacks of this queue that are executing
    std::vector<Monitor> monitors;
    std::vector<TerminationCallback> termination_callbacks;
    std::condition_variable idle_cv;
    bool finalizing = false;  // termination callbacks are being invoked
    XTaskQueueObject* handle = nullptr;  // null once the last handle is closed
};

}  // namespace

struct XTaskQueuePortObject {
    XTaskQueueObject* queue = nullptr;
    uint32_t index = 0;
};

struct XTaskQueueObject {
    uint32_t signature = kQueueSignature;
    uint32_t handle_refs = 1;
    std::shared_ptr<QueueCore> core;
    XTaskQueuePortObject port_handles[2];
};

namespace {

struct DelayedItem {
    std::shared_ptr<Port> port;
    uint32_t port_index = 0;
    Item item;
};

struct Waiter {
    uint64_t id = 0;
    std::shared_ptr<QueueCore> owner;
    std::shared_ptr<Port> port;
    uint32_t port_index = 0;
    void* handle = nullptr;
    XTaskQueueCallback* callback = nullptr;
    void* context = nullptr;
    bool armed = true;
};

struct AsyncState;

// Heap-allocated and never destroyed: its threads may outlive static destruction.
struct Engine {
    std::mutex mutex;
    std::unordered_set<XTaskQueueObject*> handles;
    std::unordered_set<XTaskQueuePortObject*> port_handles;
    uint64_t next_token = 1;

    std::deque<std::shared_ptr<Port>> pool_jobs;
    std::condition_variable pool_cv;
    std::vector<std::thread> pool_threads;
    uint32_t pool_idle = 0;

    std::multimap<Clock::time_point, DelayedItem> timers;
    std::condition_variable timer_cv;
    std::thread timer_thread;

    std::vector<Waiter> waiters;
    std::thread wait_thread;
    void* wait_wake = nullptr;

    bool stopping = false;

    XTaskQueueObject* process_queue = nullptr;
    bool process_queue_cleared = false;

    std::mutex block_locks[64];
};

Engine& GetEngine() {
    static Engine* engine = new Engine();
    return *engine;
}

using Lock = std::unique_lock<std::mutex>;

thread_local std::vector<QueueCore*> t_running;

uint32_t RunningOnThisThread(const QueueCore* q) {
    return static_cast<uint32_t>(std::count(t_running.begin(), t_running.end(), q));
}

void PoolWorker();
void TimerLoop();
void WaitLoop();

uint32_t PoolLimit() {
    const uint32_t n = std::thread::hardware_concurrency();
    return std::min<uint32_t>(std::max<uint32_t>(n, 2), 8);
}

void PostPoolJobLocked(Engine& e, const std::shared_ptr<Port>& port) {
    e.pool_jobs.push_back(port);
    if (e.pool_idle == 0 && e.pool_threads.size() < PoolLimit()) {
        e.pool_threads.emplace_back(PoolWorker);
    }
    e.pool_cv.notify_one();
}

void EnqueueReadyLocked(Engine& e, const std::shared_ptr<Port>& port, Item item) {
    port->ready.push_back(std::move(item));
    switch (port->mode) {
        case XTaskQueueDispatchMode_Manual:
            port->cv.notify_one();
            break;
        case XTaskQueueDispatchMode_SerializedThreadPool:
            if (!port->serial_busy) {
                port->serial_busy = true;
                PostPoolJobLocked(e, port);
            }
            break;
        default:  // ThreadPool, and delayed/canceled items of Immediate ports
            PostPoolJobLocked(e, port);
            break;
    }
}

void NotifyMonitors(Lock& lk, const std::shared_ptr<QueueCore>& core, uint32_t port_index) {
    if (core->monitors.empty() || !core->handle) {
        return;
    }
    const std::vector<Monitor> monitors = core->monitors;
    XTaskQueueObject* handle = core->handle;
    lk.unlock();
    for (const auto& m : monitors) {
        m.callback(m.context, handle, static_cast<XTaskQueuePort>(port_index));
    }
    lk.lock();
}

// Completes termination once nothing of the queue is queued or running.
// `own` is the number of this queue's callbacks executing on the calling thread.
void FinishTerminationLocked(Lock& lk, const std::shared_ptr<QueueCore>& core, uint32_t own) {
    if (core->state != QueueState::Terminating || core->pending != 0 || core->active > own) {
        return;
    }
    core->state = QueueState::Terminated;
    core->finalizing = true;
    std::vector<TerminationCallback> callbacks;
    callbacks.swap(core->termination_callbacks);
    lk.unlock();
    for (const auto& c : callbacks) {
        c.callback(c.context);
    }
    lk.lock();
    core->finalizing = false;
    core->idle_cv.notify_all();
}

void RearmWaiterLocked(Engine& e, uint64_t id) {
    for (auto& w : e.waiters) {
        if (w.id == id) {
            w.armed = true;
#if defined(_WIN32)
            if (e.wait_wake) {
                SetEvent(static_cast<HANDLE>(e.wait_wake));
            }
#endif
            return;
        }
    }
}

// Runs `item` (already removed from its port). Lock is held on entry and exit.
void RunItemLocked(Engine& e, Lock& lk, Item item) {
    const std::shared_ptr<QueueCore> owner = item.owner;
    --owner->pending;
    ++owner->active;
    const bool canceled = owner->state != QueueState::Active;
    lk.unlock();
    t_running.push_back(owner.get());
    item.callback(item.context, canceled);
    t_running.pop_back();
    lk.lock();
    --owner->active;
    if (item.waiter_id != 0) {
        RearmWaiterLocked(e, item.waiter_id);
    }
    if (owner->state == QueueState::Terminating) {
        owner->idle_cv.notify_all();
        FinishTerminationLocked(lk, owner, 0);
    }
}

void PoolRunOneLocked(Engine& e, Lock& lk, const std::shared_ptr<Port>& port) {
    const bool serialized = port->mode == XTaskQueueDispatchMode_SerializedThreadPool;
    if (port->ready.empty()) {
        if (serialized) {
            port->serial_busy = false;
        }
        return;
    }
    Item item = std::move(port->ready.front());
    port->ready.pop_front();
    port->serial_thread = serialized ? std::this_thread::get_id() : std::thread::id();
    RunItemLocked(e, lk, std::move(item));
    port->serial_thread = std::thread::id();
    if (serialized) {
        if (port->ready.empty()) {
            port->serial_busy = false;
        } else {
            PostPoolJobLocked(e, port);
        }
    }
}

// Runs one queued item of `port` that belongs to `core` on the calling thread.
bool RunOwnedLocked(Engine& e, Lock& lk, const std::shared_ptr<Port>& port,
                    const std::shared_ptr<QueueCore>& core) {
    const bool serialized = port->mode == XTaskQueueDispatchMode_SerializedThreadPool;
    // A busy serialized port is left to the pool, unless the callback it is running
    // is the one we are being called from: then only this thread can make progress.
    const bool reentrant =
        serialized && port->serial_busy && port->serial_thread == std::this_thread::get_id();
    if (serialized && port->serial_busy && !reentrant) {
        return false;
    }
    auto it = std::find_if(port->ready.begin(), port->ready.end(),
                           [&](const Item& i) { return i.owner == core; });
    if (it == port->ready.end()) {
        return false;
    }
    Item item = std::move(*it);
    port->ready.erase(it);
    if (reentrant) {
        RunItemLocked(e, lk, std::move(item));  // the outer callback keeps the port
        return true;
    }
    if (serialized) {
        port->serial_busy = true;
        port->serial_thread = std::this_thread::get_id();
    }
    RunItemLocked(e, lk, std::move(item));
    if (serialized) {
        port->serial_thread = std::thread::id();
        if (port->ready.empty()) {
            port->serial_busy = false;
        } else {
            PostPoolJobLocked(e, port);
        }
    }
    return true;
}

void PoolWorker() {
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    for (;;) {
        ++e.pool_idle;
        e.pool_cv.wait(lk, [&] { return e.stopping || !e.pool_jobs.empty(); });
        --e.pool_idle;
        if (e.stopping) {
            return;
        }
        std::shared_ptr<Port> port = std::move(e.pool_jobs.front());
        e.pool_jobs.pop_front();
        PoolRunOneLocked(e, lk, port);
    }
}

void TimerLoop() {
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    while (!e.stopping) {
        if (e.timers.empty()) {
            e.timer_cv.wait(lk);
            continue;
        }
        const auto due = e.timers.begin()->first;
        if (Clock::now() < due) {
            e.timer_cv.wait_until(lk, due);
            continue;
        }
        DelayedItem d = std::move(e.timers.begin()->second);
        e.timers.erase(e.timers.begin());
        const std::shared_ptr<QueueCore> owner = d.item.owner;
        EnqueueReadyLocked(e, d.port, std::move(d.item));
        NotifyMonitors(lk, owner, d.port_index);
    }
}

#if defined(_WIN32)
void WaitLoop() {
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    while (!e.stopping) {
        HANDLE handles[MAXIMUM_WAIT_OBJECTS];
        uint64_t ids[MAXIMUM_WAIT_OBJECTS];
        DWORD count = 0;
        handles[count] = static_cast<HANDLE>(e.wait_wake);
        ids[count++] = 0;
        for (const auto& w : e.waiters) {
            if (w.armed && count < MAXIMUM_WAIT_OBJECTS) {
                handles[count] = static_cast<HANDLE>(w.handle);
                ids[count++] = w.id;
            }
        }
        lk.unlock();
        const DWORD r = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
        lk.lock();
        if (r == WAIT_FAILED) {
            // A registered handle was closed; drop the waiters that no longer wait.
            e.waiters.erase(
                std::remove_if(e.waiters.begin(), e.waiters.end(), [](const Waiter& w) {
                    return WaitForSingleObject(static_cast<HANDLE>(w.handle), 0) == WAIT_FAILED;
                }),
                e.waiters.end());
            continue;
        }
        const DWORD index = r - WAIT_OBJECT_0;
        if (index == 0 || index >= count) {
            continue;
        }
        for (auto& w : e.waiters) {
            if (w.id != ids[index] || !w.armed) {
                continue;
            }
            w.armed = false;
            if (w.owner->state == QueueState::Active) {
                const std::shared_ptr<QueueCore> owner = w.owner;
                const std::shared_ptr<Port> port = w.port;
                const uint32_t port_index = w.port_index;
                ++w.owner->pending;
                Item item;
                item.owner = w.owner;
                item.callback = w.callback;
                item.context = w.context;
                item.waiter_id = w.id;
                EnqueueReadyLocked(e, port, std::move(item));
                NotifyMonitors(lk, owner, port_index);  // unlocks: `w` is stale after this
            }
            break;
        }
    }
}
#endif

std::shared_ptr<QueueCore> LookupLocked(Engine& e, XTaskQueueHandle queue) {
    if (!queue || e.handles.find(queue) == e.handles.end()) {
        return nullptr;
    }
    return queue->core;
}

XTaskQueueObject* NewHandleLocked(Engine& e, std::shared_ptr<QueueCore> core) {
    auto* handle = new XTaskQueueObject();
    handle->core = std::move(core);
    handle->core->handle = handle;
    for (uint32_t i = 0; i < 2; ++i) {
        handle->port_handles[i].queue = handle;
        handle->port_handles[i].index = i;
        e.port_handles.insert(&handle->port_handles[i]);
    }
    e.handles.insert(handle);
    return handle;
}

XTaskQueueObject* EnsureProcessQueueLocked(Engine& e) {
    if (!e.process_queue && !e.process_queue_cleared) {
        auto core = std::make_shared<QueueCore>();
        for (auto& port : core->ports) {
            port = std::make_shared<Port>();
            port->mode = XTaskQueueDispatchMode_ThreadPool;
        }
        e.process_queue = NewHandleLocked(e, std::move(core));
    }
    return e.process_queue;
}

void RemoveWaitersLocked(Engine& e, const QueueCore* core) {
    e.waiters.erase(std::remove_if(e.waiters.begin(), e.waiters.end(),
                                   [&](const Waiter& w) { return w.owner.get() == core; }),
                    e.waiters.end());
}

XGameResult SubmitInternal(XTaskQueueHandle queue, uint32_t port_index, uint32_t delay_ms,
                           void* context, XTaskQueueCallback* callback) {
    if (!callback || port_index > 1) {
        return XGR_E_INVALIDARG;
    }
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    const std::shared_ptr<QueueCore> core = LookupLocked(e, queue);
    if (!core) {
        return XGR_E_INVALIDARG;
    }
    if (core->state != QueueState::Active) {
        return XGR_E_ABORT;
    }
    const std::shared_ptr<Port> port = core->ports[port_index];
    Item item;
    item.owner = core;
    item.callback = callback;
    item.context = context;

    if (delay_ms == 0 && port->mode == XTaskQueueDispatchMode_Immediate) {
        NotifyMonitors(lk, core, port_index);
        ++core->pending;
        RunItemLocked(e, lk, std::move(item));
        return XGR_S_OK;
    }

    ++core->pending;
    if (delay_ms != 0) {
        DelayedItem d;
        d.port = port;
        d.port_index = port_index;
        d.item = std::move(item);
        e.timers.emplace(Clock::now() + std::chrono::milliseconds(delay_ms), std::move(d));
        if (!e.timer_thread.joinable()) {
            e.timer_thread = std::thread(TimerLoop);
        }
        e.timer_cv.notify_one();
        return XGR_S_OK;
    }
    EnqueueReadyLocked(e, port, std::move(item));
    NotifyMonitors(lk, core, port_index);
    return XGR_S_OK;
}

}  // namespace

extern "C" {

XGameResult XTaskQueueCreate(XTaskQueueDispatchMode workDispatchMode,
                             XTaskQueueDispatchMode completionDispatchMode,
                             XTaskQueueHandle* queue) {
    if (!queue) {
        return XGR_E_POINTER;
    }
    *queue = nullptr;
    const uint32_t modes[2] = {static_cast<uint32_t>(workDispatchMode),
                               static_cast<uint32_t>(completionDispatchMode)};
    if (modes[0] > XTaskQueueDispatchMode_Immediate || modes[1] > XTaskQueueDispatchMode_Immediate) {
        return XGR_E_INVALIDARG;
    }
    auto core = std::make_shared<QueueCore>();
    for (uint32_t i = 0; i < 2; ++i) {
        core->ports[i] = std::make_shared<Port>();
        core->ports[i]->mode = modes[i];
    }
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    *queue = NewHandleLocked(e, std::move(core));
    return XGR_S_OK;
}

XGameResult XTaskQueueCreateComposite(XTaskQueuePortHandle workPort,
                                      XTaskQueuePortHandle completionPort,
                                      XTaskQueueHandle* queue) {
    if (!queue) {
        return XGR_E_POINTER;
    }
    *queue = nullptr;
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    if (e.port_handles.find(workPort) == e.port_handles.end() ||
        e.port_handles.find(completionPort) == e.port_handles.end()) {
        return XGR_E_INVALIDARG;
    }
    auto core = std::make_shared<QueueCore>();
    core->ports[0] = workPort->queue->core->ports[workPort->index];
    core->ports[1] = completionPort->queue->core->ports[completionPort->index];
    *queue = NewHandleLocked(e, std::move(core));
    return XGR_S_OK;
}

XGameResult XTaskQueueGetPort(XTaskQueueHandle queue, XTaskQueuePort port,
                              XTaskQueuePortHandle* portHandle) {
    if (!portHandle) {
        return XGR_E_POINTER;
    }
    *portHandle = nullptr;
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    if (!LookupLocked(e, queue) || static_cast<uint32_t>(port) > 1) {
        return XGR_E_INVALIDARG;
    }
    *portHandle = &queue->port_handles[static_cast<uint32_t>(port)];
    return XGR_S_OK;
}

XGameResult XTaskQueueDuplicateHandle(XTaskQueueHandle queueHandle,
                                      XTaskQueueHandle* duplicatedHandle) {
    if (!duplicatedHandle) {
        return XGR_E_POINTER;
    }
    *duplicatedHandle = nullptr;
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    if (!LookupLocked(e, queueHandle)) {
        return XGR_E_INVALIDARG;
    }
    ++queueHandle->handle_refs;
    *duplicatedHandle = queueHandle;
    return XGR_S_OK;
}

void XTaskQueueCloseHandle(XTaskQueueHandle queue) {
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    const std::shared_ptr<QueueCore> core = LookupLocked(e, queue);
    if (!core || --queue->handle_refs != 0) {
        return;
    }
    e.handles.erase(queue);
    e.port_handles.erase(&queue->port_handles[0]);
    e.port_handles.erase(&queue->port_handles[1]);
    core->handle = nullptr;
    core->monitors.clear();
    RemoveWaitersLocked(e, core.get());

    // Nothing can dispatch a manual port of a queue without handles: drop what is
    // still queued there. Items on thread-pool ports keep the core alive and run.
    for (const auto& port : core->ports) {
        if (port->mode != XTaskQueueDispatchMode_Manual) {
            continue;
        }
        for (auto it = port->ready.begin(); it != port->ready.end();) {
            if (it->owner == core) {
                it = port->ready.erase(it);
                --core->pending;
            } else {
                ++it;
            }
        }
        for (auto it = e.timers.begin(); it != e.timers.end();) {
            if (it->second.item.owner == core && it->second.port == port) {
                it = e.timers.erase(it);
                --core->pending;
            } else {
                ++it;
            }
        }
    }
    FinishTerminationLocked(lk, core, RunningOnThisThread(core.get()));
    lk.unlock();
    delete queue;
}

XGameResult XTaskQueueSubmitCallback(XTaskQueueHandle queue, XTaskQueuePort port,
                                     void* callbackContext, XTaskQueueCallback* callback) {
    return SubmitInternal(queue, static_cast<uint32_t>(port), 0, callbackContext, callback);
}

XGameResult XTaskQueueSubmitDelayedCallback(XTaskQueueHandle queue, XTaskQueuePort port,
                                            uint32_t delayMs, void* callbackContext,
                                            XTaskQueueCallback* callback) {
    return SubmitInternal(queue, static_cast<uint32_t>(port), delayMs, callbackContext, callback);
}

bool XTaskQueueDispatch(XTaskQueueHandle queue, XTaskQueuePort port, uint32_t timeoutInMs) {
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    const std::shared_ptr<QueueCore> core = LookupLocked(e, queue);
    if (!core || static_cast<uint32_t>(port) > 1) {
        return false;
    }
    const std::shared_ptr<Port> p = core->ports[static_cast<uint32_t>(port)];
    if (p->mode != XTaskQueueDispatchMode_Manual) {
        return false;  // the engine dispatches the other modes itself
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutInMs);
    while (p->ready.empty()) {
        // A terminating queue accepts nothing new and its delayed items are already
        // ready, so an empty port stays empty.
        if (timeoutInMs == 0 || e.stopping || core->state != QueueState::Active) {
            return false;
        }
        if (timeoutInMs == kInfiniteTimeout) {
            p->cv.wait(lk);
        } else if (p->cv.wait_until(lk, deadline) == std::cv_status::timeout && p->ready.empty()) {
            return false;
        }
    }
    Item item = std::move(p->ready.front());
    p->ready.pop_front();
    RunItemLocked(e, lk, std::move(item));
    return true;
}

XGameResult XTaskQueueTerminate(XTaskQueueHandle queue, bool wait, void* callbackContext,
                                XTaskQueueTerminatedCallback* callback) {
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    const std::shared_ptr<QueueCore> core = LookupLocked(e, queue);
    if (!core) {
        return XGR_E_INVALIDARG;
    }
    if (core->state != QueueState::Active) {
        return XGR_E_ABORT;
    }
    core->state = QueueState::Terminating;
    if (callback) {
        core->termination_callbacks.push_back({callback, callbackContext});
    }
    RemoveWaitersLocked(e, core.get());

    // Delayed items become ready now; every queued item is delivered as canceled.
    for (auto it = e.timers.begin(); it != e.timers.end();) {
        if (it->second.item.owner == core) {
            DelayedItem d = std::move(it->second);
            it = e.timers.erase(it);
            EnqueueReadyLocked(e, d.port, std::move(d.item));
        } else {
            ++it;
        }
    }
    for (const auto& port : core->ports) {
        port->cv.notify_all();
    }

    const uint32_t own = RunningOnThisThread(core.get());
    if (wait) {
        // The caller delivers the queue's canceled items itself, so a manual queue
        // can be terminated from the thread that normally pumps it.
        while (core->state == QueueState::Terminating &&
               (core->pending != 0 || core->active > own)) {
            bool ran = false;
            for (const auto& port : core->ports) {
                while (RunOwnedLocked(e, lk, port, core)) {
                    ran = true;
                }
            }
            if (!ran && (core->pending != 0 || core->active > own)) {
                core->idle_cv.wait_for(lk, std::chrono::milliseconds(20));
            }
        }
    }
    FinishTerminationLocked(lk, core, own);
    if (wait) {
        // Another thread may have completed the termination; let its callbacks finish.
        core->idle_cv.wait(lk, [&] { return !core->finalizing; });
    }
    return XGR_S_OK;
}

bool XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle* queue) {
    if (!queue) {
        return false;
    }
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    XTaskQueueObject* q = EnsureProcessQueueLocked(e);
    if (!q) {
        *queue = nullptr;
        return false;
    }
    ++q->handle_refs;
    *queue = q;
    return true;
}

void XTaskQueueSetCurrentProcessTaskQueue(XTaskQueueHandle queue) {
    Engine& e = GetEngine();
    XTaskQueueObject* old = nullptr;
    {
        Lock lk(e.mutex);
        old = e.process_queue;
        if (LookupLocked(e, queue)) {
            ++queue->handle_refs;
            e.process_queue = queue;
            e.process_queue_cleared = false;
        } else {
            e.process_queue = nullptr;
            e.process_queue_cleared = true;
        }
    }
    if (old) {
        XTaskQueueCloseHandle(old);
    }
}

XGameResult XTaskQueueRegisterMonitor(XTaskQueueHandle queue, void* callbackContext,
                                      XTaskQueueMonitorCallback* callback,
                                      XTaskQueueRegistrationToken* token) {
    if (!callback || !token) {
        return XGR_E_INVALIDARG;
    }
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    const std::shared_ptr<QueueCore> core = LookupLocked(e, queue);
    if (!core) {
        return XGR_E_INVALIDARG;
    }
    token->token = e.next_token++;
    core->monitors.push_back({token->token, callback, callbackContext});
    return XGR_S_OK;
}

void XTaskQueueUnregisterMonitor(XTaskQueueHandle queue, XTaskQueueRegistrationToken token) {
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    const std::shared_ptr<QueueCore> core = LookupLocked(e, queue);
    if (!core) {
        return;
    }
    core->monitors.erase(std::remove_if(core->monitors.begin(), core->monitors.end(),
                                        [&](const Monitor& m) { return m.token == token.token; }),
                         core->monitors.end());
}

XGameResult XTaskQueueRegisterWaiter(XTaskQueueHandle queue, XTaskQueuePort port,
                                     void* waitHandle, void* callbackContext,
                                     XTaskQueueCallback* callback,
                                     XTaskQueueRegistrationToken* token) {
#if defined(_WIN32)
    if (!waitHandle || !callback || !token || static_cast<uint32_t>(port) > 1) {
        return XGR_E_INVALIDARG;
    }
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    const std::shared_ptr<QueueCore> core = LookupLocked(e, queue);
    if (!core) {
        return XGR_E_INVALIDARG;
    }
    if (core->state != QueueState::Active) {
        return XGR_E_ABORT;
    }
    if (e.waiters.size() >= MAXIMUM_WAIT_OBJECTS - 1) {
        return XGR_E_OUTOFMEMORY;
    }
    if (!e.wait_wake) {
        e.wait_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!e.wait_wake) {
            return XGR_E_OUTOFMEMORY;
        }
    }
    Waiter w;
    w.id = e.next_token++;
    w.owner = core;
    w.port = core->ports[static_cast<uint32_t>(port)];
    w.port_index = static_cast<uint32_t>(port);
    w.handle = waitHandle;
    w.callback = callback;
    w.context = callbackContext;
    token->token = w.id;
    e.waiters.push_back(std::move(w));
    if (!e.wait_thread.joinable()) {
        e.wait_thread = std::thread(WaitLoop);
    }
    SetEvent(static_cast<HANDLE>(e.wait_wake));
    return XGR_S_OK;
#else
    (void)queue;
    (void)port;
    (void)waitHandle;
    (void)callbackContext;
    (void)callback;
    (void)token;
    return XGR_E_NOT_SUPPORTED;
#endif
}

void XTaskQueueUnregisterWaiter(XTaskQueueHandle /*queue*/, XTaskQueueRegistrationToken token) {
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    e.waiters.erase(std::remove_if(e.waiters.begin(), e.waiters.end(),
                                   [&](const Waiter& w) { return w.id == token.token; }),
                    e.waiters.end());
#if defined(_WIN32)
    if (e.wait_wake) {
        SetEvent(static_cast<HANDLE>(e.wait_wake));
    }
#endif
}

bool XTaskQueueCanDispatch(XTaskQueueHandle queue, XTaskQueuePort port) {
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    const std::shared_ptr<QueueCore> core = LookupLocked(e, queue);
    if (!core || static_cast<uint32_t>(port) > 1) {
        return false;
    }
    const std::shared_ptr<Port> p = core->ports[static_cast<uint32_t>(port)];
    if (p->mode != XTaskQueueDispatchMode_Manual) {
        return false;
    }
    return !p->ready.empty();
}

}  // extern "C"

// ---------------------------------------------------------------------------
// XAsync / XAsyncProvider
// ---------------------------------------------------------------------------
namespace {

struct AsyncState {
    std::atomic<uint32_t> refs{1};  // the initial reference belongs to the block
    XAsyncProviderData data{};
    XAsyncProvider* provider = nullptr;
    const void* identity = nullptr;
    XTaskQueueHandle queue = nullptr;  // owned duplicate
    void* owned_context = nullptr;     // allocated by XAsyncBeginAlloc; freed on destruction

    std::mutex mutex;
    std::condition_variable cv;
    XGameResult status = XGR_E_PENDING;
    size_t result_size = 0;
    bool work_scheduled = false;
    bool detached = false;  // unlinked from the block; Cleanup is owed exactly once
    uint32_t provider_depth = 0;   // provider calls in progress (Begin/DoWork/Cancel/GetResult)
    bool cleanup_deferred = false; // Cleanup was requested during such a call
    bool cleaned = false;          // Cleanup ran (or is running): no further provider calls
};

// What XAsyncBlock::internal holds. The status stays readable after the state is gone.
struct BlockInternal {
    AsyncState* state;
    XGameResult status;
    uint32_t signature;
};
static_assert(sizeof(BlockInternal) <= sizeof(XAsyncBlock::internal),
              "BlockInternal must fit XAsyncBlock::internal");

std::mutex& BlockLock(const XAsyncBlock* block) {
    return GetEngine().block_locks[(reinterpret_cast<uintptr_t>(block) >> 4) % 64];
}

void ReleaseState(AsyncState* s) {
    if (s && s->refs.fetch_sub(1) == 1) {
        if (s->queue) {
            XTaskQueueCloseHandle(s->queue);
        }
        std::free(s->owned_context);
        delete s;
    }
}

struct StateRef {
    AsyncState* s = nullptr;
    StateRef() = default;
    StateRef(const StateRef&) = delete;
    StateRef& operator=(const StateRef&) = delete;
    ~StateRef() { ReleaseState(s); }
    AsyncState* operator->() const { return s; }
    explicit operator bool() const { return s != nullptr; }
};

// Returns false when the block was never passed to XAsyncBegin.
bool AcquireState(XAsyncBlock* block, StateRef* ref, XGameResult* status) {
    if (!block) {
        return false;
    }
    std::lock_guard<std::mutex> lock(BlockLock(block));
    BlockInternal in;
    std::memcpy(&in, block->internal, sizeof(in));
    if (in.signature != kAsyncSignature) {
        return false;
    }
    if (in.state) {
        in.state->refs.fetch_add(1);
        ref->s = in.state;
    }
    *status = in.status;
    return true;
}

// Unlinks the state from its block (the block must still be alive). Returns true
// for the single caller that then owes CleanupState.
bool DetachState(AsyncState* s) {
    XAsyncBlock* block = s->data.async;
    std::lock_guard<std::mutex> block_lock(BlockLock(block));
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->detached) {
        return false;
    }
    s->detached = true;
    BlockInternal in;
    std::memcpy(&in, block->internal, sizeof(in));
    if (in.signature == kAsyncSignature && in.state == s) {
        in.state = nullptr;
        std::memcpy(block->internal, &in, sizeof(in));
    }
    return true;
}

void CleanupState(AsyncState* s) {
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->provider_depth != 0) {
            // The provider is still on a stack (it typically completed the call from
            // DoWork): its context must outlive that call.
            s->cleanup_deferred = true;
            return;
        }
        s->cleaned = true;
    }
    s->provider(XAsyncOp_Cleanup, &s->data);
    ReleaseState(s);  // the block's reference
}

// Calls the provider; the caller holds a reference on `s`. Cleanup never overlaps
// another provider call: one requested meanwhile runs when the call returns.
// With `only_if_pending` the call is skipped (E_ABORT) once the operation has
// completed; that check and the registration of the call are one step, so a
// concurrent completion cannot clean the provider up in between.
XGameResult InvokeProvider(AsyncState* s, XAsyncOp op, const XAsyncProviderData* data,
                           bool only_if_pending) {
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->cleaned || (only_if_pending && s->status != XGR_E_PENDING)) {
            return XGR_E_ABORT;
        }
        ++s->provider_depth;
    }
    const XGameResult hr = s->provider(op, data);
    bool cleanup = false;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        cleanup = --s->provider_depth == 0 && s->cleanup_deferred;
        if (cleanup) {
            s->cleanup_deferred = false;
            s->cleaned = true;
        }
    }
    if (cleanup) {
        s->provider(XAsyncOp_Cleanup, &s->data);
        ReleaseState(s);  // the block's reference
    }
    return hr;
}

void SCARLETTX_GDK_CALLBACK AsyncCompletionCallback(void* context, bool /*canceled*/) {
    auto* s = static_cast<AsyncState*>(context);
    XAsyncBlock* block = s->data.async;
    XAsyncCompletionRoutine* callback = block->callback;
    // Guests commonly free the block inside the completion callback: unlink a
    // payload-less state first and do not touch the block afterwards.
    const bool cleanup = s->result_size == 0 && DetachState(s);
    if (callback) {
        callback(block);
    }
    if (cleanup) {
        CleanupState(s);
    }
    ReleaseState(s);
}

// The caller holds a reference on `s`.
void CompleteState(AsyncState* s, XGameResult result, size_t required_size) {
    if (result == XGR_E_PENDING) {
        return;
    }
    XAsyncBlock* block = s->data.async;
    XAsyncCompletionRoutine* callback = nullptr;
    bool cleanup = false;
    {
        std::lock_guard<std::mutex> block_lock(BlockLock(block));
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->status != XGR_E_PENDING) {
            return;
        }
        s->status = result;
        s->result_size = result < 0 ? 0 : required_size;
        BlockInternal in;
        std::memcpy(&in, block->internal, sizeof(in));
        const bool linked = in.signature == kAsyncSignature && in.state == s;
        if (linked) {
            callback = block->callback;
            in.status = result;
        }
        // Without a callback or payload nothing else will reference the block, and a
        // polling guest may free it as soon as the status is final: unlink right here.
        if (!callback && s->result_size == 0 && !s->detached) {
            s->detached = true;
            cleanup = true;
            if (linked) {
                in.state = nullptr;
            }
        }
        if (linked) {
            std::memcpy(block->internal, &in, sizeof(in));
        }
    }
    s->cv.notify_all();

    if (callback) {
        s->refs.fetch_add(1);
        if (XTaskQueueSubmitCallback(s->queue, XTaskQueuePort_Completion, s,
                                     AsyncCompletionCallback) >= 0) {
            return;
        }
        ReleaseState(s);  // the queue was terminated; the callback cannot be delivered
        cleanup = s->result_size == 0 && DetachState(s);
    }
    if (cleanup) {
        CleanupState(s);
    }
}

void SCARLETTX_GDK_CALLBACK AsyncWorkCallback(void* context, bool canceled) {
    auto* s = static_cast<AsyncState*>(context);
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->work_scheduled = false;
    }
    // Both are no-ops for a call that completed meanwhile (e.g. it was canceled).
    if (canceled) {
        CompleteState(s, XGR_E_ABORT, 0);
    } else {
        const XGameResult hr = InvokeProvider(s, XAsyncOp_DoWork, &s->data, true);
        CompleteState(s, hr, 0);  // also a no-op for E_PENDING
    }
    ReleaseState(s);
}

struct RunContext {
    XAsyncWork* work = nullptr;
    std::atomic<int> state{0};  // 0 = not started, 1 = work started, 2 = canceled
};

XGameResult SCARLETTX_GDK_CALLBACK RunProvider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<RunContext*>(data->context);
    switch (op) {
        case XAsyncOp_DoWork: {
            int expected = 0;
            if (!ctx->state.compare_exchange_strong(expected, 1)) {
                return XGR_E_ABORT;
            }
            return ctx->work(data->async);
        }
        case XAsyncOp_Cancel: {
            // Work that already started runs to its own result.
            int expected = 0;
            if (ctx->state.compare_exchange_strong(expected, 2)) {
                XAsyncComplete(data->async, XGR_E_ABORT, 0);
            }
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

const char kRunIdentity[] = "XAsyncRun";

}  // namespace

extern "C" {

XGameResult XAsyncGetStatus(XAsyncBlock* asyncBlock, bool wait) {
    StateRef s;
    XGameResult status = XGR_E_INVALIDARG;
    if (!AcquireState(asyncBlock, &s, &status)) {
        return XGR_E_INVALIDARG;
    }
    if (!s) {
        return status;
    }
    Lock lk(s->mutex);
    if (wait) {
        s->cv.wait(lk, [&] { return s->status != XGR_E_PENDING; });
    }
    return s->status;
}

XGameResult XAsyncGetResultSize(XAsyncBlock* asyncBlock, size_t* bufferSize) {
    if (!bufferSize) {
        return XGR_E_POINTER;
    }
    StateRef s;
    XGameResult status = XGR_E_INVALIDARG;
    if (!AcquireState(asyncBlock, &s, &status)) {
        return XGR_E_INVALIDARG;
    }
    size_t size = 0;
    if (s) {
        std::lock_guard<std::mutex> lock(s->mutex);
        status = s->status;
        size = s->result_size;
    }
    if (status == XGR_E_PENDING) {
        return status;
    }
    *bufferSize = status < 0 ? 0 : size;
    return status;
}

void XAsyncCancel(XAsyncBlock* asyncBlock) {
    StateRef s;
    XGameResult status = XGR_E_INVALIDARG;
    if (!AcquireState(asyncBlock, &s, &status) || !s) {
        return;
    }
    InvokeProvider(s.s, XAsyncOp_Cancel, &s->data, true);
}

static XGameResult AsyncBeginInternal(XAsyncBlock* asyncBlock,
                                      void* context,
                                      bool alloc_context,
                                      size_t context_size,
                                      const void* identity,
                                      const char* /*identityName*/,
                                      XAsyncProvider* provider) {
    if (!asyncBlock || !provider) {
        return XGR_E_INVALIDARG;
    }
    XTaskQueueHandle queue = nullptr;
    if (asyncBlock->queue) {
        if (XTaskQueueDuplicateHandle(asyncBlock->queue, &queue) < 0) {
            return XGR_E_INVALIDARG;
        }
    } else if (!XTaskQueueGetCurrentProcessTaskQueue(&queue)) {
        return XGR_E_NO_TASK_QUEUE;
    }

    void* owned = nullptr;
    if (alloc_context && context_size > 0) {
        owned = std::calloc(1, context_size);
        if (!owned) {
            XTaskQueueCloseHandle(queue);
            return XGR_E_OUTOFMEMORY;
        }
        context = owned;
    }

    auto* s = new (std::nothrow) AsyncState();
    if (!s) {
        std::free(owned);
        XTaskQueueCloseHandle(queue);
        return XGR_E_OUTOFMEMORY;
    }
    s->data.async = asyncBlock;
    s->data.context = context;
    s->provider = provider;
    s->identity = identity;
    s->queue = queue;
    s->owned_context = owned;
    s->refs.fetch_add(1);  // ours, for the calls below
    {
        std::lock_guard<std::mutex> lock(BlockLock(asyncBlock));
        BlockInternal in{s, XGR_E_PENDING, kAsyncSignature};
        std::memset(asyncBlock->internal, 0, sizeof(asyncBlock->internal));
        std::memcpy(asyncBlock->internal, &in, sizeof(in));
    }
    const XGameResult hr = InvokeProvider(s, XAsyncOp_Begin, &s->data, true);
    if (hr < 0) {
        CompleteState(s, hr, 0);
    }
    ReleaseState(s);
    return XGR_S_OK;
}

XGameResult XAsyncBegin(XAsyncBlock* asyncBlock, void* context, const void* identity,
                        const char* identityName, XAsyncProvider* provider) {
    return AsyncBeginInternal(asyncBlock, context, false, 0, identity, identityName, provider);
}

XGameResult XAsyncBeginAlloc(XAsyncBlock* asyncBlock, const void* identity,
                             const char* identityName, XAsyncProvider* provider,
                             size_t contextSize) {
    return AsyncBeginInternal(asyncBlock, nullptr, true, contextSize, identity, identityName,
                              provider);
}

XGameResult XAsyncSchedule(XAsyncBlock* asyncBlock, uint32_t delayInMs) {
    StateRef s;
    XGameResult status = XGR_E_INVALIDARG;
    if (!AcquireState(asyncBlock, &s, &status) || !s) {
        return XGR_E_INVALIDARG;
    }
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->work_scheduled) {
            return XGR_E_UNEXPECTED;
        }
        s->work_scheduled = true;
    }
    s->refs.fetch_add(1);
    const XGameResult hr = XTaskQueueSubmitDelayedCallback(
        s->queue, XTaskQueuePort_Work, delayInMs, s.s, AsyncWorkCallback);
    if (hr < 0) {
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->work_scheduled = false;
        }
        ReleaseState(s.s);
    }
    return hr;
}

void XAsyncComplete(XAsyncBlock* asyncBlock, XGameResult result, size_t requiredBufferSize) {
    StateRef s;
    XGameResult status = XGR_E_INVALIDARG;
    if (!AcquireState(asyncBlock, &s, &status) || !s) {
        return;
    }
    CompleteState(s.s, result, requiredBufferSize);
}

XGameResult XAsyncGetResult(XAsyncBlock* asyncBlock, const void* identity, size_t bufferSize,
                            void* buffer, size_t* bufferUsed) {
    StateRef s;
    XGameResult status = XGR_E_INVALIDARG;
    if (!AcquireState(asyncBlock, &s, &status)) {
        return XGR_E_INVALIDARG;
    }
    if (!s) {
        // Completed without a payload (or the payload was already fetched).
        return status < 0 ? status : XGR_E_NOT_SUPPORTED;
    }
    size_t size = 0;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        status = s->status;
        size = s->result_size;
    }
    if (status < 0) {
        return status;  // includes E_PENDING
    }
    if (identity != s->identity) {
        return XGR_E_INVALIDARG;
    }
    if (size == 0) {
        return XGR_E_NOT_SUPPORTED;
    }
    if (!buffer) {
        return XGR_E_INVALIDARG;
    }
    if (bufferSize < size) {
        return XGR_E_NOT_SUFFICIENT_BUFFER;
    }
    XAsyncProviderData data = s->data;
    data.buffer = buffer;
    data.bufferSize = bufferSize;
    const XGameResult hr = InvokeProvider(s.s, XAsyncOp_GetResult, &data, false);
    if (hr >= 0 && bufferUsed) {
        *bufferUsed = size;
    }
    if (DetachState(s.s)) {
        CleanupState(s.s);
    }
    return hr;
}

XGameResult XAsyncRun(XAsyncBlock* asyncBlock, XAsyncWork* work) {
    if (!asyncBlock || !work) {
        return XGR_E_INVALIDARG;
    }
    auto* ctx = new RunContext();
    ctx->work = work;
    XGameResult hr = XAsyncBegin(asyncBlock, ctx, kRunIdentity, kRunIdentity, RunProvider);
    if (hr < 0) {
        delete ctx;
        return hr;
    }
    hr = XAsyncSchedule(asyncBlock, 0);
    if (hr < 0) {
        XAsyncComplete(asyncBlock, hr, 0);
    }
    return hr;
}

}  // extern "C"

// ---------------------------------------------------------------------------
// HLE-side helpers
// ---------------------------------------------------------------------------
namespace xgr {
namespace {

using HleRef = std::shared_ptr<HleAsyncState>;
const char kHleIdentity[] = "ScarlettX HLE async";

XGameResult SCARLETTX_GDK_CALLBACK HleProvider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ref = static_cast<HleRef*>(data->context);
    HleAsyncState* st = ref->get();
    switch (op) {
        case XAsyncOp_DoWork: {
            bool canceled = false;
            {
                std::lock_guard<std::mutex> lock(st->mutex);
                canceled = st->canceled;
            }
            if (canceled) {
                CompleteHleAsync(st, XGR_E_ABORT, 0);
            } else if (st->work_fn) {
                st->work_fn(st);
            }
            XGameResult status = XGR_E_FAIL;
            size_t size = 0;
            {
                std::lock_guard<std::mutex> lock(st->mutex);
                if (!st->completed) {
                    st->status = XGR_E_FAIL;
                    st->completed = true;
                }
                status = st->status;
                size = st->result_size;
            }
            XAsyncComplete(data->async, status, size);
            return XGR_E_PENDING;
        }
        case XAsyncOp_Cancel: {
            std::lock_guard<std::mutex> lock(st->mutex);
            st->canceled = true;
            return XGR_S_OK;
        }
        case XAsyncOp_GetResult:
            return XGR_E_NOT_SUPPORTED;
        case XAsyncOp_Cleanup:
            delete ref;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

}  // namespace

XGameResult BeginHleAsync(XAsyncBlock* async, std::function<void(HleAsyncState*)> work_fn) {
    if (!async) {
        return XGR_E_INVALIDARG;
    }
    auto* ref = new HleRef(std::make_shared<HleAsyncState>());
    (*ref)->block = async;
    (*ref)->work_fn = std::move(work_fn);
    XGameResult hr = XAsyncBegin(async, ref, kHleIdentity, kHleIdentity, HleProvider);
    if (hr < 0) {
        delete ref;
        return hr;
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return hr;
}

void CompleteHleAsync(HleAsyncState* state, XGameResult hr, size_t result_size) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->completed) {
        state->status = state->canceled ? XGR_E_ABORT : hr;
        state->result_size = result_size;
        state->completed = true;
    }
}

std::shared_ptr<HleAsyncState> GetHleAsyncState(XAsyncBlock* async) {
    StateRef s;
    XGameResult status = XGR_E_INVALIDARG;
    if (!AcquireState(async, &s, &status) || !s || s->provider != HleProvider) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->detached) {
        return nullptr;
    }
    return *static_cast<HleRef*>(s->data.context);
}

void ReleaseHleAsyncResult(XAsyncBlock* async) {
    StateRef s;
    XGameResult status = XGR_E_INVALIDARG;
    if (AcquireState(async, &s, &status) && s && s->provider == HleProvider && DetachState(s.s)) {
        CleanupState(s.s);
    }
}

void EnsureProcessTaskQueue() {
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    EnsureProcessQueueLocked(e);
}

bool IsTaskQueueHandle(void* handle) {
    Engine& e = GetEngine();
    Lock lk(e.mutex);
    return e.handles.find(static_cast<XTaskQueueObject*>(handle)) != e.handles.end();
}

void ShutdownTaskSystem() {
    Engine& e = GetEngine();
    XTaskQueueObject* process_queue = nullptr;
    {
        Lock lk(e.mutex);
        process_queue = e.process_queue;
        e.process_queue = nullptr;
        e.process_queue_cleared = false;
    }
    if (process_queue) {
        XTaskQueueTerminate(process_queue, true, nullptr, nullptr);
        XTaskQueueCloseHandle(process_queue);
    }

    std::vector<std::thread> threads;
    {
        Lock lk(e.mutex);
        e.stopping = true;
        threads.swap(e.pool_threads);
        if (e.timer_thread.joinable()) {
            threads.push_back(std::move(e.timer_thread));
        }
        if (e.wait_thread.joinable()) {
            threads.push_back(std::move(e.wait_thread));
        }
        e.pool_cv.notify_all();
        e.timer_cv.notify_all();
#if defined(_WIN32)
        if (e.wait_wake) {
            SetEvent(static_cast<HANDLE>(e.wait_wake));
        }
#endif
    }
    for (auto& t : threads) {
        if (t.get_id() == std::this_thread::get_id()) {
            t.detach();  // called from one of our own callbacks
        } else {
            t.join();
        }
    }
    Lock lk(e.mutex);
    e.stopping = false;
    // Work that was queued for the pool or the timer is picked up again on restart.
    if (!e.pool_jobs.empty()) {
        e.pool_threads.emplace_back(PoolWorker);
    }
    if (!e.timers.empty()) {
        e.timer_thread = std::thread(TimerLoop);
    }
#if defined(_WIN32)
    if (!e.waiters.empty()) {
        e.wait_thread = std::thread(WaitLoop);  // queues that outlive the shutdown
    }
#endif
}

}  // namespace xgr
