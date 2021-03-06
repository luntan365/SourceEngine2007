// Copyright © 1996-2018, Valve Corporation, All rights reserved.

#include "iresource_listing_writer.h"

#include "filesystem.h"
#include "tier0/include/icommandline.h"
#include "tier1/KeyValues.h"
#include "tier1/characterset.h"
#include "tier1/fmtstr.h"
#include "tier1/utlbuffer.h"
#include "tier1/utlrbtree.h"
#include "tier1/utlstring.h"
#include "tier1/utlvector.h"

#include "tier0/include/memdbgon.h"

bool SaveResList(const CUtlRBTree<CUtlString, int> &list, ch const *pchFileName,
                 ch const *pchSearchPath) {
  FileHandle_t fh = g_pFullFileSystem->Open(pchFileName, "wt", pchSearchPath);
  if (fh != FILESYSTEM_INVALID_HANDLE) {
    for (int i = list.FirstInorder(); i != list.InvalidIndex();
         i = list.NextInorder(i)) {
      g_pFullFileSystem->Write(list[i].String(), strlen(list[i].String()), fh);
      g_pFullFileSystem->Write("\n", 1, fh);
    }

    g_pFullFileSystem->Close(fh);
    return true;
  }
  return false;
}

void LoadResList(CUtlRBTree<CUtlString, int> &list, ch const *pchFileName,
                 ch const *pchSearchPath) {
  CUtlBuffer buffer(0, 0, CUtlBuffer::TEXT_BUFFER);
  if (!g_pFullFileSystem->ReadFile(pchFileName, pchSearchPath, buffer)) {
    // does not exist
    return;
  }

  characterset_t breakSet;
  CharacterSetBuild(&breakSet, "");

  // parse reslist
  ch szToken[SOURCE_MAX_PATH];
  for (;;) {
    int nTokenSize = buffer.ParseToken(&breakSet, szToken, sizeof(szToken));
    if (nTokenSize <= 0) {
      break;
    }

    Q_strlower(szToken);
    Q_FixSlashes(szToken);

    // Ensure filename has "quotes" around it
    CUtlString s;
    if (szToken[0] == '\"') {
      Assert(strlen(szToken) > 2);
      Assert(szToken[strlen(szToken) - 1] == '\"');
      s = szToken;
    } else {
      s = CFmtStr("\"%s\"", szToken);
    }

    int idx = list.Find(s);
    if (idx == list.InvalidIndex()) {
      list.Insert(s);
    }
  }
}

static bool ReslistLogLessFunc(CUtlString const &pLHS, CUtlString const &pRHS) {
  return CaselessStringLessThan(pLHS.Get(), pRHS.Get());
}

void SortResList(ch const *pchFileName, ch const *pchSearchPath) {
  CUtlRBTree<CUtlString, int> sorted(0, 0, ReslistLogLessFunc);
  LoadResList(sorted, pchFileName, pchSearchPath);

  // Now write it back out
  SaveResList(sorted, pchFileName, pchSearchPath);
}

void MergeResLists(CUtlVector<CUtlString> &fileNames, ch const *pchOutputFile,
                   ch const *pchSearchPath) {
  CUtlRBTree<CUtlString, int> sorted(0, 0, ReslistLogLessFunc);
  for (int i = 0; i < fileNames.Count(); ++i) {
    LoadResList(sorted, fileNames[i].String(), pchSearchPath);
  }

  // Now write it back out
  SaveResList(sorted, pchOutputFile, pchSearchPath);
}

struct WorkItem {
  WorkItem() {}

  CUtlString SubDir;
  CUtlString AddCommands;
};

class ResourceListingWriter : public IResourceListingWriter {
 public:
  enum {
    STATE_BUILDINGRESLISTS = 0,
    STATE_GENERATINGCACHES,
  };

  ResourceListingWriter();

  void Init(ch const *pchBaseDir, ch const *pchGameDir) override;
  bool IsActive() override;
  void Shutdown() override;

