# astrite++ — Design

> **Status:** Design target. Nothing is built yet; [TODO.md](TODO.md) is the build order.
>
> **What this is:** a secure, fast, flexible coding agent in modern C++ (C++23 floor, C++26
> trajectory). No dynamic-language runtime, no plugin loader, **and no executable extension code
> of any kind** — not even sandboxed bytecode/WASM. The agent's behavior is *data*: declarative
> **components** (behavior trees in YAML) interpreted by a small C++ kernel over a closed set of
> primitives. The default agent is itself built from this mechanism, which is the proof the
> mechanism is expressive enough.
>
> The core bet: **extensibility should be inspectable data; authority should be a reviewed kernel
> capability.** Agents may rewrite prompts, routing, tool composition, policies, and even their own
> component data — without gaining the ability to ship new executable code, import dependencies,
> read secrets, open sockets, or widen their sandbox. Rewrite strategy, not confinement.

## 0. Status

Greenfield. This document is the target; nothing below exists. The repository contains this file,
`TODO.md`, and an empty tree.

## 1. Why C++?

The question everyone will ask: a security-first agent, in 2026, and you didn't pick Rust, Go, or
Zig? The answer has a positive case and an honest ledger.

### The claim under test

astrite's security claim was never "the implementation language is safe." It is architectural:
a **closed primitive catalog**, a **static authority fold** over finite data, and an **OS sandbox
floor that holds even when the interpreter is buggy**. If that claim is real, it should survive a
language without a borrow checker. astrite++ tests the claim honestly — and strengthens one part
of it (see §1's ledger and §7's confined loader).

### The supply-chain case

The threat astrite is built against is the supply chain: ecosystems where installing a tool means
running third-party code with ambient authority, and where a build pulls hundreds of transitive
packages, each able to execute code at install or build time. Measured against that threat:

- **Rust**'s guarantee is real, but a typical agent build (async runtime + TLS + serde + TUI)
  resolves to *hundreds* of crates, each with `build.rs`/proc-macro install-time code execution.
  The guarantee protects you from memory bugs, not from the dependency graph.
- **Go**'s standard library shrinks the graph beautifully, but brings a runtime and GC between you
  and the syscall layer, and seccomp/Landlock interact delicately with its thread model and cgo.
- **Zig** is closest in spirit (explicit control flow, superb comptime), but pre-1.0: an unstable
  language, stdlib, and toolchain are themselves a supply-chain and stability risk for a
  security-first project.

astrite++'s budget: **three vendored source libraries plus system libcurl** (§12, §14). No package
manager, no registry resolution, no install hooks, no build-time code execution by dependencies.
Every third-party line is pinned source in `third_party/`, reviewable in one sitting. The sandbox
layer is **zero** dependencies — raw syscalls (§7).

### The language-fit case

Post-C++23, the kernel is a best-case modern C++ program:

- `std::variant` + `std::visit` over an overload set with **no catch-all** is a compile-time
  exhaustive `match` — add a seventh node kind and every visit site fails to compile.
- `std::expected<T, E>` with monadic `and_then`/`transform` carries every fallible path; no
  exceptions, no error-code soup.
- `std::indirect<T>` (C++26; a 20-line `box<T>` until stdlibs ship it) makes the recursive IR
  value-semantic — copyable, comparable, no `unique_ptr` ceremony.
- The evaluator needs **no async runtime at all** (§8): one model stream at a time, per-effect
  child processes, `std::jthread` + `std::stop_token` + one small channel. Plain recursion.
- The seccomp BPF filter is a `constexpr std::array<sock_filter, N>` — assembled and checked at
  compile time.
- Embedded default components are code-generated into constexpr IR, so the **authority fold over
  the shipped defaults is a `static_assert`** (§7): the binary cannot be built with a default
  component that exceeds its declared capabilities.

C++26 contracts (pre/post conditions on the primitive boundary) and static reflection
(serde-style derive of the loader from the IR types) are the trajectory; neither is load-bearing
for v0.

### The honest ledger

C++ has no memory-safety guarantee, and the place that bites is exactly where the kernel touches
attacker-influenced bytes: project-local component YAML (a planted config file is an attack) and
model-emitted JSON. astrite++ answers with architecture, not assertion:

1. **Style floor.** The kernel is value-semantic tree-walking over variants and maps — no manual
   `new`/`delete`, no pointer arithmetic, no shared mutable state. Enforced by clang-tidy,
   hardened stdlib modes, sanitizer CI, and permanent fuzz targets (§15).
2. **Mature, fuzzed parsers** for the two untrusted-input formats — never hand-rolled (§12).
3. **Confine the parser like an effect** (§7). Untrusted YAML is parsed inside the same
   no-fs/no-net sandbox child machinery every effect already uses. A parser bug is a confined
   crash, not kernel compromise. This is the C++-specific design move, and it *sharpens* the
   thesis: even the kernel treats its own loader as untrusted-input handling.

