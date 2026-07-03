// uniflow.hpp - single-threaded, step-driven cooperative async framework.
// Single header, C++17.
//
// A module is a class deriving from Uniflow<Derived>; its logic is a chain of
// step member functions returning StepResult. One pump thread per Runtime
// drives every attached module round-robin; blocking work goes to a thread pool
// via SubmitAsync. No framework-side global state - the user creates a Runtime
// and constructs modules with 'Runtime&'.
//
// A flow is organised into tasks. Each task is a struct deriving from
// uniflow::Task<Flow>; it OWNS the state its steps share AND the step member
// functions themselves. The flow holds one instance of each task, declared
// public so a peer can launch it. A step is a member of its task, so it names
// no context - it reaches sibling state directly and the parent flow through
// flow(). Each task designates its first step by overriding Entry(); a task is
// launched from ANY thread with task.StartFlow() (or module.StartTask(task)),
// returning StartResult (Ok / Busy). A module runs one task at a time. Steps
// within a task advance with Next(UF_FN(fn)); crossing to another task is
// StartTask(other) - Next never leaves the current task.
//
//   class OrderRouter : public uniflow::Uniflow<OrderRouter> {
//   public:
//       explicit OrderRouter(uniflow::Runtime& rt)
//           : uniflow::Uniflow<OrderRouter>(rt, "OrderRouter")
//       { AddTask(route); }
//       struct Route : uniflow::Task<OrderRouter> {     // public: peers launch it
//           StepResult Entry() override { return OnRoute_Begin(); }
//           StepResult OnRoute_Begin() { return Next(UF_FN(OnRoute_Done)); }
//           StepResult OnRoute_Done()  { return Done(); }
//       } route;
//   };
//
//   uniflow::Runtime rt;
//   OrderRouter      router{rt};
//   router.route.StartFlow();          // launch the task (when idle)
//   router.WaitUntilIdle();
#pragma once

#include <algorithm>
#include <any>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace uniflow
{

// ----- Version -----
// uniflow's own version. Distinct from the bundled BS::thread_pool version
// (BS_THREAD_POOL_VERSION_*), which tracks the upstream pool library.
#define UNIFLOW_VERSION_MAJOR 1
#define UNIFLOW_VERSION_MINOR 0
#define UNIFLOW_VERSION_PATCH 0
#define UNIFLOW_VERSION_STRING "1.0.0"

inline constexpr int         kVersionMajor = UNIFLOW_VERSION_MAJOR;
inline constexpr int         kVersionMinor = UNIFLOW_VERSION_MINOR;
inline constexpr int         kVersionPatch = UNIFLOW_VERSION_PATCH;
inline constexpr const char* kVersion      = UNIFLOW_VERSION_STRING;

// ----- Time -----
// Duration is in milliseconds so debugger watches show readable integers
// (e.g. 8133 instead of 8133430000). Clock::now() differences come back as
// the platform clock's native unit (typically nanoseconds); convert with
// to_duration() at the boundary into anything that gets stored in or
// compared against a Duration.
using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = std::chrono::milliseconds;

inline double to_ms(Duration d) { return static_cast<double>(d.count()); }

inline double to_ms(Clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

// Lossy but explicit ns-or-whatever -> ms cast. Sub-millisecond precision
// is dropped on purpose (durations are stored as ms for debugger sanity).
template <class Rep, class Period>
inline Duration to_duration(const std::chrono::duration<Rep, Period>& d)
{
    return std::chrono::duration_cast<Duration>(d);
}

// ----- Virtual clock: scalable / freezable LOGICAL time -----
// The time source for UFTimer and the StayUntil step deadline. By default it
// tracks the real steady_clock 1:1, but it can be sped up / slowed down
// (SetScale) or paused (Freeze/Resume) - useful for simulation playback and
// for not letting logical timeouts fire while a line is held / e-stopped.
//
// It governs ONLY logical waits. Async / IO deadlines (the SubmitAsync timeout)
// and the pump's own sleeps stay on real wall-clock - a network call does not
// speed up because the sim clock was scaled.
//
// Now() is computed fresh on every call (no per-round caching), so a timer's
// reference stays exactly the instant it was armed, even if the step that
// armed it burned time first. Thread-safe.
class VirtualClock
{
public:
    VirtualClock()
    {
        base_real_    = Clock::now();
        base_virtual_ = base_real_;
    }

    TimePoint Now() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return now_locked();
    }

    // Rate at which virtual time advances: 1.0 = real time, 10.0 = ten times
    // faster, 0.0 = stopped (Freeze is the explicit pause). Rebased so the
    // change is continuous (no jump in Now()).
    void SetScale(double scale)
    {
        std::lock_guard<std::mutex> lk(mu_);
        rebase_locked();
        scale_ = scale;
    }
    double Scale() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return scale_;
    }

    // Stop virtual time. Elapsed() on every bound timer freezes and logical
    // timeouts (StayUntil) stop counting down until Resume().
    void Freeze()
    {
        std::lock_guard<std::mutex> lk(mu_);
        rebase_locked();
        frozen_ = true;
    }
    void Resume()
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (frozen_)
        {
            base_real_ = Clock::now();
            frozen_    = false;
        }
    }
    bool Frozen() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return frozen_;
    }

private:
    TimePoint now_locked() const
    {
        if (frozen_)
            return base_virtual_;
        std::chrono::duration<double, std::nano> real_elapsed = Clock::now() - base_real_;
        return base_virtual_ + std::chrono::duration_cast<Clock::duration>(real_elapsed * scale_);
    }
    // Capture the current virtual time as the new origin so a scale / freeze
    // change does not discontinuously move Now().
    void rebase_locked()
    {
        base_virtual_ = now_locked();
        base_real_    = Clock::now();
    }

    mutable std::mutex mu_;
    TimePoint          base_real_;
    TimePoint          base_virtual_;
    double             scale_  = 1.0;
    bool               frozen_ = false;
};

// Process-wide real-time clock: the default source for a standalone UFTimer
// (scale 1, never frozen). A timer bound to a Runtime's clock instead follows
// that runtime's scale / freeze.
inline const VirtualClock& RealClock()
{
    static VirtualClock c;
    return c;
}

// ----- StepAction: a step returns an intent, not a state change -----
// Four intents only: Stay, Next, Done, Fail. Stay carries an optional
// per-module gate delay (default zero -> pump idle_sleep_ms cadence).
enum class StepAction : uint8_t
{
    Stay, // re-run the same step (optional per-module gate via Stay(d))
    Next, // advance to result.next_fn on the next pump round
    Done, // flow completed normally -> idle
    Fail  // flow aborted -> idle
};

inline const char* to_string(StepAction a)
{
    switch (a)
    {
    case StepAction::Stay:
        return "Stay";
    case StepAction::Next:
        return "Next";
    case StepAction::Done:
        return "Done";
    case StepAction::Fail:
        return "Fail";
    }
    return "?";
}

// ----- StartResult: outcome of StartTask / StartFlow (launching a task) -----
//   Ok   - the task was launched.
//   Busy - a task is already running on this module; nothing happened.
enum class StartResult : uint8_t
{
    Ok,
    Busy
};

inline const char* to_string(StartResult r)
{
    switch (r)
    {
    case StartResult::Ok:
        return "Ok";
    case StartResult::Busy:
        return "Busy";
    }
    return "?";
}

// ----- Async support -----

// Stored in the async result slot when a submission exceeds its timeout.
struct AsyncTimeout : std::runtime_error
{
    AsyncTimeout(const char* job, Duration elapsed)
        : std::runtime_error(std::string("async timeout: ") + job), job_label(job), elapsed(elapsed)
    {
    }
    const char* job_label;
    Duration    elapsed;
};

// Opaque handle returned by SubmitAsync, identifying one submission for the
// lifetime of the flow. Monotonic within a flow; 0 is the invalid id, returned
// when a submission is rejected (the in-flight cap was hit). Pass it to a
// continuation step (Next(UF_FN(step), id)) and read the worker's result there
// with AsyncResult<T>(id).
using AsyncId = std::int64_t;

// By-value snapshot of one async submission, returned by AsyncResult<T>(id).
// 'state' classifies the slot; 'return_value' holds the worker's result and is
// engaged ONLY when state == Done. A bad / cleared / cap-rejected id (including
// 0) reads back as NotFound, so a missing slot drops into the same error branch
// as a failed one - there is no pointer to dereference and no way to read a
// result that is not there.
template <class T> struct AsyncOutcome
{
    enum class State
    {
        NotFound, // id never matched a live slot (bad id, id == 0, or cleared)
        Pending,  // the worker is still in flight
        Done,     // the worker returned - 'return_value' holds it
        Failed,   // the worker threw
        TimedOut  // the worker missed its deadline
    };

    State            state = State::NotFound;
    std::optional<T> return_value; // engaged iff state == Done (and T matched)

    bool ok() const { return state == State::Done; }
    bool pending() const { return state == State::Pending; }
    bool failed() const { return state == State::Failed; }
    bool is_timeout() const { return state == State::TimedOut; }
    bool found() const { return state != State::NotFound; }
};

// One in-flight (or just-resolved) async submission, owned by the flow and
// held in a std::deque so a slot's address stays stable as new submissions are
// appended (a vector would reallocate and dangle). The pump thread is the only
// slot writer once the worker is launched: the worker writes its result into
// 'promise' (a shared_ptr it co-owns, NOT the slot), and the pump's per-round
// sweep moves it out through 'fut' into value/exc and flips 'done'. Because the
// worker never touches the slot or the flow, a dropped slot (ClearAsync) simply
// abandons the worker - it finishes into an orphaned promise, harmlessly.
struct AsyncSlot
{
    AsyncId     id    = 0;
    const char* label = nullptr;
    Duration    timeout      = Duration::max();
    TimePoint   submitted_at = {};
    bool        slow_warned  = false; // OnSlowAsync fired once for this slot

    std::future<std::any> fut;             // pump-thread read side
    bool                  done = false;    // resolved: value / exc / timed_out set
    std::any              value;           // set on success
    std::exception_ptr    exc;             // set if the worker threw
    bool                  timed_out = false;
};

// ----- Executor abstraction -----
class IExecutor
{
public:
    virtual ~IExecutor()                              = default;
    virtual void   Submit(std::function<void()> task) = 0;
    virtual size_t QueueDepth() const                 = 0;
};

// Runs the task synchronously on the calling thread. Useful for tests - the
// whole flow becomes deterministic. NOT for production: it blocks the pump
// thread inside SubmitAsync.
class InlineExecutor : public IExecutor
{
public:
    void   Submit(std::function<void()> task) override { task(); }
    size_t QueueDepth() const override { return 0; }
};

// ----- Trace + observer -----
// One TraceEntry per STEP (not per tick) - Stay re-entries collapse into a
// single entry recorded when the step finally Next/Done/Fail'd.
// 'ticks' = how many times the body ran during that step. Kept for
// inspection by custom observers; the bundled ConsoleObserver does not
// print it because tick counts rarely help with judgement at a glance.
struct TraceEntry
{
    enum class Kind
    {
        StepTransition,
        AsyncCompletion
    };
    Kind        kind;            // which of the two timing fields applies
    int         ordinal = 0;     // step number within the flow
    std::string name;            // step name or async job label
    int         ticks     = 0;   // StepTransition: body invocations in this step
    double      step_ms   = 0.0; // StepTransition: wall time spent in this step
    double      async_ms  = 0.0; // AsyncCompletion: pool wait time in ms
    bool        had_error = false;
    bool        timed_out = false;
    StepAction  action    = StepAction::Stay; // StepTransition: terminal action
};

// Aggregate stats over a set of step body invocations ('ticks'). Each
// tick's wall time is measured ONLY around the body call - Stay sleeps
// and async-pending waits are NOT included. Single-threaded round-robin
// is responsive only as long as no individual tick monopolises the pump
// thread, so per-step and per-flow tick statistics are the primary lens
// for spotting an outlier.
struct TickStats
{
    int    count  = 0;   // number of body invocations measured
    double min_ms = 0.0; // shortest tick
    double max_ms = 0.0; // longest tick
    double avg_ms = 0.0; // mean (sum / count)
};

// Tick statistics rolled up over a whole flow, plus a pointer to which
// step owned the worst tick - the prime "who blocked the pump?" answer.
struct FlowTickSummary
{
    TickStats        all;      // count/min/max/avg over every tick in the flow
    std::string_view max_step; // name of the step that ran the worst tick
};

// Counters that persist across runs of a flow on the same module. max_seen_
// length drives the "reached #N/#M" log: N this run, M the longest ever.
struct FlowStats
{
    int         last_success_length = 0;
    int         max_seen_length     = 0;
    std::size_t success_count       = 0;
    std::size_t fail_count          = 0;
};

// ----- Per-round (pump cycle) profiling -----
// A pump round = one pass of the pump loop: drain posted callbacks, then run
// every active module once. When a round runs long (a step or post hogged the
// single pump thread), these structures explain which segment cost what.

// One timed unit of work inside a round: a step body invocation or a posted
// callback. 'ms' is the wall time that segment held the pump thread.
struct RoundSegment
{
    enum class Kind
    {
        Step,
        Post
    };
    Kind        kind = Kind::Step;
    std::string obj;   // module instance name (Step), or "rt#N" (Post)
    std::string label; // step name (Step), or caller "file:line fn()" (Post)
    double      ms = 0.0;
};

// Breakdown of a single pump round, handed to OnSlowRound. 'segments' is
// filled only when round tracing is enabled (Runtime::SetRoundTracing);
// without it you still get 'busy_ms' (the round's total work time) but no
// per-segment detail - a cheap "a slow round happened" signal.
struct RoundProfile
{
    int                       runtime_index = -1;
    double                    busy_ms       = 0.0; // round work time (no sleep)
    int                       segment_count = 0;
    std::vector<RoundSegment> segments;
};

// Running stats over pump rounds that actually did work (pure idle polling
// rounds are not counted). min/max/avg describe the round work-time
// distribution; 'last' is the most recent round. Read via
// Runtime::GetRoundStats, cleared (peak reset) via Runtime::ResetRoundStats.
struct RoundStats
{
    std::size_t count   = 0;
    double      min_ms  = 0.0;
    double      max_ms  = 0.0;
    double      avg_ms  = 0.0;
    double      last_ms = 0.0;
};

// Origin record: where in the source was a flow launched? Currently always
// blank - tasks are launched by StartTask, which does not capture a call site -
// but the field is still threaded through OnFlowStarted / OnFlowEnded for
// observers that want it. The directory part of any path is stripped at capture
// time so log lines stay readable without leaking absolute build paths.
struct FlowOrigin
{
    const char* file     = nullptr; // basename only (no directory), nullptr if raw
    int         line     = 0;
    const char* function = nullptr; // __FUNCTION__ of the caller
};

// Walk to the last path separator and return everything after it; if the
// path has no separator, return the whole thing unchanged. constexpr so the
// stripping happens at compile time on the literal that __FILE__ expands to.
constexpr const char* basename_of(const char* path)
{
    const char* file = path;
    for (const char* p = path; *p != '\0'; ++p)
        if (*p == '/' || *p == '\\')
            file = p + 1;
    return file;
}

// Source location of a Post / PostAndWait / Link call, captured by the
// PostAt / PostAndWaitAt / LinkAt methods (file basename / __LINE__ /
// __FUNCTION__) and handed to the matching observer hooks so cross-runtime
// traffic is traceable to its caller. All fields stay blank when the bare
// Post / PostAndWait / Link method is called. Mirrors FlowOrigin, which plays
// the same role for flow launches.
struct CallSite
{
    const char* file     = nullptr; // basename only (no directory)
    int         line     = 0;
    const char* function = nullptr; // __FUNCTION__ of the caller
};

// "file:line function()" for logs, or "(no caller)" when blank (the bare
// Post / PostAndWait / Link methods were used without a UF_ macro).
inline std::string to_string(CallSite c)
{
    if (!c.file)
        return "(no caller)";
    std::ostringstream os;
    os << c.file << ":" << c.line;
    if (c.function)
        os << " " << c.function << "()";
    return os.str();
}

// Every log line / metric the framework produces is funnelled through one of
// these hooks - the framework itself never touches std::cout. Subclass and
// pass via Runtime::Opts::observer; override only the events you care about.
//
// All time arguments are double milliseconds, rounded at the framework
// boundary. Internal accounting still uses chrono::duration; the conversion
// happens once here so user observers do not need to know about Duration.
class IUniflowObserver
{
public:
    virtual ~IUniflowObserver() = default;

    // Fired once when StartTask / StartFlow arms a fresh task on 'obj'. Called
    // on the pump thread driving that module, before any step runs. 'origin' is
    // the source location of the caller (blank now that tasks are launched by
    // StartTask).
    virtual void OnFlowStarted([[maybe_unused]] std::string_view obj,
                               [[maybe_unused]] FlowOrigin       origin)
    {
    }

    // Fired once per STEP (not per tick), at the moment the step transitions
    // away - either Next to a different step, or Done / Fail terminating
    // the flow. Reports the step that JUST FINISHED and the one about to
    // start, with totals accumulated over the previous step's lifetime
    // (sum of every Stay re-entry plus the final return):
    //
    //   'prev_step'      = name of the step that just finished.
    //   'next_step'      = name of the step about to start, or "" when the
    //                      flow ends (Done / Fail).
    //   'step_ordinal'   = 0-based index of prev_step within the flow.
    //   'ticks_in_step'  = how many times prev_step's body ran (Stay re-
    //                      entries plus the final call).
    //   'elapsed_ms'     = wall time from prev_step entry until this
    //                      transition.
    //   'description'    = the last value prev_step set via Describe(...).
    //
    // Use this for high-signal flow logs. There is no per-tick callback by
    // design - a Stay loop iterating thousands of times no longer floods
    // the observer.
    //
    //   'step_ticks'     = body-time stats for this step. min/max/avg are
    //                      pure pump-thread time spent in the body (no
    //                      Stay sleeps, no async-pending waits), and
    //                      step_ticks.count is the body invocation count.
    //                      Use these to spot a single tick that
    //                      monopolised the pump - elapsed_ms alone
    //                      cannot distinguish 'one slow tick' from
    //                      'many short ticks while Stay-polling'.
    virtual void OnStepChanged([[maybe_unused]] std::string_view obj,
                               [[maybe_unused]] std::string_view prev_step,
                               [[maybe_unused]] std::string_view next_step,
                               [[maybe_unused]] std::string_view description,
                               [[maybe_unused]] int              step_ordinal,
                               [[maybe_unused]] double           elapsed_ms,
                               [[maybe_unused]] const TickStats& step_ticks)
    {
    }

