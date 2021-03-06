// Copyright © 1996-2018, Valve Corporation, All rights reserved.
//
// Purpose: Methods for muti-core dx9 threading

#include "d3d_async.h"

#include <cstdlib>
#include "ShaderAPIDX8_Global.h"
#include "UtlDict.h"
#include "UtlStringMap.h"
#include "UtlVector.h"
#include "base/include/windows/windows_light.h"
#include "datacache/idatacache.h"
#include "ishadersystem.h"
#include "locald3dtypes.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterialsystemhardwareconfig.h"
#include "materialsystem/ishader.h"
#include "materialsystem/shader_vcs_version.h"
#include "recording.h"
#include "shaderapidx8.h"
#include "tier0/include/fasttimer.h"
#include "tier0/include/vprof.h"
#include "tier1/convar.h"
#include "tier1/utlbuffer.h"
#include "tier1/utlsymbol.h"
#include "utllinkedlist.h"

#include "tier0/include/memdbgon.h"

template <class T, int QSIZE>
class FixedWorkQueue {
  T Data[QSIZE];
  char pad0[256];
  volatile int n_added;
  int write_index;
  char pad1[256];  // make sure these don't share cache lines
  volatile int n_removed;
  int read_index;

 public:
  FixedWorkQueue() {
    read_index = write_index = 0;
    n_added = n_removed = 0;
  }

  int IsEmpty() { return (n_added == n_removed); }

  int IsFull() { return (n_added - n_removed) >= QSIZE; }

  T GetWorkUnit() {
    if (IsEmpty()) return 0;
    return Data[read_index];
  }

  void MarkUnitDone() {
    n_removed++;
    read_index = (read_index + 1) % QSIZE;
  }

  void AddWorkUnit(T unit) {
    while (IsFull()) Sleep(0);
    Data[write_index] = unit;
    n_added++;
    write_index = (write_index + 1) % QSIZE;
  }
};

#define N_PUSH_BUFFERS 500

static volatile PushBuffer *PushBuffers[N_PUSH_BUFFERS];
FixedWorkQueue<PushBuffer *, N_PUSH_BUFFERS> PBQueue;

void __cdecl OurThreadInit(void *ourthis) {
  ((Direct3DDevice9Wrapper *)ourthis)->RunThread();
}

void Direct3DDevice9Wrapper::RunThread() {
  SetThreadAffinityMask(GetCurrentThread(), 2);
  for (;;) {
    PushBuffer *Pbuf = PBQueue.GetWorkUnit();
    if (!Pbuf) {
      ;  // Sleep(0);
    } else {
      ExecutePushBuffer(Pbuf);
      PBQueue.MarkUnitDone();
      Pbuf->m_State = PUSHBUFFER_AVAILABLE;
    }
  }
}

#define MAXIMUM_NUMBER_OF_BUFFERS_LOCKED_AT_ONCE 16

struct RememberedPointer {
  void *m_pKey;
  void *m_pRememberedPtr;
} RememberedPointerHistory[MAXIMUM_NUMBER_OF_BUFFERS_LOCKED_AT_ONCE];

void Direct3DDevice9Wrapper::SetASyncMode(bool onoff) {
  if (onoff) {
    if (!async_thread_handle_) {
      // allocate push buffers if we need to
      if (PushBuffers[0] == NULL) {
        for (int i = 0; i < N_PUSH_BUFFERS; i++)
          PushBuffers[i] = new PushBuffer;
      }
      // create thread and init communications
      memset(RememberedPointerHistory, 0, sizeof(RememberedPointerHistory));
      SetThreadAffinityMask(GetCurrentThread(), 1);
      async_thread_handle_ = _beginthread(OurThreadInit, 128 * 1024, this);
    }
  } else {
    Synchronize();
  }
}

