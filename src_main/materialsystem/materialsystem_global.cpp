// Copyright � 1996-2018, Valve Corporation, All rights reserved.

#include "materialsystem_global.h"
#include "shaderapi/ishaderapi.h"
#include "shadersystem.h"

#include "tier0/include/memdbgon.h"

IShaderAPI* g_pShaderAPI = 0;
IShaderDeviceMgr* g_pShaderDeviceMgr = 0;
IShaderDevice* g_pShaderDevice = 0;
IShaderShadow* g_pShaderShadow = 0;
