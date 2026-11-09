// Keeps ExitLag on IPv4.
//
// In the VM, ExitLag only ever had IPv4 (libvirt's NAT network has no IPv6).
// Under Wine it sees the host's IPv6 and tunnelled over it, while its route
// measurements (IcmpSendEcho2Ex) went over IPv4: routes picked for one
// network, game traffic on another. We hide IPv6 from what ExitLag asks the
// system (it still opens IPv6 tunnels to addresses from its own server
// list; see wine/README.md, "Latency"):
//
//   getaddrinfo            AF_UNSPEC lookups return IPv4 only; AF_INET6 none
//   GetAdaptersAddresses   no IPv6 addresses, gateways or prefixes
//   GetIpForwardTable2     no IPv6 routes
//   Icmp6CreateFile        fails (no IPv6 pings)
//
// IPv6 sockets themselves still work: refusing them (WSAEAFNOSUPPORT) made
// ExitLag die of an unhandled C++ exception. ExitLag's own IPv6 sockets are
// logged, to see what still uses IPv6.
//
// These are import hooks in ExitLag's modules (as they load). ipv6=1 in
// nfshim.ini turns this off.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <winternl.h>

void logmsg(const char *fmt, ...);
void *hookImport(HMODULE module, const char *dll, const char *name, void *fn);

typedef INT(WSAAPI *getaddrinfo_t)(PCSTR, PCSTR, const ADDRINFOA *, PADDRINFOA *);
typedef ULONG(WINAPI *GetAdaptersAddresses_t)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
typedef NETIO_STATUS(WINAPI *GetIpForwardTable2_t)(ADDRESS_FAMILY, PMIB_IPFORWARD_TABLE2 *);
typedef SOCKET(WSAAPI *WSASocketW_t)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);

static getaddrinfo_t realGetaddrinfo;
static GetAdaptersAddresses_t realGetAdaptersAddresses;
static GetIpForwardTable2_t realGetIpForwardTable2;
static WSASocketW_t realWSASocketW;

static volatile LONG logged[5];

static int once(int i)
{
	return InterlockedIncrement(&logged[i]) <= 3;
}

static INT WSAAPI hookGetaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA *hints, PADDRINFOA *res)
{
	ADDRINFOA h = {0};
	if (hints)
		h = *hints;
	if (h.ai_family == AF_INET6) {
		if (once(0))
			logmsg("ipv4only: getaddrinfo(%s, AF_INET6) refused", node ? node : "(null)");
		if (res)
			*res = NULL;
		return WSANO_DATA;
	}
	if (h.ai_family == AF_UNSPEC)
		h.ai_family = AF_INET;
	return realGetaddrinfo(node, service, &h, res);
}

static int isV6(SOCKET_ADDRESS *a)
{
	return a->lpSockaddr && a->lpSockaddr->sa_family == AF_INET6;
}

// dropV6 unlinks a list's IPv6 entries. All of GetAdaptersAddresses' address
// lists start with { Length/Flags; Next; SOCKET_ADDRESS Address }.
#define dropV6(head)                                       \
	do {                                                   \
		for (__typeof__(head) *pp = &(head); *pp;)         \
			if (isV6(&(*pp)->Address))                     \
				*pp = (*pp)->Next;                         \
			else                                           \
				pp = &(*pp)->Next;                         \
	} while (0)

static ULONG WINAPI hookGetAdaptersAddresses(ULONG family, ULONG flags, PVOID reserved, PIP_ADAPTER_ADDRESSES out,
                                             PULONG size)
{
	if (family == AF_INET6) {
		if (once(1))
			logmsg("ipv4only: GetAdaptersAddresses(AF_INET6) -> no data");
		return ERROR_NO_DATA;
	}
	ULONG ret = realGetAdaptersAddresses(family, flags, reserved, out, size);
	if (ret != ERROR_SUCCESS || family == AF_INET)
		return ret;
	for (PIP_ADAPTER_ADDRESSES a = out; a; a = a->Next) {
		dropV6(a->FirstUnicastAddress);
		dropV6(a->FirstAnycastAddress);
		dropV6(a->FirstMulticastAddress);
		dropV6(a->FirstDnsServerAddress);
		dropV6(a->FirstPrefix);
		dropV6(a->FirstGatewayAddress);
		dropV6(a->FirstWinsServerAddress);
		a->Flags &= ~IP_ADAPTER_IPV6_ENABLED;
		a->Ipv6IfIndex = 0;
	}
	return ret;
}

static NETIO_STATUS WINAPI hookGetIpForwardTable2(ADDRESS_FAMILY family, PMIB_IPFORWARD_TABLE2 *table)
{
	if (family == AF_UNSPEC)
		family = AF_INET;
	NETIO_STATUS ret = realGetIpForwardTable2(family, table);
	if (ret == NO_ERROR && family == AF_INET6 && table && *table)
		(*table)->NumEntries = 0;
	return ret;
}

