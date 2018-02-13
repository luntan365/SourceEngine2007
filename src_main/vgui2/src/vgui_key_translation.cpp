// Copyright � 1996-2017, Valve Corporation, All rights reserved.

#ifdef WIN32
#include "base/include/windows/windows_light.h"
#include "xbox/xboxstubs.h"
#endif

#include "tier0/include/dbg.h"
#include "vgui_key_translation.h"

#ifdef POSIX
#define VK_RETURN -1
#endif

#include "inputsystem/iinputsystem.h"
#include "tier2/tier2.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/include/memdbgon.h"

vgui::KeyCode KeyCode_VirtualKeyToVGUI(int key) {
  // Some tools load vgui for localization and never use input
  if (!g_pInputSystem) return KEY_NONE;
  return g_pInputSystem->VirtualKeyToButtonCode(key);
}

int KeyCode_VGUIToVirtualKey(vgui::KeyCode code) {
  // Some tools load vgui for localization and never use input
  if (!g_pInputSystem) return VK_RETURN;
  return g_pInputSystem->ButtonCodeToVirtualKey(code);
}
