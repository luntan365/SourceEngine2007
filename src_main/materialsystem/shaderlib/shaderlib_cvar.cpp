// Copyright � 1996-2018, Valve Corporation, All rights reserved.

#include "icvar.h"

#include "tier1/tier1.h"

#include "tier0/include/memdbgon.h"

// ConVar stuff.
class CShaderLibConVarAccessor : public IConCommandBaseAccessor {
 public:
  bool RegisterConCommandBase(ConCommandBase *pCommand) override {
    // Link to engine's list instead
    g_pCVar->RegisterConCommand(pCommand);

    char const *pValue = g_pCVar->GetCommandLineValue(pCommand->GetName());
    if (pValue && !pCommand->IsCommand()) {
      ((ConVar *)pCommand)->SetValue(pValue);
    }

    return true;
  }
};

CShaderLibConVarAccessor g_ConVarAccessor;

void InitShaderLibCVars(CreateInterfaceFn cvarFactory) {
  if (g_pCVar) ConVar_Register(0, &g_ConVarAccessor);
}
