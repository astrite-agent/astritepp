// The FTXUI frontend scaffold. Transcript state is event-folded and unit-tested
// (transcript.hpp); this file is wiring: an input box, a follow-tail transcript,
// a worker thread per turn, and an event pump that repaints on runtime events.
// NOTE(M8): rendering here is a pre-UiTree scaffold — M8 replaces the per-kind
// drawing below with the UiTree interpreter; no business logic may grow here.

#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "frontends/common.hpp"
#include "frontends/tui/transcript.hpp"
#include "runtime/runtime.hpp"
#include "sandbox/sandbox.hpp"

namespace {

ftxui::Element render_segment(const astrite::tui::Segment& s) {
    using namespace ftxui;
    using Kind = astrite::tui::Segment::Kind;
    switch (s.kind) {
    case Kind::user:
        return hbox({text("> ") | bold, paragraph(s.text)}) | color(Color::Cyan);
    case Kind::assistant:
        return paragraph(s.done ? s.text : s.text + " ▌");
    case Kind::tool: {
        Elements rows;
        rows.push_back(text("[tool " + s.tool_name + " " + s.tool_args + "]") | dim);
        if (s.done) {
            rows.push_back(text("  -> " + s.tool_result) | dim);
        }
        return vbox(std::move(rows));
    }
    case Kind::error:
        return paragraph("error: " + s.text) | color(Color::Red);
    }
    return text("");
}

} // namespace

int main(int argc, char** argv) {
    using namespace ftxui;

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

    ScreenInteractive screen = ScreenInteractive::Fullscreen();
    std::mutex mu;
    astrite::tui::Transcript transcript;
    std::string input_text;
    std::optional<std::jthread> turn;

    // Event pump: folds runtime events into the transcript and repaints.
    // events().close() after the loop unblocks the final pop().
    std::jthread pump([&] {
        while (auto event = runtime.events().pop()) {
            {
                std::lock_guard lock(mu);
                transcript.apply(*event);
            }
            screen.PostEvent(Event::Custom);
        }
    });

    InputOption input_opt;
    input_opt.placeholder = "message astrite++ — /quit to exit";
    input_opt.multiline = false; // Enter submits (v6 defaults to multiline)
    input_opt.on_enter = [&] {
        std::string text = input_text;
        if (text.empty()) {
            return;
        }
        if (text == "/quit" || text == "/exit") {
            screen.Exit();
            return;
        }
        {
            std::lock_guard lock(mu);
            if (transcript.turn_active()) {
                return; // one turn at a time
            }
            transcript.user(text);
        }
        input_text.clear();
        if (turn.has_value() && turn->joinable()) {
            turn->join(); // prior turn already saw its terminal event
        }
        turn.emplace([&runtime, text] { runtime.submit_user_message(text); });
    };
    Component input = Input(&input_text, input_opt);

    Component view = Renderer(input, [&] {
        Elements lines;
        bool active = false;
        {
            std::lock_guard lock(mu);
            active = transcript.turn_active();
            for (const astrite::tui::Segment& s : transcript.segments()) {
                lines.push_back(render_segment(s));
                lines.push_back(text(""));
            }
        }
        if (!lines.empty()) {
            lines.back() |= focus; // follow-tail: keep the newest output in frame
        }
        return vbox({
            vbox(std::move(lines)) | vscroll_indicator | yframe | flex,
            separator(),
            hbox({text(active ? "~ " : "> ") | bold, input->Render() | flex}),
        });
    });

    screen.Loop(view);
    runtime.events().close();
    return 0;
    // jthread destructors join: `turn` may wait out an in-flight model call
    // (stop_token-based stream cancellation is M8 polish).
}
