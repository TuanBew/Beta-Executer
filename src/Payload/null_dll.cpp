// null_dll.cpp — Minimal DLL for injection testing
// Does nothing in DllMain, just confirms the LoadLibraryA path works.
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // Write a marker file to confirm DllMain was called
        HANDLE h = CreateFileA("null_dll_loaded.txt",
            GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        break;
    }
    return TRUE;
}