PushBuffer *Direct3DDevice9Wrapper::FindFreePushBuffer(
    PushBufferState newstate) {
  for (;;) {
    for (int i = 0; i < N_PUSH_BUFFERS; i++) {
      if (PushBuffers[i]->m_State == PUSHBUFFER_AVAILABLE) {
        PushBuffers[i]->m_State = newstate;
        return (PushBuffer *)PushBuffers[i];
      }
    }
    // hmm, out of push buffers. better sleep and try again later
    SubmitPushBufferAndGetANewOne();
    Sleep(0);
  }
}

void Direct3DDevice9Wrapper::GetPushBuffer() {
  current_push_buffer_ = FindFreePushBuffer(PUSHBUFFER_BEING_FILLED);
  output_ptr_ = current_push_buffer_->m_BufferData;
  push_buffer_free_slots_ = PUSHBUFFER_NELEMS - 1;  // leave room for end marker
}

void Direct3DDevice9Wrapper::SubmitPushBufferAndGetANewOne() {
  // submit the current push buffer
  if (current_push_buffer_) {
    if (output_ptr_ ==
        current_push_buffer_
            ->m_BufferData)  // haven't done anyting, don't bother
      return;
    *(output_ptr_) = PBCMD_END;  // mark end
    current_push_buffer_->m_State = PUSHBUFFER_SUBMITTED;
    // here, enqueue for task
    PBQueue.AddWorkUnit(current_push_buffer_);
  }
  GetPushBuffer();
}

void Direct3DDevice9Wrapper::SubmitIfNotBusy() {
  if (PBQueue.IsEmpty()) SubmitPushBufferAndGetANewOne();
}

void Direct3DDevice9Wrapper::Synchronize() {
  if (ASyncMode()) {
    SubmitPushBufferAndGetANewOne();
    // here, wait for queue to become empty
    while (!PBQueue.IsEmpty()) {
      // Sleep(1);
    }
  }
}

void Direct3DDevice9Wrapper::AsynchronousLock(IDirect3DIndexBuffer9 *ib,
                                              size_t offset, size_t size,
                                              void **ptr, DWORD flags,
                                              LockedBufferContext *lb) {
  if (size <= sizeof(PushBuffers[0]->m_BufferData)) {
    // can use one of our pushbuffers for this
    lb->m_pPushBuffer =
        FindFreePushBuffer(PUSHBUFFER_BEING_USED_FOR_LOCKEDDATA);
    *(ptr) = lb->m_pPushBuffer->m_BufferData;
    Assert(*ptr);
    lb->m_pMallocedMemory = NULL;
  } else  // out of buffer space or size too big
  {
    lb->m_pPushBuffer = NULL;
    lb->m_pMallocedMemory = new uint8_t[size];
    *(ptr) = lb->m_pMallocedMemory;
  }
  // now, push lock commands
  AllocatePushBufferSpace(1 + N_DWORDS_IN_PTR + 3);
  *(output_ptr_++) = PBCMD_ASYNC_LOCK_IB;
  *((LPDIRECT3DINDEXBUFFER *)output_ptr_) = ib;
  output_ptr_ += N_DWORDS_IN_PTR;
  *(output_ptr_++) = offset;
  *(output_ptr_++) = size;
  *(output_ptr_++) = flags;
}

void Direct3DDevice9Wrapper::AsynchronousLock(IDirect3DVertexBuffer9 *vb,
                                              size_t offset, size_t size,
                                              void **ptr, DWORD flags,
                                              LockedBufferContext *lb) {
  // we have commands in flight. Need to use temporary memory for this lock.
  // if the size needed is < the amount of space in a push buffer, we can use
  // a push buffer for the buffer. Otherwise, we're going to malloc one.
  if (size <= sizeof(PushBuffers[0]->m_BufferData)) {
    // can use one of our pushbuffers for this
    lb->m_pPushBuffer =
        FindFreePushBuffer(PUSHBUFFER_BEING_USED_FOR_LOCKEDDATA);
    *(ptr) = lb->m_pPushBuffer->m_BufferData;
    Assert(*ptr);
    lb->m_pMallocedMemory = NULL;
  } else  // out of buffer space or size too big
  {
    lb->m_pPushBuffer = NULL;
    lb->m_pMallocedMemory = new uint8_t[size];
    *(ptr) = lb->m_pMallocedMemory;
  }
  // now, push lock commands
  AllocatePushBufferSpace(1 + N_DWORDS_IN_PTR + 3);
  *(output_ptr_++) = PBCMD_ASYNC_LOCK_VB;
  *((LPDIRECT3DVERTEXBUFFER *)output_ptr_) = vb;
  output_ptr_ += N_DWORDS_IN_PTR;
  *(output_ptr_++) = offset;
  *(output_ptr_++) = size;
  *(output_ptr_++) = flags;
}

