// Stops two of ExitLag's threads from spinning.
//
// Qt (and ExitLag's overlay) pace frames with IDXGIOutput::WaitForVBlank.
// DXVK has no vblank event to wait on and busy-waits until the estimated next
// vblank instead, so each of those threads kept a core busy. We replace the
// method, for every output (DXVK's outputs share one vtable), with a sleep
// until the next 60 Hz tick: these threads only drive UI animations.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <dxgi.h>

void logmsg(const char *fmt, ...);

static const IID iidFactory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

static LARGE_INTEGER freq;
static volatile LONG calls;

static HRESULT STDMETHODCALLTYPE waitForVBlank(IDXGIOutput *self)
{
	(void)self;
	InterlockedIncrement(&calls);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	LONGLONG period = freq.QuadPart / 60;
	LONGLONG wait = period - now.QuadPart % period;
	DWORD ms = (DWORD)(wait * 1000 / freq.QuadPart);
	Sleep(ms ? ms : 1);
	return S_OK;
}

static DWORD WINAPI install(void *arg)
{
	(void)arg;
	QueryPerformanceFrequency(&freq);
	HMODULE dxgi = LoadLibraryA("dxgi.dll");
	HRESULT(WINAPI * create)(REFIID, void **) = dxgi ? (void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
	IDXGIFactory1 *factory = NULL;
	if (!create || FAILED(create(&iidFactory1, (void **)&factory))) {
		logmsg("vblank: no DXGI factory");
		return 0;
	}
	IDXGIAdapter1 *adapter = NULL;
	IDXGIOutput *output = NULL;
	for (UINT a = 0; !output && SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, a, &adapter)); a++) {
		IDXGIAdapter1_EnumOutputs(adapter, 0, &output);
		IDXGIAdapter1_Release(adapter);
	}
	IDXGIFactory1_Release(factory);
	if (!output) {
		logmsg("vblank: no DXGI output");
		return 0;
	}
	// IDXGIOutput::WaitForVBlank is slot 10 (IUnknown 3, IDXGIObject 4,
	// GetDesc, GetDisplayModeList, FindClosestMatchingMode).
	void **slot = &(*(void ***)output)[10];
	DWORD old;
	if (VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
		*slot = (void *)waitForVBlank;
		VirtualProtect(slot, sizeof *slot, old, &old);
		logmsg("vblank: WaitForVBlank replaced");
	}
	IDXGIOutput_Release(output);
	for (;;) {
		Sleep(60000);
		LONG n = InterlockedExchange(&calls, 0);
		if (n)
			logmsg("vblank: %ld waits in the last minute", n);
	}
	return 0;
}

void vblankInstall(void)
{
	CloseHandle(CreateThread(NULL, 0, install, NULL, 0, NULL));
}
