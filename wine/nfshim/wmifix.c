// Works around ExitLag's WMI self-check under Wine.
//
// At startup ExitLag runs "SELECT GroupComponent,PartComponent FROM
// Win32_LogicalDiskRootDirectory", a class Wine's wbemprox lacks. We hook
// ExitLag's CoCreateInstance import, and on the WbemLocator it creates, the
// IWbemServices that ConnectServer returns, and answer that query ourselves
// with one row (drive C: and its root directory).
//
// The self-check still reports WMI as broken (for a reason not tracked down;
// every WMI call it makes succeeds), and then asks with a MessageBoxA whether
// to "repair" WMI. That prompt is answered No without being shown.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <wbemcli.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

void logmsg(const char *fmt, ...);

static const CLSID clsidWbemLocator = {0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};

static const IID iidWbemLocator = {0xdc12a687, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};

static const wchar_t *missingClasses[] = {L"Win32_LogicalDiskRootDirectory"};

typedef HRESULT(WINAPI *CoCreateInstance_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef HRESULT(STDMETHODCALLTYPE *ConnectServer_t)(IWbemLocator *, const BSTR, const BSTR, const BSTR,
                                                    const BSTR, long, const BSTR, IWbemContext *, IWbemServices **);
typedef HRESULT(STDMETHODCALLTYPE *ExecQuery_t)(IWbemServices *, const BSTR, const BSTR, long, IWbemContext *,
                                                IEnumWbemClassObject **);

static CoCreateInstance_t realCoCreateInstance;
static ConnectServer_t realConnectServer;
static ExecQuery_t realExecQuery;
typedef HRESULT(STDMETHODCALLTYPE *GetObject_t)(IWbemServices *, const BSTR, long, IWbemContext *,
                                                IWbemClassObject **, IWbemCallResult **);
static GetObject_t realGetObject;

static HRESULT STDMETHODCALLTYPE hookGetObject(IWbemServices *self, const BSTR path, long flags, IWbemContext *ctx,
                                               IWbemClassObject **obj, IWbemCallResult **res)
{
	HRESULT hr = realGetObject(self, path, flags, ctx, obj, res);
	char p[256] = "";
	if (path)
		WideCharToMultiByte(CP_UTF8, 0, path, -1, p, sizeof p, NULL, NULL);
	if (FAILED(hr))
		logmsg("WMI: GetObject '%s' -> 0x%08lx", p, (unsigned long)hr);
	return hr;
}

static void patchSlot(void **slot, void *fn)
{
	DWORD old;
	if (VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
		*slot = fn;
		VirtualProtect(slot, sizeof *slot, old, &old);
	}
}

// ---- a minimal result set: an enumerator over one Win32_LogicalDiskRootDirectory row ----
//
// Both objects are vtables of plain slots; every slot we don't implement
// logs and returns E_NOTIMPL (on x64 the callee never pops arguments, so one
// stub shape fits every signature).

enum { nProps = 2 };
static const wchar_t *propNames[nProps] = {L"GroupComponent", L"PartComponent"};

typedef struct {
	void **vtbl;
	LONG refs;
	int iter;
	BSTR values[nProps];
} FakeObject;

typedef struct {
	void **vtbl;
	LONG refs;
	int pos;
	FakeObject *row;
} FakeEnum;

#define STUB(kind, n) \
	static HRESULT STDMETHODCALLTYPE kind##Stub##n(void *self) \
	{ \
		(void)self; \
		logmsg("WMI: fake " #kind " slot " #n " called (not implemented)"); \
		return E_NOTIMPL; \
	}
#define STUBS(kind) \
	STUB(kind, 0) STUB(kind, 1) STUB(kind, 2) STUB(kind, 3) STUB(kind, 4) STUB(kind, 5) STUB(kind, 6) \
	STUB(kind, 7) STUB(kind, 8) STUB(kind, 9) STUB(kind, 10) STUB(kind, 11) STUB(kind, 12) STUB(kind, 13) \
	STUB(kind, 14) STUB(kind, 15) STUB(kind, 16) STUB(kind, 17) STUB(kind, 18) STUB(kind, 19) STUB(kind, 20) \
	STUB(kind, 21) STUB(kind, 22) STUB(kind, 23) STUB(kind, 24) STUB(kind, 25) STUB(kind, 26)
STUBS(object)
STUBS(enum)
#define S(kind) kind##Stub0, kind##Stub1, kind##Stub2, kind##Stub3, kind##Stub4, kind##Stub5, kind##Stub6, \
	kind##Stub7, kind##Stub8, kind##Stub9, kind##Stub10, kind##Stub11, kind##Stub12, kind##Stub13, \
	kind##Stub14, kind##Stub15, kind##Stub16, kind##Stub17, kind##Stub18, kind##Stub19, kind##Stub20, \
	kind##Stub21, kind##Stub22, kind##Stub23, kind##Stub24, kind##Stub25, kind##Stub26
static HRESULT(STDMETHODCALLTYPE *objectStubs[27])(void *) = {S(object)};
static HRESULT(STDMETHODCALLTYPE *enumStubs[27])(void *) = {S(enum)};

static void *objVtbl[27], *enumVtbl[8];

static HRESULT STDMETHODCALLTYPE fakeQI(void *self, REFIID iid, void **out)
{
	(void)iid;
	// Callers only ask for the interface they already hold (or IUnknown).
	*out = self;
	((FakeObject *)self)->refs++; // refs is at the same offset in both types
	return S_OK;
}

static ULONG STDMETHODCALLTYPE objAddRef(FakeObject *o) { return InterlockedIncrement(&o->refs); }

static ULONG STDMETHODCALLTYPE objRelease(FakeObject *o)
{
	LONG n = InterlockedDecrement(&o->refs);
	if (!n) {
		for (int i = 0; i < nProps; i++)
			SysFreeString(o->values[i]);
		free(o);
	}
	return n;
}

static int propIndex(LPCWSTR name)
{
	for (int i = 0; name && i < nProps; i++)
		if (!_wcsicmp(name, propNames[i]))
			return i;
	return -1;
}

static HRESULT STDMETHODCALLTYPE objGet(FakeObject *o, LPCWSTR name, long flags, VARIANT *val, CIMTYPE *type, long *flavor)
{
	(void)flags;
	int i = propIndex(name);
	if (i < 0)
		return WBEM_E_NOT_FOUND;
	if (val) {
		VariantInit(val);
		V_VT(val) = VT_BSTR;
		V_BSTR(val) = SysAllocString(o->values[i]);
	}
	if (type)
		*type = CIM_REFERENCE;
	if (flavor)
		*flavor = WBEM_FLAVOR_ORIGIN_PROPAGATED;
	return WBEM_S_NO_ERROR;
}

static HRESULT STDMETHODCALLTYPE objGetNames(FakeObject *o, LPCWSTR qual, long flags, VARIANT *qualVal, SAFEARRAY **names)
{
	(void)o, (void)qual, (void)flags, (void)qualVal;
	SAFEARRAY *sa = SafeArrayCreateVector(VT_BSTR, 0, nProps);
	if (!sa)
		return WBEM_E_OUT_OF_MEMORY;
	for (LONG i = 0; i < nProps; i++) {
		BSTR b = SysAllocString(propNames[i]);
		SafeArrayPutElement(sa, &i, b);
		SysFreeString(b);
	}
	*names = sa;
	return WBEM_S_NO_ERROR;
}

static HRESULT STDMETHODCALLTYPE objBeginEnum(FakeObject *o, long flags)
{
	(void)flags;
	o->iter = 0;
	return WBEM_S_NO_ERROR;
}

static HRESULT STDMETHODCALLTYPE objNext(FakeObject *o, long flags, BSTR *name, VARIANT *val, CIMTYPE *type, long *flavor)
{
	if (o->iter >= nProps)
		return WBEM_S_NO_MORE_DATA;
	int i = o->iter++;
	if (name)
		*name = SysAllocString(propNames[i]);
	return objGet(o, propNames[i], flags, val, type, flavor);
}

static HRESULT STDMETHODCALLTYPE objEndEnum(FakeObject *o)
{
	o->iter = nProps;
	return WBEM_S_NO_ERROR;
}

static ULONG STDMETHODCALLTYPE enumAddRef(FakeEnum *e) { return InterlockedIncrement(&e->refs); }

static ULONG STDMETHODCALLTYPE enumRelease(FakeEnum *e)
{
	LONG n = InterlockedDecrement(&e->refs);
	if (!n) {
		objRelease(e->row);
		free(e);
	}
	return n;
}

static HRESULT STDMETHODCALLTYPE enumReset(FakeEnum *e)
{
	e->pos = 0;
	return WBEM_S_NO_ERROR;
}

static HRESULT STDMETHODCALLTYPE enumNext(FakeEnum *e, long timeout, ULONG count, FakeObject **objs, ULONG *returned)
{
	(void)timeout;
	ULONG n = 0;
	if (count && e->pos == 0) {
		objAddRef(e->row);
		objs[n++] = e->row;
		e->pos = 1;
	}
	if (returned)
		*returned = n;
	return n == count ? WBEM_S_NO_ERROR : WBEM_S_FALSE;
}

static HRESULT STDMETHODCALLTYPE enumSkip(FakeEnum *e, long timeout, ULONG count)
{
	(void)timeout;
	if (count && e->pos == 0) {
		e->pos = 1;
		return count == 1 ? WBEM_S_NO_ERROR : WBEM_S_FALSE;
	}
	return count ? WBEM_S_FALSE : WBEM_S_NO_ERROR;
}

static IEnumWbemClassObject *rootDirectoryRows(void)
{
	if (!objVtbl[0]) {
		for (int i = 0; i < 27; i++)
			objVtbl[i] = (void *)objectStubs[i];
		for (int i = 0; i < 8; i++)
			enumVtbl[i] = (void *)enumStubs[i];
		objVtbl[0] = enumVtbl[0] = (void *)fakeQI;
		objVtbl[1] = (void *)objAddRef;
		objVtbl[2] = (void *)objRelease;
		objVtbl[4] = (void *)objGet;
		objVtbl[7] = (void *)objGetNames;
		objVtbl[8] = (void *)objBeginEnum;
		objVtbl[9] = (void *)objNext;
		objVtbl[10] = (void *)objEndEnum;
		enumVtbl[1] = (void *)enumAddRef;
		enumVtbl[2] = (void *)enumRelease;
		enumVtbl[3] = (void *)enumReset;
		enumVtbl[4] = (void *)enumNext;
		enumVtbl[7] = (void *)enumSkip;
	}
	wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = L".";
	DWORD hn = MAX_COMPUTERNAME_LENGTH + 1;
	GetComputerNameW(host, &hn);
	wchar_t group[128], part[128];
	_snwprintf(group, 128, L"\\\\%ls\\root\\cimv2:Win32_LogicalDisk.DeviceID=\"C:\"", host);
	_snwprintf(part, 128, L"\\\\%ls\\root\\cimv2:Win32_Directory.Name=\"c:\\\\\"", host);

	FakeObject *o = calloc(1, sizeof *o);
	FakeEnum *e = calloc(1, sizeof *e);
	if (!o || !e) {
		free(o);
		free(e);
		return NULL;
	}
	o->vtbl = objVtbl;
	o->refs = 1;
	o->values[0] = SysAllocString(group);
	o->values[1] = SysAllocString(part);
	e->vtbl = enumVtbl;
	e->refs = 1;
	e->row = o;
	return (IEnumWbemClassObject *)e;
}

static int queryNeedsFix(const wchar_t *q)
{
	for (size_t i = 0; i < sizeof missingClasses / sizeof *missingClasses; i++) {
		size_t n = wcslen(missingClasses[i]);
		for (const wchar_t *p = q; *p; p++)
			if (!_wcsnicmp(p, missingClasses[i], n))
				return 1;
	}
	return 0;
}

static HRESULT STDMETHODCALLTYPE hookExecQuery(IWbemServices *self, const BSTR lang, const BSTR query, long flags,
                                               IWbemContext *ctx, IEnumWbemClassObject **out)
{
	if (query && queryNeedsFix(query)) {
		if (!out || !(*out = rootDirectoryRows()))
			return WBEM_E_OUT_OF_MEMORY;
		return WBEM_S_NO_ERROR;
	}
	HRESULT hr = realExecQuery(self, lang, query, flags, ctx, out);
	char q[256] = "";
	if (query)
		WideCharToMultiByte(CP_UTF8, 0, query, -1, q, sizeof q, NULL, NULL);
	if (FAILED(hr))
		logmsg("WMI: ExecQuery '%s' -> 0x%08lx", q, (unsigned long)hr);
	return hr;
}

static HRESULT STDMETHODCALLTYPE hookConnectServer(IWbemLocator *self, const BSTR ns, const BSTR user, const BSTR pw,
                                                   const BSTR locale, long flags, const BSTR auth, IWbemContext *ctx,
                                                   IWbemServices **out)
{
	HRESULT hr = realConnectServer(self, ns, user, pw, locale, flags, auth, ctx, out);
	char n[128] = "";
	if (ns)
		WideCharToMultiByte(CP_UTF8, 0, ns, -1, n, sizeof n, NULL, NULL);
	if (FAILED(hr))
		logmsg("WMI: ConnectServer(%s) -> 0x%08lx", n, (unsigned long)hr);
	if (SUCCEEDED(hr) && out && *out) {
		void **vtbl = *(void ***)*out;
		// IWbemServices::ExecQuery is slot 20; all instances share the vtable.
		if (vtbl[20] != (void *)hookExecQuery) {
			realExecQuery = (ExecQuery_t)vtbl[20];
			patchSlot(&vtbl[20], (void *)hookExecQuery);
		}
		// IWbemServices::GetObject is slot 6.
		if (vtbl[6] != (void *)hookGetObject) {
			realGetObject = (GetObject_t)vtbl[6];
			patchSlot(&vtbl[6], (void *)hookGetObject);
		}
	}
	return hr;
}

static HRESULT WINAPI hookCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID *out)
{
	HRESULT hr = realCoCreateInstance(clsid, outer, ctx, iid, out);
	if (SUCCEEDED(hr) && out && *out && IsEqualCLSID(clsid, &clsidWbemLocator)) {
		IWbemLocator *loc = NULL;
		if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out, &iidWbemLocator, (void **)&loc))) {
			void **vtbl = *(void ***)loc;
			// IWbemLocator::ConnectServer is slot 3.
			if (vtbl[3] != (void *)hookConnectServer) {
				realConnectServer = (ConnectServer_t)vtbl[3];
				patchSlot(&vtbl[3], (void *)hookConnectServer);
			}
			IWbemLocator_Release(loc);
		}
	}
	return hr;
}

