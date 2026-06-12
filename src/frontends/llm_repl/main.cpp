// astrite-llm: a tiny standalone REPL for exercising the LLM provider backends
// directly — no kernel, no components, no sandbox. Picks a provider from the
// environment (ASTRITE_PROVIDER / *_API_KEY / *_MODEL, plus a local .env), runs
// a real multi-turn chat, streams the reply, and prints usage (including cache
// tokens) so we can watch provider-specific features like prompt caching.

#include <cstdio>
#include <iostream>
#include <string>
#include <utility>

#include "llm/provider.hpp"
#include "llm/provider_factory.hpp"
#include "llm/sse.hpp"
#include "schema/value.hpp"
#include "util/dotenv.hpp"

using namespace astrite;

namespace {
schema::Value user_message(std::string text) {
    schema::Value m = schema::Value::object();
    m.set("role", schema::Value::str("user"));
    m.set("content", schema::Value::str(std::move(text)));
    return m;
}
} // namespace

int main() {
    util::load_dotenv();

    auto provider = llm::make_provider_from_env();
    if (!provider.has_value()) {
        std::fprintf(stderr, "astrite-llm: %s\n", provider.error().c_str());
        return 1;
    }
    llm::Provider& p = **provider;

    const int ctx = p.context_limit(p.model());
    std::printf("astrite-llm · provider=%s · model=%s · context=%d\n", p.name().c_str(),
                p.model().c_str(), ctx);
    std::printf("commands: /models  /reset  /quit\n");

    schema::Value::List conversation; // grows across turns (stable prefix -> caching)
    std::string line;
    while (true) {
        std::fputs("\n› ", stdout);
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) {
            std::fputc('\n', stdout);
            break;
        }
        if (line.empty()) {
            continue;
        }
        if (line == "/quit" || line == "/exit") {
            break;
        }
        if (line == "/reset") {
            conversation.clear();
            std::printf("(conversation cleared)\n");
            continue;
        }
        if (line == "/models") {
            auto models = p.list_models();
            if (!models.has_value()) {
                std::printf("models: %s\n", models.error().c_str());
            } else {
                for (const llm::ModelInfo& m : *models) {
                    std::printf("  %-40s ctx=%d\n", m.id.c_str(), m.context_limit);
                }
            }
            continue;
        }

        conversation.push_back(user_message(line));
        auto result = p.stream_chat(
            schema::Value::list(conversation), schema::Value::list({}),
            [](std::string_view delta) {
                std::fwrite(delta.data(), 1, delta.size(), stdout);
                std::fflush(stdout);
            },
            {});
        std::fputc('\n', stdout);
        if (!result.has_value()) {
            std::printf("[error] %s\n", result.error().c_str());
            conversation.pop_back(); // drop the user turn that didn't get a reply
            continue;
        }
        conversation.push_back(llm::assistant_message(*result));
        if (!result->reasoning.text.empty()) {
            std::printf("[reasoning] %s\n", result->reasoning.text.c_str());
        }
        const llm::Usage& u = result->usage;
        if (u.present) {
            std::printf("[usage prompt=%d completion=%d total=%d cache_read=%d cache_write=%d]\n",
                        u.prompt_tokens, u.completion_tokens, u.total_tokens, u.cache_read_tokens,
                        u.cache_write_tokens);
        }
    }
    return 0;
}
