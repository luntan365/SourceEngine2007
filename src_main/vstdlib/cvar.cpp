// Copyright © 1996-2018, Valve Corporation, All rights reserved.

#include "vstdlib/cvar.h"

#include <ctype.h>
#include "tier0/include/icommandline.h"
#include "tier0/include/vprof.h"
#include "tier1/KeyValues.h"
#include "tier1/convar.h"
#include "tier1/strtools.h"
#include "tier1/tier1.h"
#include "tier1/utlbuffer.h"
#include "tier1/utlrbtree.h"

#ifdef OS_POSIX
#include <wctype.h>
#include <cwchar>
#endif

#include "tier0/include/memdbgon.h"

// Default implementation  of CvarQuery
class CDefaultCvarQuery : public CBaseAppSystem<ICvarQuery> {
 public:
  void *QueryInterface(const char *pInterfaceName) override {
    if (!Q_stricmp(pInterfaceName, CVAR_QUERY_INTERFACE_VERSION))
      return (ICvarQuery *)this;
    return nullptr;
  }

  bool AreConVarsLinkable(const ConVar *child, const ConVar *parent) override {
    return true;
  }
};

static CDefaultCvarQuery s_DefaultCvarQuery;
static ICvarQuery *s_pCVarQuery = nullptr;

// Default implementation
class CCvar : public ICvar {
 public:
  CCvar();

  // Methods of IAppSystem
  bool Connect(CreateInterfaceFn factory) override;
  void Disconnect() override;
  void *QueryInterface(const char *pInterfaceName) override;
  InitReturnVal_t Init() override;
  void Shutdown() override;

  // Inherited from ICVar
  CVarDLLIdentifier_t AllocateDLLIdentifier() override;
  void RegisterConCommand(ConCommandBase *pCommandBase) override;
  void UnregisterConCommand(ConCommandBase *pCommandBase) override;
  void UnregisterConCommands(CVarDLLIdentifier_t id) override;
  const char *GetCommandLineValue(const char *pVariableName) override;
  ConCommandBase *FindCommandBase(const char *name) override;
  const ConCommandBase *FindCommandBase(const char *name) const override;
  ConVar *FindVar(const char *var_name) override;
  const ConVar *FindVar(const char *var_name) const override;
  ConCommand *FindCommand(const char *name) override;
  const ConCommand *FindCommand(const char *name) const override;
  ConCommandBase *GetCommands() override;
  const ConCommandBase *GetCommands(void) const override;
  void InstallGlobalChangeCallback(FnChangeCallback_t callback) override;
  void RemoveGlobalChangeCallback(FnChangeCallback_t callback) override;
  void CallGlobalChangeCallbacks(ConVar *var, const char *pOldString,
                                 float flOldValue) override;
  void InstallConsoleDisplayFunc(IConsoleDisplayFunc *pDisplayFunc) override;
  void RemoveConsoleDisplayFunc(IConsoleDisplayFunc *pDisplayFunc) override;
  void ConsoleColorPrintf(const Color &clr, const char *pFormat,
                          ...) const override;
  void ConsolePrintf(const char *pFormat, ...) const override;
  void ConsoleDPrintf(const char *pFormat, ...) const override;
  void RevertFlaggedConVars(int nFlag) override;
  void InstallCVarQuery(ICvarQuery *pQuery) override;

 private:
  enum {
    CONSOLE_COLOR_PRINT = 0,
    CONSOLE_PRINT,
    CONSOLE_DPRINT,
  };

  // Standard console commands
  CON_COMMAND_MEMBER_F(
      CCvar, "find", Find,
      "Find concommands with the specified string in their name/help text.", 0)

  void DisplayQueuedMessages();

  CUtlVector<FnChangeCallback_t> m_GlobalChangeCallbacks;
  CUtlVector<IConsoleDisplayFunc *> m_DisplayFuncs;
  int m_nNextDLLIdentifier;
  ConCommandBase *m_pConCommandList;

  // temporary console area so we can store prints before console display funs
  // are installed
  mutable CUtlBuffer m_TempConsoleBuffer;
};

// Factor for CVars
static CCvar s_Cvar;
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CCvar, ICvar, CVAR_INTERFACE_VERSION, s_Cvar);

