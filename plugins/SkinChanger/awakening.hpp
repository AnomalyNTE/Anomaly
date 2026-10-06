#pragma once
#include <anomaly/sdk/cpp.hpp>
namespace lite_awakening {
void Load(const AnomalyHostApiV1* host) noexcept;
void Start() noexcept;
AnomalyStatusV1 Stop() noexcept;
void Update() noexcept;
void SetVisible(bool visible) noexcept;
void Draw(const AnomalyUiServiceV1* ui) noexcept;
}
