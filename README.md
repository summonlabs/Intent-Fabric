# Intent Fabric

**Authoritative desired-state intent runtime for Summon Software Labs Fabric OS.**

Intent Fabric owns the canonical declarative statement of what the network should
be, independent of device-specific configuration syntax and independent of the
mechanics of rollout. Its output is authoritative desired-state intent plus
validated, normalized, generation-bound intent artifacts that downstream
planners and configuration systems consume.

* Version: 1.0.0
* Language: C++20, no third-party dependencies
* Library: libifabric (CMake package IntentFabric, target IntentFabric::ifabric)
* Binaries: ifabric (inspection and commit tooling), ifabricd (intent daemon)

## Systems boundary

Intent Fabric owns:

* the canonical declarative intent model: topology expectations, routing and
  path policy, capacity and reservation constraints, administrative state,
  service classes, redundancy and failure-domain constraints, and maintenance /
  upgrade eligibility hooks;
* canonical normalization and stable content identity;
* layered validation and deterministic conflict detection;
* atomic intent transactions and immutable generation lineage;
* versioned, integrity-checked persistence of committed generations;
* subscription and read APIs that let downstream runtimes reject stale
  generations.

Intent Fabric does **not** own, and does not implement:

* device configuration rendering or distribution (Configuration Fabric);
* transition sequencing (Change Planner);
* staged deployment (Rollout Fabric);
* maintenance scheduling (Maintenance Fabric);
* actual-vs-intended observation (Network Drift Observatory).

The intent model therefore contains no device configuration syntax, no
transition plan, no rollout step and no observed state. It is the authoritative
statement of intent and nothing else.

## Build and test

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

Options: IFABRIC_BUILD_TOOLS, IFABRIC_BUILD_TESTS, IFABRIC_BUILD_EXAMPLES,
IFABRIC_BUILD_BENCH, IFABRIC_ENABLE_SANITIZERS (GCC/Clang).

MSVC is compiled with /W4 /WX; GCC and Clang with -Wall -Wextra -Wpedantic
-Werror plus shadowing, conversion, old-style-cast and format checks. First-party
warning count is zero in both Debug and Release.

ctest runs the test binary plus the downstream packaging proof: the project is
installed into a scratch prefix, and an independent consumer in tests/downstream
is configured, built and executed against the installed artifact through
find_package(IntentFabric 1.0 REQUIRED) only.

## Identity, normalization and content identity

Every domain object carries a strongly typed identity: FabricId, SiteId, PodId,
RackId, DeviceId, PortId, LinkId, PolicyId, IntentObjectId, TenantId, WorkloadId,
ServiceClassId, ConflictId, ActorId, ProposalId and GenerationId. Text form is
prefix.local; the parser validates the class prefix, so a FabricId can never be
silently used where a DeviceId is required. Identities used where the class is
only known at runtime (AnyId) sort by class and then by local part, which makes
every identity-keyed collection canonical by construction.

normalize() is total and stable:

* every collection is sorted by identity, with the canonical serialization of an
  element as a deterministic tie-break, so even documents containing duplicate
  identities normalize to a stable order;
* set-valued fields (service-class sets, capabilities, upgrade groups, feature
  lists) are sorted and de-duplicated;
* optional fields are materialized, so an omitted field and its default value
  produce the same form;
* free-text fields are trimmed.

Two identities are computed over the canonical form:

* **document identity** - SHA-256 over the whole canonical document, including
  the declared schema version and provenance;
* **content identity** - SHA-256 over the semantic payload only: domain,
  features, topology, capability catalog, service classes, policies, tenants,
  workloads and intent objects. Provenance and the declared schema minor are
  excluded, so the same desired state authored in a compatible older minor, or
  narrated by a different actor at a different time, has the same content
  identity.

## Schema versioning

The schema is ifabric.intent. This build reads major 1, minors 0 through 3, and
always normalizes to 1.3.

* A different major is rejected with UnsupportedSchemaMajor.
* A newer minor is rejected with UnsupportedSchemaMinor; the runtime never
  guesses at semantics it does not implement.
