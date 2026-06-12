// The FTXUI frontend. Pure, unit-tested models do the thinking — transcript.hpp
// (the event fold), commands.hpp (the slash registry), view_state.hpp (focus /
// selection / collapse / modal). This file wires them to FTXUI: a header with a
// live spinner, a scrollable styled transcript with per-segment selection and
// collapse, a slash-command completion popup, scrollable /help·/commands·/status
// modals, Tab for completion-or-focus-switch, and Escape to cancel a turn.
// NOTE(M8): per-segment drawing here is the pre-UiTree scaffold; the deeper M8
// replaces it with the UiTree interpreter. The shell (scroll/popup/modal/nav/
// cancel) is durable across that migration.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include "frontends/common.hpp"
#include "llm/provider_factory.hpp"
#include "frontends/tui/commands.hpp"
#include "frontends/tui/config.hpp"
#include "frontends/tui/history.hpp"
#include "frontends/tui/markdown_render.hpp"
#include "frontends/tui/segment_render.hpp"
#include "frontends/tui/transcript.hpp"
#include "frontends/tui/transcript_io.hpp"
#include "frontends/tui/transcript_view.hpp"
#include "frontends/tui/view_state.hpp"
#include "frontends/tui/widgets.hpp"
#include "runtime/runtime.hpp"
#include "sandbox/sandbox.hpp"
#include "util/dotenv.hpp"

namespace {

using namespace ftxui;
namespace at = astrite::tui; // FTXUI also defines `Focus`/`Modal`; keep ours qualified
using astrite::tui::Segment;
using astrite::tui::ViewState;

using astrite::tui::clip_lines;
using astrite::tui::first_line;
using astrite::tui::render_segment;
using astrite::tui::segment_body;

// The /config panel element, framing the (interactive) checkbox `fields` with
// read-only runtime info. Returned without center/clear_under — the FTXUI Modal
// component adds those. The checkboxes themselves are real components (mouse +
// keyboard), built in main() and bound to the preference bools.
Element config_panel(const std::string& provider, const std::string& model,
                     const std::string& base_url, const std::string& workspace,
                     const std::string& policy, const std::string& session, Element fields) {
    auto info = [](std::string k, std::string v) {
        return hbox({text(std::move(k)) | color(Color::GrayLight) | size(WIDTH, EQUAL, 14),
                     text(std::move(v))});
    };
    return vbox({
               text(" config ") | bold | color(Color::White) | bgcolor(Color::Blue),
               separator(),
               text("Runtime") | bold | color(Color::YellowLight),
               info("provider", provider),
               info("model", model),
               info("base url", base_url),
               info("workspace", workspace),
               info("authority", policy),
               info("session", session),
               text(""),
               text("Preferences  (space toggles · click · esc closes)") | bold |
                   color(Color::YellowLight),
               std::move(fields),
               text(""),
               paragraph("Persistent, file-backed config (a profile component) is planned for "
                         "M10; these toggles apply to the current session.") |
                   dim,
           }) |
           border | bgcolor(Color::Black) | size(WIDTH, LESS_THAN, 92) | size(HEIGHT, LESS_THAN, 32);
}

Element commands_modal_content() {
    std::vector<std::vector<Element>> rows;
    // Style cells directly, not via SelectRow/SelectColumn Decorate — those also
    // touch the adjacent border cells (a style/dim bleed onto the grid lines).
    rows.push_back({text("Command") | bold, text("Description") | bold});
    for (const astrite::tui::SlashCommand& c : astrite::tui::all_commands()) {
        rows.push_back({text(c.name) | color(Color::CyanLight),
                        text(c.summary) | color(Color::GrayLight)});
    }
    Table table(rows);
    table.SelectAll().Border(LIGHT);
    table.SelectRow(0).Border(LIGHT);
    return table.Render();
}

// Help content is authored as markdown and rendered through the same renderer
// that styles assistant output — dogfooding it.
Element help_modal_content() {
    static const std::string kHelp = R"(# astrite++

A secure coding agent whose behaviour is declarative component data interpreted
by a small kernel. Every effect is capability-gated and runs confined by an OS
sandbox.

## Input

Type a message and press **Enter**. Start with `/` for a command — a completion
popup appears; **Tab** fills in the highlighted command.

## Navigation

**Tab** switches focus between the input and the transcript. In the transcript,
**↑/↓** or **j/k** select a segment, **Enter/Space** collapse or expand it,
**g/G** or Home/End jump to the ends. PageUp/PageDown, Ctrl-U/Ctrl-D, and the
mouse wheel scroll any time.

## Turns

**Esc** cancels an in-flight turn. The spinner in the header shows when the
agent is working.)";
    return astrite::tui::render_markdown(kHelp);
}

struct UsageView {
    int prompt = 0, completion = 0, total = 0, cache_read = 0, cache_write = 0;
    bool present = false;
    const std::vector<int>* history = nullptr;
    int context_window = 128000;
};

