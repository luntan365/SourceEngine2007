// Copyright � 1996-2018, Valve Corporation, All rights reserved.

#ifndef IMATERIALVAR_H
#define IMATERIALVAR_H

#include "mathlib/vector4d.h"
#include "tier0/include/platform.h"
#include "tier1/utlsymbol.h"

class IMaterial;
class VMatrix;
class ITexture;

#define MAKE_MATERIALVAR_FOURCC(ch0, ch1, ch2, ch3)     \
  ((unsigned long)(ch0) | ((unsigned long)(ch1) << 8) | \
   ((unsigned long)(ch2) << 16) | ((unsigned long)(ch3) << 24))

// This fourcc is reserved.
#define FOURCC_UNKNOWN MAKE_MATERIALVAR_FOURCC('U', 'N', 'K', 'N')

//-----------------------------------------------------------------------------
// Various material var types
//-----------------------------------------------------------------------------
enum MaterialVarType_t {
  MATERIAL_VAR_TYPE_FLOAT = 0,
  MATERIAL_VAR_TYPE_STRING,
  MATERIAL_VAR_TYPE_VECTOR,
  MATERIAL_VAR_TYPE_TEXTURE,
  MATERIAL_VAR_TYPE_INT,
  MATERIAL_VAR_TYPE_FOURCC,
  MATERIAL_VAR_TYPE_UNDEFINED,
  MATERIAL_VAR_TYPE_MATRIX,
  MATERIAL_VAR_TYPE_MATERIAL,
};

typedef u16 MaterialVarSym_t;

class IMaterialVar {
 public:
  typedef unsigned long FourCC;

 protected:
  // base data and accessors
  char* m_pStringVal;
  int m_intVal;
  Vector4D m_VecVal;

  // member data. total = 4 bytes
  u8 m_Type : 4;
  u8 m_nNumVectorComps : 3;
  u8 m_bFakeMaterialVar : 1;
  u8 m_nTempIndex;
  CUtlSymbol m_Name;

 public:
  // class factory methods
  static IMaterialVar* Create(IMaterial* pMaterial, char const* pKey,
                              VMatrix const& matrix);
  static IMaterialVar* Create(IMaterial* pMaterial, char const* pKey,
                              char const* pVal);
  static IMaterialVar* Create(IMaterial* pMaterial, char const* pKey,
                              f32* pVal, int numcomps);
  static IMaterialVar* Create(IMaterial* pMaterial, char const* pKey,
                              f32 val);
  static IMaterialVar* Create(IMaterial* pMaterial, char const* pKey, int val);
  static IMaterialVar* Create(IMaterial* pMaterial, char const* pKey);
  static void Destroy(IMaterialVar* pVar);
  static MaterialVarSym_t GetSymbol(char const* pName);
  static MaterialVarSym_t FindSymbol(char const* pName);
  static bool SymbolMatches(char const* pName, MaterialVarSym_t symbol);
  static void DeleteUnreferencedTextures(bool enable);

  virtual ITexture* GetTextureValue(void) = 0;

  virtual char const* GetName(void) const = 0;
  virtual MaterialVarSym_t GetNameAsSymbol() const = 0;

  virtual void SetFloatValue(f32 val) = 0;

  virtual void SetIntValue(int val) = 0;

  virtual void SetStringValue(char const* val) = 0;
  virtual char const* GetStringValue(void) const = 0;

  // Use FourCC values to pass app-defined data structures between
  // the proxy and the shader. The shader should ignore the data if
  // its FourCC type not correct.
  virtual void SetFourCCValue(FourCC type, void* pData) = 0;
  virtual void GetFourCCValue(FourCC* type, void** ppData) = 0;

  // Vec (dim 2-4)
  virtual void SetVecValue(f32 const* val, int numcomps) = 0;
  virtual void SetVecValue(f32 x, f32 y) = 0;
  virtual void SetVecValue(f32 x, f32 y, f32 z) = 0;
  virtual void SetVecValue(f32 x, f32 y, f32 z, f32 w) = 0;
  virtual void GetLinearVecValue(f32* val, int numcomps) const = 0;

