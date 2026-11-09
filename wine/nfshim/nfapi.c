// nfapi.dll replacement for running ExitLag under Wine.
//
// ExitLag diverts game traffic with NetFilter SDK: nfapi.dll talks to the
// nfextlag.sys WFP driver, which Wine can't load. This DLL exports the 18
// nfapi functions ExitLag imports and stands in for the driver. The "game
// sockets" it reports are the game's UDP sockets on the Linux host, which
// exitlag-relayd forwards to us over its wire protocol (internal/wire): one
// endpoint per game source port.
//
//   host -> us (Data):  handler->udpSend, or straight out when ExitLag has
//                       disabled filtering for the endpoint
//   nf_udpPostSend:     sent from the endpoint's own Winsock socket
//   that socket's replies: handler->udpReceive, or straight back to the host
//   nf_udpPostReceive:  back to the host (to the game)
//
// ExitLag recognizes games by process name, so endpoints carry the PID of an
// idle stand-in process named after the game's .exe.
//
// Settings come from C:\Relay\nfshim.ini (key=value lines):
//   host=127.0.0.1:27999      exitlag-relayd's address
//   game=cs2.exe              exe name of the stand-in process, used until
//                             exitlag-relayd names the selected game itself
//   profile=1                 log where busy threads spend their time (profile.c)
//   windiag=1                 log ExitLag's window geometry changes (windiag.c)
//   ipv6=1                    let ExitLag use IPv6 (ipv4only.c; read at start)
// The file is re-read every few seconds; a log goes to C:\Relay\nfshim.log.

#define WIN32_LEAN_AND_MEAN
// Windows' fd_set is a counted array; CS2 alone opens ~90 game ports.
#define FD_SETSIZE 1024
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---- NetFilter SDK types (nfdriver.h / nfapi.h, structs are packed) ----

typedef uint64_t ENDPOINT_ID;
typedef int NF_STATUS;
enum {
	NF_STATUS_SUCCESS = 0,
	NF_STATUS_FAIL = -1,
	NF_STATUS_INVALID_ENDPOINT_ID = -2,
	NF_STATUS_NOT_INITIALIZED = -3,
};

#pragma pack(push, 1)
typedef struct {
	unsigned long processId;
	unsigned short ip_family;
	unsigned char localAddress[28];
} NF_UDP_CONN_INFO;

typedef struct {
	unsigned long flags;
	long optionsLength;
	unsigned char options[1];
} NF_UDP_OPTIONS;

typedef struct {
	uint64_t inLimit, outLimit;
} NF_FLOWCTL_DATA;

typedef struct {
	uint64_t inBytes, outBytes;
} NF_FLOWCTL_STAT;

// NF_RULE followed by a MAX_PATH process name; 0x283 bytes in this build.
typedef struct {
	int protocol;
	unsigned long processId;
	unsigned char direction;
	unsigned short localPort, remotePort, ip_family;
	unsigned char localIp[16], localMask[16], remoteIp[16], remoteMask[16];
	unsigned long filteringFlag;
	wchar_t processName[MAX_PATH];
	unsigned char rest[0x283 - 0x53 - 2 * MAX_PATH];
} NF_RULE_EX;
#pragma pack(pop)

// class NF_EventHandler: 16 virtuals, no destructor (ExitLag's NfEventHandler
// vtable has exactly 16 slots).
enum {
	EV_threadStart, EV_threadEnd,
	EV_tcpConnectRequest, EV_tcpConnected, EV_tcpClosed, EV_tcpReceive,
	EV_tcpSend, EV_tcpCanReceive, EV_tcpCanSend,
	EV_udpCreated, EV_udpConnectRequest, EV_udpClosed, EV_udpReceive,
	EV_udpSend, EV_udpCanReceive, EV_udpCanSend,
};
typedef struct { void **vtbl; } Handler;
typedef void (*fn_void)(Handler *);
typedef void (*fn_conn)(Handler *, ENDPOINT_ID, NF_UDP_CONN_INFO *);
typedef void (*fn_data)(Handler *, ENDPOINT_ID, const unsigned char *, const char *, int, NF_UDP_OPTIONS *);

