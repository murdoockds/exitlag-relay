// Stand-in for a game's process: ExitLag recognizes games by process name,
// so nfapi.dll runs a copy of this named after the game's .exe and reports
// its PID for the game's sockets.
#include <windows.h>

int WINAPI WinMain(HINSTANCE a, HINSTANCE b, LPSTR c, int d)
{
	(void)a, (void)b, (void)c, (void)d;
	Sleep(INFINITE);
	return 0;
}
