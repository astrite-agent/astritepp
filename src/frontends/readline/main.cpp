// The readline frontend: the end-to-end proof vehicle, not a throwaway. It reads
// lines, runs each turn on a worker thread, and renders the runtime event stream
// — streamed deltas live, tool activity inline. All agent behavior lives in the
// components; this file is plumbing.

#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "embedded_components.hpp"
#include "runtime/runtime.hpp"
#include "schema/load.hpp"
#include "util/overloaded.hpp"

namespace fs = std::filesystem;
using namespace astrite;

namespace {

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return std::move(ss).str();
}

std::expected<kernel::ComponentMap, std::string> load_components() {
    kernel::ComponentMap map;
    for (const auto& [name, embedded] : components::ALL) {
        std::string source(embedded);
        const fs::path override_path =
            fs::path(".astrite/components") / (std::string(name) + ".json");
        if (auto disk = read_file(override_path)) {
            // NOTE(M9): overrides will be trust-gated against the embedded baseline
            // (capability/risk diffs + hash-pinned records). The sprint loads them
            // un-gated but never silently.
            std::fprintf(stderr, "[override] %s loaded from %s (trust gate arrives at M9)\n",
                         std::string(name).c_str(), override_path.string().c_str());
            source = std::move(*disk);
        }
        auto component = schema::load_component_jsonc(source);
        if (!component.has_value()) {
            return std::unexpected(std::string(name) + ": " + component.error().message);
        }
        if (component->name != name) {
            return std::unexpected("component file for `" + std::string(name) +
                                   "` declares mismatched name `" + component->name + "`");
        }
        map.emplace(component->name, std::move(*component));
    }
    return map;
}

std::string system_prompt() {
    return "You are astrite++, a minimal secure coding agent. Your behavior is declarative "
           "component data interpreted by a small kernel; effects are capability-gated. "
           "Workspace root: " +
           fs::current_path().string() +
           ". Use the available tools when they help, and answer concisely.";
}

} // namespace

int main() {
    auto components = load_components();
    if (!components.has_value()) {
        std::fprintf(stderr, "astrite: component load failed: %s\n", components.error().c_str());
        return 1;
    }

    runtime::Options opts;
    opts.session_file = fs::path(".astrite/sessions/current.jsonl");
    opts.system_prompt = system_prompt();
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