The result is a defense-in-depth story where the OS floor — which never depended on language
safety — does the same job it always did, and the layer above it is hardened, fuzzed, and
architecturally distrusted.

## 2. Goals & non-goals

**Pillars, in priority order:** security → stability → speed → flexibility. Where they conflict,
security wins.

- **Security is the differentiator.** Two rules: (1) **no executable extensions** — extensions are
  guaranteed-declarative components, never code, so the entire "extension ships and runs attacker
  code" class is gone by construction; (2) **authority confinement** — the model is *untrusted*,
  every effect is capability-gated and confined by an OS-level sandbox that fails **closed**.
- **Stability:** tiny vendored dependency tree; pinned toolchain; deterministic, network-free
  builds; every component provably terminates (the interpreter is total); hardened release builds.
- **Speed:** native binary, no runtime, no GC, streaming model output rendered incrementally.
- **Flexibility:** behavior is data. New tools, commands, personas, policies, and whole agent
  loops are authored as components without touching C++. An OpenAI-compatible endpoint drives
  OpenAI, vLLM, llama.cpp server, LM Studio, OpenRouter, and Anthropic's compat endpoint by
  config alone.

YAML is not the security boundary. The security boundary is the closed primitive catalog, typed
schema validation, capability analysis, explicit trust/consent points, and the sandbox.

**Non-goals (v0):** executable/plugin extensions of any kind including **WASM** (rejected outright —
§6); MCP (**[PHASE 2]**); native non-OpenAI protocols (**[PHASE 2]**); Windows/macOS sandbox
*enforcement* (may build, but the sandbox fails closed off-Linux — §7); a GUI; remote/daemon
operation.

## 3. The core idea — a radical interpreter

The C++ binary is a **kernel**, and the kernel is small: an **evaluator** + a closed set of
**primitives** + the **sandbox**. Nothing else. Everything we normally think of as "the agent" —
the ReAct turn loop, context compaction, subagents, every built-in tool, slash commands, personas —
ships as **declarative data** the kernel interprets. Defaults and user extensions are the *same
kind of artifact*: a **component**.

```
              ┌──────────────────────── astrite++ kernel (C++) ──────────────────────┐
              │                                                                       │
  YAML        │   loader            evaluator            primitive catalog            │
 component ──▶│  confined parse ──▶ walk behavior  ──▶  effect | predicate | transform│
              │  + typed decode     tree, tick by                  │                  │
              │  + authority-fold   tick (total)                   ▼                  │
              │  (abort on          ▲          │            sandbox child             │
              │   mismatch)         │ events   │ effects   (Landlock + seccomp)       │
              │                     └──────────┘                                      │
              └───────────────────────────────────────────────────────────────────────┘
```

Three properties make this safe rather than "just a programming language with the same risk":

1. **Authority confinement, not logic prohibition.** A component can only invoke primitives
   explicitly granted to it (its **capability set**). The interpreter has no ambient authority —
   it cannot reach a syscall, file, socket, or import the kernel did not hand it. A malicious
   component is confined exactly like a single tool call.
2. **Provably total.** The only loops are bounded (`repeat {max}`) and `foreach` over a *finite*
   collection (capped). All genuinely-unbounded iteration (the SSE stream fold, retry timing, the
   input pump) lives in the **kernel**, never in data.
3. **Statically analyzable.** Because a component is finite data, the loader computes its
   **maximal reachable authority** before it runs (a pure tree-walk through iteration bodies and
   statically-known dispatch targets), enforces the lethal-trifecta rule, and rejects any uncapped
   loop — *before* a single effect fires. For embedded defaults this fold runs at **compile time**.

**There is no expression language and there are no operators.** Boolean logic is the tree
structure (`AND` = `all`, `OR` = `any`). Comparisons and containment are *predicate primitives*
that return success/failure. Computation is *transform primitives* that read references and write
a result. Data flows through a structural `{ref: …}` lookup convention that never computes.

## 4. Components

A **component** is a single YAML document that decodes into a behavior tree.

- **Load:** parse the YAML (in the confined loader child — §7), validate against the component
  **schema** (§5), then run the **authority fold** (§7). **If anything fails to match, abort —
  fail closed.**
- **Run:** **walk the tree.** Each node is either a *composition* keyword or a *leaf* that invokes
  a primitive. Effect-leaves execute inside a sandbox child.

Component kinds (all the same artifact, distinguished by a `kind` field): `tool` (model-callable,
carries a JSON input schema), `command` (user-invoked, e.g. a slash command), `agent` (a turn/loop,
incl. the default loop and subagents), `persona`/`policy`/`profile` (instruction, gating, and
endpoint data).

### Worked examples (these are the whole language)

A built-in tool, `fs_edit` — exact-match replacement over a sandboxed primitive:

