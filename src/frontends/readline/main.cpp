// The readline frontend: the end-to-end proof vehicle, not a throwaway. It reads
// lines, runs each turn on a worker thread, and renders the runtime event stream
// — streamed deltas live, tool activity inline. All agent behavior lives in the
// components; this file is plumbing.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include "frontends/common.hpp"
#include "runtime/runtime.hpp"
#include "sandbox/sandbox.hpp"
#include "util/overloaded.hpp"

namespace fs = std::filesystem;
using namespace astrite;

int main(int argc, char** argv) {
    // First: serve a self-re-exec sandbox helper invocation, if that's what we are.
    if (int rc = sandbox::maybe_run_helper(argc, argv); rc >= 0) {
        return rc;
    }

    auto components = frontends::load_components();
    if (!components.has_value()) {
        std::fprintf(stderr, "astrite: component load failed: %s\n", components.error().c_str());
        return 1;
    }

    runtime::Options opts;
    opts.session_file = fs::path(".astrite/sessions/current.jsonl");
    opts.system_prompt = frontends::system_prompt();
    opts.components = std::move(*components);

    auto rt = runtime::Runtime::create(std::move(opts));
    if (!rt.has_value()) {
        std::fprintf(stderr, "astrite: %s\n", rt.error().c_str());
        return 1;
    }
    runtime::Runtime& runtime = **rt;

    std::string line;
    while (true) {
        std::fputs("> ", stdout);
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) {
            std::fputc('\n', stdout);
            break; // Ctrl-D
        }
        if (line.empty()) {
            continue;
        }
        if (line == "/quit" || line == "/exit") {
            break;
        }

        std::jthread turn([&runtime, line] { runtime.submit_user_message(line); });

        bool mid_stream = false;
        bool terminal = false;
        while (!terminal) {
            auto event = runtime.events().pop();
            if (!event.has_value()) {
                break;
            }
            terminal = std::visit(
                overloaded{
                    [&](const runtime::Started&) { return false; },
                    [&](const runtime::AssistantDelta& d) {
                        std::fwrite(d.text.data(), 1, d.text.size(), stdout);
                        std::fflush(stdout);
                        mid_stream = true;
                        return false;
                    },
                    [&](const runtime::AssistantSettled&) { return false; },
                    [&](const runtime::ToolStarted& t) {
                        if (mid_stream) {
                            std::fputc('\n', stdout);
                            mid_stream = false;
                        }
                        std::printf("[tool %s %s]\n", t.name.c_str(), t.arguments.c_str());
                        return false;
                    },
                    [&](const runtime::ToolFinished& t) {
                        std::printf("[tool %s -> %s]\n", t.name.c_str(), t.content.c_str());
                        return false;
                    },
                    [&](const runtime::TurnFinished&) { return true; },
                    [&](const runtime::TurnError& e) {
                        std::fprintf(stderr, "\nerror: %s\n", e.message.c_str());
                        return true;
                    },
                },
                *event);
        }
        if (mid_stream) {
            std::fputc('\n', stdout);
        }
        // jthread joins here: the renderer saw the terminal event before the
        // prompt returns — output never races the next prompt.
    }
    return 0;
}
