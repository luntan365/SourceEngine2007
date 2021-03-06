// Copyright � 1996-2018, Valve Corporation, All rights reserved.

#include "CommentaryExplanationDialog.h"

#include <cstdio>
#include "BasePanel.h"
#include "EngineInterface.h"
#include "GameUI_Interface.h"
#include "tier1/convar.h"
#include "vgui/IInput.h"
#include "vgui/ISurface.h"

using namespace vgui;

 
#include "tier0/include/memdbgon.h"

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CCommentaryExplanationDialog::CCommentaryExplanationDialog(
    vgui::Panel *parent, char *pszFinishCommand)
    : BaseClass(parent, "CommentaryExplanationDialog") {
  SetDeleteSelfOnClose(true);
  SetSizeable(false);

  input()->SetAppModalSurface(GetVPanel());

  LoadControlSettings("Resource/CommentaryExplanationDialog.res");

  MoveToCenterOfScreen();

  GameUI().PreventEngineHideGameUI();

  // Save off the finish command
  Q_snprintf(m_pszFinishCommand, sizeof(m_pszFinishCommand), pszFinishCommand);
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CCommentaryExplanationDialog::~CCommentaryExplanationDialog() {}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CCommentaryExplanationDialog::OnKeyCodePressed(KeyCode code) {
  if (code == KEY_ESCAPE) {
    Close();
  } else {
    BaseClass::OnKeyCodePressed(code);
  }
}

//-----------------------------------------------------------------------------
// Purpose: handles button commands
//-----------------------------------------------------------------------------
void CCommentaryExplanationDialog::OnCommand(const char *command) {
  if (!_stricmp(command, "ok")) {
    Close();
    BasePanel()->FadeToBlackAndRunEngineCommand(m_pszFinishCommand);
  } else if (!_stricmp(command, "cancel") || !_stricmp(command, "close")) {
    Close();
  } else {
    BaseClass::OnCommand(command);
  }
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CCommentaryExplanationDialog::OnClose(void) {
  BaseClass::OnClose();
  GameUI().AllowEngineHideGameUI();
}