```yaml
component: fs_edit
kind: tool
caps: [fs.edit]
tree:
  all:
    - do: fs.edit
      with:
        path: { ref: input.path }
        old_text: { ref: input.old_text }
        new_text: { ref: input.new_text }
        expected_replacements: { ref: input.expected_replacements, default: 1 }
      as: out
output: {ref: out}
```

The core loop, `turn` — the bounded ReAct loop expressed as data:

```yaml
component: turn
kind: agent
caps: [session.read, session.append, llm.stream, dispatch]
tree:
  repeat:
    max: 16
    until: { when: bool.is_true, with: { in: {ref: done} } }
    body:
      all:
        - do: session.read           # context = the event-sourced log
          as: ctx
        - do: llm.stream             # kernel owns the SSE fold; the leaf settles ONE value
          with: { messages: {ref: ctx.messages}, tools: {ref: cfg.tools} }
          as: resp
        - do: session.append
          with: { record: {ref: resp.message} }
        - do: coll.is_empty
          with: { in: {ref: resp.tool_calls} }
          as: done
        - any:
            - when: bool.is_true     # model asked for no tools → turn done
              with: { in: {ref: done} }
            - foreach:               # else run each requested tool
                over: {ref: resp.tool_calls}
                as: tc
                body:
                  all:
                    - do: dispatch   # kernel resolves tc.name → a registered tool component
                      with: { call: {ref: tc} }
                      as: out
                    - do: obj.build
                      with:
                        role: tool
                        tool_call_id: {ref: out.tool_call_id}
                        content: {ref: out.content}
                      as: tool_msg
                    - do: session.append
                      with: { record: {ref: tool_msg} }
```

The "keep going until no tool-calls or budget" loop is the component's bounded `repeat`. `dispatch`
resolves a tool name against the statically-known registry of tool components, so the authority
fold still sees the union of their capabilities (no dynamic-code escape). **Walking this component
end-to-end against a live model — the whole ReAct loop as data, hot-swappable from disk — is the
project's central proof milestone.**

## 5. The component schema

The schema is the closed grammar YAML is validated against — and the schema *is* the typed IR.
There is no hand-rolled string-dispatch validation pass; an unknown key, a wrong shape, or a node
with zero or two discriminator keys fails the decode with a location.

**Composition keywords** — v0: `all`, `any`, `foreach`, `repeat`. Later: `map`, `parallel`, `fold`.
(All total.)

| Keyword | Shape | Semantics |
|---|---|---|
| `all` | `[node…]` | AND — run in order, fail-fast; Success iff all succeed |
| `any` | `[node…]` | OR — first child to succeed wins (retries / failover / escalation) |
| `foreach` | `{over: ref, as: name, body: node}` | walk `body` once per element of a **finite** collection (capped at 1024); per-element binding is scoped; body status is ignored except hard errors |
| `repeat` | `{body: node, until: node, max: u32}` | walk `body` until `until` succeeds, bounded by `max` (default 16) — the kernel-free re-tick loop |
| `map` / `parallel` / `fold` | — | **later**: collection transform, concurrent children, bounded accumulator |

**Decorators** (wrap one `child`; all total) — **later, none in v0:** `invert`, `force`, `while
{cond, max}`, `timeout {ms}`, `budget {tokens|usd|ms}`, `retry {max, backoff}`, `guard {cond}`.

**Leaf forms:**

| Form | Shape | Meaning |
|---|---|---|
| Action | `do: <primitive>`, `with: {args}`, `as: <slot>?` | invoke a primitive; optionally bind its result to a blackboard slot (always Success) |
| Condition | `when: <predicate>`, `with: {args}` | invoke a predicate returning a bool; Success/Failure, no bind |

Guards are strict: `when` requires a boolean predicate result; **no generic JSON truthiness**
decides control flow. A non-boolean predicate result is a kernel error. A security-oriented kernel
should not let an accidental object return from a predicate read as "success."

**Values** are literals, `{ref: slot.path}`, or `{ref: slot.path, default: literal}` (pure lookup +
structural projection; field access and literal indices only; references never compute and never
nest).

Node status is `Success | Failure | Running`; the kernel re-ticks `Running` nodes (this is how a
blocked approval will suspend and resume).

### The IR in C++

The IR is a recursive sum type; the discriminator is key presence, decoded with
exactly-one-of-N enforcement and unknown-field rejection:

```cpp
namespace astrite::schema {

struct Node;

struct All       { std::vector<Node> children; };
struct Any       { std::vector<Node> children; };
struct Action    { std::string primitive; ArgMap with; std::optional<std::string> bind; };
struct Condition { std::string predicate; ArgMap with; };
struct Foreach   { ArgValue over; std::string item; box<Node> body; };          // box → std::indirect
struct Repeat    { box<Node> body, until; std::uint32_t max = 16; };

struct Node : std::variant<All, Any, Action, Condition, Foreach, Repeat> {
    using variant::variant;
};

struct Component {
    std::string name;                  // `component:`
    std::string kind;                  // tool | command | agent | persona | policy | profile
    std::vector<std::string> caps;
    std::optional<std::string> description;
    std::optional<Value> parameters;   // model-facing JSON Schema, passed through verbatim
    Node tree;
    std::optional<ArgValue> output;
};

} // namespace astrite::schema
```