  // revisit: is this a good interface for textures?
  virtual void SetTextureValue(ITexture*) = 0;

  virtual IMaterial* GetMaterialValue(void) = 0;
  virtual void SetMaterialValue(IMaterial*) = 0;

  virtual bool IsDefined() const = 0;
  virtual void SetUndefined() = 0;

  // Matrix
  virtual void SetMatrixValue(VMatrix const& matrix) = 0;
  virtual const VMatrix& GetMatrixValue() = 0;
  virtual bool MatrixIsIdentity() const = 0;

  // Copy....
  virtual void CopyFrom(IMaterialVar* pMaterialVar) = 0;

  virtual void SetValueAutodetectType(char const* val) = 0;

  virtual IMaterial* GetOwningMaterial() = 0;

  // set just 1 component
  virtual void SetVecComponentValue(f32 fVal, int nComponent) = 0;

 protected:
  virtual int GetIntValueInternal(void) const = 0;
  virtual f32 GetFloatValueInternal(void) const = 0;
  virtual f32 const* GetVecValueInternal() const = 0;
  virtual void GetVecValueInternal(f32* val, int numcomps) const = 0;
  virtual int VectorSizeInternal() const = 0;

 public:
  SOURCE_FORCEINLINE MaterialVarType_t GetType() const {
    return (MaterialVarType_t)m_Type;
  }

  SOURCE_FORCEINLINE bool IsTexture() const {
    return m_Type == MATERIAL_VAR_TYPE_TEXTURE;
  }

  SOURCE_FORCEINLINE operator ITexture*() { return GetTextureValue(); }

  // NOTE: Fast methods should only be called in thread-safe situations
  SOURCE_FORCEINLINE int GetIntValueFast() const {
    // Set methods for f32 and vector update this
    return m_intVal;
  }

  SOURCE_FORCEINLINE f32 GetFloatValueFast() const { return m_VecVal[0]; }

  SOURCE_FORCEINLINE f32 const* GetVecValueFast() const { return m_VecVal.Base(); }

  SOURCE_FORCEINLINE void GetVecValueFast(f32* val, int numcomps) const {
    Assert((numcomps > 0) && (numcomps <= 4));
    for (int i = 0; i < numcomps; i++) {
      val[i] = m_VecVal[i];
    }
  }

  SOURCE_FORCEINLINE int VectorSizeFast() const { return m_nNumVectorComps; }

#ifdef FAST_MATERIALVAR_ACCESS
  SOURCE_FORCEINLINE int GetIntValue() const { return GetIntValueFast(); }

  SOURCE_FORCEINLINE f32 GetFloatValue() const { return GetFloatValueFast(); }

  SOURCE_FORCEINLINE f32 const* GetVecValue() const { return GetVecValueFast(); }

  SOURCE_FORCEINLINE void GetVecValue(f32* val, int numcomps) const {
    GetVecValueFast(val, numcomps);
  }

  SOURCE_FORCEINLINE int VectorSize() const { return VectorSizeFast(); }
#else  // !FAST_MATERIALVAR_ACCESS
  SOURCE_FORCEINLINE int GetIntValue() const { return GetIntValueInternal(); }

  SOURCE_FORCEINLINE f32 GetFloatValue() const {
    return GetFloatValueInternal();
  }

  SOURCE_FORCEINLINE f32 const* GetVecValue() const { return GetVecValueInternal(); }

  SOURCE_FORCEINLINE void GetVecValue(f32* val, int numcomps) const {
    return GetVecValueInternal(val, numcomps);
  }

  SOURCE_FORCEINLINE int VectorSize() const { return VectorSizeInternal(); }
#endif

 private:
  SOURCE_FORCEINLINE void SetTempIndex(int nIndex) { m_nTempIndex = nIndex; }

  friend void EnableThreadedMaterialVarAccess(bool bEnable,
                                              IMaterialVar** ppParams,
                                              int nVarCount);
};

#endif  // IMATERIALVAR_H