// Returns a CVar dictionary for tool usage
CreateInterfaceFn VStdLib_GetICVarFactory() { return Sys_GetFactoryThis(); }

CCvar::CCvar()
    : m_nNextDLLIdentifier{0},
      m_pConCommandList{0},
      m_TempConsoleBuffer(0, 1024) {}

// Methods of IAppSystem
bool CCvar::Connect(CreateInterfaceFn factory) {
  ConnectTier1Libraries(&factory, 1);

  s_pCVarQuery = (ICvarQuery *)factory(CVAR_QUERY_INTERFACE_VERSION, nullptr);
  if (!s_pCVarQuery) {
    s_pCVarQuery = &s_DefaultCvarQuery;
  }

  ConVar_Register();
  return true;
}

void CCvar::Disconnect() {
  ConVar_Unregister();
  s_pCVarQuery = nullptr;
  DisconnectTier1Libraries();
}

InitReturnVal_t CCvar::Init() { return INIT_OK; }

void CCvar::Shutdown() {}

void *CCvar::QueryInterface(const char *pInterfaceName) {
  // We implement the ICvar interface
  if (!V_strcmp(pInterfaceName, CVAR_INTERFACE_VERSION)) return (ICvar *)this;

  return nullptr;
}

// Method allowing the engine ICvarQuery interface to take over
void CCvar::InstallCVarQuery(ICvarQuery *pQuery) {
  Assert(s_pCVarQuery == &s_DefaultCvarQuery);
  s_pCVarQuery = pQuery ? pQuery : &s_DefaultCvarQuery;
}

// Used by DLLs to be able to unregister all their commands + convars
CVarDLLIdentifier_t CCvar::AllocateDLLIdentifier() {
  return m_nNextDLLIdentifier++;
}

