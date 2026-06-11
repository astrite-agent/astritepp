# astrite++ — TODO

Goal: a modern-C++ astrite that runs the **full model loop** (read transcript → stream model →
dispatch tools → re-tick) as **data** — with native sandboxing, a confined loader, compile-time
authority checks on the shipped defaults, **no shell** (execution is declared task components run
as transactions — DESIGN §6), and a third-party surface small enough to review in one
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

*Enforcement core done 2026-06-10: `apply()` ratchet + filters + Linux-gated enforcement tests,
all proven by forked syscall probes. Delivery mechanisms (self-re-exec, fork+exec) are wired by
their consumers — M3 confined loader, M7 fs/task — and check off there.*

- [x] `no_new_privs` via `prctl(PR_SET_NO_NEW_PRIVS)` (first rung of the ratchet).
- [x] seccomp filter as **`constexpr std::array<sock_filter, 10>`**: arch-guarded, denies
      `AF_INET`/`AF_INET6` socket creation with `EPERM`, default-allow (DESIGN §7 v0); explicit
      jump-offset layout + `static_assert`. (Full default-deny allowlist still the eventual goal.)
- [x] Landlock ruleset with **best-effort ABI clamp** (queries version, masks fs/net rights to
      what the kernel + headers support — newer flags `#ifdef`-guarded since headers can lag the
      kernel): per-base path rules (read = read/list/exec; write adds create/write/remove),
      net deny-all by handling net + adding no net rules. `system_read_exec_paths()` grants the
      existing toolchain dirs.
- [x] Self-re-exec child delivery: hidden `__sbx` subcommand, one artifact, one trust root —
      done with M7b's fs helpers (`run_fs_op` forks+execs `/proc/self/exe`). The confined loader
      (M3) reuses the same mechanism.
- [ ] Child env hygiene: `clearenv()` + minimal envp + clean HOME. **→ M7** (fork+exec path).
- [ ] Fork+pre-exec path for `task.run` children. **→ M7.**
- [x] **Enforcement tests (Linux-gated):** read/write confined to base; `AF_INET`/`AF_INET6`
      denied; `AF_UNIX` still allowed; a real dynamically-linked tool (`true`) execs under the
      policy; unopenable base skipped (fail-closed), ungranted path denied.
- [x] Fail-closed: missing Landlock ABI ⇒ `apply()` returns false ⇒ child exits 126, body never
      runs. (Off-Linux: the whole layer is `#ifdef __linux__`; structured off-Linux deny is M7
      when a frontend actually invokes it.)

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
- [x] `walk(node, bb, ctx) -> std::expected<Status, KernelError>`: `std::visit`, no catch-all
      (compile-time exhaustive); strict boolean guards (non-bool predicate result = error);
      `foreach` scoped binding (save → bind → restore) + 1024-item cap; `repeat {until, max}`.
- [x] Primitive `Registry` + `Context { primitives, components, observer }`; pure prims
      (`bool.is_true`, `coll.is_empty`, `obj.build`).
- [x] `dispatch` builtin: tool-call name → registered component, fresh scope, walk, return
      `{tool_call_id, content}`; unknown tool / bad args / tool failure → error-as-content
      (recoverable); corrupt envelope → walk error.
- [x] **Authority fold + trace**: maximal reachable primitive authority by tree-walk (through
      `foreach`/`repeat` bodies and the dispatch table); reject reachable ⊃ granted; effect
      categories + risk tags still pending; `dispatch` is a routing cap distinct from effect caps.