Element status_modal_content(const std::string& provider, const std::string& model,
                             const std::string& workspace, const astrite::tui::Transcript& t,
                             bool active, const UsageView& u) {
    int tools = 0, turns = 0;
    for (const Segment& s : t.segments()) {
        if (s.kind == Segment::Kind::tool) {
            ++tools;
        } else if (s.kind == Segment::Kind::user) {
            ++turns;
        }
    }
    // Label cells get the soft colour directly; SelectColumn(0).Decorate(dim) would
    // also dim the grid lines adjacent to column 0 (a style bleed onto the border).
    auto label = [](std::string s) { return text(std::move(s)) | color(Color::GrayLight); };
    std::vector<std::vector<Element>> body{
        {label("provider"), text(provider)},
        {label("model"), text(model)},
        {label("workspace"), text(workspace)},
        {label("segments"), text(std::to_string(t.segments().size()))},
        {label("user turns"), text(std::to_string(turns))},
        {label("tool calls"), text(std::to_string(tools))},
        {label("state"), text(active ? "working" : "ready") |
                             color(active ? Color::YellowLight : Color::GreenLight)},
    };
    if (u.present) {
        body.push_back({label("prompt tokens"), text(std::to_string(u.prompt))});
        body.push_back({label("completion tokens"), text(std::to_string(u.completion))});
        body.push_back({label("total tokens"), text(std::to_string(u.total))});
        if (u.cache_read > 0 || u.cache_write > 0) {
            body.push_back({label("cache read"),
                            text(std::to_string(u.cache_read)) | color(Color::GreenLight)});
            body.push_back({label("cache write"), text(std::to_string(u.cache_write))});
        }
    }
    Table table(body);
    table.SelectAll().Border(LIGHT);

    Elements out{table.Render()};
    if (u.present) {
        const float fill = std::min(1.0f, static_cast<float>(u.prompt) /
                                              static_cast<float>(u.context_window));
        const int pct = static_cast<int>(fill * 100.0f);
        out.push_back(text(""));
        out.push_back(text("Context window") | bold | color(Color::YellowLight));
        out.push_back(hbox({
            gauge(fill) | color(pct >= 80 ? Color::RedLight : Color::GreenLight) | flex,
            text(" " + std::to_string(u.prompt) + " / " + std::to_string(u.context_window) + "  (" +
                 std::to_string(pct) + "%)") |
                dim,
        }));
        if (u.history != nullptr && u.history->size() >= 2) {
            const std::vector<int>& h = *u.history;
            out.push_back(text(""));
            out.push_back(text("Total tokens / model call") | bold | color(Color::YellowLight));
            out.push_back(graph([&h](int w, int hh) {
                              std::vector<int> cols(static_cast<size_t>(std::max(0, w)), 0);
                              int maxv = 1;
                              for (int v : h) {
                                  maxv = std::max(maxv, v);
                              }
                              for (int i = 0; i < w; ++i) {
                                  int idx = static_cast<int>(h.size()) - w + i;
                                  if (idx >= 0) {
                                      cols[static_cast<size_t>(i)] =
                                          h[static_cast<size_t>(idx)] * hh / maxv;
                                  }
                              }
                              return cols;
                          }) |
                          color(Color::CyanLight) | size(HEIGHT, EQUAL, 6));
        }
    }
    return vbox(std::move(out));
}

} // namespace

