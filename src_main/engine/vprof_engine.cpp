// Copyright © 1996-2018, Valve Corporation, All rights reserved.
//
// Purpose: VProf engine integration

#include "vprof_engine.h"

#include "cmd.h"
#include "con_nprint.h"
#include "iengine.h"
#include "materialsystem/imaterialsystem.h"
#include "sv_main.h"
#include "sys.h"
#include "tier0/include/basetypes.h"
#include "tier0/include/vprof.h"
#include "tier1/convar.h"
#include "tier1/strtools.h"
#ifndef SWDS
#include "vgui_baseui_interface.h"
#include "vgui_vprofpanel.h"
#endif
#include "filesystem_engine.h"
#include "ivprofexport.h"
#include "sv_remoteaccess.h"
#include "tier1/UtlVector.h"
#include "tier1/utlstring.h"
#include "tier1/utlvector.h"
#include "vprof_record.h"

#include "tier0/include/memdbgon.h"

#ifdef VPROF_ENABLED
void VProfExport_StartOrStop();

static ConVar vprof_dump_spikes(
    "vprof_dump_spikes", "0", 0,
    "Framerate at which vprof will begin to dump spikes to the console. 0 = "
    "disabled, negative to reset after dump");
static ConVar vprof_dump_spikes_node(
    "vprof_dump_spikes_node", "", 0,
    "Node to start report from when doing a dump spikes");
static ConVar vprof_dump_spikes_budget_group(
    "vprof_dump_spikes_budget_group", "", 0,
    "Budget gtNode to start report from when doing a dump spikes");
static ConVar vprof_dump_oninterval(
    "vprof_dump_oninterval", "0", 0,
    "Interval (in seconds) at which vprof will batch up data and dump it to "
    "the console.");

static void (*g_pfnDeferredOp)();

static void ExecuteDeferredOp() {
  if (g_pfnDeferredOp) {
    (*g_pfnDeferredOp)();
    g_pfnDeferredOp = NULL;
  }
}

const double MAX_SPIKE_REPORT = 1.0;
const int MAX_SPIKE_REPORT_FRAMES = 10;
static double LastSpikeTime = 0;
static int LastSpikeFrame = 0;
bool g_VProfSignalSpike;  // used by xbox
static ConVar vprof_counters("vprof_counters", "0");

extern bool con_debuglog;
extern ConVar con_logfile;
static bool g_fVprofOnByUI;
static bool g_bVProfNoVSyncOff = false;

class ConsoleLogger {
 public:
  ConsoleLogger() {
#if !defined(SWDS)
    m_condebugEnabled = con_debuglog;
#else
    m_condebugEnabled = false;
#endif
    if (!m_condebugEnabled) {
      g_pFileSystem->CreateDirHierarchy("vprof");
      while (1) {
        ++m_index;
        const char *fname = va("vprof/vprof%d.txt", m_index);
        if (g_pFileSystem->FileExists(fname)) {
          continue;
        }

#if !defined(SWDS)
        con_logfile.SetValue(fname);
#endif
        break;
      }
    }
  }

  ~ConsoleLogger() {
    if (!m_condebugEnabled) {
#if !defined(SWDS)
      con_logfile.SetValue("");
#endif
    }
  }

 private:
  static int m_index;
  bool m_condebugEnabled;
};

int ConsoleLogger::m_index = 0;

static float s_flIntervalStartTime = 0.0f;