static HANDLE WINAPI hookIcmp6CreateFile(void)
{
	if (once(2))
		logmsg("ipv4only: Icmp6CreateFile refused");
	SetLastError(ERROR_NOT_SUPPORTED);
	return INVALID_HANDLE_VALUE;
}

static SOCKET WSAAPI hookWSASocketW(int af, int type, int protocol, LPWSAPROTOCOL_INFOW info, GROUP g, DWORD flags)
{
	if ((af == AF_INET6 || (info && info->iAddressFamily == AF_INET6)) && once(3))
		logmsg("ipv4only: WSASocketW(AF_INET6, %d, %d) from %p", type, protocol, __builtin_return_address(0));
	return realWSASocketW(af, type, protocol, info, g, flags);
}

// The real functions come from GetProcAddress (ipv4onlyInstall), so it
// doesn't matter which module's import is hooked first.
static void hookModule(HMODULE m, int own)
{
	hookImport(m, "ws2_32.dll", "getaddrinfo", (void *)hookGetaddrinfo);
	hookImport(m, "iphlpapi.dll", "GetAdaptersAddresses", (void *)hookGetAdaptersAddresses);
	hookImport(m, "iphlpapi.dll", "GetIpForwardTable2", (void *)hookGetIpForwardTable2);
	hookImport(m, "iphlpapi.dll", "Icmp6CreateFile", (void *)hookIcmp6CreateFile);
	if (own)
		hookImport(m, "ws2_32.dll", "WSASocketW", (void *)hookWSASocketW);
}

// ExitLag's own code (IPv6 sockets logged only for these) and Qt's network
// module.
static const WCHAR *const ownModules[] = {L"ExitLag.exe", L"libexitlag.dll"};
static const WCHAR *const otherModules[] = {L"Qt6Network.dll"};

typedef struct {
	ULONG Flags;
	const UNICODE_STRING *FullDllName, *BaseDllName;
	PVOID DllBase;
	ULONG SizeOfImage;
} DLL_LOADED;
typedef VOID(CALLBACK *DLL_NOTIFY)(ULONG reason, const DLL_LOADED *data, PVOID ctx);
typedef LONG(NTAPI *LdrRegisterDllNotification_t)(ULONG, DLL_NOTIFY, PVOID, PVOID *);

static int nameIs(const UNICODE_STRING *s, const WCHAR *name)
{
	size_t n = wcslen(name);
	return s && s->Buffer && s->Length / 2 == n && !_wcsnicmp(s->Buffer, name, n);
}

static VOID CALLBACK dllLoaded(ULONG reason, const DLL_LOADED *data, PVOID ctx)
{
	(void)ctx;
	if (reason != 1)
		return;
	for (size_t i = 0; i < sizeof ownModules / sizeof *ownModules; i++)
		if (nameIs(data->BaseDllName, ownModules[i]))
			hookModule((HMODULE)data->DllBase, 1);
	for (size_t i = 0; i < sizeof otherModules / sizeof *otherModules; i++)
		if (nameIs(data->BaseDllName, otherModules[i]))
			hookModule((HMODULE)data->DllBase, 0);
}

void ipv4onlyInstall(void)
{
	HMODULE ws2 = GetModuleHandleA("ws2_32.dll"), iph = LoadLibraryA("iphlpapi.dll");
	realGetaddrinfo = (getaddrinfo_t)GetProcAddress(ws2, "getaddrinfo");
	realWSASocketW = (WSASocketW_t)GetProcAddress(ws2, "WSASocketW");
	realGetAdaptersAddresses = (GetAdaptersAddresses_t)GetProcAddress(iph, "GetAdaptersAddresses");
	realGetIpForwardTable2 = (GetIpForwardTable2_t)GetProcAddress(iph, "GetIpForwardTable2");
	if (!realGetaddrinfo || !realWSASocketW || !realGetAdaptersAddresses || !realGetIpForwardTable2) {
		logmsg("ipv4only: missing ws2_32/iphlpapi functions, not installed");
		return;
	}
	int hooked = 0;
	for (size_t i = 0; i < sizeof ownModules / sizeof *ownModules; i++) {
		HMODULE m = GetModuleHandleW(ownModules[i]);
		if (m)
			hookModule(m, 1), hooked++;
	}
	for (size_t i = 0; i < sizeof otherModules / sizeof *otherModules; i++) {
		HMODULE m = GetModuleHandleW(otherModules[i]);
		if (m)
			hookModule(m, 0), hooked++;
	}
	LdrRegisterDllNotification_t reg =
	    (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification");
	PVOID cookie;
	if (reg)
		reg(0, dllLoaded, NULL, &cookie);
	logmsg("ipv4only: hooked %d modules now%s", hooked, reg ? ", the rest as they load" : "");
}
