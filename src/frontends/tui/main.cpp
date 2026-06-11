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
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include "frontends/common.hpp"
#include "frontends/tui/commands.hpp"
#include "frontends/tui/history.hpp"
#include "frontends/tui/markdown_render.hpp"
#include "frontends/tui/transcript.hpp"
#include "frontends/tui/view_state.hpp"
#include "frontends/tui/widgets.hpp"
#include "runtime/runtime.hpp"
#include "sandbox/sandbox.hpp"

namespace {

using namespace ftxui;
namespace at = astrite::tui; // FTXUI also defines `Focus`/`Modal`; keep ours qualified
using astrite::tui::Segment;
using astrite::tui::ViewState;

std::string first_line(const std::string& s) {
    std::size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

std::string clip_lines(const std::string& s, std::size_t max_lines) {
    std::size_t lines = 0, pos = 0;
    while (pos != std::string::npos && lines < max_lines) {
        pos = s.find('\n', pos);
        if (pos != std::string::npos) {
            ++pos;
            ++lines;
        }
    }
    if (pos == std::string::npos) {
        return s;
    }
    std::size_t remaining = 0;
    for (std::size_t p = pos; p < s.size(); ++p) {
        if (s[p] == '\n') {
            ++remaining;
        }
    }
    return s.substr(0, pos) + "… (+" + std::to_string(remaining + 1) + " more lines)";
}

Element segment_body(const Segment& s, bool collapsed, bool markdown) {
    switch (s.kind) {
    case Segment::Kind::user:
        return hbox({text("› ") | bold, paragraph(s.text)}) | color(Color::CyanLight);
    case Segment::Kind::assistant:
        if (collapsed) {
            return paragraph(first_line(s.text) + " …") | color(Color::GrayLight);
        }
        // Settled messages render as styled markdown (when enabled); the streaming
        // partial stays raw (no half-parsed flicker) with a cursor.
        if (!s.done) {
            return paragraph(s.text + " ▌");
        }
        return markdown ? astrite::tui::render_markdown(s.text) : paragraph(s.text);
    case Segment::Kind::tool: {
        std::string head = std::string(collapsed ? "▸ " : "▾ ") + "⚙ " + s.tool_name;
        if (!s.tool_args.empty() && s.tool_args != "{}") {
            head += " " + first_line(s.tool_args);
        }
        if (collapsed) {
            return text(head) | color(Color::YellowLight);
        }
        Elements rows{text(head) | color(Color::YellowLight)};
        rows.push_back(s.done ? paragraph(clip_lines(s.tool_result, 14)) | color(Color::GrayLight)
                              : text("running…") | dim);
        return vbox(std::move(rows)) | borderLight | color(Color::GrayDark);
    }
    case Segment::Kind::error:
        return hbox({text("✗ ") | bold, paragraph(s.text)}) | color(Color::RedLight) | borderLight;
    }
    return text("");
}

Element render_segment(const Segment& s, int idx, bool selected, bool collapsed, bool show_index,
                       bool markdown) {
    Element body = segment_body(s, collapsed, markdown);
    Element accent = selected ? (text("┃ ") | color(Color::Cyan)) : text("  ");
    Element row = hbox({accent, body | flex});
    if (show_index) {
        std::string label = (idx < 10 ? " " : "") + std::to_string(idx);
        Element gutter = text(label + " ") | (selected ? color(Color::Cyan) : dim);
        row = hbox({gutter, row | flex});
    }
    return row;
}

Element config_modal_content(const std::string& model, const std::string& base_url,
                             const std::string& workspace, const std::string& policy,
                             const std::string& session, bool pref_markdown, bool pref_index,
                             int sel) {
    auto info = [](std::string k, std::string v) {
        return hbox({text(k) | dim | size(WIDTH, EQUAL, 14), text(std::move(v))});
    };
    auto toggle = [&](int idx, std::string label, bool on) {
        Element r = hbox({text(on ? "[x] " : "[ ] ") | color(on ? Color::GreenLight : Color::GrayDark),
                          text(std::move(label))});
        return idx == sel ? r | inverted : r;
    };
    return vbox({
        text("Runtime") | bold | color(Color::YellowLight),
        info("model", model),
        info("base url", base_url),
        info("workspace", workspace),
        info("authority", policy),
        info("session", session),
        text(""),
        text("Preferences  (↑/↓ select · enter toggle)") | bold | color(Color::YellowLight),
        toggle(0, "Render markdown", pref_markdown),
        toggle(1, "Always show segment indices", pref_index),
        text(""),
        paragraph("Persistent, file-backed config (a profile component) is planned for M10; "
                  "these toggles apply to the current session.") |
            dim,
    });
}

Element commands_modal_content() {
    Elements rows;
    for (const astrite::tui::SlashCommand& c : astrite::tui::all_commands()) {
        rows.push_back(hbox({
            text(c.name) | color(Color::CyanLight) | size(WIDTH, EQUAL, 14),
            text(c.summary) | color(Color::GrayLight),
        }));
    }
    return vbox(std::move(rows));
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

Element status_modal_content(const std::string& model, const std::string& workspace,
                             const astrite::tui::Transcript& t, bool active) {
    int tools = 0, turns = 0;
    for (const Segment& s : t.segments()) {
        if (s.kind == Segment::Kind::tool) {
            ++tools;
        } else if (s.kind == Segment::Kind::user) {
            ++turns;
        }
    }
    auto row = [](std::string k, std::string v) {
        return hbox({text(k) | dim | size(WIDTH, EQUAL, 14), text(std::move(v))});
    };
    return vbox({
        row("model", model),
        row("workspace", workspace),
        row("segments", std::to_string(t.segments().size())),
        row("user turns", std::to_string(turns)),
        row("tool calls", std::to_string(tools)),
        row("state", active ? "working" : "ready"),
    });
}

} // namespace

int main(int argc, char** argv) {
    if (int rc = astrite::sandbox::maybe_run_helper(argc, argv); rc >= 0) {
        return rc;
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
    auto rt = astrite::runtime::Runtime::create(std::move(opts));
    if (!rt.has_value()) {
        std::fprintf(stderr, "astrite-tui: %s\n", rt.error().c_str());
        return 1;
    }
    astrite::runtime::Runtime& runtime = **rt;

    const char* env_model = std::getenv("OPENAI_MODEL");
    const std::string model = env_model != nullptr ? env_model : "gpt-5.2";
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
    int config_sel = 0;
    bool pref_markdown = true;
    bool pref_index = false;
    int cursor_pos = 0;               // shared with the Input widget (readline editing)
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

    const char* env_base = std::getenv("OPENAI_BASE_URL");
    const std::string base_url = env_base != nullptr ? env_base : "https://api.openai.com/v1";
    const char* env_policy = std::getenv("ASTRITE_AUTHORITY_POLICY");
    const std::string policy = env_policy != nullptr ? env_policy : "review";
    const std::string session_path = ".astrite/sessions/current.jsonl";

    auto seg_count = [&] { return static_cast<int>(transcript.segments().size()); };
    auto turn_active = [&] { return transcript.turn_active(); };

    // Pump runtime events onto the UI thread as Closures (Post is thread-safe and
    // wakes the loop), so transcript mutation happens on the UI thread — no mutex.
    std::jthread pump([&] {
        while (auto event = runtime.events().pop()) {
            screen.Post([&transcript, ev = std::move(*event)] { transcript.apply(ev); });
        }
    });

    auto run_command = [&](const std::string& line) {
        const astrite::tui::SlashCommand* cmd = astrite::tui::lookup(line);
        if (cmd == nullptr) {
            transcript.apply(astrite::runtime::TurnError{"unknown command: " + first_line(line)});
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
            view_state.open_modal(at::Modal::config);
            config_sel = 0;
        } else if (n == "/clear") {
            if (!turn_active()) {
                transcript.clear();
                view_state.focus_input();
            }
        } else if (n == "/focus") {
            view_state.toggle_focus(seg_count());
        } else if (n == "/collapse") {
            view_state.collapse_all(seg_count());
        } else if (n == "/expand") {
            view_state.expand_all();
        } else if (n == "/top") {
            following = false;
            scroll = 0.0f;
        } else if (n == "/bottom") {
            following = true;
            scroll = 1.0f;
        }
    };

    InputOption input_opt;
    input_opt.placeholder = "message astrite++  ·  / for commands  ·  Tab for transcript";
    input_opt.multiline = false;
    input_opt.cursor_position = &cursor_pos;
    // No inverted/white-background field; just dim the placeholder. The terminal
    // cursor is drawn by FTXUI independently of this.
    input_opt.transform = [](InputState state) {
        if (state.is_placeholder) {
            state.element |= dim;
        }
        return state.element;
    };
    input_opt.on_change = [&] { history.reset(); }; // typing exits history navigation
    input_opt.on_enter = [&] {
        std::string text = input_text;
        if (text.empty()) {
            return;
        }
        history.push(text);
        if (!text.empty() && text.front() == '/') {
            run_command(text);
            input_text.clear();
            return;
        }
        if (turn_active()) {
            return;
        }
        {
            transcript.user(text);
        }
        input_text.clear();
        scroll = 1.0f;
        following = true;
        if (turn.has_value() && turn->joinable()) {
            turn->join();
        }
        turn.emplace(
            [&runtime, text](std::stop_token st) { runtime.submit_user_message(text, st); });
    };
    Component input = Input(&input_text, input_opt);

    Component view = Renderer(input, [&] {
        Elements lines;
        bool active = false;
        int count = 0;
        const bool transcript_focus = view_state.focus() == at::Focus::transcript;
        {
            active = transcript.turn_active();
            const auto& segs = transcript.segments();
            count = static_cast<int>(segs.size());
            for (int i = 0; i < count; ++i) {
                const Segment& seg = segs[static_cast<size_t>(i)];
                lines.push_back(render_segment(seg, i,
                                               transcript_focus && view_state.selected() == i,
                                               view_state.is_collapsed(i),
                                               transcript_focus || pref_index, pref_markdown));
            }
        }
        if (lines.empty()) {
            lines.push_back(text("Ask astrite++ to read, search, or edit the workspace.") | dim);
        }

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
        Element header = hbox({
            text(" astrite++ ") | bold | bgcolor(Color::Blue) | color(Color::White),
            text("  " + model + "  ") | color(Color::GrayLight),
            text("· " + workspace + " ") | dim,
            filler(),
            transcript_focus ? text("[transcript] ") | color(Color::CyanLight) : text(""),
            status,
            text(" "),
        });

        Element body = vbox(std::move(lines)) | focusPositionRelative(0.0f, scroll) |
                       vscroll_indicator | yframe | flex;

        Elements stack{header, separator(), body, separator()};

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

        std::string hints = rsearch ? " type to search · ctrl-r older · enter accept · esc cancel "
                            : transcript_focus
                                ? " ↑/↓ j/k select · enter collapse · tab input · /help "
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
                content = status_modal_content(model, workspace, transcript, active);
                break;
            }
            case at::Modal::config:
                title = "config";
                content = config_modal_content(model, base_url, workspace, policy, session_path,
                                               pref_markdown, pref_index, config_sel);
                break;
            case at::Modal::none:
                break;
            }
            const bool is_config = view_state.modal() == at::Modal::config;
            root_el = at::modal_overlay(root_el, title, content, modal_scroll,
                                    is_config ? "↑/↓ select · enter toggle · esc close"
                                              : "↑/↓ j/k pgup/pgdn ctrl-u/ctrl-d scroll · esc close");
        }
        return root_el;
    });