`box<T>` is a small value-semantic heap indirection (deep copy, deep compare); it becomes
`std::indirect<T>` verbatim when stdlibs ship C++26. The walk is `std::visit` over an overload set
with no catch-all — **compile-time exhaustive**. Loader strategy: parse YAML to a generic tree
(rapidyaml, inside the confined child — §7), then a ~150-line typed decode enforcing
exactly-one-of-six discriminator keys and rejecting unknown fields, mirroring the schema above.
Embedded defaults skip the runtime path entirely: a build-time generator emits them as constexpr
IR literals (§7).

## 6. The primitive catalog

The closed set of leaves. Anything a component can *do* is here; if a capability isn't in this
catalog, the only way to add it is a reviewed in-tree C++ change — **never** a user-supplied
plugin, and **never** WASM.

> **WASM is rejected.** It is still executable code (just sandboxed), it re-introduces a
> compile/audit/opaque-binary pipeline, and behavior-as-opaque-bytecode sacrifices the
> human-comprehensibility-as-data that is the entire point. New computation is a new primitive,
> reviewed in-tree, in C++ under this project's hardening regime.

### Effect primitives (capability-tagged; the *only* things that touch the world)

Each is tagged `ReadOnly | Write | Exec | Network`, is invocable only if its id is in the
component's granted capability set, and **executes inside a sandbox child (§7 ground truth).**

| Primitive | Cap | Notes |
|---|---|---|
| `fs.read` / `fs.list` / `fs.write` / `fs.edit` / `fs.delete` / `fs.rename` / `fs.mkdir` / `fs.stat` | ReadOnly/Write | `path` is a `PathRef@base` — relative-by-type, rooted at a granted base anchor. `fs.list` returns bounded, sorted entries and does not follow symlinks; `fs.edit` is exact-match replacement with expected match counts; `fs.delete` is non-recursive; `fs.rename` rejects existing targets; 4 MiB read/write caps |
| `search.find` | ReadOnly | bounded sorted workspace text search; literal or linear-time regex (RE2-class, never backtracking); skips symlinks and generated/private state |
| `proc.exec` / `proc.exec_scratch` | Exec | `argv` is a **vector**, never a shell string (no shell-injection surface); kernel enforces timeout + output cap. `exec_scratch` runs against a disposable workspace copy |
| `llm.stream` | Network | kernel owns the SSE fold + index-keyed tool-call accumulation; leaf settles one `{text, tool_calls, finish, usage}` per tick |
| `http.fetch` | Network | **later**; host/scheme allowlist; size/time caps; counts toward lethal-trifecta |
| `session.append` / `session.read` / `session.fold` | Write/ReadOnly | append-only JSONL; `fold` is a kernel-driven cursor fold for resuming sessions larger than the blackboard |
| `tui.emit` / `tui.prompt` | Write / *UntrustedInput* | **later**; render to the UI; `tui.prompt` is the blocking human-input effect |
| `subagent.spawn` | Exec | **later**; child over a sub-tree with caps ⊆ parent (statically checked), depth/tick/token-bounded |
| `dispatch` | Dispatch | kernel resolves a tool-call's `name` → its registered tool component and ticks it. Route authority and effect authority are separate: the caller needs only dispatch, the selected tool's own caps are checked at dispatch time |
| `clock.now` / `rand.bytes` | ReadOnly | **later**; the *only* time/entropy sources; seedable for replayable runs |
| `secret.get` | ReadOnly | **later**; resolves to a redacting `Secret` value no serializing sink accepts |
| `ctx.get` | ReadOnly | runtime-owned execution facts: workspace root, override dir, session log, scratch dir, model, doctrine |
| `component.check` / `component.catalog` | — | **later**; validation, authority/risk traces, capability diffs, trust-record candidates, live component inventory |
| `path.resolve` | — | turn an untrusted path string + a base anchor into a confined path reference, or structured failure, without touching the filesystem |
| `json.coerce` | — | validate and conservatively coerce untrusted JSON into a declared shape, returning structured failure without crashing |

### Predicate primitives (pure; return Success/Failure)

v0 ships the minimum the default loop needs (`bool.is_true`, `coll.is_empty`), growing toward:
`val.eq`; `num.lt/le/gt/ge/in_range`; `str.eq_ci/contains/starts_with/ends_with/matches_glob/is_empty`;
`len.eq/lt/gt`; `coll.contains/all/any`; `map.has_key`; `slot.is_set/is_null/is_type`;
`path.within/is_relative/has_extension`; `enum.is`; `policy.permits` / `policy.requires_prompt`.
**No regex predicate** — regex lives in `search.find`.

