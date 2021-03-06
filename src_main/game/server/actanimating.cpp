// Copyright � 1996-2018, Valve Corporation, All rights reserved.
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//
#include "cbase.h"
#include "actanimating.h"
#include "animation.h"
#include "activitylist.h"

 
#include "tier0/include/memdbgon.h"

BEGIN_DATADESC( CActAnimating )
	DEFINE_CUSTOM_FIELD( m_Activity, ActivityDataOps() ),
END_DATADESC()


void CActAnimating::SetActivity( Activity act ) 
{ 
	int sequence = SelectWeightedSequence( act ); 
	if ( sequence != ACTIVITY_NOT_AVAILABLE )
	{
		ResetSequence( sequence );
		m_Activity = act; 
		SetCycle( 0 );
	}
}