    // Fired when a step body throws. 'what' is the exception's what() (or
    // 'unknown' for non-std exceptions). The flow is forcibly Failed right
    // after this hook returns; the exception does NOT propagate out of the
    // pump thread.
    virtual void OnStepThrew([[maybe_unused]] std::string_view obj,
                             [[maybe_unused]] std::string_view step,
                             [[maybe_unused]] std::string_view what,
                             [[maybe_unused]] int step_ordinal, [[maybe_unused]] int tick)
    {
    }

    // Fired right after SubmitAsync successfully enqueues 'job' to the pool.
    virtual void OnAsyncSubmitted([[maybe_unused]] std::string_view obj,
                                  [[maybe_unused]] std::string_view step,
                                  [[maybe_unused]] std::string_view job)
    {
    }

    // Fired once per async job, after the worker either returned a value,
    // threw, or missed its deadline.
    virtual void OnAsyncCompleted([[maybe_unused]] std::string_view obj,
                                  [[maybe_unused]] std::string_view job,
                                  [[maybe_unused]] double wait_ms, [[maybe_unused]] bool had_error,
                                  [[maybe_unused]] bool timed_out)
    {
    }

    // Single-thread-protection alarm: a step held the pump thread longer
    // than Config::slow_step_threshold_ms this single body invocation.
    // Set the threshold to Duration::max() to disable.
    virtual void OnSlowCpuStep([[maybe_unused]] std::string_view obj,
                               [[maybe_unused]] std::string_view step,
                               [[maybe_unused]] double           cpu_ms)
    {
    }

    // Fired at most once per async job, when its in-flight time crosses
    // Config::slow_async_threshold_ms. Set the threshold to Duration::max()
    // (the default) to disable.
    virtual void OnSlowAsync([[maybe_unused]] std::string_view obj,
                             [[maybe_unused]] std::string_view job,
                             [[maybe_unused]] double           wait_so_far_ms)
    {
    }

    // Fired once per abandoned worker when ClearAsync (or a flow ending with
    // submissions still in flight) drops a slot whose worker had not finished.
    // The worker KEEPS RUNNING on the pool until it completes naturally; its
    // result is discarded. 'pending_ms' is how long it had been in flight when
    // abandoned. A loud signal that a worker outlived its flow - if these
    // accumulate, pool threads can starve (no portable way to kill a worker).
    virtual void OnAsyncAbandoned([[maybe_unused]] std::string_view obj,
                                  [[maybe_unused]] std::string_view job,
                                  [[maybe_unused]] double           pending_ms)
    {
    }

    // Fired when SubmitAsync is rejected because Config::max_inflight_async
    // submissions are already in flight for this flow. The job did NOT enqueue
    // and SubmitAsync returned id 0; 'inflight' is the live-slot count at the
    // moment of refusal. Catches a flow that fires async recklessly without
    // reading or clearing results.
    virtual void OnAsyncHighWater([[maybe_unused]] std::string_view obj,
                                  [[maybe_unused]] std::string_view job,
                                  [[maybe_unused]] std::size_t      inflight)
    {
    }

    // Fired at the end of a pump round whose work time exceeded
    // Config::slow_round_threshold_ms. 'profile.busy_ms' is the round's total
    // work time (no inter-round sleep); 'profile.segments' lists each step /
    // post that ran THIS round with its duration - but only when round
    // tracing is on (Runtime::SetRoundTracing). Answers "a 50ms cycle
    // happened - which step or post caused it?". Called on the pump thread.
    virtual void OnSlowRound([[maybe_unused]] int                 runtime_index,
                             [[maybe_unused]] const RoundProfile& profile)
    {
    }

    // Fired once when a flow leaves the running state via Done or Fail.
    //   'terminal_action'    - Done or Fail
    //   'final_step_ordinal' - logical step count reached (Next-only)
    //   'trace'              - ordered StepTransition + AsyncCompletion log
    //   'wall_ms'            - flow start -> end wall time, ms
    //   'total_step_ms'      - summed pump-thread time across every step
    //                          body invocation, ms
    //   'total_async_ms'     - summed time the flow spent gated on async, ms
    //   'flow_ticks'         - count / min / max / avg body time across
    //                          the whole flow, plus the name of the step
    //                          that ran the worst tick. Primary lens for
    //                          'what blocked the pump on this flow?'.
    //   'origin'             - flow launch source location (currently always blank)
    virtual void OnFlowEnded([[maybe_unused]] std::string_view               obj,
                             [[maybe_unused]] StepAction                     terminal_action,
                             [[maybe_unused]] int                            final_step_ordinal,
                             [[maybe_unused]] const std::vector<TraceEntry>& trace,
                             [[maybe_unused]] double wall_ms, [[maybe_unused]] double total_step_ms,
                             [[maybe_unused]] double                 total_async_ms,
                             [[maybe_unused]] const FlowTickSummary& flow_ticks,
                             [[maybe_unused]] const FlowStats&       stats,
                             [[maybe_unused]] FlowOrigin             origin)
    {
    }

    // ----- Cross-runtime traffic (Post / PostAndWait / Link) -----
    // Logging for these is mandatory in practice - a callback hopping pump
    // threads is exactly the kind of control flow that is invisible in a
    // stack trace, so every hook carries the caller's source location.

    // Fired when a callback is enqueued via Post / PostAndWait. Runs on the
    // CALLING thread, which may be any thread (another runtime's step, a
    // non-uniflow worker, main) - implementations MUST be thread-safe.
    //   'runtime_index' - index of the runtime the callback was posted to.
    //   'blocking'      - true for PostAndWait, false for Post.
    //   'caller'        - PostAt / PostAndWaitAt call site (blank if posted
    //                     through the bare Post / PostAndWait method).
    virtual void OnPostSubmitted([[maybe_unused]] int runtime_index, [[maybe_unused]] bool blocking,
                                 [[maybe_unused]] CallSite caller)
    {
    }

    // Fired on the pump thread right after a posted callback runs.
    //   'queue_wait_ms' - time the callback spent queued before the pump
    //                     picked it up.
    virtual void OnPostExecuted([[maybe_unused]] int runtime_index, [[maybe_unused]] bool blocking,
                                [[maybe_unused]] double   queue_wait_ms,
                                [[maybe_unused]] CallSite caller)
    {
    }

    // Fired right after Runtime::Link() parks 'linked's pump and hands its
    // objects to 'driver's pump. Runs on the thread that called Link.
    virtual void OnLinked([[maybe_unused]] int driver_index, [[maybe_unused]] int linked_index,
                          [[maybe_unused]] CallSite caller)
    {
    }
};

// Default observer - pretty-prints to stdout in fixed-width columns. Thread-
// safe: every Runtime's pump thread may call into it.
//
// Column layout (STEP rows):
//   [ obj          ] step                       description                     #s elapsed
//   [ UF_Stage        ] OnProcess_WaitHwReady      hw ready handshake settling     #04 elapsed=0.02ms
class ConsoleObserver : public IUniflowObserver
{
public:
    static constexpr int kColObj  = 14;
    static constexpr int kColStep = 28;
    static constexpr int kColDesc = 36;

    void OnFlowStarted(std::string_view obj, FlowOrigin origin) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] FLOW START";
        if (origin.file)
        {
            std::cout << "  caller=" << origin.file << ":" << origin.line;
            if (origin.function)
                std::cout << " " << origin.function << "()";
        }
        std::cout << "\n";
    }
    void OnStepChanged(std::string_view obj, std::string_view prev_step, std::string_view next_step,
                       std::string_view description, int step_ordinal, double elapsed_ms,
                       const TickStats& step_ticks) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::string                 transition(prev_step);
        if (!next_step.empty())
        {
            transition += " -> ";
            transition += std::string(next_step);
        }
        std::cout << "[" << pad(obj, kColObj) << "] " << pad(transition, kColStep) << " "
                  << pad(description, kColDesc) << " "
                  << "#" << pad2(step_ordinal) << " elapsed=" << fmt_ms(elapsed_ms) << "  tick x"
                  << step_ticks.count << " avg=" << fmt_ms(step_ticks.avg_ms)
                  << " min=" << fmt_ms(step_ticks.min_ms) << " max=" << fmt_ms(step_ticks.max_ms)
                  << "\n";
    }
    void OnStepThrew(std::string_view obj, std::string_view step, std::string_view what,
                     int step_ordinal, int /*tick*/) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] " << pad(step, kColStep) << " "
                  << "[THREW] " << what << " #" << pad2(step_ordinal) << "\n";
    }
    void OnAsyncSubmitted(std::string_view obj, std::string_view step,
                          std::string_view job) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] " << pad(step, kColStep) << " "
                  << "ASYNC SUBMIT  " << job << "\n";
    }
    void OnAsyncCompleted(std::string_view obj, std::string_view job, double wait_ms,
                          bool had_error, bool timed_out) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] " << pad("", kColStep) << " "
                  << "ASYNC DONE    " << job << "  wait=" << fmt_ms(wait_ms);
        if (timed_out)
            std::cout << "  [TIMEOUT]";
        else if (had_error)
            std::cout << "  [ERROR]";
        std::cout << "\n";
    }
    void OnSlowCpuStep(std::string_view obj, std::string_view step, double cpu_ms) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] " << pad(step, kColStep) << " "
                  << "[SLOW CPU]  step held pump for " << fmt_ms(cpu_ms) << "\n";
    }
    void OnSlowAsync(std::string_view obj, std::string_view job, double wait_so_far_ms) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] " << pad("", kColStep) << " "
                  << "[SLOW ASYNC]  " << job << "  pending=" << fmt_ms(wait_so_far_ms) << "\n";
    }
    void OnAsyncAbandoned(std::string_view obj, std::string_view job, double pending_ms) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] " << pad("", kColStep) << " "
                  << "[ASYNC ABANDONED]  " << job << "  pending=" << fmt_ms(pending_ms) << "\n";
    }
    void OnAsyncHighWater(std::string_view obj, std::string_view job, std::size_t inflight) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] " << pad("", kColStep) << " "
                  << "[ASYNC HIGHWATER]  " << job << " rejected, inflight=" << inflight << "\n";
    }
    void OnSlowRound(int runtime_index, const RoundProfile& profile) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(rt_label(runtime_index), kColObj) << "] "
                  << "[SLOW ROUND]  busy=" << fmt_ms(profile.busy_ms)
                  << "  segments=" << profile.segment_count;
        if (profile.segments.empty())
            std::cout << "  (enable round tracing for the per-segment breakdown)";
        std::cout << "\n";
        for (const RoundSegment& s : profile.segments)
            std::cout << "                 "
                      << pad(s.kind == RoundSegment::Kind::Step ? "Step" : "Post", 5) << " "
                      << pad(s.obj, kColObj) << " " << pad(s.label, kColStep) << " " << fmt_ms(s.ms)
                      << "\n";
    }
    void OnFlowEnded(std::string_view obj, StepAction terminal_action, int final_step_ordinal,
                     const std::vector<TraceEntry>& /*trace*/, double wall_ms, double total_step_ms,
                     double total_async_ms, const FlowTickSummary& flow_ticks,
                     const FlowStats& /*stats*/, FlowOrigin        origin) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] FLOW "
                  << (terminal_action == StepAction::Done ? "END  DONE  " : "END  FAIL  ")
                  << "steps=#" << pad2(final_step_ordinal) << "  wall=" << fmt_ms(wall_ms)
                  << "  step=" << fmt_ms(total_step_ms) << "  async=" << fmt_ms(total_async_ms)
                  << "  tick x" << flow_ticks.all.count << " avg=" << fmt_ms(flow_ticks.all.avg_ms)
                  << " min=" << fmt_ms(flow_ticks.all.min_ms)
                  << " max=" << fmt_ms(flow_ticks.all.max_ms);
        if (!flow_ticks.max_step.empty())
            std::cout << " (" << flow_ticks.max_step << ")";
        if (origin.file)
        {
            std::cout << "  caller=" << origin.file << ":" << origin.line;
            if (origin.function)
                std::cout << " " << origin.function << "()";
        }
        std::cout << "\n";
    }
    void OnPostSubmitted(int runtime_index, bool blocking, CallSite caller) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(rt_label(runtime_index), kColObj) << "] "
                  << pad(blocking ? "POSTWAIT SUBMIT" : "POST SUBMIT", kColStep) << " ";
        print_caller(caller);
        std::cout << "\n";
    }
    void OnPostExecuted(int runtime_index, bool blocking, double queue_wait_ms,
                        CallSite caller) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(rt_label(runtime_index), kColObj) << "] "
                  << pad(blocking ? "POSTWAIT RUN" : "POST RUN", kColStep) << " "
                  << "queue=" << fmt_ms(queue_wait_ms) << "  ";
        print_caller(caller);
        std::cout << "\n";
    }
    void OnLinked(int driver_index, int linked_index, CallSite caller) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(rt_label(driver_index), kColObj) << "] " << pad("LINK", kColStep)
                  << " "
                  << "rt#" << linked_index << " -> rt#" << driver_index << "  ";
        print_caller(caller);
        std::cout << "\n";
    }

private:
    static std::string rt_label(int idx)
    {
        std::ostringstream os;
        os << "rt#" << idx;
        return os.str();
    }
    // Caller-side of the lock: only invoked from the locked hooks above.
    static void print_caller(CallSite c)
    {
        if (c.file)
        {
            std::cout << "caller=" << c.file << ":" << c.line;
            if (c.function)
                std::cout << " " << c.function << "()";
        }
    }
    static std::string pad(std::string_view sv, int width)
    {
        std::string s(sv);
        if (static_cast<int>(s.size()) < width)
            s.append(width - s.size(), ' ');
        return s;
    }
    static std::string pad2(int v)
    {
        std::ostringstream os;
        os << std::setw(2) << std::setfill('0') << v;
        return os.str();
    }
    static std::string fmt_ms(double ms)
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(2) << ms << "ms";
        return os.str();
    }
    std::mutex mu_;
};

// ----- Config: per-Runtime tuning (pass via Runtime::Opts) -----
struct Config
{
    // Pump-thread rest period when NO flow is running anywhere on this
    // Runtime (every module is idle - Done / Fail terminated or never
    // started). Short by default so a fresh StartTask() is picked up
    // quickly without burning CPU.
    Duration idle_sleep_ms = std::chrono::milliseconds(1);

    // Pump-thread rest period when flows ARE running but every active
    // module returned Stay or was gated this round (no Next / Done /
    // Fail). Longer than idle_sleep_ms because steady-state polling rarely
    // needs sub-ms reaction - 20ms keeps CPU near zero while still
    // feeling responsive.
    Duration stay_sleep_ms = std::chrono::milliseconds(20);

    // Pump-thread rest period between rounds when at least one module
    // advanced this round (Next / Done / Fail). Default 0 = burst mode:
    // chains of Next steps run as fast as the pump can dispatch them.
    // Set to e.g. 1ms to trade a little latency for less CPU during
    // heavy bursts.
    Duration step_interval_sleep_ms = Duration::zero();

    // OnSlowCpuStep alarm threshold. If a single step body invocation
    // holds the pump thread for longer than this, OnSlowCpuStep fires
    // once for that invocation. Set to Duration::max() to disable.
    Duration slow_step_threshold_ms = std::chrono::milliseconds(10);

    // OnSlowAsync alarm threshold. Fires once per async job, the first
    // time the job's in-flight time crosses this. Default disabled - opt
    // in by setting a finite value when you actually care.
    Duration slow_async_threshold_ms = Duration::max();

    // OnSlowRound alarm threshold. A pump round (drain posts + run every
    // active module once) whose work time exceeds this fires OnSlowRound.
    // Default disabled. The per-segment breakdown in the report requires
    // round tracing to be on (see trace_rounds / Runtime::SetRoundTracing).
    Duration slow_round_threshold_ms = Duration::max();

    // Initial state of heavy per-round tracing: per-step / per-post timing
    // collected into the OnSlowRound breakdown. Off by default because it
    // allocates a small record per active segment each round. The cheap
    // round-duration stats (GetRoundStats) are collected regardless. Toggle
    // at runtime with Runtime::SetRoundTracing.
    bool trace_rounds = false;

    // Maximum async submissions in flight at once per flow. SubmitAsync past
    // this is rejected: it returns id 0 and fires OnAsyncHighWater instead of
    // enqueuing the job. 0 disables the cap. Guards a flow that fires async
    // without ever reading results (or clearing) from growing the slot list -
    // and the pool backlog behind it - without bound.
    std::size_t max_inflight_async = 64;
};

// ----- detail: hidden machinery -----
namespace detail
{

// Set once per pump thread to the Runtime's process-wide index; -1 on any
// other thread. Useful for user-side asserts that catch cross-runtime
// access (calling foo.something() from a step on a different Runtime is
// unsafe because foo is being driven by a different pump thread).
inline thread_local int t_runtime_idx = -1;

// Monotonic counter so each Runtime gets a stable distinct index.
inline std::atomic<int>& next_runtime_index()
{
    static std::atomic<int> n{0};
    return n;
}

// Uniflow<Derived> is a template, so a Runtime cannot hold "all modules"
// directly - each module is a different type. IUniflowObject is the
// non-template base they share, so the pump can drive them uniformly.
class IUniflowObject
{
public:
    virtual ~IUniflowObject()   = default;
    virtual bool IsIdle() const = 0;
    // Run one pump tick on this module. Returns true iff the step body
    // ran AND its result was a transition (Next / Done / Fail). Stay
    // results and gated-on-async both return false so the pump can decide
    // to sleep idle_sleep_ms / stay_sleep_ms when nothing advanced.
    // 'prof' (when non-null) collects this tick's step timing into the
    // current round's profile for OnSlowRound; null when round tracing is off.
    virtual bool ExecuteOnce(IUniflowObserver&, RoundProfile* prof) = 0;
};

// Forward-declared factory; defined at the bottom of this header, after the
// BS::thread_pool body, because the default executor wraps that pool.
std::unique_ptr<IExecutor> make_default_executor(std::size_t threads);

} // namespace detail

