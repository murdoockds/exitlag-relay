// Hands ExitLag's tray icon to exitlag-tray, which shows it as a
// StatusNotifierItem with a DBusMenu, so the desktop's tray draws the menu
// itself, where it was clicked.
//
// Wine's own tray item (Proton's StatusNotifierItem) only offers ContextMenu
// (x, y): Wine then has ExitLag open a Win32 popup menu at those coordinates,
// taken as Windows screen coordinates. Trays pass what they like there
// (Noctalia: something relative to its tray widget), and Windows coordinates
// don't match the compositor's layout anyway, so the menu opened on another
// monitor.
//
// With exitlag-tray running (EXITLAG_TRAY_PORT set and the helper saying
// hello), ExitLag's Shell_NotifyIconW calls don't reach Wine; we keep the
// icon, tooltip and callback, and talk to the helper over UDP on 127.0.0.1:
//
//   helper -> us: HELLO (every second), ACTIVATE, SECONDARY, MENU, CLICK <id>
//   us -> helper: STATE <0|1>\n<tooltip>   ICON <w> <h>\n<ARGB32, big-endian>
//                 ITEMS\n<id>\t<parent>\t<flags>\t<label>\n...   (reply to MENU)
//
// MENU and CLICK work by sending ExitLag's tray window the WM_CONTEXTMENU a
// right-click would; Qt then calls TrackPopupMenuEx, which we answer without
// showing anything: with the menu's items (MENU) or the chosen command (CLICK).
// Without the helper everything goes to Wine as before.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void logmsg(const char *fmt, ...);

// ---- hooks on qwindows.dll's imports (it's loaded after us, as a Qt plugin)

typedef BOOL(WINAPI *Shell_NotifyIconW_t)(DWORD, PNOTIFYICONDATAW);
typedef BOOL(WINAPI *TrackPopupMenuEx_t)(HMENU, UINT, int, int, HWND, LPTPMPARAMS);
typedef BOOL(WINAPI *TrackPopupMenu_t)(HMENU, UINT, int, int, int, HWND, const RECT *);
static Shell_NotifyIconW_t realShellNotify;
static TrackPopupMenuEx_t realTrackEx;
static TrackPopupMenu_t realTrack;

// ---- state

static CRITICAL_SECTION lock;
static SOCKET sock = INVALID_SOCKET;
static struct sockaddr_in helper;
static DWORD helperSeen; // tick of the last HELLO (0: never)

static struct {
	int owned;       // registered with us (not Wine)
	int passthrough; // registered with Wine (no helper at the time)
	HWND hwnd;
	UINT id, callback, version;
	WCHAR tip[128];
	BYTE *argb;
	int w, h;
} tray;

enum { MenuNormal, MenuCapture, MenuChoose };
static volatile LONG menuMode;
static int menuChoice;
static HANDLE menuDone;
static char menuText[16384];

static int helperAlive(void)
{
	return helperSeen && GetTickCount() - helperSeen < 5000;
}

static void sendHelper(const void *buf, int len)
{
	if (helperSeen)
		sendto(sock, buf, len, 0, (struct sockaddr *)&helper, sizeof helper);
}

static void sendState(void)
{
	char buf[512];
	int n = snprintf(buf, sizeof buf, "STATE %d\n", tray.owned);
	n += WideCharToMultiByte(CP_UTF8, 0, tray.tip, -1, buf + n, sizeof buf - n, NULL, NULL);
	sendHelper(buf, n > 0 ? n - 1 : 0); // without the NUL
	if (tray.owned && tray.argb && tray.w * tray.h * 4 < 60000) {
		int size = tray.w * tray.h * 4;
		char *msg = malloc(64 + size);
		int hn = snprintf(msg, 64, "ICON %d %d\n", tray.w, tray.h);
		memcpy(msg + hn, tray.argb, size);
		sendHelper(msg, hn + size);
		free(msg);
	}
}

