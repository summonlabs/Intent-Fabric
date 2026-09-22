// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/runtime.hpp"

#include <algorithm>

namespace ifabric {

std::string_view to_string(ProposalState value) noexcept {
  switch (value) {
    case ProposalState::Proposed: return "proposed";
    case ProposalState::Validated: return "validated";
    case ProposalState::Rejected: return "rejected";
    case ProposalState::Committed: return "committed";
    case ProposalState::Cancelled: return "cancelled";
  }
  return "unknown";
}

JsonValue Proposal::to_json() const {
  JsonValue out;
  (void)out.set("id", JsonValue::string(id.str()));
  (void)out.set("actor", JsonValue::string(actor.str()));
  (void)out.set("domain", JsonValue::string(domain.str()));
  (void)out.set("state", JsonValue::string(std::string(to_string(state))));
  (void)out.set("base_generation", JsonValue::string(base_generation.str()));
  (void)out.set("epoch", JsonValue::string(epoch.str()));
  (void)out.set("incarnation", JsonValue::string(incarnation.str()));
  (void)out.set("created_at", JsonValue::string(format_timestamp(created_at)));
  (void)out.set("content_digest", JsonValue::string(content.hex()));
  (void)out.set("document_digest", JsonValue::string(document.hex()));
  if (!rejection_reason.empty()) {
    (void)out.set("rejection_reason", JsonValue::string(rejection_reason));
  }
  (void)out.set("validation", report.to_json());
  (void)out.set("diff", diff.to_json());
  return out;
}

std::string Proposal::render() const {
  std::string out = id.str();
  out.append(" [");
  out.append(to_string(state));
  out.append("] actor=");
  out.append(actor.str());
  out.append(" base=");
  out.append(base_generation.str());
  out.append(" epoch=");
  out.append(epoch.str());
  out.append(" content=");
  out.append(content.short_hex());
  if (!rejection_reason.empty()) {
    out.append("\n  rejected: ");
    out.append(rejection_reason);
  }
  return out;
}

JsonValue RuntimeStats::to_json() const {
  JsonValue out;
  (void)out.set("proposals_created",
                JsonValue::integer(static_cast<std::int64_t>(proposals_created)));
  (void)out.set("proposals_rejected",
                JsonValue::integer(static_cast<std::int64_t>(proposals_rejected)));
  (void)out.set("proposals_committed",
                JsonValue::integer(static_cast<std::int64_t>(proposals_committed)));
  (void)out.set("proposals_cancelled",
                JsonValue::integer(static_cast<std::int64_t>(proposals_cancelled)));
  (void)out.set("commit_attempts", JsonValue::integer(static_cast<std::int64_t>(commit_attempts)));
  (void)out.set("commit_failures", JsonValue::integer(static_cast<std::int64_t>(commit_failures)));
  (void)out.set("stale_generation_rejections",
                JsonValue::integer(static_cast<std::int64_t>(stale_generation_rejections)));
  (void)out.set("stale_epoch_rejections",
                JsonValue::integer(static_cast<std::int64_t>(stale_epoch_rejections)));
  (void)out.set("publications", JsonValue::integer(static_cast<std::int64_t>(publications)));
  (void)out.set("subscription_drops",
                JsonValue::integer(static_cast<std::int64_t>(subscription_drops)));
  (void)out.set("pending_proposals",
                JsonValue::integer(static_cast<std::int64_t>(pending_proposals)));
  (void)out.set("subscribers", JsonValue::integer(static_cast<std::int64_t>(subscribers)));
  (void)out.set("head", JsonValue::string(head.str()));
  return out;
}

std::string RuntimeStats::render() const {
  std::string out = "head=";
  out.append(head.is_genesis() ? std::string("<none>") : head.str());
  out.append(" proposals: created=");
  out.append(std::to_string(proposals_created));
  out.append(" committed=");
  out.append(std::to_string(proposals_committed));
  out.append(" rejected=");
  out.append(std::to_string(proposals_rejected));
  out.append(" cancelled=");
  out.append(std::to_string(proposals_cancelled));
  out.append(" pending=");
  out.append(std::to_string(pending_proposals));
  out.append("\ncommits: attempts=");
  out.append(std::to_string(commit_attempts));
  out.append(" failures=");
  out.append(std::to_string(commit_failures));
  out.append(" stale-generation=");
  out.append(std::to_string(stale_generation_rejections));
  out.append(" stale-epoch=");
  out.append(std::to_string(stale_epoch_rejections));
  out.append("\npublications=");
  out.append(std::to_string(publications));
  out.append(" subscribers=");
  out.append(std::to_string(subscribers));
  out.append(" drops=");
  out.append(std::to_string(subscription_drops));
  return out;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
IntentRuntime::~IntentRuntime() = default;

Result<std::unique_ptr<IntentRuntime>> IntentRuntime::create(const Path& directory,
                                                             const DomainId& domain,
                                                             const Options& options) {
  auto store = IntentStore::create(directory, domain, options.store);
  if (!store) {
    return store.error();
  }
  auto runtime = std::unique_ptr<IntentRuntime>(new IntentRuntime());
  runtime->store_ = std::move(store.value());
  runtime->options_ = options;
  return runtime;
}

Result<std::unique_ptr<IntentRuntime>> IntentRuntime::open(const Path& directory,
                                                           const Options& options) {
  auto store = IntentStore::open(directory, options.store);
  if (!store) {
    return store.error();
  }
  auto runtime = std::unique_ptr<IntentRuntime>(new IntentRuntime());
  runtime->store_ = std::move(store.value());
  runtime->options_ = options;
  return runtime;
}

// ---------------------------------------------------------------------------
// Read surface
// ---------------------------------------------------------------------------
FenceToken IntentRuntime::fence(const ActorId& writer) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  FenceToken token;
  token.expected_generation = store_->head_generation();
  token.expected_epoch = store_->epoch();
  token.expected_incarnation = store_->incarnation();
  token.writer = writer;
  return token;
}

bool IntentRuntime::has_committed() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return store_->has_committed();
}

