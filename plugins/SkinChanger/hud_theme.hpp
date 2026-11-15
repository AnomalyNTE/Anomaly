#pragma once

struct AnomalyHostApiV1;
struct AnomalyStatusV1;
struct AnomalyUiServiceV1;

namespace lite_hud_theme {
void Load(const AnomalyHostApiV1* host) noexcept;
bool Available() noexcept;
AnomalyStatusV1 Stop() noexcept;
void Update() noexcept;
void Draw(const AnomalyUiServiceV1* ui);
}