* An older minor is upgraded by documented structural migrations, each of which
  is content-preserving and reported in the validation report:

  | migration | change |
  | --- | --- |
  | 1.0 to 1.1 | port.speed to port.speed_bps, link.bandwidth to link.capacity_bps |
  | 1.1 to 1.2 | maintenance-eligibility defaults (requires_redundancy_headroom, upgrade_groups) |
  | 1.2 to 1.3 | device.port_count to declared_port_count, capability_catalog[].ports to port_count |

  A migration that meets a contradictory document (both the legacy and the
  current field present) is rejected with AmbiguousSemantics.
* Required features are checked against a closed registry. A feature this build
  does not implement is rejected with FeatureNotAvailable; a declaration that
  requires a feature introduced after its declared minor is rejected as
  internally inconsistent.

## Validation layers

Validation runs in layers and reports every diagnostic it finds, in a
deterministic order:

1. **schema compatibility** - schema version, feature registry, feature/minor
   consistency;
2. **syntax** - object bounds, identity well-formedness, enumerated ranges,
   service-class budget, mandatory capacity and speed declarations;
3. **referential integrity** - every reference resolves, duplicate identities,
   cyclic references in the declared reference graph;
4. **capability compatibility** - device models must be described by the
   capability catalog, roles, port counts and speeds must be within what the
   model provides, and every service class a device carries must be backed by a
   capability the model actually declares;
5. **topology feasibility** - containment consistency, port index uniqueness and
   bounds, one link per port, endpoint speed agreement, host and fabric port
   roles, device connectivity, explicit-path connectivity, workload redundancy
   spread;
6. **policy conflict** - deterministic conflict detection (below);
7. **invariant violation** - cross-object invariants such as an enabled link on
   a disabled device or port, a workload placed on a disabled device, and the
   intent property registry (unknown properties and wrong subject classes are
   rejected rather than ignored).

Unknown facts are never treated as satisfied. A device whose model is not in the
capability catalog is an error, not an assumption. A link with no declared
capacity is an error. A capability requirement outside the runtime's vocabulary
is an error. A reservation against unobservable capacity is an error.

## Deterministic conflicts

Each conflict carries a machine-readable identity derived from its rule and its
canonicalized participants, so identical inputs produce identical conflict
identities regardless of declaration order, and each conflict carries a
human-readable explanation naming the governing declaration, the selected
outcome and the rejected alternatives.

Implemented rules: administrative-state disagreement, intent-value disagreement,
routing-action disagreement at equal precedence, capacity overcommit against
observed capacity, unachievable redundancy requirements, tenant-isolation
overlap, and a maintenance drain requirement conflicting with a pinned
administrative state.

## Transactions, authority and fencing

One runtime instance owns exactly one intent domain:

    propose -> validate -> compare -> commit generation -> publish

* A proposal captures the head generation, epoch and incarnation observed when
  it was created.
* Validation produces a layered report; the proposal's semantic diff against the
  committed generation is computed at the same time. With no committed
  generation the comparison base is the empty domain, so the first commit
  reports every declared object as an addition.
* Commit re-checks the head generation, the epoch and the incarnation, and the
  store re-checks the fence token. A writer that believes it is at an older
  generation cannot overwrite a newer one (StaleGeneration / StaleWriter), and a
  writer from a previous process incarnation is fenced out (StaleEpoch).
* A failed proposal or a failed commit leaves the authoritative head untouched.
  Committed generations are immutable: they cannot be cancelled, re-committed or
  rewritten.
* A cancelled proposal can never publish: cancellation is honoured before the
  commit point, and after the commit point the operation has already succeeded.
* Exactly one committed generation is authoritative at a time. The store holds
  one head pointer per domain, and every commit advances it by exactly one.

Subscriptions are bounded per subscriber with drop accounting; a slow subscriber
never blocks a commit. acquire(min_generation) and require_content(digest) let a
downstream runtime reject a generation that is older than it requires or whose
content has been superseded.

