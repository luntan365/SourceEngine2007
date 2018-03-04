// Copyright � 1996-2018, Valve Corporation, All rights reserved.

#ifndef POLYLIB_H
#define POLYLIB_H

#include "mathlib/mathlib.h"

struct winding_t {
  int numpoints;
  Vector *p;  // variable sized
  int maxpoints;
  winding_t *next;
};

#define MAX_POINTS_ON_WINDING 64

// you can define on_epsilon in the makefile as tighter
// point on plane side epsilon
// todo: need a world-space epsilon, a lightmap-space epsilon, and a texture
// space epsilon or at least convert from a world-space epsilon to lightmap and
// texture space epsilons
#ifndef ON_EPSILON
#define ON_EPSILON 0.1
#endif

winding_t *AllocWinding(int points);
f32 WindingArea(winding_t *w);
void WindingCenter(winding_t *w, Vector &center);
f32 WindingAreaAndBalancePoint(winding_t *w, Vector &center);
void ClipWindingEpsilon(winding_t *in, const Vector &normal, f32 dist,
                        f32 epsilon, winding_t **front, winding_t **back);

// translates everything by offset, then does the clip, then translates back (to
// keep precision)
void ClipWindingEpsilon_Offset(winding_t *in, const Vector &normal, f32 dist,
                               f32 epsilon, winding_t **front,
                               winding_t **back, const Vector &offset);

void ClassifyWindingEpsilon(winding_t *in, const Vector &normal, f32 dist,
                            f32 epsilon, winding_t **front, winding_t **back,
                            winding_t **on);
void ClassifyWindingEpsilon_Offset(winding_t *in, const Vector &normal,
                                   f32 dist, f32 epsilon, winding_t **front,
                                   winding_t **back, winding_t **on,
                                   const Vector &offset);

winding_t *ChopWinding(winding_t *in, const Vector &normal, f32 dist);
winding_t *CopyWinding(winding_t *w);
winding_t *ReverseWinding(winding_t *w);
winding_t *BaseWindingForPlane(const Vector &normal, f32 dist);
void CheckWinding(winding_t *w);
void WindingPlane(winding_t *w, Vector &normal, f32 *dist);
void RemoveColinearPoints(winding_t *w);
int WindingOnPlaneSide(winding_t *w, const Vector &normal, f32 dist);
void FreeWinding(winding_t *w);
void WindingBounds(winding_t *w, Vector &mins, Vector &maxs);

void ChopWindingInPlace(winding_t **w, const Vector &normal, f32 dist,
                        f32 epsilon);
// frees the original if clipped

bool PointInWinding(Vector const &pt, winding_t *pWinding);

// translates a winding by offset
void TranslateWinding(winding_t *pWinding, const Vector &offset);

void pw(winding_t *w);

#endif  // POLYLIB_H