// ======================================================================
//  Runtime - user-owned, exactly one per pump thread.
//
//  Construction spins up the pump thread, wires up the executor (default:
//  BS::thread_pool with 'Opts::threads' workers), the observer (default:
//  ConsoleObserver), and the config. Destruction stops the pump and joins.
//
//  Modules attach to a Runtime via their 'Uniflow(Runtime&)' ctor; the same
//  Runtime drives every module attached to it.
//
//  Multi-runtime use:
//    - Create more than one 'Runtime' to get more than one pump thread.
//    - A module belongs to exactly one Runtime; directly touching another
//      runtime's module state from a step is unsafe (no locks between modules
//      on different pumps). Use 'RuntimeIndex()' / 'DriverIndex()' and
//      'detail::t_runtime_idx' for asserts.
//    - To reach state owned by another runtime safely, 'Post' a callback to
//      it (runs on its pump thread, no locks) or 'PostAndWait' for a result.
//    - If two runtimes end up sharing enough state that posting is not
//      enough, 'Link' one onto the other: a single pump thread then drives
//      both, restoring the lock-free invariant across them. One-way.
// ======================================================================
class Runtime
{
public:
    struct Opts
    {
        // Worker thread count for the default executor. 0 picks
        // hardware_concurrency. Ignored if 'executor' is set.
        std::size_t threads = 0;

        // Override the executor. If null, a BS::thread_pool with 'threads'
        // workers is constructed internally.
        std::unique_ptr<IExecutor> executor;

        // Override the observer. If null, a ConsoleObserver is used.
        std::unique_ptr<IUniflowObserver> observer;

        // Per-Runtime config.
        Config config{};
    };

    Runtime() : Runtime(Opts{}) {}
    explicit Runtime(Opts opts)
        : index_(detail::next_runtime_index().fetch_add(1)), config_(opts.config),
          executor_(opts.executor ? std::move(opts.executor)
                                  : detail::make_default_executor(opts.threads)),
          observer_(opts.observer ? std::move(opts.observer) : std::make_unique<ConsoleObserver>()),
          driver_index_(index_)
    {
        trace_rounds_.store(config_.trace_rounds, std::memory_order_relaxed);
        pump_ = std::thread([this] { pump_loop(); });
    }

    ~Runtime()
    {
        // Lifetime around linked-drive. If we were linked into a driver,
        // detach so its pump stops touching our now-dying objects. If we ARE
        // a driver, clear the back-pointer on each linked runtime so their
        // destructors do not call back into this freed object. Destroying a
        // driver while it still drives linked runtimes is a lifetime error -
        // those modules simply stop being pumped.
        if (driver_)
            driver_->unlink_(this);
        {
            std::lock_guard<std::mutex> lk(linked_mu_);
            for (Runtime* l : linked_)
                l->driver_ = nullptr;
        }
        stop_.store(true, std::memory_order_relaxed);
        signal_wake();
        if (pump_.joinable())
            pump_.join();
    }

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    int               index() const { return index_; }
    IExecutor&        executor() { return *executor_; }
    IUniflowObserver& observer() { return *observer_; }
    const Config&     config() const { return config_; }
    // Logical clock for this runtime's timers / StayUntil deadlines. Scale or
    // freeze it (rt.clock().SetScale(10), .Freeze(), .Resume()) to play the
    // whole flow back at a different rate or hold it. Bind a UFTimer to it
    // with UFTimer{rt.clock()}.
    VirtualClock& clock() { return clock_; }

    // -- Called by Uniflow base; could be friended, but the surface is
    //    small enough to keep public without leaking machinery to users --
    void attach(detail::IUniflowObject* m)
    {
        std::lock_guard<std::recursive_mutex> lk(objects_mu_);
        objects_.push_back(m);
    }
    void detach(detail::IUniflowObject* m)
    {
        std::lock_guard<std::recursive_mutex> lk(objects_mu_);
        objects_.erase(std::remove(objects_.begin(), objects_.end(), m), objects_.end());
    }

    // -- Cross-thread function posting --
    // Post a callback to run on THIS runtime's pump thread. Safe to call
    // from any thread: another runtime's step, a non-uniflow worker, the
    // main thread. The pump drains posted callbacks at the top of each round
    // and runs them on the pump thread, so they may freely touch state owned
    // by this runtime's modules WITHOUT locks - the single-thread invariant
    // still holds. Keep the callback short and non-blocking; it runs OUTSIDE
    // the step/flow model (no trace, no StepResult). For blocking work, have
    // the callback start a flow or submit to an executor.
    //
    // Prefer PostAt with a CallSite: it tags the call with its source location
    // so OnPostSubmitted / OnPostExecuted can report where the callback came
    // from. The bare Post posts with a blank call site.
    void Post(std::function<void()> fn) { PostAt(CallSite{}, std::move(fn)); }

    void PostAt(CallSite caller, std::function<void()> fn)
    {
        observer_->OnPostSubmitted(index_, /*blocking=*/false, caller);
        PostedTask t;
        t.fn           = std::move(fn);
        t.caller       = caller;
        t.blocking     = false;
        t.submitted_at = Clock::now();
        {
            std::lock_guard<std::mutex> lk(posted_mu_);
            posted_.push_back(std::move(t));
        }
        Wake(); // service the callback this round, not after the next nap
    }

    // Post a callback and block the CALLING thread until the pump runs it,
    // returning whatever the callback returns via std::future. Use this to
    // read or mutate state owned by this runtime from outside its pump
    // thread without locks. Prefer PostAndWaitAt with a CallSite for logging.
    //
    // MUST NOT be called from the thread that drives this runtime - the pump
    // would have to run the task while it is blocked waiting for the result
    // (deadlock); an assert guards exactly that case. Also avoid calling it
    // from a step on ANOTHER runtime: it blocks that runtime's pump (and
    // every module on it) until this pump services the task.
    template <class F>
    auto PostAndWait(F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>>
    {
        return PostAndWaitAt(CallSite{}, std::forward<F>(fn));
    }

    template <class F>
    auto PostAndWaitAt(CallSite caller, F&& fn)
        -> std::future<std::invoke_result_t<std::decay_t<F>>>
    {
        assert(detail::t_runtime_idx != driver_index_ &&
               "PostAndWait called from the pump thread that drives this "
               "runtime - would deadlock");
        using R             = std::invoke_result_t<std::decay_t<F>>;
        auto           task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        std::future<R> fut  = task->get_future();
        observer_->OnPostSubmitted(index_, /*blocking=*/true, caller);
        PostedTask t;
        t.fn           = [task]() { (*task)(); };
        t.caller       = caller;
        t.blocking     = true;
        t.submitted_at = Clock::now();
        {
            std::lock_guard<std::mutex> lk(posted_mu_);
            posted_.push_back(std::move(t));
        }
        Wake(); // service the callback this round, not after the next nap
        return fut;
    }

    // Force the pump out of its inter-round nap right now instead of waiting
    // up to stay_sleep_ms / idle_sleep_ms. Safe to call from any thread.
    // StartTask and Post already call this; call it yourself after mutating
    // module state through your own channel so an event-thread arrival is
    // serviced immediately rather than on the next scheduled poll. If this
    // runtime has been Link()ed into a driver, the driver's pump - the thread
    // actually sleeping on its behalf - is the one woken.
    void Wake()
    {
        Runtime* tgt = driver_ ? driver_ : this;
        tgt->signal_wake();
    }

    // -- Linked-drive: collapse another runtime onto THIS pump thread --
    // After Link(other), 'other' keeps its own observer / executor / config
    // and its own module list, but its pump thread is stopped and THIS pump
    // thread drives its objects (and drains its posted queue) every round.
    // The two runtimes then share one thread, so modules on either side may
    // touch shared state without locks. Per-module policy (slow thresholds,
    // observer, executor) stays each runtime's own; only the pump-sleep
    // cadence is governed by this (the driver) runtime's Config.
    //
    // One-way by design: there is no Unlink. Once flows on the two sides may
    // have grown cross-dependencies, independence cannot be safely re-
    // established, so the link is permanent for the lifetime of 'other'.
    //
    // Call from a thread that is NOT 'other's pump thread. Flat linking only:
    // 'other' must not already be linked, and neither runtime may itself be
    // driving others. Prefer LinkAt with a CallSite for caller logging.
    void Link(Runtime& other) { LinkAt(CallSite{}, other); }

    void LinkAt(CallSite caller, Runtime& other)
    {
        assert(&other != this && "Link: cannot link a runtime to itself");
        assert(other.driver_ == nullptr && "Link: 'other' is already linked");
        assert(driver_ == nullptr && "Link: a linked runtime cannot also be a driver");
        {
            std::lock_guard<std::mutex> lk(other.linked_mu_);
            assert(other.linked_.empty() && "Link: 'other' is itself a driver - flat linking only");
        }
        // Stop other's pump and wait for its current round to finish. join()
        // returning is the quiescence point: from here no thread drives
        // other's objects until this pump picks them up on the next round.
        other.stop_.store(true, std::memory_order_relaxed);
        other.signal_wake();
        if (other.pump_.joinable())
            other.pump_.join();
        other.driver_       = this;
        other.driver_index_ = index_;
        {
            std::lock_guard<std::mutex> lk(linked_mu_);
            linked_.push_back(&other);
        }
        observer_->OnLinked(index_, other.index_, caller);
    }

    // Effective driver-thread index: this runtime's own index normally, or
    // the driver's index after Link(). Compare against detail::t_runtime_idx
    // in user asserts to check "am I on the thread allowed to touch this
    // runtime's modules?".
    int driver_index() const { return driver_index_; }

    // -- Per-round (pump cycle) monitoring --
    // Toggle heavy per-round tracing: per-step / per-post timing captured into
    // the OnSlowRound breakdown. The cheap round-duration stats below are
    // collected either way; this only controls the per-segment detail. Safe
    // to flip from any thread at runtime.
    void SetRoundTracing(bool on) { trace_rounds_.store(on, std::memory_order_relaxed); }
    bool RoundTracingEnabled() const { return trace_rounds_.load(std::memory_order_relaxed); }

    // Snapshot of pump-round work-time stats (rounds that did work only;
    // idle polling rounds are excluded). Thread-safe.
    RoundStats GetRoundStats() const
    {
        std::lock_guard<std::mutex> lk(round_mu_);
        RoundStats                  s;
        s.count   = round_count_;
        s.min_ms  = round_count_ ? round_min_ : 0.0;
        s.max_ms  = round_count_ ? round_max_ : 0.0;
        s.avg_ms  = round_count_ ? round_sum_ / static_cast<double>(round_count_) : 0.0;
        s.last_ms = round_last_;
        return s;
    }

    // Clear the round stats - in particular resets the max peak. Thread-safe.
    void ResetRoundStats()
    {
        std::lock_guard<std::mutex> lk(round_mu_);
        round_count_ = 0;
        round_min_ = round_max_ = round_sum_ = round_last_ = 0.0;
    }

private:
    // Outcome of one pump round, in increasing order of 'how busy were
    // we?'. The round walks all modules and only ever UPGRADES this
    // value, never downgrades - so the final state reflects the busiest
    // module observed. Drives which sleep knob applies before the next
    // round.
    enum class RoundOutcome
    {
        Idle,
        Staying,
        Advanced
    };

    // Pump policy: each round, run every non-idle module once. Sleep
    // between rounds is picked by RoundOutcome:
    //   Advanced -> step_interval_sleep_ms (default 0 = burst; chains of
    //               Next run as fast as the pump can dispatch)
    //   Staying  -> stay_sleep_ms          (default 20ms; steady-state
    //               polling cadence while flows are running)
    //   Idle     -> idle_sleep_ms          (default 1ms; quick pickup of a
    //               fresh StartTask)
    // Any knob set to Duration::zero() skips sleep_for entirely.
    void pump_loop()
    {
        detail::t_runtime_idx = index_;
        for (;;)
        {
            if (stop_.load(std::memory_order_relaxed))
                return;
            RoundOutcome sleepLevel = RoundOutcome::Idle;

            // Per-round profiling. The busy timer wraps the work portion
            // (drains + module runs) only, never the inter-round sleep. The
            // per-segment sink is allocated only when heavy tracing is on; the
            // round-duration stats are recorded regardless.
            const bool   tracing = trace_rounds_.load(std::memory_order_relaxed);
            RoundProfile prof;
            prof.runtime_index       = index_;
            RoundProfile*   sink     = tracing ? &prof : nullptr;
            const TimePoint round_t0 = Clock::now();

            // Drain posted callbacks first - this runtime, then any linked
            // ones - so state they change is visible to the steps that run
            // this round. A drained batch counts as activity so the pump does
            // not fall back to the long idle nap right after doing work.
            if (DrainPosted(sink))
                sleepLevel = RoundOutcome::Advanced;
            {
                std::lock_guard<std::mutex> lk(linked_mu_);
                for (Runtime* l : linked_)
                    if (l->DrainPosted(sink))
                        sleepLevel = RoundOutcome::Advanced;
            }

            // Drive this runtime's objects, then any linked runtimes' objects
            // - all on this single pump thread. Each runtime uses its own
            // observer (RunObjectsOnce reads this->observer_).
            RunObjectsOnce(sleepLevel, sink);
            {
                std::lock_guard<std::mutex> lk(linked_mu_);
                for (Runtime* l : linked_)
                    l->RunObjectsOnce(sleepLevel, sink);
            }

            // Record round stats + slow-round alarm, but only for rounds that
            // did work; pure idle polling rounds (busy ~0) would swamp the
            // stats and mean nothing.
            if (sleepLevel != RoundOutcome::Idle)
                RecordRound(Clock::now() - round_t0, prof);

            // Sleep cadence is the driver runtime's Config; linked runtimes
            // contribute their busiest outcome via the shared sleepLevel but
            // not their sleep knobs.
            Duration nap_ms;
            switch (sleepLevel)
            {
            case RoundOutcome::Advanced:
                nap_ms = config_.step_interval_sleep_ms;
                break;
            case RoundOutcome::Staying:
                nap_ms = config_.stay_sleep_ms;
                break;
            case RoundOutcome::Idle:
                nap_ms = config_.idle_sleep_ms;
                break;
            }
            // Interruptible rest. Wake() (from StartTask / Post / a user
            // event thread) sets wake_requested_ and signals the cv, so a
            // freshly-arrived event is serviced this instant instead of
            // after the full nap. The predicate also covers stop_, so
            // teardown does not have to wait out the nap.
            if (nap_ms > Duration::zero())
            {
                std::unique_lock<std::mutex> lk(wake_mu_);
                wake_cv_.wait_for(
                    lk, nap_ms,
                    [this] { return stop_.load(std::memory_order_relaxed) || wake_requested_; });
                wake_requested_ = false;
            }
        }
    }

    // Run every non-idle object owned by THIS runtime once, folding the
    // busiest outcome into 'level' (never downgrades). Uses this runtime's
    // own observer so linked runtimes keep their own logging. Exactly one
    // thread ever calls this for a given runtime at a time: its own pump, or
    // - after Link() parked that pump - the driver's pump.
    void RunObjectsOnce(RoundOutcome& level, RoundProfile* prof)
    {
        std::lock_guard<std::recursive_mutex> lk(objects_mu_);
        for (std::size_t i = 0; i < objects_.size(); ++i)
        {
            detail::IUniflowObject* o = objects_[i];
            if (o->IsIdle())
                continue;
            if (level == RoundOutcome::Idle)
                level = RoundOutcome::Staying;
            if (o->ExecuteOnce(*observer_, prof))
                level = RoundOutcome::Advanced;
        }
    }

    // Run and clear every callback posted to THIS runtime. Swaps the queue
    // out under the lock and runs the batch outside it, so a callback may
    // Post() more work (lands in the next round) without re-entering the
    // lock, and a flood of posts cannot starve module stepping. Returns true
    // iff at least one callback ran.
    bool DrainPosted(RoundProfile* prof)
    {
        std::deque<PostedTask> batch;
        {
            std::lock_guard<std::mutex> lk(posted_mu_);
            if (posted_.empty())
                return false;
            batch.swap(posted_);
        }
        for (auto& t : batch)
        {
            double          wait_ms = to_ms(Clock::now() - t.submitted_at);
            const TimePoint run_t0  = Clock::now();
            t.fn();
            Clock::duration run_dt = Clock::now() - run_t0;
            observer_->OnPostExecuted(index_, t.blocking, wait_ms, t.caller);
            if (prof)
            {
                RoundSegment seg;
                seg.kind  = RoundSegment::Kind::Post;
                seg.obj   = "rt#" + std::to_string(index_);
                seg.label = uniflow::to_string(t.caller);
                seg.ms    = to_ms(run_dt);
                prof->segments.push_back(std::move(seg));
            }
        }
        return true;
    }

    // Fold one completed work round into the stats and, if it crossed the
    // slow-round threshold, emit OnSlowRound with the profile (segments only
    // populated when tracing was on). Stats use a first-sample init so no
    // <limits> sentinel is needed.
    void RecordRound(Clock::duration dt, RoundProfile& prof)
    {
        double ms = to_ms(dt);
        {
            std::lock_guard<std::mutex> lk(round_mu_);
            if (round_count_ == 0)
            {
                round_min_ = ms;
                round_max_ = ms;
            }
            else
            {
                if (ms < round_min_)
                    round_min_ = ms;
                if (ms > round_max_)
                    round_max_ = ms;
            }
            ++round_count_;
            round_sum_ += ms;
            round_last_ = ms;
        }
        if (config_.slow_round_threshold_ms != Duration::max() &&
            dt > config_.slow_round_threshold_ms)
        {
            prof.busy_ms       = ms;
            prof.segment_count = static_cast<int>(prof.segments.size());
            observer_->OnSlowRound(index_, prof);
        }
    }

    // Remove 'other' from this driver's linked list (called by other's
    // destructor). Not an Unlink in the logical sense - it only severs the
    // dangling pointer when a linked runtime dies before its driver.
    void unlink_(Runtime* other)
    {
        std::lock_guard<std::mutex> lk(linked_mu_);
        linked_.erase(std::remove(linked_.begin(), linked_.end(), other), linked_.end());
    }

    // Raise the wake flag under wake_mu_ and signal the cv. Holding the lock
    // while setting the flag closes the lost-wakeup window: the pump checks
    // the same flag under the same lock before sleeping. Always targets THIS
    // runtime's pump - callers route to the driver via Wake() when linked.
    void signal_wake()
    {
        {
            std::lock_guard<std::mutex> lk(wake_mu_);
            wake_requested_ = true;
        }
        wake_cv_.notify_one();
    }

    int          index_;
    Config       config_;
    VirtualClock clock_; // logical time for timers / StayUntil

    // Interruptible inter-round sleep. The pump waits on wake_cv_ for nap_ms;
    // signal_wake() (via Wake() / stop / an async worker finishing) raises
    // wake_requested_ and notifies so fresh work runs without waiting out the
    // nap. Declared before executor_/observer_/pump_ so it OUTLIVES them: an
    // async worker may call Wake() as it completes, and the executor's
    // destructor joins that worker while these members are still alive.
    std::mutex              wake_mu_;
    std::condition_variable wake_cv_;
    bool                    wake_requested_ = false;

    std::unique_ptr<IExecutor>        executor_;
    std::unique_ptr<IUniflowObserver> observer_;

    std::recursive_mutex                 objects_mu_;
    std::vector<detail::IUniflowObject*> objects_;
    std::atomic<bool>                    stop_{false};

    // Cross-thread posted callbacks, run on this pump thread each round. Each
    // carries its caller's source location and submit time so the observer
    // can report where it came from and how long it queued.
    struct PostedTask
    {
        std::function<void()> fn;
        CallSite              caller;
        bool                  blocking = false;
        TimePoint             submitted_at;
    };
    std::mutex             posted_mu_;
    std::deque<PostedTask> posted_;

    // Linked-drive. linked_ = runtimes THIS pump also drives. driver_ is set
    // (back-pointer) when this runtime has been Link()ed into another;
    // driver_index_ is the index of the thread that drives our objects (our
    // own index when standalone, the driver's index once linked).
    std::mutex            linked_mu_;
    std::vector<Runtime*> linked_;
    Runtime*              driver_ = nullptr;
    int                   driver_index_;

    // Per-round (pump cycle) monitoring. trace_rounds_ gates the heavy
    // per-segment capture; the duration stats below are always collected
    // (under round_mu_) for rounds that did work.
    std::atomic<bool>  trace_rounds_{false};
    mutable std::mutex round_mu_;
    std::size_t        round_count_ = 0;
    double             round_min_   = 0.0;
    double             round_max_   = 0.0;
    double             round_sum_   = 0.0;
    double             round_last_  = 0.0;

    std::thread pump_;
};

// Two ways to wait in a step:
//   (1) SubmitAsync: offload blocking work to the pool; pump is woken on
//       completion. Best for I/O, CPU work.
//   (2) UFTimer: poll a condition. Best for HW flags or peer state where
//       there is no completion callback. HeldFor answers "has the condition
//       STAYED true long enough?" (settling); Elapsed / Passed answer the
//       raw "how long since armed?" question.
class UFTimer
{
public:
    // A standalone timer reads the process real clock; bind to a Runtime's
    // clock (UFTimer{rt.clock()}) to follow its scale / freeze.
    UFTimer() : clk_(&RealClock()), armed_at_(clk_->Now()) {}
    explicit UFTimer(const VirtualClock& clk) : clk_(&clk), armed_at_(clk.Now()) {}

