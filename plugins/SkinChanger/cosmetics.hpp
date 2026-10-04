#pragma once
#include <anomaly/sdk/cpp.hpp>

AnomalyStatusV1 CosmeticsLoad(const AnomalyHostApiV1* host, void** context) noexcept;
AnomalyStatusV1 CosmeticsStart(void* context) noexcept;
AnomalyStatusV1 CosmeticsStop(void* context, uint32_t reason) noexcept;
void CosmeticsUnload(void* context) noexcept;
void CosmeticsUpdate(void* context, double delta) noexcept;
void CosmeticsDraw(void* context, const AnomalyUiServiceV1* ui, int category) noexcept;
