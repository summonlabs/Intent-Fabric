// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The authoritative intent runtime for one intent domain.
//
// Transaction shape: propose -> validate -> compare -> commit generation ->
// publish. Exactly one committed generation is authoritative at any moment,
// committed generations are immutable, and a failed proposal never partially
// mutates authoritative desired state.
//
// Authority fencing: every proposal captures the head generation, epoch and
// incarnation observed when it was created. A proposal created under a previous
// epoch or incarnation, or against a superseded head generation, cannot commit.

#ifndef IFABRIC_RUNTIME_HPP
#define IFABRIC_RUNTIME_HPP

#include "ifabric/diff.hpp"
#include "ifabric/store.hpp"
#include "ifabric/validate.hpp"

#include <deque>
#include <mutex>
#include <unordered_map>

namespace ifabric {

enum class ProposalState : std::uint8_t {
  Proposed = 0,
  Validated = 1,
  Rejected = 2,
  Committed = 3,
  Cancelled = 4
};
std::string_view to_string(ProposalState value) noexcept;

struct Proposal {
  ProposalId id;
  ActorId actor;
  DomainId domain;
  GenerationNumber base_generation;
  Epoch epoch;
  IncarnationId incarnation;
  Timestamp created_at{};
  ProposalState state = ProposalState::Proposed;
  ContentDigest content;
  DocumentDigest document;
  IntentDocument intent;
  ValidationReport report;
  SemanticDiff diff;
  std::string rejection_reason;

  JsonValue to_json() const;
  std::string render() const;
};

struct Publication {
  GenerationRef reference;
  std::uint64_t sequence = 0;
  std::uint64_t dropped = 0;
};

struct CommitOutcome {
  GenerationRef reference;
  SemanticDiff diff;
  Impact impact = Impact::None;
};

struct RuntimeStats {
  std::uint64_t proposals_created = 0;
  std::uint64_t proposals_rejected = 0;
  std::uint64_t proposals_committed = 0;
  std::uint64_t proposals_cancelled = 0;
  std::uint64_t commit_attempts = 0;
  std::uint64_t commit_failures = 0;
  std::uint64_t stale_generation_rejections = 0;
  std::uint64_t stale_epoch_rejections = 0;
  std::uint64_t publications = 0;
  std::uint64_t subscription_drops = 0;
  std::size_t pending_proposals = 0;
  std::size_t subscribers = 0;
  GenerationNumber head;

  JsonValue to_json() const;
  std::string render() const;
};

class IntentRuntime {
 public:
  struct Options {
    StoreOptions store;
    std::size_t max_pending_proposals = kMaxPendingProposals;
    std::size_t max_subscribers = kMaxSubscribers;
    std::size_t subscriber_queue = kMaxSubscriberQueue;
    bool reject_empty_diff = false;
    bool publish_on_commit = true;
  };

  ~IntentRuntime();
  IntentRuntime(const IntentRuntime&) = delete;
  IntentRuntime& operator=(const IntentRuntime&) = delete;

  static Result<std::unique_ptr<IntentRuntime>> create(const Path& directory, const DomainId& domain,
                                                       const Options& options = {});
  static Result<std::unique_ptr<IntentRuntime>> open(const Path& directory,
                                                     const Options& options = {});

  const DomainId& domain() const noexcept { return store_->domain(); }
  Epoch epoch() const noexcept { return store_->epoch(); }
  const IncarnationId& incarnation() const noexcept { return store_->incarnation(); }
  bool recovered() const noexcept { return store_->recovered(); }
  const std::string& recovery_note() const noexcept { return store_->recovery_note(); }

  // Current fence a writer must present.
  FenceToken fence(const ActorId& writer) const;

  bool has_committed() const;
  Result<GenerationRef> head() const;
  Result<IntentDocument> head_document() const;
  Result<IntentDocument> document_at(const GenerationNumber& number) const;
  Result<std::vector<GenerationSummary>> generations(std::size_t limit) const;
  VerificationReport verify_store() const;

  // Transaction stages.
  Result<ProposalId> propose(IntentDocument document, const ActorId& actor);
  Result<ValidationReport> validate_proposal(const ProposalId& id);
  Result<SemanticDiff> compare_proposal(const ProposalId& id);
  Result<CommitOutcome> commit_proposal(const ProposalId& id, const FenceToken& fence);
  Result<CommitOutcome> submit(IntentDocument document, const ActorId& actor,
                               const FenceToken& fence);

  Result<Proposal> inspect_proposal(const ProposalId& id) const;
  Error cancel_proposal(const ProposalId& id, std::string reason);
  std::vector<Proposal> pending_proposals() const;

  // Subscription / read APIs for downstream runtimes.
  Result<std::uint64_t> subscribe();
  Error unsubscribe(std::uint64_t subscription);
  Result<std::optional<Publication>> poll(std::uint64_t subscription);
  // Rejects a consumer that asks for a generation newer than the committed one.
  Result<GenerationRef> acquire(const GenerationNumber& min_generation) const;
  // Rejects a consumer holding a generation whose content has been superseded.
  Result<GenerationRef> require_content(const ContentDigest& expected) const;

  // Shutdown: admission stops, no further generation can be published.
  void request_shutdown();
  bool shutting_down() const;
  RuntimeStats stats() const;

 private:
  IntentRuntime() = default;

  struct Subscriber {
    std::uint64_t id = 0;
    std::deque<Publication> queue;
    std::uint64_t dropped = 0;
  };

  Error admit(const char* operation) const;
  Proposal* find(const ProposalId& id);
  const Proposal* find(const ProposalId& id) const;
  Error evict_if_needed();
  void publish_locked(const GenerationRef& reference);

  std::unique_ptr<IntentStore> store_;
  Options options_;
  mutable std::mutex mutex_;
  std::vector<Proposal> proposals_;
  std::unordered_map<std::string, std::size_t> proposal_index_;
  std::vector<Subscriber> subscribers_;
  std::uint64_t next_subscription_ = 1;
  std::uint64_t publication_sequence_ = 0;
  bool shutting_down_ = false;
  RuntimeStats stats_;
};

}  // namespace ifabric

#endif  // IFABRIC_RUNTIME_HPP
