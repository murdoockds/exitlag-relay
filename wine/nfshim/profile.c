// A sampling profiler for finding busy loops in ExitLag under Wine (ptrace
// is off-limits on this machine, and winedbg can't reach a running prefix).
// Enabled with profile=1 in nfshim.ini.
//
// Every 10 ms each thread that used CPU since the last tick is suspended
// just long enough to copy its RIP and the top of its stack (no locks are
// taken while it is suspended), then resumed. Every 10 s the busiest threads
// are logged with their hottest RIPs and the return addresses most often
// found on their stacks, as module+offset.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void logmsg(const char *fmt, ...);

enum { maxThreads = 256, maxPCs = 64, stackWords = 512, maxRets = 64 };

typedef struct {
	DWORD64 pc;
	unsigned count;
} Hit;

typedef struct {
	DWORD tid;
	HANDLE h;
	ULONGLONG lastCpu;
	unsigned samples;
	Hit pcs[maxPCs];
	Hit rets[maxRets];
} Thread;

static Thread threads[maxThreads];
static int nThreads;

static void addHit(Hit *hits, int n, DWORD64 pc)
{
	int empty = -1, least = 0;
	for (int i = 0; i < n; i++) {
		if (hits[i].pc == pc) {
			hits[i].count++;
			return;
		}
		if (!hits[i].count && empty < 0)
			empty = i;
		if (hits[i].count < hits[least].count)
			least = i;
	}
	int slot = empty >= 0 ? empty : least;
	hits[slot].pc = pc;
	hits[slot].count = 1;
}

// codeModule reports whether p lies in an executable section of a loaded
// image; used to tell return addresses from other stack data.
static int codeModule(DWORD64 p, char *name, size_t len, DWORD64 *off)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery((void *)p, &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE ||
	    !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
		return 0;
	if (name) {
		char path[MAX_PATH] = "?";
		GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof path);
		const char *base = strrchr(path, '\\');
		snprintf(name, len, "%s", base ? base + 1 : path);
		*off = p - (DWORD64)mbi.AllocationBase;
	}
	return 1;
}

static void refreshThreads(void)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return;
	THREADENTRY32 te = {.dwSize = sizeof te};
	DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
	for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
		if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
			continue;
		int known = 0;
		for (int i = 0; i < nThreads; i++)
			if (threads[i].tid == te.th32ThreadID)
				known = 1;
		if (known || nThreads >= maxThreads)
			continue;
		HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
		                      te.th32ThreadID);
		if (!h)
			continue;
		memset(&threads[nThreads], 0, sizeof threads[nThreads]);
		threads[nThreads].tid = te.th32ThreadID;
		threads[nThreads].h = h;
		nThreads++;
	}
	CloseHandle(snap);
}

static ULONGLONG cpuTime(HANDLE h)
{
	FILETIME c, e, k, u;
	if (!GetThreadTimes(h, &c, &e, &k, &u))
		return 0;
	return ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime) +
	       ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime);
}

static void sample(Thread *t)
{
	static DWORD64 stack[stackWords];
	CONTEXT ctx = {.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER};
	if (SuspendThread(t->h) == (DWORD)-1)
		return;
	int ok = GetThreadContext(t->h, &ctx);
	int words = 0;
	if (ok) {
		// Copy what's left of this stack page and the next ones, up to
		// stackWords, without touching guard or unmapped pages.
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery((void *)ctx.Rsp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
		    !(mbi.Protect & PAGE_GUARD)) {
			DWORD64 end = (DWORD64)mbi.BaseAddress + mbi.RegionSize;
			words = (int)((end - ctx.Rsp) / 8);
			if (words > stackWords)
				words = stackWords;
			memcpy(stack, (void *)ctx.Rsp, words * 8);
		}
	}
	ResumeThread(t->h);
	if (!ok)
		return;
	t->samples++;
	addHit(t->pcs, maxPCs, ctx.Rip);
	// Count each code address once per sample.
	DWORD64 seen[32];
	int nseen = 0;
	for (int i = 0; i < words && nseen < 32; i++) {
		DWORD64 v = stack[i];
		if (v < 0x10000 || !codeModule(v, NULL, 0, NULL))
			continue;
		int dup = 0;
		for (int j = 0; j < nseen; j++)
			dup |= seen[j] == v;
		if (dup)
			continue;
		seen[nseen++] = v;
		addHit(t->rets, maxRets, v);
	}
}

static int byCount(const void *a, const void *b)
{
	return (int)((const Hit *)b)->count - (int)((const Hit *)a)->count;
}

static void report(unsigned ticks)
{
	for (int i = 0; i < nThreads; i++) {
		Thread *t = &threads[i];
		if (t->samples * 10 < ticks) // under ~10% of one core
			goto reset;
		wchar_t *wname = NULL;
		char name[128] = "";
		HRESULT(WINAPI * getDesc)(HANDLE, PWSTR *) =
		    (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
		if (getDesc && SUCCEEDED(getDesc(t->h, &wname)) && wname) {
			WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof name, NULL, NULL);
			LocalFree(wname);
		}
		logmsg("profile: thread %04lx '%s' busy in %u of %u ticks", t->tid, name, t->samples, ticks);
		qsort(t->pcs, maxPCs, sizeof(Hit), byCount);
		for (int k = 0; k < 8 && t->pcs[k].count; k++) {
			char mod[64];
			DWORD64 off;
			if (codeModule(t->pcs[k].pc, mod, sizeof mod, &off))
				logmsg("profile:   pc  %5u  %s+0x%llx", t->pcs[k].count, mod, off);
			else
				logmsg("profile:   pc  %5u  0x%llx", t->pcs[k].count, t->pcs[k].pc);
		}
		qsort(t->rets, maxRets, sizeof(Hit), byCount);
		for (int k = 0; k < 16 && t->rets[k].count; k++) {
			char mod[64];
			DWORD64 off;
			if (codeModule(t->rets[k].pc, mod, sizeof mod, &off))
				logmsg("profile:   ret %5u  %s+0x%llx", t->rets[k].count, mod, off);
		}
	reset:
		t->samples = 0;
		memset(t->pcs, 0, sizeof t->pcs);
		memset(t->rets, 0, sizeof t->rets);
	}
}

static DWORD WINAPI profileMain(void *arg)
{
	(void)arg;
	logmsg("profile: sampling every 10 ms");
	timeBeginPeriod(1);
	unsigned ticks = 0;
	for (;;) {
		if (ticks % 200 == 0)
			refreshThreads();
		Sleep(10);
		ticks++;
		for (int i = 0; i < nThreads; i++) {
			ULONGLONG cpu = cpuTime(threads[i].h);
			if (cpu != threads[i].lastCpu) {
				threads[i].lastCpu = cpu;
				sample(&threads[i]);
			}
		}
		if (ticks % 1000 == 0) {
			report(1000);
		}
	}
	return 0;
}

void profileStart(void)
{
	CloseHandle(CreateThread(NULL, 0, profileMain, NULL, 0, NULL));
}