Result<GenerationRef> IntentRuntime::head() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return store_->head();
}

Result<IntentDocument> IntentRuntime::head_document() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!store_->has_committed()) {
    return Error(ErrorCode::NoCommittedGeneration, "no committed generation is authoritative");
  }
  const auto record = store_->load_head();
  if (!record) {
    return record.error();
  }
  return record->document;
}

Result<IntentDocument> IntentRuntime::document_at(const GenerationNumber& number) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const auto record = store_->load(number);
  if (!record) {
    return record.error();
  }
  return record->document;
}

Result<std::vector<GenerationSummary>> IntentRuntime::generations(std::size_t limit) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return store_->list(limit);
}

VerificationReport IntentRuntime::verify_store() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return store_->verify_all();
}

// ---------------------------------------------------------------------------
// Proposals
// ---------------------------------------------------------------------------
Error IntentRuntime::admit(const char* operation) const {
  if (shutting_down_) {
    return Error(ErrorCode::Cancelled,
                 std::string("runtime is shutting down and no longer admits ") + operation);
  }
  return Error();
}

Error IntentRuntime::evict_if_needed() {
  if (proposals_.size() < options_.max_pending_proposals) {
    return Error();
  }
  // Terminal proposals are retired and the identity index is rebuilt from
  // scratch. Compacting in place would leave every stored offset stale and
  // could resolve an identity to an out-of-range slot.
  std::vector<Proposal> kept;
  kept.reserve(proposals_.size());
  for (auto& proposal : proposals_) {
    if (proposal.state == ProposalState::Committed || proposal.state == ProposalState::Rejected ||
        proposal.state == ProposalState::Cancelled) {
      continue;
    }
    kept.push_back(std::move(proposal));
  }
  if (kept.size() == proposals_.size()) {
    return Error(ErrorCode::ProposalLimitExceeded,
                 "too many live proposals; no terminal proposal can be retired",
                 std::to_string(proposals_.size()));
  }
  proposals_ = std::move(kept);
  proposal_index_.clear();
  for (std::size_t i = 0; i < proposals_.size(); ++i) {
    proposal_index_.emplace(proposals_[i].id.str(), i);
  }
  stats_.pending_proposals = proposals_.size();
  return Error();
}

