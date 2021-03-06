// Copyright � 1996-2018, Valve Corporation, All rights reserved.

#ifndef QFILES_H
#define QFILES_H

//
// qfiles.h: quake file formats
// This file must be identical in the quake and utils directories
//

#include "bspfile.h"
#include "tier0/include/basetypes.h"
#include "base/include/macros.h"
#include "worldsize.h"

#define MAX_OSPATH 260
#define MAX_QPATH 64

/*
========================================================================

The .pak files are just a linear collapse of a directory tree

========================================================================
*/

#define IDPAKHEADER (('K' << 24) + ('C' << 16) + ('A' << 8) + 'P')

#endif  // QFILES_H
