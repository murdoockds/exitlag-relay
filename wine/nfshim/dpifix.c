// Keeps ExitLag's main window at the right size for the monitor it's on.
//
// Under Proton's Wayland driver a window's pixels are its output's pixels,
// and Wine reports 96 DPI everywhere; exitlag-wine gives Qt each monitor's
// scale instead (QT_SCREEN_SCALE_FACTORS, by monitor name). Moving the window
// between outputs then just works: Wine rescales it to keep its size on
// screen and Qt switches scale. But Qt sizes a new window for the primary
// screen, and niri maps it on whichever output is focused; when those differ
// (or the window is re-shown from the tray on another output), nothing
// rescales it, and ExitLag comes up at the wrong scale or cut off. Then we
// resize it by the ratio of the two scales, like a WM_DPICHANGED would.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void logmsg(const char *fmt, ...);

enum { maxMonitors = 16 };

static struct {
	char gdi[CCHDEVICENAME]; // \\.\DISPLAYn
	char name[64];           // EDID monitor name, as Qt's QScreen::name()
} monitors[maxMonitors];
static int nMonitors;

static void loadMonitorNames(void)
{
	UINT32 np = 0, nm = 0;
	nMonitors = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np)
		return;
	DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
	DISPLAYCONFIG_MODE_INFO *modes = calloc(nm ? nm : 1, sizeof *modes);
	if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
		for (UINT32 i = 0; i < np && nMonitors < maxMonitors; i++) {
			DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {.header = {
			    .type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, .size = sizeof src,
			    .adapterId = paths[i].sourceInfo.adapterId, .id = paths[i].sourceInfo.id}};
			DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {.header = {
			    .type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, .size = sizeof tgt,
			    .adapterId = paths[i].targetInfo.adapterId, .id = paths[i].targetInfo.id}};
			if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS ||
			    DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS)
				continue;
			WideCharToMultiByte(CP_UTF8, 0, src.viewGdiDeviceName, -1, monitors[nMonitors].gdi,
			                    sizeof monitors[nMonitors].gdi, NULL, NULL);
			WideCharToMultiByte(CP_UTF8, 0, tgt.monitorFriendlyDeviceName, -1, monitors[nMonitors].name,
			                    sizeof monitors[nMonitors].name, NULL, NULL);
			nMonitors++;
		}
	}
	free(paths);
	free(modes);
}

// scaleOf returns the QT_SCREEN_SCALE_FACTORS entry for a monitor (1 if none).
static double scaleOf(HMONITOR mon)
{
	MONITORINFOEXA mi = {.cbSize = sizeof mi};
	char factors[1024];
	if (!GetMonitorInfoA(mon, (MONITORINFO *)&mi) ||
	    !GetEnvironmentVariableA("QT_SCREEN_SCALE_FACTORS", factors, sizeof factors))
		return 1;
	const char *name = NULL;
	for (int i = 0; i < nMonitors; i++)
		if (!strcmp(monitors[i].gdi, mi.szDevice))
			name = monitors[i].name;
	if (!name)
		return 1;
	for (char *e = strtok(factors, ";"); e; e = strtok(NULL, ";")) {
		char *eq = strrchr(e, '=');
		if (eq && (size_t)(eq - e) == strlen(name) && !strncmp(e, name, eq - e))
			return atof(eq + 1) > 0 ? atof(eq + 1) : 1;
	}
	return 1;
}

static HWND mainWindow(void)
{
	HWND w = NULL;
	while ((w = FindWindowExA(NULL, w, NULL, "ExitLag"))) {
		DWORD pid;
		char cls[64];
		GetWindowThreadProcessId(w, &pid);
		GetClassNameA(w, cls, sizeof cls);
		if (pid == GetCurrentProcessId() && !strncmp(cls, "Qt", 2) && IsWindowVisible(w))
			return w;
	}
	return NULL;
}

static DWORD WINAPI dpifixMain(void *arg)
{
	(void)arg;
	char factors[8];
	if (!GetEnvironmentVariableA("QT_SCREEN_SCALE_FACTORS", factors, sizeof factors))
		return 0; // no per-monitor scales: nothing to keep in sync
	HWND seen = NULL;
	HMONITOR lastMon = NULL;
	SIZE lastSize = {0};
	for (;;) {
		Sleep(100);
		HWND w = mainWindow();
		if (!w) {
			seen = NULL; // hidden (tray) or not created yet
			continue;
		}
		RECT r;
		GetWindowRect(w, &r);
		SIZE size = {r.right - r.left, r.bottom - r.top};
		HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
		double from;
		if (w != seen) {
			// Newly shown: Qt sized it for the primary screen.
			if (!nMonitors)
				loadMonitorNames();
			POINT origin = {0, 0};
			from = scaleOf(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
			seen = w;
		} else if (mon != lastMon && size.cx == lastSize.cx && size.cy == lastSize.cy) {
			// Moved to another output without Wine rescaling it (yet):
			// give Wine a moment before deciding.
			Sleep(300);
			GetWindowRect(w, &r);
			size = (SIZE){r.right - r.left, r.bottom - r.top};
			mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
			from = (size.cx == lastSize.cx && size.cy == lastSize.cy) ? scaleOf(lastMon) : 0;
		} else {
			from = 0;
		}
		double to = scaleOf(mon);
		if (from > 0 && fabs(to / from - 1) > 0.01) {
			int cx = (int)lround(size.cx * to / from), cy = (int)lround(size.cy * to / from);
			logmsg("dpifix: window at scale %.2f on a %.2f monitor, %ldx%ld -> %dx%d", from, to, size.cx,
			       size.cy, cx, cy);
			SetWindowPos(w, NULL, 0, 0, cx, cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			GetWindowRect(w, &r);
			size = (SIZE){r.right - r.left, r.bottom - r.top};
		}
		lastMon = mon;
		lastSize = size;
	}
	return 0;
}

void dpifixStart(void)
{
	CloseHandle(CreateThread(NULL, 0, dpifixMain, NULL, 0, NULL));
}