Proposal* IntentRuntime::find(const ProposalId& id) {
  const auto it = proposal_index_.find(id.str());
  if (it == proposal_index_.end()) {
    return nullptr;
  }
  return &proposals_[it->second];
}

const Proposal* IntentRuntime::find(const ProposalId& id) const {
  const auto it = proposal_index_.find(id.str());
  if (it == proposal_index_.end()) {
    return nullptr;
  }
  return &proposals_[it->second];
}

Result<ProposalId> IntentRuntime::propose(IntentDocument document, const ActorId& actor) {
  const std::lock_guard<std::mutex> guard(mutex_);
  const Error admitted = admit("proposals");
  if (!admitted.ok()) {
    return admitted;
  }
  if (actor.empty()) {
    return Error(ErrorCode::Unauthorized, "a proposal must name the actor that produced it");
  }
  const Error evicted = evict_if_needed();
  if (!evicted.ok()) {
    return evicted;
  }
  if (document.domain != store_->domain()) {
    return Error(ErrorCode::UnknownDomain,
                 "proposal targets a different intent domain than this runtime",
                 document.domain.str());
  }
  Proposal proposal;
  proposal.id = ProposalId();
  for (int attempt = 0; attempt < 8 && proposal.id.empty(); ++attempt) {
    const auto candidate = ProposalId::from_local(random_hex_128().substr(0, 16));
    if (!candidate) {
      return candidate.error();
    }
    if (find(candidate.value()) == nullptr) {
      proposal.id = candidate.value();
    }
  }
  if (proposal.id.empty()) {
    return Error(ErrorCode::Internal, "could not allocate a unique proposal identity");
  }
  proposal.actor = actor;
  proposal.domain = document.domain;
  proposal.base_generation = store_->head_generation();
  proposal.epoch = store_->epoch();
  proposal.incarnation = store_->incarnation();
  proposal.created_at = SystemClock().now();
  proposal.intent = std::move(document);
  const auto canonical = canonicalize(proposal.intent);
  if (!canonical) {
    proposal.state = ProposalState::Rejected;
    proposal.rejection_reason = canonical.error().render();
  } else {
    proposal.intent = canonical->normalized;
    proposal.content = canonical->content_digest;
    proposal.document = canonical->document_digest;
  }
  stats_.proposals_created = saturating_add(stats_.proposals_created, 1);
  proposal_index_.emplace(proposal.id.str(), proposals_.size());
  proposals_.push_back(std::move(proposal));
  stats_.pending_proposals = proposals_.size();
  return proposals_.back().id;
}

Result<ValidationReport> IntentRuntime::validate_proposal(const ProposalId& id) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Proposal* proposal = find(id);
  if (proposal == nullptr) {
    return Error(ErrorCode::ProposalNotFound, "unknown proposal", id.str());
  }
  if (proposal->state == ProposalState::Cancelled) {
    return Error(ErrorCode::Cancelled, "proposal was cancelled and cannot be validated", id.str());
  }
  if (proposal->state == ProposalState::Committed) {
    return proposal->report;
  }
  if (proposal->state == ProposalState::Rejected && proposal->rejection_reason.find("stale") !=
                                                        std::string::npos) {
    return Error(ErrorCode::ProposalRejected, proposal->rejection_reason, id.str());
  }
  if (proposal->epoch != store_->epoch() || proposal->incarnation != store_->incarnation()) {
    proposal->state = ProposalState::Rejected;
    proposal->rejection_reason = "proposal was created under a previous epoch or incarnation";
    stats_.proposals_rejected = saturating_add(stats_.proposals_rejected, 1);
    stats_.stale_epoch_rejections = saturating_add(stats_.stale_epoch_rejections, 1);
    return Error(ErrorCode::StaleEpoch, proposal->rejection_reason, proposal->id.str());
  }
  if (proposal->base_generation != store_->head_generation()) {
    proposal->state = ProposalState::Rejected;
    proposal->rejection_reason =
        "proposal was created against generation " + proposal->base_generation.str() +
        " but the committed head is " + store_->head_generation().str();
    stats_.proposals_rejected = saturating_add(stats_.proposals_rejected, 1);
    stats_.stale_generation_rejections = saturating_add(stats_.stale_generation_rejections, 1);
    return Error(ErrorCode::StaleGeneration, proposal->rejection_reason, proposal->id.str());
  }
  proposal->report = validate_document(proposal->intent, proposal->intent.schema);
  // With no committed generation the comparison base is the empty domain, so
  // the very first committed generation reports every declared object as an
  // addition rather than an empty diff.
  IntentDocument base;
  if (store_->has_committed()) {
    const auto record = store_->load_head();
    if (!record) {
      return record.error();
    }
    base = record->document;
  } else {
    base.domain = store_->domain();
    base.schema = proposal->intent.schema;
  }
  proposal->diff = diff_documents(base, proposal->intent);
  if (proposal->report.passed()) {
    proposal->state = ProposalState::Validated;
  } else {
    proposal->state = ProposalState::Rejected;
    proposal->rejection_reason = "validation failed with " +
                                 std::to_string(proposal->report.error_count()) + " error(s)";
    stats_.proposals_rejected = saturating_add(stats_.proposals_rejected, 1);
  }
  return proposal->report;
}

