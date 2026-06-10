# astrite++ — TODO

Goal: a modern-C++ astrite that runs the **full model loop** (read transcript → stream model →
dispatch tools → re-tick) as **data** — with native sandboxing, a confined loader, compile-time
authority checks on the shipped defaults, and a third-party surface small enough to review in one
sitting: **2 vendored source libs + system libcurl** (FTXUI arrives with the TUI), zero of which
execute code at install or build time.

Design rationale and the full spec live in [DESIGN.md](DESIGN.md). Order below is
dependency-ordered; **M6 is the central bet** (loop-as-data, live, end-to-end).

---

## Dependency budget (the whole point)

Target — hold this line; every addition is a reviewed diff:

| layer | deps | notes |
|---|---|---|
| sandbox | **none** | raw syscalls: `prctl`, `seccomp(2)`, `landlock_*(2)`; constexpr BPF |
| kernel / schema / blackboard | **none** | stdlib only (`variant`, `expected`, hand-rolled `Value`) |
| component decode (JSONC) | `glaze` (vendored) | one grammar at the trust boundary; meta-derived typed decode, deny-unknown-keys |
| JSON / model output | `glaze` (same lib) | header-only, zero-dep, exception-free |
| HTTP / TLS / SSE | **system libcurl** | trust class of libc; not vendored, not counted |
| TUI (M8) | `ftxui` (vendored) | itself zero-dep; raw-ANSI floor is the fallback |
| tests (dev-only) | Catch2 amalgamated *or* doctest | open decision |

---

## M0 — Skeleton, toolchain, CI gates

The hygiene story must be real before any feature code exists.