// ---- wire protocol (internal/wire) ----

enum { TypeData = 1, TypeHello = 2, TypeAck = 3, TypeGame = 4, HeaderLen = 9 };
static const char Magic[4] = {'C', 'S', '2', 'R'};

// ---- state ----

typedef struct Endpoint {
	struct Endpoint *next;
	ENDPOINT_ID id;
	uint16_t hostPort;   // game's source port on the Linux host
	SOCKET sock;         // stands in for the game's socket towards the internet;
	                     // opened on first use (ExitLag tunnels everything it
	                     // boosts itself, so most endpoints never need one)
	int filtering;       // indicate traffic to ExitLag's handler
	unsigned fc;         // flow-control handle (stats only)
	DWORD lastUsed;      // tick of the last datagram from the host
	NF_UDP_CONN_INFO info;
} Endpoint;

static CRITICAL_SECTION lock;
static Handler *handler;
static HANDLE netThread;
static volatile LONG stopping;
static SOCKET hostSock = INVALID_SOCKET;
static struct sockaddr_in hostAddr;
static Endpoint *endpoints;
static ENDPOINT_ID nextId = 1;
static SOCKET graveyard[64];
static int graveyardLen;
static DWORD lastAck;

static char cfgHost[64] = "127.0.0.1:27999";
static char cfgGame[MAX_PATH];  // effective game: hostGame if the daemon sent one, else the ini's
static char fileGame[MAX_PATH];
static char hostGame[MAX_PATH];
static int hostGameSet;
static HANDLE gameProc;
static DWORD gamePid;
static char gameRunning[MAX_PATH];
static HANDLE job;

#define FC_MAX 256
static NF_FLOWCTL_STAT fcStat[FC_MAX];
static int fcUsed[FC_MAX];

static const char *dir = "C:\\Relay";
static FILE *logf;

void wmifixInstall(void);
void profileStart(void);
void vblankInstall(void);
void windiagStart(void);
void dpifixStart(void);
void trayInstall(void);
void ipv4onlyInstall(void);
static int cfgWindiag;
static int cfgProfile;
static int cfgIPv6;

void logmsg(const char *fmt, ...)
{
	if (!logf)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	EnterCriticalSection(&lock);
	fprintf(logf, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(logf, fmt, ap);
	va_end(ap);
	fputc('\n', logf);
	fflush(logf);
	LeaveCriticalSection(&lock);
}

static const char *fmtaddr(const unsigned char *sa, char *buf)
{
	const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
	if (!sa || in->sin_family != AF_INET) {
		strcpy(buf, "?");
		return buf;
	}
	const unsigned char *b = (const unsigned char *)&in->sin_addr;
	sprintf(buf, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], ntohs(in->sin_port));
	return buf;
}

// ---- config and the stand-in game process ----

static void readConfig(void)
{
	char path[MAX_PATH], line[512];
	snprintf(path, sizeof path, "%s\\nfshim.ini", dir);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char game[MAX_PATH] = "";
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		if (!strcmp(line, "host"))
			snprintf(cfgHost, sizeof cfgHost, "%s", eq + 1);
		else if (!strcmp(line, "game"))
			snprintf(game, sizeof game, "%s", eq + 1);
		else if (!strcmp(line, "profile"))
			cfgProfile = atoi(eq + 1);
		else if (!strcmp(line, "windiag"))
			cfgWindiag = atoi(eq + 1);
		else if (!strcmp(line, "ipv6"))
			cfgIPv6 = atoi(eq + 1);
	}
	fclose(f);
	strcpy(fileGame, game);
}

static void pickGame(void)
{
	const char *g = hostGameSet ? hostGame : fileGame;
	if (strpbrk(g, "\\/:*?\"<>|"))
		g = "";
	strcpy(cfgGame, g);
}