Result<SemanticDiff> IntentRuntime::compare_proposal(const ProposalId& id) {
  const std::lock_guard<std::mutex> guard(mutex_);
  const Proposal* proposal = find(id);
  if (proposal == nullptr) {
    return Error(ErrorCode::ProposalNotFound, "unknown proposal", id.str());
  }
  if (proposal->state == ProposalState::Proposed) {
    return Error(ErrorCode::ProposalNotValidated,
                 "proposal has not been validated, so no semantic diff is available", id.str());
  }
  if (proposal->state == ProposalState::Rejected) {
    return Error(ErrorCode::ProposalRejected, proposal->rejection_reason, id.str());
  }
  if (proposal->state == ProposalState::Cancelled) {
    return Error(ErrorCode::Cancelled, "proposal was cancelled", id.str());
  }
  return proposal->diff;
}

Result<CommitOutcome> IntentRuntime::commit_proposal(const ProposalId& id, const FenceToken& token) {
  const std::lock_guard<std::mutex> guard(mutex_);
  stats_.commit_attempts = saturating_add(stats_.commit_attempts, 1);
  const Error admitted = admit("commits");
  if (!admitted.ok()) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    return admitted;
  }
  Proposal* proposal = find(id);
  if (proposal == nullptr) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    return Error(ErrorCode::ProposalNotFound, "unknown proposal", id.str());
  }
  if (proposal->state == ProposalState::Cancelled) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    stats_.proposals_cancelled = saturating_add(stats_.proposals_cancelled, 1);
    return Error(ErrorCode::Cancelled,
                 "a cancelled proposal must never publish a committed generation", id.str());
  }
  if (proposal->state == ProposalState::Rejected) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    return Error(ErrorCode::ProposalRejected, proposal->rejection_reason, id.str());
  }
  if (proposal->state == ProposalState::Committed) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    return Error(ErrorCode::InvariantViolation,
                 "proposal has already been committed; committed generations are immutable",
                 id.str());
  }
  if (proposal->state != ProposalState::Validated) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    return Error(ErrorCode::ProposalNotValidated,
                 "proposal must be validated before it can be committed", id.str());
  }
  if (proposal->epoch != store_->epoch() || proposal->incarnation != store_->incarnation()) {
    proposal->state = ProposalState::Rejected;
    proposal->rejection_reason = "proposal was created under a previous epoch or incarnation";
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    stats_.stale_epoch_rejections = saturating_add(stats_.stale_epoch_rejections, 1);
    return Error(ErrorCode::StaleEpoch, proposal->rejection_reason, proposal->id.str());
  }
  if (proposal->base_generation != store_->head_generation()) {
    proposal->state = ProposalState::Rejected;
    proposal->rejection_reason =
        "proposal was created against generation " + proposal->base_generation.str() +
        " but the committed head is " + store_->head_generation().str();
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    stats_.stale_generation_rejections = saturating_add(stats_.stale_generation_rejections, 1);
    return Error(ErrorCode::StaleGeneration, proposal->rejection_reason, proposal->id.str());
  }
  if (token.writer != proposal->actor) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    return Error(ErrorCode::Unauthorized,
                 "the fence token names a different writer than the proposal", token.writer.str());
  }
  if (token.expected_epoch != proposal->epoch ||
      token.expected_incarnation != proposal->incarnation) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    stats_.stale_epoch_rejections = saturating_add(stats_.stale_epoch_rejections, 1);
    return Error(ErrorCode::StaleEpoch,
                 "the fence token belongs to a different epoch or incarnation than the proposal",
                 token.render());
  }
  if (token.expected_generation != proposal->base_generation) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    stats_.stale_generation_rejections = saturating_add(stats_.stale_generation_rejections, 1);
    return Error(ErrorCode::StaleGeneration,
                 "the fence token names a different base generation than the proposal",
                 token.render());
  }
  if (options_.reject_empty_diff && proposal->diff.empty()) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    return Error(ErrorCode::EmptyDiffRejected,
                 "the proposal reproduces the committed generation exactly", id.str());
  }

  const auto next = store_->head_generation().is_genesis()
                        ? GenerationNumber::from(1)
                        : store_->head_generation().next();
  if (!next) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    return next.error();
  }
  GenerationRecord record;
  record.domain = store_->domain();
  record.generation = next.value();
  record.writer = proposal->actor;
  record.committed_at = SystemClock().now();
  record.document = proposal->intent;
  record.provenance = proposal->intent.provenance;

  const auto committed = store_->commit(std::move(record), token);
  if (!committed) {
    stats_.commit_failures = saturating_add(stats_.commit_failures, 1);
    if (committed.error().code == ErrorCode::StaleWriter) {
      stats_.stale_generation_rejections = saturating_add(stats_.stale_generation_rejections, 1);
    } else if (committed.error().code == ErrorCode::StaleEpoch) {
      stats_.stale_epoch_rejections = saturating_add(stats_.stale_epoch_rejections, 1);
    }
    return committed.error();
  }
  proposal->state = ProposalState::Committed;
  stats_.proposals_committed = saturating_add(stats_.proposals_committed, 1);
  CommitOutcome outcome;
  outcome.reference = committed.value();
  outcome.diff = proposal->diff;
  outcome.impact = proposal->diff.worst;
  if (options_.publish_on_commit) {
    publish_locked(committed.value());
  }
  return outcome;
}

