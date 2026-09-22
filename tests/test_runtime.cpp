// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/runtime.hpp"
#include "ifabric/synth.hpp"
#include "ifabric_test.hpp"
#include "process.hpp"

#include <atomic>
#include <thread>

using namespace ifabric;

namespace {

DomainId core_domain() { return DomainId::parse("dom.core").value(); }

IntentDocument document_for(std::uint64_t seed) {
  SynthOptions options;
  options.seed = seed;
  options.leaves = 2 + static_cast<std::size_t>(seed % 3);
  IntentDocument document = synthesize(core_domain(), options);
  document.provenance.description = "generation " + std::to_string(seed);
  return document;
}

ActorId writer(std::string_view name) { return ActorId::parse(std::string(name)).value(); }

struct Fixture {
  ifabric_test::TempDirectory directory;
  std::unique_ptr<IntentRuntime> runtime;

  explicit Fixture(const std::string& label) : directory(label) {
    auto opened = IntentRuntime::create(directory.path(), core_domain());
    runtime = opened.ok() ? std::move(opened.value()) : nullptr;
  }
};

}  // namespace

IFABRIC_TEST(runtime, propose_validate_compare_commit_publish) {
  Fixture fixture("runtime-happy");
  REQUIRE(fixture.runtime != nullptr);
  const auto subscription = fixture.runtime->subscribe();
  REQUIRE(subscription.ok());

  const auto proposal = fixture.runtime->propose(document_for(1), writer("actor.planner"));
  REQUIRE(proposal.ok());
  const auto report = fixture.runtime->validate_proposal(proposal.value());
  REQUIRE(report.ok());
  CHECK(report->passed());
  const auto diff = fixture.runtime->compare_proposal(proposal.value());
  REQUIRE(diff.ok());
  CHECK(!diff->empty());
  const auto inspected = fixture.runtime->inspect_proposal(proposal.value());
  REQUIRE(inspected.ok());
  CHECK_EQ(inspected->state, ProposalState::Validated);
  CHECK_EQ(inspected->base_generation.is_genesis(), true);

  const auto outcome = fixture.runtime->commit_proposal(
      proposal.value(), fixture.runtime->fence(writer("actor.planner")));
  REQUIRE(outcome.ok());
  CHECK_EQ(outcome->reference.number.value(), 1ull);
  CHECK_EQ(outcome->reference.content.hex(), content_digest_of(document_for(1)).hex());

  const auto publication = fixture.runtime->poll(subscription.value());
  REQUIRE(publication.ok());
  REQUIRE(publication->has_value());
  CHECK_EQ(publication->value().reference.number.value(), 1ull);
  const auto again = fixture.runtime->poll(subscription.value());
  REQUIRE(again.ok());
  CHECK(!again->has_value());

  const auto stats = fixture.runtime->stats();
  CHECK_EQ(stats.proposals_created, 1ull);
  CHECK_EQ(stats.proposals_committed, 1ull);
  CHECK_EQ(stats.publications, 1ull);
  CHECK_EQ(stats.head.value(), 1ull);
}

IFABRIC_TEST(runtime, invalid_proposal_never_becomes_authoritative) {
  Fixture fixture("runtime-invalid");
  REQUIRE(fixture.runtime != nullptr);
  SynthOptions options;
  options.defect = SynthDefect::DuplicateIdentity;
  const auto proposal = fixture.runtime->propose(synthesize(core_domain(), options),
                                                 writer("actor.planner"));
  REQUIRE(proposal.ok());
  const auto report = fixture.runtime->validate_proposal(proposal.value());
  REQUIRE(report.ok());
  CHECK(!report->passed());
  const auto commit = fixture.runtime->commit_proposal(
      proposal.value(), fixture.runtime->fence(writer("actor.planner")));
  CHECK_ERROR_CODE(commit.error(), ErrorCode::ProposalRejected);
  CHECK(!fixture.runtime->has_committed());
  CHECK_ERROR_CODE(fixture.runtime->head().error(), ErrorCode::NoCommittedGeneration);
  CHECK_EQ(fixture.runtime->generations(0).value().size(), std::size_t{0});
}

