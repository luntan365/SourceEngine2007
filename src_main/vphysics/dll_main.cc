// Copyright © 1996-2018, Valve Corporation, All rights reserved.

#include "base/include/windows/windows_light.h"
#include <cassert>

BOOL APIENTRY DllMain(_In_ HINSTANCE instance, _In_ DWORD call_reason,
                      _In_ LPVOID) {
  switch (call_reason) {
    case DLL_PROCESS_ATTACH:
      ::DisableThreadLibraryCalls(instance);
      break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
      break;
    default:
      assert(false);
  }
  return TRUE;
}