Result<CommitOutcome> IntentRuntime::submit(IntentDocument document, const ActorId& actor,
                                            const FenceToken& token) {
  const auto id = propose(std::move(document), actor);
  if (!id) {
    return id.error();
  }
  const auto report = validate_proposal(id.value());
  if (!report) {
    return report.error();
  }
  return commit_proposal(id.value(), token);
}

Result<Proposal> IntentRuntime::inspect_proposal(const ProposalId& id) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const Proposal* proposal = find(id);
  if (proposal == nullptr) {
    return Error(ErrorCode::ProposalNotFound, "unknown proposal", id.str());
  }
  return *proposal;
}

Error IntentRuntime::cancel_proposal(const ProposalId& id, std::string reason) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Proposal* proposal = find(id);
  if (proposal == nullptr) {
    return Error(ErrorCode::ProposalNotFound, "unknown proposal", id.str());
  }
  if (proposal->state == ProposalState::Committed) {
    return Error(ErrorCode::InvariantViolation,
                 "a committed generation cannot be cancelled; committed generations are immutable",
                 id.str());
  }
  if (proposal->state == ProposalState::Cancelled) {
    return Error();
  }
  proposal->state = ProposalState::Cancelled;
  proposal->rejection_reason =
      reason.empty() ? std::string("cancelled by the caller") : std::move(reason);
  stats_.proposals_cancelled = saturating_add(stats_.proposals_cancelled, 1);
  return Error();
}