IFABRIC_TEST(runtime, commit_requires_validation) {
  Fixture fixture("runtime-unvalidated");
  REQUIRE(fixture.runtime != nullptr);
  const auto proposal = fixture.runtime->propose(document_for(1), writer("actor.planner"));
  REQUIRE(proposal.ok());
  CHECK_ERROR_CODE(fixture.runtime->compare_proposal(proposal.value()).error(),
                   ErrorCode::ProposalNotValidated);
  const auto commit = fixture.runtime->commit_proposal(
      proposal.value(), fixture.runtime->fence(writer("actor.planner")));
  CHECK_ERROR_CODE(commit.error(), ErrorCode::ProposalNotValidated);
  CHECK(!fixture.runtime->has_committed());
}

IFABRIC_TEST(runtime, stale_base_generation_is_fenced) {
  Fixture fixture("runtime-stale");
  REQUIRE(fixture.runtime != nullptr);
  const auto first = fixture.runtime->propose(document_for(1), writer("actor.planner"));
  const auto second = fixture.runtime->propose(document_for(2), writer("actor.planner"));
  REQUIRE(first.ok());
  REQUIRE(second.ok());
  REQUIRE(fixture.runtime->validate_proposal(first.value()).ok());
  REQUIRE(fixture.runtime->validate_proposal(second.value()).ok());
  REQUIRE(fixture.runtime
              ->commit_proposal(first.value(), fixture.runtime->fence(writer("actor.planner")))
              .ok());
  const auto late = fixture.runtime->commit_proposal(
      second.value(), fixture.runtime->fence(writer("actor.planner")));
  CHECK_ERROR_CODE(late.error(), ErrorCode::StaleGeneration);
  CHECK_EQ(fixture.runtime->stats().stale_generation_rejections, 1ull);
  CHECK_EQ(fixture.runtime->head().value().number.value(), 1ull);
}

IFABRIC_TEST(runtime, fence_token_must_match_the_proposal) {
  Fixture fixture("runtime-fence");
  REQUIRE(fixture.runtime != nullptr);
  const auto proposal = fixture.runtime->propose(document_for(1), writer("actor.planner"));
  REQUIRE(proposal.ok());
  REQUIRE(fixture.runtime->validate_proposal(proposal.value()).ok());
  FenceToken wrong_epoch = fixture.runtime->fence(writer("actor.planner"));
  wrong_epoch.expected_epoch = Epoch(wrong_epoch.expected_epoch.value() + 3);
  CHECK_ERROR_CODE(
      fixture.runtime->commit_proposal(proposal.value(), wrong_epoch).error(),
      ErrorCode::StaleEpoch);
  FenceToken wrong_writer = fixture.runtime->fence(writer("actor.other"));
  CHECK_ERROR_CODE(
      fixture.runtime->commit_proposal(proposal.value(), wrong_writer).error(),
      ErrorCode::Unauthorized);
  FenceToken wrong_generation = fixture.runtime->fence(writer("actor.planner"));
  wrong_generation.expected_generation = GenerationNumber::from(7).value();
  CHECK_ERROR_CODE(
      fixture.runtime->commit_proposal(proposal.value(), wrong_generation).error(),
      ErrorCode::StaleGeneration);
  CHECK(!fixture.runtime->has_committed());
}

