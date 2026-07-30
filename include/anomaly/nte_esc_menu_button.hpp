#pragma once

#include "anomaly/sdk/base.h"

#include <cstdint>
#include <string>
#include <vector>

namespace anomaly {

struct NteEscMenuButtonSnapshot final {
    AnomalyGenerationHandleV1 handle{};
    std::string id;
    std::string label;
    std::uint32_t icon_format{};
    std::vector<std::uint8_t> icon_bytes;
};

}  // namespace anomaly
