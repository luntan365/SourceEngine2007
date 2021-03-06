// Copyright © 1996-2018, Valve Corporation, All rights reserved.

#include "net_ws_headers.h"

#include "base/include/windows/scoped_winsock_initializer.h"
#include "net_ws_queued_packet_sender.h"
#include "tier1/lzss.h"

#include "tier0/include/memdbgon.h"

#define NET_COMPRESSION_STACKBUF_SIZE 4096

static ConVar net_showsplits("net_showsplits", "0", 0,
                             "Show info about packet splits");

static ConVar ipname("ip", "localhost", 0, "Overrides IP for multihomed hosts");
static ConVar hostport("hostport", va("%i", PORT_SERVER), 0,
                       "Host game server port");
static ConVar hostip("hostip", "", 0, "Host game server ip");
static ConVar clientport("clientport", va("%i", PORT_CLIENT), 0,
                         "Host game client port");
static ConVar hltvport("tv_port", va("%i", PORT_HLTV), 0, "Host SourceTV port");
static ConVar matchmakingport("matchmakingport", va("%i", PORT_MATCHMAKING), 0,
                              "Host Matchmaking port");
static ConVar systemlinkport("systemlinkport", va("%i", PORT_SYSTEMLINK), 0,
                             "System Link port");

static ConVar fakelag("net_fakelag", "0", FCVAR_CHEAT,
                      "Lag all incoming network data (including loopback) by "
                      "this many milliseconds.");
static ConVar fakeloss(
    "net_fakeloss", "0", FCVAR_CHEAT,
    "Simulate packet loss as a percentage (negative means drop 1/n packets)");
static ConVar droppackets("net_droppackets", "0", FCVAR_CHEAT,
                          "Drops next n packets on client");
static ConVar fakejitter("net_fakejitter", "0", FCVAR_CHEAT,
                         "Jitter fakelag packet time");

static ConVar net_compressvoice(
    "net_compressvoice", "0", 0,
    "Attempt to compress out of band voice payloads (360 only).");
ConVar net_usesocketsforloopback("net_usesocketsforloopback", "0", 0,
                                 "Use network sockets layer even for listen "
                                 "server local player's packets (multiplayer "
                                 "only).");

#ifdef _DEBUG
static ConVar fakenoise(
    "net_fakenoise", "0", FCVAR_CHEAT,
    "Simulate corrupt network packets (changes n bits per packet randomly)");
static ConVar fakeshuffle(
    "net_fakeshuffle", "0", FCVAR_CHEAT,
    "Shuffles order of every nth packet (needs net_fakelag)");
static ConVar recvpackets("net_recvpackets", "-1", FCVAR_CHEAT,
                          "Receive exactly next n packets if >= 0");
static ConVar net_savelargesplits("net_savelargesplits", "-1", 0,
                                  "If not -1, then if a split has this many or "
                                  "more split parts, save the entire packet to "
                                  "disc for analysis.");
#endif

//-----------------------------------------------------------------------------
// Toggle Xbox 360 network security to allow cross-platform testing
//-----------------------------------------------------------------------------
#define X360SecureNetwork() false
#define IPPROTO_VDP IPPROTO_UDP

extern ConVar net_showudp;
extern ConVar net_showtcp;
extern ConVar net_blocksize;
extern ConVar host_timescale;
extern int host_framecount;

void NET_ClearQueuedPacketsForChannel(INetChannel *chan);

#define DEF_LOOPBACK_SIZE 2048

struct netsocket_t {
  int nPort;        // UDP/TCP use same port number
  bool bListening;  // true if TCP port is listening
  int hUDP;         // handle to UDP socket from socket()
  int hTCP;         // handle to TCP socket from socket()
};

struct pendingsocket_t {
  int newsock;  // handle of new socket
  int netsock;  // handle of listen socket
  float time;
  netadr_t addr;
};

#include "tier0/include/memdbgoff.h"

struct loopback_t {
  char *data;   // loopback buffer
  int datalen;  // current data length
  char defbuffer[DEF_LOOPBACK_SIZE];

  DECLARE_FIXEDSIZE_ALLOCATOR(loopback_t);
};

#include "tier0/include/memdbgon.h"

DEFINE_FIXEDSIZE_ALLOCATOR(loopback_t, 2, CMemoryPool::GROW_SLOW);

// Split long packets.  Anything over 1460 is failing on some routers
struct LONGPACKET {
  int currentSequence;
  int splitCount;
  int totalSize;
  int nExpectedSplitSize;
  // This has to be big enough to hold the largest message
  char buffer[NET_MAX_MESSAGE];
};

// Use this to pick apart the network stream, must be packed
#pragma pack(1)
struct SPLITPACKET {
  int netID;
  int sequenceNumber;
  int packetID : 16;
  int nSplitSize : 16;
};
#pragma pack()

#define MIN_USER_MAXROUTABLE_SIZE 576  // ( X.25 Networks )
#define MAX_USER_MAXROUTABLE_SIZE MAX_ROUTABLE_PAYLOAD

#define MAX_SPLIT_SIZE (MAX_USER_MAXROUTABLE_SIZE - sizeof(SPLITPACKET))
#define MIN_SPLIT_SIZE (MIN_USER_MAXROUTABLE_SIZE - sizeof(SPLITPACKET))

static ConVar sv_maxroutable(
    "sv_maxroutable", "1260", 0,
    "Server upper bound on net_maxroutable that a client can use.", true,
    MIN_USER_MAXROUTABLE_SIZE, true, MAX_USER_MAXROUTABLE_SIZE);

ConVar net_maxroutable("net_maxroutable", "1260",
                       FCVAR_ARCHIVE | FCVAR_USERINFO,
                       "Requested max packet size before packets are 'split'.",
                       true, MIN_USER_MAXROUTABLE_SIZE, true,
                       MAX_USER_MAXROUTABLE_SIZE);

netadr_t net_local_adr;
double net_time = 0.0f;  // current time, updated each frame

static CUtlVector<netsocket_t>
    net_sockets;  // the 4 sockets, Server, Client, HLTV, Matchmaking
static CUtlVector<netpacket_t> net_packets;

static source::windows::ScopedWinsockInitializer *scoped_winsock_initializer =
    nullptr;
static bool net_multiplayer = false;  // if true, configured for Multiplayer
static bool net_noip = false;    // Disable IP support, can't switch to MP mode
static bool net_nodns = false;   // Disable DNS request to avoid long timeouts
static bool net_notcp = true;    // Disable TCP support
static bool net_nohltv = false;  // disable HLTV support
static bool net_dedicated = false;  // true is dedicated system
static int net_error = 0;  // global error code updated with NET_GetLastError()

static CUtlVectorMT<CUtlVector<CNetChan *> > s_NetChannels;
static CUtlVectorMT<CUtlVector<pendingsocket_t> > s_PendingSockets;

CTSQueue<loopback_t *> s_LoopBacks[LOOPBACK_SOCKETS];
static netpacket_t
    *s_pLagData[MAX_SOCKETS];  // List of lag structures, if fakelag is set.

unsigned short NET_HostToNetShort(unsigned short us_in) { return htons(us_in); }

unsigned short NET_NetToHostShort(unsigned short us_in) { return ntohs(us_in); }

// This macro is used to capture the return value of a function call while
// recording a VCR file. During playback, it will get the return value out of
// the VCR file instead of actually calling the function.
#if !defined(NO_VCR)
#define VCR_NONPLAYBACKFN(call, resultVar, eventName)          \
  {                                                            \
    if (VCRGetMode() != VCR_Playback) resultVar = call;        \
                                                               \
    VCRGenericValue(eventName, &resultVar, sizeof(resultVar)); \
  }
#else
#define VCR_NONPLAYBACKFN(call, resultVar, eventName)   \
  {                                                     \
    if (VCRGetMode() != VCR_Playback) resultVar = call; \
  }
#endif

#define CASE_ERROR(error) \
  case error:             \
    return #error

