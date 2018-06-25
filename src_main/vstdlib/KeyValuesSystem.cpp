// Copyright © 1996-2018, Valve Corporation, All rights reserved.

#include "vstdlib/IKeyValuesSystem.h"

#include "tier0/include/threadtools.h"
#include "tier1/keyvalues.h"
#include "tier1/mempool.h"
#include "tier1/memstack.h"
#include "tier1/utlsymbol.h"

#include "tier0/include/memdbgon.h"

#ifdef NO_SBH  // no need to pool if using tier0 small block heap
#define KEYVALUES_USE_POOL 1
#endif

// Central storage point for KeyValues memory and symbols.
class CKeyValuesSystem : public IKeyValuesSystem {
 public:
  CKeyValuesSystem();
  ~CKeyValuesSystem();

  // registers the size of the KeyValues in the specified instance
  // so it can build a properly sized memory pool for the KeyValues objects
  // the sizes will usually never differ but this is for versioning safety
  void RegisterSizeofKeyValues(usize size) override;

  // allocates/frees a KeyValues object from the shared mempool
  void *AllocKeyValuesMemory(usize size) override;
  void FreeKeyValuesMemory(void *pMem) override;

  // symbol table access (used for key names)
  HKeySymbol GetSymbolForString(const char *name, bool bCreate) override;
  const char *GetStringForSymbol(HKeySymbol symbol) override;

  // returns the wide version of ansi, also does the lookup on #'d strings
  void GetLocalizedFromANSI(const char *ansi, wchar_t *outBuf,
                            int unicodeBufferSizeInBytes);
  void GetANSIFromLocalized(const wchar_t *wchar, char *outBuf,
                            int ansiBufferSizeInBytes);

  // for debugging, adds KeyValues record into global list so we can track
  // memory leaks
  void AddKeyValuesToMemoryLeakList(void *pMem, HKeySymbol name) override;
  void RemoveKeyValuesFromMemoryLeakList(void *pMem) override;

 private:
#ifdef KEYVALUES_USE_POOL
  CMemoryPool *m_pMemPool;
#endif
  usize m_iMaxKeyValuesSize;

  // string hash table
  CMemoryStack m_Strings;
  struct hash_item_t {
    int stringIndex;
    hash_item_t *next;
  };
  CMemoryPool m_HashItemMemPool;
  CUtlVector<hash_item_t> m_HashTable;
  int CaseInsensitiveHash(const char *string, int iBounds);

  struct MemoryLeakTracker_t {
    int nameIndex;
    void *pMem;
  };
  static bool MemoryLeakTrackerLessFunc(const MemoryLeakTracker_t &lhs,
                                        const MemoryLeakTracker_t &rhs) {
    return lhs.pMem < rhs.pMem;
  }
  CUtlRBTree<MemoryLeakTracker_t, int> m_KeyValuesTrackingList;

  CThreadFastMutex m_mutex;
};

// Instance singleton and expose interface to rest of code.
IKeyValuesSystem *KeyValuesSystem() {
  static CKeyValuesSystem keyValuesSystem;
  return &keyValuesSystem;
}

CKeyValuesSystem::CKeyValuesSystem()
    : m_HashItemMemPool(sizeof(hash_item_t), 64, CMemoryPool::GROW_FAST,
                        "CKeyValuesSystem::m_HashItemMemPool"),
      m_KeyValuesTrackingList(0, 0, MemoryLeakTrackerLessFunc) {
  // initialize hash table
  m_HashTable.AddMultipleToTail(2047);
  for (int i = 0; i < m_HashTable.Count(); i++) {
    m_HashTable[i].stringIndex = 0;
    m_HashTable[i].next = nullptr;
  }

  m_Strings.Init(4 * 1024 * 1024, 64 * 1024, 0, 4);
  char *pszEmpty = ((char *)m_Strings.Alloc(1));
  *pszEmpty = 0;

#ifdef KEYVALUES_USE_POOL
  m_pMemPool = nullptr;
#endif
  m_iMaxKeyValuesSize = sizeof(KeyValues);
}

CKeyValuesSystem::~CKeyValuesSystem() {
#ifdef KEYVALUES_USE_POOL
#ifdef _DEBUG
  // display any memory leaks
  if (m_pMemPool && m_pMemPool->Count() > 0) {
    DevMsg("Leaked KeyValues blocks: %d\n", m_pMemPool->Count());
  }

  // iterate all the existing keyvalues displaying their names
  for (int i = 0; i < m_KeyValuesTrackingList.MaxElement(); i++) {
    if (m_KeyValuesTrackingList.IsValidIndex(i)) {
      DevMsg("\tleaked KeyValues(%s)\n",
             &m_Strings[m_KeyValuesTrackingList[i].nameIndex]);
    }
  }
#endif

  delete m_pMemPool;
#endif
}

