// dxgi_backend.h
#pragma once

#include <windows.h>
#include <dxgi.h>

namespace OpenAnsel::DXGI {

bool InitializeHooks();
void OnFactoryCreated(IDXGIFactory* factory);
void ShutdownHooks();
void Log(const char* msg);

} // namespace OpenAnsel::DXGI
