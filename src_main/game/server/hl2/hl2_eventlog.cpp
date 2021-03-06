// Copyright � 1996-2018, Valve Corporation, All rights reserved.
//
// Purpose: 
//
// $NoKeywords: $
//

#include "cbase.h"
#include "../EventLog.h"
#include "tier1/keyvalues.h"

 
#include "tier0/include/memdbgon.h"

class CHL2EventLog : public CEventLog
{
private:
	typedef CEventLog BaseClass;

public:
	virtual char const *Name() { return "CHL2EventLog"; }

	virtual ~CHL2EventLog() {};

public:
	bool PrintEvent( IGameEvent * event )	// override virtual function
	{
		if ( BaseClass::PrintEvent( event ) )
		{
			return true;
		}
	
		if ( Q_strcmp(event->GetName(), "hl2_") == 0 )
		{
			return PrintHL2Event( event );
		}

		return false;
	}

protected:

	bool PrintHL2Event( IGameEvent * event )	// print Mod specific logs
	{
	//	const char * name = event->GetName() + Q_strlen("hl2_"); // remove prefix

		return false;
	}

};

static CHL2EventLog s_HL2EventLog;

//-----------------------------------------------------------------------------
// Singleton access
//-----------------------------------------------------------------------------
IGameSystem* GameLogSystem()
{
	return &s_HL2EventLog;
}