void CCvar::RegisterConCommand(ConCommandBase *variable) {
  // Already registered
  if (variable->IsRegistered()) return;

  variable->m_bRegistered = true;

  const char *pName = variable->GetName();
  if (!pName || !pName[0]) {
    variable->m_pNext = nullptr;
    return;
  }

  // If the variable is already defined, then setup the new variable as a proxy
  // to it.
  const ConCommandBase *pOther = FindVar(variable->GetName());
  if (pOther) {
    if (variable->IsCommand() || pOther->IsCommand()) {
      Warning(
          "WARNING: unable to link %s and %s because one or more is a "
          "ConCommand.\n",
          variable->GetName(), pOther->GetName());
    } else {
      // This cast is ok because we make sure they're ConVars above.
      const ConVar *pChildVar = static_cast<const ConVar *>(variable);
      const ConVar *pParentVar = static_cast<const ConVar *>(pOther);

      // See if it's a valid linkage
      if (s_pCVarQuery->AreConVarsLinkable(pChildVar, pParentVar)) {
        // Make sure the default values are the same (but only spew about this
        // for FCVAR_REPLICATED)
        if (pChildVar->m_pszDefaultValue && pParentVar->m_pszDefaultValue &&
            pChildVar->IsFlagSet(FCVAR_REPLICATED) &&
            pParentVar->IsFlagSet(FCVAR_REPLICATED)) {
          if (Q_stricmp(pChildVar->m_pszDefaultValue,
                        pParentVar->m_pszDefaultValue) != 0) {
            Warning(
                "Parent and child ConVars with different default values! %s "
                "child: %s parent: %s (parent wins)\n",
                variable->GetName(), pChildVar->m_pszDefaultValue,
                pParentVar->m_pszDefaultValue);
          }
        }

        const_cast<ConVar *>(pChildVar)->m_pParent =
            const_cast<ConVar *>(pParentVar)->m_pParent;

        // check the parent's callbacks and slam if doesn't have, warn if both
        // have callbacks
        if (pChildVar->m_fnChangeCallback) {
          if (!pParentVar->m_fnChangeCallback) {
            const_cast<ConVar *>(pParentVar)->m_fnChangeCallback =
                pChildVar->m_fnChangeCallback;
          } else {
            Warning("Convar %s has multiple different change callbacks\n",
                    variable->GetName());
          }
        }

        // make sure we don't have conflicting help strings.
        if (pChildVar->m_pszHelpString &&
            Q_strlen(pChildVar->m_pszHelpString) != 0) {
          if (pParentVar->m_pszHelpString &&
              Q_strlen(pParentVar->m_pszHelpString) != 0) {
            if (Q_stricmp(pParentVar->m_pszHelpString,
                          pChildVar->m_pszHelpString) != 0) {
              Warning(
                  "Convar %s has multiple help strings:\n\tparent (wins): "
                  "\"%s\"\n\tchild: \"%s\"\n",
                  variable->GetName(), pParentVar->m_pszHelpString,
                  pChildVar->m_pszHelpString);
            }
          } else {
            const_cast<ConVar *>(pParentVar)->m_pszHelpString =
                pChildVar->m_pszHelpString;
          }
        }

        // make sure we don't have conflicting FCVAR_CHEAT flags.
        if ((pChildVar->m_nFlags & FCVAR_CHEAT) !=
            (pParentVar->m_nFlags & FCVAR_CHEAT)) {
          Warning(
              "Convar %s has conflicting FCVAR_CHEAT flags (child: %s, parent: "
              "%s, parent wins)\n",
              variable->GetName(),
              (pChildVar->m_nFlags & FCVAR_CHEAT) ? "FCVAR_CHEAT"
                                                  : "no FCVAR_CHEAT",
              (pParentVar->m_nFlags & FCVAR_CHEAT) ? "FCVAR_CHEAT"
                                                   : "no FCVAR_CHEAT");
        }

        // make sure we don't have conflicting FCVAR_REPLICATED flags.
        if ((pChildVar->m_nFlags & FCVAR_REPLICATED) !=
            (pParentVar->m_nFlags & FCVAR_REPLICATED)) {
          Warning(
              "Convar %s has conflicting FCVAR_REPLICATED flags (child: %s, "
              "parent: %s, parent wins)\n",
              variable->GetName(),
              (pChildVar->m_nFlags & FCVAR_REPLICATED) ? "FCVAR_REPLICATED"
                                                       : "no FCVAR_REPLICATED",
              (pParentVar->m_nFlags & FCVAR_REPLICATED)
                  ? "FCVAR_REPLICATED"
                  : "no FCVAR_REPLICATED");
        }

        // make sure we don't have conflicting FCVAR_DONTRECORD flags.
        if ((pChildVar->m_nFlags & FCVAR_DONTRECORD) !=
            (pParentVar->m_nFlags & FCVAR_DONTRECORD)) {
          Warning(
              "Convar %s has conflicting FCVAR_DONTRECORD flags (child: %s, "
              "parent: %s, parent wins)\n",
              variable->GetName(),
              (pChildVar->m_nFlags & FCVAR_DONTRECORD) ? "FCVAR_DONTRECORD"
                                                       : "no FCVAR_DONTRECORD",
              (pParentVar->m_nFlags & FCVAR_DONTRECORD)
                  ? "FCVAR_DONTRECORD"
                  : "no FCVAR_DONTRECORD");
        }
      }
    }

    variable->m_pNext = nullptr;
    return;
  }

  // link the variable in
  variable->m_pNext = m_pConCommandList;
  m_pConCommandList = variable;
}

void CCvar::UnregisterConCommand(ConCommandBase *pCommandToRemove) {
  // Not registered? Don't bother
  if (!pCommandToRemove->IsRegistered()) return;

  pCommandToRemove->m_bRegistered = false;

  // TODO(d.rattman): Should we make this a doubly-linked list? Would remove
  // faster
  ConCommandBase *pPrev = nullptr;
  for (ConCommandBase *pCommand = m_pConCommandList; pCommand;
       pCommand = pCommand->m_pNext) {
    if (pCommand != pCommandToRemove) {
      pPrev = pCommand;
      continue;
    }

    if (pPrev == nullptr) {
      m_pConCommandList = pCommand->m_pNext;
    } else {
      pPrev->m_pNext = pCommand->m_pNext;
    }
    pCommand->m_pNext = nullptr;
    break;
  }
}

void CCvar::UnregisterConCommands(CVarDLLIdentifier_t id) {
  ConCommandBase *pNewList = nullptr;
  ConCommandBase *pCommand = m_pConCommandList;

  while (pCommand) {
    ConCommandBase *pNext = pCommand->m_pNext;
    if (pCommand->GetDLLIdentifier() != id) {
      pCommand->m_pNext = pNewList;
      pNewList = pCommand;
    } else {
      // Unlink
      pCommand->m_bRegistered = false;
      pCommand->m_pNext = nullptr;
    }

    pCommand = pNext;
  }

  m_pConCommandList = pNewList;
}

