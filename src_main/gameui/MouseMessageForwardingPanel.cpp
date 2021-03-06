// Copyright � 1996-2018, Valve Corporation, All rights reserved.
//
// Purpose: 
//


#include "MouseMessageForwardingPanel.h"
#include "tier1/keyvalues.h"

 
#include "tier0/include/memdbgon.h"

using namespace vgui;

CMouseMessageForwardingPanel::CMouseMessageForwardingPanel( Panel *parent, const char *name ) : BaseClass( parent, name )
{
	// don't draw an
	SetPaintEnabled(false);
	SetPaintBackgroundEnabled(false);
	SetPaintBorderEnabled(false);
}

void CMouseMessageForwardingPanel::PerformLayout()
{
	// fill out the whole area
	int w, t;
	GetParent()->GetSize(w, t);
	SetBounds(0, 0, w, t);
}

void CMouseMessageForwardingPanel::OnMousePressed( vgui::MouseCode code )
{
	CallParentFunction( new KeyValues("MousePressed", "code", code) );
}

void CMouseMessageForwardingPanel::OnMouseDoublePressed( vgui::MouseCode code )
{
	CallParentFunction( new KeyValues("MouseDoublePressed", "code", code) );
}