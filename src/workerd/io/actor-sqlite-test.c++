// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-sqlite.h"
#include <kj/test.h>
#include <kj/debug.h>
#include "io-gate.h"
#include <workerd/util/capnp-mock.h>
#include <workerd/util/test.h>

namespace workerd {
namespace {

static constexpr kj::Date oneMs = 1 * kj::MILLISECONDS + kj::UNIX_EPOCH;
static constexpr kj::Date twoMs = 2 * kj::MILLISECONDS + kj::UNIX_EPOCH;
static constexpr kj::Date threeMs = 3 * kj::MILLISECONDS + kj::UNIX_EPOCH;
static constexpr kj::Date fourMs = 4 * kj::MILLISECONDS + kj::UNIX_EPOCH;
static constexpr kj::Date fiveMs = 5 * kj::MILLISECONDS + kj::UNIX_EPOCH;

template <typename T>
kj::Promise<T> eagerlyReportExceptions(kj::Promise<T> promise, kj::SourceLocation location = {}) {
  return promise.eagerlyEvaluate([location](kj::Exception&& e) -> T {
    KJ_LOG_AT(ERROR, location, e);
    kj::throwFatalException(kj::mv(e));
  });
}

// Expect that a synchronous result is returned.
template <typename T>
T expectSync(kj::OneOf<T, kj::Promise<T>> result, kj::SourceLocation location = {}) {
  KJ_SWITCH_ONEOF(result) {
    KJ_CASE_ONEOF(promise, kj::Promise<T>) {
      KJ_FAIL_ASSERT_AT(location, "result was unexpectedly asynchronous");
    }
    KJ_CASE_ONEOF(value, T) {
      return kj::mv(value);
    }
  }
  KJ_UNREACHABLE;
}

struct ActorSqliteTestOptions final {
  bool monitorOutputGate = true;
};

struct ActorSqliteTest final {
  kj::EventLoop loop;
  kj::WaitScope ws;

  OutputGate gate;
  kj::Own<const kj::Directory> vfsDir;
  SqliteDatabase::Vfs vfs;
  SqliteDatabase db;

  struct Call final {
    kj::String desc;
    kj::Own<kj::PromiseFulfiller<void>> fulfiller;
  };
  kj::Vector<Call> calls;

  struct ActorSqliteTestHooks final: public ActorSqlite::Hooks {
  public:
    explicit ActorSqliteTestHooks(ActorSqliteTest& parent): parent(parent) {}

    kj::Promise<void> scheduleRun(kj::Maybe<kj::Date> newAlarmTime) override {
      KJ_IF_SOME(h, parent.scheduleRunHandler) {
        return h(newAlarmTime);
      }
      auto desc = newAlarmTime.map([](auto& t) {
        return kj::str("scheduleRun(", t, ")");
      }).orDefault(kj::str("scheduleRun(none)"));
      auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
      parent.calls.add(Call{kj::mv(desc), kj::mv(fulfiller)});
      return kj::mv(promise);
    }

    ActorSqliteTest& parent;
  };
  kj::Maybe<kj::Function<kj::Promise<void>(kj::Maybe<kj::Date>)>> scheduleRunHandler;
  ActorSqliteTestHooks hooks = ActorSqliteTestHooks(*this);

  ActorSqlite actor;

  kj::Promise<void> gateBrokenPromise;
  kj::UnwindDetector unwindDetector;

  explicit ActorSqliteTest(ActorSqliteTestOptions options = {})
      : ws(loop),
        vfsDir(kj::newInMemoryDirectory(kj::nullClock())),
        vfs(*vfsDir),
        db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY),
        actor(kj::attachRef(db), gate, KJ_BIND_METHOD(*this, commitCallback), hooks),
        gateBrokenPromise(options.monitorOutputGate ? eagerlyReportExceptions(gate.onBroken())
                                                    : kj::Promise<void>(kj::READY_NOW)) {}

  ~ActorSqliteTest() noexcept(false) {
    if (!unwindDetector.isUnwinding()) {
      // Make sure if the output gate has been broken, the exception was reported. This is
      // important to report errors thrown inside flush(), since those won't otherwise propagate
      // into the test body.
      gateBrokenPromise.poll(ws);

      // Make sure there's no outstanding async work we haven't considered:
      pollAndExpectCalls({}, "unexpected calls at end of test");
    }
  }

  kj::Promise<void> commitCallback() {
    auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
    calls.add(Call{kj::str("commit"), kj::mv(fulfiller)});
    return kj::mv(promise);
  }