inline void RememberLockedPointer(void *key, void *value) {
  int repl = -1;
  int i;
  for (i = 0; i < MAXIMUM_NUMBER_OF_BUFFERS_LOCKED_AT_ONCE; i++) {
    if (RememberedPointerHistory[i].m_pKey == key) break;
    if ((repl == -1) && (RememberedPointerHistory[i].m_pRememberedPtr == 0))
      repl = i;
  }
  if (i != MAXIMUM_NUMBER_OF_BUFFERS_LOCKED_AT_ONCE) {
    RememberedPointerHistory[i].m_pRememberedPtr = value;
    if (value == NULL) RememberedPointerHistory[i].m_pKey = NULL;
  } else {
    if (repl == -1) {
      Assert(0);
    } else {
      RememberedPointerHistory[repl].m_pKey = key;
      RememberedPointerHistory[repl].m_pRememberedPtr = value;
    }
  }
}

inline void *RecallLockedPointer(void *key) {
  for (int i = 0; i < MAXIMUM_NUMBER_OF_BUFFERS_LOCKED_AT_ONCE; i++)
    if (RememberedPointerHistory[i].m_pKey == key)
      return RememberedPointerHistory[i].m_pRememberedPtr;
  return NULL;
}

void Direct3DDevice9Wrapper::HandleAsynchronousLockVBCommand(
    uint32_t const *dptr) {
  dptr++;
  LPDIRECT3DVERTEXBUFFER vb = *((LPDIRECT3DVERTEXBUFFER *)dptr);
  dptr += N_DWORDS_IN_PTR;
  uint32_t offset = *(dptr++);
  uint32_t size = *(dptr++);
  uint32_t flags = *(dptr++);
  void *locked_ptr = 0;
  vb->Lock(offset, size, &locked_ptr, flags);
  RememberLockedPointer(vb, locked_ptr);
}

void Direct3DDevice9Wrapper::HandleAsynchronousUnLockVBCommand(
    uint32_t const *dptr) {
  dptr++;
  LPDIRECT3DVERTEXBUFFER vb = *((LPDIRECT3DVERTEXBUFFER *)dptr);
  dptr += N_DWORDS_IN_PTR;
  LockedBufferContext lb = *((LockedBufferContext *)dptr);
  dptr += N_DWORDS(LockedBufferContext);
  size_t unlock_size = *(dptr++);
  void *locked_data = RecallLockedPointer(vb);
  Assert(locked_data);
  if (lb.m_pPushBuffer) {
    Assert(!lb.m_pMallocedMemory);
    if (locked_data)
      memcpy(locked_data, lb.m_pPushBuffer->m_BufferData, unlock_size);
    lb.m_pPushBuffer->m_State = PUSHBUFFER_AVAILABLE;
  } else if (lb.m_pMallocedMemory) {
    Assert(!lb.m_pPushBuffer);
    if (locked_data) memcpy(locked_data, lb.m_pMallocedMemory, unlock_size);
    delete[](uint8_t *) lb.m_pMallocedMemory;
  }
  // now, actually unlock
  RememberLockedPointer(vb, NULL);
  vb->Unlock();
}