// hookImport replaces module's import of dll!name with fn and returns the
// original, or NULL if the module doesn't import it.
void *hookImport(HMODULE module, const char *dll, const char *name, void *fn)
{
	BYTE *base = (BYTE *)module;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dirent = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dirent.VirtualAddress)
		return NULL;
	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dirent.VirtualAddress); d->Name; d++) {
		if (_stricmp((char *)(base + d->Name), dll))
			continue;
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
		IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
		for (; names->u1.AddressOfData; names++, addrs++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
			if (strcmp((char *)ibn->Name, name))
				continue;
			void *orig = (void *)addrs->u1.Function;
			patchSlot((void **)&addrs->u1.Function, fn);
			return orig;
		}
	}
	return NULL;
}

// ExitLag's WMI self-check still fails under Wine (it isn't the query
// above alone), and then it asks, with a MessageBoxA, whether to "repair WMI"
// (stop winmgmt, reset the repository). The answer must be no; answer it
// without showing the box. Any other message box is shown as usual.
typedef int(WINAPI *MessageBoxA_t)(HWND, LPCSTR, LPCSTR, UINT);
static MessageBoxA_t realMessageBoxA;

static int WINAPI hookMessageBoxA(HWND wnd, LPCSTR text, LPCSTR caption, UINT type)
{
	if (text && strstr(text, "repair WMI")) {
		logmsg("WMI: answered No to \"%s\"", text);
		return IDNO;
	}
	return realMessageBoxA(wnd, text, caption, type);
}

void wmifixInstall(void)
{
	void *mb = hookImport(GetModuleHandleA(NULL), "user32.dll", "MessageBoxA", (void *)hookMessageBoxA);
	if (mb)
		realMessageBoxA = (MessageBoxA_t)mb;

	void *orig = hookImport(GetModuleHandleA(NULL), "ole32.dll", "CoCreateInstance", (void *)hookCoCreateInstance);
	if (orig) {
		realCoCreateInstance = (CoCreateInstance_t)orig;
		logmsg("WMI fix installed");
	}
}