void PreUpdateProfile(float filteredtime) {
  Assert(g_VProfCurrentProfile.AtRoot());

  ExecuteDeferredOp();
  VProfExport_StartOrStop();
  VProfRecord_StartOrStop();

  // Check to see if it is time to dump the data and restart collection.
  if (g_VProfCurrentProfile.IsEnabled() &&
      (vprof_dump_oninterval.GetFloat() != 0.0f)) {
    float flCurrentTime = eng->GetCurTime();
    float flIntervalTime = vprof_dump_oninterval.GetFloat();

    g_VProfCurrentProfile.MarkFrame();

    if ((s_flIntervalStartTime + flIntervalTime) < flCurrentTime) {
      // Dump the current profile.
      g_VProfCurrentProfile.OutputReport(
          VPRT_SUMMARY | VPRT_LIST_BY_TIME | VPRT_LIST_BY_AVG_TIME |
          VPRT_LIST_BY_TIME_LESS_CHILDREN | VPRT_LIST_TOP_ITEMS_ONLY);

      // Stop the current profile.
      g_VProfCurrentProfile.Stop();

      // Reset and restart the current profile.
      g_VProfCurrentProfile.Reset();
      g_VProfCurrentProfile.Start();
      s_flIntervalStartTime = flCurrentTime;
    }
  }

  if (g_VProfCurrentProfile.IsEnabled() && vprof_dump_spikes.GetFloat()) {
    float spikeThreash = fabsf(vprof_dump_spikes.GetFloat());
    g_VProfCurrentProfile.MarkFrame();
    bool bSuppressRestart = false;
    if (g_VProfSignalSpike || eng->GetFrameTime() > (1.f / spikeThreash)) {
      if (g_VProfSignalSpike ||
          (Plat_FloatTime() - LastSpikeTime > MAX_SPIKE_REPORT &&
           g_ServerGlobalVariables.framecount >
               LastSpikeFrame + MAX_SPIKE_REPORT_FRAMES)) {
        ConsoleLogger consoleLog;
        g_VProfCurrentProfile.OutputReport(
            VPRT_SUMMARY | VPRT_LIST_BY_TIME | VPRT_LIST_BY_AVG_TIME |
                VPRT_LIST_BY_TIME_LESS_CHILDREN | VPRT_LIST_TOP_ITEMS_ONLY,
            (vprof_dump_spikes_node.GetString()[0])
                ? vprof_dump_spikes_node.GetString()
                : NULL,
            (vprof_dump_spikes_budget_group.GetString()[0])
                ? g_VProfCurrentProfile.BudgetGroupNameToBudgetGroupID(
                      vprof_dump_spikes_budget_group.GetString())
                : -1);

        LastSpikeTime = Plat_FloatTime();
        LastSpikeFrame = g_ServerGlobalVariables.framecount;

        if (vprof_dump_spikes.GetFloat() < 0.0) {
          vprof_dump_spikes.SetValue(0.0f);
          // g_VProfCurrentProfile.Stop();
          g_fVprofOnByUI = false;
          bSuppressRestart = true;
        }
      }
      g_VProfSignalSpike = false;
    }

    int iStartDepth = 0;
    do {
      g_VProfCurrentProfile.Stop();
      iStartDepth++;
    } while (g_VProfCurrentProfile.IsEnabled());

    if (!bSuppressRestart) {
      g_VProfCurrentProfile.Reset();

      while (iStartDepth--) {
        g_VProfCurrentProfile.Start();
      }
    }

    Assert(g_VProfCurrentProfile.AtRoot());
    Assert(g_VProfCurrentProfile.IsEnabled());
  }

  int nCounterType = vprof_counters.GetInt();
  if (nCounterType) {
    int i;
    int n = g_VProfCurrentProfile.GetNumCounters();
    int nprintIndex = 0;
    for (i = 0; i < n; i++) {
      if (g_VProfCurrentProfile.GetCounterGroup(i) != (nCounterType - 1))
        continue;
      const char *pName;
      int val;
      pName = g_VProfCurrentProfile.GetCounterNameAndValue(i, val);
      Con_NPrintf(nprintIndex, "%s = %d\n", pName, val);
      nprintIndex++;
    }
  }
  g_VProfCurrentProfile.ResetCounters(COUNTER_GROUP_DEFAULT);
  g_VProfCurrentProfile.ResetCounters(COUNTER_GROUP_TEXTURE_PER_FRAME);

  // This MUST come before GetVProfPanel()->UpdateProfile(), because
  // UpdateProfile uses the data we snapshot here.
  VProfExport_SnapshotVProfHistory();

#ifdef VPROF_ENABLED
  VProfRecord_Snapshot();
#endif

#ifndef SWDS
  // Update the vgui panel
  if (GetVProfPanel()) GetVProfPanel()->UpdateProfile(filteredtime);
#endif
}