### Transform primitives (pure; read refs, write a slot)

- **Trust-boundary (the load-bearing pair):** `json.coerce` and `path.resolve` are the *only*
  entry points from untrusted model output into the typed, confined world; everything downstream
  is then statically typed.
- **Strings/data (growing set):** `str.replace/slice/split/join/lines/trim/case`,
  `template.render` (typed, per-destination-escaped holes `${name:ShellArg|PathSeg|JsonStr|PlainStr}`),
  `diff.unified`, `json.get/parse/stringify`, `obj.build/merge`, `list.*`, `num.add/sub`.
- **Domain constructors:** `msg.system/user/assistant/tool_result`, typed UiEvent builders,
  `err.last`.

## 7. Capability & authority model — and the ground truth

Two layers; do not confuse them.

**Policy / intent (declarative, statically analyzable).** A component declares `caps`; its leaves
carry capability tags; structural guards (`when: path.within`, `when: policy.permits`) gate calls.
The authority walk traverses the whole tree — through iteration bodies and statically-known
dispatch targets — and computes the maximal reachable primitive authority. It rejects a component
whose reachable primitives exceed the granted set. There is **no runtime policy language**:
authority is which primitives/components are reachable, and that is decidable by tree-walk plus
the registered tool table. Project-local overrides that add reachable capabilities or risk
warnings are rejected fail-closed unless an exact hash-pinned user-level trust record matches.
Policy modes: `review` trust-gates authority/risk increases; `strict` rejects newly introduced
lethal-trifecta paths outright.

**Compile-time fold for embedded defaults.** The build generates constexpr IR from
`components/*.yaml` (`tools/gen_components`), and the authority fold — already a pure tree walk —
runs in `consteval`: `static_assert(reached_authority(turn) ⊆ turn.caps)` for every embedded
component. A default component that exceeds its declared capabilities is a *compile error*. Disk
overrides still go through the full runtime path, so hot-swap is unaffected.

**Enforcement floor (the ground truth).** Every effect primitive's runtime *is* a sandbox child:
`PR_SET_NO_NEW_PRIVS` → seccomp-bpf filter → Landlock ruleset scoped to exactly the granted bases →
op/exec. **This holds even if the interpreter, the authority fold, a structural guard, or a
primitive is buggy.** `path.resolve@base` producing a confined path is the *belt*; the Landlock
ruleset scoped to that base is the ground-truth *suspenders* — an escape requires **both** an
interpreter-layer bug **and** a sandbox bug. In a language without memory-safety guarantees this
layering is not optional politeness; it is the design.

Invariants:

- **Policy narrows, never widens, the OS floor.**
- **Runtime authority widening is impossible from data.** "Always allow this folder/host" is a
  kernel re-grant + re-consent producing a *new* statically-analyzed capability set — never
  something a component can do to itself.
- **Fail closed.** No OS enforcement floor available (off-Linux, or kernel too old for the needed
  Landlock ABI) ⇒ deny. Never degrade to interpreter-only confinement.

### Sandbox specifics — native syscalls, zero dependencies

Landlock, seccomp, and `no_new_privs` are raw Linux syscalls; this layer needs **no third-party
code at all**:

- **Why a child process:** Landlock and seccomp are irreversible one-way ratchets on a process and
  its descendants. The long-running kernel legitimately needs broad access (model network,
  terminal, project reads), so it cannot self-restrict. Each effect that touches the world is
  delegated to a short-lived child. Delivery is **self-re-exec with a hidden subcommand** (one
  artifact, one trust root); `proc.exec` applies the policy in the forked child before `execve`.
- **Landlock:** ruleset built at ABI **V1** for fs and **V4** for net, derived from the granted
  base anchors; a read-exec set for toolchain/system dirs (`/usr`, `/lib`, `/lib64`, `/bin`,
  `/sbin`, `/etc`, existing entries only); essential devices (`/dev/null`, …); everything else
  denied. Child network is **deny-all** — only the kernel talks to the model endpoint.
- **seccomp:** the BPF program is a **`constexpr std::array<sock_filter, N>`**, hand-assembled and
  length-checked at compile time. Default action **`EPERM`** (friendlier tool error reporting);
  the v0 filter denies `AF_INET`/`AF_INET6` socket creation; a full default-deny allowlist is the
  eventual target.
- **Child env hygiene:** `clearenv()` + minimal envp + clean HOME — the API key lives in the
  parent's environment and never reaches a child.

### The confined loader (C++-specific, load-bearing)