// iconToArgb converts an HICON to the StatusNotifierItem pixmap format.
static void iconToArgb(HICON icon)
{
	ICONINFO ii;
	if (!icon || !GetIconInfo(icon, &ii))
		return;
	BITMAP bm;
	HBITMAP color = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
	if (GetObjectA(color, sizeof bm, &bm)) {
		int w = bm.bmWidth, h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
		BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof bi.bmiHeader, .biWidth = w, .biHeight = -h,
		                               .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB}};
		BYTE *bgra = malloc(w * h * 4), *mask = malloc(w * h * 4);
		HDC dc = GetDC(NULL);
		if (bgra && mask && GetDIBits(dc, color, 0, h, bgra, &bi, DIB_RGB_COLORS)) {
			int anyAlpha = 0;
			for (int i = 0; i < w * h; i++)
				anyAlpha |= bgra[i * 4 + 3];
			if (!anyAlpha && ii.hbmMask) // old-style icon: alpha from the AND mask
				GetDIBits(dc, ii.hbmMask, 0, h, mask, &bi, DIB_RGB_COLORS);
			BYTE *argb = malloc(w * h * 4);
			for (int i = 0; argb && i < w * h; i++) {
				BYTE a = anyAlpha ? bgra[i * 4 + 3] : (mask[i * 4] ? 0 : 255);
				argb[i * 4 + 0] = a;
				argb[i * 4 + 1] = bgra[i * 4 + 2];
				argb[i * 4 + 2] = bgra[i * 4 + 1];
				argb[i * 4 + 3] = bgra[i * 4 + 0];
			}
			if (argb) {
				free(tray.argb);
				tray.argb = argb;
				tray.w = w;
				tray.h = h;
			}
		}
		ReleaseDC(NULL, dc);
		free(bgra);
		free(mask);
	}
	if (ii.hbmColor)
		DeleteObject(ii.hbmColor);
	if (ii.hbmMask)
		DeleteObject(ii.hbmMask);
}

static void takeData(PNOTIFYICONDATAW d)
{
	tray.hwnd = d->hWnd;
	tray.id = d->uID;
	if (d->uFlags & NIF_MESSAGE)
		tray.callback = d->uCallbackMessage;
	if (d->uFlags & NIF_TIP)
		lstrcpynW(tray.tip, d->szTip, sizeof tray.tip / sizeof *tray.tip);
	if (d->uFlags & NIF_ICON)
		iconToArgb(d->hIcon);
}

static BOOL WINAPI hookShellNotify(DWORD msg, PNOTIFYICONDATAW d)
{
	EnterCriticalSection(&lock);
	if (tray.passthrough || (!tray.owned && !helperAlive())) {
		if (msg == NIM_ADD)
			tray.passthrough = 1;
		if (d)
			takeData(d);
		if (msg == NIM_SETVERSION && d)
			tray.version = d->uVersion;
		if (msg == NIM_DELETE)
			tray.passthrough = 0;
		LeaveCriticalSection(&lock);
		return realShellNotify(msg, d);
	}
	switch (msg) {
	case NIM_ADD:
	case NIM_MODIFY:
		takeData(d);
		tray.owned = 1;
		break;
	case NIM_DELETE:
		tray.owned = 0;
		break;
	case NIM_SETVERSION:
		tray.version = d->uVersion;
		break;
	}
	sendState();
	LeaveCriticalSection(&lock);
	return TRUE;
}

// ---- menus

