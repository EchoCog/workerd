// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "trace-streaming.h"

#include <workerd/util/uuid.h>

namespace workerd {

// ======================================================================================
// TailIDs
namespace {
// The UuidId implementation is really intended only for testing and local development.
// In production, it likely makes more sense to use a RayID or something that can be
// better correlated to other diagnostic and tracing mechanisms, and that can be better
// guaranteed to be sufficiently unique across the entire production environment.
class UuidId final: public StreamingTrace::IdFactory::Id {
public:
  UuidId(): uuid(randomUUID(kj::none)) {}
  UuidId(kj::String value): uuid(kj::mv(value)) {}
  KJ_DISALLOW_COPY_AND_MOVE(UuidId);

  kj::String toString() const override {
    return kj::str(uuid);
  }

  bool equals(const Id& other) const override {
    return uuid == other.toString();
  }

  kj::Own<Id> clone() const override {
    return kj::heap<UuidId>(toString());
  }

private:
  kj::String uuid;
};

class UuidIdFactory final: public StreamingTrace::IdFactory {
public:
  kj::Own<Id> newId() override {
    return kj::heap<UuidId>();
  }
};

UuidIdFactory uuidIdFactory;
}  // namespace

kj::Own<StreamingTrace::IdFactory> StreamingTrace::IdFactory::newUuidIdFactory() {
  return kj::Own<UuidIdFactory>(&uuidIdFactory, kj::NullDisposer::instance);
}

kj::Own<const StreamingTrace::IdFactory::Id> StreamingTrace::IdFactory::newIdFromString(
    kj::StringPtr str) {
  // This is cheating a bit. We're not actually creating a UUID here but the UuidId class
  // is really just a wrapper around a string so we can safely use it here.
  return kj::heap<const UuidId>(kj::str(str));
}

// ======================================================================================
// StreamingTrace

namespace {
constexpr trace::SpanClose::Outcome eventOutcomeToSpanOutcome(const EventOutcome& outcome) {
  switch (outcome) {
    case EventOutcome::UNKNOWN:
      return trace::SpanClose::Outcome::UNKNOWN;
    case EventOutcome::OK:
      return trace::SpanClose::Outcome::OK;
    case EventOutcome::RESPONSE_STREAM_DISCONNECTED:
      [[fallthrough]];
    case EventOutcome::CANCELED:
      return trace::SpanClose::Outcome::CANCELED;
    case EventOutcome::LOAD_SHED:
      [[fallthrough]];
    case EventOutcome::EXCEEDED_CPU:
      [[fallthrough]];
    case EventOutcome::KILL_SWITCH:
      [[fallthrough]];
    case EventOutcome::DAEMON_DOWN:
      [[fallthrough]];
    case EventOutcome::SCRIPT_NOT_FOUND:
      [[fallthrough]];
    case EventOutcome::EXCEEDED_MEMORY:
      [[fallthrough]];
    case EventOutcome::EXCEPTION:
      return trace::SpanClose::Outcome::EXCEPTION;
  }
  KJ_UNREACHABLE;
}
}  // namespace

struct StreamingTrace::Impl {
  kj::Own<const IdFactory::Id> id;
  trace::Onset onsetInfo;
  StreamingTrace::Delegate delegate;
  const StreamingTrace::TimeProvider& timeProvider;
  uint32_t spanCounter = 0;
  uint32_t sequenceCounter = 0;