void PostUpdateProfile() {
  if (g_VProfCurrentProfile.IsEnabled() && !vprof_dump_spikes.GetFloat() &&
      !vprof_dump_oninterval.GetFloat()) {
    g_VProfCurrentProfile.MarkFrame();
  }
}

static bool g_fVprofCacheMissOnByUI = false;
static char g_szDefferedArg1[128];

#define DEFERRED_CON_COMMAND(cmd, help)                             \
  static void cmd##_Impl();                                         \
  CON_COMMAND(cmd, help) {                                          \
    g_pfnDeferredOp = cmd##_Impl;                                   \
    Q_strncpy(g_szDefferedArg1, args[1], sizeof(g_szDefferedArg1)); \
  }                                                                 \
  static void cmd##_Impl()

CON_COMMAND_F(spike, "generates a fake spike", FCVAR_CHEAT) { Sys_Sleep(1000); }

CON_COMMAND(
    vprof_vtune_group,
    "enable vtune for a particular vprof group (\"disable\" to disable)") {
  if (args.ArgC() != 2) {
    Warning("vprof_vtune_group groupName (disable to turn off)\n");
    return;
  }
  const char *pArg = args[1];
  if (Q_stricmp(pArg, "disable") == 0) {
    g_VProfCurrentProfile.DisableVTuneGroup();
  } else {
    g_VProfCurrentProfile.EnableVTuneGroup(args[1]);
  }
}

CON_COMMAND(vprof_dump_groupnames,
            "Write the names of all of the vprof groups to the console.") {
  int n = g_VProfCurrentProfile.GetNumBudgetGroups();
  int i;
  for (i = 0; i < n; i++) {
    Msg("group %d: \"%s\"\n", i, g_VProfCurrentProfile.GetBudgetGroupName(i));
  }
}

DEFERRED_CON_COMMAND(vprof_cachemiss, "Toggle VProf cache miss checking") {
  if (!g_fVprofCacheMissOnByUI) {
    Msg("VProf cache miss enabled.\n");
    g_VProfCurrentProfile.PMEEnable(true);
    g_fVprofCacheMissOnByUI = true;
  } else {
    Msg("VProf cache miss disabled.\n");
    g_VProfCurrentProfile.PMEEnable(false);
    g_fVprofCacheMissOnByUI = false;
  }
}

DEFERRED_CON_COMMAND(vprof_cachemiss_on, "Turn on VProf cache miss checking") {
  if (!g_fVprofCacheMissOnByUI) {
    Msg("VProf cache miss enabled.\n");
    g_VProfCurrentProfile.PMEEnable(true);
    g_fVprofCacheMissOnByUI = true;
  }
}

DEFERRED_CON_COMMAND(vprof_cachemiss_off,
                     "Turn off VProf cache miss checking") {
  if (g_fVprofCacheMissOnByUI) {
    Msg("VProf cache miss disabled.\n");
    g_VProfCurrentProfile.PMEEnable(false);
    g_fVprofCacheMissOnByUI = false;
  }
}

DEFERRED_CON_COMMAND(vprof, "Toggle VProf profiler") {
  if (!g_fVprofOnByUI) {
    Msg("VProf enabled.\n");
    g_VProfCurrentProfile.Start();
    g_fVprofOnByUI = true;
  } else {
    Msg("VProf disabled.\n");
    g_VProfCurrentProfile.Stop();
    g_fVprofOnByUI = false;
  }
}

DEFERRED_CON_COMMAND(vprof_on, "Turn on VProf profiler") {
  if (!g_fVprofOnByUI) {
    Msg("VProf enabled.\n");
    g_VProfCurrentProfile.Start();
    g_fVprofOnByUI = true;
  }
}

CON_COMMAND(budget_toggle_group, "Turn a budget group on/off") {
  if (args.ArgC() != 2) {
    return;
  }

  int budgetGroup =
      g_VProfCurrentProfile.BudgetGroupNameToBudgetGroupIDNoCreate(args[1]);

  if (budgetGroup == -1) {
    return;
  }

  g_VProfCurrentProfile.HideBudgetGroup(
      budgetGroup, !(g_VProfCurrentProfile.GetBudgetGroupFlags(budgetGroup) &
                     BUDGETFLAG_HIDDEN));
}

