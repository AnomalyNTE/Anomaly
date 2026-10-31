#pragma once
#include <anomaly/sdk/cpp.hpp>
namespace lite_vehicle {
void Load(const AnomalyHostApiV1*) noexcept;
void Start() noexcept;
AnomalyStatusV1 Stop() noexcept;
void Update() noexcept;
void SetVisible(bool) noexcept;
void Draw(const AnomalyUiServiceV1*) noexcept;
}