  void SetupCommandLine() override;
  bool ShouldContinue() override;

  virtual void Collate();

 private:
  bool InitCommandFile(ch const *pchGameDir, ch const *pchCommandFile);
  void LoadMapList(ch const *pchGameDir, CUtlVector<CUtlString> &vecMaps,
                   ch const *pchMapFile);
  void CollateFiles(ch const *pchResListFilename);

  bool m_bInitialized;
  bool m_bActive;
  CUtlString m_sBaseDir;
  CUtlString m_sGameDir;
  CUtlString m_sFullGamePath;

  CUtlString m_sFinalDir;
  CUtlString m_sWorkingDir;
  CUtlString m_sBaseCommandLine;
  CUtlString m_sOriginalCommandLine;
  CUtlString m_sInitialStartMap;

  int m_nCurrentWorkItem;
  CUtlVector<WorkItem> m_WorkItems;

  CUtlVector<CUtlString> m_MapList;
  int m_nCurrentState;
};

IResourceListingWriter *ResourceListing() {
  static ResourceListingWriter resource_listing_writer;
  return &resource_listing_writer;
}

ResourceListingWriter::ResourceListingWriter()
    : m_bInitialized(false),
      m_bActive(false),
      m_nCurrentWorkItem(0),
      m_nCurrentState(STATE_BUILDINGRESLISTS) {
  MEM_ALLOC_CREDIT();

  m_sFinalDir = "reslists";
  m_sWorkingDir = "reslists_work";
}

void ResourceListingWriter::CollateFiles(ch const *pchResListFilename) {
  CUtlVector<CUtlString> vecReslists;

  for (int i = 0; i < m_WorkItems.Count(); ++i) {
    ch fn[SOURCE_MAX_PATH];
    sprintf_s(fn, "%s\\%s\\%s\\%s", m_sFullGamePath.String(),
              m_sWorkingDir.String(), m_WorkItems[i].SubDir.String(),
              pchResListFilename);
    vecReslists.AddToTail(fn);
  }

  MergeResLists(vecReslists,
                CFmtStr("%s\\%s\\%s", m_sFullGamePath.String(),
                        m_sFinalDir.String(), pchResListFilename),
                "GAME");
}

void ResourceListingWriter::Init(ch const *pchBaseDir, ch const *pchGameDir) {
  // Because we have to call this inside the first Apps "PreInit", we need only
  // Init on the very first call
  if (m_bInitialized) {
    return;
  }

  m_bInitialized = true;

  m_sBaseDir = pchBaseDir;
  m_sGameDir = pchGameDir;

  ch path[SOURCE_MAX_PATH];
  sprintf_s(path, "%s/%s", m_sBaseDir.String(), m_sGameDir.String());
  Q_FixSlashes(path);
  Q_strlower(path);
  m_sFullGamePath = path;

  const ch *pchCommandFile = nullptr;
  if (CommandLine()->CheckParm("-makereslists", &pchCommandFile) &&
      pchCommandFile) {
    // base path setup, now can get and parse command file
    InitCommandFile(path, pchCommandFile);
  }
}

void ResourceListingWriter::Shutdown() {
  if (!m_bActive) return;
}

bool ResourceListingWriter::IsActive() { return m_bInitialized && m_bActive; }

void ResourceListingWriter::Collate() {
  ch szDir[SOURCE_MAX_PATH];
  sprintf_s(szDir, "%s\\%s", m_sFullGamePath.String(), m_sFinalDir.String());
  g_pFullFileSystem->CreateDirHierarchy(szDir, "GAME");

  // Now create the collated/merged data
  CollateFiles("all.lst");
  CollateFiles("engine.lst");
  for (int i = 0; i < m_MapList.Count(); ++i) {
    CollateFiles(CFmtStr("%s.lst", m_MapList[i].String()));
  }
}

