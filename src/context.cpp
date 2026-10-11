#include "ovicode/context.hpp"

#include <algorithm>

namespace ovicode {

namespace {

std::size_t messageBytes(const Message& message) {
    std::size_t bytes = message.text.size() + message.tool_call_id.size() +
                        message.tool_name.size() + 64;
    if (!message.reasoning_details.is_null()) {
        bytes += message.reasoning_details.dump().size();
    }
    for (const auto& call : message.tool_calls) {
        bytes += call.id.size() + call.name.size() + call.input.dump().size() +
                 64;
    }
    return bytes;
}

}  // namespace

std::size_t estimateConversationBytes(const std::vector<Message>& messages) {
    std::size_t total = 0;
    for (const auto& message : messages) total += messageBytes(message);
    return total;
}

ContextCompactionResult compactConversation(std::vector<Message>& messages,
                                            std::size_t max_bytes) {
    ContextCompactionResult result;
    result.estimated_bytes = estimateConversationBytes(messages);
    if (max_bytes == 0 || result.estimated_bytes <= max_bytes) return result;

    // Remove complete oldest exchanges. A retained suffix always starts with a
    // user message, never an orphaned assistant tool call or tool result.
    while (result.estimated_bytes > max_bytes && messages.size() > 1) {
        auto next_user = std::find_if(
            messages.begin() + 1, messages.end(), [](const Message& message) {
                return message.role == Message::Role::User;
            });
        if (next_user == messages.end()) break;
        result.removed_messages +=
            static_cast<std::size_t>(next_user - messages.begin());
        messages.erase(messages.begin(), next_user);
        result.estimated_bytes = estimateConversationBytes(messages);
    }
    result.exceeds_budget = result.estimated_bytes > max_bytes;
    return result;
}

}  // namespace ovicode