  // Polls the event loop, then asserts that the description of calls up to this point match the
  // expectation and returns their fulfillers.  Also clears the call log.
  //
  // TODO(cleanup): Is there a better way to do mocks?  capnp-mock looks nice, but seems a bit
  // heavyweight for this test.
  kj::Vector<kj::Own<kj::PromiseFulfiller<void>>> pollAndExpectCalls(
      std::initializer_list<kj::StringPtr> expCallDescs,
      kj::StringPtr message = ""_kj,
      kj::SourceLocation location = {}) {
    ws.poll();
    auto callDescs = KJ_MAP(c, calls) { return kj::str(c.desc); };
    KJ_ASSERT_AT(callDescs == heapArray(expCallDescs), location, kj::str(message));
    auto fulfillers = KJ_MAP(c, calls) { return kj::mv(c.fulfiller); };
    calls.clear();
    return kj::mv(fulfillers);
  }

  // A few driver methods for convenience.
  auto get(kj::StringPtr key, ActorCache::ReadOptions options = {}) {
    return actor.get(kj::str(key), options);
  }
  auto getAlarm(ActorCache::ReadOptions options = {}) {
    return actor.getAlarm(options);
  }
  auto put(kj::StringPtr key, kj::StringPtr value, ActorCache::WriteOptions options = {}) {
    return actor.put(kj::str(key), kj::heapArray(value.asBytes()), options);
  }
  auto setAlarm(kj::Maybe<kj::Date> newTime, ActorCache::WriteOptions options = {}) {
    return actor.setAlarm(newTime, options);
  }
};

KJ_TEST("initial alarm value is unset") {
  ActorSqliteTest test;

  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("can set and get alarm") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("alarm write happens transactionally with storage ops") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.put("foo", "bar");
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectSync(test.get("foo"))) == kj::str("bar").asBytes());
}

KJ_TEST("storage op without alarm change does not wait on scheduler") {
  ActorSqliteTest test;

  test.put("foo", "bar");
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectSync(test.get("foo"))) == kj::str("bar").asBytes());
  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("alarm scheduling starts synchronously before implicit local db commit") {
  ActorSqliteTest test;

  // In workerd (unlike edgeworker), there is no remote storage, so there is no work done in
  // commitCallback(); the local db is considered durably stored after the synchronous sqlite
  // commit() call returns.  If a commit includes an alarm state change that requires scheduling
  // before the commit call, it needs to happen synchronously.  Since workerd synchronously
  // schedules alarms, we just need to ensure that the database is in a pre-commit state when
  // scheduleRun() is called.

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});

  bool startedScheduleRun = false;
  test.scheduleRunHandler = [&](kj::Maybe<kj::Date>) -> kj::Promise<void> {
    startedScheduleRun = true;

    KJ_EXPECT_THROW_MESSAGE(
        "cannot start a transaction within a transaction", test.db.run("BEGIN TRANSACTION"));

    return kj::READY_NOW;
  };

  test.setAlarm(oneMs);
  KJ_ASSERT(!startedScheduleRun);
  test.ws.poll();
  KJ_ASSERT(startedScheduleRun);

  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("alarm scheduling starts synchronously before explicit local db commit") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});

  bool startedScheduleRun = false;
  test.scheduleRunHandler = [&](kj::Maybe<kj::Date>) -> kj::Promise<void> {
    startedScheduleRun = true;

    // Not sure if there is a good way to detect savepoint presence without mutating the db state,
    // but this is sufficient to verify the test properties:

    // Verify that we are not within a nested savepoint.
    KJ_EXPECT_THROW_MESSAGE(
        "no such savepoint: _cf_savepoint_1", test.db.run("RELEASE _cf_savepoint_1"));

    // Verify that we are within the root savepoint.
    test.db.run("RELEASE _cf_savepoint_0");
    KJ_EXPECT_THROW_MESSAGE(
        "no such savepoint: _cf_savepoint_0", test.db.run("RELEASE _cf_savepoint_0"));

    // We don't actually care what happens in the test after this point, but it's slightly simpler
    // to readd the savepoint to allow the test to complete cleanly:
    test.db.run("SAVEPOINT _cf_savepoint_0");

    return kj::READY_NOW;
  };

  {
    auto txn = test.actor.startTransaction();
    txn->setAlarm(oneMs, {});

    KJ_ASSERT(!startedScheduleRun);
    txn->commit();
    KJ_ASSERT(startedScheduleRun);

    test.pollAndExpectCalls({"commit"})[0]->fulfill();
  }

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("alarm scheduling does not start synchronously before nested explicit local db commit") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});

  bool startedScheduleRun = false;
  test.scheduleRunHandler = [&](kj::Maybe<kj::Date>) -> kj::Promise<void> {
    startedScheduleRun = true;
    return kj::READY_NOW;
  };

  {
    auto txn1 = test.actor.startTransaction();

    {
      auto txn2 = test.actor.startTransaction();
      txn2->setAlarm(oneMs, {});

      txn2->commit();
      KJ_ASSERT(!startedScheduleRun);
    }

    txn1->commit();
    KJ_ASSERT(startedScheduleRun);

    test.pollAndExpectCalls({"commit"})[0]->fulfill();
  }

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("can clear alarm") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  test.setAlarm(kj::none);
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("can set alarm twice") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);
}