void ResourceListingWriter::SetupCommandLine() {
  if (!m_bActive) return;

  switch (m_nCurrentState) {
    case STATE_BUILDINGRESLISTS: {
      Assert(m_nCurrentWorkItem < m_WorkItems.Count());

      const WorkItem &work = m_WorkItems[m_nCurrentWorkItem];

      // Clean the working dir
      ch szWorkingDir[SOURCE_MAX_PATH];
      sprintf_s(szWorkingDir, "%s\\%s", m_sWorkingDir.String(),
                work.SubDir.String());

      ch szFullWorkingDir[SOURCE_MAX_PATH];
      sprintf_s(szFullWorkingDir, "%s\\%s", m_sFullGamePath.String(),
                szWorkingDir);
      g_pFullFileSystem->CreateDirHierarchy(szFullWorkingDir, "GAME");

      // Preserve startmap
      ch const *pszStartMap = nullptr;
      CommandLine()->CheckParm("-startmap", &pszStartMap);
      ch szMap[SOURCE_MAX_PATH] = {0};
      if (pszStartMap) {
        strcpy_s(szMap, pszStartMap);
      }

      // Prepare stuff
      // Reset command line based on current state
      ch szCmd[512];
      sprintf_s(szCmd, "%s %s %s -reslistdir %s",
                m_sOriginalCommandLine.String(), m_sBaseCommandLine.String(),
                work.AddCommands.String(), szWorkingDir);

      Warning("Reslists:  Setting command line:\n'%s'\n", szCmd);

      CommandLine()->CreateCmdLine(szCmd);
      // Never rebuild caches by default, inly do it in STATE_GENERATINGCACHES
      CommandLine()->AppendParm("-norebuildaudio", nullptr);
      if (szMap[0]) {
        CommandLine()->AppendParm("-startmap", szMap);
      }
    } break;
    case STATE_GENERATINGCACHES: {
      Collate();

      // Prepare stuff
      // Reset command line based on current state
      ch szCmd[512];
      sprintf_s(szCmd, "%s -reslistdir %s -rebuildaudio",
                m_sOriginalCommandLine.String(), m_sFinalDir.String());

      Warning("Caches:  Setting command line:\n'%s'\n", szCmd);

      CommandLine()->CreateCmdLine(szCmd);

      CommandLine()->RemoveParm("-norebuildaudio");
      CommandLine()->RemoveParm("-makereslists");

      ++m_nCurrentState;
    } break;
  }
}

bool ResourceListingWriter::ShouldContinue() {
  if (!m_bActive) return false;

  bool bContinueAdvancing = false;
  do {
    switch (m_nCurrentState) {
      default:
        break;
      case STATE_BUILDINGRESLISTS: {
        CommandLine()->RemoveParm("-startmap");

        // Advance to next time
        ++m_nCurrentWorkItem;

        if (m_nCurrentWorkItem >= m_WorkItems.Count()) {
          // Will stay in the loop
          ++m_nCurrentState;
          bContinueAdvancing = true;
        } else {
          return true;
        }
      } break;
      case STATE_GENERATINGCACHES: {
        return true;
      } break;
    }
  } while (bContinueAdvancing);

  return false;
}

void ResourceListingWriter::LoadMapList(ch const *pchGameDir,
                                        CUtlVector<CUtlString> &vecMaps,
                                        ch const *pchMapFile) {
  ch fullpath[SOURCE_MAX_PATH];
  sprintf_s(fullpath, "%s/%s", pchGameDir, pchMapFile);

  // Load them in
  CUtlBuffer buf(0, 0, CUtlBuffer::TEXT_BUFFER);

  if (g_pFullFileSystem->ReadFile(fullpath, "GAME", buf)) {
    ch szMap[SOURCE_MAX_PATH];
    while (true) {
      buf.GetLine(szMap, sizeof(szMap));
      if (!szMap[0]) break;

      // Strip trailing CR/LF chars
      usize len = strlen(szMap);
      while (len >= 1 && szMap[len - 1] == '\n' || szMap[len - 1] == '\r') {
        szMap[len - 1] = '\0';
        len = strlen(szMap);
      }

      CUtlString newMap = szMap;
      vecMaps.AddToTail(newMap);
    }
  } else {
    Error("Unable to maplist file %s\n", fullpath);
  }
}