static void stopGame(void)
{
	if (gameProc) {
		TerminateProcess(gameProc, 0);
		CloseHandle(gameProc);
		gameProc = NULL;
	}
	gamePid = 0;
	gameRunning[0] = 0;
}

// syncGame keeps one idle process named cfgGame running. It is in a job that
// dies with ExitLag, so it can't outlive us.
static void syncGame(void)
{
	if (gameProc && WaitForSingleObject(gameProc, 0) == WAIT_OBJECT_0)
		stopGame();
	if (!_stricmp(gameRunning, cfgGame) && (gameProc || !cfgGame[0]))
		return;
	stopGame();
	if (!cfgGame[0])
		return;

	char src[MAX_PATH], games[MAX_PATH], exe[MAX_PATH];
	snprintf(src, sizeof src, "%s\\idle.exe", dir);
	snprintf(games, sizeof games, "%s\\games", dir);
	snprintf(exe, sizeof exe, "%s\\%s", games, cfgGame);
	CreateDirectoryA(games, NULL);
	CopyFileA(src, exe, FALSE);

	STARTUPINFOA si = {.cb = sizeof si};
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(exe, NULL, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
	                    NULL, games, &si, &pi)) {
		logmsg("start %s: error %lu", exe, GetLastError());
		strcpy(gameRunning, cfgGame); // don't retry every tick
		return;
	}
	if (job)
		AssignProcessToJobObject(job, pi.hProcess);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	gameProc = pi.hProcess;
	gamePid = pi.dwProcessId;
	strcpy(gameRunning, cfgGame);
	logmsg("stand-in %s running as pid %lu", cfgGame, gamePid);
}

// ---- endpoints ----

static Endpoint *findLocked(ENDPOINT_ID id)
{
	for (Endpoint *e = endpoints; e; e = e->next)
		if (e->id == id)
			return e;
	return NULL;
}

static Endpoint *findPortLocked(uint16_t port)
{
	for (Endpoint *e = endpoints; e; e = e->next)
		if (e->hostPort == port)
			return e;
	return NULL;
}

static void toHost(const unsigned char *remote, uint16_t hostPort, const char *buf, int len)
{
	const struct sockaddr_in *r = (const struct sockaddr_in *)remote;
	if (!r || r->sin_family != AF_INET || len < 0 || len > 65535 - HeaderLen)
		return;
	char out[65536];
	out[0] = TypeData;
	memcpy(out + 1, &r->sin_addr, 4);
	memcpy(out + 5, &r->sin_port, 2); // already big-endian
	out[7] = hostPort >> 8;
	out[8] = hostPort & 0xff;
	memcpy(out + HeaderLen, buf, len);
	sendto(hostSock, out, HeaderLen + len, 0, (struct sockaddr *)&hostAddr, sizeof hostAddr);
}

static void countFc(unsigned fc, int in, int len)
{
	if (fc && fc < FC_MAX) {
		if (in)
			fcStat[fc].inBytes += len;
		else
			fcStat[fc].outBytes += len;
	}
}

static SOCKET newSocket(void)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
		return s;
	struct sockaddr_in any = {.sin_family = AF_INET};
	bind(s, (struct sockaddr *)&any, sizeof any);
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);
	// Windows reports ICMP port-unreachable as WSAECONNRESET on the next
	// recvfrom; don't let one dead remote break the socket.
	BOOL off = FALSE;
	DWORD ret;
	WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);
	return s;
}