  Impl(kj::Own<const IdFactory::Id> id,
      trace::Onset&& onset,
      StreamingTrace::Delegate delegate,
      const TimeProvider& timeProvider)
      : id(kj::mv(id)),
        onsetInfo(kj::mv(onset)),
        delegate(kj::mv(delegate)),
        timeProvider(timeProvider) {}
};

kj::Own<StreamingTrace> StreamingTrace::create(IdFactory& idFactory,
    trace::Onset&& onset,
    Delegate delegate,
    const TimeProvider& timeProvider) {
  return kj::heap<StreamingTrace>(idFactory.newId(), kj::mv(onset), kj::mv(delegate), timeProvider);
}

StreamingTrace::StreamingTrace(kj::Own<const IdFactory::Id> id,
    trace::Onset&& onset,
    Delegate delegate,
    const TimeProvider& timeProvider)
    : impl(kj::heap<StreamingTrace::Impl>(
          kj::mv(id), kj::mv(onset), kj::mv(delegate), timeProvider)) {}

StreamingTrace::~StreamingTrace() noexcept(false) {
  if (impl != kj::none) {
    // If the streaming tracing is dropped without having an outcome explicitly
    // specified, the outcome is explicitly set to unknown.
    setOutcome(trace::Outcome(EventOutcome::UNKNOWN));
  }
  // Stage spans should be closed by calling setOutcome above
  KJ_ASSERT(spans.empty(), "all stage spans must be closed before the trace is destroyed");
}

void StreamingTrace::setEventInfo(trace::EventInfo&& eventInfo) {
  auto& i = KJ_ASSERT_NONNULL(impl, "the streaming trace is closed");
  KJ_ASSERT(i->onsetInfo.info == kj::none, "the onset event info can only be set once");
  i->onsetInfo.info = kj::mv(eventInfo);
  StreamEvent event(
      i->id->toString(), {}, i->timeProvider.getNow(), getNextSequence(), i->onsetInfo.clone());
  addStreamEvent(kj::mv(event));
}

void StreamingTrace::setOutcome(trace::Outcome&& outcome) {
  KJ_IF_SOME(i, impl) {
    // If the event info was never set on the streaming trace, setting the outcome
    // is a non-op.
    if (i->onsetInfo.info == kj::none) {
      impl = kj::none;
      return;
    }

    for (auto& span: spans) {
      span.setOutcome(eventOutcomeToSpanOutcome(outcome.outcome));
    }
    KJ_ASSERT(spans.empty(), "all stage spans must be closed before the trace is destroyed");
    StreamEvent event(
        i->id->toString(), {}, i->timeProvider.getNow(), getNextSequence(), kj::mv(outcome));
    addStreamEvent(kj::mv(event));

    // Then close out the stream by destroying the impl
    impl = kj::none;
  }
}

void StreamingTrace::addDropped(uint32_t start, uint32_t end) {
  KJ_IF_SOME(i, impl) {
    KJ_REQUIRE_NONNULL(i->onsetInfo.info, "the event info must be set before other events");
    StreamEvent event(i->id->toString(), {}, i->timeProvider.getNow(), getNextSequence(),
        trace::Dropped{start, end});
    addStreamEvent(kj::mv(event));
  }
}

kj::Maybe<kj::Own<StreamingTrace::Span>> StreamingTrace::newChildSpan(trace::Tags tags) {
  KJ_IF_SOME(i, impl) {
    KJ_REQUIRE_NONNULL(i->onsetInfo.info, "the event info must be set before other events");
    return kj::heap<StreamingTrace::Span>(spans, *this, 0, kj::mv(tags));
  }
  return kj::none;
}

void StreamingTrace::addLog(trace::LogV2&& log) {
  KJ_IF_SOME(i, impl) {
    KJ_REQUIRE_NONNULL(i->onsetInfo.info, "the event info must be set before other events");
    StreamEvent event(
        i->id->toString(), {}, i->timeProvider.getNow(), getNextSequence(), kj::mv(log));
    addStreamEvent(kj::mv(event));
  }
}

void StreamingTrace::addException(trace::Exception&& exception) {
  KJ_IF_SOME(i, impl) {
    KJ_REQUIRE_NONNULL(i->onsetInfo.info, "the event info must be set before other events");
    StreamEvent event(
        i->id->toString(), {}, i->timeProvider.getNow(), getNextSequence(), kj::mv(exception));
    addStreamEvent(kj::mv(event));
  }
}

void StreamingTrace::addDiagnosticChannelEvent(trace::DiagnosticChannelEvent&& dce) {
  KJ_IF_SOME(i, impl) {
    KJ_REQUIRE_NONNULL(i->onsetInfo.info, "the event info must be set before other events");
    StreamEvent event(
        i->id->toString(), {}, i->timeProvider.getNow(), getNextSequence(), kj::mv(dce));
    addStreamEvent(kj::mv(event));
  }
}

void StreamingTrace::addMark(kj::StringPtr mark) {
  KJ_IF_SOME(i, impl) {
    KJ_REQUIRE_NONNULL(i->onsetInfo.info, "the event info must be set before other events");
    StreamEvent event(i->id->toString(), {}, i->timeProvider.getNow(), getNextSequence(),
        trace::Mark(kj::str(mark)));
    addStreamEvent(kj::mv(event));
  }
}

void StreamingTrace::addMetrics(trace::Metrics&& metrics) {
  KJ_IF_SOME(i, impl) {
    KJ_REQUIRE_NONNULL(i->onsetInfo.info, "the event info must be set before other events");
    StreamEvent event(
        i->id->toString(), {}, i->timeProvider.getNow(), getNextSequence(), kj::mv(metrics));
    addStreamEvent(kj::mv(event));
  }
}

void StreamingTrace::addSubrequest(trace::Subrequest&& subrequest) {
  KJ_IF_SOME(i, impl) {
    KJ_REQUIRE_NONNULL(i->onsetInfo.info, "the event info must be set before other events");
    StreamEvent event(
        i->id->toString(), {}, i->timeProvider.getNow(), getNextSequence(), kj::mv(subrequest));
    addStreamEvent(kj::mv(event));
  }
}

void StreamingTrace::addSubrequestOutcome(trace::SubrequestOutcome&& outcome) {
  KJ_IF_SOME(i, impl) {
    KJ_REQUIRE_NONNULL(i->onsetInfo.info, "the event info must be set before other events");
    StreamEvent event(
        i->id->toString(), {}, i->timeProvider.getNow(), getNextSequence(), kj::mv(outcome));
    addStreamEvent(kj::mv(event));
  }
}

void StreamingTrace::addCustom(trace::Tags&& tags) {
  KJ_IF_SOME(i, impl) {
    KJ_REQUIRE_NONNULL(i->onsetInfo.info, "the event info must be set before other events");
    StreamEvent event(
        i->id->toString(), {}, i->timeProvider.getNow(), getNextSequence(), kj::mv(tags));
    addStreamEvent(kj::mv(event));
  }
}

uint32_t StreamingTrace::getNextSpanId() {
  auto& i = KJ_ASSERT_NONNULL(impl, "the streaming trace is closed");
  return ++i->spanCounter;
}

uint32_t StreamingTrace::getNextSequence() {
  auto& i = KJ_ASSERT_NONNULL(impl, "the streaming trace is closed");
  return i->sequenceCounter++;
}

void StreamingTrace::addStreamEvent(StreamEvent&& event) {
  KJ_IF_SOME(i, impl) {
    i->delegate(kj::mv(event));
  }
}

kj::Maybe<const StreamingTrace::IdFactory::Id&> StreamingTrace::getId() const {
  KJ_IF_SOME(i, impl) {
    return *i->id;
  }
  return kj::none;
}

// ======================================================================================

struct StreamingTrace::Span::Impl {
  StreamingTrace::Span& self;
  kj::List<Span, &Span::link>& spans;
  StreamingTrace& trace;
  uint32_t id;
  uint32_t parentSpan;
  trace::Tags tags;
  bool eventInfoSet = false;
  Impl(StreamingTrace::Span& self,
      kj::List<Span, &Span::link>& spans,
      StreamingTrace& trace,
      uint32_t parentSpan,
      trace::Tags tags)
      : self(self),
        spans(spans),
        trace(trace),
        id(this->trace.getNextSpanId()),
        parentSpan(parentSpan),
        tags(kj::mv(tags)) {
    spans.add(self);
  }
  KJ_DISALLOW_COPY_AND_MOVE(Impl);
  ~Impl() {
    KJ_ASSERT(self.link.isLinked());
    spans.remove(self);
  }

