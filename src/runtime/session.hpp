#pragma once

// Append-only JSONL transcript: the event-sourced conversation. The file IS the
// context — the turn component re-reads it every tick and decides what context
// means. Records are one JSON object per line; a corrupt line fails the read
// closed rather than silently dropping history.

#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "kernel/error.hpp"
#include "schema/value.hpp"
#include "schema/value_json.hpp"

namespace astrite::runtime {

class Session {
public:
    explicit Session(std::filesystem::path file) : file_(std::move(file)) {
        if (file_.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(file_.parent_path(), ec);
        }
    }

    [[nodiscard]] std::expected<void, kernel::KernelError> append(const schema::Value& record) {
        std::ofstream out(file_, std::ios::app | std::ios::binary);
        if (!out) {
            return std::unexpected(
                kernel::KernelError{"session.append: cannot open " + file_.string()});
        }
        out << schema::to_json(record) << '\n';
        out.flush();
        if (!out) {
            return std::unexpected(
                kernel::KernelError{"session.append: write failed for " + file_.string()});
        }
        return {};
    }

    // Returns { messages: [...] }; a missing file is an empty conversation.
    [[nodiscard]] std::expected<schema::Value, kernel::KernelError> read() const {
        using schema::Value;
        Value::List messages;
        std::ifstream in(file_, std::ios::binary);
        if (in) {
            std::string line;
            std::size_t line_no = 0;
            while (std::getline(in, line)) {
                ++line_no;
                if (line.empty()) {
                    continue;
                }
                auto record = schema::parse_json(line);
                if (!record.has_value()) {
                    return std::unexpected(kernel::KernelError{
                        "session.read: corrupt record at " + file_.string() + ":" +
                        std::to_string(line_no)});
                }
                messages.push_back(std::move(*record));
            }
        }
        Value ctx = Value::object();
        ctx.set("messages", Value::list(std::move(messages)));
        return ctx;
    }

    [[nodiscard]] const std::filesystem::path& file() const { return file_; }

private:
    std::filesystem::path file_;
};

} // namespace astrite::runtime