Untrusted bytes get parsed by C/C++ parsers; a parser bug must not be kernel compromise. So the
loader reuses the effect-child machinery: **component YAML from disk is parsed in a no-fs, no-net,
rlimited self-re-exec child**, which returns either the typed IR over a pipe in a dumb
length-prefixed encoding, or a structured error. The kernel-side decoder of that pipe protocol is
deliberately trivial (fixed tags, length checks, depth caps) — the complex, grammar-shaped attack
surface (YAML anchors, indentation, UTF-8 edge cases) never runs in the kernel's address space.
A malformed or hostile component file produces a confined child abort and a clean "rejected:
parse failure" — which is also exactly the fail-closed behavior the trust model wants.

Trust: project-local components are **untrusted by default** (a planted config file is an attack).
An override is schema-checked, name-checked, authority-checked, reported with capability and
risk-warning diffs, and rejected if it adds reachable authority or risk warnings without an exact
hash-pinned user-level trust record.

## 8. The kernel — evaluator, concurrency, streaming

- **Evaluator:** `walk(node, bb, ctx) -> std::expected<Status, KernelError>` — plain recursion,
  `std::visit` per node, exhaustive by construction. The kernel dispatches primitive calls and the
  `dispatch` builtin. A component is always bounded. The `turn` component owns its ReAct re-tick
  with bounded `repeat {until, max}`; the kernel owns the genuinely-unbounded machinery a
  component cannot express: the SSE stream fold inside `llm.stream`, retry/backoff timing, the
  input pump, cancellation.
- **No async runtime.** The agent runs one model stream at a time and forks per-effect children;
  an executor would be ceremony. Concurrency is `std::jthread` + `std::stop_token` + one small
  bounded MPSC channel of typed runtime events that frontends consume. The turn executes on a
  worker jthread so input/rendering stay live. `parallel`/subagents later are more jthreads, not a
  new substrate. If the `Running`/approval-suspension model ever needs real suspension, C++20
  coroutines map naturally onto BT ticks (`Running` = suspended frame) — a future option, not the
  baseline.
- **Streaming:** `llm.stream` is OpenAI-compatible (`POST {base_url}/chat/completions`,
  `stream:true`, SSE) over a blocking libcurl easy handle; the write callback feeds a line
  accumulator. **Sharp edge:** streamed `tool_calls` arrive as index-keyed fragments accumulated
  across chunks before dispatch — index is untrusted (map key only, never an allocation size),
  id/name overwrite, arguments append, capped distinct calls, incomplete entries filtered.
  Cancellation via the progress callback checking a `stop_token`. Recorded SSE fixture captures
  drive the unit tests; live verification against a real endpoint is a milestone gate.
- **Cross-turn state is event-sourced** in the JSONL session log and reseeded each turn; the
  blackboard is turn/activation-scoped. There is no mutable long-lived blackboard.
- **Prompt context is runtime-owned.** The initial system record names the workspace root,
  override directory, session log, scratch directory, authority policy, loaded tool components,
  and the operating doctrine: rewrite strategy, not confinement. It deliberately omits secrets.
- **Lifecycle statechart** (coarse session lifecycle + human-in-the-loop gating, itself data) is
  a later slice; v0's driver is a plain readline loop.

## 9. Blackboard & data mechanics

- **Value:** a small recursive variant — `Null, Bool, Int(i64), Float, Str, List, Object` — where
  `Object` is a sorted `std::vector<std::pair<std::string, Value>>` (flat-map representation;
  incomplete-type-safe, cache-friendly, deterministic iteration). ~150 lines, zero dependencies.
  Target refinement: `Bytes`, `Path`, `Handle<Cap>` members and **write-once per scope
  activation**.
- **Blackboard:** a slot-keyed store over `Value`; the only state. `foreach` scopes its
  per-element binding (save → bind → restore).
- **References:** `{ref: slot.path}` resolves by pure lookup + structural projection (field
  access, literal array indices) — never computes; missing/type-mismatch → `KernelError` (no
  crash, no silent null).
- **Templating (later):** `template.render` fills only typed, per-destination-escaped holes from
  refs — injection-safe by construction; escapers are pure kernel functions.

## 10. Frontends

- **v0: readline.** `stdin` line loop + streamed deltas to stdout + tool activity lines, consuming
  the runtime event channel. This is the end-to-end proof vehicle, not a throwaway: the runtime is
  frontend-agnostic and the readline frontend stays.
- **TUI: FTXUI** (vendored, itself zero-dependency) — follow-tail scrollback, wrapping input,
  selectable/collapsible transcript segments, slash commands, component authority cards, trust
  prompts. Unidirectional state + `update`/`view`; no business logic in the view. A raw-ANSI
  floor is the fallback if FTXUI disappoints.

## 11. Config, sessions, components on disk

- **Config:** env vars first (`OPENAI_API_KEY`, `OPENAI_MODEL`, `OPENAI_BASE_URL`,
  `ASTRITE_AUTHORITY_POLICY`); TOML at the platform config dir + per-project `.astrite/` later.
- **Components:** the default agent ships as embedded built-in components (generated constexpr IR);
  users override them under `.astrite/components/<name>.yaml` (disk override preferred — this is
  what makes the loop hot-swappable). Overrides go through the confined loader + authority fold +
  trust gate.