    // Re-arm both the raw stopwatch and the HeldFor sustain accumulator.
    void Restart()
    {
        armed_at_   = clk_->Now();
        cond_since_ = TimePoint{};
    }
    // Native clock units (nanoseconds on most platforms). Compared against
    // a Duration (ms) deadline works because common_type promotes ms to ns.
    Clock::duration Elapsed() const { return clk_->Now() - armed_at_; }
    bool            Passed(Duration duration) const { return Elapsed() >= duration; }

    // Returns true once 'condition' has held continuously for 'duration' since
    // it first turned true; any tick the condition reads false resets the
    // accumulator and returns false. Once satisfied it keeps returning true
    // while the condition stays true (level semantics). Use for debounce /
    // settling ("input X stable for N ms"), NOT for a one-shot deadline -
    // that is the step timeout (see the Stay timeout discussion).
    template <class Predicate, std::enable_if_t<std::is_invocable_r_v<bool, Predicate>, int> = 0>
    bool HeldFor(Predicate condition, Duration duration)
    {
        return HeldFor(static_cast<bool>(condition()), duration);
    }
    bool HeldFor(bool condition, Duration duration)
    {
        if (!condition)
        {
            cond_since_ = TimePoint{};
            return false;
        }
        if (cond_since_ == TimePoint{})
            cond_since_ = clk_->Now();
        return clk_->Now() - cond_since_ >= duration;
    }

private:
    const VirtualClock* clk_; // time source (process real clock by default)
    TimePoint           armed_at_;
    // Epoch == "condition is not currently held"; otherwise the moment it
    // last turned true. Kept separate from armed_at_ so HeldFor and
    // Elapsed/Passed do not clobber each other.
    TimePoint cond_since_{};
};

// What a step returns: an intent, not a state change. A flow-independent type
// (it depends on nothing in Derived), so a step's return type is simply
// 'uniflow::StepResult' rather than nested under the flow class. Uniflow
// re-exports it as a member alias 'StepResult'.
struct StepResult
{
    StepAction action = StepAction::Stay;
    // Next: the step to advance to. StayUntil: the timeout target to route to
    // when the step deadline passes. Unused otherwise.
    std::function<StepResult()> next_fn;
    const char*                 next_name = nullptr; // name of next_fn for the log
    // StayUntil only: deadline measured from step entry. Zero (the default)
    // means "plain Stay, no step timeout".
    Duration timeout = Duration::zero();
    // StayUntil with a wait condition (post-wait / settling): cond is polled
    // each round and, once it has stayed true continuously for 'settle', the
    // step transitions to success_fn BEFORE the timeout. next_fn stays the
    // timeout target. An empty cond means the classic timeout-only Stay.
    std::function<bool()>       cond;
    Duration                    settle = Duration::zero();
    std::function<StepResult()> success_fn;
    const char*                 success_name = nullptr;
};

// ----- Task-Level Syntax: the per-task bookkeeping base -----
//
// A unit operation ("task") groups several steps that share state. The user
// declares it as a struct deriving from uniflow::Task<Flow> (defined below,
// after Uniflow) - which OWNS that shared state AND the step member functions -
// and holds ONE instance on the flow. TaskBase is the non-template base of
// Task<Flow> that carries the framework bookkeeping every task gets for free:
// the task name (Name()), the wall time since the task was entered (Elapsed()),
// and the ordered list of steps visited within the task (Trajectory()). OnEnter
// is called once each time the task is entered, before its first step - reset
// per-task members there.
class TaskBase
{
public:
    virtual ~TaskBase() = default;

    // One finished step within the unit: which step, how long it held (wall
    // time from entry to transition, summed over its Stay re-entries), and how
    // many times its body ran (ticks). Recorded when the step transitions away.
    struct StepVisit
    {
        const char* name  = nullptr; // step name
        double      ms    = 0.0;     // wall time spent in the step
        int         ticks = 0;       // body invocations (Stay re-entries + final)
    };

    // Override to reset this unit's own members when the unit is (re-)entered.
    virtual void OnEnter() {}

    const std::string&            Name() const { return uf_name_; }
    Clock::duration               Elapsed() const { return uf_timer_.Elapsed(); }
    const std::vector<StepVisit>& Trajectory() const { return uf_trajectory_; }

    // -- framework-internal: driven by Uniflow --
    // Called once when the task is entered (StartTask / StartFlow armed it, or
    // an in-task StartTask switched to it). Resets the per-task clock and
    // trajectory, then runs the user OnEnter.
    void uf_enter_unit_()
    {
        uf_ensure_name_();
        uf_timer_.Restart();
        uf_trajectory_.clear();
        OnEnter();
    }
    // Cache the demangled task name (normally done on entry); lets the module
    // answer CurrentTaskName() the instant a task is armed, before its first
    // pump round runs uf_enter_unit_.
    const std::string& uf_ensure_name_()
    {
        if (uf_name_.empty())
            uf_name_ = uf_short_type_(typeid(*this).name());
        return uf_name_;
    }
    // Called once when a step OF THIS TASK transitions away, with its totals.
    void uf_record_step_(const char* name, double ms, int ticks)
    {
        uf_trajectory_.push_back(StepVisit{name, ms, ticks});
    }

private:
    // typeid().name() is "struct Flow::Task_Pick" on MSVC; keep the last
    // identifier ("Task_Pick"). Other compilers may mangle - the name is a
    // convenience, not a contract.
    static std::string uf_short_type_(const char* raw)
    {
        std::string s(raw ? raw : "");
        std::size_t pos = s.find_last_of(": ");
        return pos == std::string::npos ? s : s.substr(pos + 1);
    }
    std::string            uf_name_;
    UFTimer                uf_timer_;
    std::vector<StepVisit> uf_trajectory_;
};

// Uniflow<Derived> - CRTP base. A module is 'class Foo : public Uniflow<Foo>'
// with a ctor taking 'Runtime&' and passing a name to the base. Its steps are
// member functions returning StepResult; flows are held as members of your own
// App and reached via 'App::inst()' (no framework-side singleton).
template <class Derived> class Uniflow : public detail::IUniflowObject
{
public:
    // The step-intent type, shared across all flows (defined above at namespace
    // scope). Re-exported as a member so 'Foo::StepResult' keeps working.
    using StepResult = ::uniflow::StepResult;

    // -- Constructors --
    //   Uniflow(rt)         anonymous (named "Uniflow")
    //   Uniflow(rt, "name") named (what every module should use)
    // The base attaches the module to 'rt' and starts driving it on the
    // next pump round (initially idle - no task until StartTask()).
    explicit Uniflow(Runtime& rt) : Uniflow(rt, nullptr) {}
    Uniflow(Runtime& rt, const char* name) : runtime_(&rt), uf_step_timer_(rt.clock())
    {
        instance_name_ = name ? name : "Uniflow";
        uf_auto_timers_.push_back(&uf_step_timer_);
        runtime_->attach(this);
    }

    ~Uniflow() override
    {
        // Destroying a module mid-flow is a use-after-free hazard (the pump
        // may be in its step). Contract: destroy modules only when idle.
        assert(!flow_running_ && "uniflow: module destroyed while a flow runs");
        if (runtime_)
            runtime_->detach(this);
    }

    Uniflow(const Uniflow&)            = delete;
    Uniflow& operator=(const Uniflow&) = delete;

    // -- IUniflowObject --
    bool IsIdle() const override
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        return !flow_running_;
    }
    bool ExecuteOnce(IUniflowObserver& obs, RoundProfile* prof) override;

    // -- Public control surface --

    // Bind a task to this flow. Call once per task from the module constructor
    // (typically one line per task). It wires the task's flow() back-pointer so
    // the task's steps can reach this flow and so task.StartFlow() knows which
    // module to launch. Does NOT start anything.
    //
    //   AddTask(task_pick_);
    //   AddTask(task_place_);
    template <class TaskT>
    void AddTask(TaskT& task)
    {
        static_assert(std::is_base_of_v<TaskBase, TaskT>,
                      "AddTask: task must derive from uniflow::Task<Flow>");
        task.uf_bind_(static_cast<Derived*>(this));
    }

    // Run a task: launch this module's flow at 'task's Entry() step. Callable
    // from ANY thread - a peer, an event thread, the orchestrator - the arm is
    // serialised against the pump. Equivalent to task.StartFlow(). A module runs
    // one task at a time, so:
    //   - StartResult::Ok   - the task was launched.
    //   - StartResult::Busy - a task is already running; nothing happened.
    //                         (Poll IsIdle() and retry, the usual pattern.)
    template <class TaskT>
    StartResult StartTask(TaskT& task)
    {
        static_assert(std::is_base_of_v<TaskBase, TaskT>,
                      "StartTask: task must derive from uniflow::Task<Flow>");
        return arm_flow_(&task) ? StartResult::Ok : StartResult::Busy;
    }

private:
    // Shared flow-arm: install 'task's Entry() as the first step and reset all
    // per-flow accounting. Returns false (no-op) if a task is already running.
    // The entry thunk enters the unit (OnEnter) on the PUMP thread, not here -
    // arm may be called from any thread.
    template <class TaskT>
    bool arm_flow_(TaskT* task)
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        if (flow_running_)
            return false;

        Uniflow* self = this;
        curr_fn_      = [self, task]() -> StepResult
        {
            self->uf_begin_unit_(task);
            return task->Entry();
        };
        auto now        = Clock::now();
        flow_running_   = true;
        curr_step_name_ = "Entry";
        curr_step_description_.clear();
        step_ordinal_       = 0;
        tick_count_         = 0;
        ticks_in_step_      = 0;
        flow_started_at_    = now;
        step_started_at_    = now;
        step_started_at_v_  = runtime_->clock().Now();
        total_cpu_          = {};
        total_async_        = {};
        step_tick_min_      = Clock::duration::max();
        step_tick_max_      = Clock::duration::zero();
        step_tick_sum_      = Clock::duration::zero();
        flow_tick_min_      = Clock::duration::max();
        flow_tick_max_      = Clock::duration::zero();
        flow_tick_max_step_ = nullptr;
        async_jobs_.clear();
        next_async_id_  = 1;
        uf_active_unit_ = nullptr;
        uf_current_task_ = task;
        task->uf_ensure_name_();
        trace_.clear();
        flow_started_pending_ = true;
        flow_origin_          = FlowOrigin{};
        // The first step is a step change too: arm the auto-reset timers so the
        // entry step sees fresh timers, not time accrued since construction.
        uf_reset_step_timers_();
        // Nudge the pump so the entry step runs on the next round instead of
        // after the current nap - the whole point of launching a task from an
        // event thread is a prompt first step.
        runtime_->Wake();
        return true;
    }

    // Re-arm every auto-reset timer and clear the StayUntil settle accumulator.
    // Called on each step change (Next / StayUntil timeout / task switch / flow
    // start) but NOT on a Stay.
    void uf_reset_step_timers_()
    {
        for (UFTimer* t : uf_auto_timers_)
            t->Restart();
        uf_settle_since_ = TimePoint{};
    }

public:
    // Block the calling thread until the running flow finishes (returns at
    // once if already idle). Call from the OWNING thread - never from inside
    // a step.
    void WaitUntilIdle()
    {
        std::unique_lock<std::mutex> lk(op_mu_);
        idle_cv_.wait(lk, [this] { return !flow_running_; });
    }

    const std::string& InstanceName() const { return instance_name_; }
    int                RuntimeIndex() const { return runtime_->index(); }
    // Index of the thread that drives this module (own runtime normally, the
    // driver's after Runtime::Link()). Compare to detail::t_runtime_idx in
    // asserts that guard against off-thread access.
    int                DriverIndex() const { return runtime_->driver_index(); }
    const std::string& CurrentStepDescription() const { return curr_step_description_; }
    // Name of the step currently running ("Entry" before the first Next,
    // empty when idle). Copied under the op lock so it does not tear against
    // the pump rewriting it on a transition.
    std::string CurrentStepName() const
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        return curr_step_name_ ? std::string(curr_step_name_) : std::string();
    }
    // 0-based index of the current step within the running flow (0 = entry
    // step); -1 when idle.
    int CurrentStepOrdinal() const
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        return flow_running_ ? step_ordinal_ : -1;
    }
    // Class name of the task currently running ("Task_Pick" - the demangled
    // short type), empty when idle. Tracks an in-task StartTask switch.
    std::string CurrentTaskName() const
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        return (flow_running_ && uf_current_task_) ? uf_current_task_->Name()
                                                   : std::string();
    }

    // The module's built-in per-step timer: re-armed on every step change (but
    // not on a Stay). Reach it from a step via flow().StepTimer(); use Passed /
    // Elapsed / HeldFor on it the same way as any UFTimer. (TaskBase::Elapsed
    // is a separate, per-task stopwatch reset on task entry, not per step.)
    UFTimer&       StepTimer() { return uf_step_timer_; }
    const UFTimer& StepTimer() const { return uf_step_timer_; }

    // Create a UFTimer bound to this runtime's clock and register it for
    // auto-reset: it re-arms on every step change, like the built-in StepTimer.
    // The module owns it (the reference stays valid for the module's lifetime);
    // grab it once and store the reference/pointer. For a self-managed timer,
    // construct a UFTimer{Clock()} you reset yourself instead.
    UFTimer& NewAutoTimer()
    {
        uf_owned_timers_.emplace_back(runtime_->clock());
        uf_auto_timers_.push_back(&uf_owned_timers_.back());
        return uf_owned_timers_.back();
    }

    // The runtime's logical clock (scale / freeze), for building your own timers.
    const VirtualClock& Clock() const { return runtime_->clock(); }

protected:
    // -- Step intents at module scope. Steps live on tasks (uniflow::Task<Flow>)
    //    and get the full set - Stay / Next / Done / Fail / StayUntil / StayTimeout - from
    //    there; these three are kept here so a module helper can also end a flow
    //    if it ever needs to. --
    // Stay() - re-run this step on the next pump round (Config::stay_sleep_ms
    //          cadence). Done() - flow finished normally; module goes idle.
    // Fail() - flow aborted; module goes idle.
    StepResult Stay() { return {StepAction::Stay, {}, nullptr}; }
    StepResult Done() { return {StepAction::Done, {}, nullptr}; }
    StepResult Fail() { return {StepAction::Fail, {}, nullptr}; }

    // Per-module exception policy. Override in Derived to control what
    // happens when a step body throws:
    //   return false (default) - rethrow (std::terminate); fail-fast crash.
    //   return true             - log via OnStepThrew, end the flow as Fail,
    //                             pump survives.
    bool CatchStepExceptions() const { return false; }

    // Attach a human-readable description to the current step. Tasks call this
    // through Task::Describe (which forwards here); it is also callable from a
    // module method that wants to annotate the running step.
    template <class... Args> void Describe(Args&&... args)
    {
        std::ostringstream os;
        (os << ... << std::forward<Args>(args));
        curr_step_description_ = os.str();
    }

    // Submit a static (or free) function to the Runtime's executor. The worker
    // is named by a function pointer plus its log label; 'timeout' of
    // Duration::max() (the default) means no deadline. The UF_FN(fn) helper
    // supplies the pointer + label. Returns an AsyncId for the submission, or 0
    // if it was rejected (Config::max_inflight_async reached - OnAsyncHighWater
    // fires). Carry the id to the step that reads the result and pass it to
    // AsyncResult<T>(id). Any later step may read it - several jobs can be in
    // flight at once. Passing a pointer/reference argument is allowed (the
    // pointer value is copied in), but the pointee's lifetime and thread-safety
    // across the worker thread are the caller's responsibility:
    //
    //   AsyncId id = SubmitAsync(UF_FN(SimulateStartCmd));
    template <class R, class... Params, class... Args>
    [[nodiscard]] AsyncId SubmitAsync(R (*fn)(Params...), const char* job_label,
                                      Duration timeout = Duration::max(), Args&&... args);

    // Read the result of submission 'id'. Returns an AsyncOutcome<T> by value:
    // NotFound (bad / cleared / 0 id), Pending (still in flight), Done (value
    // engaged), Failed, or TimedOut. Safe to call every round - a Pending
    // result is the cue to Stay() and poll again.
    template <class T> AsyncOutcome<T> AsyncResult(AsyncId id)
    {
        AsyncOutcome<T> out;
        for (AsyncSlot& s : async_jobs_)
        {
            if (s.id != id)
                continue;
            if (!s.done)
            {
                out.state = AsyncOutcome<T>::State::Pending;
            }
            else if (s.timed_out)
            {
                out.state = AsyncOutcome<T>::State::TimedOut;
            }
            else if (s.exc)
            {
                out.state = AsyncOutcome<T>::State::Failed;
            }
            else
            {
                out.state = AsyncOutcome<T>::State::Done;
                // Pointer-form any_cast: a mismatched T (the caller named the
                // wrong result type) leaves 'value' disengaged rather than
                // throwing or dereferencing null, so ok() reads false.
                if (T* p = std::any_cast<T>(&s.value))
                    out.return_value = *p;
            }
            return out;
        }
        return out; // NotFound
    }

    // True while any submission for this flow is still in flight (not yet
    // resolved by the pump sweep). The join-all primitive: a finishing step
    // polls this and Stay()s - pair with StayUntil for a deadline - until it
    // reads false, then harvests each result with AsyncResult<T>(id).
    bool AnyAsyncPending() const
    {
        for (const AsyncSlot& s : async_jobs_)
            if (!s.done)
                return true;
        return false;
    }

    // Drop every async slot for this flow. Submissions still in flight are
    // ABANDONED: the worker keeps running on the pool until it finishes
    // naturally (its result is discarded), and OnAsyncAbandoned fires once per
    // abandoned worker so the leak is visible. Resolved slots are dropped
    // quietly. Call it when a flow gives up waiting (e.g. after a StayUntil
    // timeout) and moves on to other work.
    void ClearAsync() { uf_drop_async_slots_(); }