KJ_TEST("setting duplicate alarm is no-op") {
  ActorSqliteTest test;

  test.setAlarm(kj::none);
  test.pollAndExpectCalls({});

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({});
}

KJ_TEST("tells alarm handler to cancel when committed alarm is empty") {
  ActorSqliteTest test;

  KJ_ASSERT(test.actor.armAlarmHandler(oneMs, false) == kj::none);
}

KJ_TEST("tells alarm handler to cancel when committed alarm does not match handler alarm") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  KJ_ASSERT(test.actor.armAlarmHandler(twoMs, false) == kj::none);
}

KJ_TEST("dirty alarm during handler does not cancel alarm") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  test.setAlarm(twoMs);
  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
}

KJ_TEST("getAlarm() returns null during handler") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false));
    test.pollAndExpectCalls({});

    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
  }
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
}

KJ_TEST("alarm handler handle clears alarm when dropped with no writes") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("alarm deleter does not clear alarm when dropped with writes") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false));
    test.setAlarm(twoMs);
  }
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);
}

KJ_TEST("can cancel deferred alarm deletion during handler") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false));
    test.actor.cancelDeferredAlarmDeletion();
  }

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("canceling deferred alarm deletion outside handler has no effect") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();

  test.actor.cancelDeferredAlarmDeletion();

  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("canceling deferred alarm deletion outside handler edge case") {
  // Presumably harmless to cancel deletion if the client requests it after the handler ends but
  // before the event loop runs the commit code?  Trying to cancel deletion outside the handler is
  // a bit of a contract violation anyway -- maybe we should just assert against it?
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.actor.cancelDeferredAlarmDeletion();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("canceling deferred alarm deletion is idempotent") {
  // Not sure if important, but matches ActorCache behavior.
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false));
    test.actor.cancelDeferredAlarmDeletion();
    test.actor.cancelDeferredAlarmDeletion();
  }

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("handler alarm is not deleted when commit fails") {
  ActorSqliteTest test({.monitorOutputGate = false});

  auto promise = test.gate.onBroken();

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto handle = test.actor.armAlarmHandler(oneMs, false);

    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
  }
  test.pollAndExpectCalls({"commit"})[0]->reject(KJ_EXCEPTION(FAILED, "a_rejected_commit"));

  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", promise.wait(test.ws));
}

KJ_TEST("setting earlier alarm persists alarm scheduling before db") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);

  // Update alarm to be earlier.  We expect the alarm scheduling to be persisted before the db.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("setting later alarm persists db before alarm scheduling") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  // Update alarm to be later.  We expect the db to be persisted before the alarm scheduling.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);
}