- [ ] Make the fold **constexpr-callable** (no allocation surprises) — consumed by M6 codegen.
- [x] Adversarial decode tests (wrong shapes, double discriminators, unknown keys, hostile
      input, comments-don't-relax-strictness).
- [x] Kernel walk tests with fake primitives + authority tests.
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

## M7 — Real tools under the sandbox: fs/search + tasks & transactions

*The second bet (no shell — DESIGN §6) starts collecting evidence here.*

- [x] `path.resolve` + `json.coerce` — the trust-boundary pair (M7a, pure zero-cap prims).
- [x] **Self-re-exec delivery (M1's deferred item), done M7b**: hidden `__sbx` subcommand;
      `run_fs_op` forks + execs `/proc/self/exe` into a fresh confined child (safe from a
      multithreaded host — execs immediately), reads the framed result over a pipe. Frontends +
      test binary dispatch the helper first in `main()`.
- [x] fs **read** primitives via sandbox children: `fs.read` (4 MiB cap, rejects dirs),
      `fs.list` (bounded sorted listing, dirs marked, symlinks omitted). Belt (`path.resolve`)
      in the prim + suspenders (Landlock read-confined child).
- [x] fs **write** primitives (M7c): `fs.write` (create/overwrite, content via the child's
      stdin so it never goes through argv) and `fs.edit` (exact-match replacement, fails unless
      the match count equals `expected_replacements` so edits never silently mis-apply). The
      helper gained a write-confined mode (`write_paths` policy) + a bidirectional pipe; Landlock
      denies writes outside the base (proven), a count mismatch leaves the file untouched
      (proven). `fs.delete`/`fs.mkdir`/`fs.rename` still pending (with the task/transaction half).
- [x] `search.find`: bounded literal text search (path:line:text, capped); skips symlinks,
      binary files, and VCS/build state dirs. (Linear-time regex engine only if/when justified.)
- [ ] `task` component kind (DESIGN §4/§6): pinned argv template with typed, shape-validated
      holes (`ident`/`path-seg`/enum/int-range; **never option position**; `--` separators pinned
      in the template), cwd anchor, effect class (`check` | `produce` + declared product paths),
      `net` declaration (consent-diffed), timeout, output caps.
- [ ] `task.run` — the **only** execution primitive, `dispatch`'s twin: verb resolved against the
      static task table; parameters bind as discrete argv elements; the model selects, never
      composes.
- [ ] Transactions: persistent task-scoped scratch (reflink/CoW where the fs supports it; copy +
      skip-lists elsewhere — caches persist so hermetic stays fast); no network unless declared;
      settle to `{status, output (capped, untrusted), delta}`; `check` promotes nothing,
      `produce` promotes only declared product paths; promotion via kernel chrome.
- [ ] Live-workspace execution = per-invocation kernel-chrome escalation, never grantable;
      frontend `!command` stays human-composed, human authority.
- [ ] Project-type detection proposes task candidates (Cargo/CMake/npm…); consent batched at
      first contact.
- [ ] Argument-injection corpus: leading-dash hole values rejected, option-position holes
      rejected at load, pinned `--` honored.
- [x] fs tool components embedded + authority-checked: `fs_read.json`, `fs_list.json`,
      `search_find.json`, `fs_write.json`, `fs_edit.json`. (`workspace_context` + dogfood task
      components pending with the task half.)
- [x] End-to-end confinement check (live, 2026-06-10): model listed `components/`, read
      `echo.json`, and an attempted `/etc/hostname` read was rejected by the belt
      (`absolute path is an escape`); the suspenders (Landlock denying `..` past the base even
      when the belt is bypassed) are proven in the fs-helper unit tests. Task network/scratch
      checks land with the task half.

## M8 — UI-as-data + TUI (`src/frontends/tui/`)

*The loop-as-data bet, made a second time: views are data too (DESIGN §10).*

- [x] **FTXUI scaffold (pre-UiTree), 2026-06-10**: vendored ftxui v6.1.9 (examples/doc stripped,
      stub CMakeLists noted in VERSIONS); `astrite-tui` binary — input box (Enter submits,
      `/quit` exits), follow-tail transcript, per-turn worker thread, event pump repainting on
      runtime events; transcript state is a pure, unit-tested event fold (`transcript.hpp`)
      shared-ready for the UiTree interpreter; live-verified under a pty (tool card rendered
      mid-turn). Warning wall moved to a target-scoped interface so vendored trees build with
      their own flags.
- [x] **TUI chrome enhancement, 2026-06-10**: header bar (branding + model + workspace + live
      `ready`/`working` status with an animated spinner driven by a ticker thread); scrollable
      transcript via `focusPositionRelative` with follow-tail that releases on manual scroll
      (PageUp/PageDown/Home/End + mouse wheel) and a "↓ more" indicator; styled segments
      (cyan user lines, streaming `▌` cursor, bordered tool cards with clipped results, red
      error cards); keybinding footer; **Escape cancels an in-flight turn** — wired through new
      runtime `submit_user_message(text, stop_token)` cancellation that aborts the curl stream.
      Durable across the UiTree migration (only per-segment drawing gets replaced). The deeper
      M8 (UiTree value type, `ui.*` vocabulary, `view` components, chrome invariant) is below.
- [x] **TUI interaction polish, 2026-06-10**: a local slash-command registry (`commands.hpp`,
      pure + unit-tested) with a live **completion popup** (Tab fills the highlighted command);
      scrollable **modal overlays** for `/help` · `/commands` · `/status` (dbox + centered panel,
      Esc closes); a pure **view-state model** (`view_state.hpp`, unit-tested) for focus /
      selection / collapse / modal; **Tab** completes a slash token or switches focus to the
      transcript; transcript focus gives **indexed per-segment selection** (↑/↓, Home/End) with
      **collapse/expand** (Enter/Space, plus `/collapse`·`/expand` bulk) — tool cards and
      assistant text fold to a one-line summary; `/clear`, `/top`, `/bottom`, `/focus`. Pure
      cores green (11 cases); live-verified under a pty (popup → Tab → modal → Esc → turn).
- [x] **Markdown rendering, 2026-06-10**: safe zero-dep markdown subset parser (`markdown.hpp`,
      pure + 12 tests; no HTML/raw passthrough — unrecognised input stays literal) + an FTXUI
      renderer (`markdown_render.hpp`) flowing styled inline runs through a wrapping flexbox.
      Settled assistant messages render styled; `/help` is authored as markdown. Backs the future
      `ui.markdown` constructor.
- [x] **vi bindings**: j/k/g/G + Ctrl-U/Ctrl-D half-page on transcript nav and modal scroll
      (only where the user isn't typing). `select_relative` added to view-state (tested).
- [x] **`/config` interactive dialog**: a modal showing runtime info (model, base url, workspace,
      authority policy, session) + selectable preference toggles (render markdown, always-index)
      flipped with Enter/Space. Persistent file-backed config remains M10.
- [x] **Readline-style input, 2026-06-10**: not GNU readline (terminal-ownership conflict with
      FTXUI + GPL vs our MIT/Apache, minimal-dep ethos) — a focused extension of FTXUI's Input.
      Removed the default `inverted` transform (no more white input background). Pure tested
      `history.hpp`: Up/Down recall with draft preservation + backward substring search.
      **Ctrl-R reverse-i-search** mode (type to narrow, Ctrl-R cycles older, Enter accepts,
      Esc cancels). Emacs editing via bound `cursor_position`: Ctrl-A/E (line ends), Ctrl-U/K
      (kill to start/end), Ctrl-W/Alt-Backspace (kill word back), Alt-b/f (word motion),
      Alt-d (kill word forward). Follow-ups to expand: kill-ring, vi command mode.
- [x] **FTXUI leverage pass, 2026-06-11** ("use the native thing, don't hand-roll"): `clear_under`
      modal backdrop (the idiom FTXUI's own Modal uses); OSC-8 `hyperlink()` markdown links;
      `hflow` for styled wrapping (what `paragraph()` uses internally); RAF spinner +
      `Post(Closure)`+`Event::Custom` plumbing (ticker thread + mutex gone); `/commands` & `/status`
      as `Table`; token-usage `gauge()` + `graph()` from real `stream_options.include_usage` data
      (context indicator in header, gauge+sparkline in /status); built-in mouse drag-selection
      styled via `selection*` decorators. Off-screen `Screen` render tests guard composition.
- [x] **Render-loop bug found+fixed (2026-06-11)**: FTXUI is event-driven, not reactive — a
      `Closure` task runs but does NOT invalidate the frame; only Event/AnimationTask do (proven
      with a render-counting harness). Out-of-band updates must `PostEvent(Event::Custom)`.
- [ ] **Interactive-component hosting (Collapsible cards, completion as a focusable widget, config
      Checkboxes)** — all need a root-`Container` + FTXUI-managed-focus rearchitecture, which is the
      M8 UiTree-interpreter shape. Assessed (2026-06-11) as a focused effort, not a bolt-on onto the
      single-`Renderer`+`CatchEvent` model. **Finding:** the completion popup as a `Menu` is a
      *non-improvement* — a `Menu` wants focus while the user types in the `Input`; the current
      DOM autocomplete (Input focused + manual selected-index + rendered list) is the idiomatic
      pattern for autocomplete-under-input, like `Input` itself being the one component worth using.

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
- [ ] Agent-proposed `task` components through the same path ("run the fuzzer for 60 seconds"):
      consent card shows exact argv template, scope, effect class, net; hash-pinned on approval.
      *The payoff: agents grow their own verb set — reviewably.*

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
endgame; coroutine ticks; generated-IR format; v0 widget vocabulary; bounded job handles for
interactive/long-running processes (until then: the human-authority `!command` hatch).