void VProfOn() {
  CCommand args;
  vprof_on(args);
}

DEFERRED_CON_COMMAND(vprof_off, "Turn off VProf profiler") {
  if (g_fVprofOnByUI) {
    Msg("VProf disabled.\n");
    g_VProfCurrentProfile.Stop();
    g_fVprofOnByUI = false;
  }
}

DEFERRED_CON_COMMAND(vprof_reset, "Reset the stats in VProf profiler") {
  Msg("VProf reset.\n");
  g_VProfCurrentProfile.Reset();

#ifndef SWDS
  if (GetVProfPanel()) {
    GetVProfPanel()->Reset();
  }
#endif
}

DEFERRED_CON_COMMAND(vprof_reset_peaks,
                     "Reset just the peak time in VProf profiler") {
  Msg("VProf peaks reset.\n");
  g_VProfCurrentProfile.ResetPeaks();
}

DEFERRED_CON_COMMAND(vprof_generate_report,
                     "Generate a report to the console.") {
  g_VProfCurrentProfile.Pause();
  ConsoleLogger consoleLog;
  g_VProfCurrentProfile.OutputReport(
      VPRT_FULL & ~VPRT_HIERARCHY,
      (g_szDefferedArg1[0]) ? g_szDefferedArg1 : NULL);
  g_VProfCurrentProfile.Resume();
}

DEFERRED_CON_COMMAND(
    vprof_generate_report_budget,
    "Generate a report to the console based on budget group.") {
  if (!g_szDefferedArg1[0]) {
    return;
  }
  g_VProfCurrentProfile.Pause();
  ConsoleLogger consoleLog;
  g_VProfCurrentProfile.OutputReport(
      VPRT_FULL & ~VPRT_HIERARCHY, NULL,
      g_VProfCurrentProfile.BudgetGroupNameToBudgetGroupID(g_szDefferedArg1));
  g_VProfCurrentProfile.Resume();
}

DEFERRED_CON_COMMAND(vprof_generate_report_hierarchy,
                     "Generate a report to the console.") {
  g_VProfCurrentProfile.Pause();
  ConsoleLogger consoleLog;
  g_VProfCurrentProfile.OutputReport(VPRT_HIERARCHY);
  g_VProfCurrentProfile.Resume();
}

DEFERRED_CON_COMMAND(vprof_generate_report_AI,
                     "Generate a report to the console.") {
  // This is an unfortunate artifact of deferred commands not supporting
  // arguments
  g_VProfCurrentProfile.Pause();
  ConsoleLogger consoleLog;
  g_VProfCurrentProfile.OutputReport((VPRT_FULL & ~VPRT_HIERARCHY), "NPCs");
  g_VProfCurrentProfile.Resume();
}

DEFERRED_CON_COMMAND(vprof_generate_report_AI_only,
                     "Generate a report to the console.") {
  // This is an unfortunate artifact of deferred commands not supporting
  // arguments
  g_VProfCurrentProfile.Pause();
  ConsoleLogger consoleLog;
  g_VProfCurrentProfile.OutputReport(
      (VPRT_FULL & ~VPRT_HIERARCHY), "NPCs",
      g_VProfCurrentProfile.BudgetGroupNameToBudgetGroupID(
          VPROF_BUDGETGROUP_NPCS));
  g_VProfCurrentProfile.Resume();
}

DEFERRED_CON_COMMAND(vprof_generate_report_map_load,
                     "Generate a report to the console.") {
  // This is an unfortunate artifact of deferred commands not supporting
  // arguments
  g_VProfCurrentProfile.Pause();
  ConsoleLogger consoleLog;
  g_VProfCurrentProfile.OutputReport(VPRT_FULL, "Host_NewGame");
  g_VProfCurrentProfile.Resume();
}

// Exports for the dedicated server UI.
class CVProfExport : public IVProfExport {
 public:
  CVProfExport() {
    m_nListeners = 0;
    m_bStart = m_bStop = false;
    m_BudgetFlagsFilter = 0;
  }