int main(int argc, char** argv) {
    if (int rc = astrite::sandbox::maybe_run_helper(argc, argv); rc >= 0) {
        return rc;
    }
    astrite::util::load_dotenv(); // pick up provider keys from a local .env (env wins)

    // Human-owned UI config (display + collapse + reasoning toggles). Loaded before
    // the runtime so show_reasoning can request provider reasoning; explicit env
    // still wins (overwrite=0), and toggling it later applies on the next launch.
    const std::filesystem::path config_path = ".astrite/config.json";
    astrite::tui::UiConfig cfg = astrite::tui::load_config(config_path);
    if (cfg.show_reasoning) {
        ::setenv("ANTHROPIC_THINKING_BUDGET", "2048", /*overwrite=*/0);
        ::setenv("OPENAI_REASONING_SUMMARY", "1", /*overwrite=*/0);
    }

    auto components = astrite::frontends::load_components();
    if (!components.has_value()) {
        std::fprintf(stderr, "astrite-tui: component load failed: %s\n",
                     components.error().c_str());
        return 1;
    }
    astrite::runtime::Options opts;
    opts.session_file = std::filesystem::path(".astrite/sessions/current.jsonl");
    opts.system_prompt = astrite::frontends::system_prompt();
    opts.components = std::move(*components);
    // A persisted in-app selection is authoritative: build that provider and inject
    // it. Empty config => the runtime falls back to the env factory (ASTRITE_PROVIDER
    // / inference). Keys always come from the environment, never config.
    if (!cfg.provider.empty() || !cfg.model.empty()) {
        auto built = astrite::llm::make_named_provider(cfg.provider, cfg.model);
        if (built.has_value()) {
            opts.provider = *built;
        } else {
            std::fprintf(stderr, "astrite-tui: %s (using environment default)\n",
                         built.error().c_str());
        }
    }
    auto rt = astrite::runtime::Runtime::create(std::move(opts));
    if (!rt.has_value()) {
        std::fprintf(stderr, "astrite-tui: %s\n", rt.error().c_str());
        return 1;
    }
    astrite::runtime::Runtime& runtime = **rt;

    // Identity comes from the *live* provider the runtime built (factory-selected),
    // not OpenAI env vars — so the header/status/config tell the truth on every
    // backend. (Empty only in the test path where a chat fn is injected.)
    // Mutable: a live /model or /provider switch updates these, and the header /
    // status / config read them by reference from their render closures.
    std::string provider = runtime.provider_name();
    std::string model = runtime.model();
    const std::string workspace = std::filesystem::current_path().string();

    ScreenInteractive screen = ScreenInteractive::Fullscreen();
    // All UI state below is touched only on the UI thread: the runtime-event pump
    // posts Closures (screen.Post) that apply transcript updates inside the event
    // loop, so no mutex is needed. The spinner animates via RequestAnimationFrame.
    astrite::tui::Transcript transcript;
    ViewState view_state;
    std::string input_text;
    std::optional<std::jthread> turn;
    const auto t0 = std::chrono::steady_clock::now();
    float scroll = 1.0f;
    bool following = true;
    int popup_sel = 0;
    float modal_scroll = 0.0f;
    bool config_shown = false; // drives the FTXUI Modal component (focusable checkboxes)
    // The transcript as literal focusable FTXUI components (M8). FTXUI manages
    // focus among segments; `body_sel` selects the transcript pane (0) vs the
    // input (1) so focus flows to the right place. `cfg` drives markdown/indices
    // and per-kind collapse defaults.
    astrite::tui::TranscriptView tv(transcript, &cfg);
    int body_sel = 1;
    int cursor_pos = 0;               // shared with the Input widget (readline editing)
    bool paste_mode = false;          // inside a bracketed paste (newlines = literal, not submit)
    astrite::tui::History history;    // Up/Down recall + Ctrl-R search
    bool rsearch = false;             // reverse-i-search mode
    std::string rquery;               // the search query being typed
    std::string rsearch_draft;        // input restored if the search is cancelled
    int rmatch = -1;                  // history index of the current match

    // Word boundaries for Ctrl-W / Alt-b / Alt-f / Alt-d.
    auto word_back = [](const std::string& s, int p) {
        int i = p;
        while (i > 0 && s[static_cast<size_t>(i) - 1] == ' ') --i;
        while (i > 0 && s[static_cast<size_t>(i) - 1] != ' ') --i;
        return i;
    };
    auto word_fwd = [](const std::string& s, int p) {
        int n = static_cast<int>(s.size()), i = p;
        while (i < n && s[static_cast<size_t>(i)] == ' ') ++i;
        while (i < n && s[static_cast<size_t>(i)] != ' ') ++i;
        return i;
    };

    std::string base_url = runtime.base_url();
    const char* env_policy = std::getenv("ASTRITE_AUTHORITY_POLICY");
    const std::string policy = env_policy != nullptr ? env_policy : "review";
    const std::string session_path = ".astrite/sessions/current.jsonl";
    // The typed UI transcript lives beside the LLM message log; restore it so a
    // reopened client shows the prior conversation (cards, errors, and all).
    const std::filesystem::path transcript_path = ".astrite/sessions/current.transcript.jsonl";
    transcript.restore(astrite::tui::load_transcript(transcript_path));
    auto persist = [&] { astrite::tui::save_transcript(transcript_path, transcript.segments()); };
    // Off-UI-thread slot for /models (a network call); auto-joined on reassign/exit.
    // Declared after the objects its closures reference so it is destroyed first.
    std::optional<std::jthread> models_job;
    // Prefer the provider's real context window; fall back to env, then a default.
    const char* env_ctx = std::getenv("OPENAI_CONTEXT_WINDOW");
    int context_window = runtime.context_window();
    if (context_window <= 0) {
        context_window = env_ctx != nullptr ? std::max(1, std::atoi(env_ctx)) : 128000;
    }

    // Token usage, updated from AssistantSettled events (UI thread only).
    struct UsageState {
        int prompt = 0, completion = 0, total = 0, cache_read = 0, cache_write = 0;
        bool present = false;
        std::vector<int> history; // total_tokens per model call, for the graph
    } usage;
    auto apply_event = [&](const astrite::runtime::Event& ev) {
        transcript.apply(ev);
        if (const auto* s = std::get_if<astrite::runtime::AssistantSettled>(&ev);
            s != nullptr && s->has_usage) {
            usage.prompt = s->prompt_tokens;
            usage.completion = s->completion_tokens;
            usage.total = s->total_tokens;
            usage.cache_read = s->cache_read_tokens;
            usage.cache_write = s->cache_write_tokens;
            usage.present = true;
            usage.history.push_back(s->total_tokens);
            if (usage.history.size() > 512) {
                usage.history.erase(usage.history.begin());
            }
        }
        // Persist on settled state, not on every streaming delta.
        if (!std::holds_alternative<astrite::runtime::AssistantDelta>(ev)) {
            persist();
        }
    };

    auto turn_active = [&] { return transcript.turn_active(); };

    // A proposal card's Approve/Reject buttons act here — the human consenting,
    // same path as /approve. Runs on the UI thread; accept takes the turn lock, so
    // guard it against an in-flight turn.
    tv.set_proposal_action([&](const std::string& name, bool approve) {
        if (!approve) {
            transcript.notice(runtime.reject_proposal(name) ? "rejected `" + name + "`"
                                                            : "no staged proposal `" + name + "`");
        } else if (turn_active()) {
            transcript.notice("finish the current turn before approving");
        } else {
            auto r = runtime.accept_proposal(name);
            transcript.notice(r.has_value()
                                  ? "approved `" + r->name + "` (" + r->kind + ", " + r->risk +
                                        " risk) — installed and pinned"
                                  : "could not approve `" + name + "`: " + r.error());
        }
        persist();
    });

    // Pump runtime events onto the UI thread. The Closure applies the transcript
    // update on the UI thread (so no mutex is needed); the Event::Custom is what
    // actually triggers the redraw. FTXUI is event-driven, not reactive — a Closure
    // task runs but does NOT invalidate the frame; only Event/AnimationTask do
    // (verified empirically). Both are queued in order, so apply-then-render.
    std::jthread pump([&] {
        while (auto event = runtime.events().pop()) {
            screen.Post([&apply_event, ev = std::move(*event)] { apply_event(ev); });
            screen.PostEvent(Event::Custom);
        }
    });

    auto run_command = [&](const std::string& line) {
        const astrite::tui::SlashCommand* cmd = astrite::tui::lookup(line);
        if (cmd == nullptr) {
            transcript.apply(astrite::runtime::TurnError{"unknown command: " + first_line(line)});
            persist();
            return;
        }
        const std::string& n = cmd->name;
        if (n == "/quit" || n == "/exit") {
            screen.Exit();
        } else if (n == "/help") {
            view_state.open_modal(at::Modal::help);
            modal_scroll = 0.0f;
        } else if (n == "/commands") {
            view_state.open_modal(at::Modal::commands);
            modal_scroll = 0.0f;
        } else if (n == "/status") {
            view_state.open_modal(at::Modal::status);
            modal_scroll = 0.0f;
        } else if (n == "/config") {
            config_shown = true; // shown via the FTXUI Modal component below
        } else if (n == "/clear") {
            if (!turn_active()) {
                transcript.clear();
                body_sel = 1;
            }
        } else if (n == "/focus") {
            body_sel = body_sel == 0 ? 1 : 0;
            if (body_sel == 0) {
                tv.select(tv.count() - 1);
            }
        } else if (n == "/collapse") {
            tv.collapse_all();
        } else if (n == "/expand") {
            tv.expand_all();
        } else if (n == "/top") {
            following = false;
            scroll = 0.0f;
        } else if (n == "/bottom") {
            following = true;
            scroll = 1.0f;
        } else if (n == "/proposals") {
            auto names = runtime.pending_proposals();
            if (names.empty()) {
                transcript.notice("no components proposed for review");
            } else {
                std::string list;
                for (const std::string& nm : names) {
                    list += (list.empty() ? "" : ", ") + nm;
                }
                transcript.notice("proposed (review with /approve <name> or /reject <name>): " +
                                  list);
            }
            following = true;
            scroll = 1.0f;
        } else if (n == "/approve" || n == "/reject") {
            // The consent action is a human action handled here (kernel chrome),
            // never by the model. The argument is the proposal name.
            std::string arg = line.size() > n.size() ? line.substr(n.size() + 1) : "";
            while (!arg.empty() && arg.front() == ' ') {
                arg.erase(arg.begin());
            }
            while (!arg.empty() && arg.back() == ' ') {
                arg.pop_back();
            }
            if (arg.empty()) {
                transcript.notice(std::string("usage: ") + n + " <proposal-name>");
            } else if (n == "/approve" && turn_active()) {
                // accept_proposal takes the turn lock; don't block the UI mid-turn.
                transcript.notice("finish the current turn before approving");
            } else if (n == "/approve") {
                auto r = runtime.accept_proposal(arg);
                transcript.notice(r.has_value()
                                      ? "approved `" + r->name + "` (" + r->kind + ", " + r->risk +
                                            " risk) — installed and pinned"
                                      : "could not approve `" + arg + "`: " + r.error());
            } else {
                transcript.notice(runtime.reject_proposal(arg) ? "rejected `" + arg + "`"
                                                               : "no staged proposal `" + arg + "`");
            }
            following = true;
            scroll = 1.0f;
        } else if (n == "/model" || n == "/provider") {
            std::string arg = line.size() > n.size() ? line.substr(n.size() + 1) : "";
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back() == ' ') arg.pop_back();
            if (arg.empty()) {
                transcript.notice(std::string("usage: ") +
                                  (n == "/model" ? "/model <model-id>" : "/provider <name> [model]"));
            } else if (turn_active()) {
                // set_provider takes the turn lock; don't block the UI mid-turn.
                transcript.notice("finish the current turn before switching");
            } else {
                std::string want_provider, want_model;
                if (n == "/model") {
                    want_provider = runtime.provider_name(); // keep the current backend
                    want_model = arg;
                } else { // /provider <name> [model]
                    const std::size_t sp = arg.find(' ');
                    want_provider = arg.substr(0, sp);
                    want_model = sp == std::string::npos ? std::string() : arg.substr(sp + 1);
                    while (!want_model.empty() && want_model.front() == ' ')
                        want_model.erase(want_model.begin());
                }
                auto built = astrite::llm::make_named_provider(want_provider, want_model);
                if (!built.has_value()) {
                    transcript.notice("could not switch: " + built.error());
                } else {
                    runtime.set_provider(*built);
                    provider = runtime.provider_name();
                    model = runtime.model();
                    base_url = runtime.base_url();
                    context_window = runtime.context_window();
                    if (context_window <= 0) {
                        context_window = env_ctx != nullptr ? std::max(1, std::atoi(env_ctx)) : 128000;
                    }
                    cfg.provider = provider; // persist the in-app choice
                    cfg.model = model;
                    (void)astrite::tui::save_config(config_path, cfg);
                    transcript.notice("switched to " + provider + " · " + model);
                }
            }
            following = true;
            scroll = 1.0f;
        } else if (n == "/models") {
            if (provider.empty()) {
                transcript.notice("no live provider — model selection unavailable");
            } else {
                transcript.notice("fetching models from " + provider + " …");
                // The catalog is a network call: run it off the UI thread and post
                // the result back. Reassigning the slot joins any prior fetch.
                models_job = std::jthread([&runtime, &screen, &transcript, transcript_path] {
                    auto models = runtime.list_models();
                    screen.Post([&transcript, transcript_path, models = std::move(models)] {
                        if (!models.has_value()) {
                            transcript.notice("models: " + models.error());
                        } else if (models->empty()) {
                            transcript.notice("models: (none reported)");
                        } else {
                            std::string list;
                            for (const astrite::llm::ModelInfo& m : *models) {
                                list += "\n  " + m.id;
                                if (m.context_limit > 0) {
                                    list += "  ctx=" + std::to_string(m.context_limit);
                                }
                            }
                            transcript.notice("models:" + list);
                        }
                        astrite::tui::save_transcript(transcript_path, transcript.segments());
                    });
                    screen.PostEvent(Event::Custom); // Closures don't redraw; this does
                });
            }
            following = true;
            scroll = 1.0f;
        }
        persist(); // most commands don't change the transcript; cheap when unchanged
    };

    // Submit the current input as one message (possibly multi-line). Called from
    // the event handler on Enter — not via Input::on_enter, because a multiline
    // Input would also insert a newline on Return.
    auto do_submit = [&] {
        std::string text = input_text;
        if (text.empty()) {
            return;
        }
        history.push(text);
        input_text.clear();
        cursor_pos = 0;
        if (text.front() == '/') {
            run_command(text);
            return;
        }
        if (turn_active()) {
            return;
        }
        transcript.user(text);
        persist();
        scroll = 1.0f;
        following = true;
        if (turn.has_value() && turn->joinable()) {
            turn->join();
        }
        turn.emplace(
            [&runtime, text](std::stop_token st) { runtime.submit_user_message(text, st); });
    };

    InputOption input_opt;
    // A single-space placeholder gives the cursor a cell on an empty input —
    // FTXUI's empty-content render path otherwise draws no cursor. multiline lets
    // pasted/typed newlines display; Enter is intercepted in the handler to submit.
    input_opt.placeholder = " ";
    input_opt.multiline = true;
    input_opt.cursor_position = &cursor_pos;
    input_opt.transform = [](InputState state) {
        if (state.is_placeholder && state.focused) {
            return state.element | focusCursorBarBlinking; // cursor on the empty field
        }
        return state.element;
    };
    input_opt.on_change = [&] { history.reset(); }; // typing exits history navigation
    Component input = Input(&input_text, input_opt);

    // The interactive tree: the transcript (real focusable segment components) and
    // the input, with `body_sel` choosing which has FTXUI focus. The Renderer below
    // draws chrome around them; the outer CatchEvent handles global/mode keys.
    Component body = Container::Vertical({tv.component(), input}, &body_sel);

    Component view = Renderer(body, [&] {
        tv.sync(); // reconcile segment components before rendering (safe: before iterate)
        const bool transcript_focus = body_sel == 0;
        const int count = tv.count();
        if (transcript_focus) {
            tv.select(tv.selected()); // clamp into range as the transcript grows/shrinks
        }
        const bool active = transcript.turn_active();

        Element transcript_el = count > 0
                                    ? tv.component()->Render()
                                    : text("Ask astrite++ to read, search, or edit the workspace.") |
                                          dim;

        if (active) {
            screen.RequestAnimationFrame(); // keep frames coming while the turn runs
        }
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  t0)
                .count();
        Element status =
            active ? hbox({spinner(5, static_cast<size_t>(elapsed_ms / 90)),
                           text(" working") | color(Color::YellowLight), text("  esc cancel") | dim})
                   : text("ready") | color(Color::GreenLight);
        Element ctx_indicator = text("");
        if (usage.present) {
            const int pct = std::min(100, usage.prompt * 100 / std::max(1, context_window));
            ctx_indicator = text("· ctx " + std::to_string(pct) + "% ") |
                            color(pct >= 80 ? Color::RedLight : Color::GrayLight);
        }
        Element header = hbox({
            text(" astrite++ ") | bold | bgcolor(Color::Blue) | color(Color::White),
            text("  " + provider + " · " + model + "  ") | color(Color::GrayLight),
            text("· " + workspace + " ") | dim,
            filler(),
            ctx_indicator,
            transcript_focus ? text("[transcript] ") | color(Color::CyanLight) : text(""),
            status,
            text(" "),
        });

        // In transcript focus, FTXUI scrolls to the focused segment (marked with
        // `focus`); otherwise honor the manual scroll (wheel / PgUp / End-follow).
        // Mouse drag-to-select is FTXUI built-in; style the selection intentionally.
        Element transcript_pane = transcript_el | selectionBackgroundColor(Color::Blue) |
                                  selectionForegroundColor(Color::White);
        transcript_pane = transcript_focus
                              ? transcript_pane | yframe | vscroll_indicator | flex
                              : transcript_pane | focusPositionRelative(0.0f, scroll) | yframe |
                                    vscroll_indicator | flex;

        Elements stack{header, separator(), transcript_pane, separator()};

        // Completion popup: a bare leading-slash token in the input box.
        auto candidates = astrite::tui::complete(input_text);
        if (!candidates.empty() && !transcript_focus) {
            if (popup_sel >= static_cast<int>(candidates.size())) {
                popup_sel = static_cast<int>(candidates.size()) - 1;
            }
            Elements rows;
            for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
                Element r = hbox({
                    text(candidates[static_cast<size_t>(i)]->name) | size(WIDTH, EQUAL, 13),
                    text(candidates[static_cast<size_t>(i)]->summary) | dim,
                });
                rows.push_back(i == popup_sel ? r | inverted : r);
            }
            stack.push_back(vbox(std::move(rows)) | borderLight | color(Color::CyanLight));
        }

        if (rsearch) {
            stack.push_back(hbox({
                text("(reverse-i-search)`" + rquery + "': ") | color(Color::YellowLight),
                text(input_text) | flex,
            }));
        } else {
            stack.push_back(hbox({text(active ? "~ " : "› ") | bold, input->Render() | flex}));
        }

        std::string hints =
            rsearch ? " type to search · ctrl-r older · enter accept · esc cancel "
            : transcript_focus
                ? " ↑/↓ j/k select · enter collapse · ←/→ enter on cards · click · tab input "
                : " enter send · ↑/↓ history · ctrl-r search · tab transcript · /help ";
        stack.push_back(hbox({text(hints) | dim, filler(),
                              following ? text("") : text("↓ more (End) ") | color(Color::YellowLight)}));

        Element root_el = vbox(std::move(stack));

        if (view_state.modal_open()) {
            std::string title;
            Element content;
            switch (view_state.modal()) {
            case at::Modal::help:
                title = "help";
                content = help_modal_content();
                break;
            case at::Modal::commands:
                title = "commands";
                content = commands_modal_content();
                break;
            case at::Modal::status: {
                title = "status";
                content = status_modal_content(
                    provider, model, workspace, transcript, active,
                    UsageView{usage.prompt, usage.completion, usage.total, usage.cache_read,
                              usage.cache_write, usage.present, &usage.history, context_window});
                break;
            }
            case at::Modal::config: // handled by the FTXUI Modal component, not here
            case at::Modal::none:
                break;
            }
            if (!title.empty()) {
                root_el = at::modal_overlay(root_el, title, content, modal_scroll,
                                            "↑/↓ j/k pgup/pgdn ctrl-u/ctrl-d scroll · esc close");
            }
        }
        if (config_shown) {
            root_el = root_el | dim; // dim the backdrop under the (clear_under) config modal
        }
        return root_el;
    });

    Component main_view = CatchEvent(view, [&](Event e) {
        // Modal open: scroll + close, swallow the rest. (/config is a separate FTXUI
        // Modal component and never reaches this handler while it is shown.)
        if (view_state.modal_open()) {
            const bool half_down = e.input() == "\x04"; // Ctrl-D
            const bool half_up = e.input() == "\x15";   // Ctrl-U
            if (e == Event::Escape || e == Event::Return || e == Event::Character("q")) {
                view_state.close_modal();
            } else if (e == Event::ArrowDown || e == Event::PageDown ||
                       e == Event::Character("j")) {
                modal_scroll = std::min(1.0f, modal_scroll + 0.12f);
            } else if (e == Event::ArrowUp || e == Event::PageUp || e == Event::Character("k")) {
                modal_scroll = std::max(0.0f, modal_scroll - 0.12f);
            } else if (half_down) {
                modal_scroll = std::min(1.0f, modal_scroll + 0.4f);
            } else if (half_up) {
                modal_scroll = std::max(0.0f, modal_scroll - 0.4f);
            } else if (e == Event::Home || e == Event::Character("g")) {
                modal_scroll = 0.0f;
            } else if (e == Event::End || e == Event::Character("G")) {
                modal_scroll = 1.0f;
            }
            return true;
        }

        // Transcript focus: navigate + collapse the real segment components. We set
        // the selector (FTXUI focus follows it); Enter/space toggle the focused one.
        if (body_sel == 0) {
            const int count = tv.count();
            constexpr int kHalfPage = 10;
            const bool interactive = tv.selected_is_interactive();
            if (e == Event::Tab || e == Event::Escape) {
                body_sel = 1; // back to input
            } else if (e == Event::ArrowUp || e == Event::Character("k")) {
                tv.select(tv.selected() - 1);
            } else if (e == Event::ArrowDown || e == Event::Character("j")) {
                tv.select(tv.selected() + 1);
            } else if (e.input() == "\x15") { // Ctrl-U
                tv.select(tv.selected() - kHalfPage);
            } else if (e.input() == "\x04") { // Ctrl-D
                tv.select(tv.selected() + kHalfPage);
            } else if (e == Event::Home || e == Event::Character("g")) {
                tv.select(0);
            } else if (e == Event::End || e == Event::Character("G")) {
                tv.select(count - 1);
            } else if (interactive &&
                       (e == Event::Return || e == Event::ArrowLeft || e == Event::ArrowRight)) {
                return false; // let the proposal card's buttons take Enter / ← / →
            } else if (e == Event::Return || e == Event::Character(" ")) {
                tv.toggle_selected();
            }
            return true;
        }

        // Reverse-i-search (Ctrl-R) mode captures all keys.
        if (rsearch) {
            const auto& items = history.items();
            if (e == Event::Return) {
                rsearch = false;
                cursor_pos = static_cast<int>(input_text.size());
            } else if (e == Event::Escape || e.input() == "\x07") { // Esc / Ctrl-G cancel
                input_text = rsearch_draft;
                rsearch = false;
                cursor_pos = static_cast<int>(input_text.size());
            } else if (e.input() == "\x12") { // Ctrl-R: next older match
                int from = rmatch <= 0 ? -1 : rmatch - 1;
                int next = history.search_backward(rquery, from);
                if (next >= 0) {
                    rmatch = next;
                    input_text = items[static_cast<size_t>(rmatch)];
                }
            } else if (e == Event::Backspace) {
                if (!rquery.empty()) {
                    rquery.pop_back();
                }
                rmatch = history.search_backward(rquery, static_cast<int>(items.size()) - 1);
                input_text = rmatch >= 0 ? items[static_cast<size_t>(rmatch)] : std::string{};
            } else if (e.is_character()) {
                rquery += e.character();
                rmatch = history.search_backward(rquery, static_cast<int>(items.size()) - 1);
                if (rmatch >= 0) {
                    input_text = items[static_cast<size_t>(rmatch)];
                }
            } else {
                rsearch = false; // any other key accepts the current match and exits
                cursor_pos = static_cast<int>(input_text.size());
            }
            return true;
        }

        // Input focus.

        // Bracketed paste: a pasted block is wrapped in ESC[200~ … ESC[201~, so we
        // can tell pasted newlines (literal) from a typed Enter (submit). FTXUI
        // delivers the markers as Special events.
        if (e.input() == "\x1b[200~") {
            paste_mode = true;
            return true;
        }
        if (e.input() == "\x1b[201~") {
            paste_mode = false;
            return true;
        }
        if (paste_mode) {
            if (e == Event::Return) {
                input_text.insert(static_cast<size_t>(std::clamp(cursor_pos, 0,
                                                                 static_cast<int>(input_text.size()))),
                                  "\n");
                ++cursor_pos;
                return true;
            }
            return false; // pasted characters flow to the Input
        }

        auto candidates = astrite::tui::complete(input_text);
        if (!candidates.empty()) {
            const bool sel_ok = popup_sel >= 0 && popup_sel < static_cast<int>(candidates.size());
            if (e == Event::Tab) {
                if (sel_ok) {
                    input_text = candidates[static_cast<size_t>(popup_sel)]->name + " ";
                    cursor_pos = static_cast<int>(input_text.size()); // cursor follows to the end
                    popup_sel = 0;
                }
                return true;
            }
            if (e == Event::Return) {
                // Enter accepts the highlighted command (and runs it), rather than
                // submitting the partially-typed text.
                if (sel_ok) {
                    std::string name = candidates[static_cast<size_t>(popup_sel)]->name;
                    history.push(name);
                    input_text.clear();
                    cursor_pos = 0;
                    popup_sel = 0;
                    run_command(name);
                }
                return true;
            }
            if (e == Event::ArrowDown) {
                popup_sel = std::min(popup_sel + 1, static_cast<int>(candidates.size()) - 1);
                return true;
            }
            if (e == Event::ArrowUp) {
                popup_sel = std::max(popup_sel - 1, 0);
                return true;
            }
        } else if (e == Event::Tab && tv.count() > 0) {
            body_sel = 0;                // focus the transcript
            tv.select(tv.count() - 1);   // start at the newest segment
            return true;
        }

        // History recall (no completion popup active).
        if (e == Event::ArrowUp) {
            input_text = history.up(input_text);
            cursor_pos = static_cast<int>(input_text.size());
            return true;
        }
        if (e == Event::ArrowDown) {
            input_text = history.down(input_text);
            cursor_pos = static_cast<int>(input_text.size());
            return true;
        }

        // Readline-style editing (Ctrl + Alt).
        if (e.input() == "\x12") { // Ctrl-R: enter reverse search
            rsearch = true;
            rquery.clear();
            rmatch = -1;
            rsearch_draft = input_text;
            return true;
        }
        {
            int n = static_cast<int>(input_text.size());
            cursor_pos = std::clamp(cursor_pos, 0, n);
            if (e.input() == "\x01") { // Ctrl-A: line start
                cursor_pos = 0;
                return true;
            }
            if (e.input() == "\x05") { // Ctrl-E: line end
                cursor_pos = n;
                return true;
            }
            if (e.input() == "\x15") { // Ctrl-U: kill to start
                input_text.erase(0, static_cast<size_t>(cursor_pos));
                cursor_pos = 0;
                return true;
            }
            if (e.input() == "\x0b") { // Ctrl-K: kill to end
                input_text.erase(static_cast<size_t>(cursor_pos));
                return true;
            }
            if (e.input() == "\x17" || e.input() == "\x1b\x7f") { // Ctrl-W / Alt-Backspace
                int b = word_back(input_text, cursor_pos);
                input_text.erase(static_cast<size_t>(b), static_cast<size_t>(cursor_pos - b));
                cursor_pos = b;
                return true;
            }
            if (e.input() == "\x1b" "b") { // Alt-b: word back
                cursor_pos = word_back(input_text, cursor_pos);
                return true;
            }
            if (e.input() == "\x1b" "f") { // Alt-f: word forward
                cursor_pos = word_fwd(input_text, cursor_pos);
                return true;
            }
            if (e.input() == "\x1b" "d") { // Alt-d: kill word forward
                int f = word_fwd(input_text, cursor_pos);
                input_text.erase(static_cast<size_t>(cursor_pos),
                                 static_cast<size_t>(f - cursor_pos));
                return true;
            }
        }

        // Alt+Enter / Shift+Enter insert a newline (multi-line composition);
        // a plain Enter submits. (Enter is intercepted here so the multiline Input
        // doesn't also insert a newline.)
        if (e.input() == "\x1b\r" || e.input() == "\x1b\n" || e.input() == "\x1b[13;2u") {
            int at = std::clamp(cursor_pos, 0, static_cast<int>(input_text.size()));
            input_text.insert(static_cast<size_t>(at), "\n");
            cursor_pos = at + 1;
            return true;
        }
        if (e == Event::Return) {
            do_submit();
            return true;
        }

        if (e == Event::Escape) {
            if (turn.has_value() && turn_active()) {
                turn->request_stop();
            }
            return true;
        }
        if (e == Event::PageUp) {
            following = false;
            scroll = std::max(0.0f, scroll - 0.25f);
            return true;
        }
        if (e == Event::PageDown) {
            scroll = std::min(1.0f, scroll + 0.25f);
            following = scroll >= 0.999f;
            return true;
        }
        if (e.is_mouse()) {
            if (e.mouse().button == Mouse::WheelUp) {
                following = false;
                scroll = std::max(0.0f, scroll - 0.1f);
                return true;
            }
            if (e.mouse().button == Mouse::WheelDown) {
                scroll = std::min(1.0f, scroll + 0.1f);
                following = scroll >= 0.999f;
                return true;
            }
            // Other mouse events fall through to the components: clicking a segment
            // focuses it (the focusable Renderer takes focus, which sets body_sel and
            // the selected segment); dragging is FTXUI's built-in text selection.
        }
        return false;
    });

    // /config as a real FTXUI Modal: checkboxes bound to the config fields get
    // keyboard + mouse for free; Modal routes events to it (and applies
    // clear_under) while config_shown, and back to main_view otherwise. Changes are
    // persisted to .astrite/config.json when the modal closes.
    Component cb_markdown = Checkbox("Render markdown", &cfg.markdown);
    Component cb_index = Checkbox("Always show segment indices", &cfg.show_index);
    Component cb_collapse_tools = Checkbox("Collapse tool calls by default", &cfg.collapse_tool_calls);
    Component cb_collapse_reasoning =
        Checkbox("Collapse reasoning traces by default", &cfg.collapse_reasoning);
    Component cb_show_reasoning =
        Checkbox("Show reasoning traces (restart to apply)", &cfg.show_reasoning);
    Component config_fields = Container::Vertical(
        {cb_markdown, cb_index, cb_collapse_tools, cb_collapse_reasoning, cb_show_reasoning});
    Component config_panel_view = Renderer(config_fields, [&] {
        return config_panel(provider, model, base_url, workspace, policy, session_path,
                            config_fields->Render());
    });
    Component config_modal = CatchEvent(config_panel_view, [&](Event e) {
        if (e == Event::Escape || e == Event::Character("q")) {
            config_shown = false;
            (void)astrite::tui::save_config(config_path, cfg); // persist the human's choices
            return true;
        }
        return false; // arrows / space / mouse → the checkboxes
    });
    Component root = Modal(main_view, config_modal, &config_shown);

    // Enable bracketed paste at the terminal. FTXUI doesn't manage mode 2004, so
    // this survives its alt-screen/mouse setup; the ESC[200~ / ESC[201~ markers
    // then arrive as Special events the handler brackets. Disable it on exit.
    std::fputs("\x1b[?2004h", stdout);
    std::fflush(stdout);
    screen.Loop(root);
    std::fputs("\x1b[?2004l", stdout);
    std::fflush(stdout);
    runtime.events().close();
    if (turn.has_value() && turn->joinable()) {
        turn->request_stop();
    }
    return 0;
}
