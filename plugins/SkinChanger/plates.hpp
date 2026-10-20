#pragma once
#include <anomaly/sdk/cpp.hpp>
namespace lite_plates {
void Load(const AnomalyHostApiV1*) noexcept;
void Start() noexcept;
void Stop() noexcept;
void Update() noexcept;
void Draw(const AnomalyUiServiceV1*) noexcept;
}