  inline CVProfile *GetActiveVProfile() { return g_pVProfileForDisplay; }

  inline bool CanShowBudgetGroup(int iGroup) {
    return (GetActiveVProfile()->GetBudgetGroupFlags(iGroup) &
            m_BudgetFlagsFilter) != 0;
  }

  virtual void AddListener() {
    ++m_nListeners;
    if (m_nListeners == 1)
      m_bStart = true;  // Defer the command till vprof is ready.
  }

  virtual void RemoveListener() {
    --m_nListeners;
    if (m_nListeners == 0)
      m_bStop = true;  // Defer the command till vprof is ready.
  }

  virtual void SetBudgetFlagsFilter(int filter) {
    m_BudgetFlagsFilter = filter;
  }

  virtual int GetNumBudgetGroups() {
    int nTotalGroups =
        std::min(m_Times.Count(), GetActiveVProfile()->GetNumBudgetGroups());
    int nRet = 0;
    for (int i = 0; i < nTotalGroups; i++) {
      if (CanShowBudgetGroup(i)) ++nRet;
    }
    return nRet;
  }

  virtual void GetBudgetGroupInfos(CExportedBudgetGroupInfo *pInfos) {
    int iOut = 0;
    int nTotalGroups =
        std::min(m_Times.Count(), GetActiveVProfile()->GetNumBudgetGroups());
    for (int i = 0; i < nTotalGroups; i++) {
      if (CanShowBudgetGroup(i)) {
        pInfos[iOut].m_pName = GetActiveVProfile()->GetBudgetGroupName(i);

        int red, green, blue, alpha;
        GetActiveVProfile()->GetBudgetGroupColor(i, red, green, blue, alpha);
        pInfos[iOut].m_Color = Color(red, green, blue, alpha);

        pInfos[iOut].m_BudgetFlags =
            GetActiveVProfile()->GetBudgetGroupFlags(i);
        ++iOut;
      }
    }
  }

  virtual void GetBudgetGroupTimes(
      float times[IVProfExport::MAX_BUDGETGROUP_TIMES]) {
    int nTotalGroups =
        std::min(m_Times.Count(), GetActiveVProfile()->GetNumBudgetGroups());
    int nGroups =
        std::min(nTotalGroups, (int)IVProfExport::MAX_BUDGETGROUP_TIMES);
    memset(times, 0, sizeof(times[0]) * nGroups);

    int iOut = 0;
    for (int i = 0; i < nTotalGroups; i++) {
      if (CanShowBudgetGroup(i)) {
        times[iOut] = m_Times[i];
        ++iOut;
      }
    }
  }

  void GetAllBudgetGroupTimes(float *pTimes) {
    int nTotalGroups = GetActiveVProfile()->GetNumBudgetGroups();
    for (int i = 0; i < nTotalGroups; i++) {
      pTimes[i] = CanShowBudgetGroup(i) ? m_Times[i] : 0.0f;
    }
  }

  virtual void PauseProfile() {
    if (materials) materials->Flush();

    g_VProfCurrentProfile.Pause();
  }

  virtual void ResumeProfile() {
    if (materials) materials->Flush();

    g_VProfCurrentProfile.Resume();
  }

 public:
  void StartOrStop() {
    if (m_bStart) {
      g_VProfCurrentProfile.Start();
      m_bStart = false;
    }

    if (m_bStop) {
      g_VProfCurrentProfile.Stop();
      m_bStop = false;
    }
  }