void Direct3DDevice9Wrapper::HandleAsynchronousLockIBCommand(
    uint32_t const *dptr) {
  dptr++;
  LPDIRECT3DINDEXBUFFER ib = *((LPDIRECT3DINDEXBUFFER *)dptr);
  Assert(ib);
  dptr += N_DWORDS_IN_PTR;
  uint32_t offset = *(dptr++);
  uint32_t size = *(dptr++);
  uint32_t flags = *(dptr++);
  void *locked_ptr = 0;
  ib->Lock(offset, size, &locked_ptr, flags);
  RememberLockedPointer(ib, locked_ptr);
}

void Direct3DDevice9Wrapper::HandleAsynchronousUnLockIBCommand(
    uint32_t const *dptr) {
  dptr++;
  LPDIRECT3DINDEXBUFFER ib = *((LPDIRECT3DINDEXBUFFER *)dptr);
  dptr += N_DWORDS_IN_PTR;
  LockedBufferContext lb = *((LockedBufferContext *)dptr);
  dptr += N_DWORDS(LockedBufferContext);
  size_t unlock_size = *(dptr++);
  void *locked_data = RecallLockedPointer(ib);
  Assert(locked_data);
  if (lb.m_pPushBuffer) {
    Assert(!lb.m_pMallocedMemory);
    if (locked_data)
      memcpy(locked_data, lb.m_pPushBuffer->m_BufferData, unlock_size);
    lb.m_pPushBuffer->m_State = PUSHBUFFER_AVAILABLE;
  } else if (lb.m_pMallocedMemory) {
    Assert(!lb.m_pPushBuffer);
    if (locked_data) memcpy(locked_data, lb.m_pMallocedMemory, unlock_size);
    delete[](uint8_t *) lb.m_pMallocedMemory;
  }
  // now, actually unlock
  RememberLockedPointer(ib, NULL);
  ib->Unlock();
}

static inline void *FetchPtr(uint32_t const *mem) {
  void **p = (void **)mem;
  return *p;
}

#define CALC_STATS 1
#if CALC_STATS
int n_commands_executed = 0;
int n_pbs_executed = 0;
#endif