// endpointSocket returns endpoint id's socket, opening it on first use, or
// INVALID_SOCKET if the endpoint is gone.
static SOCKET endpointSocket(ENDPOINT_ID id)
{
	EnterCriticalSection(&lock);
	Endpoint *e = findLocked(id);
	SOCKET s = e ? e->sock : INVALID_SOCKET;
	LeaveCriticalSection(&lock);
	if (!e || s != INVALID_SOCKET)
		return s;
	SOCKET n = newSocket();
	EnterCriticalSection(&lock);
	e = findLocked(id);
	if (e && e->sock == INVALID_SOCKET)
		e->sock = n, n = INVALID_SOCKET;
	s = e ? e->sock : INVALID_SOCKET;
	LeaveCriticalSection(&lock);
	if (n != INVALID_SOCKET)
		closesocket(n); // lost a race, or the endpoint closed meanwhile
	return s;
}

static Endpoint *newEndpoint(uint16_t port)
{
	Endpoint *e = calloc(1, sizeof *e);
	if (!e)
		return NULL;
	e->sock = INVALID_SOCKET;
	e->hostPort = port;
	e->filtering = 1;
	e->lastUsed = GetTickCount();
	e->info.processId = gamePid;
	e->info.ip_family = AF_INET;
	struct sockaddr_in *la = (struct sockaddr_in *)e->info.localAddress;
	la->sin_family = AF_INET;
	la->sin_port = htons(port);
	EnterCriticalSection(&lock);
	e->id = nextId++;
	e->next = endpoints;
	endpoints = e;
	LeaveCriticalSection(&lock);
	logmsg("endpoint %llu for game port %u (pid %lu)", e->id, port, gamePid);
	return e;
}

// ---- the network thread ----

static void handleHost(char *buf, int n)
{
	if (n == 5 && buf[0] == TypeAck && !memcmp(buf + 1, Magic, 4)) {
		lastAck = GetTickCount();
		return;
	}
	if (n >= 5 && n - 5 < MAX_PATH && buf[0] == TypeGame && !memcmp(buf + 1, Magic, 4)) {
		char g[MAX_PATH];
		memcpy(g, buf + 5, n - 5);
		g[n - 5] = 0;
		if (!hostGameSet || strcmp(g, hostGame)) {
			logmsg("exitlag-relayd selected game '%s'", g);
			strcpy(hostGame, g);
			hostGameSet = 1;
			pickGame();
			syncGame();
		}
		return;
	}
	if (n < HeaderLen || buf[0] != TypeData)
		return;
	struct sockaddr_in dst = {.sin_family = AF_INET};
	memcpy(&dst.sin_addr, buf + 1, 4);
	memcpy(&dst.sin_port, buf + 5, 2);
	uint16_t port = ((unsigned char)buf[7] << 8) | (unsigned char)buf[8];
	const char *payload = buf + HeaderLen;
	int len = n - HeaderLen;

	EnterCriticalSection(&lock);
	Endpoint *e = findPortLocked(port);
	LeaveCriticalSection(&lock);
	int created = 0;
	if (!e) {
		if (!(e = newEndpoint(port)))
			return;
		created = 1;
	}
	EnterCriticalSection(&lock);
	e->lastUsed = GetTickCount();
	int filtering = e->filtering;
	ENDPOINT_ID id = e->id;
	NF_UDP_CONN_INFO info = e->info;
	LeaveCriticalSection(&lock);

	if (created && handler)
		((fn_conn)handler->vtbl[EV_udpCreated])(handler, id, &info);
	if (filtering && handler) {
		NF_UDP_OPTIONS opt = {0};
		((fn_data)handler->vtbl[EV_udpSend])(handler, id, (unsigned char *)&dst, payload, len, &opt);
	} else {
		SOCKET s = endpointSocket(id);
		if (s != INVALID_SOCKET)
			sendto(s, payload, len, 0, (struct sockaddr *)&dst, sizeof dst);
	}
}

// What the network thread polls: the endpoints that have a socket.
typedef struct {
	ENDPOINT_ID id;
	SOCKET sock;
	uint16_t hostPort;
} Polled;