- **Sessions:** append-only JSONL at `.astrite/sessions/current.jsonl` — the event-sourced
  transcript the `turn` component reads and appends to. Line-indexed reader and `tail`/`range`
  facade for transcript windows; per-id sessions and resume later.

## 12. Tech baseline

| Decision | Choice | Note |
|---|---|---|
| Language | **C++23 floor** | GCC 14+ / Clang 18+; C++26 features adopted as toolchains ship them (`std::indirect`, contracts, reflection) |
| License | `MIT OR Apache-2.0` | |
| Build | CMake + Ninja, **pinned toolchain** | no network at configure or build time; everything vendored |
| Exceptions / RTTI | `-fno-exceptions -fno-rtti` (lean) | `std::expected` at every boundary; parser aborts are confined to the loader child, where abort *is* fail-closed |
| Concurrency | `std::jthread` + `std::stop_token` + bounded MPSC | no async runtime, no executor |
| HTTP/TLS | **system libcurl** | easy API, blocking, write-callback SSE, HTTP/2 free; same trust class as libc — not a vendored dependency |
| YAML | **rapidyaml** (vendored) | parsed only inside the confined loader child; anchor expansion bounded (billion-laughs guard) |
| JSON | **glaze** (vendored, header-only, zero-dep) | exception-free; rejects unknown keys by default |
| Regex | RE2-class only if/when needed | linear-time guarantee; **no backtracking engine, ever** |
| TUI | **FTXUI** (vendored) — later milestone | readline frontend first |
| Errors | `std::expected<T, Error>` + monadic ops | typed error enums per layer; no exceptions, no errno soup |
| Logging | stderr first; structured later | |
| Tests | Catch2 amalgamated or doctest (vendored, dev-only) | open decision — §18 |
| Fuzzing | libFuzzer targets: loader pipe decode, YAML→IR, SSE, `json.coerce` | permanent CI smoke + growing corpus |
| Sandbox | raw syscalls: `prctl(2)`, `seccomp(2)`, `landlock_*(2)` | **zero dependencies**; constexpr BPF |

## 13. Project layout

Thin binary over libraries so everything is unit- and integration-testable. Namespaces mirror
directories (`astrite::schema`, `astrite::kernel`, …). Named C++ modules are an open decision
(§18); the layout does not depend on it.

```
astrite++/
├─ CMakeLists.txt            # Ninja, pinned toolchain, vendored deps only, hardening flags
├─ DESIGN.md / TODO.md
├─ third_party/              # vendored, pinned source: rapidyaml, glaze, (ftxui, test fw)
├─ tools/
│  └─ gen_components/        # build-time YAML → constexpr IR codegen (in-tree C++, same toolchain)
├─ src/
│  ├─ schema/                # Value, ArgValue, Node, Component; pipe-protocol types
│  ├─ kernel/                # walk, blackboard, refs, authority fold + trace, registry
│  ├─ sandbox/               # no_new_privs → seccomp → Landlock; self-re-exec children
│  ├─ loader/                # confined parse child + kernel-side IR decode + trust records
│  ├─ llm/                   # curl SSE client + index-keyed tool-call accumulation
│  ├─ runtime/               # effect prims, event channel, turn execution, config
│  └─ frontends/
│     ├─ readline/           # v0 frontend (stays)
│     └─ tui/                # FTXUI frontend (later)
├─ components/               # the DEFAULT AGENT, as data (turn.yaml, fs_read.yaml, …)
└─ tests/                    # unit + enforcement + golden traces; tests/fuzz/ targets
```

## 14. Dependency hygiene — the reason this project exists

- **Vendored source only.** Three libraries (`rapidyaml`, `glaze`, later `ftxui`) plus a dev-only
  test framework live in `third_party/` as pinned, hash-recorded source — no package manager, no
  registry resolution, no `FetchContent` at configure time, **no install- or build-time code
  execution by dependencies**. Updating a dependency is a reviewed diff like any other.
- **System libcurl** is linked, not vendored: it is in the same trust class as libc, the
  dynamic linker, and the kernel — infrastructure the host already trusts, patched by the distro.
- **The sandbox layer has zero dependencies.** The layer with the strongest claim has the
  smallest surface.
- The build is **network-free and reproducible-leaning**: pinned toolchain version, `-ffile-prefix-map`,
  deterministic archives. A CI job builds with the network namespace disabled to prove it.

## 15. Memory safety & hardening

The ledger from §1, made operational:

- **Style floor (enforced):** value semantics throughout the kernel; no owning raw pointers, no
  manual `new`/`delete`, no pointer arithmetic, no shared mutable state across threads except the
  event channel. clang-tidy (bugprone-\*, cert-\*, cppcoreguidelines ownership checks) + `-Wall
  -Wextra -Wconversion -Werror` are CI gates.