IFABRIC_TEST(runtime, cancelled_proposal_never_publishes) {
  Fixture fixture("runtime-cancel");
  REQUIRE(fixture.runtime != nullptr);
  const auto subscription = fixture.runtime->subscribe();
  REQUIRE(subscription.ok());
  const auto proposal = fixture.runtime->propose(document_for(1), writer("actor.planner"));
  REQUIRE(proposal.ok());
  REQUIRE(fixture.runtime->validate_proposal(proposal.value()).ok());
  REQUIRE(fixture.runtime->cancel_proposal(proposal.value(), "operator aborted").ok());
  const auto commit = fixture.runtime->commit_proposal(
      proposal.value(), fixture.runtime->fence(writer("actor.planner")));
  CHECK_ERROR_CODE(commit.error(), ErrorCode::Cancelled);
  CHECK(!fixture.runtime->has_committed());
  const auto publication = fixture.runtime->poll(subscription.value());
  REQUIRE(publication.ok());
  CHECK(!publication->has_value());
  CHECK_EQ(fixture.runtime->stats().publications, 0ull);
}

IFABRIC_TEST(runtime, committed_generation_cannot_be_cancelled_or_recommitted) {
  Fixture fixture("runtime-immutable");
  REQUIRE(fixture.runtime != nullptr);
  const auto proposal = fixture.runtime->propose(document_for(1), writer("actor.planner"));
  REQUIRE(proposal.ok());
  REQUIRE(fixture.runtime->validate_proposal(proposal.value()).ok());
  REQUIRE(fixture.runtime
              ->commit_proposal(proposal.value(), fixture.runtime->fence(writer("actor.planner")))
              .ok());
  CHECK_EQ(fixture.runtime->cancel_proposal(proposal.value(), "too late").code,
           ErrorCode::InvariantViolation);
  const auto repeat = fixture.runtime->commit_proposal(
      proposal.value(), fixture.runtime->fence(writer("actor.planner")));
  CHECK_ERROR_CODE(repeat.error(), ErrorCode::InvariantViolation);
  CHECK_EQ(fixture.runtime->head().value().number.value(), 1ull);
}

IFABRIC_TEST(runtime, shutdown_stops_admission) {
  Fixture fixture("runtime-shutdown");
  REQUIRE(fixture.runtime != nullptr);
  const auto proposal = fixture.runtime->propose(document_for(1), writer("actor.planner"));
  REQUIRE(proposal.ok());
  REQUIRE(fixture.runtime->validate_proposal(proposal.value()).ok());
  fixture.runtime->request_shutdown();
  CHECK(fixture.runtime->shutting_down());
  CHECK_ERROR_CODE(fixture.runtime->propose(document_for(2), writer("actor.planner")).error(),
                   ErrorCode::Cancelled);
  const auto commit = fixture.runtime->commit_proposal(
      proposal.value(), fixture.runtime->fence(writer("actor.planner")));
  CHECK_ERROR_CODE(commit.error(), ErrorCode::Cancelled);
  CHECK(!fixture.runtime->has_committed());
  CHECK_ERROR_CODE(fixture.runtime->subscribe().error(), ErrorCode::Cancelled);
}

IFABRIC_TEST(runtime, subscription_and_stale_consumer_apis) {
  Fixture fixture("runtime-subscriptions");
  REQUIRE(fixture.runtime != nullptr);
  const auto subscription = fixture.runtime->subscribe();
  REQUIRE(subscription.ok());
  CHECK_ERROR_CODE(fixture.runtime->acquire(GenerationNumber::from(1).value()).error(),
                   ErrorCode::NoCommittedGeneration);
  const auto outcome = fixture.runtime->submit(document_for(1), writer("actor.planner"),
                                               fixture.runtime->fence(writer("actor.planner")));
  REQUIRE(outcome.ok());
  const auto acquired = fixture.runtime->acquire(GenerationNumber::from(1).value());
  REQUIRE(acquired.ok());
  CHECK_EQ(acquired->number.value(), 1ull);
  CHECK(fixture.runtime->require_content(acquired->content).ok());
  CHECK_ERROR_CODE(
      fixture.runtime->require_content(ContentDigest::of("something else")).error(),
      ErrorCode::StaleGeneration);
  CHECK_ERROR_CODE(fixture.runtime->acquire(GenerationNumber::from(2).value()).error(),
                   ErrorCode::StaleGeneration);
  const auto publication = fixture.runtime->poll(subscription.value());
  REQUIRE(publication.ok());
  REQUIRE(publication->has_value());
  CHECK_EQ(publication->value().sequence, 1ull);
  REQUIRE(fixture.runtime->unsubscribe(subscription.value()).ok());
  CHECK_ERROR_CODE(fixture.runtime->poll(subscription.value()).error(),
                   ErrorCode::InvalidArgument);
}