## Persistence

A store directory contains:

    store.json     descriptor: format, format version, domain, store identity
    head.json      the authoritative head pointer
    head.json.bak  the previous good head pointer
    lock           an exclusive process-scoped lock
    records/       one immutable record per committed generation
    lineage.log    an append-only lineage journal

Each record is a 128-byte header plus a canonical JSON envelope. The header
carries a magic value, format version, generation, payload length, payload
CRC-32, payload SHA-256, the head digest for this generation, and a header
CRC-32. The envelope carries the domain, generation, epoch, incarnation, writer,
commit time, content and document digests, lineage digest, parent reference with
the parent's head digest, and the full intent document.

Verification is layered: file length, magic, format version, header CRC, payload
CRC, payload SHA-256, the content and document digests recomputed from the
embedded document, agreement with the head pointer, and the chain link to the
parent record's head digest while the parent is still retained.

Recovery is conservative. Opening a store validates the head pointer and its
backup independently and takes the highest generation that fully verifies; if
neither verifies, the store refuses to open with StoreCorrupt rather than guess a
committed generation. Records that no head pointer references, and files in the
record directory that are not well-formed generation records, are treated as
debris from an interrupted commit and are removed. A deserializable record is
never treated as current merely because it parses. Pruned generations read back
as GenerationPruned, never as stale authority.

## Transport

ifabricd serves one intent domain over TCP using a framed protocol: a 72-byte
header (magic, protocol version, message type, flags, request identity, payload
length, payload CRC-32, payload SHA-256, header CRC-32) followed by a canonical
JSON payload. Declared lengths are bounded before any allocation. Every
connection performs a handshake that binds it to the server's epoch and
incarnation; requests carrying a stale binding are rejected with StaleEpoch.

Servers are bounded in worker threads, concurrent connections, pending
connections and frame size. Shutdown stops admission, closes active connections,
joins the worker pool and is idempotent.

## Concurrency and lock discipline

The runtime is written to a single discipline, and it is audited explicitly:

* **One mutex per runtime, never held across a re-entrant call.** All runtime
  entry points take IntentRuntime::mutex_ for the whole operation, including the
  store write, which is what makes the head advance atomic with respect to other
  callers.
* **No callbacks beneath a lock, and no event emission beneath a lock.**
  Subscribers are polled, not pushed: publish_locked only appends to a bounded
  per-subscriber deque with drop accounting. No user code ever runs while the
  mutex is held.
* **No read-to-write upgrade paths.** Every entry point acquires the mutex
  exactly once, in write mode; there are no recursive or shared acquisitions and
  no nested lock ordering, because the runtime has only one lock.
* **The store has no internal lock.** It is documented as externally
  synchronized by the runtime, and the process-scoped store lock is the only
  cross-process exclusion mechanism. Lock acquisition order is always runtime
  mutex then store lock, and the store lock is acquired once at open and released
  at close, never nested.
* **Shutdown joins from outside the state lock.** IntentServer::finish releases
  the connection mutex before notifying and joining workers, so a worker
  returning from its session can still take the connection mutex to retire
  itself. The server's shutdown mutex is a leaf: nothing is acquired beneath it.
* **Cancellation is idempotent and never re-enters.** cancel_proposal takes the
  runtime mutex once; request_shutdown sets an admission flag. Neither waits on
  work started by another thread while holding the lock.
* **Re-entrancy audit.** IntentRuntime never calls back into itself: submit is a
  documented composition helper that is only called by callers that do not
  already hold the mutex, commit_proposal does not call propose, and
  validate_proposal does not call commit_proposal. The store never calls back
  into the runtime.

## Proof surfaces

**REAL** - exercised by the test suite on this machine:

* canonicalization stability, content identity and schema migration behaviour,
  over seeded permutations of every collection;
* deterministic conflict identities and explanations across permutations;
* layered validation, including adversarial cyclic references, duplicate
  identities, impossible constraints, unknown capabilities and models, and
  oversized documents;
