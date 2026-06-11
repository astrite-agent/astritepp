// The FTXUI frontend. Transcript state is an event-folded, unit-tested pure
// model (transcript.hpp); this file is the interactive shell around it: a header
// with a live spinner, a scrollable styled transcript (mouse wheel + keys,
// follow-tail that releases on manual scroll), bordered tool cards, Escape to
// cancel an in-flight turn, and a keybinding footer.
// NOTE(M8): rendering here is the pre-UiTree scaffold — the deeper M8 work
// replaces the per-segment drawing below with the UiTree interpreter. The shell
// (scroll, spinner, keybindings, cancel) is durable across that migration.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include "frontends/common.hpp"
#include "frontends/tui/transcript.hpp"
#include "runtime/runtime.hpp"
#include "sandbox/sandbox.hpp"

namespace {

using namespace ftxui;
using astrite::tui::Segment;

// Truncate a long tool result for display (full result stays in the session log).
std::string clip_lines(const std::string& s, std::size_t max_lines) {
    std::size_t lines = 0;
    std::size_t pos = 0;
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

Element render_segment(const Segment& s) {
    switch (s.kind) {
    case Segment::Kind::user:
        return hbox({text("› ") | bold, paragraph(s.text)}) | color(Color::CyanLight);
    case Segment::Kind::assistant:
        return paragraph(s.done ? s.text : s.text + " ▌");
    case Segment::Kind::tool: {
        Elements rows;
        std::string head = "⚙ " + s.tool_name;
        if (!s.tool_args.empty() && s.tool_args != "{}") {
            head += " " + clip_lines(s.tool_args, 1);
        }
        rows.push_back(text(head) | color(Color::YellowLight));
        if (s.done) {
            rows.push_back(paragraph(clip_lines(s.tool_result, 12)) | color(Color::GrayLight));
        } else {
            rows.push_back(text("running…") | dim);
        }
        return vbox(std::move(rows)) | borderLight | color(Color::GrayDark);
    }
    case Segment::Kind::error:
        return hbox({text("✗ ") | bold, paragraph(s.text)}) | color(Color::RedLight) | borderLight;
    }
    return text("");
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
    std::string input_text;
    std::optional<std::jthread> turn;
    std::atomic<int> frame{0};
    float scroll = 1.0f;   // 0 = top, 1 = bottom
    bool following = true; // stick to the tail until the user scrolls up

    auto turn_active = [&] {
        std::lock_guard lock(mu);
        return transcript.turn_active();
    };

    // Event pump: fold runtime events into the transcript and repaint.
    std::jthread pump([&] {
        while (auto event = runtime.events().pop()) {
            {
                std::lock_guard lock(mu);
                transcript.apply(*event);
            }
            screen.PostEvent(Event::Custom);
        }
    });

    // Ticker: animate the spinner while a turn runs (FTXUI only repaints on events).
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

    InputOption input_opt;
    input_opt.placeholder = "message astrite++  ·  /quit to exit";
    input_opt.multiline = false;
    input_opt.on_enter = [&] {
        std::string text = input_text;
        if (text.empty()) {
            return;
        }
        if (text == "/quit" || text == "/exit") {
            screen.Exit();
            return;
        }
        if (turn_active()) {
            return; // one turn at a time
        }
        {
            std::lock_guard lock(mu);
            transcript.user(text);
        }
        input_text.clear();
        scroll = 1.0f;
        following = true;
        if (turn.has_value() && turn->joinable()) {
            turn->join(); // the prior turn already reached its terminal event
        }
        turn.emplace([&runtime, text](std::stop_token st) {
            runtime.submit_user_message(text, st);
        });
    };
    Component input = Input(&input_text, input_opt);

    Component view = Renderer(input, [&] {
        Elements lines;
        bool active = false;
        {
            std::lock_guard lock(mu);
            active = transcript.turn_active();
            for (const Segment& s : transcript.segments()) {
                lines.push_back(render_segment(s));
            }
        }
        if (lines.empty()) {
            lines.push_back(text("Ask astrite++ to read, search, or edit the workspace.") | dim);
        }

        Element status =
            active ? hbox({spinner(5, static_cast<size_t>(frame.load(std::memory_order_relaxed))),
                           text(" working") | color(Color::YellowLight),
                           text("  esc to cancel") | dim})
                   : text("ready") | color(Color::GreenLight);

        Element header = hbox({
                             text(" astrite++ ") | bold | bgcolor(Color::Blue) |
                                 color(Color::White),
                             text("  " + model + "  ") | color(Color::GrayLight),
                             text("· " + workspace + " ") | dim,
                             filler(),
                             status,
                             text(" "),
                         });

        Element body = vbox(std::move(lines)) | focusPositionRelative(0.0f, scroll) |
                       vscroll_indicator | yframe | flex;

        std::string hints = " enter send · esc cancel · pgup/pgdn · home/end · /quit ";
        Element footer = hbox({
            text(hints) | dim,
            filler(),
            following ? text("") : text("↓ more (End) ") | color(Color::YellowLight),
        });

        return vbox({
            header,
            separator(),
            body,
            separator(),
            hbox({text(active ? "~ " : "› ") | bold, input->Render() | flex}),
            footer,
        });
    });

    Component root = CatchEvent(view, [&](Event e) {
        if (e == Event::Escape) {
            if (turn.has_value() && turn_active()) {
                turn->request_stop(); // cancel the in-flight model stream
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
        if (e == Event::Home) {
            following = false;
            scroll = 0.0f;
            return true;
        }
        if (e == Event::End) {
            following = true;
            scroll = 1.0f;
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
        return false; // everything else (typing, Enter) goes to the input
    });

    screen.Loop(root);
    runtime.events().close();
    if (turn.has_value() && turn->joinable()) {
        turn->request_stop(); // unblock a still-streaming turn on exit
    }
    return 0;
}
