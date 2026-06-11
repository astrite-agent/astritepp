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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
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
#include "frontends/tui/transcript.hpp"
#include "frontends/tui/view_state.hpp"
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

Element segment_body(const Segment& s, bool collapsed) {
    switch (s.kind) {
    case Segment::Kind::user:
        return hbox({text("› ") | bold, paragraph(s.text)}) | color(Color::CyanLight);
    case Segment::Kind::assistant:
        if (collapsed) {
            return paragraph(first_line(s.text) + " …") | color(Color::GrayLight);
        }
        return paragraph(s.done ? s.text : s.text + " ▌");
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

Element render_segment(const Segment& s, int idx, bool selected, bool collapsed, bool show_index) {
    Element body = segment_body(s, collapsed);
    Element accent = selected ? (text("┃ ") | color(Color::Cyan)) : text("  ");
    Element row = hbox({accent, body | flex});
    if (show_index) {
        std::string label = (idx < 10 ? " " : "") + std::to_string(idx);
        Element gutter = text(label + " ") | (selected ? color(Color::Cyan) : dim);
        row = hbox({gutter, row | flex});
    }
    return row;
}

// A scrollable modal panel (dimmed background underneath, centered box on top).
Element modal_overlay(Element base, const std::string& title, Element content, float scroll) {
    Element panel =
        vbox({
            text(" " + title + " ") | bold | color(Color::White) | bgcolor(Color::Blue),
            separator(),
            content | focusPositionRelative(0.0f, scroll) | yframe | flex,
            separator(),
            text("↑/↓ pgup/pgdn scroll · esc close") | dim | hcenter,
        }) |
        border | bgcolor(Color::Black) | size(WIDTH, LESS_THAN, 92) | size(HEIGHT, LESS_THAN, 32);
    return dbox({base | dim, panel | center});
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

Element help_modal_content() {
    auto line = [](std::string s) { return paragraph(std::move(s)); };
    return vbox({
        text("astrite++") | bold,
        line("A secure coding agent whose behaviour is declarative component data "
             "interpreted by a small kernel. Every effect is capability-gated and runs "
             "confined by an OS sandbox."),
        text(""),
        text("Input") | bold | color(Color::YellowLight),
        line("Type a message and press Enter. Start with '/' for a command — a completion "
             "popup appears; Tab fills in the highlighted command."),
        text(""),
        text("Navigation") | bold | color(Color::YellowLight),
        line("Tab switches focus between the input and the transcript. In the transcript, "
             "↑/↓ select a segment, Enter/Space collapse or expand it, Home/End jump to "
             "ends. PageUp/PageDown and the mouse wheel scroll any time."),
        text(""),
        text("Turns") | bold | color(Color::YellowLight),
        line("Esc cancels an in-flight turn. The spinner in the header shows when the "
             "agent is working."),
    });
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
    std::mutex mu;
    astrite::tui::Transcript transcript;
    ViewState view_state;
    std::string input_text;
    std::optional<std::jthread> turn;
    std::atomic<int> frame{0};
    float scroll = 1.0f;
    bool following = true;
    int popup_sel = 0;
    float modal_scroll = 0.0f;

    auto seg_count = [&] {
        std::lock_guard lock(mu);
        return static_cast<int>(transcript.segments().size());
    };
    auto turn_active = [&] {
        std::lock_guard lock(mu);
        return transcript.turn_active();
    };

    std::jthread pump([&] {
        while (auto event = runtime.events().pop()) {
            {
                std::lock_guard lock(mu);
                transcript.apply(*event);
            }
            screen.PostEvent(Event::Custom);
        }
    });
    std::jthread ticker([&](std::stop_token st) {
        using namespace std::chrono_literals;
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(120ms);
            if (turn_active()) {
                frame.fetch_add(1, std::memory_order_relaxed);
                screen.PostEvent(Event::Custom);
            }
        }
    });

    auto run_command = [&](const std::string& line) {
        const astrite::tui::SlashCommand* cmd = astrite::tui::lookup(line);
        if (cmd == nullptr) {
            std::lock_guard lock(mu);
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
        } else if (n == "/clear") {
            if (!turn_active()) {
                std::lock_guard lock(mu);
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
    input_opt.on_enter = [&] {
        std::string text = input_text;
        if (text.empty()) {
            return;
        }
        if (!text.empty() && text.front() == '/') {
            run_command(text);
            input_text.clear();
            return;
        }
        if (turn_active()) {
            return;
        }
        {
            std::lock_guard lock(mu);
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
            std::lock_guard lock(mu);
            active = transcript.turn_active();
            const auto& segs = transcript.segments();
            count = static_cast<int>(segs.size());
            for (int i = 0; i < count; ++i) {
                const Segment& seg = segs[static_cast<size_t>(i)];
                lines.push_back(render_segment(seg, i,
                                               transcript_focus && view_state.selected() == i,
                                               view_state.is_collapsed(i), transcript_focus));
            }
        }
        if (lines.empty()) {
            lines.push_back(text("Ask astrite++ to read, search, or edit the workspace.") | dim);
        }

        Element status =
            active ? hbox({spinner(5, static_cast<size_t>(frame.load(std::memory_order_relaxed))),
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

        stack.push_back(hbox({text(active ? "~ " : "› ") | bold, input->Render() | flex}));

        std::string hints = transcript_focus
                                ? " ↑/↓ select · enter collapse · tab input · /help "
                                : " enter send · tab transcript · esc cancel · /help ";
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
                std::lock_guard lock(mu);
                title = "status";
                content = status_modal_content(model, workspace, transcript, active);
                break;
            }
            case at::Modal::none:
                break;
            }
            root_el = modal_overlay(root_el, title, content, modal_scroll);
        }
        return root_el;
    });

    Component root = CatchEvent(view, [&](Event e) {
        // Modal open: scroll + close, swallow the rest.
        if (view_state.modal_open()) {
            if (e == Event::Escape || e == Event::Return || e == Event::Character("q")) {
                view_state.close_modal();
            } else if (e == Event::ArrowDown || e == Event::PageDown) {
                modal_scroll = std::min(1.0f, modal_scroll + 0.12f);
            } else if (e == Event::ArrowUp || e == Event::PageUp) {
                modal_scroll = std::max(0.0f, modal_scroll - 0.12f);
            } else if (e == Event::Home) {
                modal_scroll = 0.0f;
            } else if (e == Event::End) {
                modal_scroll = 1.0f;
            }
            return true;
        }

        // Transcript focus: segment navigation + collapse.
        if (view_state.focus() == at::Focus::transcript) {
            int count = seg_count();
            if (e == Event::Tab || e == Event::Escape) {
                view_state.focus_input();
            } else if (e == Event::ArrowUp) {
                view_state.select_prev(count);
            } else if (e == Event::ArrowDown) {
                view_state.select_next(count);
            } else if (e == Event::Home) {
                view_state.select_first(count);
            } else if (e == Event::End) {
                view_state.select_last(count);
            } else if (e == Event::Return || e == Event::Character(" ")) {
                view_state.toggle_collapse_selected();
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