* transaction atomicity, cancellation racing a commit in two threads, concurrent
  commits from eight threads, and stale-writer and stale-epoch fencing;
* store integrity: payload, header and envelope corruption, truncation,
  head-pointer escape attempts, interrupted commits, recovery from the head
  pointer backup, pruning, retention bounds, and cross-process lock exclusion;
* process-level behaviour over real TCP loopback sockets: an ifabricd process is
  started, committed to, killed without a graceful shutdown, restarted, and the
  committed generation, the advanced epoch and the fresh incarnation are
  verified, together with rejection of a proposal identity from the previous
  incarnation and of a writer that still believes the head is genesis;
* the command line surface, end to end, as separate OS processes.

**SYNTHETIC** - the reference topology, the randomized stress documents and the
defect corpus are generated by ifabric::synthesize from a seed. There is no
network hardware, no switch operating system, no device configuration protocol
and no real fabric in this repository.

**UNSUPPORTED** - not claimed and not tested here: RDMA, NVLink, multi-GPU,
switch-vendor specific behaviour, firmware behaviour, line-rate performance, and
any multi-node or multi-host deployment. The transport is a single-process server
with bounded worker threads over TCP; it does not claim clustering, replication
or consensus.

## Benchmarks

ifabric_bench [scale] measures completed work: a fully parsed and modelled
document, a complete canonicalization, a complete layered validation, a complete
conflict pass, a complete semantic diff, a durably committed generation
(including fsync and the atomic head replacement), and a full store reopen and
integrity walk. Enqueue or submission latency is never reported.

Representative Release output (ifabric_bench 2) on the development machine, with a
synthetic reference document of 310 objects and 66,297 canonical bytes:

    parse + model build             827 units/s     52 MiB/s
    canonicalize                    725 units/s     46 MiB/s
    validate (all layers)           458 units/s
    conflict detection           30,293 units/s
    semantic diff                   436 units/s
    durable commit (fsync)           75 units/s
    store reopen + full verify        9 units/s

The scale argument multiplies the iteration count; "durable commit" includes a
canonicalization, a layered validation, a semantic diff, a record write with
fsync, a lineage append and an atomic head replacement per unit.

## Examples

    ifabric_example_boundaries    the implemented systems boundary and vocabularies
    ifabric_example_validate      validate a file or the reference document
    ifabric_example_transaction   a full propose, validate, compare, commit and
                                  publish cycle, then restart and verification

## Command line

    ifabric validate   <file> [--json]                     layered validation
    ifabric normalize  <file> [--out <file>] [--json]      canonical normalized form
    ifabric digest     <file> [--json]                     content and document identity
    ifabric explain    <file> [--json]                     explain a rejected commit
    ifabric schema | capabilities | properties
    ifabric init       --store <dir> --domain <dom.x>
    ifabric commit     --store <dir> --file <f> --actor <a> [--expect-generation <n>]
    ifabric generations --store <dir> [--limit <n>] [--json]
    ifabric show       --store <dir> [--generation <n>] [--json]
    ifabric diff       --store <dir> --from <n> --to <n> [--json]
    ifabric verify     --store <dir> [--json]
    ifabric serve      --store <dir> [--host <h>] [--port <n>]
    ifabric remote     --host <h> --port <n> <head|stats|generations|verify|commit|shutdown>

Exit codes: 0 success, 1 operational failure (including a rejected commit), 2
usage error.

## Limitations

* One intent domain per store; a domain is a single authority scope.
* Capability reasoning is a declared capability vocabulary. It reasons about
  what a document declares, not about what a device reports at runtime.
* The conflict rules are the seven listed above. They are not a general
  constraint solver.
* The redundancy feasibility check counts disjoint neighbours at the declared
  failure-domain level; it is not a full k-edge-disjoint path computation over
  the whole fabric.
* Semantically equivalent input is defined by the normalization rules above. Two
  documents that differ in ways normalization does not erase, such as a different
  declared schema minor, have different document identities even though their
  content identities agree.
* Retention is bounded by configuration; the store refuses new generations
  rather than growing without limit.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