bool ResourceListingWriter::InitCommandFile(ch const *pchGameDir,
                                            ch const *pchCommandFile) {
  if (*pchCommandFile == '+' || *pchCommandFile == '-') {
    Msg("falling back to legacy reslists system\n");
    return false;
  }

  ch fullpath[SOURCE_MAX_PATH];
  sprintf_s(fullpath, "%s/%s", pchGameDir, pchCommandFile);

  CUtlBuffer buf;
  if (!g_pFullFileSystem->ReadFile(fullpath, "GAME", buf)) {
    Error("Unable to load '%s'\n", fullpath);
    return false;
  }

  KeyValues *kv = new KeyValues("reslists");
  if (!kv->LoadFromBuffer("reslists", (const ch *)buf.Base())) {
    Error("Unable to parse keyvalues from '%s'\n", fullpath);
    kv->deleteThis();
    return false;  //-V773
  }

  CUtlString sMapListFile = kv->GetString("maplist", "maplist.txt");
  LoadMapList(pchGameDir, m_MapList, sMapListFile);
  if (m_MapList.Count() <= 0) {
    Error("Maplist file '%s' empty or missing!!!\n", sMapListFile.String());
    kv->deleteThis();
    return false;
  }

  ch const *pszSolo = nullptr;
  if (CommandLine()->CheckParm("+map", &pszSolo) && pszSolo) {
    m_MapList.Purge();

    CUtlString newMap;
    newMap = pszSolo;
    m_MapList.AddToTail(newMap);
  }

  m_nCurrentWorkItem = CommandLine()->ParmValue("-startstage", 0);

  ch const *pszStartMap = nullptr;
  CommandLine()->CheckParm("-startmap", &pszStartMap);
  if (pszStartMap) {
    m_sInitialStartMap = pszStartMap;
  }

  CommandLine()->RemoveParm("-startstage");
  CommandLine()->RemoveParm("-makereslists");
  CommandLine()->RemoveParm("-reslistdir");
  CommandLine()->RemoveParm("-norebuildaudio");
  CommandLine()->RemoveParm("-startmap");

  m_sOriginalCommandLine = CommandLine()->GetCmdLine();

  // Add it back in for first map
  if (pszStartMap) {
    CommandLine()->AppendParm("-startmap", m_sInitialStartMap.String());
  }

  m_sBaseCommandLine = kv->GetString("basecommandline", "");
  m_sFinalDir = kv->GetString("finaldir", m_sFinalDir.String());
  m_sWorkingDir = kv->GetString("workdir", m_sWorkingDir.String());

  int i = 0;
  do {
    ch sz[32];
    sprintf_s(sz, "%i", i);
    KeyValues *subKey = kv->FindKey(sz, false);
    if (!subKey) break;

    WorkItem work;

    work.SubDir = subKey->GetString("subdir", "");
    work.AddCommands = subKey->GetString("addcommands", "");

    if (work.SubDir.Length() > 0) {
      m_WorkItems.AddToTail(work);
    } else {
      Error("%s: failed to specify 'subdir' for item %s\n", fullpath, sz);
    }

    ++i;
  } while (true);

  m_bActive = m_WorkItems.Count() > 0;
  m_nCurrentWorkItem =
      std::clamp(m_nCurrentWorkItem, 0, m_WorkItems.Count() - 1);

  bool bCollate = CommandLine()->CheckParm("-collate") ? true : false;
  if (bCollate) {
    Collate();
    m_bActive = false;
    exit(-1);
  }

  kv->deleteThis();

  return m_bActive;
}