KJ_TEST("multiple set-earlier in-flight alarms wait for earliest before committing db") {
  ActorSqliteTest test;

  // Initialize alarm state to 5ms.
  test.setAlarm(fiveMs);
  test.pollAndExpectCalls({"scheduleRun(5ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == fiveMs);

  // Gate is not blocked.
  auto gateWaitBefore = test.gate.wait();
  KJ_ASSERT(gateWaitBefore.poll(test.ws));

  // Update alarm to be earlier (4ms).  We expect the alarm scheduling to start.
  test.setAlarm(fourMs);
  auto fulfiller4Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(4ms)"})[0]);
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == fourMs);

  // Gate as-of 4ms update is blocked.
  auto gateWait4ms = test.gate.wait();
  KJ_ASSERT(!gateWait4ms.poll(test.ws));

  // While 4ms scheduling request is in-flight, update alarm to be even earlier (3ms).  We expect
  // the 4ms request to block the 3ms scheduling request.
  test.setAlarm(threeMs);
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == threeMs);

  // Gate as-of 3ms update is blocked.
  auto gateWait3ms = test.gate.wait();
  KJ_ASSERT(!gateWait3ms.poll(test.ws));

  // Update alarm to be even earlier (2ms).  We expect scheduling requests to still be blocked.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);

  // Gate as-of 2ms update is blocked.
  auto gateWait2ms = test.gate.wait();
  KJ_ASSERT(!gateWait2ms.poll(test.ws));

  // Fulfill the 4ms request.  We expect the 2ms scheduling to start, because that is the current
  // alarm value.
  fulfiller4Ms->fulfill();
  auto fulfiller2Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]);
  test.pollAndExpectCalls({});

  // While waiting for 2ms request, update alarm time to be 1ms.  Expect scheduling to be blocked.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  // Gate as-of 1ms update is blocked.
  auto gateWait1ms = test.gate.wait();
  KJ_ASSERT(!gateWait1ms.poll(test.ws));

  // Fulfill the 2ms request.  We expect the 1ms scheduling to start.
  fulfiller2Ms->fulfill();
  auto fulfiller1Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]);
  test.pollAndExpectCalls({});

  // Fulfill the 1ms request.  We expect a single db commit to start (coalescing all previous db
  // commits together).
  fulfiller1Ms->fulfill();
  auto commitFulfiller = kj::mv(test.pollAndExpectCalls({"commit"})[0]);
  test.pollAndExpectCalls({});

  // We expect all earlier gates to be blocked until commit completes.
  KJ_ASSERT(!gateWait4ms.poll(test.ws));
  KJ_ASSERT(!gateWait3ms.poll(test.ws));
  KJ_ASSERT(!gateWait2ms.poll(test.ws));
  KJ_ASSERT(!gateWait1ms.poll(test.ws));
  commitFulfiller->fulfill();
  KJ_ASSERT(gateWait4ms.poll(test.ws));
  KJ_ASSERT(gateWait3ms.poll(test.ws));
  KJ_ASSERT(gateWait2ms.poll(test.ws));
  KJ_ASSERT(gateWait1ms.poll(test.ws));

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("setting later alarm times does scheduling after db commit") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  // Gate is not blocked.
  auto gateWaitBefore = test.gate.wait();
  KJ_ASSERT(gateWaitBefore.poll(test.ws));

  // Set alarm to 2ms.  Expect 2ms db commit to start.
  test.setAlarm(twoMs);
  auto commit2MsFulfiller = kj::mv(test.pollAndExpectCalls({"commit"})[0]);
  test.pollAndExpectCalls({});

  // Gate as-of 2ms update is blocked.
  auto gateWait2Ms = test.gate.wait();
  KJ_ASSERT(!gateWait2Ms.poll(test.ws));

  // Set alarm to 3ms.  Expect 3ms db commit to start.
  test.setAlarm(threeMs);
  auto commit3MsFulfiller = kj::mv(test.pollAndExpectCalls({"commit"})[0]);
  test.pollAndExpectCalls({});

  // Gate as-of 3ms update is blocked.
  auto gateWait3Ms = test.gate.wait();
  KJ_ASSERT(!gateWait3Ms.poll(test.ws));

  // Fulfill 2ms db commit.  Expect 2ms alarm to be scheduled.
  commit2MsFulfiller->fulfill();
  auto fulfiller2Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]);
  test.pollAndExpectCalls({});

  // Fulfill 3ms db commit.  Expect 3ms alarm to be scheduled.
  commit3MsFulfiller->fulfill();
  auto fulfiller3Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(3ms)"})[0]);
  test.pollAndExpectCalls({});

  // Gate is blocked until outstanding alarm scheduling is fulfilled.
  KJ_ASSERT(!gateWait2Ms.poll(test.ws));
  fulfiller2Ms->fulfill();
  KJ_ASSERT(gateWait2Ms.poll(test.ws));

  KJ_ASSERT(!gateWait3Ms.poll(test.ws));
  fulfiller3Ms->fulfill();
  KJ_ASSERT(gateWait3Ms.poll(test.ws));
}

KJ_TEST("rejected alarm scheduling request breaks gate") {
  ActorSqliteTest test({.monitorOutputGate = false});

  auto promise = test.gate.onBroken();

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->reject(
      KJ_EXCEPTION(FAILED, "a_rejected_scheduleRun"));

  KJ_EXPECT_THROW_MESSAGE("a_rejected_scheduleRun", promise.wait(test.ws));
}

KJ_TEST("getAlarm/setAlarm check for brokenness") {
  ActorSqliteTest test({.monitorOutputGate = false});

  auto promise = test.gate.onBroken();

  // Break gate
  test.put("foo", "bar");
  test.pollAndExpectCalls({"commit"})[0]->reject(KJ_EXCEPTION(FAILED, "a_rejected_commit"));

  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", promise.wait(test.ws));

  // Apparently we don't actually set brokenness until the taskFailed handler runs, but presumably
  // this is OK?
  test.getAlarm();

  // Ensure taskFailed handler runs and notices brokenness:
  test.ws.poll();

  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", test.getAlarm());
  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", test.setAlarm(kj::none));
  test.pollAndExpectCalls({});
}

}  // namespace
}  // namespace workerd