  StreamEvent makeStreamEvent(auto payload) const {
    auto& tailId = KJ_ASSERT_NONNULL(trace.getId(), "the streaming trace is closed");
    auto& traceImpl = KJ_ASSERT_NONNULL(trace.impl);
    return StreamEvent(tailId.toString(),
        StreamEvent::Span{
          .id = id,
          .parent = parentSpan,
        },
        traceImpl->timeProvider.getNow(), trace.getNextSequence(), kj::mv(payload));
  }
};

StreamingTrace::Span::Span(kj::List<Span, &Span::link>& parentSpans,
    StreamingTrace& trace,
    uint32_t parentSpan,
    trace::Tags tags)
    : impl(kj::heap<Impl>(*this, parentSpans, trace, parentSpan, kj::mv(tags))) {
  KJ_DASSERT(this, link.isLinked());
}

void StreamingTrace::Span::setOutcome(trace::SpanClose::Outcome outcome, trace::Tags tags) {
  KJ_IF_SOME(i, impl) {  // Then close out the stream by destroying the impl
    for (auto& span: spans) {
      span.setOutcome(outcome);
    }
    KJ_ASSERT(spans.empty(), "all child spans must be closed before the parent span is closed");
    i->trace.addStreamEvent(i->makeStreamEvent(trace::SpanClose(outcome, kj::mv(tags))));
    impl = kj::none;
  }
}

StreamingTrace::Span::~Span() noexcept(false) {
  setOutcome(trace::SpanClose::Outcome::UNKNOWN);
  KJ_ASSERT(spans.empty(), "all schild spans must be closed before the trace is destroyed");
}

void StreamingTrace::Span::addLog(trace::LogV2&& log) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(kj::mv(log)));
  }
}

