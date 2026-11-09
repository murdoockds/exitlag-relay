// Logs ExitLag's top-level windows (geometry, styles, size limits, DPI,
// monitor) whenever they change; windiag=1 in nfshim.ini. For debugging
// window sizing and scaling under Wine's Wayland driver.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

void logmsg(const char *fmt, ...);

static char last[16][512];

static BOOL CALLBACK each(HWND w, LPARAM arg)
{
	int *n = (int *)arg;
	DWORD pid;
	GetWindowThreadProcessId(w, &pid);
	if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || *n >= 16)
		return TRUE;
	RECT wr, cr;
	GetWindowRect(w, &wr);
	GetClientRect(w, &cr);
	MINMAXINFO mm = {0};
	SendMessageTimeoutA(w, WM_GETMINMAXINFO, 0, (LPARAM)&mm, SMTO_ABORTIFHUNG, 200, NULL);
	UINT(WINAPI * dpiFor)(HWND) = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
	HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXA mi = {.cbSize = sizeof mi};
	GetMonitorInfoA(mon, (MONITORINFO *)&mi);
	char cls[64], title[64], line[512];
	GetClassNameA(w, cls, sizeof cls);
	GetWindowTextA(w, title, sizeof title);
	snprintf(line, sizeof line,
	         "win %p '%s' [%s] rect %ld,%ld %ldx%ld client %ldx%ld style %08lx ex %08lx min %ldx%ld max %ldx%ld dpi %u on %s (%ldx%ld)",
	         (void *)w, title, cls, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom,
	         (unsigned long)GetWindowLongA(w, GWL_STYLE), (unsigned long)GetWindowLongA(w, GWL_EXSTYLE),
	         mm.ptMinTrackSize.x, mm.ptMinTrackSize.y, mm.ptMaxTrackSize.x, mm.ptMaxTrackSize.y,
	         dpiFor ? dpiFor(w) : 0, mi.szDevice, mi.rcMonitor.right - mi.rcMonitor.left,
	         mi.rcMonitor.bottom - mi.rcMonitor.top);
	if (strcmp(line, last[*n])) {
		strcpy(last[*n], line);
		logmsg("windiag: %s", line);
	}
	(*n)++;
	return TRUE;
}

static DWORD WINAPI windiagMain(void *arg)
{
	(void)arg;
	char *env = GetEnvironmentStringsA();
	static char names[16384];
	size_t used = 0;
	for (char *e = env; e && *e; e += strlen(e) + 1) {
		size_t n = strcspn(e, "=");
		if (!n || used + n + 2 >= sizeof names)
			continue;
		memcpy(names + used, e, n);
		used += n;
		names[used++] = ' ';
	}
	names[used] = 0;
	logmsg("windiag: env names: %s", names);
	FreeEnvironmentStringsA(env);
	for (;;) {
		int n = 0;
		EnumWindows(each, (LPARAM)&n);
		Sleep(1000);
	}
	return 0;
}

void windiagStart(void)
{
	CloseHandle(CreateThread(NULL, 0, windiagMain, NULL, 0, NULL));
}
