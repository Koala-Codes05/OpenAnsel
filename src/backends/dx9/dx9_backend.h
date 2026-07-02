#pragma once

#include <windows.h>

namespace OpenAnsel::DX9 {

bool InitializeHooks();
bool InitializeHooksProxyMode();  // synchronous init used by d3d9_proxy.cpp (no polling)
void ShutdownHooks();

} // namespace OpenAnsel::DX9
