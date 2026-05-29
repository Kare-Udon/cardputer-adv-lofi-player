#pragma once

#include "lofi_core.hpp"

#include <cstdint>

namespace lofi_input {

enum class QueueAdmission {
    Enqueue,
    DropIncomingRepeat,
    DropOldestThenEnqueue,
};

bool action_repeats(lofi::Action action);
bool action_uses_fast_repeat(lofi::Action action);
bool action_is_repeat_hot_path(lofi::Action action);
uint32_t initial_repeat_delay_ms(lofi::Action action);
uint32_t repeat_interval_ms(lofi::Action action, uint32_t repeat_count);
QueueAdmission queue_admission(bool queue_full, bool repeated);

} // namespace lofi_input
