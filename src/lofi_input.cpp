#include "lofi_input.hpp"

namespace lofi_input {

bool action_repeats(lofi::Action action)
{
    return action == lofi::Action::Up || action == lofi::Action::Down || action == lofi::Action::Left ||
           action == lofi::Action::Right;
}

bool action_uses_fast_repeat(lofi::Action action)
{
    return action == lofi::Action::Up || action == lofi::Action::Down;
}

bool action_is_repeat_hot_path(lofi::Action action)
{
    return action_repeats(action);
}

uint32_t initial_repeat_delay_ms(lofi::Action action)
{
    return action_uses_fast_repeat(action) ? 220 : 430;
}

uint32_t repeat_interval_ms(lofi::Action action, uint32_t repeat_count)
{
    if (action_uses_fast_repeat(action)) {
        return repeat_count < 4 ? 120U : (repeat_count < 12 ? 80U : 55U);
    }
    return repeat_count < 4 ? 170U : (repeat_count < 12 ? 95U : 55U);
}

QueueAdmission queue_admission(bool queue_full, bool repeated)
{
    if (!queue_full) {
        return QueueAdmission::Enqueue;
    }
    return repeated ? QueueAdmission::DropIncomingRepeat : QueueAdmission::DropOldestThenEnqueue;
}

} // namespace lofi_input