// Finds base commands
const ConCommandBase *CCvar::FindCommandBase(const char *name) const {
  const ConCommandBase *cmd = GetCommands();
  for (; cmd; cmd = cmd->GetNext()) {
    if (!Q_stricmp(name, cmd->GetName())) return cmd;
  }
  return nullptr;
}

ConCommandBase *CCvar::FindCommandBase(const char *name) {
  ConCommandBase *cmd = GetCommands();
  for (; cmd; cmd = cmd->GetNext()) {
    if (!Q_stricmp(name, cmd->GetName())) return cmd;
  }
  return nullptr;
}

// Purpose Finds ConVars
const ConVar *CCvar::FindVar(const char *var_name) const {
  VPROF_INCREMENT_COUNTER("CCvar::FindVar", 1);
  VPROF("CCvar::FindVar");
  const ConCommandBase *var = FindCommandBase(var_name);
  if (!var || var->IsCommand()) return nullptr;

  return static_cast<const ConVar *>(var);
}

ConVar *CCvar::FindVar(const char *var_name) {
  VPROF_INCREMENT_COUNTER("CCvar::FindVar", 1);
  VPROF("CCvar::FindVar");
  ConCommandBase *var = FindCommandBase(var_name);
  if (!var || var->IsCommand()) return nullptr;

  return static_cast<ConVar *>(var);
}

// Purpose Finds ConCommands
const ConCommand *CCvar::FindCommand(const char *pCommandName) const {
  const ConCommandBase *var = FindCommandBase(pCommandName);
  if (!var || !var->IsCommand()) return nullptr;

  return static_cast<const ConCommand *>(var);
}

ConCommand *CCvar::FindCommand(const char *pCommandName) {
  ConCommandBase *var = FindCommandBase(pCommandName);
  if (!var || !var->IsCommand()) return nullptr;

  return static_cast<ConCommand *>(var);
}

const char *CCvar::GetCommandLineValue(const char *pVariableName) {
  int nLen = Q_strlen(pVariableName);
  char *pSearch = stack_alloc<char>(nLen + 2);
  pSearch[0] = '+';
  memcpy(&pSearch[1], pVariableName, nLen + 1);
  return CommandLine()->ParmValue(pSearch);
}

ConCommandBase *CCvar::GetCommands() { return m_pConCommandList; }

const ConCommandBase *CCvar::GetCommands() const {
  return m_pConCommandList;
}

// Install, remove global callbacks
void CCvar::InstallGlobalChangeCallback(FnChangeCallback_t callback) {
  Assert(callback && m_GlobalChangeCallbacks.Find(callback) < 0);
  m_GlobalChangeCallbacks.AddToTail(callback);
}

void CCvar::RemoveGlobalChangeCallback(FnChangeCallback_t callback) {
  Assert(callback);
  m_GlobalChangeCallbacks.FindAndRemove(callback);
}

void CCvar::CallGlobalChangeCallbacks(ConVar *var, const char *pOldString,
                                      float flOldValue) {
  int nCallbackCount = m_GlobalChangeCallbacks.Count();
  for (int i = 0; i < nCallbackCount; ++i) {
    (*m_GlobalChangeCallbacks[i])(var, pOldString, flOldValue);
  }
}

// Sets convars containing the flags to their default value
void CCvar::RevertFlaggedConVars(int nFlag) {
  for (const ConCommandBase *var = GetCommands(); var; var = var->GetNext()) {
    if (var->IsCommand()) continue;

    ConVar *c = (ConVar *)var;

    if (!c->IsFlagSet(nFlag)) continue;

    // It's == to the default value, don't count
    if (!Q_stricmp(c->GetDefault(), c->GetString())) continue;

    c->Revert();

    // DevMsg( "%s = \"%s\" (reverted)\n", cvar->GetName(), cvar->GetString() );
  }
}

