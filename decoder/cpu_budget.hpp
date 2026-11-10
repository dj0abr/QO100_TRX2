// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <optional>

namespace qo100 {
// Call before creating sockets/threads. The original process supervises the
// worker and its descendants; only the forked worker returns std::nullopt.
// No root privileges, cgroup delegation or external limiter is required.
// Budget includes supervisor CPU, all worker threads and subprocesses. Unused
// quota cannot accumulate into a later burst. Children run nice 19/SCHED_IDLE.
std::optional<int> decoder_cpu_budget(unsigned percent);
}