  void CalculateBudgetGroupTimes_Recursive(CVProfNode *pNode) {
    // If this node's info is filtered out, then put it in its parent's budget
    // group.
    CVProfNode *pTestNode = pNode;
    while (pTestNode != GetActiveVProfile()->GetRoot() &&
           (!CanShowBudgetGroup(pTestNode->GetBudgetGroupID()) ||
            (GetActiveVProfile()->GetBudgetGroupFlags(
                 pTestNode->GetBudgetGroupID()) &
             BUDGETFLAG_HIDDEN) != 0)) {
      pTestNode = pTestNode->GetParent();
    }

    int groupID = pTestNode->GetBudgetGroupID();
    double nodeTime = pNode->GetPrevTimeLessChildren();
    if (groupID >= 0 &&
        groupID < std::min(m_Times.Count(),
                           (int)IVProfExport::MAX_BUDGETGROUP_TIMES)) {
      m_Times[groupID] += nodeTime;
    } else {
      Assert(false);
    }

    if (pNode->GetSibling()) {
      CalculateBudgetGroupTimes_Recursive(pNode->GetSibling());
    }
    if (pNode->GetChild()) {
      CalculateBudgetGroupTimes_Recursive(pNode->GetChild());
    }

    if (!VProfRecord_IsPlayingBack()) {
      pNode->ClearPrevTime();
    }
  }

  void SnapshotVProfHistory() {
    // Don't do the work if there are no listeners.
    if (!GetActiveVProfile()->IsEnabled()) return;

    if (m_Times.Count() < GetActiveVProfile()->GetNumBudgetGroups()) {
      m_Times.SetSize(GetActiveVProfile()->GetNumBudgetGroups());
    }

    memset(m_Times.Base(), 0,
           sizeof(m_Times[0]) * GetActiveVProfile()->GetNumBudgetGroups());
    CVProfNode *pNode = GetActiveVProfile()->GetRoot();
    if (pNode && pNode->GetChild()) {
      CalculateBudgetGroupTimes_Recursive(pNode->GetChild());
    }
  }

 private:
  CUtlVector<float> m_Times;  // Times from the most recent snapshot.
  int m_nListeners;
  int m_BudgetFlagsFilter;  // We can only capture one type of filtered data at
                            // a time.
  bool m_bStart;
  bool m_bStop;
};

CVProfExport g_VProfExport;
IVProfExport *g_pVProfExport = &g_VProfExport;

EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CVProfExport, IVProfExport,
                                  VPROF_EXPORT_INTERFACE_VERSION,
                                  g_VProfExport);

void VProfExport_SnapshotVProfHistory() {
  g_VProfExport.SnapshotVProfHistory();
}

void VProfExport_StartOrStop() { g_VProfExport.StartOrStop(); }

// Used by rpt
void VProfExport_Pause() { g_VProfExport.PauseProfile(); }

void VProfExport_Resume() { g_VProfExport.ResumeProfile(); }

// Used to point the budget panel at remote data
void OverrideVProfExport(IVProfExport *pExport) {
  if (g_pVProfExport == &g_VProfExport) {
    g_pVProfExport = pExport;
  }
}

void ResetVProfExport(IVProfExport *pExport) {
  if (g_pVProfExport == pExport) {
    g_pVProfExport = &g_VProfExport;
  }
}

// Listener to vprof data
struct VProfListenInfo_t {
  ra_listener_id m_nListenerId;
  float m_flLastSentVProfDataTime;
  CUtlVector<CUtlString> m_SentGroups;

  VProfListenInfo_t() : m_flLastSentVProfDataTime(0.0f) {}
  VProfListenInfo_t(ra_listener_id nListenerId)
      : m_nListenerId(nListenerId), m_flLastSentVProfDataTime(0.0f) {}
  bool operator==(const VProfListenInfo_t &src) const {
    return src.m_nListenerId == m_nListenerId;
  }

 private:
  VProfListenInfo_t(const VProfListenInfo_t &src);
};

static CUtlVector<VProfListenInfo_t> s_VProfListeners;

// Purpose: serialize and send data to remote listeners
static int FindSentGroupIndex(VProfListenInfo_t &info, const char *pGroupName) {
  int nCount = info.m_SentGroups.Count();
  for (int i = 0; i < nCount; ++i) {
    if (!strcmp(pGroupName, info.m_SentGroups[i].Get())) return i;
  }
  return -1;
}