private:
    // Tasks reach module internals (Describe / SubmitAsync / AsyncResult / the
    // arm + unit-switch helpers) through this friendship.
    template <class> friend class Task;

    // Enter 'unit' as the active task if it is not already: rebind the active
    // pointer and run its OnEnter / re-arm. Idempotent so a first step that
    // Stay-polls (the entry thunk re-running) does not re-enter the unit every
    // round. Runs on the PUMP thread (called from the entry / switch thunk), so
    // user OnEnter code sees the single-thread invariant.
    void uf_begin_unit_(TaskBase* unit)
    {
        if (uf_active_unit_ != unit)
        {
            uf_active_unit_ = unit;
            uf_current_task_ = unit;
            unit->uf_enter_unit_();
        }
    }

    // Drop every async slot, warning (OnAsyncAbandoned) for each worker still
    // in flight. Shared by ClearAsync and flow teardown. Workers hold their own
    // promise, so an abandoned one finishes harmlessly into a dropped future.
    void uf_drop_async_slots_()
    {
        for (AsyncSlot& s : async_jobs_)
        {
            if (!s.done)
                runtime_->observer().OnAsyncAbandoned(instance_name_, s.label ? s.label : "",
                                                      to_ms(Clock::now() - s.submitted_at));
        }
        async_jobs_.clear();
    }

    // Reset to idle once a flow reaches Done / Fail. Peers polling for
    // this module's idleness pick the transition up on their next pump
    // round (within stay_sleep_ms / idle_sleep_ms).
    void ClearFlow()
    {
        flow_running_   = false;
        curr_fn_        = nullptr;
        curr_step_name_ = nullptr;
        curr_step_description_.clear();
        step_ordinal_       = 0;
        tick_count_         = 0;
        ticks_in_step_      = 0;
        step_tick_min_      = Clock::duration::max();
        step_tick_max_      = Clock::duration::zero();
        step_tick_sum_      = Clock::duration::zero();
        flow_tick_min_      = Clock::duration::max();
        flow_tick_max_      = Clock::duration::zero();
        flow_tick_max_step_ = nullptr;
        uf_drop_async_slots_();
        next_async_id_  = 1;
        flow_origin_    = FlowOrigin{};
        uf_active_unit_ = nullptr;
        uf_current_task_ = nullptr;
    }

    std::string instance_name_;
    Runtime*    runtime_ = nullptr;

    // Serialises external StartTask()/WaitUntilIdle() against pump ExecuteOnce().
    mutable std::mutex      op_mu_;
    std::condition_variable idle_cv_;

    // -- Current position within the running flow --
    bool        flow_running_   = false;
    const char* curr_step_name_ = nullptr;
    std::string curr_step_description_;
    int         step_ordinal_      = 0;
    int         tick_count_        = 0; // total body invocations in flow
    int         ticks_in_step_     = 0; // body invocations in current step
    TimePoint   flow_started_at_   = {};
    TimePoint   step_started_at_   = {}; // wall-clock start of current step
    TimePoint   step_started_at_v_ = {}; // virtual-clock start (StayUntil deadline)
    // Internal accumulators use the clock's native precision (typically
    // ns) so OnFlowEnded's step= / async= totals don't round to 0ms when
    // every step body finishes in microseconds.
    Clock::duration total_cpu_   = {};
    Clock::duration total_async_ = {};
    // Tick-time stats. Per-step (reset on each Next/Done/Fail transition)
    // drives OnStepChanged; per-flow (kept until ClearFlow) drives
    // FlowTickSummary in OnFlowEnded. min_ uses Clock::duration::max() as
    // its 'no samples yet' sentinel.
    Clock::duration step_tick_min_        = Clock::duration::max();
    Clock::duration step_tick_max_        = Clock::duration::zero();
    Clock::duration step_tick_sum_        = Clock::duration::zero();
    Clock::duration flow_tick_min_        = Clock::duration::max();
    Clock::duration flow_tick_max_        = Clock::duration::zero();
    const char*     flow_tick_max_step_   = nullptr; // step name owning worst tick
    bool            flow_started_pending_ = false;
    FlowOrigin      flow_origin_          = {};

    std::function<StepResult()> curr_fn_;

    // -- Built-in per-step timer + the auto-reset registry --
    // uf_step_timer_ is the module's built-in timer, re-armed on every step
    // change (Next / StayUntil timeout / task switch / flow start) but NOT on a
    // Stay; reach it from a step via flow().StepTimer(). uf_owned_timers_ holds
    // the timers handed out by NewAutoTimer() (a deque so their addresses stay
    // stable); uf_auto_timers_ is the reset list (the built-in plus every owned
    // one). uf_settle_since_ is the StayUntil-with-condition settle accumulator
    // (epoch == condition not currently held), reset on each step change.
    UFTimer                  uf_step_timer_;
    std::deque<UFTimer>      uf_owned_timers_;
    std::vector<UFTimer*>    uf_auto_timers_;
    TimePoint                uf_settle_since_ = {};

    // The unit context currently active, for unit-entry detection (a threaded
    // context whose address differs means we crossed a boundary) and for
    // recording each step's visit into its Trajectory() on transition.
    TaskBase* uf_active_unit_ = nullptr;
    // The task the running flow was armed with: set at arm (and on an in-task
    // switch) so CurrentTaskName() is answerable immediately; null when idle.
    TaskBase* uf_current_task_ = nullptr;

    // -- Async slots: every in-flight / just-resolved submission for this flow.
    //    A deque keeps slot addresses stable as submissions are appended. The
    //    pump sweeps it each round (ExecuteOnce) to resolve completions, fire
    //    observer hooks, and arm the slow-async alarm; steps read results by id
    //    via AsyncResult<T>(id). Ids are monotonic within a flow; reset to 1 at
    //    flow end so logs restart per flow. --
    std::deque<AsyncSlot> async_jobs_;
    AsyncId               next_async_id_ = 1;

    // -- Trace + cross-flow stats handed to OnFlowEnded --
    std::vector<TraceEntry> trace_;
    FlowStats               stats_;
};

// ----- Task<Flow> - the per-task base a user task derives from -----
//
// A task OWNS the state its steps share AND the step member functions. Deriving
// from Task<Flow> gives a step everything it needs without a context argument:
//   - flow()                  - the parent flow, by typed reference. Because the
//                               task is a nested type of the flow, flow() reaches
//                               the flow's private members too (flow().x_).
//   - Next(UF_FN(Step))       - advance to a sibling step of THIS task.
//   - Stay() / StayTimeout(...) / StayUntil(...) - poll this step (with a deadline).
//   - Done() / Fail()         - end the flow (the task is the whole flow here).
//   - StartTask(other)        - switch the flow to ANOTHER task (Next never
//                               leaves the current task).
//   - Describe(...) / SubmitAsync(...) / AsyncResult<T>(id) - forwarded to flow().
//
// Override Entry() to name the task's first step (return it directly). Launch a
// task with task.StartFlow() or module.StartTask(task); a module runs one task at
// a time. AddTask(task) in the flow constructor wires flow() once.
//
//   struct Task_Pick : uniflow::Task<Flow_LoadPicker> {
//       StepResult Entry() override { return Step_CmdMoveToSource(); }
//       StepResult Step_CmdMoveToSource() {
//           flow().x_->Move(...);                 // parent flow state
//           return Next(UF_FN(Step_WaitAtSource));
//       }
//       StepResult Step_WaitAtSource() { ... }
//   } task_pick_;
template <class FlowT> class Task : public TaskBase
{
public:
    using StepResult = ::uniflow::StepResult;

    // The parent flow, by typed reference. Wired by Uniflow::AddTask before any
    // step runs; a step reaches sibling tasks (flow().task_other_), the flow's
    // own state (flow().x_), and its peers through it.
    FlowT&       flow() { return *flow_; }
    const FlowT& flow() const { return *flow_; }

    // The task's first step. Override to return it directly:
    //   StepResult Entry() override { return Step_First(); }
    virtual StepResult Entry() = 0;

    // Launch the parent flow at this task's Entry(). Callable from ANY thread;
    // equivalent to module.StartTask(*this). Ok if launched, Busy if a task is
    // already running on the module.
    StartResult StartFlow() { return flow_->StartTask(*this); }

    // -- framework-internal: AddTask wires this once --
    void uf_bind_(FlowT* flow) { flow_ = flow; }

protected:
    // Re-run this step next pump round.
    StepResult Stay() const { return {StepAction::Stay, {}, nullptr}; }
    // End the flow (normal / aborted); the module goes idle.
    StepResult Done() const { return {StepAction::Done, {}, nullptr}; }
    StepResult Fail() const { return {StepAction::Fail, {}, nullptr}; }

    // Advance to step 'fn' of THIS task on the next pump round. 'Self' is the
    // concrete task type, deduced from the member pointer; UF_FN(fn) supplies
    // '&Self::fn, "fn"', so the call reads as a plain function name:
    //
    //   return Next(UF_FN(Step_WaitAtSource));
    //
    // A step may take arguments; pass them after the name and they are bound
    // (by value, decayed to the parameter types) into the next-step thunk. The
    // usual use is carrying an AsyncId to the step that reads its result, so
    // the value lives on the call chain instead of in a task member:
    //
    //   return Next(UF_FN(Step_WaitAck), id);   // Step_WaitAck(AsyncId)
    template <class Self, class... P, class... A>
    StepResult Next(StepResult (Self::*fn)(P...), const char* name, A&&... args)
    {
        Self*                          self = static_cast<Self*>(this);
        std::tuple<std::decay_t<P>...> bound{std::forward<A>(args)...};
        return {StepAction::Next,
                [self, fn, bound = std::move(bound)]() -> StepResult
                {
                    return std::apply([self, fn](const auto&... a) { return (self->*fn)(a...); },
                                      bound);
                },
                name};
    }

    // A Stay carrying a step deadline (the plain timeout escape): keep polling
    // THIS step, but if 'timeout' elapses since the step was ENTERED (repeated
    // Stay ticks do not reset it), transition to step 'fn' instead - the
    // step-level "catch". The body decides the success path with its own
    // Next/Done/Fail; this only guarantees an exit if the wait never resolves.
    // Like Next, the timeout target may take bound arguments:
    //
    //   return StayTimeout(3000ms, UF_FN(Step_HwTimeout));
    template <class Self, class... P, class... A>
    StepResult StayTimeout(Duration timeout, StepResult (Self::*fn)(P...), const char* name,
                           A&&... args)
    {
        StepResult r = Next(fn, name, std::forward<A>(args)...);
        r.action     = StepAction::Stay;
        r.timeout    = timeout;
        return r;
    }

    // StayUntil with a wait condition (post-wait / settling). Poll 'cond' each
    // round; once it has stayed true continuously for 'settle' of logical time,
    // transition to 'success'. If 'timeout' elapses since step entry first,
    // transition to 'timeout_step' instead. Pass each step target with UF_FN; in
    // this form neither target takes bound arguments. Argument order matches the
    // Python / C# ports: condition, settle, success, timeout, timeout step.
    //
    //   return StayUntil([this] { return SensorOn(); }, 3s, UF_FN(Step_Clamp),
    //                    100s, UF_FN(Step_Error));
    template <class Self, class CondF>
    StepResult StayUntil(CondF cond, Duration settle, StepResult (Self::*success)(),
                         const char* sname, Duration timeout,
                         StepResult (Self::*timeout_step)(), const char* tname)
    {
        Self*      self = static_cast<Self*>(this);
        StepResult r;
        r.action       = StepAction::Stay;
        r.timeout      = timeout;
        r.next_fn      = [self, timeout_step]() -> StepResult { return (self->*timeout_step)(); };
        r.next_name    = tname;
        r.settle       = settle;
        r.cond         = [cond = std::move(cond)]() -> bool { return static_cast<bool>(cond()); };
        r.success_fn   = [self, success]() -> StepResult { return (self->*success)(); };
        r.success_name = sname;
        return r;
    }

    // Re-Stay this step while any async submission for the flow is still in
    // flight, else advance to 'then'. The barrier-join convenience over
    // AnyAsyncPending(); for a deadline, write the explicit form by hand:
    //   if (AnyAsyncPending()) return StayTimeout(dur, UF_FN(Step_GaveUp));
    template <class Self, class... P, class... A>
    StepResult JoinAllAsync(StepResult (Self::*then)(P...), const char* name, A&&... args)
    {
        if (flow_->AnyAsyncPending())
            return Stay();
        return Next(then, name, std::forward<A>(args)...);
    }

    // Switch the running flow to ANOTHER task of the same flow, entering it at
    // its Entry() on the next pump round. This is the ONLY way to leave the
    // current task mid-flow - Next stays within the task. (Most flows instead
    // end a task with Done() and let an orchestrator launch the next one.)
    template <class OtherTask>
    StepResult StartTask(OtherTask& other)
    {
        static_assert(std::is_base_of_v<TaskBase, OtherTask>,
                      "StartTask: target must derive from uniflow::Task<Flow>");
        FlowT*     mod = flow_;
        OtherTask* o   = &other;
        return {StepAction::Next,
                [mod, o]() -> StepResult
                {
                    mod->uf_begin_unit_(o);
                    return o->Entry();
                },
                "Entry"};
    }

    // Attach a human-readable description to the current step (forwarded to the
    // flow, which holds the live description the observer reports).
    template <class... Args> void Describe(Args&&... args)
    {
        flow_->Describe(std::forward<Args>(args)...);
    }

    // Offload a static / free function to the Runtime's executor; carry the
    // returned AsyncId to the step that reads the result with AsyncResult<T>(id).
    // Returns 0 if rejected (in-flight cap reached). Forwarded to the flow.
    // UF_FN(fn) supplies the pointer + label:
    //
    //   AsyncId id = SubmitAsync(UF_FN(SimulateStartCmd));
    template <class R, class... Params, class... Args>
    [[nodiscard]] AsyncId SubmitAsync(R (*fn)(Params...), const char* job_label,
                                      Duration timeout = Duration::max(), Args&&... args)
    {
        return flow_->SubmitAsync(fn, job_label, timeout, std::forward<Args>(args)...);
    }

    // Read submission 'id's result (NotFound / Pending / Done / Failed /
    // TimedOut). Forwarded to the flow.
    template <class T> AsyncOutcome<T> AsyncResult(AsyncId id)
    {
        return flow_->template AsyncResult<T>(id);
    }

    // True while any submission for the flow is still in flight. Forwarded.
    bool AnyAsyncPending() const { return flow_->AnyAsyncPending(); }

    // Abandon and drop every async slot for the flow (OnAsyncAbandoned fires per
    // in-flight worker). Forwarded.
    void ClearAsync() { flow_->ClearAsync(); }

private:
    FlowT* flow_ = nullptr;
};

// ----- Uniflow<Derived>: out-of-line definitions -----