static void handleEndpoint(const Polled *snap)
{
	char buf[65536];
	for (;;) {
		struct sockaddr_in from;
		int fl = sizeof from;
		int n = recvfrom(snap->sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 0)
			return;
		EnterCriticalSection(&lock);
		Endpoint *e = findLocked(snap->id);
		int filtering = e ? e->filtering : 0;
		if (e)
			countFc(e->fc, 1, n);
		LeaveCriticalSection(&lock);
		if (!e)
			return;
		if (filtering && handler) {
			NF_UDP_OPTIONS opt = {0};
			((fn_data)handler->vtbl[EV_udpReceive])(handler, snap->id, (unsigned char *)&from, buf, n, &opt);
		} else {
			toHost((unsigned char *)&from, snap->hostPort, buf, n);
		}
	}
}

// expire closes endpoints the host hasn't used for 30 s. Sockets are closed
// a round later, so a concurrent nf_udpPostSend never hits a reused handle.
static void expire(void)
{
	for (int i = 0; i < graveyardLen; i++)
		closesocket(graveyard[i]);
	graveyardLen = 0;

	DWORD now = GetTickCount();
	for (;;) {
		EnterCriticalSection(&lock);
		Endpoint **pp = &endpoints, *e = NULL;
		for (; *pp; pp = &(*pp)->next)
			if (now - (*pp)->lastUsed > 30000 &&
			    ((*pp)->sock == INVALID_SOCKET || graveyardLen < 64)) {
				e = *pp;
				*pp = e->next;
				if (e->sock != INVALID_SOCKET)
					graveyard[graveyardLen++] = e->sock;
				break;
			}
		LeaveCriticalSection(&lock);
		if (!e)
			return;
		logmsg("endpoint %llu (port %u) idle, closed", e->id, e->hostPort);
		if (handler)
			((fn_conn)handler->vtbl[EV_udpClosed])(handler, e->id, &e->info);
		free(e);
	}
}

static int resolveHost(void)
{
	char host[64];
	snprintf(host, sizeof host, "%s", cfgHost);
	char *colon = strrchr(host, ':');
	if (!colon)
		return 0;
	*colon = 0;
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(atoi(colon + 1))};
	if (inet_pton(AF_INET, host, &a.sin_addr) != 1)
		return 0;
	hostAddr = a;
	return 1;
}

static DWORD WINAPI netMain(void *arg)
{
	(void)arg;
	if (handler)
		((fn_void)handler->vtbl[EV_threadStart])(handler);
	char buf[65536];
	DWORD lastHello = 0, lastConfig = 0, lastExpire = GetTickCount();
	int wasUp = 0;
	while (!stopping) {
		DWORD now = GetTickCount();
		if (now - lastConfig >= 3000) {
			readConfig();
			pickGame();
			resolveHost();
			syncGame();
			lastConfig = now;
		}
		if (now - lastHello >= 1000) {
			char hello[5] = {TypeHello, 'C', 'S', '2', 'R'};
			sendto(hostSock, hello, 5, 0, (struct sockaddr *)&hostAddr, sizeof hostAddr);
			lastHello = now;
			int up = lastAck && now - lastAck < 5000;
			if (up != wasUp)
				logmsg(up ? "connected to exitlag-relayd at %s" : "lost contact with exitlag-relayd at %s", cfgHost);
			wasUp = up;
		}
		if (now - lastExpire >= 5000) {
			expire();
			lastExpire = now;
		}

		static fd_set rd;
		static Polled snap[FD_SETSIZE - 1];
		FD_ZERO(&rd);
		FD_SET(hostSock, &rd);
		int ns = 0;
		EnterCriticalSection(&lock);
		for (Endpoint *e = endpoints; e && ns < FD_SETSIZE - 1; e = e->next) {
			if (e->sock == INVALID_SOCKET)
				continue;
			snap[ns] = (Polled){e->id, e->sock, e->hostPort};
			FD_SET(e->sock, &rd);
			ns++;
		}
		LeaveCriticalSection(&lock);
		struct timeval tv = {0, 200000};
		if (select(0, &rd, NULL, NULL, &tv) <= 0)
			continue;
		if (FD_ISSET(hostSock, &rd)) {
			for (;;) {
				int n = recv(hostSock, buf, sizeof buf, 0);
				if (n < 0)
					break;
				handleHost(buf, n);
			}
		}
		for (int i = 0; i < ns; i++)
			if (FD_ISSET(snap[i].sock, &rd))
				handleEndpoint(&snap[i]);
	}
	if (handler)
		((fn_void)handler->vtbl[EV_threadEnd])(handler);
	return 0;
}