// Purpose: serialize and send data to remote listeners
void WriteRemoteVProfGroupData(VProfListenInfo_t &info) {
  int nGroupCount = g_pVProfileForDisplay->GetNumBudgetGroups();
  int nInitialCount = info.m_SentGroups.Count();

  // Build list of unsent groups to send
  int nSendCount = 0;
  int *pIndex = stack_alloc<int>(nGroupCount);
  for (int i = 0; i < nGroupCount; ++i) {
    const char *pName = g_pVProfileForDisplay->GetBudgetGroupName(i);
    if (FindSentGroupIndex(info, pName) >= 0) continue;
    int j = info.m_SentGroups.AddToTail();
    info.m_SentGroups[j] = pName;
    pIndex[nSendCount++] = i;
  }

  if (nSendCount == 0) return;

  CUtlBuffer buf(1024, 1024);
  buf.PutInt(nInitialCount);
  buf.PutInt(nSendCount);

  for (int i = 0; i < nSendCount; i++) {
    int nIndex = pIndex[i];
    int red, green, blue, alpha;
    g_pVProfileForDisplay->GetBudgetGroupColor(nIndex, red, green, blue, alpha);
    buf.PutUnsignedChar((unsigned char)red);
    buf.PutUnsignedChar((unsigned char)green);
    buf.PutUnsignedChar((unsigned char)blue);
    buf.PutUnsignedChar((unsigned char)alpha);

    const char *pName = g_pVProfileForDisplay->GetBudgetGroupName(nIndex);
    buf.PutString(pName);
  }

  g_ServerRemoteAccess.SendVProfData(info.m_nListenerId, true, buf.Base(),
                                     buf.TellMaxPut());
}

static ConVar rpt_vprof_time("rpt_vprof_time", "0.25",
                             FCVAR_HIDDEN | FCVAR_DONTRECORD, "");
void WriteRemoteVProfData() {
  // Throttle sending too much data
  float flMaxDelta = rpt_vprof_time.GetFloat();
  float flTime = Plat_FloatTime();
  bool bShouldSend = false;
  int nListenerCount = s_VProfListeners.Count();
  for (int i = 0; i < nListenerCount; i++) {
    if (flTime - s_VProfListeners[i].m_flLastSentVProfDataTime >= flMaxDelta) {
      bShouldSend = true;
      break;
    }
  }

  if (!bShouldSend) return;

  int nGroupCount = g_pVProfileForDisplay->GetNumBudgetGroups();
  float *pTimes = stack_alloc<float>(nGroupCount);
  g_VProfExport.GetAllBudgetGroupTimes(pTimes);

  for (int i = 0; i < nListenerCount; i++) {
    if (flTime - s_VProfListeners[i].m_flLastSentVProfDataTime < flMaxDelta)
      continue;

    WriteRemoteVProfGroupData(s_VProfListeners[i]);
    s_VProfListeners[i].m_flLastSentVProfDataTime = flTime;

    // Re-order send times to match send group order
    usize nSentSize = s_VProfListeners[i].m_SentGroups.Count() * sizeof(float);
    float *pSentTimes = stack_alloc<float>(nSentSize / sizeof(float));
    memset(pSentTimes, 0, nSentSize);
    for (int j = 0; j < nGroupCount; ++j) {
      int nIndex = FindSentGroupIndex(
          s_VProfListeners[i], g_pVProfileForDisplay->GetBudgetGroupName(j));
      Assert(nIndex >= 0);
      pSentTimes[nIndex] = pTimes[j];
    }
    g_ServerRemoteAccess.SendVProfData(s_VProfListeners[i].m_nListenerId, false,
                                       pSentTimes, nSentSize);
  }
}

// Purpose: add a new endpoint to send data to
void RegisterVProfDataListener(ra_listener_id listenerID) {
  RemoveVProfDataListener(listenerID);
  int nIndex = s_VProfListeners.AddToTail();
  s_VProfListeners[nIndex].m_nListenerId = listenerID;
  g_VProfExport.AddListener();
  WriteRemoteVProfGroupData(s_VProfListeners[nIndex]);
}

// Purpose: remove an endpoint we are sending data to
void RemoveVProfDataListener(ra_listener_id listenerID) {
  VProfListenInfo_t findInfo(listenerID);
  if (s_VProfListeners.FindAndRemove(findInfo)) {
    g_VProfExport.RemoveListener();
  }
}

#endif