template <class Derived>
bool Uniflow<Derived>::ExecuteOnce(IUniflowObserver& obs, RoundProfile* prof)
{
    std::lock_guard<std::mutex> lk(op_mu_);

    if (flow_started_pending_)
    {
        obs.OnFlowStarted(instance_name_, flow_origin_);
        flow_started_pending_ = false;
    }
    if (!flow_running_)
        return false;

    const Config& cfg = runtime_->config();

    // -- 1. Sweep async slots (non-blocking) --
    //    Resolve every finished worker into its slot, fire the completion /
    //    slow-async hooks, and mark deadline misses. This does NOT gate the
    //    step: a continuation polls AnyAsyncPending() / AsyncResult<T>(id) and
    //    decides for itself when to Stay and wait, so any later step (not just
    //    the immediate next one) can await results.
    {
        auto now = Clock::now();
        for (AsyncSlot& s : async_jobs_)
        {
            if (s.done)
                continue;
            auto elapsed = now - s.submitted_at;
            bool ready = s.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            // Guard the deadline compare against Duration::max() - ms::max()
            // multiplied up to ns overflows int64 and the comparison flips.
            bool timed_out = s.timeout != Duration::max() && elapsed > s.timeout;

            if (!ready && !timed_out)
            {
                // Slow-async alarm: fire once per slot when its in-flight time
                // first crosses the configured threshold.
                if (!s.slow_warned && cfg.slow_async_threshold_ms != Duration::max() &&
                    elapsed > cfg.slow_async_threshold_ms)
                {
                    obs.OnSlowAsync(instance_name_, s.label ? s.label : "", to_ms(elapsed));
                    s.slow_warned = true;
                }
                continue; // still in flight; leave the slot pending
            }

            if (timed_out && !ready)
            {
                // Deadline missed. The worker is NOT killed - no portable way -
                // it keeps running and finishes into its (now ignored) promise.
                s.value.reset();
                s.exc       = std::make_exception_ptr(AsyncTimeout{s.label, to_duration(elapsed)});
                s.timed_out = true;
            }
            else
            {
                try
                {
                    s.value     = s.fut.get();
                    s.exc       = nullptr;
                    s.timed_out = false;
                }
                catch (...)
                {
                    s.value.reset();
                    s.exc       = std::current_exception();
                    s.timed_out = false;
                }
            }
            s.done = true;
            obs.OnAsyncCompleted(instance_name_, s.label ? s.label : "", to_ms(elapsed),
                                 s.exc != nullptr, s.timed_out);

            TraceEntry te;
            te.kind      = TraceEntry::Kind::AsyncCompletion;
            te.ordinal   = step_ordinal_;
            te.name      = s.label ? s.label : "";
            te.async_ms  = to_ms(elapsed);
            te.had_error = s.exc != nullptr;
            te.timed_out = s.timed_out;
            trace_.push_back(std::move(te));
            total_async_ += elapsed;
        }
    }

    // -- 2. Run the step (timed) --
    const char* step_name = curr_step_name_;
    tick_count_++;
    ticks_in_step_++;
    auto       t0 = Clock::now();
    StepResult r;
    bool       step_had_error = false;
    try
    {
        r = curr_fn_(); // CALL STEP!
    }
    catch (...)
    {
        std::exception_ptr ep = std::current_exception();
        std::string        throw_what;
        try
        {
            std::rethrow_exception(ep);
        }
        catch (const std::exception& e)
        {
            throw_what = e.what();
        }
        catch (...)
        {
            throw_what = "(non-std exception)";
        }

        obs.OnStepThrew(instance_name_, step_name ? step_name : "", throw_what, step_ordinal_,
                        tick_count_);

        if (!static_cast<Derived*>(this)->CatchStepExceptions())
            std::rethrow_exception(ep);

        step_had_error = true;
        r              = StepResult{StepAction::Fail, {}, nullptr};
    }
    auto cpu_dt = Clock::now() - t0;
    total_cpu_ += cpu_dt;

    // Tick stats. Per-step accumulators are reset at each transition; the
    // per-flow ones live for the whole flow and remember which step ran
    // the worst tick so OnFlowEnded can point at it. Round-robin pumping
    // is responsive only as long as no single tick monopolises the
    // thread, so we want these numbers visible.
    if (cpu_dt < step_tick_min_)
        step_tick_min_ = cpu_dt;
    if (cpu_dt > step_tick_max_)
        step_tick_max_ = cpu_dt;
    step_tick_sum_ += cpu_dt;
    if (cpu_dt < flow_tick_min_)
        flow_tick_min_ = cpu_dt;
    if (cpu_dt > flow_tick_max_)
    {
        flow_tick_max_      = cpu_dt;
        flow_tick_max_step_ = step_name;
    }

    // Round profiling: record this step body as a segment of the current
    // pump round (only when heavy tracing armed this round's sink).
    if (prof)
    {
        RoundSegment seg;
        seg.kind  = RoundSegment::Kind::Step;
        seg.obj   = instance_name_;
        seg.label = step_name ? step_name : "(entry)";
        seg.ms    = to_ms(cpu_dt);
        prof->segments.push_back(std::move(seg));
    }

    // Slow-cpu alarm: fire when this single body invocation held the
    // pump thread for longer than the configured threshold.
    if (!step_had_error && cfg.slow_step_threshold_ms != Duration::max() &&
        cpu_dt > cfg.slow_step_threshold_ms)
    {
        obs.OnSlowCpuStep(instance_name_, step_name ? step_name : "", to_ms(cpu_dt));
    }

    // -- 3. Apply the StepResult.
    //    On step transitions (Next / Done / Fail) we emit one trace entry
    //    and OnStepChanged for the step that JUST finished, with cumulative
    //    ticks_in_step_ and the wall time spent in this step. Stay keeps
    //    accumulating into the same step. The return value of ExecuteOnce
    //    is true iff a transition happened this round; the pump uses that
    //    to decide whether to sleep step_interval_sleep_ms / stay_sleep_ms /
    //    idle_sleep_ms before the next round. --
    // A StayUntil wait condition that has stayed true continuously for 'settle'
    // (post-wait / settling) becomes a transition to its success target. Checked
    // before the timeout so a satisfied wait wins if both are ready this round.
    if (r.action == StepAction::Stay && r.cond)
    {
        if (r.cond())
        {
            if (uf_settle_since_ == TimePoint{})
                uf_settle_since_ = runtime_->clock().Now();
            if (r.success_fn && runtime_->clock().Now() - uf_settle_since_ >= r.settle)
            {
                r.action    = StepAction::Next;
                r.next_fn   = std::move(r.success_fn);
                r.next_name = r.success_name;
            }
        }
        else
        {
            uf_settle_since_ = TimePoint{};
        }
    }

    // A StayUntil whose deadline has passed becomes a transition to its
    // timeout target. Measured on the runtime's virtual clock from step entry
    // (step_started_at_v_) so the repeated Stay ticks while polling do not keep
    // pushing it back, and a Freeze()/scale on that clock holds/scales the
    // deadline too. next_fn / next_name already hold the target, so the Next
    // handling below routes there and resets the per-step clock.
    if (r.action == StepAction::Stay && r.timeout != Duration::zero() &&
        runtime_->clock().Now() - step_started_at_v_ >= r.timeout)
    {
        r.action = StepAction::Next;
    }

    bool advanced = false;
    switch (r.action)
    {
    case StepAction::Stay:
        break;
    case StepAction::Next:
    case StepAction::Done:
    case StepAction::Fail:
    {
        advanced             = true;
        auto   transition_at = Clock::now();
        double step_wall_ms  = to_ms(transition_at - step_started_at_);

        const char* prev_name = step_name ? step_name : "";
        const char* next_name = r.action == StepAction::Next && r.next_name ? r.next_name : "";

        TraceEntry te;
        te.kind      = TraceEntry::Kind::StepTransition;
        te.ordinal   = step_ordinal_;
        te.name      = prev_name;
        te.ticks     = ticks_in_step_;
        te.step_ms   = step_wall_ms;
        te.action    = r.action;
        te.had_error = step_had_error;
        trace_.push_back(std::move(te));

        TickStats step_ticks;
        step_ticks.count  = ticks_in_step_;
        step_ticks.min_ms = step_tick_min_ == Clock::duration::max() ? 0.0 : to_ms(step_tick_min_);
        step_ticks.max_ms = to_ms(step_tick_max_);
        step_ticks.avg_ms = ticks_in_step_ > 0 ? to_ms(step_tick_sum_) / ticks_in_step_ : 0.0;

        obs.OnStepChanged(instance_name_, prev_name, next_name, curr_step_description_,
                          step_ordinal_, step_wall_ms, step_ticks);

        // Append the finished step to its task's trajectory (name + wall time
        // held + tick count). uf_active_unit_ is the task this step belongs to
        // (set by uf_begin_unit_ when the task was entered; a StartTask switch
        // only changes it next round when the new task's first step runs).
        if (uf_active_unit_)
            uf_active_unit_->uf_record_step_(prev_name, step_wall_ms, ticks_in_step_);

        if (r.action == StepAction::Next)
        {
            curr_fn_        = std::move(r.next_fn);
            curr_step_name_ = r.next_name;
            curr_step_description_.clear();
            step_ordinal_++;
            ticks_in_step_     = 0;
            step_started_at_   = transition_at;
            step_started_at_v_ = runtime_->clock().Now();
            // Reset per-step tick stats for the next step. Per-flow ones
            // persist until OnFlowEnded.
            step_tick_min_ = Clock::duration::max();
            step_tick_max_ = Clock::duration::zero();
            step_tick_sum_ = Clock::duration::zero();
            // A Next is a step change: re-arm the auto-reset timers (built-in
            // StepTimer + every NewAutoTimer one) and clear the settle window.
            uf_reset_step_timers_();
        }
        else
        {
            // Done / Fail - flow ends.
            if (r.action == StepAction::Done)
            {
                stats_.success_count++;
                stats_.last_success_length = step_ordinal_;
                if (step_ordinal_ > stats_.max_seen_length)
                    stats_.max_seen_length = step_ordinal_;
            }
            else
            {
                stats_.fail_count++;
            }

            FlowTickSummary fts;
            fts.all.count  = tick_count_;
            fts.all.min_ms = flow_tick_min_ == Clock::duration::max() ? 0.0 : to_ms(flow_tick_min_);
            fts.all.max_ms = to_ms(flow_tick_max_);
            fts.all.avg_ms = tick_count_ > 0 ? to_ms(total_cpu_) / tick_count_ : 0.0;
            fts.max_step =
                flow_tick_max_step_ ? std::string_view(flow_tick_max_step_) : std::string_view();

            obs.OnFlowEnded(instance_name_, r.action, step_ordinal_, trace_,
                            to_ms(transition_at - flow_started_at_), to_ms(total_cpu_),
                            to_ms(total_async_), fts, stats_, flow_origin_);
            ClearFlow();
            idle_cv_.notify_all();
        }
        break;
    }
    }
    return advanced;
}

// SubmitAsync hands a job to the Runtime's executor and appends a slot. The
// worker is a runtime function pointer (a static member or free function);
// instance state is unsafe across threads, so pass needed data through args.
// Returns the new slot's AsyncId, or 0 if the in-flight cap rejected it.
//
// The worker captures only the shared promise, the Runtime pointer, fn and the
// copied args - NEVER 'this' or the slot. So an abandoned worker (its slot
// dropped by ClearAsync / flow end) just finishes into an orphaned promise and
// calls rt->Wake(); both are safe (the Runtime's wake primitives outlive the
// executor that joins workers at teardown).
template <class Derived>
template <class R, class... Params, class... Args>
AsyncId Uniflow<Derived>::SubmitAsync(R (*fn)(Params...), const char* job_label, Duration timeout,
                                      Args&&... args)
{
    // Reject past the in-flight cap (counting only unresolved slots) so a flow
    // that fires without ever reading results cannot grow the slot list - and
    // the pool backlog behind it - without bound.
    const Config& cfg      = runtime_->config();
    std::size_t   inflight = 0;
    for (const AsyncSlot& s : async_jobs_)
        if (!s.done)
            ++inflight;
    if (cfg.max_inflight_async != 0 && inflight >= cfg.max_inflight_async)
    {
        runtime_->observer().OnAsyncHighWater(instance_name_, job_label ? job_label : "", inflight);
        return 0;
    }

    auto promise = std::make_shared<std::promise<std::any>>();

    AsyncSlot slot;
    slot.id           = next_async_id_++;
    slot.label        = job_label;
    slot.timeout      = timeout;
    slot.submitted_at = Clock::now();
    slot.fut          = promise->get_future();
    async_jobs_.push_back(std::move(slot));
    const AsyncId id = async_jobs_.back().id;

    auto tup = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...);

    Runtime* rt = runtime_;
    runtime_->executor().Submit(
        [promise, rt, fn, tup = std::move(tup)]() mutable
        {
            try
            {
                if constexpr (std::is_void_v<R>)
                {
                    std::apply([fn](auto&&... a) { fn(std::forward<decltype(a)>(a)...); },
                               std::move(tup));
                    promise->set_value(std::any{});
                }
                else
                {
                    R result =
                        std::apply([fn](auto&&... a)
                                   { return fn(std::forward<decltype(a)>(a)...); }, std::move(tup));
                    promise->set_value(std::any(std::move(result)));
                }
            }
            catch (...)
            {
                promise->set_exception(std::current_exception());
            }
            rt->Wake();
        });

    runtime_->observer().OnAsyncSubmitted(instance_name_, curr_step_name_ ? curr_step_name_ : "",
                                          job_label ? job_label : "");
    return id;
}

} // namespace uniflow

// ----- Argument helper for the direct-call API -----
//
// UF_FN does NOT wrap a call - it only expands the awkward argument pair (a
// member / function pointer plus its stringified name) so the function stays
// visible at the call site (and to IntelliSense). Used inside a task's step,
// 'decltype(*this)' is the task, so it names a sibling step or a static worker
// declared on that task alike:
//
//   return Next(UF_FN(Step_WaitAtSource));
//   return StayTimeout(3000ms, UF_FN(Step_HwTimeout));
//   SubmitAsync(UF_FN(SimulateStartCmd));
#define UF_FN(fn) (&std::remove_reference_t<decltype(*this)>::fn), #fn

// ======================================================================
//  BS::thread_pool - bundled in-place (was a separate header).
//
//  Upstream: https://github.com/bshoshany/thread-pool
//  Author:   Barak Shoshany (baraksh@gmail.com)
//  Version:  4.1.0 (2024-03-22)
//  License:  MIT (full notice preserved in the /** ... */ block below)
//
//  If you use this library in published research, please cite:
//    Barak Shoshany, "A C++17 Thread Pool for High-Performance
//    Scientific Computing", doi:10.1016/j.softx.2024.101687,
//    SoftwareX 26 (2024) 101687, arXiv:2105.00613
// ======================================================================
/**
 * @file BS_thread_pool.hpp
 * @author Barak Shoshany (baraksh@gmail.com) (https://baraksh.com)
 * @version 4.1.0
 * @date 2024-03-22
 * @copyright Copyright (c) 2024 Barak Shoshany. Licensed under the MIT license. If you found this project useful, please consider starring it on GitHub! If you use this library in software of any kind, please provide a link to the GitHub repository https://github.com/bshoshany/thread-pool in the source code and documentation. If you use this library in published research, please cite it as follows: Barak Shoshany, "A C++17 Thread Pool for High-Performance Scientific Computing", doi:10.1016/j.softx.2024.101687, SoftwareX 26 (2024) 101687, arXiv:2105.00613
 *
 * @brief BS::thread_pool: a fast, lightweight, and easy-to-use C++17 thread pool library. This header file contains the main thread pool class and some additional classes and definitions. No other files are needed in order to use the thread pool itself.
 */

#ifndef __cpp_exceptions
    #define BS_THREAD_POOL_DISABLE_EXCEPTION_HANDLING
    #undef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
#endif

#include <chrono>             // std::chrono
#include <condition_variable> // std::condition_variable
#include <cstddef>            // std::size_t
#ifdef BS_THREAD_POOL_ENABLE_PRIORITY
    #include <cstdint>        // std::int_least16_t
#endif
#ifndef BS_THREAD_POOL_DISABLE_EXCEPTION_HANDLING
    #include <exception>      // std::current_exception
#endif
#include <functional>         // std::function
#include <future>             // std::future, std::future_status, std::promise
#include <memory>             // std::make_shared, std::make_unique, std::shared_ptr, std::unique_ptr
#include <mutex>              // std::mutex, std::scoped_lock, std::unique_lock
#include <optional>           // std::nullopt, std::optional
#include <queue>              // std::priority_queue (if priority enabled), std::queue
#ifdef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
    #include <stdexcept>      // std::runtime_error
#endif
#include <thread>             // std::thread
#include <type_traits>        // std::conditional_t, std::decay_t, std::invoke_result_t, std::is_void_v, std::remove_const_t (if priority enabled)
#include <utility>            // std::forward, std::move
#include <vector>             // std::vector

/**
 * @brief A namespace used by Barak Shoshany's projects.
 */
namespace BS {
// Macros indicating the version of the thread pool library.
#define BS_THREAD_POOL_VERSION_MAJOR 4
#define BS_THREAD_POOL_VERSION_MINOR 1
#define BS_THREAD_POOL_VERSION_PATCH 0

class thread_pool;

/**
 * @brief A type to represent the size of things.
 */
using size_t = std::size_t;

/**
 * @brief A convenient shorthand for the type of `std::thread::hardware_concurrency()`. Should evaluate to unsigned int.
 */
using concurrency_t = std::invoke_result_t<decltype(std::thread::hardware_concurrency)>;

#ifdef BS_THREAD_POOL_ENABLE_PRIORITY
/**
 * @brief A type used to indicate the priority of a task. Defined to be an integer with a width of (at least) 16 bits.
 */
using priority_t = std::int_least16_t;

/**
 * @brief A namespace containing some pre-defined priorities for convenience.
 */
namespace pr {
    constexpr priority_t highest = 32767;
    constexpr priority_t high = 16383;
    constexpr priority_t normal = 0;
    constexpr priority_t low = -16384;
    constexpr priority_t lowest = -32768;
} // namespace pr

    // Macros used internally to enable or disable the priority arguments in the relevant functions.
    #define BS_THREAD_POOL_PRIORITY_INPUT , const priority_t priority = 0
    #define BS_THREAD_POOL_PRIORITY_OUTPUT , priority
#else
    #define BS_THREAD_POOL_PRIORITY_INPUT
    #define BS_THREAD_POOL_PRIORITY_OUTPUT
#endif

/**
 * @brief A namespace used to obtain information about the current thread.
 */
namespace this_thread {
    /**
     * @brief A type returned by `BS::this_thread::get_index()` which can optionally contain the index of a thread, if that thread belongs to a `BS::thread_pool`. Otherwise, it will contain no value.
     */
    using optional_index = std::optional<size_t>;

    /**
     * @brief A type returned by `BS::this_thread::get_pool()` which can optionally contain the pointer to the pool that owns a thread, if that thread belongs to a `BS::thread_pool`. Otherwise, it will contain no value.
     */
    using optional_pool = std::optional<thread_pool*>;

    /**
     * @brief A helper class to store information about the index of the current thread.
     */
    class [[nodiscard]] thread_info_index
    {
        friend class BS::thread_pool;

    public:
        /**
         * @brief Get the index of the current thread. If this thread belongs to a `BS::thread_pool` object, it will have an index from 0 to `BS::thread_pool::get_thread_count() - 1`. Otherwise, for example if this thread is the main thread or an independent `std::thread`, `std::nullopt` will be returned.
         *
         * @return An `std::optional` object, optionally containing a thread index. Unless you are 100% sure this thread is in a pool, first use `std::optional::has_value()` to check if it contains a value, and if so, use `std::optional::value()` to obtain that value.
         */
        [[nodiscard]] optional_index operator()() const
        {
            return index;
        }

    private:
        /**
         * @brief The index of the current thread.
         */
        optional_index index = std::nullopt;
    }; // class thread_info_index

    /**
     * @brief A helper class to store information about the thread pool that owns the current thread.
     */
    class [[nodiscard]] thread_info_pool
    {
        friend class BS::thread_pool;

    public:
        /**
         * @brief Get the pointer to the thread pool that owns the current thread. If this thread belongs to a `BS::thread_pool` object, a pointer to that object will be returned. Otherwise, for example if this thread is the main thread or an independent `std::thread`, `std::nullopt` will be returned.
         *
         * @return An `std::optional` object, optionally containing a pointer to a thread pool. Unless you are 100% sure this thread is in a pool, first use `std::optional::has_value()` to check if it contains a value, and if so, use `std::optional::value()` to obtain that value.
         */
        [[nodiscard]] optional_pool operator()() const
        {
            return pool;
        }

