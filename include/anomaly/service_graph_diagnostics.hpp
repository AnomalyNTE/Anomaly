#pragma once

#include "anomaly/service_graph.hpp"

#include <string>

namespace anomaly {

[[nodiscard]] std::string SerializeServiceGraphSnapshotJson(
    const ServiceGraphSnapshot& snapshot);

}  // namespace anomaly
