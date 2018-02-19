// Copyright � 1996-2018, Valve Corporation, All rights reserved.
//
// The morph operator class - sets morph target weights on meshes
//
//=============================================================================

#ifndef DMEMORPHOPERATOR_H
#define DMEMORPHOPERATOR_H
#ifdef _WIN32
#pragma once
#endif

#include "movieobjects/dmeoperator.h"


//-----------------------------------------------------------------------------
// Forward declarations
//-----------------------------------------------------------------------------
class CDmElement;
class CDmeMesh;


//-----------------------------------------------------------------------------
// A class representing a camera
//-----------------------------------------------------------------------------
class CDmeMorphOperator : public CDmeOperator
{
	DEFINE_ELEMENT( CDmeMorphOperator, CDmeOperator );

public:
	virtual void Operate();

	// need this until we have the EditApply message queue
	void OnAttributeChanged( CDmAttribute *pAttribute );

	virtual void GetInputAttributes ( CUtlVector< CDmAttribute * > &attrs );
	virtual void GetOutputAttributes( CUtlVector< CDmAttribute * > &attrs );

	// accessors
	u32 NumDeltaStateWeights();
	CDmElement *GetDeltaStateWeight( u32 i );
	CDmeMesh *GetMesh();

protected:
	CDmaElement< CDmeMesh >    m_mesh;
	CDmaElementArray< CDmElement > m_deltaStateWeights;
	CDmaString                 m_baseStateName;
};


#endif // DMEMORPHOPERATOR_H
