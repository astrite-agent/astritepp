#pragma once

// The local slash-command registry: commands the frontend handles itself (they
// never reach the model). Pure data + prefix completion, so the popup and the
// dispatch share one source of truth and it's unit-testable without FTXUI.

#include <string>
#include <string_view>
#include <vector>

namespace astrite::tui {

struct SlashCommand {
    std::string name;    // includes the leading '/'
    std::string summary; // one line, shown in the popup and the /commands modal
};

inline const std::vector<SlashCommand>& all_commands() {
    static const std::vector<SlashCommand> kCommands = {
        {"/help", "Show the help overlay"},
        {"/commands", "List all slash commands"},
        {"/status", "Show session status"},
        {"/clear", "Clear the transcript view"},
        {"/focus", "Toggle focus: input ↔ transcript"},
        {"/collapse", "Collapse all transcript segments"},
        {"/expand", "Expand all transcript segments"},
        {"/top", "Scroll to the top"},
        {"/bottom", "Scroll to the bottom (follow)"},
        {"/quit", "Exit astrite++"},
        {"/exit", "Exit astrite++"},
    };
    return kCommands;
}

// Completion candidates for a raw input string. Only a bare leading-slash token
// (no spaces) is a completion context; anything else returns empty.
inline std::vector<const SlashCommand*> complete(std::string_view input) {
    std::vector<const SlashCommand*> out;
    if (input.empty() || input.front() != '/' ||
        input.find(' ') != std::string_view::npos) {
        return out;
    }
    for (const SlashCommand& c : all_commands()) {
        if (std::string_view(c.name).substr(0, input.size()) == input) {
            out.push_back(&c);
        }
    }
    return out;
}

// Resolve a command line ("/help me") to its command via the leading token.
inline const SlashCommand* lookup(std::string_view line) {
    if (line.empty() || line.front() != '/') {
        return nullptr;
    }
    std::string_view token = line.substr(0, line.find(' '));
    for (const SlashCommand& c : all_commands()) {
        if (c.name == token) {
            return &c;
        }
    }
    return nullptr;
}

} // namespace astrite::tui
