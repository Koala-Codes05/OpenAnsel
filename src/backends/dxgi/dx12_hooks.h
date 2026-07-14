// dx12_hooks.h
#pragma once

#include <dxgi.h>

namespace OpenAnsel::DX12 {

void HookSwapChain(IDXGISwapChain* swapChain);

} // namespace OpenAnsel::DX12
