// Copyright � 1996-2018, Valve Corporation, All rights reserved.

#ifndef BASEPROPERTIESCONTAINER_H
#define BASEPROPERTIESCONTAINER_H

#include "dme_controls/ElementPropertiesTree.h"

class CBasePropertiesContainer : public CElementPropertiesTreeInternal {
  DECLARE_CLASS_SIMPLE(CBasePropertiesContainer,
                       CElementPropertiesTreeInternal);

 public:
  CBasePropertiesContainer(vgui::Panel *parent, IDmNotify *pNotify,
                           CDmeEditorTypeDictionary *pDict = NULL);

  virtual bool IsDroppable(CUtlVector<KeyValues *> &msglist);
  virtual void OnPanelDropped(CUtlVector<KeyValues *> &msglist);
};

#endif  // BASEPROPERTIESCONTAINER_H