- **Hardened release builds:** hardened stdlib mode (`_GLIBCXX_ASSERTIONS` / libc++ hardening —
  bounds-checked `operator[]` in production), `_FORTIFY_SOURCE=3`, `-fstack-protector-strong`,
  `-fstack-clash-protection`, `-ftrivial-auto-var-init=zero`, `-fcf-protection`, full RELRO +
  `-z now`, PIE.
- **Sanitizer CI:** ASan+UBSan on the full test suite every commit; TSan on the runtime/channel
  tests; MSan or Valgrind where practical.
- **Permanent fuzzing:** libFuzzer targets on every untrusted-input decoder — the loader pipe
  protocol, the YAML→IR decode (run inside *and* outside the child in fuzz builds), the SSE/
  tool-call accumulator, `json.coerce`. Corpus checked in; CI smoke-fuzzes per commit.
- **Architectural distrust:** the confined loader child (§7) — the grammar-shaped parsing attack
  surface never runs in the kernel's address space.
- **Trajectory:** C++26 contracts (`pre`/`post` on the primitive boundary: argv non-empty, paths
  confined, output capped) in hardened builds; C++26 erroneous behavior tightens uninitialized
  reads; static reflection eventually derives the typed decode from the IR definitions.

## 16. Errors, observability, testing

- **Errors:** typed error enums per layer carried in `std::expected`; a single render boundary in
  the frontend. No exceptions (§12).
- **Observability:** stderr diagnostics first; structured span-style logging around each
  tick/effect/sandbox invocation later; effective Landlock ABI + seccomp action recorded per
  session.
- **Tests:** per-layer units; **schema-validation tests** (malformed components fail the decode,
  unknown keys rejected, zero/two-discriminator nodes rejected); **kernel walk tests** (all six
  nodes with fake primitives, ref resolution, scoping); **authority tests** (cap reachability,
  dispatch tracing, a directory scan that loads and checks every bundled component, strict-mode
  trifecta rejection); **sandbox enforcement tests** (denied paths/syscalls actually fail — fs
  confinement, TCP, UDP; real dev tools still run under the exec policy; Linux-gated);
  **loader-child tests** (hostile YAML → confined failure, pipe-protocol fuzz); **SSE/stream
  tests** (fragment accumulation over recorded captures); and eventually the **golden suite**
  asserting every default component produces its expected effect trace — the living proof of the
  thesis.

## 17. Milestones

Summary — the full checklist lives in [TODO.md](TODO.md).

| # | Deliverable | Proves |
|---|---|---|
| M0 | Skeleton: toolchain, CMake, vendoring, CI gates | the hygiene story is real from day one |
| M1 | Sandbox floor (native syscalls, constexpr BPF) | enforcement tests pass; zero-dep claim |
| M2 | Schema + kernel: Value, IR, walk, blackboard, authority fold | the clean-core claim |
| M3 | Confined loader child | parser bugs are confined crashes |
| M4 | LLM client: curl SSE + tool-call accumulation | streaming sharp edge handled |
| M5 | Runtime: effect prims, registry, events, turn execution | the wiring |
| M6 | **Loop-as-data**: embedded `turn.yaml` (consteval-checked) + readline frontend, live end-to-end | **the central bet** |
| M7 | fs/search/exec primitives + tool components under the sandbox | a useful agent |
| M8 | FTXUI TUI | daily-drivable |
| M9 | Trust & self-modification: `component.check`/`catalog`, override diffs, trust records | the authority story complete |
| M10 | Polish: config, sessions/resume, packaging | |
| P2 | MCP client, native non-OpenAI providers | **[PHASE 2]** |

## 18. Open decisions

1. **Named modules vs headers.** Start with headers + namespaces; revisit modules (`import std;`,
   `astrite.kernel`) once the pinned toolchain handles them without friction.
2. **Test framework:** Catch2 v3 amalgamated (active, two files) vs doctest (single header,
   quieter maintenance). Dev-only either way.
3. **`-fno-exceptions` confirmation** once rapidyaml's error-callback path is validated in the
   child (abort-in-child is acceptable fail-closed behavior; confirm ergonomics).
4. **`box<T>` now → `std::indirect<T>`** migration timing as stdlibs ship it.
5. **TUI:** FTXUI vs raw-ANSI floor — decide at M8 with a spike.
6. **Approval mode names/defaults** (`read-only` / `auto-edit` / `full-auto`).
7. **Secrets backend** (env vs OS keyring); **packaging** (static-leaning binary? musl?).
8. **seccomp endgame:** keep deny-inet default-allow, or move to a full default-deny allowlist.
9. **Coroutine ticks** for `Running`/approval suspension — only if/when the lifecycle statechart
   needs real suspension.
10. **Generated-IR format:** C++ designated-initializer literals (readable diffs) vs embedded
    compile-time-parsed bytes. Lean: literals.