static void appendMenu(HMENU m, int parent, int *nextSub, size_t *len)
{
	int n = GetMenuItemCount(m);
	for (int i = 0; i < n; i++) {
		WCHAR wlabel[256] = L"";
		MENUITEMINFOW mii = {.cbSize = sizeof mii,
		                     .fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING,
		                     .dwTypeData = wlabel,
		                     .cch = 255};
		if (!GetMenuItemInfoW(m, i, TRUE, &mii))
			continue;
		char label[512];
		WideCharToMultiByte(CP_UTF8, 0, wlabel, -1, label, sizeof label, NULL, NULL);
		int id = mii.hSubMenu ? (*nextSub)++ : (int)mii.wID;
		unsigned flags = (mii.fType & MFT_SEPARATOR ? 1 : 0) | (mii.fState & MFS_DISABLED ? 2 : 0) |
		                 (mii.fState & MFS_CHECKED ? 4 : 0) | (mii.fType & MFT_RADIOCHECK ? 8 : 0) |
		                 (mii.hSubMenu ? 16 : 0);
		int w = snprintf(menuText + *len, sizeof menuText - *len, "%d\t%d\t%u\t%s\n", id, parent, flags, label);
		if (w < 0 || *len + w >= sizeof menuText)
			return;
		*len += w;
		if (mii.hSubMenu)
			appendMenu(mii.hSubMenu, id, nextSub, len);
	}
}

static BOOL answerMenu(HMENU m, UINT flags, HWND owner)
{
	if (menuMode == MenuCapture) {
		size_t len = (size_t)snprintf(menuText, sizeof menuText, "ITEMS\n");
		int nextSub = 1000000; // ids for submenus, which have no command id
		appendMenu(m, 0, &nextSub, &len);
		SetEvent(menuDone);
		return 0;
	}
	int id = menuChoice;
	SetEvent(menuDone);
	if (flags & TPM_RETURNCMD)
		return id;
	if (id)
		PostMessageW(owner, WM_COMMAND, id, 0);
	return TRUE;
}

static BOOL WINAPI hookTrackEx(HMENU m, UINT flags, int x, int y, HWND owner, LPTPMPARAMS params)
{
	if (menuMode == MenuNormal)
		return realTrackEx(m, flags, x, y, owner, params);
	return answerMenu(m, flags, owner);
}

static BOOL WINAPI hookTrack(HMENU m, UINT flags, int x, int y, int reserved, HWND owner, const RECT *rect)
{
	if (menuMode == MenuNormal)
		return realTrack(m, flags, x, y, reserved, owner, rect);
	return answerMenu(m, flags, owner);
}

// postTray sends the tray window the notification a click would.
static void postTray(UINT event)
{
	if (!tray.hwnd || !tray.callback)
		return;
	if (tray.version >= NOTIFYICON_VERSION_4)
		PostMessageW(tray.hwnd, tray.callback, 0, MAKELPARAM(event, tray.id));
	else
		PostMessageW(tray.hwnd, tray.callback, tray.id, event);
}

// showMenu makes ExitLag build its tray menu and answers it in mode.
static int showMenu(LONG mode, int choice)
{
	ResetEvent(menuDone);
	menuChoice = choice;
	InterlockedExchange(&menuMode, mode);
	postTray(WM_CONTEXTMENU);
	int ok = WaitForSingleObject(menuDone, 2000) == WAIT_OBJECT_0;
	InterlockedExchange(&menuMode, MenuNormal);
	return ok;
}

// ---- helper link

static DWORD WINAPI trayMain(void *arg)
{
	int port = (int)(INT_PTR)arg;
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port),
	                        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	if (sock == INVALID_SOCKET || bind(sock, (struct sockaddr *)&a, sizeof a)) {
		logmsg("tray: can't listen on 127.0.0.1:%d (error %d)", port, WSAGetLastError());
		return 0;
	}
	char buf[1024];
	for (;;) {
		struct sockaddr_in from;
		int fl = sizeof from;
		int n = recvfrom(sock, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
		if (n <= 0)
			continue;
		buf[n] = 0;
		if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
			continue;
		if (!strcmp(buf, "HELLO")) {
			EnterCriticalSection(&lock);
			int first = !helperAlive();
			helper = from;
			helperSeen = GetTickCount();
			if (first) {
				logmsg("tray: exitlag-tray connected");
				if (tray.passthrough) {
					// Registered with Wine before the helper came up: take it over.
					NOTIFYICONDATAW d = {.cbSize = sizeof d, .hWnd = tray.hwnd, .uID = tray.id};
					realShellNotify(NIM_DELETE, &d);
					tray.passthrough = 0;
					tray.owned = 1;
				}
				sendState();
			}
			LeaveCriticalSection(&lock);
		} else if (!strcmp(buf, "ACTIVATE")) {
			postTray(NIN_SELECT);
		} else if (!strcmp(buf, "SECONDARY")) {
			postTray(WM_MBUTTONUP);
		} else if (!strcmp(buf, "MENU")) {
			if (showMenu(MenuCapture, 0))
				sendHelper(menuText, (int)strlen(menuText));
			else
				logmsg("tray: ExitLag didn't open its menu");
		} else if (!strncmp(buf, "CLICK ", 6)) {
			int id = atoi(buf + 6);
			if (!showMenu(MenuChoose, id))
				logmsg("tray: menu command %d not delivered", id);
		}
	}
	return 0;
}

