// Copyright � 1996-2017, Valve Corporation, All rights reserved.
//
// Purpose: 
//


#include "cbase.h"
#include "variant_t.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/include/memdbgon.h"

void variant_t::SetEntity( CBaseEntity *val ) 
{ 
	eVal = val;
	fieldType = FIELD_EHANDLE; 
}


