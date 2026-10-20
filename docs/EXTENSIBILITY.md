# Target architecture: composable capabilities in C11

This design requires prototype validation and is not an implemented API specification. The current split provides separate libraries;
contracts for replacing capabilities from an external repository remain to be built.

The goal is an extensible networking foundation for telemetry, DNS, tunnels,
file transfers and deadline-sensitive streams. These applications do not define
the limits of the core.

## 1. Composition kernel and mandatory guarantees

The kernel assembles capabilities, validates dependencies and budgets, and manages
lifecycle. It does not require POSIX sockets, a cipher suite, a media format or a
particular buffer layout. Official and external modules use the same public APIs.

Replacements must preserve the advertised profile's guarantees: validate before
mutation, explicit buffer ownership, bounded output, explicit errors and no silent
abandonment of accepted reliable messages. Native plugins are trusted code; the
framework is not a sandbox and cannot enforce contracts against arbitrary code.

Dynamic loading is optional. MCU composition is static; hosts may optionally load
shared libraries conforming to a published C ABI.

## 2. Proposed boundaries

| Capability | Main contract | Example providers |
| --- | --- | --- |
| Storage | Reserve/release handles; capacity, alignment and access | Caller arrays, static pools, registered memory |
| Wire format | Encode, bound and validate envelopes and records | Current TFV, future variable-payload format |
| Delivery | Accept messages and expose terminal outcomes | Sequenced, reliable ordered, future reliable unordered |
| Recovery | Interpret reception evidence and propose repairs | Current policy, SACK and time-based detection, optional FEC |
| Scheduling | Select eligible work within priorities and budgets | Fairness, deadlines, pacing, congestion control |
| Protection | Establish protected associations and authenticate/open/seal | AEAD adapter with key management, external secure transport |
| Transport | Accept/reject submissions and return completions | POSIX UDP, Windows, lwIP, simulation |
| Observation | Export bounded events and counters | Deterministic traces, metrics, diagnostics |
| Application | Map application protocols onto delivery contracts | DNS, streaming, tunnels with TUN adapters |

These are contract boundaries, not nine mandatory plugins per packet. Initial
implementations may group tightly coupled capabilities. Recovery proposes repairs;
the scheduler applies the same budgets used for data and control. Plugins cannot
bypass pacing. Delivery policy and codec jointly declare a supported wire profile:
a codec cannot change arbitrarily while reliability retains incompatible assumptions.

## 3. Three data contracts before plugin interfaces

### Application message

A message carries an identifier, channel, payload spans and explicit delivery policy.
Its size is independent of TFV. The current API remains a convenient adapter for
four-byte inline messages. Larger buffers do not increase MTU or sequence-window
limits. Fragmentation and reassembly are bounded parts of the selected wire profile.
Windows remain smaller than half the sequence space. Wider sequences require a new
profile and conformance tests.

### Buffer and handle

The caller owns storage; submission creates an explicit loan. A handle includes an
index and generation. ACK, failure, cancellation and reset terminate protocol
references; completions terminate I/O references. Reuse requires all references
to have ended.

Memory descriptions specify CPU read/write access, alignment, coherence and backend
registration requirements. A GPU handle is not an ordinary CPU pointer. Exportable
memory alone does not establish direct GPU/NIC support.

### Packet plan

The engine produces a bounded plan: spans, header scratch with explicit lifetime,
total length, attempt identifier and transport metadata. Message storage is
independent of the I/O backend. Backends can accept spans or flatten them into
provided storage within declared limits. No implicit concatenation or allocation
is required.

## 4. Transactional transmission and separate identities

1. Prepare selects messages and reserves resources without consuming pending ACKs,
   attempts or transmission timers.
2. Submit transfers a loan when the backend accepts the datagram.
3. Commit records acceptance; cancel releases reservations after rejection. The
   API specifies events permitted between these steps.
4. I/O completion releases backend resources; peer acknowledgement ends delivery
   responsibility. These events are distinct.

Separate reliable-message, network-attempt and buffer identifiers. Retransmission
must not produce a second application delivery. Late completions must not release
reassigned buffers. OS acceptance does not prove physical transmission.

## 5. Security and composition compatibility

Bound protected envelopes, authenticate them and check replay before mutating
reliability state. Protect ACKs and controls as well as data. Protection modules
own cryptographic state; codecs must not receive keys through incidental coupling.

Every protected attempt follows its provider's nonce rules. A 16-bit reliable-message
sequence alone is insufficient as an encryption nonce. FEC/encryption ordering,
repair grouping and payload reuse belong to a specified, tested composition rather
than arbitrary callback ordering. Secure profiles fail when required protection is
unavailable; they never silently fall back to plaintext.

Declare local API versions separately from wire-profile identifiers and versions.
Different local implementations can interoperate through the same wire profile.
Capability negotiation is bounded and authenticated in secure profiles. Local buffer
placement does not require network negotiation.

## 6. Initialization-time composition and a direct data path

A C11 descriptor declares name, contract version, services, dependencies, memory
requirements and lifecycle operations. Specialized callbacks are typed; avoid a
universal `void *event` bus. Callers provide descriptor arrays using designated
initializers.

Creation validates dependencies, cycles, compatibility, MTU after overhead, segment
limits and storage budgets. Composition remains fixed during traffic. Traffic-dependent
costs are bounded or exposed through backpressure; a manifest cannot establish
hardware worst-case execution time by itself.

Static compositions may specialize into direct calls. Flexible compositions resolve
operations once and use typed tables at necessary boundaries, preferably per packet
or batch. Measure indirect-call and additional-code costs. No plugin discovery,
global locks, network discovery or allocation belongs in the critical path.

One owner processes each session's data, timers, resets and completions. Shutdown
stops admission, completes or cancels work, waits for loans and I/O references, then
destroys modules in reverse dependency order. Hot swapping is deferred: never unload
modules while callbacks or buffers remain active.

## 7. Required evidence

- An external plugin repository uses only public headers.
- Two storage providers and two transports pass the same traces.
- Simulation and UDP make identical protocol decisions for identical events;
  physical execution timings are measured separately.
- Tests cover loss, replay, reordering, module failure, rejected sends, completions
  after reset, descriptor exhaustion and incomplete shutdown.
- Compare total memory, CPU, copied bytes, traffic, delivery, p99/p99.9 and recovery
  time with identical workloads and guarantees.
- The reference static composition remains straightforward to integrate on MCU.

## 8. Implementation order

1. Fix known I/O, timer and validation defects; preserve baseline traces and define
   latency and memory targets.
2. Prototype the three data contracts and transactional submission with simulation
   and reference flattening, without hidden wire changes.
3. Extract TX/RX storage and demonstrate an external provider without engine changes.
   Publish the initial plugin SDK and conformance tests.
4. Add a protected composition and second real backend. Test negotiation, MTU,
   nonces, shutdown and completions under loss and saturation.
5. Develop SACK, probes, pacing and FEC within these contracts. Isolate wire changes
   in explicitly versioned profiles.
6. Explore acceleration and multipath only after measuring an actual bottleneck.

The distinguishing milestone is demonstrated external extensibility with measurable
compositions and controlled memory. Industrial comparisons cannot replace evidence
or overcome physical network limits.