void StreamingTrace::Span::addException(trace::Exception&& exception) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(kj::mv(exception)));
  }
}

void StreamingTrace::Span::addDiagnosticChannelEvent(trace::DiagnosticChannelEvent&& event) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(kj::mv(event)));
  }
}

void StreamingTrace::Span::addMark(kj::StringPtr mark) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(trace::Mark(kj::str(mark))));
  }
}

void StreamingTrace::Span::addMetrics(trace::Metrics&& metrics) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(kj::mv(metrics)));
  }
}

void StreamingTrace::Span::addSubrequest(trace::Subrequest&& subrequest) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(kj::mv(subrequest)));
  }
}

void StreamingTrace::Span::addSubrequestOutcome(trace::SubrequestOutcome&& outcome) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(kj::mv(outcome)));
  }
}

void StreamingTrace::Span::addCustom(trace::Tags&& tags) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(kj::mv(tags)));
  }
}

kj::Maybe<kj::Own<StreamingTrace::Span>> StreamingTrace::Span::newChildSpan(trace::Tags tags) {
  KJ_IF_SOME(i, impl) {
    return kj::heap<Span>(spans, i->trace, i->id, kj::mv(tags));
  }
  return kj::none;
}

// ======================================================================================
// StreamEvent

namespace {
StreamEvent::Span getSpan(const rpc::Trace::StreamEvent::Reader& reader) {
  auto span = reader.getSpan();
  return StreamEvent::Span{
    .id = span.getId(),
    .parent = span.getParent(),
  };
}

StreamEvent::Event getEvent(const rpc::Trace::StreamEvent::Reader& reader) {
  auto event = reader.getEvent();
  switch (event.which()) {
    case rpc::Trace::StreamEvent::Event::Which::ONSET: {
      return trace::Onset(event.getOnset());
    }
    case rpc::Trace::StreamEvent::Event::Which::OUTCOME: {
      return trace::Outcome(event.getOutcome());
    }
    case rpc::Trace::StreamEvent::Event::Which::DROPPED: {
      return trace::Dropped(event.getDropped());
    }
    case rpc::Trace::StreamEvent::Event::Which::SPAN_CLOSE: {
      return trace::SpanClose(event.getSpanClose());
    }
    case rpc::Trace::StreamEvent::Event::Which::LOG: {
      return trace::LogV2(event.getLog());
    }
    case rpc::Trace::StreamEvent::Event::Which::EXCEPTION: {
      return trace::Exception(event.getException());
    }
    case rpc::Trace::StreamEvent::Event::Which::DIAGNOSTIC_CHANNEL: {
      return trace::DiagnosticChannelEvent(event.getDiagnosticChannel());
    }
    case rpc::Trace::StreamEvent::Event::Which::MARK: {
      return trace::Mark(event.getMark());
    }
    case rpc::Trace::StreamEvent::Event::Which::METRICS: {
      auto metrics = event.getMetrics();
      kj::Vector<trace::Metric> vec(metrics.size());
      for (size_t i = 0; i < metrics.size(); i++) {
        trace::Metric metric(metrics[i]);
        vec.add(kj::mv(metric));
      }
      return vec.releaseAsArray();
    }
    case rpc::Trace::StreamEvent::Event::Which::SUBREQUEST: {
      return trace::Subrequest(event.getSubrequest());
    }
    case rpc::Trace::StreamEvent::Event::Which::SUBREQUEST_OUTCOME: {
      return trace::SubrequestOutcome(event.getSubrequestOutcome());
    }
    case rpc::Trace::StreamEvent::Event::Which::CUSTOM: {
      auto custom = event.getCustom();
      kj::Vector<trace::Tag> vec(custom.size());
      for (size_t i = 0; i < custom.size(); i++) {
        trace::Tag tag(custom[i]);
        vec.add(kj::mv(tag));
      }
      return vec.releaseAsArray();
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

StreamEvent::StreamEvent(
    kj::String id, Span span, kj::Date timestampNs, uint32_t sequence, Event event)
    : id(kj::mv(id)),
      span(kj::mv(span)),
      timestampNs(timestampNs),
      sequence(sequence),
      event(kj::mv(event)) {}

StreamEvent::StreamEvent(rpc::Trace::StreamEvent::Reader reader)
    : id(kj::str(reader.getId())),
      span(getSpan(reader)),
      timestampNs(reader.getTimestampNs() * kj::MILLISECONDS + kj::UNIX_EPOCH),
      sequence(reader.getSequence()),
      event(getEvent(reader)) {}

void StreamEvent::copyTo(rpc::Trace::StreamEvent::Builder builder) const {
  builder.setId(id);
  auto spanBuilder = builder.initSpan();
  spanBuilder.setId(span.id);
  spanBuilder.setParent(span.parent);
  builder.setTimestampNs((timestampNs - kj::UNIX_EPOCH) / kj::MILLISECONDS);
  builder.setSequence(sequence);

  auto eventBuilder = builder.initEvent();
  KJ_SWITCH_ONEOF(event) {
    KJ_CASE_ONEOF(onset, trace::Onset) {
      onset.copyTo(eventBuilder.getOnset());
    }
    KJ_CASE_ONEOF(outcome, trace::Outcome) {
      outcome.copyTo(eventBuilder.getOutcome());
    }
    KJ_CASE_ONEOF(dropped, trace::Dropped) {
      dropped.copyTo(eventBuilder.getDropped());
    }
    KJ_CASE_ONEOF(span, trace::SpanClose) {
      span.copyTo(eventBuilder.getSpanClose());
    }
    KJ_CASE_ONEOF(log, trace::LogV2) {
      log.copyTo(eventBuilder.getLog());
    }
    KJ_CASE_ONEOF(exception, trace::Exception) {
      exception.copyTo(eventBuilder.getException());
    }
    KJ_CASE_ONEOF(diagnosticChannelEvent, trace::DiagnosticChannelEvent) {
      diagnosticChannelEvent.copyTo(eventBuilder.getDiagnosticChannel());
    }
    KJ_CASE_ONEOF(mark, trace::Mark) {
      mark.copyTo(eventBuilder.getMark());
    }
    KJ_CASE_ONEOF(metrics, trace::Metrics) {
      auto metricsBuilder = eventBuilder.initMetrics(metrics.size());
      for (size_t i = 0; i < metrics.size(); i++) {
        metrics[i].copyTo(metricsBuilder[i]);
      }
    }
    KJ_CASE_ONEOF(subrequest, trace::Subrequest) {
      subrequest.copyTo(eventBuilder.getSubrequest());
    }
    KJ_CASE_ONEOF(subrequestOutcome, trace::SubrequestOutcome) {
      subrequestOutcome.copyTo(eventBuilder.getSubrequestOutcome());
    }
    KJ_CASE_ONEOF(tags, trace::Tags) {
      auto tagsBuilder = eventBuilder.initCustom(tags.size());
      for (size_t i = 0; i < tags.size(); i++) {
        tags[i].copyTo(tagsBuilder[i]);
      }
    }
  }
}

StreamEvent StreamEvent::clone() const {
  Span maybeNewSpan{
    .id = span.id,
    .parent = span.parent,
  };

  Event newEvent = ([&]() -> Event {
    KJ_SWITCH_ONEOF(event) {
      KJ_CASE_ONEOF(onset, trace::Onset) {
        return onset.clone();
      }
      KJ_CASE_ONEOF(outcome, trace::Outcome) {
        return outcome.clone();
      }
      KJ_CASE_ONEOF(dropped, trace::Dropped) {
        return dropped.clone();
      }
      KJ_CASE_ONEOF(span, trace::SpanClose) {
        return span.clone();
      }
      KJ_CASE_ONEOF(log, trace::LogV2) {
        return log.clone();
      }
      KJ_CASE_ONEOF(exception, trace::Exception) {
        return exception.clone();
      }
      KJ_CASE_ONEOF(diagnosticChannelEvent, trace::DiagnosticChannelEvent) {
        return diagnosticChannelEvent.clone();
      }
      KJ_CASE_ONEOF(mark, trace::Mark) {
        return mark.clone();
      }
      KJ_CASE_ONEOF(metric, trace::Metrics) {
        kj::Vector<trace::Metric> newMetrics(metric.size());
        for (auto& m: metric) {
          newMetrics.add(m.clone());
        }
        return newMetrics.releaseAsArray();
      }
      KJ_CASE_ONEOF(subrequest, trace::Subrequest) {
        return subrequest.clone();
      }
      KJ_CASE_ONEOF(subrequestOutcome, trace::SubrequestOutcome) {
        return subrequestOutcome.clone();
      }
      KJ_CASE_ONEOF(tags, trace::Tags) {
        kj::Vector<trace::Tag> newTags(tags.size());
        for (auto& tag: tags) {
          newTags.add(tag.clone());
        }
        return newTags.releaseAsArray();
      }
    }
    KJ_UNREACHABLE;
  })();

  return StreamEvent(kj::str(id), kj::mv(maybeNewSpan), timestampNs, sequence, kj::mv(newEvent));
}

}  // namespace workerd