// ---- exported API ----

NF_STATUS shim_init(const char *driverName, Handler *h)
{
	readConfig();
	pickGame();
	logmsg("nf_init(%s, %p) host=%s game=%s", driverName ? driverName : "(null)", (void *)h, cfgHost, cfgGame);
	if (netThread)
		return NF_STATUS_SUCCESS;
	if (!h)
		return NF_STATUS_FAIL;
	if (!resolveHost()) {
		logmsg("bad host address %s", cfgHost);
		return NF_STATUS_FAIL;
	}
	hostSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (hostSock == INVALID_SOCKET)
		return NF_STATUS_FAIL;
	struct sockaddr_in any = {.sin_family = AF_INET};
	bind(hostSock, (struct sockaddr *)&any, sizeof any);
	u_long nb = 1;
	ioctlsocket(hostSock, FIONBIO, &nb);
	BOOL off = FALSE;
	DWORD ret;
	WSAIoctl(hostSock, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);

	handler = h;
	stopping = 0;
	netThread = CreateThread(NULL, 0, netMain, NULL, 0, NULL);
	return netThread ? NF_STATUS_SUCCESS : NF_STATUS_FAIL;
}

void shim_free(void)
{
	logmsg("nf_free");
	if (!netThread)
		return;
	stopping = 1;
	WaitForSingleObject(netThread, 5000);
	CloseHandle(netThread);
	netThread = NULL;
	EnterCriticalSection(&lock);
	while (endpoints) {
		Endpoint *e = endpoints;
		endpoints = e->next;
		if (e->sock != INVALID_SOCKET)
			closesocket(e->sock);
		free(e);
	}
	LeaveCriticalSection(&lock);
	closesocket(hostSock);
	hostSock = INVALID_SOCKET;
	handler = NULL;
	stopGame();
}

static LONG sendLogged, recvLogged;

NF_STATUS shim_udpPostSend(ENDPOINT_ID id, const unsigned char *remote, const char *buf, int len, NF_UDP_OPTIONS *opt)
{
	(void)opt;
	EnterCriticalSection(&lock);
	Endpoint *e = findLocked(id);
	if (e)
		countFc(e->fc, 0, len);
	LeaveCriticalSection(&lock);
	if (InterlockedIncrement(&sendLogged) <= 5) {
		char a[32];
		logmsg("nf_udpPostSend(%llu, %s, %d)%s", id, fmtaddr(remote, a), len, e ? "" : " unknown endpoint");
	}
	SOCKET s = e ? endpointSocket(id) : INVALID_SOCKET;
	if (s == INVALID_SOCKET)
		return NF_STATUS_INVALID_ENDPOINT_ID;
	sendto(s, buf, len, 0, (const struct sockaddr *)remote, sizeof(struct sockaddr_in));
	return NF_STATUS_SUCCESS;
}

NF_STATUS shim_udpPostReceive(ENDPOINT_ID id, const unsigned char *remote, const char *buf, int len, NF_UDP_OPTIONS *opt)
{
	(void)opt;
	EnterCriticalSection(&lock);
	Endpoint *e = findLocked(id);
	uint16_t port = e ? e->hostPort : 0;
	LeaveCriticalSection(&lock);
	if (InterlockedIncrement(&recvLogged) <= 5) {
		char a[32];
		logmsg("nf_udpPostReceive(%llu, %s, %d)%s", id, fmtaddr(remote, a), len, e ? "" : " unknown endpoint");
	}
	if (!e)
		return NF_STATUS_INVALID_ENDPOINT_ID;
	toHost(remote, port, buf, len);
	return NF_STATUS_SUCCESS;
}

