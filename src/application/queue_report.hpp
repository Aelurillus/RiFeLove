#pragma once

#include "database/task_store.hpp"
#include <string>
#include <vector>

namespace rifelove::application {

// Produces a read-only diagnostic snapshot. Perceptual quality scores remain
// explicitly unmeasured unless a future report has a matching reference set.
[[nodiscard]] std::string serializeQueueReport(const std::string& databasePath,
    const std::vector<ConfiguredTask>& tasks);

} // namespace rifelove::application