IFABRIC_TEST(runtime, submit_rejects_an_invalid_document_without_commit) {
  Fixture fixture("runtime-submit-invalid");
  REQUIRE(fixture.runtime != nullptr);
  SynthOptions options;
  options.defect = SynthDefect::DanglingReference;
  const auto outcome = fixture.runtime->submit(synthesize(core_domain(), options),
                                               writer("actor.planner"),
                                               fixture.runtime->fence(writer("actor.planner")));
  CHECK_ERROR_CODE(outcome.error(), ErrorCode::ProposalRejected);
  CHECK(!fixture.runtime->has_committed());
}

IFABRIC_TEST(runtime, empty_diff_policy_is_honoured) {
  ifabric_test::TempDirectory directory("runtime-empty-diff");
  IntentRuntime::Options options;
  options.reject_empty_diff = true;
  auto runtime = IntentRuntime::create(directory.path(), core_domain(), options);
  REQUIRE(runtime.ok());
  const auto first = runtime.value()->submit(document_for(1), writer("actor.planner"),
                                             runtime.value()->fence(writer("actor.planner")));
  REQUIRE(first.ok());
  const auto identical = runtime.value()->submit(document_for(1), writer("actor.planner"),
                                                 runtime.value()->fence(writer("actor.planner")));
  CHECK_ERROR_CODE(identical.error(), ErrorCode::EmptyDiffRejected);
  CHECK_EQ(runtime.value()->head().value().number.value(), 1ull);
}

IFABRIC_TEST(runtime, failed_commit_leaves_the_store_untouched) {
  ifabric_test::TempDirectory directory("runtime-atomicity");
  {
    auto runtime = IntentRuntime::create(directory.path(), core_domain());
    REQUIRE(runtime.ok());
    REQUIRE(runtime.value()
                ->submit(document_for(1), writer("actor.planner"),
                         runtime.value()->fence(writer("actor.planner")))
                .ok());
    SynthOptions options;
    options.defect = SynthDefect::ImpossibleRedundancy;
    CHECK_ERROR_CODE(runtime.value()
                         ->submit(synthesize(core_domain(), options), writer("actor.planner"),
                                  runtime.value()->fence(writer("actor.planner")))
                         .error(),
                     ErrorCode::ProposalRejected);
    CHECK_EQ(runtime.value()->head().value().number.value(), 1ull);
  }
  auto reopened = IntentRuntime::open(directory.path());
  REQUIRE(reopened.ok());
  CHECK_EQ(reopened.value()->head().value().number.value(), 1ull);
  CHECK_EQ(reopened.value()->generations(0).value().size(), std::size_t{1});
  CHECK(reopened.value()->verify_store().ok());
  CHECK(!reopened.value()->recovered());
}

IFABRIC_TEST(runtime, restart_preserves_generations_and_bumps_the_epoch) {
  ifabric_test::TempDirectory directory("runtime-restart");
  Epoch first_epoch;
  IncarnationId first_incarnation;
  {
    auto runtime = IntentRuntime::create(directory.path(), core_domain());
    REQUIRE(runtime.ok());
    first_epoch = runtime.value()->epoch();
    first_incarnation = runtime.value()->incarnation();
    REQUIRE(runtime.value()
                ->submit(document_for(1), writer("actor.planner"),
                         runtime.value()->fence(writer("actor.planner")))
                .ok());
  }
  auto reopened = IntentRuntime::open(directory.path());
  REQUIRE(reopened.ok());
  CHECK(reopened.value()->epoch().value() > first_epoch.value());
  CHECK_NE(reopened.value()->incarnation().str(), first_incarnation.str());
  const auto document = reopened.value()->head_document();
  REQUIRE(document.ok());
  CHECK_EQ(content_digest_of(document.value()).hex(), content_digest_of(document_for(1)).hex());
}