static NF_STATUS setFiltering(ENDPOINT_ID id, int on, const char *what)
{
	EnterCriticalSection(&lock);
	Endpoint *e = findLocked(id);
	if (e)
		e->filtering = on;
	LeaveCriticalSection(&lock);
	logmsg("%s(%llu)%s", what, id, e ? "" : " unknown endpoint");
	return e ? NF_STATUS_SUCCESS : NF_STATUS_INVALID_ENDPOINT_ID;
}

NF_STATUS shim_udpDisableFiltering(ENDPOINT_ID id) { return setFiltering(id, 0, "nf_udpDisableFiltering"); }
NF_STATUS shim_udpEnableFiltering(ENDPOINT_ID id) { return setFiltering(id, 1, "nf_udpEnableFiltering"); }

// The rule is 19 bytes: a 16-byte address, a 2-byte port and a flag byte
// (from the real nfapi.dll). Endpoints here only ever carry game traffic, so
// it's just logged.
NF_STATUS shim_udpAddRemoteAddrRule(ENDPOINT_ID id, const unsigned char *rule)
{
	if (!rule)
		return NF_STATUS_FAIL;
	char hex[64] = "";
	for (int i = 0; i < 19; i++)
		sprintf(hex + 2 * i, "%02x", rule[i]);
	logmsg("nf_udpAddRemoteAddrRule(%llu, %s)", id, hex);
	return NF_STATUS_SUCCESS;
}

NF_STATUS shim_setRulesEx(NF_RULE_EX *rules, int count)
{
	logmsg("nf_setRulesEx(%d rules)", count);
	for (int i = 0; rules && i < count && i < 64; i++) {
		NF_RULE_EX *r = &rules[i];
		char name[MAX_PATH * 3];
		WideCharToMultiByte(CP_UTF8, 0, r->processName, -1, name, sizeof name, NULL, NULL);
		logmsg("  rule %d: proto %d pid %lu dir %u lport %u rport %u family %u flag 0x%lx name '%s'",
		       i, r->protocol, r->processId, r->direction, ntohs(r->localPort), ntohs(r->remotePort),
		       r->ip_family, r->filteringFlag, name);
	}
	return NF_STATUS_SUCCESS;
}

void shim_setIPEventHandler(void *h) { logmsg("nf_setIPEventHandler(%p)", h); }

// Driver extension version; ExitLag compares it with HKLM\SOFTWARE\ExitLag
// NfsdkExtVersion, which its installer writes.
unsigned long shim_getExtensionsVersion(void)
{
	DWORD v = 8, size = sizeof v;
	RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ExitLag", "NfsdkExtVersion", RRF_RT_REG_DWORD, NULL, &v, &size);
	logmsg("nf_getExtensionsVersion -> %lu", v);
	return v;
}

BOOL shim_getProcessNameFromKernel(DWORD pid, wchar_t *buf, DWORD len)
{
	BOOL ok = FALSE;
	HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (p) {
		DWORD n = len;
		ok = QueryFullProcessImageNameW(p, 0, buf, &n);
		CloseHandle(p);
	}
	static LONG logged;
	if (InterlockedIncrement(&logged) <= 20) {
		char name[MAX_PATH * 3] = "";
		if (ok)
			WideCharToMultiByte(CP_UTF8, 0, buf, -1, name, sizeof name, NULL, NULL);
		logmsg("nf_getProcessNameFromKernel(%lu) -> %s", pid, ok ? name : "(failed)");
	}
	return ok;
}

