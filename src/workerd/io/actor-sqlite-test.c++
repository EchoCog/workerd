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
      auto desc = newAlarmTime.map([](auto& t) {
        return kj::str("scheduleRun(", t, ")");
      }).orDefault(kj::str("scheduleRun(none)"));
      auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
      parent.calls.add(Call{kj::mv(desc), kj::mv(fulfiller)});
      return kj::mv(promise);
    }

    ActorSqliteTest& parent;
  };
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
}

KJ_TEST("can clear alarm") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
  test.pollAndExpectCalls({});

  test.setAlarm(kj::none);
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

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

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  test.setAlarm(kj::none);
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  test.ws.poll();  // needs additional poll?

  KJ_ASSERT(test.actor.armAlarmHandler(oneMs, false) == kj::none);
}

KJ_TEST("tells alarm handler to cancel when committed alarm does not match requested alarm") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  test.ws.poll();  // needs additional poll?

  KJ_ASSERT(test.actor.armAlarmHandler(twoMs, false) == kj::none);
}

KJ_TEST("dirty alarm during handler does not cancel alarm") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  test.setAlarm(twoMs);
  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
}

KJ_TEST("getAlarm() returns null during handler") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  {
    auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false));
    test.pollAndExpectCalls({});

    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
  }
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
}

KJ_TEST("alarm handler handle clears alarm when dropped with no writes") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("alarm handler handle does not clear alarm when dropped with writes") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  {
    auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false));
    test.setAlarm(twoMs);
  }
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);
}

KJ_TEST("can cancel deferred alarm deletion during handler") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  {
    auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false));
    test.actor.cancelDeferredAlarmDeletion();
  }

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("canceling deferred alarm deletion outside handler has no effect") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  test.actor.cancelDeferredAlarmDeletion();

  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("canceling deferred alarm deletion outside handler edge case") {
  // Presumably harmless to cancel deletion if the client requests it after the handler ends but
  // before the event loop runs the commit code?  Trying to cancel deletion outside the handler is
  // a bit of a contract violation anyway -- maybe we should just assert against it?
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.actor.cancelDeferredAlarmDeletion();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("canceling deferred alarm deletion is idempotent") {
  // Not sure if important, but matches ActorCache behavior.
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

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

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto handle = test.actor.armAlarmHandler(oneMs, false);

    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
  }
  // TODO(soon): shouldn't call setAlarm to clear on rejected commit?  Or OK to assume client will
  // detect and call cancelDeferredAlarmDeletion() on failure?
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->reject(KJ_EXCEPTION(FAILED, "a_rejected_commit"));

  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", promise.wait(test.ws));
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