    Component root = CatchEvent(view, [&](Event e) {
        // Modal open: scroll + close, swallow the rest.
        // The config modal is an interactive list (select + toggle), not a scroller.
        if (view_state.modal() == at::Modal::config) {
            if (e == Event::Escape || e == Event::Character("q")) {
                view_state.close_modal();
            } else if (e == Event::ArrowUp || e == Event::Character("k")) {
                config_sel = std::max(0, config_sel - 1);
            } else if (e == Event::ArrowDown || e == Event::Character("j")) {
                config_sel = std::min(1, config_sel + 1);
            } else if (e == Event::Return || e == Event::Character(" ")) {
                if (config_sel == 0) {
                    pref_markdown = !pref_markdown;
                } else {
                    pref_index = !pref_index;
                }
            }
            return true;
        }
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

        // Transcript focus: segment navigation + collapse.
        if (view_state.focus() == at::Focus::transcript) {
            int count = seg_count();
            constexpr int kHalfPage = 10;
            if (e == Event::Tab || e == Event::Escape) {
                view_state.focus_input();
            } else if (e == Event::ArrowUp || e == Event::Character("k")) {
                view_state.select_prev(count);
            } else if (e == Event::ArrowDown || e == Event::Character("j")) {
                view_state.select_next(count);
            } else if (e.input() == "\x15") { // Ctrl-U
                view_state.select_relative(-kHalfPage, count);
            } else if (e.input() == "\x04") { // Ctrl-D
                view_state.select_relative(kHalfPage, count);
            } else if (e == Event::Home || e == Event::Character("g")) {
                view_state.select_first(count);
            } else if (e == Event::End || e == Event::Character("G")) {
                view_state.select_last(count);
            } else if (e == Event::Return || e == Event::Character(" ")) {
                view_state.toggle_collapse_selected();
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
        auto candidates = astrite::tui::complete(input_text);
        if (!candidates.empty()) {
            if (e == Event::Tab) {
                if (popup_sel >= 0 && popup_sel < static_cast<int>(candidates.size())) {
                    input_text = candidates[static_cast<size_t>(popup_sel)]->name + " ";
                    popup_sel = 0;
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
        } else if (e == Event::Tab) {
            view_state.focus_transcript(seg_count());
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
        }
        return false;
    });

    screen.Loop(root);
    runtime.events().close();
    if (turn.has_value() && turn->joinable()) {
        turn->request_stop();
    }
    return 0;
}
