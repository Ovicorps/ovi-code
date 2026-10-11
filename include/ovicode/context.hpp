#pragma once

#include <cstddef>
#include <vector>

#include "ovicode/providers.hpp"

namespace ovicode {

struct ContextCompactionResult {
    std::size_t removed_messages = 0;
    std::size_t estimated_bytes = 0;
    bool exceeds_budget = false;
};

std::size_t estimateConversationBytes(const std::vector<Message>& messages);
ContextCompactionResult compactConversation(std::vector<Message>& messages,
                                            std::size_t max_bytes);

}  // namespace ovicode