// Display queued messages
void CCvar::DisplayQueuedMessages() {
  // Display any queued up messages
  if (m_TempConsoleBuffer.TellPut() == 0) return;

  Color clr;
  int nStringLength;
  while (m_TempConsoleBuffer.IsValid()) {
    int nType = m_TempConsoleBuffer.GetChar();
    if (nType == CONSOLE_COLOR_PRINT) {
      clr.SetRawColor(m_TempConsoleBuffer.GetInt());
    }
    nStringLength = m_TempConsoleBuffer.PeekStringLength();
    char *pTemp = (char *)_alloca(nStringLength + 1);
    m_TempConsoleBuffer.GetString(pTemp);

    switch (nType) {
      case CONSOLE_COLOR_PRINT:
        ConsoleColorPrintf(clr, pTemp);
        break;

      case CONSOLE_PRINT:
        ConsolePrintf(pTemp);
        break;

      case CONSOLE_DPRINT:
        ConsoleDPrintf(pTemp);
        break;
    }
  }

  m_TempConsoleBuffer.Purge();
}

// Install a console printer
void CCvar::InstallConsoleDisplayFunc(IConsoleDisplayFunc *pDisplayFunc) {
  Assert(m_DisplayFuncs.Find(pDisplayFunc) < 0);
  m_DisplayFuncs.AddToTail(pDisplayFunc);
  DisplayQueuedMessages();
}

void CCvar::RemoveConsoleDisplayFunc(IConsoleDisplayFunc *pDisplayFunc) {
  m_DisplayFuncs.FindAndRemove(pDisplayFunc);
}

void CCvar::ConsoleColorPrintf(const Color &clr, const char *pFormat,
                               ...) const {
  char temp[8192];
  va_list argptr;
  va_start(argptr, pFormat);
  _vsnprintf_s(temp, sizeof(temp) - 1, pFormat, argptr);
  va_end(argptr);
  temp[sizeof(temp) - 1] = 0;

  int c = m_DisplayFuncs.Count();
  if (c == 0) {
    m_TempConsoleBuffer.PutChar(CONSOLE_COLOR_PRINT);
    m_TempConsoleBuffer.PutInt(clr.GetRawColor());
    m_TempConsoleBuffer.PutString(temp);
    return;
  }

  for (int i = 0; i < c; ++i) {
    m_DisplayFuncs[i]->ColorPrint(clr, temp);
  }
}

void CCvar::ConsolePrintf(const char *pFormat, ...) const {
  char temp[8192];
  va_list argptr;
  va_start(argptr, pFormat);
  _vsnprintf_s(temp, sizeof(temp) - 1, pFormat, argptr);
  va_end(argptr);
  temp[sizeof(temp) - 1] = 0;

  int c = m_DisplayFuncs.Count();
  if (c == 0) {
    m_TempConsoleBuffer.PutChar(CONSOLE_PRINT);
    m_TempConsoleBuffer.PutString(temp);
    return;
  }

  for (int i = 0; i < c; ++i) {
    m_DisplayFuncs[i]->Print(temp);
  }
}

void CCvar::ConsoleDPrintf(const char *pFormat, ...) const {
  char temp[8192];
  va_list argptr;
  va_start(argptr, pFormat);
  _vsnprintf_s(temp, sizeof(temp) - 1, pFormat, argptr);
  va_end(argptr);
  temp[sizeof(temp) - 1] = 0;

  int c = m_DisplayFuncs.Count();
  if (c == 0) {
    m_TempConsoleBuffer.PutChar(CONSOLE_DPRINT);
    m_TempConsoleBuffer.PutString(temp);
    return;
  }

  for (int i = 0; i < c; ++i) {
    m_DisplayFuncs[i]->DPrint(temp);
  }
}

// Console commands
void CCvar::Find(const CCommand &args) {
  const char *search;
  const ConCommandBase *var;

  if (args.ArgC() != 2) {
    ConMsg("Usage:  find <string>\n");
    return;
  }

  // Get substring to find
  search = args[1];

  // Loop through vars and print out findings
  for (var = GetCommands(); var; var = var->GetNext()) {
    if (var->IsFlagSet(FCVAR_DEVELOPMENTONLY) || var->IsFlagSet(FCVAR_HIDDEN))
      continue;

    if (!Q_stristr(var->GetName(), search) &&
        !Q_stristr(var->GetHelpText(), search))
      continue;

    ConVar_PrintDescription(var);
  }
}
