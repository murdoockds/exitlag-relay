// Stand-in for ExitLag's nfextlag.sys WFP driver.
//
// ExitLag only uses the WFP path while the "nfextlag" driver service runs.
// exitlag-wine points that service at this file, which Wine can load (it does
// nothing; the filtering is done by our nfapi.dll, which never talks to a
// driver). The real nfextlag.sys stays next to it: ExitLag checks its version.
typedef long NTSTATUS;

NTSTATUS DriverEntry(void *driver, void *registryPath)
{
	(void)driver, (void)registryPath;
	return 0; // STATUS_SUCCESS; no unload routine, so it stays loaded
}