    private:
        /**
         * @brief A pointer to the thread pool that owns the current thread.
         */
        optional_pool pool = std::nullopt;
    }; // class thread_info_pool

    /**
     * @brief A `thread_local` object used to obtain information about the index of the current thread.
     */
    inline thread_local thread_info_index get_index;

    /**
     * @brief A `thread_local` object used to obtain information about the thread pool that owns the current thread.
     */
    inline thread_local thread_info_pool get_pool;
} // namespace this_thread

/**
 * @brief A helper class to facilitate waiting for and/or getting the results of multiple futures at once.
 *
 * @tparam T The return type of the futures.
 */
template <typename T>
class [[nodiscard]] multi_future : public std::vector<std::future<T>>
{
public:
    // Inherit all constructors from the base class `std::vector`.
    using std::vector<std::future<T>>::vector;

    // The copy constructor and copy assignment operator are deleted. The elements stored in a `multi_future` are futures, which cannot be copied.
    multi_future(const multi_future&) = delete;
    multi_future& operator=(const multi_future&) = delete;

    // The move constructor and move assignment operator are defaulted.
    multi_future(multi_future&&) = default;
    multi_future& operator=(multi_future&&) = default;

    /**
     * @brief Get the results from all the futures stored in this `multi_future`, rethrowing any stored exceptions.
     *
     * @return If the futures return `void`, this function returns `void` as well. Otherwise, it returns a vector containing the results.
     */
    [[nodiscard]] std::conditional_t<std::is_void_v<T>, void, std::vector<T>> get()
    {
        if constexpr (std::is_void_v<T>)
        {
            for (std::future<T>& future : *this)
                future.get();
            return;
        }
        else
        {
            std::vector<T> results;
            results.reserve(this->size());
            for (std::future<T>& future : *this)
                results.push_back(future.get());
            return results;
        }
    }

    /**
     * @brief Check how many of the futures stored in this `multi_future` are ready.
     *
     * @return The number of ready futures.
     */
    [[nodiscard]] size_t ready_count() const
    {
        size_t count = 0;
        for (const std::future<T>& future : *this)
        {
            if (future.wait_for(std::chrono::duration<double>::zero()) == std::future_status::ready)
                ++count;
        }
        return count;
    }

    /**
     * @brief Check if all the futures stored in this `multi_future` are valid.
     *
     * @return `true` if all futures are valid, `false` if at least one of the futures is not valid.
     */
    [[nodiscard]] bool valid() const
    {
        bool is_valid = true;
        for (const std::future<T>& future : *this)
            is_valid = is_valid && future.valid();
        return is_valid;
    }

    /**
     * @brief Wait for all the futures stored in this `multi_future`.
     */
    void wait() const
    {
        for (const std::future<T>& future : *this)
            future.wait();
    }

    /**
     * @brief Wait for all the futures stored in this `multi_future`, but stop waiting after the specified duration has passed. This function first waits for the first future for the desired duration. If that future is ready before the duration expires, this function waits for the second future for whatever remains of the duration. It continues similarly until the duration expires.
     *
     * @tparam R An arithmetic type representing the number of ticks to wait.
     * @tparam P An `std::ratio` representing the length of each tick in seconds.
     * @param duration The amount of time to wait.
     * @return `true` if all futures have been waited for before the duration expired, `false` otherwise.
     */
    template <typename R, typename P>
    bool wait_for(const std::chrono::duration<R, P>& duration) const
    {
        const std::chrono::time_point<std::chrono::steady_clock> start_time = std::chrono::steady_clock::now();
        for (const std::future<T>& future : *this)
        {
            future.wait_for(duration - (std::chrono::steady_clock::now() - start_time));
            if (duration < std::chrono::steady_clock::now() - start_time)
                return false;
        }
        return true;
    }

    /**
     * @brief Wait for all the futures stored in this `multi_future`, but stop waiting after the specified time point has been reached. This function first waits for the first future until the desired time point. If that future is ready before the time point is reached, this function waits for the second future until the desired time point. It continues similarly until the time point is reached.
     *
     * @tparam C The type of the clock used to measure time.
     * @tparam D An `std::chrono::duration` type used to indicate the time point.
     * @param timeout_time The time point at which to stop waiting.
     * @return `true` if all futures have been waited for before the time point was reached, `false` otherwise.
     */
    template <typename C, typename D>
    bool wait_until(const std::chrono::time_point<C, D>& timeout_time) const
    {
        for (const std::future<T>& future : *this)
        {
            future.wait_until(timeout_time);
            if (timeout_time < std::chrono::steady_clock::now())
                return false;
        }
        return true;
    }
}; // class multi_future

/**
 * @brief A fast, lightweight, and easy-to-use C++17 thread pool class.
 */
class [[nodiscard]] thread_pool
{
public:
    // ============================
    // Constructors and destructors
    // ============================

    /**
     * @brief Construct a new thread pool. The number of threads will be the total number of hardware threads available, as reported by the implementation. This is usually determined by the number of cores in the CPU. If a core is hyperthreaded, it will count as two threads.
     */
    thread_pool() : thread_pool(0, [] {}) {}

    /**
     * @brief Construct a new thread pool with the specified number of threads.
     *
     * @param num_threads The number of threads to use.
     */
    explicit thread_pool(const concurrency_t num_threads) : thread_pool(num_threads, [] {}) {}

    /**
     * @brief Construct a new thread pool with the specified initialization function.
     *
     * @param init_task An initialization function to run in each thread before it starts to execute any submitted tasks. The function must take no arguments and have no return value. It will only be executed exactly once, when the thread is first constructed.
     */
    explicit thread_pool(const std::function<void()>& init_task) : thread_pool(0, init_task) {}

    /**
     * @brief Construct a new thread pool with the specified number of threads and initialization function.
     *
     * @param num_threads The number of threads to use.
     * @param init_task An initialization function to run in each thread before it starts to execute any submitted tasks. The function must take no arguments and have no return value. It will only be executed exactly once, when the thread is first constructed.
     */
    thread_pool(const concurrency_t num_threads, const std::function<void()>& init_task) : thread_count(determine_thread_count(num_threads)), threads(std::make_unique<std::thread[]>(determine_thread_count(num_threads)))
    {
        create_threads(init_task);
    }

    // The copy and move constructors and assignment operators are deleted. The thread pool uses a mutex, which cannot be copied or moved.
    thread_pool(const thread_pool&) = delete;
    thread_pool(thread_pool&&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool& operator=(thread_pool&&) = delete;

    /**
     * @brief Destruct the thread pool. Waits for all tasks to complete, then destroys all threads. Note that if the pool is paused, then any tasks still in the queue will never be executed.
     */
    ~thread_pool()
    {
        wait();
        destroy_threads();
    }

    // =======================
    // Public member functions
    // =======================

#ifdef BS_THREAD_POOL_ENABLE_NATIVE_HANDLES
    /**
     * @brief Get a vector containing the underlying implementation-defined thread handles for each of the pool's threads, as obtained by `std::thread::native_handle()`. Only enabled if `BS_THREAD_POOL_ENABLE_NATIVE_HANDLES` is defined.
     *
     * @return The native thread handles.
     */
    [[nodiscard]] std::vector<std::thread::native_handle_type> get_native_handles() const
    {
        std::vector<std::thread::native_handle_type> native_handles(thread_count);
        for (concurrency_t i = 0; i < thread_count; ++i)
        {
            native_handles[i] = threads[i].native_handle();
        }
        return native_handles;
    }
#endif

    /**
     * @brief Get the number of tasks currently waiting in the queue to be executed by the threads.
     *
     * @return The number of queued tasks.
     */
    [[nodiscard]] size_t get_tasks_queued() const
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        return tasks.size();
    }

    /**
     * @brief Get the number of tasks currently being executed by the threads.
     *
     * @return The number of running tasks.
     */
    [[nodiscard]] size_t get_tasks_running() const
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        return tasks_running;
    }

    /**
     * @brief Get the total number of unfinished tasks: either still waiting in the queue, or running in a thread. Note that `get_tasks_total() == get_tasks_queued() + get_tasks_running()`.
     *
     * @return The total number of tasks.
     */
    [[nodiscard]] size_t get_tasks_total() const
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        return tasks_running + tasks.size();
    }

    /**
     * @brief Get the number of threads in the pool.
     *
     * @return The number of threads.
     */
    [[nodiscard]] concurrency_t get_thread_count() const
    {
        return thread_count;
    }

    /**
     * @brief Get a vector containing the unique identifiers for each of the pool's threads, as obtained by `std::thread::get_id()`.
     *
     * @return The unique thread identifiers.
     */
    [[nodiscard]] std::vector<std::thread::id> get_thread_ids() const
    {
        std::vector<std::thread::id> thread_ids(thread_count);
        for (concurrency_t i = 0; i < thread_count; ++i)
        {
            thread_ids[i] = threads[i].get_id();
        }
        return thread_ids;
    }

#ifdef BS_THREAD_POOL_ENABLE_PAUSE
    /**
     * @brief Check whether the pool is currently paused. Only enabled if `BS_THREAD_POOL_ENABLE_PAUSE` is defined.
     *
     * @return `true` if the pool is paused, `false` if it is not paused.
     */
    [[nodiscard]] bool is_paused() const
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        return paused;
    }

    /**
     * @brief Pause the pool. The workers will temporarily stop retrieving new tasks out of the queue, although any tasks already executed will keep running until they are finished. Only enabled if `BS_THREAD_POOL_ENABLE_PAUSE` is defined.
     */
    void pause()
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        paused = true;
    }
#endif

    /**
     * @brief Purge all the tasks waiting in the queue. Tasks that are currently running will not be affected, but any tasks still waiting in the queue will be discarded, and will never be executed by the threads. Please note that there is no way to restore the purged tasks.
     */
    void purge()
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        while (!tasks.empty())
            tasks.pop();
    }

    /**
     * @brief Submit a function with no arguments and no return value into the task queue, with the specified priority. To push a function with arguments, enclose it in a lambda expression. Does not return a future, so the user must use `wait()` or some other method to ensure that the task finishes executing, otherwise bad things will happen.
     *
     * @tparam F The type of the function.
     * @param task The function to push.
     * @param priority The priority of the task. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     */
    template <typename F>
    void detach_task(F&& task BS_THREAD_POOL_PRIORITY_INPUT)
    {
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            tasks.emplace(std::forward<F>(task) BS_THREAD_POOL_PRIORITY_OUTPUT);
        }
        task_available_cv.notify_one();
    }

    /**
     * @brief Parallelize a loop by automatically splitting it into blocks and submitting each block separately to the queue, with the specified priority. The block function takes two arguments, the start and end of the block, so that it is only called only once per block, but it is up to the user make sure the block function correctly deals with all the indices in each block. Does not return a `multi_future`, so the user must use `wait()` or some other method to ensure that the loop finishes executing, otherwise bad things will happen.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function to loop through.
     * @param first_index The first index in the loop.
     * @param index_after_last The index after the last index in the loop. The loop will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no blocks will be submitted.
     * @param block A function that will be called once per block. Should take exactly two arguments: the first index in the block and the index after the last index in the block. `block(start, end)` should typically involve a loop of the form `for (T i = start; i < end; ++i)`.
     * @param num_blocks The maximum number of blocks to split the loop into. The default is 0, which means the number of blocks will be equal to the number of threads in the pool.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     */
    template <typename T, typename F>
    void detach_blocks(const T first_index, const T index_after_last, F&& block, const size_t num_blocks = 0 BS_THREAD_POOL_PRIORITY_INPUT)
    {
        if (index_after_last > first_index)
        {
            const blocks blks(first_index, index_after_last, num_blocks ? num_blocks : thread_count);
            for (size_t blk = 0; blk < blks.get_num_blocks(); ++blk)
                detach_task(
                    [block = std::forward<F>(block), start = blks.start(blk), end = blks.end(blk)]
                    {
                        block(start, end);
                    } BS_THREAD_POOL_PRIORITY_OUTPUT);
        }
    }

    /**
     * @brief Parallelize a loop by automatically splitting it into blocks and submitting each block separately to the queue, with the specified priority. The loop function takes one argument, the loop index, so that it is called many times per block. Does not return a `multi_future`, so the user must use `wait()` or some other method to ensure that the loop finishes executing, otherwise bad things will happen.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function to loop through.
     * @param first_index The first index in the loop.
     * @param index_after_last The index after the last index in the loop. The loop will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no blocks will be submitted.
     * @param loop The function to loop through. Will be called once per index, many times per block. Should take exactly one argument: the loop index.
     * @param num_blocks The maximum number of blocks to split the loop into. The default is 0, which means the number of blocks will be equal to the number of threads in the pool.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     */
    template <typename T, typename F>
    void detach_loop(const T first_index, const T index_after_last, F&& loop, const size_t num_blocks = 0 BS_THREAD_POOL_PRIORITY_INPUT)
    {
        if (index_after_last > first_index)
        {
            const blocks blks(first_index, index_after_last, num_blocks ? num_blocks : thread_count);
            for (size_t blk = 0; blk < blks.get_num_blocks(); ++blk)
                detach_task(
                    [loop = std::forward<F>(loop), start = blks.start(blk), end = blks.end(blk)]
                    {
                        for (T i = start; i < end; ++i)
                            loop(i);
                    } BS_THREAD_POOL_PRIORITY_OUTPUT);
        }
    }

    /**
     * @brief Submit a sequence of tasks enumerated by indices to the queue, with the specified priority. Does not return a `multi_future`, so the user must use `wait()` or some other method to ensure that the sequence finishes executing, otherwise bad things will happen.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function used to define the sequence.
     * @param first_index The first index in the sequence.
     * @param index_after_last The index after the last index in the sequence. The sequence will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no tasks will be submitted.
     * @param sequence The function used to define the sequence. Will be called once per index. Should take exactly one argument, the index.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     */
    template <typename T, typename F>
    void detach_sequence(const T first_index, const T index_after_last, F&& sequence BS_THREAD_POOL_PRIORITY_INPUT)
    {
        for (T i = first_index; i < index_after_last; ++i)
            detach_task(
                [sequence = std::forward<F>(sequence), i]
                {
                    sequence(i);
                } BS_THREAD_POOL_PRIORITY_OUTPUT);
    }

    /**
     * @brief Reset the pool with the total number of hardware threads available, as reported by the implementation. Waits for all currently running tasks to be completed, then destroys all threads in the pool and creates a new thread pool with the new number of threads. Any tasks that were waiting in the queue before the pool was reset will then be executed by the new threads. If the pool was paused before resetting it, the new pool will be paused as well.
     */
    void reset()
    {
        reset(0, [] {});
    }

    /**
     * @brief Reset the pool with a new number of threads. Waits for all currently running tasks to be completed, then destroys all threads in the pool and creates a new thread pool with the new number of threads. Any tasks that were waiting in the queue before the pool was reset will then be executed by the new threads. If the pool was paused before resetting it, the new pool will be paused as well.
     *
     * @param num_threads The number of threads to use.
     */
    void reset(const concurrency_t num_threads)
    {
        reset(num_threads, [] {});
    }

    /**
     * @brief Reset the pool with the total number of hardware threads available, as reported by the implementation, and a new initialization function. Waits for all currently running tasks to be completed, then destroys all threads in the pool and creates a new thread pool with the new number of threads and initialization function. Any tasks that were waiting in the queue before the pool was reset will then be executed by the new threads. If the pool was paused before resetting it, the new pool will be paused as well.
     *
     * @param init_task An initialization function to run in each thread before it starts to execute any submitted tasks. The function must take no arguments and have no return value. It will only be executed exactly once, when the thread is first constructed.
     */
    void reset(const std::function<void()>& init_task)
    {
        reset(0, init_task);
    }

    /**
     * @brief Reset the pool with a new number of threads and a new initialization function. Waits for all currently running tasks to be completed, then destroys all threads in the pool and creates a new thread pool with the new number of threads and initialization function. Any tasks that were waiting in the queue before the pool was reset will then be executed by the new threads. If the pool was paused before resetting it, the new pool will be paused as well.
     *
     * @param num_threads The number of threads to use.
     * @param init_task An initialization function to run in each thread before it starts to execute any submitted tasks. The function must take no arguments and have no return value. It will only be executed exactly once, when the thread is first constructed.
     */
    void reset(const concurrency_t num_threads, const std::function<void()>& init_task)
    {
#ifdef BS_THREAD_POOL_ENABLE_PAUSE
        std::unique_lock tasks_lock(tasks_mutex);
        const bool was_paused = paused;
        paused = true;
        tasks_lock.unlock();
#endif
        wait();
        destroy_threads();
        thread_count = determine_thread_count(num_threads);
        threads = std::make_unique<std::thread[]>(thread_count);
        create_threads(init_task);
#ifdef BS_THREAD_POOL_ENABLE_PAUSE
        tasks_lock.lock();
        paused = was_paused;
#endif
    }

    /**
     * @brief Submit a function with no arguments into the task queue, with the specified priority. To submit a function with arguments, enclose it in a lambda expression. If the function has a return value, get a future for the eventual returned value. If the function has no return value, get an `std::future<void>` which can be used to wait until the task finishes.
     *
     * @tparam F The type of the function.
     * @tparam R The return type of the function (can be `void`).
     * @param task The function to submit.
     * @param priority The priority of the task. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     * @return A future to be used later to wait for the function to finish executing and/or obtain its returned value if it has one.
     */
    template <typename F, typename R = std::invoke_result_t<std::decay_t<F>>>
    [[nodiscard]] std::future<R> submit_task(F&& task BS_THREAD_POOL_PRIORITY_INPUT)
    {
        const std::shared_ptr<std::promise<R>> task_promise = std::make_shared<std::promise<R>>();
        detach_task(
            [task = std::forward<F>(task), task_promise]
            {
#ifndef BS_THREAD_POOL_DISABLE_EXCEPTION_HANDLING
                try
                {
#endif
                    if constexpr (std::is_void_v<R>)
                    {
                        task();
                        task_promise->set_value();
                    }
                    else
                    {
                        task_promise->set_value(task());
                    }
#ifndef BS_THREAD_POOL_DISABLE_EXCEPTION_HANDLING
                }
                catch (...)
                {
                    try
                    {
                        task_promise->set_exception(std::current_exception());
                    }
                    catch (...)
                    {
                    }
                }
#endif
            } BS_THREAD_POOL_PRIORITY_OUTPUT);
        return task_promise->get_future();
    }

    /**
     * @brief Parallelize a loop by automatically splitting it into blocks and submitting each block separately to the queue, with the specified priority. The block function takes two arguments, the start and end of the block, so that it is only called only once per block, but it is up to the user make sure the block function correctly deals with all the indices in each block. Returns a `multi_future` that contains the futures for all of the blocks.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function to loop through.
     * @tparam R The return type of the function to loop through (can be `void`).
     * @param first_index The first index in the loop.
     * @param index_after_last The index after the last index in the loop. The loop will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no blocks will be submitted, and an empty `multi_future` will be returned.
     * @param block A function that will be called once per block. Should take exactly two arguments: the first index in the block and the index after the last index in the block. `block(start, end)` should typically involve a loop of the form `for (T i = start; i < end; ++i)`.
     * @param num_blocks The maximum number of blocks to split the loop into. The default is 0, which means the number of blocks will be equal to the number of threads in the pool.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     * @return A `multi_future` that can be used to wait for all the blocks to finish. If the block function returns a value, the `multi_future` can also be used to obtain the values returned by each block.
     */
    template <typename T, typename F, typename R = std::invoke_result_t<std::decay_t<F>, T, T>>
    [[nodiscard]] multi_future<R> submit_blocks(const T first_index, const T index_after_last, F&& block, const size_t num_blocks = 0 BS_THREAD_POOL_PRIORITY_INPUT)
    {
        if (index_after_last > first_index)
        {
            const blocks blks(first_index, index_after_last, num_blocks ? num_blocks : thread_count);
            multi_future<R> future;
            future.reserve(blks.get_num_blocks());
            for (size_t blk = 0; blk < blks.get_num_blocks(); ++blk)
                future.push_back(submit_task(
                    [block = std::forward<F>(block), start = blks.start(blk), end = blks.end(blk)]
                    {
                        return block(start, end);
                    } BS_THREAD_POOL_PRIORITY_OUTPUT));
            return future;
        }
        return {};
    }

    /**
     * @brief Parallelize a loop by automatically splitting it into blocks and submitting each block separately to the queue, with the specified priority. The loop function takes one argument, the loop index, so that it is called many times per block. It must have no return value. Returns a `multi_future` that contains the futures for all of the blocks.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function to loop through.
     * @param first_index The first index in the loop.
     * @param index_after_last The index after the last index in the loop. The loop will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no tasks will be submitted, and an empty `multi_future` will be returned.
     * @param loop The function to loop through. Will be called once per index, many times per block. Should take exactly one argument: the loop index. It cannot have a return value.
     * @param num_blocks The maximum number of blocks to split the loop into. The default is 0, which means the number of blocks will be equal to the number of threads in the pool.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     * @return A `multi_future` that can be used to wait for all the blocks to finish.
     */
    template <typename T, typename F>
    [[nodiscard]] multi_future<void> submit_loop(const T first_index, const T index_after_last, F&& loop, const size_t num_blocks = 0 BS_THREAD_POOL_PRIORITY_INPUT)
    {
        if (index_after_last > first_index)
        {
            const blocks blks(first_index, index_after_last, num_blocks ? num_blocks : thread_count);
            multi_future<void> future;
            future.reserve(blks.get_num_blocks());
            for (size_t blk = 0; blk < blks.get_num_blocks(); ++blk)
                future.push_back(submit_task(
                    [loop = std::forward<F>(loop), start = blks.start(blk), end = blks.end(blk)]
                    {
                        for (T i = start; i < end; ++i)
                            loop(i);
                    } BS_THREAD_POOL_PRIORITY_OUTPUT));
            return future;
        }
        return {};
    }

    /**
     * @brief Submit a sequence of tasks enumerated by indices to the queue, with the specified priority. Returns a `multi_future` that contains the futures for all of the tasks.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function used to define the sequence.
     * @tparam R The return type of the function used to define the sequence (can be `void`).
     * @param first_index The first index in the sequence.
     * @param index_after_last The index after the last index in the sequence. The sequence will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no tasks will be submitted, and an empty `multi_future` will be returned.
     * @param sequence The function used to define the sequence. Will be called once per index. Should take exactly one argument, the index.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     * @return A `multi_future` that can be used to wait for all the tasks to finish. If the sequence function returns a value, the `multi_future` can also be used to obtain the values returned by each task.
     */
    template <typename T, typename F, typename R = std::invoke_result_t<std::decay_t<F>, T>>
    [[nodiscard]] multi_future<R> submit_sequence(const T first_index, const T index_after_last, F&& sequence BS_THREAD_POOL_PRIORITY_INPUT)
    {
        if (index_after_last > first_index)
        {
            multi_future<R> future;
            future.reserve(static_cast<size_t>(index_after_last - first_index));
            for (T i = first_index; i < index_after_last; ++i)
                future.push_back(submit_task(
                    [sequence = std::forward<F>(sequence), i]
                    {
                        return sequence(i);
                    } BS_THREAD_POOL_PRIORITY_OUTPUT));
            return future;
        }
        return {};
    }

