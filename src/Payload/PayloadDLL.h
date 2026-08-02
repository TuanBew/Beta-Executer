#pragma once
#include <windows.h>

// Called internally by DllMain — exposed for clarity
BOOL InitPayload(HMODULE hModule);
void ShutdownPayload();