- [x] CMake + Ninja; **pinned toolchain**: clang 21 via apt.llvm.org (Ubuntu's clang 18 predates
      glaze v7's concepts usage). Document in README + CI still pending.
- [x] C++23 floor; `-fno-rtti`. Exceptions stay ON in test builds (doctest assertion machinery);
      product-binary `-fno-exceptions` tracked in DESIGN §18.3 — glaze itself is exception-free.
- [x] Warning wall: `-Wall -Wextra -Wconversion -Wshadow -Werror`.
- [ ] Hardening flags wired into the Release config (DESIGN §15): hardened stdlib mode,
      `_FORTIFY_SOURCE=3`, stack protector + clash protection, `-ftrivial-auto-var-init=zero`,
      CFI/`-fcf-protection`, full RELRO, PIE.
- [x] `third_party/` vendoring convention: pinned source + `VERSIONS` file recording upstream
      commit/hash; **no FetchContent, no network at configure/build**.
- [x] Vendor `glaze` + test framework (decided: doctest 2.5.2). rapidyaml was vendored, then
      removed with the JSONC format decision.
- [ ] `.clang-format` is in place; `.clang-tidy` (bugprone-*, cert-*, cppcoreguidelines ownership
      checks) + CI enforcement pending.
- [ ] CI jobs: build+test (Debug, Release), ASan+UBSan suite, clang-tidy, format check,
      **network-disabled build** (proves the no-network claim), fuzz smoke (added in M2/M3).
- [x] `box<T>` utility (value-semantic heap indirection, deep copy/compare; exercised by the IR
      value-semantics tests) — swap to `std::indirect<T>` when the toolchain ships it.
- [ ] LICENSE (`MIT OR Apache-2.0`), README stub pointing at DESIGN.md.

## M1 — Sandbox floor (`src/sandbox/`) — zero deps, native syscalls

- [ ] `no_new_privs` via `prctl(PR_SET_NO_NEW_PRIVS)`.
- [ ] seccomp filter as **`constexpr std::array<sock_filter, N>`**: deny `AF_INET`/`AF_INET6`
      socket creation, default action `EPERM`; static_assert program length/jump validity.
- [ ] Landlock ruleset (fs ABI V1, net ABI V4 deny-all): granted base anchors + read-exec system
      dirs (`/usr`, `/lib`, `/lib64`, `/bin`, `/sbin`, `/etc`, existing only) + essential devices.
- [ ] Self-re-exec child delivery: hidden subcommand in the binary, one artifact, one trust root;
      apply ratchet (`no_new_privs → seccomp → Landlock`) before the op.
- [ ] Child env hygiene: `clearenv()` + minimal envp + clean HOME (the API key never reaches a
      child).
- [ ] Fork+pre-exec path for `proc.exec`-style children (policy applied between fork and execve).
- [ ] **Enforcement tests (Linux-gated):** read/write/list confined to base; TCP connect denied;
      UDP/raw denied via seccomp; real dev tools (`git log`) still run under the exec policy.
- [ ] Fail-closed: missing Landlock ABI / off-Linux ⇒ structured deny, never silent degrade.

## M2 — Schema + kernel (`src/schema/`, `src/kernel/`) — stdlib only

- [x] `Value`: recursive variant (`Null/Bool/Int/Float/Str/List/Object`), Object = sorted
      `vector<pair<string, Value>>`; JSON bridge via glaze generic (i64 number mode); unit tests.
- [x] `ArgValue`: `Ref { path }` / `RefDefault { path, default }` / `Lit(Value)`.
- [x] `Node` sum type per DESIGN §5 (`All/Any/Action/Condition/Foreach/Repeat`, `box<Node>`
      recursion); `Component` record.
- [x] Typed JSONC decode via glaze: wire types + `glz::meta` (serde-style), node sum type
      auto-deduced by discriminator key (= exactly-one-of-six), unknown fields rejected, required
      fields enforced, located errors; two custom readers (`ArgValue`, `Value`); typed wire→IR
      conversion.
- [x] `Blackboard`: slot store + pure `{ref}` resolution (field access, literal indices, explicit
      default only when unresolvable; missing/mismatch → `KernelError`, never silent null).
- [ ] `walk(node, bb, ctx) -> std::expected<Status, KernelError>`: `std::visit`, no catch-all
      (compile-time exhaustive); strict boolean guards (non-bool predicate result = error);
      `foreach` scoped binding (save → bind → restore) + 1024-item cap; `repeat {until, max}`.
- [ ] Primitive `Registry` + `Context { primitives, components, observer }`; pure prims
      (`bool.is_true`, `coll.is_empty`, `obj.build`).
- [ ] `dispatch` builtin: tool-call name → registered component, fresh scope, walk, return
      `{tool_call_id, content}`; unknown tool / bad args / tool failure → error-as-content
      (recoverable); corrupt envelope → walk error.
- [ ] **Authority fold + trace**: maximal reachable primitive authority by tree-walk (through
      `foreach`/`repeat` bodies and the dispatch table); reject reachable ⊃ granted; effect
      categories + risk tags; `dispatch` as a routing cap distinct from effect caps.
- [ ] Make the fold **constexpr-callable** (no allocation surprises) — consumed by M6 codegen.
- [x] Adversarial decode tests (wrong shapes, double discriminators, unknown keys, hostile
      input, comments-don't-relax-strictness).
- [ ] Kernel walk tests with fake primitives + authority tests.
- [ ] Fuzz target: JSONC → typed-IR decode.

## M3 — Confined loader (`src/loader/`) — the C++-specific piece

- [ ] Loader child: self-re-exec subcommand that reads JSONC bytes on stdin **under the M1
      sandbox (no fs, no net, rlimits)**, runs the M2 typed decode, writes typed IR or structured
      error to stdout in a **dumb length-prefixed pipe encoding** (fixed tags, length checks,
      depth caps). (JSONC already removed the anchor-bomb class by construction; the child
      confines what parser surface remains.)
- [ ] Canonical "a node must have exactly one of: all/any/do/when/foreach/repeat" load-error
      wording (today glaze's variant deduction reports missing_key/unknown_key).
- [ ] Kernel-side pipe decoder — deliberately trivial; the grammar-shaped attack surface never
      runs in the kernel's address space.
- [ ] Child abort/crash on hostile input ⇒ clean "rejected: parse failure" (fail-closed by
      construction).
- [ ] Tests: hostile JSONC corpus (malformed, deep nesting, encoding edge cases) ⇒ confined
      failures; round-trip equality (literal IR == loaded IR) for every component.
- [ ] Fuzz targets: pipe-protocol decoder; JSONC→IR in-process (fuzz builds only) to feed the
      corpus.

## M4 — LLM client (`src/llm/`) — system libcurl + glaze

- [x] OpenAI-compatible streaming: `POST {base}/chat/completions`, `stream:true`, SSE via
      blocking curl easy handle + write-callback line accumulator (no scanner-style line-length
      trap; cap line/total growth).
- [x] **Index-keyed tool-call fragment accumulation** across chunks (the sharp edge): index is
      untrusted (map key only, never an allocation size); id/name overwrite, arguments append;
      cap 64 distinct calls; incomplete entries filtered.
- [x] Settle to one `{text, tool_calls, finish, usage}` value + assistant message (`content:null`
      for pure tool-call turns); `on_delta(text)` sink for live render.
- [x] Cancellation: curl progress callback checks `std::stop_token`.
- [x] Config from env: `OPENAI_API_KEY` (required), `OPENAI_MODEL`, `OPENAI_BASE_URL`.
- [x] Recorded SSE fixture captures in `tests/` drive the unit tests; **live verification**
      against a real endpoint (streamed text + tool-call round trip) is the milestone gate.
- [ ] Fuzz target: SSE/tool-call accumulator.

## M5 — Runtime (`src/runtime/`)

- [x] Effect prims: `llm.stream` (wraps M4), `session.read`/`session.append` (append-only JSONL
      at `.astrite/sessions/current.jsonl`), `ctx.get`.
- [x] Registry wiring: pure prims + effects + `dispatch` into a `kernel::Context`.
- [x] `Runtime`: config, registry, components map (duplicate names rejected), session log;
      workspace/doctrine system prompt seeded once on empty log (DESIGN §8 — names workspace
      root, override dir, session log, policy, tools; omits secrets).
- [x] `submit_user_message(text)`: append user msg → walk(`turn`) on a worker `std::jthread` →
      typed events; serialized (one turn at a time); exactly one terminal event per turn.
- [x] Event channel: bounded MPSC of a sealed event variant (`Started, AssistantDelta,
      AssistantSettled, ToolStarted, ToolFinished, TurnFinished, TurnError, Notice`); consumer
      must drain to the terminal event (document the contract).
- [ ] TSan job over runtime/channel tests.

## M6 — **Loop-as-data, end to end** (`components/`, `tools/gen_components/`, readline frontend)

*The central bet. Everything before this exists to make this milestone honest.*

- [x] `components/turn.json` per DESIGN §4 + a trivial `echo.json` tool component.
- [ ] `tools/gen_components`: build-time JSONC → **constexpr IR literals** (in-tree C++, same
      toolchain, uses the same loader decode); generated designated-initializer code is
      committed-diff-readable.
- [ ] **`static_assert(reached_authority(c) ⊆ c.caps)` for every embedded component** — a default
      that exceeds its declared caps is a compile error.
- [x] Disk override path: `.astrite/components/<name>.json` via the M3 confined loader + M2
      authority fold (override preferred over embedded — hot-swap).
- [x] Readline frontend (`src/frontends/readline/`): stdin loop, stream `AssistantDelta` live,
      print tool activity, renderer-settled handshake so the prompt never races streamed output.
- [x] **END TO END:** chat with a live model — multi-turn, streamed text, tool dispatch through
      `turn.json`, session persistence across restarts, **hot-swap** (edit `turn.json`, behavior
      changes with no recompile). ← *primary goal met here.* **Done 2026-06-10**: live tool
      round trip + restart persistence + echo.json disk-override hot-swap all verified.
      Sprint notes: SSE fixtures are synthesized (recorded captures still wanted); embedding
      is configure-time strings (gen_components consteval IR + static_assert still open);
      overrides load un-gated with a loud stderr warning until M9.

## M7 — Real tools under the sandbox

- [ ] `path.resolve` (untrusted string + base anchor → confined `PathRef@base` or structured
      failure, no fs touch) and `json.coerce` (untrusted JSON → declared shape or failure) — the
      trust-boundary pair.
- [ ] fs primitives via sandbox children: `fs.read`/`fs.list`/`fs.write`/`fs.edit`/`fs.delete`/
      `fs.mkdir`/`fs.rename` (4 MiB caps, bounded sorted listings, exact-match edit with expected
      count, non-recursive delete, no-overwrite rename, symlinks not followed).
- [ ] `search.find`: bounded sorted workspace text search; literal matching first (linear-time
      regex engine only if/when justified — never backtracking).
- [ ] `proc.exec` (argv vector, never shell; timeout + output cap) and `proc.exec_scratch`
      (disposable workspace copy under `.astrite/tmp`, skips generated/private state).
- [ ] Tool components: `fs_read.json`, `fs_list.json`, `fs_write.json`, `fs_edit.json`,
      `search_find.json`, `exec.json`, `exec_scratch.json`, `workspace_context.json`,
      `path_resolve.json`, `json_coerce.json`.
- [ ] End-to-end confinement check: model-driven read outside the workspace denied; child network
      denied; real build/test commands succeed inside.

## M8 — UI-as-data + TUI (`src/frontends/tui/`)

*The loop-as-data bet, made a second time: views are data too (DESIGN §10).*

- [ ] `UiTree` value type + the closed `ui.*` constructor set (pure transforms, zero-cap):
      `text`/`markdown`, `row`/`column`/`group`, `card`, `kv`, `code`, `diff`, `list`,
      `collapsible`, `status`/`progress`, `action` (affordance hint, never an event handler).
      **No `ui.approval` — consent surfaces are kernel chrome.**
- [ ] `view` component kind: pure typed value → `UiTree`; `caps: []` enforced by the loader, so a
      view override adds no authority by construction.
- [ ] `ui.emit` effect (hands a finished tree to the frontend; refuses `Secret` values) +
      `ui.prompt` (the blocking human-input effect; result is untrusted input).
- [ ] Spike FTXUI vs raw-ANSI floor; decide (DESIGN §18.5). Either way the frontend is a
      **`UiTree` interpreter from day one** — no bespoke per-card rendering code, ever.
- [ ] Readline frontend gains the trivial plain-text `UiTree` renderer (same trees, both
      frontends).
- [ ] Default views as embedded components (consteval-checked): tool-result card, status line,
      transcript segments; tool components may name a view for their output.
- [ ] **Chrome invariant:** approval/trust/authority-diff/policy surfaces are kernel-rendered;
      component trees cannot produce, occlude, or restyle them.
- [ ] Interaction: `action` affordances resolve to dispatch/runtime actions; focus, scrolling,
      selection are frontend-owned; streaming deltas stay events — views render settled values.
- [ ] Event loop merging terminal input + the runtime event channel; streaming tokens append into
      the transcript; Enter submits; Ctrl-C/Ctrl-D quit; resize-safe; logs to file, never the
      terminal.
- [ ] Then iterate toward: follow-tail scrollback + manual scroll, collapsible/selectable
      segments, slash commands, command palette, JSONL tail hydration + paged older records.
- [ ] Tests: golden `UiTree` snapshots for every default view; render smoke through both
      interpreters.

## M9 — Trust & self-modification

- [ ] `component.check`: parse/schema/authority/risk report for a local component file; baseline
      comparison (added caps, risk warnings, untrusted input/sink flow, SHA-256 identity, exact
      approval-record candidate).
- [ ] `component.catalog`: live component inventory with source/trust metadata, hashes, declared/
      reached authority, baseline diffs for overrides, policy status, callable tool names.
- [ ] Hash-pinned user-level trust records + runtime append API; startup trust prompts for exact
      override authority/risk deltas.
- [ ] Policy modes: `review` (trust-gate increases) / `strict` (reject new lethal-trifecta paths
      outright) via `ASTRITE_AUTHORITY_POLICY`.
- [ ] Propose/accept flow: model-proposed components are inert until re-checked + consented
      (same path as any disk override).
- [ ] Custom/agent-proposed `view` components through the same override → check → trust path;
      `caps: []` means the authority diff is empty by construction, so review is
      presentation-only. *The payoff: agents design their own UI elements.*

## M10 — Polish

- [ ] TOML config + per-project `.astrite/`; profiles.
- [ ] Per-id session files; resume (`session.fold` cursor).
- [ ] Structured logging; effective Landlock ABI + seccomp action recorded per session.
- [ ] Golden suite: every default component asserts its expected effect trace.
- [ ] Packaging; reproducible-build check.

---

## Guardrails (CI, standing)

- [ ] **No new third_party** without a `VERSIONS` entry + review note in the PR.
- [ ] Network-disabled build job (vendoring claim stays true).
- [ ] ASan+UBSan on every commit; TSan on runtime tests; fuzz smoke on every untrusted-input
      decoder; corpus checked in.
- [ ] clang-tidy + format + `-Werror` gates.
- [ ] Grep guards: no `system(`/`popen(` anywhere; raw `syscall(`/`fork(`/`execve(` confined to
      `src/sandbox/`; no `throw` outside third_party.
- [ ] Linux-gated sandbox enforcement tests must run in CI (a runner with Landlock ≥ ABI V1), not
      only locally.

## Known third-party issues

- **glaze v7.8.0 YAML reader** (historical): nested multi-entry block-style mappings read into
  map-typed members lose indent context after the first entry and misattribute the next key to
  the parent object; document-level generic reads were unaffected. This, plus YAML's grammar
  attack surface, drove the decision to make **JSONC the canonical component format** — the YAML
  reader is no longer in any code path. Report upstream; re-verify if YAML authoring sugar ever
  returns.

## Open decisions

Tracked in DESIGN §18: modules vs headers; test framework; `-fno-exceptions` confirmation;
`box<T>` → `std::indirect`; TUI library; approval-mode names; secrets backend; packaging; seccomp
endgame; coroutine ticks; generated-IR format; v0 widget vocabulary.