void Direct3DDevice9Wrapper::ExecutePushBuffer(PushBuffer const *pb) {
  uint32_t const *dptr = pb->m_BufferData;
  n_pbs_executed++;
  for (;;) {
    n_commands_executed++;
    switch (dptr[0]) {
      case PBCMD_END:
        n_commands_executed--;  // doesn't count
        return;

      case PBCMD_SET_RENDERSTATE: {
        Dx9Device()->SetRenderState((D3DRENDERSTATETYPE)dptr[1], dptr[2]);
        dptr += 3;
        break;
      }

      case PBCMD_SET_SAMPLER_STATE: {
        Dx9Device()->SetSamplerState(dptr[1], (D3DSAMPLERSTATETYPE)dptr[2],
                                     dptr[3]);
        dptr += 4;
        break;
      }

      case PBCMD_DRAWPRIM: {
        Dx9Device()->DrawPrimitive((D3DPRIMITIVETYPE)dptr[1], dptr[2], dptr[3]);
        dptr += 4;
        break;
      }

      case PBCMD_DRAWINDEXEDPRIM: {
        Dx9Device()->DrawIndexedPrimitive((D3DPRIMITIVETYPE)dptr[1], dptr[2],
                                          dptr[3], dptr[4], dptr[5], dptr[6]);
        dptr += 7;
        break;
      }

      case PBCMD_SET_STREAM_SOURCE: {
        Dx9Device()->SetStreamSource(
            dptr[1], (IDirect3DVertexBuffer9 *)FetchPtr(dptr + 2), dptr[3],
            dptr[4]);
        dptr += 4 + N_DWORDS(IDirect3DVertexBuffer9 *);
        break;
      }

      case PBCMD_SET_TEXTURE: {
        Dx9Device()->SetTexture(dptr[1],
                                (IDirect3DBaseTexture *)FetchPtr(dptr + 2));
        dptr += 2 + N_DWORDS_IN_PTR;
        break;
      }

      case PBCMD_SET_RENDER_TARGET: {
        Dx9Device()->SetRenderTarget(dptr[1],
                                     (IDirect3DSurface *)FetchPtr(dptr + 2));
        dptr += 2 + N_DWORDS_IN_PTR;
        break;
      }

      case PBCMD_SET_PIXEL_SHADER: {
        Dx9Device()->SetPixelShader(
            (IDirect3DPixelShader9 *)FetchPtr(dptr + 1));
        dptr += 1 + N_DWORDS_IN_PTR;
        break;
      }

      case PBCMD_SET_INDICES: {
        Dx9Device()->SetIndices((IDirect3DIndexBuffer9 *)FetchPtr(dptr + 1));
        dptr += 1 + N_DWORDS_IN_PTR;
        break;
      }

      case PBCMD_SET_DEPTH_STENCIL_SURFACE: {
        Dx9Device()->SetDepthStencilSurface(
            (IDirect3DSurface9 *)FetchPtr(dptr + 1));
        dptr += 1 + N_DWORDS_IN_PTR;
        break;
      }

      case PBCMD_SETVIEWPORT: {
        Dx9Device()->SetViewport((D3DVIEWPORT9 const *)(dptr + 1));
        dptr += 1 + N_DWORDS(D3DVIEWPORT9);
        break;
      }

      case PBCMD_SET_VERTEX_SHADER: {
        Dx9Device()->SetVertexShader(
            (IDirect3DVertexShader9 *)FetchPtr(dptr + 1));
        dptr += 1 + N_DWORDS_IN_PTR;
        break;
      }

      case PBCMD_ASYNC_LOCK_VB: {
        HandleAsynchronousLockVBCommand(dptr);
        dptr += 1 + N_DWORDS_IN_PTR + 3;
        break;
      }

      case PBCMD_ASYNC_UNLOCK_VB: {
        HandleAsynchronousUnLockVBCommand(dptr);
        dptr += 1 + N_DWORDS_IN_PTR + N_DWORDS(LockedBufferContext) + 1;
        break;
      }

      case PBCMD_ASYNC_LOCK_IB: {
        HandleAsynchronousLockIBCommand(dptr);
        dptr += 1 + N_DWORDS_IN_PTR + 3;
        break;
      }

      case PBCMD_ASYNC_UNLOCK_IB: {
        HandleAsynchronousUnLockIBCommand(dptr);
        dptr += 1 + N_DWORDS_IN_PTR + N_DWORDS(LockedBufferContext) + 1;
        break;
      }

      case PBCMD_UNLOCK_VB: {
        IDirect3DVertexBuffer9 *p =
            (IDirect3DVertexBuffer9 *)FetchPtr(dptr + 1);
        p->Unlock();
        dptr += 1 + N_DWORDS_IN_PTR;
        break;
      }
      case PBCMD_UNLOCK_IB: {
        IDirect3DIndexBuffer9 *p = (IDirect3DIndexBuffer9 *)FetchPtr(dptr + 1);
        p->Unlock();
        dptr += 1 + N_DWORDS_IN_PTR;
        break;
      }
      case PBCMD_SET_VERTEX_SHADER_CONSTANT: {
        Dx9Device()->SetVertexShaderConstantF(dptr[1], (float const *)dptr + 3,
                                              dptr[2]);
        dptr += 3 + 4 * dptr[2];
        break;
      }
      case PBCMD_SET_BOOLEAN_VERTEX_SHADER_CONSTANT: {
        Dx9Device()->SetVertexShaderConstantB(dptr[1], (int const *)dptr + 3,
                                              dptr[2]);
        dptr += 3 + dptr[2];
        break;
      }

      case PBCMD_SET_INTEGER_VERTEX_SHADER_CONSTANT: {
        Dx9Device()->SetVertexShaderConstantI(dptr[1], (int const *)dptr + 3,
                                              dptr[2]);
        dptr += 3 + 4 * dptr[2];
        break;
      }

      case PBCMD_SET_PIXEL_SHADER_CONSTANT: {
        Dx9Device()->SetPixelShaderConstantF(dptr[1], (float const *)dptr + 3,
                                             dptr[2]);
        dptr += 3 + 4 * dptr[2];
        break;
      }
      case PBCMD_SET_BOOLEAN_PIXEL_SHADER_CONSTANT: {
        Dx9Device()->SetPixelShaderConstantB(dptr[1], (int const *)dptr + 3,
                                             dptr[2]);
        dptr += 3 + dptr[2];
        break;
      }

      case PBCMD_SET_INTEGER_PIXEL_SHADER_CONSTANT: {
        Dx9Device()->SetPixelShaderConstantI(dptr[1], (int const *)dptr + 3,
                                             dptr[2]);
        dptr += 3 + 4 * dptr[2];
        break;
      }

      case PBCMD_BEGIN_SCENE: {
        Dx9Device()->BeginScene();
        dptr++;
        break;
      }

      case PBCMD_END_SCENE: {
        Dx9Device()->EndScene();
        dptr++;
        break;
      }

      case PBCMD_CLEAR: {
        dptr++;
        int count = *(dptr++);
        D3DRECT const *pRects = 0;
        if (count) {
          pRects = (D3DRECT const *)dptr;
          dptr += count * N_DWORDS(D3DRECT);
        }
        int flags = *(dptr++);
        D3DCOLOR color = *((D3DCOLOR const *)(dptr++));
        float z = *((float const *)(dptr++));
        int stencil = *(dptr++);
        Dx9Device()->Clear(count, pRects, flags, color, z, stencil);
        break;
      }

      case PBCMD_SET_VERTEXDECLARATION: {
        Dx9Device()->SetVertexDeclaration(
            (IDirect3DVertexDeclaration9 *)FetchPtr(dptr + 1));
        dptr += 1 + N_DWORDS_IN_PTR;
        break;
      }

      case PBCMD_SETCLIPPLANE: {
        Dx9Device()->SetClipPlane(dptr[1], (float const *)dptr + 2);
        dptr += 6;
      } break;

      case PBCMD_STRETCHRECT: {
        dptr++;
        IDirect3DSurface9 *pSourceSurface = (IDirect3DSurface9 *)FetchPtr(dptr);
        dptr += N_DWORDS_IN_PTR;
        RECT const *pSourceRect = 0;
        if (*(dptr++)) pSourceRect = (RECT const *)dptr;
        dptr += N_DWORDS(RECT);
        IDirect3DSurface9 *pDestSurface = (IDirect3DSurface9 *)FetchPtr(dptr);
        dptr += N_DWORDS_IN_PTR;
        RECT const *pDestRect = 0;
        if (*(dptr++)) pDestRect = (RECT const *)dptr;
        dptr += N_DWORDS(RECT);
        D3DTEXTUREFILTERTYPE Filter = (D3DTEXTUREFILTERTYPE) * (dptr++);
        Dx9Device()->StretchRect(pSourceSurface, pSourceRect, pDestSurface,
                                 pDestRect, Filter);
      } break;

      case PBCMD_PRESENT: {
        dptr++;
        RECT const *pSourceRect = 0;
        if (*(dptr++)) pSourceRect = (RECT const *)dptr;
        dptr += N_DWORDS(RECT);
        RECT const *pDestRect = 0;
        if (*(dptr++)) pDestRect = (RECT const *)dptr;
        dptr += N_DWORDS(RECT);
        HWND hDestWindowOverride = (HWND) * (dptr++);
        RGNDATA const *pDirtyRegion = 0;
        if (*(dptr++)) pDirtyRegion = (RGNDATA const *)dptr;
        dptr += N_DWORDS(RGNDATA);
        Dx9Device()->Present(pSourceRect, pDestRect, hDestWindowOverride,
                             pDirtyRegion);
        break;
      }
    }
  }
}