// Flow control: limits are ignored; byte counts are kept for the stats.
NF_STATUS shim_addFlowCtl(NF_FLOWCTL_DATA *data, unsigned *handle)
{
	(void)data;
	EnterCriticalSection(&lock);
	unsigned h = 0;
	for (unsigned i = 1; i < FC_MAX; i++)
		if (!fcUsed[i]) {
			h = i;
			fcUsed[i] = 1;
			memset(&fcStat[i], 0, sizeof fcStat[i]);
			break;
		}
	LeaveCriticalSection(&lock);
	if (handle)
		*handle = h;
	return h ? NF_STATUS_SUCCESS : NF_STATUS_FAIL;
}

NF_STATUS shim_deleteFlowCtl(unsigned h)
{
	if (h && h < FC_MAX)
		fcUsed[h] = 0;
	return NF_STATUS_SUCCESS;
}

NF_STATUS shim_modifyFlowCtl(unsigned h, NF_FLOWCTL_DATA *data)
{
	(void)h, (void)data;
	return NF_STATUS_SUCCESS;
}

NF_STATUS shim_setFlowCtlQueueParams(unsigned h, void *params)
{
	(void)h, (void)params;
	return NF_STATUS_SUCCESS;
}

NF_STATUS shim_getFlowCtlStat(unsigned h, NF_FLOWCTL_STAT *stat)
{
	if (!stat || !h || h >= FC_MAX)
		return NF_STATUS_FAIL;
	EnterCriticalSection(&lock);
	*stat = fcStat[h];
	LeaveCriticalSection(&lock);
	return NF_STATUS_SUCCESS;
}

NF_STATUS shim_setUDPFlowCtl(ENDPOINT_ID id, unsigned h)
{
	EnterCriticalSection(&lock);
	Endpoint *e = findLocked(id);
	if (e)
		e->fc = h;
	LeaveCriticalSection(&lock);
	return e ? NF_STATUS_SUCCESS : NF_STATUS_INVALID_ENDPOINT_ID;
}

NF_STATUS shim_setTCPFlowCtl(ENDPOINT_ID id, unsigned h)
{
	(void)id, (void)h;
	return NF_STATUS_INVALID_ENDPOINT_ID;
}

// Proton's Wine drops QT_* variables from the environment it gives Windows
// programs (Linux Qt settings would break Windows Qt apps), so exitlag-wine
// passes ExitLag's as EXITLAG_QT_*. We run before ExitLag's main(), so
// before Qt reads them.
static void forwardQtEnv(void)
{
	char *env = GetEnvironmentStringsA();
	for (char *e = env; e && *e; e += strlen(e) + 1) {
		if (strncmp(e, "EXITLAG_QT_", 11))
			continue;
		char name[256];
		const char *eq = strchr(e, '=');
		size_t n = eq ? (size_t)(eq - e) : 0;
		if (!eq || n - 8 >= sizeof name)
			continue;
		memcpy(name, e + 8, n - 8); // "QT_..."
		name[n - 8] = 0;
		SetEnvironmentVariableA(name, eq + 1);
		logmsg("environment: %s=%s", name, eq + 1);
	}
	FreeEnvironmentStringsA(env);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
	(void)inst, (void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		InitializeCriticalSection(&lock);
		CreateDirectoryA(dir, NULL);
		char path[MAX_PATH];
		snprintf(path, sizeof path, "%s\\nfshim.log", dir);
		logf = fopen(path, "a");
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
		job = CreateJobObjectA(NULL, NULL);
		if (job) {
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
			li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
		}
		logmsg("nfapi shim loaded into pid %lu", GetCurrentProcessId());
		forwardQtEnv();
		readConfig();
		if (!cfgIPv6)
			ipv4onlyInstall();
		dpifixStart();
		trayInstall();
		wmifixInstall();
		vblankInstall();
		if (cfgProfile)
			profileStart();
		if (cfgWindiag)
			windiagStart();
	} else if (reason == DLL_PROCESS_DETACH && !reserved) {
		stopGame();
	}
	return TRUE;
}