// Registers the size of the KeyValues in the specified instance so it can build
// a properly sized memory pool for the KeyValues objects the sizes will usually
// never differ but this is for versioning safety.
void CKeyValuesSystem::RegisterSizeofKeyValues(usize size) {
  if (size > m_iMaxKeyValuesSize) {
    m_iMaxKeyValuesSize = size;
  }
}

#ifdef KEYVALUES_USE_POOL
static void KVLeak(char const *fmt, ...) {
  va_list argptr;
  char data[1024];

  va_start(argptr, fmt);
  Q_vsnprintf(data, sizeof(data), fmt, argptr);
  va_end(argptr);

  Msg(data);
}
#endif

// Allocates a KeyValues object from the shared mempool.
void *CKeyValuesSystem::AllocKeyValuesMemory(usize size) {
#ifdef KEYVALUES_USE_POOL
  // allocate, if we don't have one yet
  if (!m_pMemPool) {
    m_pMemPool =
        new CMemoryPool(m_iMaxKeyValuesSize, 1024, CMemoryPool::GROW_FAST,
                        "CKeyValuesSystem::m_pMemPool");
    m_pMemPool->SetErrorReportFunc(KVLeak);
  }

  return m_pMemPool->Alloc(size);
#else
  return malloc(size);
#endif
}

// Frees a KeyValues object from the shared mempool.
void CKeyValuesSystem::FreeKeyValuesMemory(void *pMem) {
#ifdef KEYVALUES_USE_POOL
  m_pMemPool->Free(pMem);
#else
  heap_free(pMem);
#endif
}

// Symbol table access (used for key names).
HKeySymbol CKeyValuesSystem::GetSymbolForString(const char *name,
                                                bool bCreate) {
  if (!name) return -1;

  size_t size{strlen(name) + 1};

  AUTO_LOCK(m_mutex);

  int hash = CaseInsensitiveHash(name, m_HashTable.Count());
  int i = 0;
  hash_item_t *item = &m_HashTable[hash];

  while (true) {
    if (!_stricmp(name, (char *)m_Strings.GetBase() + item->stringIndex)) {
      return (HKeySymbol)item->stringIndex;
    }

    i++;

    if (item->next == nullptr) {
      // not found
      if (!bCreate) return -1;

      // we're not in the table
      if (item->stringIndex != 0) {
        // first item is used, an new item
        item->next =
            (hash_item_t *)m_HashItemMemPool.Alloc(sizeof(hash_item_t));
        item = item->next;
      }

      // build up the new item
      item->next = nullptr;

      char *the_string = (char *)m_Strings.Alloc(size);  //-V814
      if (!the_string) {
        Error("Out of keyvalue string space");
        return -1;
      }

      item->stringIndex = the_string - (char *)m_Strings.GetBase();
      strcpy_s(the_string, size, name);

      return (HKeySymbol)item->stringIndex;
    }

    item = item->next;
  }

  // shouldn't be able to get here
  Assert(0);
  return -1;
}

// Symbol table access.
const char *CKeyValuesSystem::GetStringForSymbol(HKeySymbol symbol) {
  if (symbol == -1) {
    return "";
  }
  return ((char *)m_Strings.GetBase() + (size_t)symbol);
}

// Adds KeyValues record into global list so we can track memory leaks.
void CKeyValuesSystem::AddKeyValuesToMemoryLeakList(void *pMem,
                                                    HKeySymbol name) {
#ifdef _DEBUG
  // only track the memory leaks in debug builds
  MemoryLeakTracker_t item = {name, pMem};
  m_KeyValuesTrackingList.Insert(item);
#endif
}

// Used to track memory leaks.
void CKeyValuesSystem::RemoveKeyValuesFromMemoryLeakList(void *pMem) {
#ifdef _DEBUG
  // only track the memory leaks in debug builds
  MemoryLeakTracker_t item = {0, pMem};
  int index = m_KeyValuesTrackingList.Find(item);
  m_KeyValuesTrackingList.RemoveAt(index);
#endif
}

// Generates a simple hash value for a string.
int CKeyValuesSystem::CaseInsensitiveHash(const char *string, int iBounds) {
  unsigned int hash = 0;

  for (; *string != 0; string++) {
    if (*string >= 'A' && *string <= 'Z') {
      hash = (hash << 1) + (*string - 'A' + 'a');
    } else {
      hash = (hash << 1) + *string;
    }
  }

  return hash % iBounds;
}