static void *hookImport(HMODULE module, const char *dll, const char *name, void *fn)
{
	BYTE *base = (BYTE *)module;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress)
		return NULL;
	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
		if (_stricmp((char *)(base + d->Name), dll))
			continue;
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
		IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
		for (; names->u1.AddressOfData; names++, addrs++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) ||
			    strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, name))
				continue;
			void *orig = (void *)addrs->u1.Function;
			DWORD old;
			VirtualProtect(&addrs->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
			addrs->u1.Function = (ULONG_PTR)fn;
			VirtualProtect(&addrs->u1.Function, sizeof(void *), old, &old);
			return orig;
		}
	}
	return NULL;
}

static void hookQwindows(HMODULE m)
{
	void *p;
	if ((p = hookImport(m, "shell32.dll", "Shell_NotifyIconW", (void *)hookShellNotify)))
		realShellNotify = p;
	if ((p = hookImport(m, "user32.dll", "TrackPopupMenuEx", (void *)hookTrackEx)))
		realTrackEx = p;
	if ((p = hookImport(m, "user32.dll", "TrackPopupMenu", (void *)hookTrack)))
		realTrack = p;
	logmsg("tray: hooked qwindows.dll (%s)", realShellNotify ? "ok" : "no Shell_NotifyIconW import");
}

// LdrRegisterDllNotification, to hook qwindows.dll as soon as it's mapped.
typedef struct {
	ULONG Flags;
	const UNICODE_STRING *FullDllName, *BaseDllName;
	PVOID DllBase;
	ULONG SizeOfImage;
} DLL_LOADED;
typedef VOID(CALLBACK *DLL_NOTIFY)(ULONG reason, const DLL_LOADED *data, PVOID ctx);
typedef LONG(NTAPI *LdrRegisterDllNotification_t)(ULONG, DLL_NOTIFY, PVOID, PVOID *);

static VOID CALLBACK dllLoaded(ULONG reason, const DLL_LOADED *data, PVOID ctx)
{
	(void)ctx;
	if (reason != 1 || !data->BaseDllName || !data->BaseDllName->Buffer)
		return;
	if (data->BaseDllName->Length / 2 == 12 && !_wcsnicmp(data->BaseDllName->Buffer, L"qwindows.dll", 12))
		hookQwindows((HMODULE)data->DllBase);
}

void trayInstall(void)
{
	char port[16];
	if (!GetEnvironmentVariableA("EXITLAG_TRAY_PORT", port, sizeof port))
		return; // no exitlag-tray: Wine's tray item
	InitializeCriticalSection(&lock);
	menuDone = CreateEventA(NULL, TRUE, FALSE, NULL);
	LdrRegisterDllNotification_t reg =
	    (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification");
	PVOID cookie;
	HMODULE qw = GetModuleHandleA("qwindows.dll");
	if (qw)
		hookQwindows(qw);
	else if (!reg || reg(0, dllLoaded, NULL, &cookie)) {
		logmsg("tray: no LdrRegisterDllNotification");
		return;
	}
	CloseHandle(CreateThread(NULL, 0, trayMain, (void *)(INT_PTR)atoi(port), 0, NULL));
}