const char *NET_ErrorString(int code) {
#if defined(_WIN32)
  switch (code) {
    CASE_ERROR(WSAEINTR);
    CASE_ERROR(WSAEBADF);
    CASE_ERROR(WSAEACCES);

    case WSAEDISCON:
      return "WSAEDISCON";
    case WSAEFAULT:
      return "WSAEFAULT";
    case WSAEINVAL:
      return "WSAEINVAL";
    case WSAEMFILE:
      return "WSAEMFILE";
    case WSAEWOULDBLOCK:
      return "WSAEWOULDBLOCK";
    case WSAEINPROGRESS:
      return "WSAEINPROGRESS";
    case WSAEALREADY:
      return "WSAEALREADY";
    case WSAENOTSOCK:
      return "WSAENOTSOCK";
    case WSAEDESTADDRREQ:
      return "WSAEDESTADDRREQ";
    case WSAEMSGSIZE:
      return "WSAEMSGSIZE";
    case WSAEPROTOTYPE:
      return "WSAEPROTOTYPE";
    case WSAENOPROTOOPT:
      return "WSAENOPROTOOPT";
    case WSAEPROTONOSUPPORT:
      return "WSAEPROTONOSUPPORT";
    case WSAESOCKTNOSUPPORT:
      return "WSAESOCKTNOSUPPORT";
    case WSAEOPNOTSUPP:
      return "WSAEOPNOTSUPP";
    case WSAEPFNOSUPPORT:
      return "WSAEPFNOSUPPORT";
    case WSAEAFNOSUPPORT:
      return "WSAEAFNOSUPPORT";
    case WSAEADDRINUSE:
      return "WSAEADDRINUSE";
    case WSAEADDRNOTAVAIL:
      return "WSAEADDRNOTAVAIL";
    case WSAENETDOWN:
      return "WSAENETDOWN";
    case WSAENETUNREACH:
      return "WSAENETUNREACH";
    case WSAENETRESET:
      return "WSAENETRESET";
    case WSAECONNABORTED:
      return "WSWSAECONNABORTEDAEINTR";
    case WSAECONNRESET:
      return "WSAECONNRESET";
    case WSAENOBUFS:
      return "WSAENOBUFS";
    case WSAEISCONN:
      return "WSAEISCONN";
    case WSAENOTCONN:
      return "WSAENOTCONN";
    case WSAESHUTDOWN:
      return "WSAESHUTDOWN";
    case WSAETOOMANYREFS:
      return "WSAETOOMANYREFS";
    case WSAETIMEDOUT:
      return "WSAETIMEDOUT";
    case WSAECONNREFUSED:
      return "WSAECONNREFUSED";
    case WSAELOOP:
      return "WSAELOOP";
    case WSAENAMETOOLONG:
      return "WSAENAMETOOLONG";
    case WSAEHOSTDOWN:
      return "WSAEHOSTDOWN";
    case WSASYSNOTREADY:
      return "WSASYSNOTREADY";
    case WSAVERNOTSUPPORTED:
      return "WSAVERNOTSUPPORTED";
    case WSANOTINITIALISED:
      return "WSANOTINITIALISED";
    case WSAHOST_NOT_FOUND:
      return "WSAHOST_NOT_FOUND";
    case WSATRY_AGAIN:
      return "WSATRY_AGAIN";
    case WSANO_RECOVERY:
      return "WSANO_RECOVERY";
    case WSANO_DATA:
      return "WSANO_DATA";
    default:
      return "UNKNOWN ERROR";
  }
#else
  return strerror(code);
#endif
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  : *s -
//			*sadr -
// Output : bool	NET_StringToSockaddr
//-----------------------------------------------------------------------------
bool NET_StringToSockaddr(const char *s, struct sockaddr *sadr) {
  char *colon;
  char copy[128];

  Q_memset(sadr, 0, sizeof(*sadr));
  ((struct sockaddr_in *)sadr)->sin_family = AF_INET;
  ((struct sockaddr_in *)sadr)->sin_port = 0;

  Q_strncpy(copy, s, sizeof(copy));
  // strip off a trailing :port if present
  for (colon = copy; *colon; colon++) {
    if (*colon == ':') {
      *colon = 0;
      ((struct sockaddr_in *)sadr)->sin_port =
          NET_HostToNetShort((short)atoi(colon + 1));
    }
  }

  if (copy[0] >= '0' && copy[0] <= '9' && Q_strstr(copy, ".")) {
    *(int *)&((struct sockaddr_in *)sadr)->sin_addr = inet_addr(copy);
  } else {
    if (net_nodns) return false;  // DNS names disabled

    struct hostent *h;
    if ((h = gethostbyname(copy)) == nullptr) return false;
    *(int *)&((struct sockaddr_in *)sadr)->sin_addr = *(int *)h->h_addr_list[0];
  }

  return true;
}

int NET_GetLastError() {
#if defined(_WIN32)
  net_error = WSAGetLastError();
#else
  net_error = errno;
#endif
#if !defined(NO_VCR)
  VCRGenericValue("WSAGetLastError", &net_error, sizeof(net_error));
#endif
  return net_error;
}

/*
==================
NET_ClearLaggedList

==================
*/
void NET_ClearLaggedList(netpacket_t **pList) {
  netpacket_t *p = (*pList);

  while (p) {
    netpacket_t *n = p->pNext;

    if (p->data) {
      delete[] p->data;
      p->data = nullptr;
    }
    delete p;
    p = n;
  }

  (*pList) = nullptr;
}

void NET_ClearLagData(int sock) {
  if (sock < MAX_SOCKETS && s_pLagData[sock]) {
    NET_ClearLaggedList(&s_pLagData[sock]);
  }
}

/*
=============
NET_StringToAdr

localhost
idnewt
idnewt:28000
192.246.40.70
192.246.40.70:28000
=============
*/
bool NET_StringToAdr(const char *s, netadr_t *a) {
  struct sockaddr saddr;

  char address[128];

  Q_strncpy(address, s, sizeof(address));

  if (!Q_strncmp(address, "localhost", 10) ||
      !Q_strncmp(address, "localhost:", 10)) {
    // subsitute 'localhost' with '127.0.0.1", both have 9 chars
    // this way we can resolve 'localhost' without DNS and still keep the port
    Q_memcpy(address, "127.0.0.1", 9);
  }

  if (!NET_StringToSockaddr(address, &saddr)) return false;

  a->SetFromSockadr(&saddr);

  return true;
}

CNetChan *NET_FindNetChannel(int socket, netadr_t &adr) {
  AUTO_LOCK_FM(s_NetChannels);

  int numChannels = s_NetChannels.Count();

  for (int i = 0; i < numChannels; i++) {
    CNetChan *chan = s_NetChannels[i];

    // sockets must match
    if (socket != chan->GetSocket()) continue;

    // and the IP:Port address
    if (adr.CompareAdr(chan->GetRemoteAddress())) {
      return chan;  // found it
    }
  }

  return nullptr;  // no channel found
}

void NET_CloseSocket(int hSocket, int sock = -1) {
  if (!hSocket) return;

  // close socket handle
  int ret;
  VCR_NONPLAYBACKFN(closesocket(hSocket), ret, "closesocket");
  if (ret == -1) {
    NET_GetLastError();
    ConMsg("WARNING! NET_CloseSocket: %s\n", NET_ErrorString(net_error));
  }

  // if hSocket mapped to hTCP, clear hTCP
  if (sock >= 0) {
    if (net_sockets[sock].hTCP == hSocket) {
      net_sockets[sock].hTCP = 0;
      net_sockets[sock].bListening = false;
    }
  }
}

/*
====================
NET_IPSocket
====================
*/
int NET_OpenSocket(const char *net_interface, int &port, int protocol) {
  struct sockaddr_in address;
  unsigned int opt;
  int newsocket = -1;

  if (protocol == IPPROTO_TCP) {
    VCR_NONPLAYBACKFN(socket(PF_INET, SOCK_STREAM, IPPROTO_TCP), newsocket,
                      "socket()");
  } else  // as UDP or VDP
  {
    VCR_NONPLAYBACKFN(socket(PF_INET, SOCK_DGRAM, protocol), newsocket,
                      "socket()");
  }

  if (newsocket == -1) {
    NET_GetLastError();
    if (net_error != WSAEAFNOSUPPORT)
      Msg("WARNING: NET_OpenSockett: socket failed: %s",
          NET_ErrorString(net_error));

    return 0;
  }

  opt = 1;  // make it non-blocking
  int ret;
  VCR_NONPLAYBACKFN(ioctlsocket(newsocket, FIONBIO, (unsigned long *)&opt), ret,
                    "ioctlsocket");
  if (ret == -1) {
    NET_GetLastError();
    Msg("WARNING: NET_OpenSocket: ioctl FIONBIO: %s\n",
        NET_ErrorString(net_error));
  }

  if (protocol == IPPROTO_TCP) {
    if (!IsX360())  // SO_KEEPALIVE unsupported on the 360
    {
      opt = 1;  // set TCP options: keep TCP connection alive
      VCR_NONPLAYBACKFN(setsockopt(newsocket, SOL_SOCKET, SO_KEEPALIVE,
                                   (char *)&opt, sizeof(opt)),
                        ret, "setsockopt");
      if (ret == -1) {
        NET_GetLastError();
        Msg("WARNING: NET_OpenSocket: setsockopt SO_KEEPALIVE: %s\n",
            NET_ErrorString(net_error));
        return 0;
      }
    }

    linger optlinger;  // set TCP options: Does not block close waiting for
                       // unsent data to be sent
    optlinger.l_linger = 0;
    optlinger.l_onoff = 0;
    VCR_NONPLAYBACKFN(setsockopt(newsocket, SOL_SOCKET, SO_LINGER,
                                 (char *)&optlinger, sizeof(optlinger)),
                      ret, "setsockopt");
    if (ret == -1) {
      NET_GetLastError();
      Msg("WARNING: NET_OpenSocket: setsockopt SO_LINGER: %s\n",
          NET_ErrorString(net_error));
      return 0;
    }

    opt = 1;  // set TCP options: Disables the Nagle algorithm for send
              // coalescing.
    VCR_NONPLAYBACKFN(setsockopt(newsocket, IPPROTO_TCP, TCP_NODELAY,
                                 (char *)&opt, sizeof(opt)),
                      ret, "setsockopt");
    if (ret == -1) {
      NET_GetLastError();
      Msg("WARNING: NET_OpenSocket: setsockopt TCP_NODELAY: %s\n",
          NET_ErrorString(net_error));
      return 0;
    }

    opt = NET_MAX_MESSAGE;  // set TCP options: set send buffer size
    VCR_NONPLAYBACKFN(
        setsockopt(newsocket, SOL_SOCKET, SO_SNDBUF, (char *)&opt, sizeof(opt)),
        ret, "setsockopt");
    if (ret == -1) {
      NET_GetLastError();
      Msg("WARNING: NET_OpenSocket: setsockopt SO_SNDBUF: %s\n",
          NET_ErrorString(net_error));
      return 0;
    }

    opt = NET_MAX_MESSAGE;  // set TCP options: set receive buffer size
    VCR_NONPLAYBACKFN(
        setsockopt(newsocket, SOL_SOCKET, SO_RCVBUF, (char *)&opt, sizeof(opt)),
        ret, "setsockopt");
    if (ret == -1) {
      NET_GetLastError();
      Msg("WARNING: NET_OpenSocket: setsockopt SO_RCVBUF: %s\n",
          NET_ErrorString(net_error));
      return 0;
    }

    return newsocket;  // don't bind TCP sockets by default
  }

  // rest is UDP only

  // VPD protocol (Xbox 360 secure network) doesn't support SO_BROADCAST
  opt = 1;  // set UDP options: make it broadcast capable
  VCR_NONPLAYBACKFN(setsockopt(newsocket, SOL_SOCKET, SO_BROADCAST,
                               (char *)&opt, sizeof(opt)),
                    ret, "setsockopt");
  if (ret == -1) {
    NET_GetLastError();
    Msg("WARNING: NET_OpenSocket: setsockopt SO_BROADCAST: %s\n",
        NET_ErrorString(net_error));
    return 0;
  }

  if (CommandLine()->FindParm("-reuse")) {
    opt = 1;  // make it reusable
    VCR_NONPLAYBACKFN(setsockopt(newsocket, SOL_SOCKET, SO_REUSEADDR,
                                 (char *)&opt, sizeof(opt)),
                      ret, "setsockopt");
    if (ret == -1) {
      NET_GetLastError();
      Msg("WARNING: NET_OpenSocket: setsockopt SO_REUSEADDR: %s\n",
          NET_ErrorString(net_error));
      return 0;
    }
  }

  if (!net_interface || !net_interface[0] ||
      !Q_strcmp(net_interface, "localhost")) {
    address.sin_addr.s_addr = INADDR_ANY;
  } else {
    NET_StringToSockaddr(net_interface, (struct sockaddr *)&address);
  }

  address.sin_family = AF_INET;

  int port_offset;  // try binding socket to port, try next 10 is port is
                    // already used

  for (port_offset = 0; port_offset < PORT_TRY_MAX; port_offset++) {
    if (port == PORT_ANY) {
      address.sin_port = 0;  // = INADDR_ANY
    } else {
      address.sin_port = NET_HostToNetShort((short)(port + port_offset));
    }

    VCR_NONPLAYBACKFN(
        bind(newsocket, (struct sockaddr *)&address, sizeof(address)), ret,
        "bind");
    if (ret != -1) {
      if (port != PORT_ANY && port_offset != 0) {
        port += port_offset;  // update port
        ConDMsg(
            "Socket bound to non-default port %i because original port was "
            "already in use.\n",
            port);
      }
      break;
    }

    NET_GetLastError();

    if (port == PORT_ANY || net_error != WSAEADDRINUSE) {
      Msg("WARNING: NNET_OpenSocket: bind: %s\n", NET_ErrorString(net_error));
      NET_CloseSocket(newsocket, -1);
      return 0;
    }

    // Try next port
  }

  if (port_offset == PORT_TRY_MAX) {
    Msg("WARNING: UDP_OpenSocket: unable to bind socket\n");
    NET_CloseSocket(newsocket, -1);
    return 0;
  }

  return newsocket;
}

int NET_ConnectSocket(int sock, const netadr_t &addr) {
  Assert((sock >= 0) && (sock < net_sockets.Count()));

  netsocket_t *netsock = &net_sockets[sock];

  if (netsock->hTCP) {
    NET_CloseSocket(netsock->hTCP, sock);
  }

  if (net_notcp) return 0;

  sockaddr saddr;

  addr.ToSockadr(&saddr);

  int anyport = PORT_ANY;

  netsock->hTCP = NET_OpenSocket(ipname.GetString(), anyport, true);

  if (!netsock->hTCP) {
    Msg("Warning! NET_ConnectSocket failed opening socket %i, port %i.\n", sock,
        net_sockets[sock].nPort);
    return false;
  }

  int ret;
  VCR_NONPLAYBACKFN(connect(netsock->hTCP, &saddr, sizeof(saddr)), ret,
                    "connect");
  if (ret == -1) {
    NET_GetLastError();

    if (net_error != WSAEWOULDBLOCK) {
      Msg("NET_ConnectSocket: %s\n", NET_ErrorString(net_error));
      return 0;
    }
  }

  return net_sockets[sock].hTCP;
}

int NET_SendStream(int nSock, const char *buf, int len, int flags) {
  // int ret = send( nSock, buf, len, flags );
  int ret = VCRHook_send(nSock, buf, len, flags);
  if (ret == -1) {
    NET_GetLastError();

    if (net_error == WSAEWOULDBLOCK) {
      return 0;  // ignore EWOULDBLOCK
    }

    Msg("NET_SendStream: %s\n", NET_ErrorString(net_error));
  }

  return ret;
}

int NET_ReceiveStream(int nSock, char *buf, int len, int flags) {
  int ret = VCRHook_recv(nSock, buf, len, flags);
  if (ret == -1) {
    NET_GetLastError();

    if (net_error == WSAEWOULDBLOCK || net_error == WSAENOTCONN) {
      return 0;  // ignore EWOULDBLOCK
    }

    Msg("NET_ReceiveStream: %s\n", NET_ErrorString(net_error));
  }

  return ret;
}

INetChannel *NET_CreateNetChannel(int socket, netadr_t *adr, const char *name,
                                  INetChannelHandler *handler,
                                  bool bForceNewChannel) {
  CNetChan *chan = nullptr;

  if (!bForceNewChannel && adr != nullptr) {
    // try to find real network channel if already existing
    if ((chan = NET_FindNetChannel(socket, *adr)) != nullptr) {
      // channel already known, clear any old stuff before Setup wipes all
      chan->Clear();
    }
  }

  if (!chan) {
    // create new channel
    chan = new CNetChan();

    AUTO_LOCK_FM(s_NetChannels);
    s_NetChannels.AddToTail(chan);
  }

  NET_ClearLagData(socket);

  // just reset and return
  chan->Setup(socket, adr, name, handler);

  return chan;
}

void NET_RemoveNetChannel(INetChannel *netchan, bool bDeleteNetChan) {
  if (!netchan) {
    return;
  }

  AUTO_LOCK_FM(s_NetChannels);
  if (s_NetChannels.Find(static_cast<CNetChan *>(netchan)) ==
      s_NetChannels.InvalidIndex()) {
    DevMsg(1, "NET_CloseNetChannel: unknown channel.\n");
    return;
  }

  s_NetChannels.FindAndRemove(static_cast<CNetChan *>(netchan));

  NET_ClearQueuedPacketsForChannel(netchan);

  if (bDeleteNetChan) delete netchan;
}

/*
=============================================================================

LOOPBACK BUFFERS FOR LOCAL PLAYER

=============================================================================
*/

void NET_SendLoopPacket(int sock, int length, const unsigned char *data,
                        const netadr_t &to) {
  loopback_t *loop;

  if (length > NET_MAX_PAYLOAD) {
    DevMsg("NET_SendLoopPacket:  packet too big (%i).\n", length);
    return;
  }

  loop = new loopback_t;

  if (length <= DEF_LOOPBACK_SIZE) {
    loop->data = loop->defbuffer;
  } else {
    loop->data = new char[length];
  }

  Q_memcpy(loop->data, data, length);
  loop->datalen = length;

  if (sock == NS_SERVER) {
    s_LoopBacks[NS_CLIENT].PushItem(loop);
  } else if (sock == NS_CLIENT) {
    s_LoopBacks[NS_SERVER].PushItem(loop);
  } else {
    DevMsg("NET_SendLoopPacket:  invalid socket (%i).\n", sock);
    return;
  }
}

//=============================================================================

int NET_CountLaggedList(netpacket_t *pList) {
  int c = 0;
  netpacket_t *p = pList;

  while (p) {
    c++;
    p = p->pNext;
  }

  return c;
}

/*
===================
NET_AddToLagged

===================
*/
void NET_AddToLagged(netpacket_t **pList, netpacket_t *pPacket) {
  if (pPacket->pNext) {
    Msg("NET_AddToLagged::Packet already linked\n");
    return;
  }

  // first copy packet

  netpacket_t *newPacket = new netpacket_t;

  (*newPacket) = (*pPacket);                           // copy packet infos
  newPacket->data = new unsigned char[pPacket->size];  // create new data buffer
  Q_memcpy(newPacket->data, pPacket->data, pPacket->size);  // copy packet data
  newPacket->pNext = nullptr;

  // if list is empty, this is our first element
  if ((*pList) == nullptr) {
    (*pList) = newPacket;  // put packet in top of list
  } else {
    netpacket_t *last = (*pList);

    while (last->pNext) {
      // got to end of list
      last = last->pNext;
    }

    // add at end
    last->pNext = newPacket;
  }
}

// Actual lag to use in msec
static float s_FakeLag = 0.0;

float NET_GetFakeLag() { return s_FakeLag; }

// How quickly we converge to a new value for fakelag
#define FAKELAG_CONVERGE 200  // ms per second

/*
==============================
NET_AdjustLag

==============================
*/
void NET_AdjustLag() {
  static double s_LastTime = 0;

  // Bound time step

  float dt = std::clamp(net_time - s_LastTime, 0.0, 0.2);

  s_LastTime = net_time;

  // Already converged?
  if (fakelag.GetFloat() == s_FakeLag) return;

  // Figure out how far we have to go
  float diff = fakelag.GetFloat() - s_FakeLag;

  // How much can we converge this frame
  float converge = FAKELAG_CONVERGE * dt;

  // Last step, go the whole way
  if (converge > fabs(diff)) {
    converge = fabs(diff);
  }

  // Converge toward fakelag.GetFloat()
  if (diff < 0.0) {
    // Converge toward fakelag.GetFloat()
    s_FakeLag -= converge;
  } else {
    s_FakeLag += converge;
  }
}

bool NET_LagPacket(bool newdata, netpacket_t *packet) {
  static int losscount[MAX_SOCKETS];

  if (packet->source >= MAX_SOCKETS)
    return newdata;  // fake lag not supported for extra sockets

  if ((droppackets.GetInt() > 0) && newdata && (packet->source == NS_CLIENT)) {
    droppackets.SetValue(droppackets.GetInt() - 1);
    return false;
  }

  if (fakeloss.GetFloat() && newdata) {
    losscount[packet->source]++;

    if (fakeloss.GetFloat() > 0.0f) {
      // Act like we didn't hear anything if we are going to lose the packet.
      // Depends on random # generator.
      if (RandomInt(0, 100) <= (int)fakeloss.GetFloat()) return false;
    } else {
      int ninterval;

      ninterval = (int)(fabs(fakeloss.GetFloat()));
      ninterval = std::max(2, ninterval);

      if (!(losscount[packet->source] % ninterval)) {
        return false;
      }
    }
  }

  if (s_FakeLag <= 0.0) {
    // Never leave any old msgs around
    for (int i = 0; i < MAX_SOCKETS; i++) {
      NET_ClearLagData(i);
    }
    return newdata;
  }

  // if new packet arrived in fakelag list
  if (newdata) {
    NET_AddToLagged(&s_pLagData[packet->source], packet);
  }

  // Now check the correct list and feed any message that is old enough.
  netpacket_t *p = s_pLagData[packet->source];  // current packet

  if (!p) return false;  // no packet in lag list

  float target = s_FakeLag;
  float maxjitter = std::min(fakejitter.GetFloat(), target * 0.5f);
  target += RandomFloat(-maxjitter, maxjitter);

  if ((p->received + (target / 1000.0f)) > net_time)
    return false;  // not time yet for this packet

#ifdef _DEBUG
  if (fakeshuffle.GetInt() && p->pNext) {
    if (!RandomInt(0, fakeshuffle.GetInt())) {
      // swap p and p->next
      netpacket_t *t = p->pNext;
      p->pNext = t->pNext;
      t->pNext = p;
      p = t;
    }
  }
#endif

  // remove packet p from list (is head)
  s_pLagData[packet->source] = p->pNext;

  // copy & adjust content
  packet->source = p->source;
  packet->from = p->from;
  packet->pNext = nullptr;      // no next
  packet->received = net_time;  // new time
  packet->size = p->size;
  packet->wiresize = p->wiresize;
  packet->stream = p->stream;

  Q_memcpy(packet->data, p->data, p->size);

  // free lag packet

  delete[] p->data;
  delete p;

  return true;
}

// Calculate MAX_SPLITPACKET_SPLITS according to the smallest split size
#define MAX_SPLITPACKET_SPLITS (NET_MAX_MESSAGE / MIN_SPLIT_SIZE)
#define SPLIT_PACKET_STALE_TIME 15.0f

class CSplitPacketEntry {
 public:
  CSplitPacketEntry() {
    memset(&from, 0, sizeof(from));

    int i;
    for (i = 0; i < MAX_SPLITPACKET_SPLITS; i++) {
      splitflags[i] = -1;
    }

    memset(&netsplit, 0, sizeof(netsplit));
    lastactivetime = 0.0f;
  }

 public:
  netadr_t from;
  int splitflags[MAX_SPLITPACKET_SPLITS];
  LONGPACKET netsplit;
  // host_time the last time any entry was received for this entry
  float lastactivetime;
};

typedef CUtlVector<CSplitPacketEntry> vecSplitPacketEntries_t;
static CUtlVector<vecSplitPacketEntries_t> net_splitpackets;

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void NET_DiscardStaleSplitpackets(const int sock) {
  vecSplitPacketEntries_t &splitPacketEntries = net_splitpackets[sock];
  int i;
  for (i = splitPacketEntries.Count() - 1; i >= 0; i--) {
    CSplitPacketEntry *entry = &splitPacketEntries[i];
    Assert(entry);

    if (net_time < (entry->lastactivetime + SPLIT_PACKET_STALE_TIME)) continue;

    splitPacketEntries.Remove(i);
  }
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  : *from -
// Output : CSplitPacketEntry
//-----------------------------------------------------------------------------
CSplitPacketEntry *NET_FindOrCreateSplitPacketEntry(const int sock,
                                                    netadr_t *from) {
  vecSplitPacketEntries_t &splitPacketEntries = net_splitpackets[sock];
  int i, count = splitPacketEntries.Count();
  CSplitPacketEntry *entry = nullptr;
  for (i = 0; i < count; i++) {
    entry = &splitPacketEntries[i];
    Assert(entry);

    if (from->CompareAdr(entry->from)) break;
  }

  if (i >= count) {
    CSplitPacketEntry newentry;
    newentry.from = *from;

    splitPacketEntries.AddToTail(newentry);

    entry = &splitPacketEntries[splitPacketEntries.Count() - 1];
  }

  Assert(entry);
  return entry;
}

static char const *DescribeSocket(int sock) {
  switch (sock) {
    default:
      break;
    case NS_CLIENT:
      return "cl ";
    case NS_SERVER:
      return "sv ";
    case NS_HLTV:
      return "htv";
    case NS_MATCHMAKING:
      return "mat";
    case NS_SYSTEMLINK:
      return "lnk";
  }

  return "??";
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  : *pData -
//			size -
//			*outSize -
// Output : bool
//-----------------------------------------------------------------------------
bool NET_GetLong(const int sock, netpacket_t *packet) {
  int packetNumber, packetCount, sequenceNumber, offset;
  short packetID;
  SPLITPACKET *pHeader;

  if (packet->size < sizeof(SPLITPACKET)) {
    Msg("Invalid split packet length %i\n", packet->size);
    return false;
  }

  CSplitPacketEntry *entry =
      NET_FindOrCreateSplitPacketEntry(sock, &packet->from);
  Assert(entry);
  if (!entry) return false;

  entry->lastactivetime = net_time;
  Assert(packet->from.CompareAdr(entry->from));

  pHeader = (SPLITPACKET *)packet->data;
  // pHeader is network endian correct
  sequenceNumber = LittleLong(pHeader->sequenceNumber);
  packetID = LittleShort(pHeader->packetID);
  // High uint8_t is packet number
  packetNumber = (packetID >> 8);
  // Low uint8_t is number of total packets
  packetCount = (packetID & 0xff);

  int nSplitSizeMinusHeader = (int)LittleShort(pHeader->nSplitSize);
  if (nSplitSizeMinusHeader < MIN_SPLIT_SIZE ||
      nSplitSizeMinusHeader > MAX_SPLIT_SIZE) {
    Msg("NET_GetLong:  Split packet from %s with invalid split size (number "
        "%i/ count %i) where size %i is out of valid range [%d - %d ]\n",
        packet->from.ToString(), packetNumber, packetCount,
        nSplitSizeMinusHeader, MIN_SPLIT_SIZE, MAX_SPLIT_SIZE);
    return false;
  }

  if (packetNumber >= MAX_SPLITPACKET_SPLITS ||
      packetCount > MAX_SPLITPACKET_SPLITS) {
    Msg("NET_GetLong:  Split packet from %s with too many split parts (number "
        "%i/ count %i) where %i is max count allowed\n",
        packet->from.ToString(), packetNumber, packetCount,
        MAX_SPLITPACKET_SPLITS);
    return false;
  }

  // First packet in split series?
  if (entry->netsplit.currentSequence == -1 ||
      sequenceNumber != entry->netsplit.currentSequence) {
    entry->netsplit.currentSequence = sequenceNumber;
    entry->netsplit.splitCount = packetCount;
    entry->netsplit.nExpectedSplitSize = nSplitSizeMinusHeader;
  }

  if (entry->netsplit.nExpectedSplitSize != nSplitSizeMinusHeader) {
    Msg("NET_GetLong:  Split packet from %s with inconsistent split size "
        "(number %i/ count %i) where size %i not equal to initial size of %i\n",
        packet->from.ToString(), packetNumber, packetCount,
        nSplitSizeMinusHeader, entry->netsplit.nExpectedSplitSize);
    return false;
  }

  int size = packet->size - sizeof(SPLITPACKET);

  if (entry->splitflags[packetNumber] != sequenceNumber) {
    // Last packet in sequence? set size
    if (packetNumber == (packetCount - 1)) {
      entry->netsplit.totalSize =
          (packetCount - 1) * nSplitSizeMinusHeader + size;
    }

    entry->netsplit.splitCount--;  // Count packet
    entry->splitflags[packetNumber] = sequenceNumber;

    if (net_showsplits.GetInt() && net_showsplits.GetInt() != 3) {
      Msg("<-- [%s] Split packet %4i/%4i seq %5i size %4i mtu %4i from %s\n",
          DescribeSocket(sock), packetNumber + 1, packetCount, sequenceNumber,
          size, nSplitSizeMinusHeader + sizeof(SPLITPACKET),
          packet->from.ToString());
    }
  } else {
    Msg("NET_GetLong:  Ignoring duplicated split packet %i of %i ( %i bytes ) "
        "from %s\n",
        packetNumber + 1, packetCount, size, packet->from.ToString());
  }

  // Copy the incoming data to the appropriate place in the buffer
  offset = (packetNumber * nSplitSizeMinusHeader);
  memcpy(entry->netsplit.buffer + offset, packet->data + sizeof(SPLITPACKET),
         size);

  // Have we received all of the pieces to the packet?
  if (entry->netsplit.splitCount <= 0) {
    entry->netsplit.currentSequence = -1;  // Clear packet
    if (entry->netsplit.totalSize > sizeof(entry->netsplit.buffer)) {
      Msg("Split packet too large! %d bytes from %s\n",
          entry->netsplit.totalSize, packet->from.ToString());
      return false;
    }

    Q_memcpy(packet->data, entry->netsplit.buffer, entry->netsplit.totalSize);
    packet->size = entry->netsplit.totalSize;
    packet->wiresize = entry->netsplit.totalSize;
    return true;
  }

  return false;
}

bool NET_GetLoopPacket(netpacket_t *packet) {
  Assert(packet);

  loopback_t *loop;

  if (packet->source > NS_SERVER) return false;

  if (!s_LoopBacks[packet->source].PopItem(&loop)) {
    return false;
  }

  if (loop->datalen == 0) {
    // no packet in loopback buffer
    delete loop;
    return (NET_LagPacket(false, packet));
  }

  // copy data from loopback buffer to packet
  packet->from.SetType(NA_LOOPBACK);
  packet->size = loop->datalen;
  packet->wiresize = loop->datalen;
  Q_memcpy(packet->data, loop->data, packet->size);

  loop->datalen = 0;  // buffer is avalibale again

  if (loop->data != loop->defbuffer) {
    delete loop->data;
    loop->data = loop->defbuffer;
  }

  delete loop;

  // allow lag system to modify packet
  return (NET_LagPacket(true, packet));
}

bool NET_ReceiveDatagram(const int sock, netpacket_t *packet) {
  Assert(packet);
  Assert(net_multiplayer);

  struct sockaddr from;
  int fromlen = sizeof(from);
  int net_socket = net_sockets[packet->source].hUDP;

#ifdef _DEBUG
  if (recvpackets.GetInt() >= 0) {
    unsigned long bytes;

    ioctlsocket(net_socket, FIONREAD, &bytes);

    if (bytes <= 0) return false;

    if (recvpackets.GetInt() == 0) return false;

    recvpackets.SetValue(recvpackets.GetInt() - 1);
  }
#endif

  int ret = VCRHook_recvfrom(net_socket, (char *)packet->data, NET_MAX_MESSAGE,
                             0, (struct sockaddr *)&from, (int *)&fromlen);
  if (ret > 0) {
    packet->wiresize = ret;

    MEM_ALLOC_CREDIT();
    CUtlMemoryFixedGrowable<byte, NET_COMPRESSION_STACKBUF_SIZE> bufVoice(
        NET_COMPRESSION_STACKBUF_SIZE);

    packet->from.SetFromSockadr(&from);
    packet->size = ret;

    if (ret < NET_MAX_MESSAGE) {
      // Check for split message
      if (LittleLong(*(int *)packet->data) == NET_HEADER_FLAG_SPLITPACKET) {
        if (!NET_GetLong(sock, packet)) return false;
      }

      // Next check for compressed message
      if (LittleLong(*(int *)packet->data) ==
          NET_HEADER_FLAG_COMPRESSEDPACKET) {
        uint8_t *pCompressedData = packet->data + sizeof(unsigned int);

        CLZSS lzss;
        // Decompress
        int actualSize = lzss.GetActualSize(pCompressedData);
        if (actualSize <= 0) return false;

        MEM_ALLOC_CREDIT();
        CUtlMemoryFixedGrowable<byte, NET_COMPRESSION_STACKBUF_SIZE>
            memDecompressed(NET_COMPRESSION_STACKBUF_SIZE);
        memDecompressed.EnsureCapacity(actualSize);

        unsigned int uDecompressedSize =
            lzss.Uncompress(pCompressedData, memDecompressed.Base());

        // packet->wiresize is already set
        Q_memcpy(packet->data, memDecompressed.Base(), uDecompressedSize);

        packet->size = uDecompressedSize;
      }

      return NET_LagPacket(true, packet);
    } else {
      ConDMsg("NET_ReceiveDatagram:  Oversize packet from %s\n",
              packet->from.ToString());
    }
  } else if (ret == -1) {  // error?
    NET_GetLastError();

    switch (net_error) {
      case WSAEWOULDBLOCK:
      case WSAECONNRESET:
      case WSAECONNREFUSED:
        break;
      case WSAEMSGSIZE:
        ConDMsg("NET_ReceivePacket: %s\n", NET_ErrorString(net_error));
        break;
      default:
        // Let's continue even after errors
        ConDMsg("NET_ReceivePacket: %s\n", NET_ErrorString(net_error));
        break;
    }
  }

  return false;
}

// Changes n random bits in a data block
void NET_AddNoise(u8 *data, int length, int number) {
  for (int i = 0; i < number; i++) {
    int randomByte = RandomInt(0, length - 1);
    int randomBit = RandomInt(0, 7);

    // get original data
    unsigned char dataByte = data[randomByte];

    // flip bit
    if (dataByte & randomBit) {
      dataByte &= ~randomBit;
    } else {
      dataByte |= randomBit;
    }

    // write back
    data[randomByte] = dataByte;
  }
}

netpacket_t *NET_GetPacket(int sock, uint8_t *scratch) {
  // Each socket has its own netpacket to allow multithreading
  netpacket_t &inpacket = net_packets[sock];

  NET_AdjustLag();
  NET_DiscardStaleSplitpackets(sock);

  // setup new packet
  inpacket.from.SetType(NA_IP);
  inpacket.from.Clear();
  inpacket.received = net_time;
  inpacket.source = sock;
  inpacket.data = scratch;
  inpacket.size = 0;
  inpacket.wiresize = 0;
  inpacket.pNext = nullptr;
  inpacket.message.SetDebugName("inpacket.message");

  // Check loopback first
  if (!NET_GetLoopPacket(&inpacket)) {
    if (!NET_IsMultiplayer()) {
      return nullptr;
    }

    // then check UDP data
    if (!NET_ReceiveDatagram(sock, &inpacket)) {
      // at last check if the lag system has a packet for us
      if (!NET_LagPacket(false, &inpacket)) {
        return nullptr;  // we don't have any new packet
      }
    }
  }

  Assert(inpacket.size);

#ifdef _DEBUG
  if (fakenoise.GetInt() > 0) {
    NET_AddNoise(inpacket.data, inpacket.size, fakenoise.GetInt());
  }
#endif

  // prepare bitbuffer for reading packet with new size
  inpacket.message.StartReading(inpacket.data, inpacket.size);

  return &inpacket;
}

void NET_ProcessPending() {
  AUTO_LOCK_FM(s_PendingSockets);
  for (int i = 0; i < s_PendingSockets.Count(); i++) {
    pendingsocket_t *psock = &s_PendingSockets[i];

    char headerBuf[5];

    if ((net_time - psock->time) > TCP_CONNECT_TIMEOUT) {
      NET_CloseSocket(psock->newsock);
      s_PendingSockets.Remove(i);
      continue;
    }

    int ret =
        NET_ReceiveStream(psock->newsock, headerBuf, sizeof(headerBuf), 0);

    if (ret == 0) {
      continue;  // nothing received
    } else if (ret == -1) {
      NET_CloseSocket(psock->newsock);
      s_PendingSockets.Remove(i);
      continue;  // connection closed somehow
    }

    bf_read header(headerBuf, sizeof(headerBuf));

    int cmd = header.ReadByte();
    unsigned long challengeNr = header.ReadLong();
    bool bOK = false;

    if (cmd == STREAM_CMD_ACKN) {
      AUTO_LOCK_FM(s_NetChannels);
      for (int j = 0; j < s_NetChannels.Count(); j++) {
        CNetChan *chan = s_NetChannels[j];

        if (chan->GetSocket() != psock->netsock) continue;

        if (challengeNr == chan->GetChallengeNr() && !chan->m_StreamSocket) {
          if (psock->addr.CompareAdr(chan->remote_address, true)) {
            chan->m_StreamSocket = psock->newsock;
            chan->m_StreamActive = true;

            chan->ResetStreaming();

            bOK = true;

            if (net_showtcp.GetInt()) {
              Msg("TCP <- %s: connection accepted\n", psock->addr.ToString());
            }

            break;
          } else {
            Msg("TCP <- %s: IP address mismatch.\n", psock->addr.ToString());
          }
        }
      }
    }

    if (!bOK) {
      Msg("TCP <- %s: invalid connection request.\n", psock->addr.ToString());
      NET_CloseSocket(psock->newsock);
    }

    s_PendingSockets.Remove(i);
  }
}

void NET_ProcessListen(int sock) {
  netsocket_t *netsock = &net_sockets[sock];

  if (!netsock->bListening) return;

  sockaddr sa;
  int nLengthAddr = sizeof(sa);

  int newSocket;

  VCR_NONPLAYBACKFN(accept(netsock->hTCP, &sa, (socklen_t *)&nLengthAddr),
                    newSocket, "accept");
#if !defined(NO_VCR)
  VCRGenericValue("sockaddr", &sa, sizeof(sa));
#endif
  if (newSocket == -1) {
    NET_GetLastError();

    if (net_error != WSAEWOULDBLOCK) {
      ConDMsg("NET_ThreadListen: %s\n", NET_ErrorString(net_error));
    }
    return;
  }

  // new connection TCP request, put in pending queue

  pendingsocket_t psock;

  psock.newsock = newSocket;
  psock.netsock = sock;
  psock.addr.SetFromSockadr(&sa);
  psock.time = net_time;

  AUTO_LOCK_FM(s_PendingSockets);
  s_PendingSockets.AddToTail(psock);

  // tell client to send challenge number to identify

  char authcmd = STREAM_CMD_AUTH;

  NET_SendStream(newSocket, &authcmd, 1, 0);

  if (net_showtcp.GetInt()) {
    Msg("TCP <- %s: connection request.\n", psock.addr.ToString());
  }
}

CTSList<uint8_t *> g_NetScratchBuffers;

void NET_ProcessSocket(int sock, IConnectionlessPacketHandler *handler) {
  netpacket_t *packet;

  Assert((sock >= 0) && (sock < net_sockets.Count()));

  // Scope for the auto_lock
  {
    AUTO_LOCK_FM(s_NetChannels);

    // get streaming data from channel sockets
    int numChannels = s_NetChannels.Count();

    for (int i = (numChannels - 1); i >= 0; i--) {
      CNetChan *netchan = s_NetChannels[i];

      // sockets must match
      if (sock != netchan->GetSocket()) continue;

      if (!netchan->ProcessStream()) {
        netchan->GetMsgHandler()->ConnectionCrashed("TCP connection failed.");
      }
    }
  }

  // now get datagrams from sockets
  uint8_t *scratch;
  if (!g_NetScratchBuffers.PopItem(&scratch)) {
    scratch = new byte[NET_MAX_MESSAGE];
  }
  while ((packet = NET_GetPacket(sock, scratch)) != nullptr) {
    if (Filter_ShouldDiscard(
            packet->from))  // filtering is done by network layer
    {
      Filter_SendBan(packet->from);  // tell them we aren't listening...
      continue;
    }

    // check for connectionless packet (0xffffffff) first
    if (LittleLong(*(unsigned int *)packet->data) == CONNECTIONLESS_HEADER) {
      packet->message.ReadLong();  // read the -1

      if (net_showudp.GetInt()) {
        Msg("UDP <- %s: sz=%i OOB '%c' wire=%i\n", packet->from.ToString(),
            packet->size, packet->data[4], packet->wiresize);
      }

      handler->ProcessConnectionlessPacket(packet);
      continue;
    }

    // check for packets from connected clients

    CNetChan *netchan = NET_FindNetChannel(sock, packet->from);

    if (netchan) {
      netchan->ProcessPacket(packet, true);
    }
    /* else	// Not an error that may happen during connect or disconnect
    {
            Msg ("Sequenced packet without connection from %s\n" ,
    packet->from.ToString() );
    }*/
  }
  g_NetScratchBuffers.PushItem(scratch);
}

void NET_LogBadPacket(netpacket_t *packet) {
  FileHandle_t fp;
  int i = 0;
  char filename[MAX_OSPATH];
  bool done = false;

  while (i < 1000 && !done) {
    Q_snprintf(filename, sizeof(filename), "badpacket%03i.dat", i);
    fp = g_pFileSystem->Open(filename, "rb");
    if (!fp) {
      fp = g_pFileSystem->Open(filename, "wb");
      g_pFileSystem->Write(packet->data, packet->size, fp);
      done = true;
    }
    if (fp) {
      g_pFileSystem->Close(fp);
    }
    i++;
  }

  if (i < 1000) {
    Msg("Error buffer for %s written to %s\n", packet->from.ToString(),
        filename);
  } else {
    Msg("Couldn't write error buffer, delete error###.dat files to make "
        "space\n");
  }
}

int NET_SendToImpl(SOCKET s, const char FAR *buf, int len,
                   const struct sockaddr FAR *to, int tolen,
                   int iGameDataLength) {
  return sendto(s, buf, len, 0, to, tolen);
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  : sock -
//			s -
//			buf -
//			len -
//			flags -
//			to -
//			tolen -
// Output : int
//-----------------------------------------------------------------------------
bool CL_IsHL2Demo();
bool CL_IsPortalDemo();
int NET_SendTo(bool verbose, SOCKET s, const char FAR *buf, int len,
               const struct sockaddr FAR *to, int tolen, int iGameDataLength) {
  int nSend = 0;

  // If it's 0.0.0.0:0, then it's a fake player + sv_stressbots and we've
  // plumbed everything all the way through here, where we finally bail out.
  sockaddr_in *pInternetAddr = (sockaddr_in *)to;
#ifdef _WIN32
  if (pInternetAddr->sin_addr.S_un.S_addr == 0
#else
  if (pInternetAddr->sin_addr.s_addr == 0
#endif
      && pInternetAddr->sin_port == 0) {
    return len;
  }

  // Normally, we shouldn't need to write this data to the file, but it can help
  // catch out-of-sync errors earlier.
  if (VCRGetMode() != VCR_Disabled && vcr_verbose.GetInt()) {
#if !defined(NO_VCR)
    VCRGenericValue("senddata", &len, sizeof(len));
    VCRGenericValue("senddata2", (char *)buf, len);
#endif
  }

  // Don't send anything out in VCR mode.. it just annoys other people testing
  // in multiplayer.
  if (VCRGetMode() != VCR_Playback) {
#ifndef SWDS
    if ((CL_IsHL2Demo() || CL_IsPortalDemo()) && !net_dedicated) {
      Error("");
    }
#endif  // _WIN32

    nSend = NET_SendToImpl(s, buf, len, to, tolen, iGameDataLength);
  }

#if defined(_DEBUG)
  if (verbose && (nSend > 0) && (len > MAX_ROUTABLE_PAYLOAD)) {
    ConDMsg("NET_SendTo:  Packet length (%i) > (%i) bytes\n", len,
            MAX_ROUTABLE_PAYLOAD);
  }
#endif
  return nSend;
}

#if defined(_DEBUG)

#include "FileSystem.h"
#include "FileSystem_Engine.h"

//-----------------------------------------------------------------------------
// Purpose:
// Output : char const
//-----------------------------------------------------------------------------
char const *NET_GetDebugFilename(char const *prefix) {
  static char filename[MAX_OSPATH];

  int i;

  for (i = 0; i < 10000; i++) {
    Q_snprintf(filename, sizeof(filename), "debug/%s%04i.dat", prefix, i);
    if (g_pFileSystem->FileExists(filename)) continue;

    return filename;
  }

  return nullptr;
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  : *filename -
//			*buf -
//			len -
//-----------------------------------------------------------------------------
void NET_StorePacket(char const *filename, uint8_t const *buf, int len) {
  FileHandle_t fh;

  g_pFileSystem->CreateDirHierarchy("debug/", "DEFAULT_WRITE_PATH");
  fh = g_pFileSystem->Open(filename, "wb");
  if (FILESYSTEM_INVALID_HANDLE != fh) {
    g_pFileSystem->Write(buf, len, fh);
    g_pFileSystem->Close(fh);
  }
}

#endif  // _DEBUG

struct SendQueueItem_t {
  SendQueueItem_t() : m_pChannel(nullptr), m_Socket((SOCKET)-1) {}

  CNetChan *m_pChannel;
  SOCKET m_Socket;
  CUtlBuffer m_Buffer;
  CUtlBuffer m_To;
};

struct SendQueue_t {
  SendQueue_t() : m_nHostFrame(0) {}
  int m_nHostFrame;
  CUtlLinkedList<SendQueueItem_t> m_SendQueue;
};

static SendQueue_t g_SendQueue;

int NET_QueuePacketForSend(CNetChan *chan, bool verbose, SOCKET s,
                           const char FAR *buf, int len,
                           const struct sockaddr FAR *to, int tolen,
                           uint32_t msecDelay) {
  // If net_queued_packet_thread was -1 at startup, then we don't even have a
  // thread.
  if (net_queued_packet_thread.GetInt() && g_pQueuedPackedSender->IsRunning()) {
    g_pQueuedPackedSender->QueuePacket(chan, s, buf, len, to, tolen, msecDelay);
  } else {
    Assert(chan);
    // Set up data structure
    SendQueueItem_t *sq =
        &g_SendQueue.m_SendQueue[g_SendQueue.m_SendQueue.AddToTail()];
    sq->m_Socket = s;
    sq->m_pChannel = chan;
    sq->m_Buffer.Put((const void *)buf, len);
    sq->m_To.Put((const void *)to, tolen);
    sq->m_pChannel->IncrementQueuedPackets();
  }

  return len;
}

void NET_SendQueuedPacket(SendQueueItem_t *sq) {
  // Msg( "Send queued packet %d\n", sq->m_Buffer.TellPut() );
  NET_SendTo(false, sq->m_Socket, (const char FAR *)sq->m_Buffer.Base(),
             sq->m_Buffer.TellPut(),
             (const struct sockaddr FAR *)sq->m_To.Base(), sq->m_To.TellPut(),
             -1);

  sq->m_pChannel->DecrementQueuedPackets();
}

void NET_ClearQueuedPacketsForChannel(INetChannel *channel) {
  CUtlLinkedList<SendQueueItem_t> &list = g_SendQueue.m_SendQueue;

  for (unsigned short i = list.Head(); i != list.InvalidIndex();) {
    unsigned short n = list.Next(i);
    SendQueueItem_t &e = list[i];
    if (e.m_pChannel == channel) {
      list.Remove(i);
    }
    i = n;
  }
}

void NET_SendQueuedPackets() {
  // Only do this once per frame
  if (host_framecount == g_SendQueue.m_nHostFrame) return;
  g_SendQueue.m_nHostFrame = host_framecount;

  CUtlLinkedList<SendQueueItem_t> &list = g_SendQueue.m_SendQueue;
  if (list.IsValidIndex(list.Head())) {
    SendQueueItem_t *sq = &list[list.Head()];
    NET_SendQueuedPacket(sq);
    list.Remove(list.Head());
  }
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  : sock -
//			s -
//			buf -
//			len -
//			flags -
//			to -
//			tolen -
// Output : int
//-----------------------------------------------------------------------------
static volatile int32_t s_SplitPacketSequenceNumber[MAX_SOCKETS] = {1};

int NET_SendLong(INetChannel *chan, int sock, SOCKET s, const char FAR *buf,
                 int len, const struct sockaddr FAR *to, int tolen,
                 int nMaxRoutableSize) {
  CNetChan *netchan = dynamic_cast<CNetChan *>(chan);

  int nSplitSizeMinusHeader = nMaxRoutableSize - sizeof(SPLITPACKET);

  int nSequenceNumber = -1;
  if (netchan) {
    nSequenceNumber = netchan->IncrementSplitPacketSequence();
  } else {
    nSequenceNumber =
        ThreadInterlockedIncrement(&s_SplitPacketSequenceNumber[sock]);
  }

  const char *sendbuf = buf;
  int sendlen = len;

  char packet[MAX_ROUTABLE_PAYLOAD];
  SPLITPACKET *pPacket = (SPLITPACKET *)packet;

  // Make pPacket data network endian correct
  pPacket->netID = LittleLong(NET_HEADER_FLAG_SPLITPACKET);
  pPacket->sequenceNumber = LittleLong(nSequenceNumber);
  pPacket->nSplitSize = LittleShort(nSplitSizeMinusHeader);

  int nPacketCount =
      (sendlen + nSplitSizeMinusHeader - 1) / nSplitSizeMinusHeader;

#if defined(_DEBUG)
  if (net_savelargesplits.GetInt() != -1 &&
      nPacketCount >= net_savelargesplits.GetInt()) {
    char const *filename = NET_GetDebugFilename("splitpacket");
    if (filename) {
      Msg("Saving split packet of %i bytes and %i packets to file %s\n",
          sendlen, nPacketCount, filename);

      NET_StorePacket(filename, (uint8_t const *)sendbuf, sendlen);
    } else {
      Msg("Too many files in debug directory, clear out old data!\n");
    }
  }
#endif

  int nBytesLeft = sendlen;
  int nPacketNumber = 0;
  int nTotalBytesSent = 0;
  bool bFirstSend = true;

  while (nBytesLeft > 0) {
    int size = std::min(nSplitSizeMinusHeader, nBytesLeft);

    pPacket->packetID =
        LittleShort((short)((nPacketNumber << 8) + nPacketCount));

    Q_memcpy(packet + sizeof(SPLITPACKET),
             sendbuf + (nPacketNumber * nSplitSizeMinusHeader), size);

    int ret = 0;

    // Setting net_queued_packet_thread to NET_QUEUED_PACKET_THREAD_DEBUG_VALUE
    // goes into a mode where all packets are queued.. can be used to
    // stress-test it. Linux threads aren't prioritized well enough for this to
    // work well (i.e. the queued packet thread doesn't get enough attention to
    // flush itself well). Plus, Linux doesn't have the problem that Windows has
    // where, if you send 6 UDP packets without giving up your timeslice, it'll
    // just discard the 7th and later packets until you Sleep(). Linux doesn't
    // have that problem so it has no need for a queue.
#ifndef OS_POSIX
    if (netchan && (!bFirstSend || net_queued_packet_thread.GetInt() ==
                                       NET_QUEUED_PACKET_THREAD_DEBUG_VALUE)) {
      uint32_t delay =
          (int)(1000.0f * ((float)(nPacketNumber *
                                   (nMaxRoutableSize + UDP_HEADER_SIZE)) /
                           (float)netchan->GetDataRate()) +
                0.5f);
      ret =
          NET_QueuePacketForSend(netchan, false, s, packet,
                                 size + sizeof(SPLITPACKET), to, tolen, delay);
    } else
#endif
    {
      // Also, we send the first packet no matter what
      // w/o a netchan, if there are too many splits, its possible the packet
      // can't be delivered.  However, this would only apply to out of band
      // stuff like
      //  server query packets, which should never require splitting anyway.
      ret = NET_SendTo(false, s, packet, size + sizeof(SPLITPACKET), to, tolen,
                       -1);
    }

    // First split send
    bFirstSend = false;

    if (ret < 0) {
      return ret;
    }

    if (ret >= size) {
      nTotalBytesSent += size;
    }

    nBytesLeft -= size;
    ++nPacketNumber;

    // Always bitch about split packets in debug
    if (net_showsplits.GetInt() && net_showsplits.GetInt() != 2) {
      netadr_t adr;

      adr.SetFromSockadr((struct sockaddr *)to);

      Msg("--> [%s] Split packet %4i/%4i seq %5i size %4i mtu %4i to %s [ "
          "total %4i ]\n",
          DescribeSocket(sock), nPacketNumber, nPacketCount,
          pPacket->sequenceNumber, size, nMaxRoutableSize, adr.ToString(),
          sendlen);
    }
  }

  return nTotalBytesSent;
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  : sock -
//			length -
//			*data -
//			to -
// Output : void NET_SendPacket
//-----------------------------------------------------------------------------

int NET_SendPacket(INetChannel *chan, int sock, const netadr_t &to,
                   const unsigned char *data, int length,
                   bf_write *pVoicePayload /* = nullptr */,
                   bool bUseCompression /*=false*/) {
  int ret;
  struct sockaddr addr;
  int net_socket;

  if (net_showudp.GetInt() &&
      (*(unsigned int *)data == CONNECTIONLESS_HEADER)) {
    Assert(!bUseCompression);
    Msg("UDP -> %s: sz=%i OOB '%c'\n", to.ToString(), length, data[4]);
  }

  if (!NET_IsMultiplayer() || to.type == NA_LOOPBACK ||
      (to.IsLocalhost() && !net_usesocketsforloopback.GetBool())) {
    Assert(!pVoicePayload);

    NET_SendLoopPacket(sock, length, data, to);
    return length;
  }

  if (to.type == NA_BROADCAST) {
    net_socket = net_sockets[sock].hUDP;
    if (!net_socket) return length;
  } else if (to.type == NA_IP) {
    net_socket = net_sockets[sock].hUDP;
    if (!net_socket) return length;
  } else {
    DevMsg("NET_SendPacket: bad address type (%i)\n", to.type);
    return length;
  }

  if ((droppackets.GetInt() < 0) && sock == NS_CLIENT) {
    droppackets.SetValue(droppackets.GetInt() + 1);
    return length;
  }

  if (fakeloss.GetFloat() > 0.0f) {
    // simulate sending this packet
    if (RandomInt(0, 100) <= (int)fakeloss.GetFloat()) return length;
  }

  to.ToSockadr(&addr);

  MEM_ALLOC_CREDIT();
  CUtlMemoryFixedGrowable<byte, NET_COMPRESSION_STACKBUF_SIZE> memCompressed(
      NET_COMPRESSION_STACKBUF_SIZE);
  CUtlMemoryFixedGrowable<byte, NET_COMPRESSION_STACKBUF_SIZE>
      memCompressedVoice(NET_COMPRESSION_STACKBUF_SIZE);

  int iGameDataLength = pVoicePayload ? length : -1;

  bool bWroteVoice = false;
  unsigned int nVoiceBytes = 0;

  if (pVoicePayload) {
    memCompressedVoice.EnsureCapacity(pVoicePayload->GetNumBytesWritten() +
                                      sizeof(unsigned short));

    uint8_t *pVoice = (uint8_t *)memCompressedVoice.Base();

    unsigned short usVoiceBits = pVoicePayload->GetNumBitsWritten();
    *(unsigned short *)pVoice = LittleShort(usVoiceBits);
    pVoice += sizeof(unsigned short);

    unsigned int nCompressedLength = pVoicePayload->GetNumBytesWritten();
    uint8_t *pOutput = nullptr;
    if (net_compressvoice.GetBool()) {
      CLZSS lzss;
      pOutput = lzss.CompressNoAlloc(pVoicePayload->GetData(),
                                     pVoicePayload->GetNumBytesWritten(),
                                     (uint8_t *)pVoice, &nCompressedLength);
    }
    if (!pOutput) {
      Q_memcpy(pVoice, pVoicePayload->GetData(),
               pVoicePayload->GetNumBytesWritten());
    }

    nVoiceBytes = nCompressedLength + sizeof(unsigned short);
  }

  if (bUseCompression) {
    CLZSS lzss;
    unsigned int nCompressedLength = length;

    memCompressed.EnsureCapacity(length + nVoiceBytes + sizeof(unsigned int));

    *(int *)memCompressed.Base() = LittleLong(NET_HEADER_FLAG_COMPRESSEDPACKET);

    uint8_t *pOutput = lzss.CompressNoAlloc(
        (uint8_t *)data, length, memCompressed.Base() + sizeof(unsigned int),
        &nCompressedLength);
    if (pOutput) {
      data = memCompressed.Base();
      length = nCompressedLength + sizeof(unsigned int);

      if (pVoicePayload && pVoicePayload->GetNumBitsWritten() > 0) {
        uint8_t *pVoice = (uint8_t *)memCompressed.Base() + length;
        Q_memcpy(pVoice, memCompressedVoice.Base(), nVoiceBytes);
      }

      iGameDataLength = length;

      length += nVoiceBytes;

      bWroteVoice = true;
    }
  }

  if (!bWroteVoice && pVoicePayload && pVoicePayload->GetNumBitsWritten() > 0) {
    memCompressed.EnsureCapacity(length + nVoiceBytes);

    uint8_t *pVoice = (uint8_t *)memCompressed.Base();
    Q_memcpy(pVoice, (const void *)data, length);
    pVoice += length;
    Q_memcpy(pVoice, memCompressedVoice.Base(), nVoiceBytes);
    data = memCompressed.Base();

    length += nVoiceBytes;
  }

  // Do we need to break this packet up?
  int nMaxRoutable = MAX_ROUTABLE_PAYLOAD;
  if (chan) {
    nMaxRoutable = std::clamp(
        chan->GetMaxRoutablePayloadSize(), MIN_USER_MAXROUTABLE_SIZE,
        std::min(sv_maxroutable.GetInt(), MAX_USER_MAXROUTABLE_SIZE));
  }

  if (length <= nMaxRoutable && !(net_queued_packet_thread.GetInt() ==
                                      NET_QUEUED_PACKET_THREAD_DEBUG_VALUE &&
                                  chan)) {
    // simple case, small packet, just send it
    ret = NET_SendTo(true, net_socket, (const char *)data, length, &addr,
                     sizeof(addr), iGameDataLength);
  } else {
    // split packet into smaller pieces
    ret = NET_SendLong(chan, sock, net_socket, (const char *)data, length,
                       &addr, sizeof(addr), nMaxRoutable);
  }

  if (ret == -1) {
    NET_GetLastError();

    // wouldblock is silent
    if (net_error == WSAEWOULDBLOCK) return 0;

    if (net_error == WSAECONNRESET) return 0;

    // some PPP links dont allow broadcasts
    if ((net_error == WSAEADDRNOTAVAIL) && (to.type == NA_BROADCAST)) return 0;

    ConDMsg("NET_SendPacket Warning: %s : %s\n", NET_ErrorString(net_error),
            to.ToString());
    ret = length;
  }

  return ret;
}

void NET_OutOfBandPrintf(int sock, const netadr_t &adr, const char *format,
                         ...) {
  va_list argptr;
  char string[MAX_ROUTABLE_PAYLOAD];

  *(unsigned int *)string = CONNECTIONLESS_HEADER;

  va_start(argptr, format);
  Q_vsnprintf(string + 4, sizeof(string) - 4, format, argptr);
  va_end(argptr);

  int length = Q_strlen(string + 4) + 5;

  NET_SendPacket(nullptr, sock, adr, (uint8_t *)string, length);
}

/*
====================
NET_CloseAllSockets
====================
*/
void NET_CloseAllSockets() {
  // shut down any existing and open sockets
  for (int i = 0; i < net_sockets.Count(); i++) {
    if (net_sockets[i].nPort) {
      NET_CloseSocket(net_sockets[i].hUDP);
      NET_CloseSocket(net_sockets[i].hTCP);

      net_sockets[i].nPort = 0;
      net_sockets[i].bListening = false;
      net_sockets[i].hUDP = 0;
      net_sockets[i].hTCP = 0;
    }
  }

  // shut down all pending sockets
  AUTO_LOCK_FM(s_PendingSockets);
  for (int j = 0; j < s_PendingSockets.Count(); j++) {
    NET_CloseSocket(s_PendingSockets[j].newsock);
  }

  s_PendingSockets.RemoveAll();
}

/*
====================
NET_FlushAllSockets
====================
*/
void NET_FlushAllSockets() {
  // drain any packets that my still lurk in our incoming queue
  char data[2048];
  struct sockaddr from;
  int fromlen = sizeof(from);

  for (int i = 0; i < net_sockets.Count(); i++) {
    if (net_sockets[i].hUDP) {
      int bytes = 1;

      // loop until no packets are pending anymore
      while (bytes > 0) {
        bytes = VCRHook_recvfrom(net_sockets[i].hUDP, data, sizeof(data), 0,
                                 (struct sockaddr *)&from, (int *)&fromlen);
      }
    }
  }
}

static void OpenSocketInternal(int nModule, int nSetPort, int nDefaultPort,
                               const char *pName, int nProtocol, bool bTryAny) {
  int port = nSetPort ? nSetPort : nDefaultPort;
  int *handle = nullptr;
  if (nProtocol == IPPROTO_TCP) {
    handle = &net_sockets[nModule].hTCP;
  } else if (nProtocol == IPPROTO_UDP || nProtocol == IPPROTO_VDP) {
    handle = &net_sockets[nModule].hUDP;
  } else {
    Sys_Error("Unrecognized protocol type %d", nProtocol);
    return;
  }

  if (!net_sockets[nModule].nPort) {
    *handle = NET_OpenSocket(ipname.GetString(), port, nProtocol);

    if (!*handle && bTryAny) {
      port = PORT_ANY;  // try again with PORT_ANY
      *handle = NET_OpenSocket(ipname.GetString(), port, nProtocol);
    }

    if (!*handle) {
      Sys_Error("Couldn't allocate any %s IP port", pName);
      return;
    }

    net_sockets[nModule].nPort = port;
  } else {
    Msg("WARNING: NET_OpenSockets: %s port %i already open.\n", pName,
        net_sockets[nModule].nPort);
  }
}

/*
====================
NET_OpenSockets
====================
*/
void NET_OpenSockets() {
  const int nProtocol = IPPROTO_UDP;

  OpenSocketInternal(NS_SERVER, hostport.GetInt(), PORT_SERVER, "server",
                     nProtocol, false);
  OpenSocketInternal(NS_CLIENT, clientport.GetInt(), PORT_SERVER, "client",
                     nProtocol, true);

  if (!net_nohltv) {
    OpenSocketInternal(NS_HLTV, hltvport.GetInt(), PORT_HLTV, "hltv", nProtocol,
                       false);
  }
}

int NET_AddExtraSocket(int port) {
  int newSocket = net_sockets.AddToTail();

  Q_memset(&net_sockets[newSocket], 0, sizeof(netsocket_t));

  OpenSocketInternal(newSocket, port, PORT_ANY, "etxra", IPPROTO_UDP, true);

  net_packets.EnsureCount(newSocket + 1);
  net_splitpackets.EnsureCount(newSocket + 1);

  return newSocket;
}

void NET_RemoveAllExtraSockets() {
  for (int i = MAX_SOCKETS; i < net_sockets.Count(); i++) {
    if (net_sockets[i].nPort) {
      NET_CloseSocket(net_sockets[i].hUDP);
      NET_CloseSocket(net_sockets[i].hTCP);
    }
  }
  net_sockets.RemoveMultiple(MAX_SOCKETS, net_sockets.Count() - MAX_SOCKETS);

  Assert(net_sockets.Count() == MAX_SOCKETS);
}

unsigned short NET_GetUDPPort(int socket) {
  if (socket < 0 || socket >= net_sockets.Count()) return 0;

  return net_sockets[socket].nPort;
}

/*
================
NET_GetLocalAddress

Returns the servers' ip address as a string.
================
*/
void NET_GetLocalAddress() {
  net_local_adr.Clear();

  if (net_noip) {
    Msg("TCP/UDP Disabled.\n");
  } else {
    char buff[512];

    // If we have changed the ip var from the command line, use that instead.
    if (Q_strcmp(ipname.GetString(), "localhost")) {
      Q_strncpy(buff, ipname.GetString(),
                sizeof(buff));  // use IP set with ipname
    } else {
      gethostname(buff, sizeof(buff));  // get own IP address
      buff[sizeof(buff) - 1] = 0;  // Ensure that it doesn't overrun the buffer
    }

    NET_StringToAdr(buff, &net_local_adr);

    int ipaddr = (net_local_adr.ip[0] << 24) + (net_local_adr.ip[1] << 16) +
                 (net_local_adr.ip[2] << 8) + net_local_adr.ip[3];

    hostip.SetValue(ipaddr);
  }
}

/*
====================
NET_IsConfigured

Is winsock ip initialized?
====================
*/
bool NET_IsMultiplayer() { return net_multiplayer; }

bool NET_IsDedicated() { return net_dedicated; }

/*
====================
NET_SetTime

Updates net_time
====================
*/
void NET_SetTime(double the_realtime) {
  static double s_last_realtime = 0;

  double frametime = the_realtime - s_last_realtime;
  s_last_realtime = the_realtime;

  if (frametime > 1.0f) {
    // if we have very long frame times because of loading stuff
    // don't apply that to net time to avoid unwanted timeouts
    frametime = 1.0f;
  } else if (frametime < 0.0f) {
    frametime = 0.0f;
  }

  // adjust network time so fakelag works with host_timescale
  net_time += frametime * host_timescale.GetFloat();
}

/*
====================
NET_RunFrame

RunFrame must be called each system frame before reading/sending on any socket
====================
*/
void NET_RunFrame(double the_realtime) {
  NET_SetTime(the_realtime);

  RCONServer().RunFrame();
  RPTServer().RunFrame();
#ifndef SWDS
  RCONClient().RunFrame();
  RPTClient().RunFrame();
#endif
  if (!NET_IsMultiplayer() || net_notcp) return;

  // process TCP sockets:
  for (int i = 0; i < net_sockets.Count(); i++) {
    if (net_sockets[i].hTCP && net_sockets[i].bListening) {
      NET_ProcessListen(i);
    }
  }

  NET_ProcessPending();
}

void NET_ClearLoopbackBuffers() {
  for (int i = 0; i < LOOPBACK_SOCKETS; i++) {
    loopback_t *loop;

    while (s_LoopBacks[i].PopItem(&loop)) {
      if (loop->data && loop->data != loop->defbuffer) {
        delete[] loop->data;
      }
      delete loop;
    }
  }
}

void NET_ConfigLoopbackBuffers(bool bAlloc) { NET_ClearLoopbackBuffers(); }

/*
====================
NET_Config

A single player game will only use the loopback code
====================
*/

void NET_Config() {
  // free anything
  NET_CloseAllSockets();  // close all UDP/TCP sockets

  net_time = 0.0f;

  // now reconfiguare

  if (net_multiplayer) {
    // don't allocate loopback buffers
    NET_ConfigLoopbackBuffers(false);

    // get localhost IP address
    NET_GetLocalAddress();

    // reopen sockets if in MP mode
    NET_OpenSockets();

    // setup the rcon server sockets
    if (net_dedicated || CommandLine()->FindParm("-usercon")) {
      netadr_t rconAddr = net_local_adr;
      rconAddr.SetPort(net_sockets[NS_SERVER].nPort);
      RCONServer().SetAddress(rconAddr.ToString());
      RCONServer().CreateSocket();
    }
  } else {
    // allocate loopback buffers
    NET_ConfigLoopbackBuffers(true);
  }

  Msg("Network: IP %s, mode %s, dedicated %s, ports %i SV / %i CL\n",
      net_local_adr.ToString(true), net_multiplayer ? "MP" : "SP",
      net_dedicated ? "Yes" : "No", net_sockets[NS_SERVER].nPort,
      net_sockets[NS_CLIENT].nPort);
}

/*
====================
NET_SetDedicated

A single player game will only use the loopback code
====================
*/

void NET_SetDedicated() {
  if (net_noip) {
    Msg("Warning! Dedicated not possible with -noip parameter.\n");
    return;
  }

  net_dedicated = true;
}

void NET_ListenSocket(int sock, bool bListen) {
  Assert((sock >= 0) && (sock < net_sockets.Count()));

  netsocket_t *netsock = &net_sockets[sock];

  if (netsock->hTCP) {
    NET_CloseSocket(netsock->hTCP, sock);
  }

  if (!NET_IsMultiplayer() || net_notcp) return;

  if (bListen) {
    const char *net_interface = ipname.GetString();

    netsock->hTCP = NET_OpenSocket(net_interface, netsock->nPort, true);

    if (!netsock->hTCP) {
      Msg("Warning! NET_ListenSocket failed opening socket %i, port %i.\n",
          sock, net_sockets[sock].nPort);
      return;
    }

    struct sockaddr_in address;

    if (!net_interface || !net_interface[0] ||
        !Q_strcmp(net_interface, "localhost")) {
      address.sin_addr.s_addr = INADDR_ANY;
    } else {
      NET_StringToSockaddr(net_interface, (struct sockaddr *)&address);
    }

    address.sin_family = AF_INET;
    address.sin_port = NET_HostToNetShort((short)(netsock->nPort));

    int ret;
    VCR_NONPLAYBACKFN(
        bind(netsock->hTCP, (struct sockaddr *)&address, sizeof(address)), ret,
        "bind");
    if (ret == -1) {
      NET_GetLastError();
      Msg("WARNING: NET_ListenSocket bind failed on socket %i, port %i.\n",
          netsock->hTCP, netsock->nPort);
      return;
    }

    VCR_NONPLAYBACKFN(listen(netsock->hTCP, TCP_MAX_ACCEPTS), ret, "listen");
    if (ret == -1) {
      NET_GetLastError();
      Msg("WARNING: NET_ListenSocket listen failed on socket %i, port %i.\n",
          netsock->hTCP, netsock->nPort);
      return;
    }

    netsock->bListening = true;
  }
}

void NET_SetMutiplayer(bool multiplayer) {
  if (net_noip && multiplayer) {
    Msg("Warning! Multiplayer mode not available with -noip parameter.\n");
    return;
  }

  if (net_dedicated && !multiplayer) {
    Msg("Warning! Singleplayer mode not available on dedicated server.\n");
    return;
  }

  // reconfigure if changed
  if (net_multiplayer != multiplayer) {
    net_multiplayer = multiplayer;
    NET_Config();
  }

  // clear loopback buffer in single player mode
  if (!multiplayer) {
    NET_ClearLoopbackBuffers();
  }
}

void NET_Init(bool is_dedicated) {
#ifndef OS_POSIX
  if (CommandLine()->FindParm("-NoQueuedPacketThread")) {
    Warning(
        "Found -NoQueuedPacketThread, so no queued packet thread will be "
        "created.\n");
  } else {
    g_pQueuedPackedSender->Setup();
  }
#endif

  if (CommandLine()->FindParm("-nodns")) {
    net_nodns = true;
  }

  if (CommandLine()->FindParm("-usetcp")) {
    net_notcp = false;
  }

  if (CommandLine()->FindParm("-nohltv")) {
    net_nohltv = true;
  }

  if (CommandLine()->FindParm("-noip")) {
    net_noip = true;
  } else {
#if defined(_WIN32)
    scoped_winsock_initializer = new source::windows::ScopedWinsockInitializer{
        source::windows::WinsockVersion::Version2_2};
    const DWORD error_code = scoped_winsock_initializer->error_code();
    if (error_code != ERROR_SUCCESS) {
      ConWarning("Winsock 2.2 unavailable (0x%.8x).", error_code);
      net_noip = true;
    }
#endif  // _WIN32
  }

  static_assert(SVC_LASTMSG < (1 << NETMSG_TYPE_BITS));
  static_assert(NET_MAX_PAYLOAD < (1 << NET_MAX_PALYLOAD_BITS));
  static_assert(MAX_FILE_SIZE < (1 << MAX_FILE_SIZE_BITS));

  net_time = 0.0;

  int port = CommandLine()->ParmValue("-port", -1);
  if (port == -1) {
    // check if they used +port by mistake.
    port = CommandLine()->ParmValue("+port", -1);
  }

  if (port != -1) {
    hostport.SetValue(port);
  }

  // clear static stuff
  net_sockets.EnsureCount(MAX_SOCKETS);
  net_packets.EnsureCount(MAX_SOCKETS);
  net_splitpackets.EnsureCount(MAX_SOCKETS);

  for (int i = 0; i < MAX_SOCKETS; ++i) {
    s_pLagData[i] = nullptr;
    Q_memset(&net_sockets[i], 0, sizeof(netsocket_t));
  }

  const char *ip = CommandLine()->ParmValue("-ip");
  // if they had a command line option for IP
  if (ip) {
    // update the cvar right now, this will get overwritten by "stuffcmds" later
    ipname.SetValue(ip);
  }

  if (is_dedicated) {
    // set dedicated MP mode
    NET_SetDedicated();
  } else {
    // set SP mode
    NET_ConfigLoopbackBuffers(true);
  }
}

void NET_Shutdown() {
  for (int i = 0; i < MAX_SOCKETS; i++) {
    NET_ClearLaggedList(&s_pLagData[i]);
  }

  g_pQueuedPackedSender->Shutdown();

  net_multiplayer = false;
  net_dedicated = false;

  NET_CloseAllSockets();
  NET_ConfigLoopbackBuffers(false);

#if defined(_WIN32)
  if (!net_noip) {
    delete scoped_winsock_initializer;
    scoped_winsock_initializer = nullptr;
  }
#endif  // _WIN32

  Assert(s_NetChannels.Count() == 0);
  Assert(s_PendingSockets.Count() == 0);
}

void NET_PrintChannelStatus(INetChannel *chan) {
  Msg("NetChannel '%s':\n", chan->GetName());
  Msg("- remote IP: %s\n", chan->GetAddress(),
      chan->IsPlayback() ? "(Demo`)" : "");
  Msg("- online: %s\n", COM_FormatSeconds(chan->GetTimeConnected()));
  Msg("- reliable: %s\n",
      chan->HasPendingReliableData() ? "pending data" : "available");
  Msg("- latency: %.1f, loss %.2f\n", chan->GetAvgLatency(FLOW_OUTGOING),
      chan->GetAvgLoss(FLOW_INCOMING));
  Msg("- packets: in %.1f/s, out %.1f/s\n", chan->GetAvgPackets(FLOW_INCOMING),
      chan->GetAvgPackets(FLOW_OUTGOING));
  Msg("- choke: in %.2f, out %.2f\n", chan->GetAvgChoke(FLOW_INCOMING),
      chan->GetAvgChoke(FLOW_OUTGOING));
  Msg("- flow: in %.1f, out %.1f kB/s\n",
      chan->GetAvgData(FLOW_INCOMING) / 1024.0f,
      chan->GetAvgData(FLOW_OUTGOING) / 1024.0f);
  Msg("- total: in %.1f, out %.1f MB\n\n",
      (float)chan->GetTotalData(FLOW_INCOMING) / (1024 * 1024),
      (float)chan->GetTotalData(FLOW_OUTGOING) / (1024 * 1024));
}

CON_COMMAND(net_channels, "Shows net channel info") {
  int numChannels = s_NetChannels.Count();

  if (numChannels == 0) {
    ConMsg("No active net channels.\n");
    return;
  }

  AUTO_LOCK_FM(s_NetChannels);
  for (int i = 0; i < numChannels; i++) {
    NET_PrintChannelStatus(s_NetChannels[i]);
  }
}

CON_COMMAND(net_start, "Inits multiplayer network sockets") {
  net_multiplayer = true;
  NET_Config();
}

CON_COMMAND(net_status, "Shows current network status") {
  AUTO_LOCK_FM(s_NetChannels);
  int numChannels = s_NetChannels.Count();

  ConMsg("Net status for host %s:\n", net_local_adr.ToString(true));

  ConMsg("- Config: %s, %s, %i connections\n",
         net_multiplayer ? "Multiplayer" : "Singleplayer",
         net_dedicated ? "dedicated" : "listen", numChannels);

  ConMsg(
      "- Ports: Client %u, Server %u, HLTV %u, Matchmaking %u, Systemlink %u\n",
      net_sockets[NS_CLIENT].nPort, net_sockets[NS_SERVER].nPort,
      net_sockets[NS_HLTV].nPort, net_sockets[NS_MATCHMAKING].nPort,
      net_sockets[NS_SYSTEMLINK].nPort);

  if (numChannels <= 0) {
    return;
  }

  // gather statistics:

  float avgLatencyOut = 0;
  float avgLatencyIn = 0;
  float avgPacketsOut = 0;
  float avgPacketsIn = 0;
  float avgLossOut = 0;
  float avgLossIn = 0;
  float avgDataOut = 0;
  float avgDataIn = 0;

  for (int i = 0; i < numChannels; i++) {
    CNetChan *chan = s_NetChannels[i];

    avgLatencyOut += chan->GetAvgLatency(FLOW_OUTGOING);
    avgLatencyIn += chan->GetAvgLatency(FLOW_INCOMING);

    avgLossIn += chan->GetAvgLoss(FLOW_INCOMING);
    avgLossOut += chan->GetAvgLoss(FLOW_OUTGOING);

    avgPacketsIn += chan->GetAvgPackets(FLOW_INCOMING);
    avgPacketsOut += chan->GetAvgPackets(FLOW_OUTGOING);

    avgDataIn += chan->GetAvgData(FLOW_INCOMING);
    avgDataOut += chan->GetAvgData(FLOW_OUTGOING);
  }

  ConMsg("- Latency: avg out %.2fs, in %.2fs\n", avgLatencyOut / numChannels,
         avgLatencyIn / numChannels);
  ConMsg("- Loss:    avg out %.1f, in %.1f\n", avgLossOut / numChannels,
         avgLossIn / numChannels);
  ConMsg("- Packets: net total out  %.1f/s, in %.1f/s\n", avgPacketsOut,
         avgPacketsIn);
  ConMsg("           per client out %.1f/s, in %.1f/s\n",
         avgPacketsOut / numChannels, avgPacketsIn / numChannels);
  ConMsg("- Data:    net total out  %.1f, in %.1f kB/s\n", avgDataOut / 1024.0f,
         avgDataIn / 1024.0f);
  ConMsg("           per client out %.1f, in %.1f kB/s\n",
         (avgDataOut / numChannels) / 1024.0f,
         (avgDataIn / numChannels) / 1024.0f);
}

//-----------------------------------------------------------------------------
// Purpose: Generic buffer compression from source into dest
// Input  : *dest -
//			*destLen -
//			*source -
//			sourceLen -
// Output : int
//-----------------------------------------------------------------------------
bool NET_BufferToBufferCompress(char *dest, unsigned int *destLen, char *source,
                                unsigned int sourceLen) {
  Assert(dest);
  Assert(destLen);
  Assert(source);

  Q_memcpy(dest, source, sourceLen);
  CLZSS s;
  unsigned int uCompressedLen = 0;
  uint8_t *pbOut = s.Compress((uint8_t *)source, sourceLen, &uCompressedLen);
  if (pbOut && uCompressedLen > 0 && uCompressedLen <= *destLen) {
    Q_memcpy(dest, pbOut, uCompressedLen);
    *destLen = uCompressedLen;
    heap_free(pbOut);
  } else {
    heap_free(pbOut);
    Q_memcpy(dest, source, sourceLen);
    *destLen = sourceLen;
    return false;
  }
  return true;
}

//-----------------------------------------------------------------------------
// Purpose: Generic buffer decompression from source into dest
// Input  : *dest -
//			*destLen -
//			*source -
//			sourceLen -
// Output : int
//-----------------------------------------------------------------------------
bool NET_BufferToBufferDecompress(char *dest, unsigned int *destLen,
                                  char *source, unsigned int sourceLen) {
  CLZSS s;
  if (s.IsCompressed((uint8_t *)source)) {
    unsigned int uDecompressedLen = s.GetActualSize((uint8_t *)source);
    if (uDecompressedLen > *destLen) {
      Sys_Error(
          "NET_BufferToBufferDecompress with improperly sized dest buffer (%u "
          "in, %u needed)\n",
          *destLen, uDecompressedLen);
      return false;
    } else {
      *destLen = s.Uncompress((uint8_t *)source, (uint8_t *)dest);
    }
  } else {
    Q_memcpy(dest, source, sourceLen);
    *destLen = sourceLen;
  }

  return true;
}