IFABRIC_TEST(runtime, concurrent_commits_are_serialised_and_lost_updates_are_impossible) {
  ifabric_test::TempDirectory directory("runtime-race");
  auto runtime = IntentRuntime::create(directory.path(), core_domain());
  REQUIRE(runtime.ok());
  constexpr int kThreads = 8;
  constexpr int kCommitsPerThread = 4;
  std::atomic<int> committed{0};
  std::atomic<int> stale{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&, thread] {
      for (int attempt = 0; attempt < kCommitsPerThread; ++attempt) {
        for (int retry = 0; retry < 4096; ++retry) {
          const ActorId actor = writer("actor.thread-" + std::to_string(thread));
          const FenceToken token = runtime.value()->fence(actor);
          const auto outcome = runtime.value()->submit(
              document_for(static_cast<std::uint64_t>(thread * 100 + attempt) + 1), actor, token);
          if (outcome.ok()) {
            committed.fetch_add(1);
            break;
          }
          if (outcome.error().code == ErrorCode::StaleGeneration ||
              outcome.error().code == ErrorCode::StaleWriter) {
            stale.fetch_add(1);
            continue;
          }
          break;
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK_EQ(committed.load(), kThreads * kCommitsPerThread);
  CHECK_EQ(runtime.value()->head().value().number.value(),
           static_cast<std::uint64_t>(kThreads * kCommitsPerThread));
  const auto summaries = runtime.value()->generations(0);
  REQUIRE(summaries.ok());
  CHECK_EQ(summaries->size(), std::size_t{kThreads * kCommitsPerThread});
  // The lineage is strictly monotonic with no gaps and no duplicates.
  for (std::size_t i = 0; i < summaries->size(); ++i) {
    CHECK_EQ(summaries.value()[i].number.value(),
             static_cast<std::uint64_t>(kThreads * kCommitsPerThread - i));
  }
  CHECK(runtime.value()->verify_store().ok());
  CHECK(stale.load() > 0);
}

IFABRIC_TEST(runtime, concurrent_commit_of_one_proposal_succeeds_exactly_once) {
  Fixture fixture("runtime-single-winner");
  REQUIRE(fixture.runtime != nullptr);
  const auto proposal = fixture.runtime->propose(document_for(1), writer("actor.planner"));
  REQUIRE(proposal.ok());
  REQUIRE(fixture.runtime->validate_proposal(proposal.value()).ok());
  std::atomic<int> successes{0};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 6; ++i) {
    threads.emplace_back([&] {
      const auto outcome = fixture.runtime->commit_proposal(
          proposal.value(), fixture.runtime->fence(writer("actor.planner")));
      if (outcome.ok()) {
        successes.fetch_add(1);
      } else {
        failures.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK_EQ(successes.load(), 1);
  CHECK_EQ(failures.load(), 5);
  CHECK_EQ(fixture.runtime->head().value().number.value(), 1ull);
  CHECK(fixture.runtime->verify_store().ok());
}

IFABRIC_TEST(runtime, proposal_limit_is_bounded_and_terminal_proposals_are_retired) {
  ifabric_test::TempDirectory directory("runtime-proposal-limit");
  IntentRuntime::Options options;
  options.max_pending_proposals = 4;
  auto runtime = IntentRuntime::create(directory.path(), core_domain(), options);
  REQUIRE(runtime.ok());
  for (int i = 0; i < 4; ++i) {
    REQUIRE(runtime.value()
                ->submit(document_for(static_cast<std::uint64_t>(i) + 1), writer("actor.planner"),
                         runtime.value()->fence(writer("actor.planner")))
                .ok());
  }
  // Terminal proposals are retired, so the bound never blocks progress.
  CHECK(runtime.value()
            ->submit(document_for(99), writer("actor.planner"),
                     runtime.value()->fence(writer("actor.planner")))
            .ok());
  CHECK(runtime.value()->pending_proposals().empty());
}

IFABRIC_TEST(runtime, eviction_keeps_live_proposals_addressable) {
  ifabric_test::TempDirectory directory("runtime-eviction-index");
  IntentRuntime::Options options;
  options.max_pending_proposals = 4;
  auto runtime = IntentRuntime::create(directory.path(), core_domain(), options);
  REQUIRE(runtime.ok());
  std::vector<ProposalId> live;
  for (int round = 0; round < 12; ++round) {
    // Interleave terminal proposals with a live one that must stay addressable.
    const auto terminal = runtime.value()->propose(document_for(static_cast<std::uint64_t>(round) + 1),
                                                   writer("actor.planner"));
    REQUIRE(terminal.ok());
    REQUIRE(runtime.value()->validate_proposal(terminal.value()).ok());
    REQUIRE(runtime.value()
                ->commit_proposal(terminal.value(), runtime.value()->fence(writer("actor.planner")))
                .ok());
    const auto pending = runtime.value()->propose(document_for(1000 + static_cast<std::uint64_t>(round)),
                                                  writer("actor.planner"));
    REQUIRE(pending.ok());
    live.push_back(pending.value());
    const auto inspected = runtime.value()->inspect_proposal(live.back());
    REQUIRE(inspected.ok());
    CHECK_EQ(inspected->state, ProposalState::Proposed);
    CHECK_EQ(inspected->id.str(), live.back().str());
    // Retire the live proposal by cancelling it, then confirm it is still the
    // identity that resolves.
    REQUIRE(runtime.value()->cancel_proposal(live.back(), "retired").ok());
    const auto after = runtime.value()->inspect_proposal(live.back());
    REQUIRE(after.ok());
    CHECK_EQ(after->state, ProposalState::Cancelled);
  }
  CHECK_EQ(runtime.value()->head().value().number.value(), 12ull);
  CHECK(runtime.value()->pending_proposals().empty());
}

IFABRIC_TEST(runtime, domain_mismatch_is_rejected) {
  Fixture fixture("runtime-domain");
  REQUIRE(fixture.runtime != nullptr);
  IntentDocument document = document_for(1);
  document.domain = DomainId::parse("dom.other").value();
  CHECK_ERROR_CODE(fixture.runtime->propose(document, writer("actor.planner")).error(),
                   ErrorCode::UnknownDomain);
}

IFABRIC_TEST(runtime, anonymous_writers_are_rejected) {
  Fixture fixture("runtime-anonymous");
  REQUIRE(fixture.runtime != nullptr);
  CHECK_ERROR_CODE(fixture.runtime->propose(document_for(1), ActorId()).error(),
                   ErrorCode::Unauthorized);
}

IFABRIC_TEST(runtime, subscriber_queues_are_bounded) {
  ifabric_test::TempDirectory directory("runtime-subscriber-bound");
  IntentRuntime::Options options;
  options.subscriber_queue = 2;
  auto runtime = IntentRuntime::create(directory.path(), core_domain(), options);
  REQUIRE(runtime.ok());
  const auto subscription = runtime.value()->subscribe();
  REQUIRE(subscription.ok());
  for (int i = 0; i < 6; ++i) {
    REQUIRE(runtime.value()
                ->submit(document_for(static_cast<std::uint64_t>(i) + 1), writer("actor.planner"),
                         runtime.value()->fence(writer("actor.planner")))
                .ok());
  }
  const auto stats = runtime.value()->stats();
  CHECK_EQ(stats.subscription_drops, 4ull);
  int seen = 0;
  for (;;) {
    const auto publication = runtime.value()->poll(subscription.value());
    REQUIRE(publication.ok());
    if (!publication->has_value()) {
      break;
    }
    ++seen;
  }
  CHECK_EQ(seen, 2);
}