#ifdef BS_THREAD_POOL_ENABLE_PAUSE
    /**
     * @brief Unpause the pool. The workers will resume retrieving new tasks out of the queue. Only enabled if `BS_THREAD_POOL_ENABLE_PAUSE` is defined.
     */
    void unpause()
    {
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            paused = false;
        }
        task_available_cv.notify_all();
    }
#endif

// Macros used internally to enable or disable pausing in the waiting and worker functions.
#ifdef BS_THREAD_POOL_ENABLE_PAUSE
    #define BS_THREAD_POOL_PAUSED_OR_EMPTY (paused || tasks.empty())
#else
    #define BS_THREAD_POOL_PAUSED_OR_EMPTY tasks.empty()
#endif

    /**
     * @brief Wait for tasks to be completed. Normally, this function waits for all tasks, both those that are currently running in the threads and those that are still waiting in the queue. However, if the pool is paused, this function only waits for the currently running tasks (otherwise it would wait forever). Note: To wait for just one specific task, use `submit_task()` instead, and call the `wait()` member function of the generated future.
     *
     * @throws `wait_deadlock` if called from within a thread of the same pool, which would result in a deadlock. Only enabled if `BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK` is defined.
     */
    void wait()
    {
#ifdef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
        if (this_thread::get_pool() == this)
            throw wait_deadlock();
#endif
        std::unique_lock tasks_lock(tasks_mutex);
        waiting = true;
        tasks_done_cv.wait(tasks_lock,
            [this]
            {
                return (tasks_running == 0) && BS_THREAD_POOL_PAUSED_OR_EMPTY;
            });
        waiting = false;
    }

    /**
     * @brief Wait for tasks to be completed, but stop waiting after the specified duration has passed.
     *
     * @tparam R An arithmetic type representing the number of ticks to wait.
     * @tparam P An `std::ratio` representing the length of each tick in seconds.
     * @param duration The amount of time to wait.
     * @return `true` if all tasks finished running, `false` if the duration expired but some tasks are still running.
     *
     * @throws `wait_deadlock` if called from within a thread of the same pool, which would result in a deadlock. Only enabled if `BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK` is defined.
     */
    template <typename R, typename P>
    bool wait_for(const std::chrono::duration<R, P>& duration)
    {
#ifdef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
        if (this_thread::get_pool() == this)
            throw wait_deadlock();
#endif
        std::unique_lock tasks_lock(tasks_mutex);
        waiting = true;
        const bool status = tasks_done_cv.wait_for(tasks_lock, duration,
            [this]
            {
                return (tasks_running == 0) && BS_THREAD_POOL_PAUSED_OR_EMPTY;
            });
        waiting = false;
        return status;
    }

    /**
     * @brief Wait for tasks to be completed, but stop waiting after the specified time point has been reached.
     *
     * @tparam C The type of the clock used to measure time.
     * @tparam D An `std::chrono::duration` type used to indicate the time point.
     * @param timeout_time The time point at which to stop waiting.
     * @return `true` if all tasks finished running, `false` if the time point was reached but some tasks are still running.
     *
     * @throws `wait_deadlock` if called from within a thread of the same pool, which would result in a deadlock. Only enabled if `BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK` is defined.
     */
    template <typename C, typename D>
    bool wait_until(const std::chrono::time_point<C, D>& timeout_time)
    {
#ifdef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
        if (this_thread::get_pool() == this)
            throw wait_deadlock();
#endif
        std::unique_lock tasks_lock(tasks_mutex);
        waiting = true;
        const bool status = tasks_done_cv.wait_until(tasks_lock, timeout_time,
            [this]
            {
                return (tasks_running == 0) && BS_THREAD_POOL_PAUSED_OR_EMPTY;
            });
        waiting = false;
        return status;
    }

#ifdef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
    // ==============
    // Public classes
    // ==============

    /**
     * @brief An exception that will be thrown by `wait()`, `wait_for()`, and `wait_until()` if the user tries to call them from within a thread of the same pool, which would result in a deadlock.
     */
    struct wait_deadlock : public std::runtime_error
    {
        wait_deadlock() : std::runtime_error("BS::thread_pool::wait_deadlock"){};
    };
#endif

private:
    // ========================
    // Private member functions
    // ========================

    /**
     * @brief Create the threads in the pool and assign a worker to each thread.
     *
     * @param init_task An initialization function to run in each thread before it starts to execute any submitted tasks.
     */
    void create_threads(const std::function<void()>& init_task)
    {
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            tasks_running = thread_count;
            workers_running = true;
        }
        for (concurrency_t i = 0; i < thread_count; ++i)
        {
            threads[i] = std::thread(&thread_pool::worker, this, i, init_task);
        }
    }

    /**
     * @brief Destroy the threads in the pool.
     */
    void destroy_threads()
    {
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            workers_running = false;
        }
        task_available_cv.notify_all();
        for (concurrency_t i = 0; i < thread_count; ++i)
        {
            threads[i].join();
        }
    }

    /**
     * @brief Determine how many threads the pool should have, based on the parameter passed to the constructor or reset().
     *
     * @param num_threads The parameter passed to the constructor or `reset()`. If the parameter is a positive number, then the pool will be created with this number of threads. If the parameter is non-positive, or a parameter was not supplied (in which case it will have the default value of 0), then the pool will be created with the total number of hardware threads available, as obtained from `std::thread::hardware_concurrency()`. If the latter returns zero for some reason, then the pool will be created with just one thread.
     * @return The number of threads to use for constructing the pool.
     */
    [[nodiscard]] static concurrency_t determine_thread_count(const concurrency_t num_threads)
    {
        if (num_threads > 0)
            return num_threads;
        if (std::thread::hardware_concurrency() > 0)
            return std::thread::hardware_concurrency();
        return 1;
    }

    /**
     * @brief A worker function to be assigned to each thread in the pool. Waits until it is notified by `detach_task()` that a task is available, and then retrieves the task from the queue and executes it. Once the task finishes, the worker notifies `wait()` in case it is waiting.
     *
     * @param idx The index of this thread.
     * @param init_task An initialization function to run in this thread before it starts to execute any submitted tasks.
     */
    void worker(const concurrency_t idx, const std::function<void()>& init_task)
    {
        this_thread::get_index.index = idx;
        this_thread::get_pool.pool = this;
        init_task();
        std::unique_lock tasks_lock(tasks_mutex);
        while (true)
        {
            --tasks_running;
            tasks_lock.unlock();
            if (waiting && (tasks_running == 0) && BS_THREAD_POOL_PAUSED_OR_EMPTY)
                tasks_done_cv.notify_all();
            tasks_lock.lock();
            task_available_cv.wait(tasks_lock,
                [this]
                {
                    return !BS_THREAD_POOL_PAUSED_OR_EMPTY || !workers_running;
                });
            if (!workers_running)
                break;
            {
#ifdef BS_THREAD_POOL_ENABLE_PRIORITY
                const std::function<void()> task = std::move(std::remove_const_t<pr_task&>(tasks.top()).task);
                tasks.pop();
#else
                const std::function<void()> task = std::move(tasks.front());
                tasks.pop();
#endif
                ++tasks_running;
                tasks_lock.unlock();
                task();
            }
            tasks_lock.lock();
        }
        this_thread::get_index.index = std::nullopt;
        this_thread::get_pool.pool = std::nullopt;
    }

    // ===============
    // Private classes
    // ===============

    /**
     * @brief A helper class to divide a range into blocks. Used by `detach_blocks()`, `submit_blocks()`, `detach_loop()`, and `submit_loop()`.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     */
    template <typename T>
    class [[nodiscard]] blocks
    {
    public:
        /**
         * @brief Construct a `blocks` object with the given specifications.
         *
         * @param first_index_ The first index in the range.
         * @param index_after_last_ The index after the last index in the range.
         * @param num_blocks_ The desired number of blocks to divide the range into.
         */
        blocks(const T first_index_, const T index_after_last_, const size_t num_blocks_) : first_index(first_index_), index_after_last(index_after_last_), num_blocks(num_blocks_)
        {
            if (index_after_last > first_index)
            {
                const size_t total_size = static_cast<size_t>(index_after_last - first_index);
                if (num_blocks > total_size)
                    num_blocks = total_size;
                block_size = total_size / num_blocks;
                remainder = total_size % num_blocks;
                if (block_size == 0)
                {
                    block_size = 1;
                    num_blocks = (total_size > 1) ? total_size : 1;
                }
            }
            else
            {
                num_blocks = 0;
            }
        }

        /**
         * @brief Get the first index of a block.
         *
         * @param block The block number.
         * @return The first index.
         */
        [[nodiscard]] T start(const size_t block) const
        {
            return first_index + static_cast<T>(block * block_size) + static_cast<T>(block < remainder ? block : remainder);
        }

        /**
         * @brief Get the index after the last index of a block.
         *
         * @param block The block number.
         * @return The index after the last index.
         */
        [[nodiscard]] T end(const size_t block) const
        {
            return (block == num_blocks - 1) ? index_after_last : start(block + 1);
        }

        /**
         * @brief Get the number of blocks. Note that this may be different than the desired number of blocks that was passed to the constructor.
         *
         * @return The number of blocks.
         */
        [[nodiscard]] size_t get_num_blocks() const
        {
            return num_blocks;
        }

    private:
        /**
         * @brief The size of each block (except possibly the last block).
         */
        size_t block_size = 0;

        /**
         * @brief The first index in the range.
         */
        T first_index = 0;

        /**
         * @brief The index after the last index in the range.
         */
        T index_after_last = 0;

        /**
         * @brief The number of blocks.
         */
        size_t num_blocks = 0;

        /**
         * @brief The remainder obtained after dividing the total size by the number of blocks.
         */
        size_t remainder = 0;
    }; // class blocks

#ifdef BS_THREAD_POOL_ENABLE_PRIORITY
    /**
     * @brief A helper class to store a task with an assigned priority.
     */
    class [[nodiscard]] pr_task
    {
        friend class thread_pool;

    public:
        /**
         * @brief Construct a new task with an assigned priority by copying the task.
         *
         * @param task_ The task.
         * @param priority_ The desired priority.
         */
        explicit pr_task(const std::function<void()>& task_, const priority_t priority_ = 0) : task(task_), priority(priority_) {}

        /**
         * @brief Construct a new task with an assigned priority by moving the task.
         *
         * @param task_ The task.
         * @param priority_ The desired priority.
         */
        explicit pr_task(std::function<void()>&& task_, const priority_t priority_ = 0) : task(std::move(task_)), priority(priority_) {}

        /**
         * @brief Compare the priority of two tasks.
         *
         * @param lhs The first task.
         * @param rhs The second task.
         * @return `true` if the first task has a lower priority than the second task, `false` otherwise.
         */
        [[nodiscard]] friend bool operator<(const pr_task& lhs, const pr_task& rhs)
        {
            return lhs.priority < rhs.priority;
        }

    private:
        /**
         * @brief The task.
         */
        std::function<void()> task = {};

        /**
         * @brief The priority of the task.
         */
        priority_t priority = 0;
    }; // class pr_task
#endif

    // ============
    // Private data
    // ============

#ifdef BS_THREAD_POOL_ENABLE_PAUSE
    /**
     * @brief A flag indicating whether the workers should pause. When set to `true`, the workers temporarily stop retrieving new tasks out of the queue, although any tasks already executed will keep running until they are finished. When set to `false` again, the workers resume retrieving tasks.
     */
    bool paused = false;
#endif

    /**
     * @brief A condition variable to notify `worker()` that a new task has become available.
     */
    std::condition_variable task_available_cv = {};

    /**
     * @brief A condition variable to notify `wait()` that the tasks are done.
     */
    std::condition_variable tasks_done_cv = {};

    /**
     * @brief A queue of tasks to be executed by the threads.
     */
#ifdef BS_THREAD_POOL_ENABLE_PRIORITY
    std::priority_queue<pr_task> tasks = {};
#else
    std::queue<std::function<void()>> tasks = {};
#endif

    /**
     * @brief A counter for the total number of currently running tasks.
     */
    size_t tasks_running = 0;

    /**
     * @brief A mutex to synchronize access to the task queue by different threads.
     */
    mutable std::mutex tasks_mutex = {};

    /**
     * @brief The number of threads in the pool.
     */
    concurrency_t thread_count = 0;

    /**
     * @brief A smart pointer to manage the memory allocated for the threads.
     */
    std::unique_ptr<std::thread[]> threads = nullptr;

    /**
     * @brief A flag indicating that `wait()` is active and expects to be notified whenever a task is done.
     */
    bool waiting = false;

    /**
     * @brief A flag indicating to the workers to keep running. When set to `false`, the workers terminate permanently.
     */
    bool workers_running = false;
}; // class thread_pool
} // namespace BS

// Default executor: BSThreadPoolExecutor wrapping BS::thread_pool, in
// uniflow::detail so user code never names it.
namespace uniflow
{
namespace detail
{

class BSThreadPoolExecutor : public IExecutor
{
public:
    explicit BSThreadPoolExecutor(std::size_t threads)
        : pool_(static_cast<BS::concurrency_t>(threads ? threads
                                                       : std::thread::hardware_concurrency()))
    {
    }

    void        Submit(std::function<void()> task) override { pool_.detach_task(std::move(task)); }
    std::size_t QueueDepth() const override { return pool_.get_tasks_queued(); }

private:
    BS::thread_pool pool_;
};

inline std::unique_ptr<IExecutor> make_default_executor(std::size_t threads)
{
    return std::make_unique<BSThreadPoolExecutor>(threads);
}

} // namespace detail
} // namespace uniflow