std::vector<Proposal> IntentRuntime::pending_proposals() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::vector<Proposal> out;
  for (const auto& proposal : proposals_) {
    if (proposal.state == ProposalState::Proposed || proposal.state == ProposalState::Validated) {
      out.push_back(proposal);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Subscriptions
// ---------------------------------------------------------------------------
Result<std::uint64_t> IntentRuntime::subscribe() {
  const std::lock_guard<std::mutex> guard(mutex_);
  const Error admitted = admit("subscribers");
  if (!admitted.ok()) {
    return admitted;
  }
  if (subscribers_.size() >= options_.max_subscribers) {
    return Error(ErrorCode::ServerBusy, "subscriber limit reached",
                 std::to_string(subscribers_.size()));
  }
  Subscriber subscriber;
  subscriber.id = next_subscription_++;
  if (store_->has_committed()) {
    Publication publication;
    publication.reference = GenerationRef{store_->head_generation(), store_->head_content(),
                                          store_->epoch(), store_->incarnation()};
    publication.sequence = ++publication_sequence_;
    subscriber.queue.push_back(publication);
  }
  const std::uint64_t id = subscriber.id;
  subscribers_.push_back(std::move(subscriber));
  stats_.subscribers = subscribers_.size();
  return id;
}

Error IntentRuntime::unsubscribe(std::uint64_t subscription) {
  const std::lock_guard<std::mutex> guard(mutex_);
  for (std::size_t i = 0; i < subscribers_.size(); ++i) {
    if (subscribers_[i].id == subscription) {
      subscribers_.erase(subscribers_.begin() + static_cast<std::ptrdiff_t>(i));
      stats_.subscribers = subscribers_.size();
      return Error();
    }
  }
  return Error(ErrorCode::InvalidArgument, "unknown subscription", std::to_string(subscription));
}

Result<std::optional<Publication>> IntentRuntime::poll(std::uint64_t subscription) {
  const std::lock_guard<std::mutex> guard(mutex_);
  for (auto& subscriber : subscribers_) {
    if (subscriber.id != subscription) {
      continue;
    }
    if (subscriber.queue.empty()) {
      return std::optional<Publication>();
    }
    Publication publication = subscriber.queue.front();
    subscriber.queue.pop_front();
    publication.dropped = subscriber.dropped;
    return std::optional<Publication>(publication);
  }
  return Error(ErrorCode::InvalidArgument, "unknown subscription", std::to_string(subscription));
}

void IntentRuntime::publish_locked(const GenerationRef& reference) {
  stats_.publications = saturating_add(stats_.publications, 1);
  for (auto& subscriber : subscribers_) {
    Publication publication;
    publication.reference = reference;
    publication.sequence = publication_sequence_ + 1;
    subscriber.queue.push_back(publication);
    while (subscriber.queue.size() > options_.subscriber_queue) {
      subscriber.queue.pop_front();
      subscriber.dropped = saturating_add(subscriber.dropped, 1);
      stats_.subscription_drops = saturating_add(stats_.subscription_drops, 1);
    }
  }
  publication_sequence_ = saturating_add(publication_sequence_, 1);
}

Result<GenerationRef> IntentRuntime::acquire(const GenerationNumber& min_generation) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const auto current = store_->head();
  if (!current) {
    return current.error();
  }
  if (current->number.value() < min_generation.value()) {
    return Error(ErrorCode::StaleGeneration,
                 "the committed generation is older than the generation the consumer requires",
                 "committed " + current->number.str() + " < required " + min_generation.str());
  }
  return current.value();
}

Result<GenerationRef> IntentRuntime::require_content(const ContentDigest& expected) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const auto current = store_->head();
  if (!current) {
    return current.error();
  }
  if (current->content != expected) {
    return Error(ErrorCode::StaleGeneration,
                 "the generation the consumer holds has been superseded",
                 "held " + expected.short_hex() + ", committed " + current->content.short_hex());
  }
  return current.value();
}

void IntentRuntime::request_shutdown() {
  const std::lock_guard<std::mutex> guard(mutex_);
  shutting_down_ = true;
}

bool IntentRuntime::shutting_down() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return shutting_down_;
}

RuntimeStats IntentRuntime::stats() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  RuntimeStats out = stats_;
  out.pending_proposals = proposals_.size();
  out.subscribers = subscribers_.size();
  out.head = store_->head_generation();
  return out;
}

}  // namespace ifabric
