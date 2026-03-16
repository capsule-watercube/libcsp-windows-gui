

#include <csp/csp_hooks.h>

#include <csp/csp_debug.h>
#include <string.h>
#include <windows.h>
#include <stdio.h>

/* Helper: Enable the privilege to shut down/reboot the system */
static int EnableShutdownPrivilege(void) {
    HANDLE hToken;
    TOKEN_PRIVILEGES tkp;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return 0;

    LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);

    tkp.PrivilegeCount = 1; 
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, (PTOKEN_PRIVILEGES)NULL, 0);

    return (GetLastError() == ERROR_SUCCESS);
}

/* --- CSP System Functions --- */

int csp_sys_tasklist(char * out) {
    strcpy(out, "Tasklist not available on Windows");
    return CSP_ERR_NONE;
}

int csp_sys_tasklist_size(void) {
    return 100;
}

uint32_t csp_sys_memfree(void) {
    // We delegate this to the hook to keep logic centralized
    return csp_memfree_hook();
}

/* --- CSP Hooks Implementation --- */

uint32_t csp_memfree_hook(void) {
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    
    if (GlobalMemoryStatusEx(&statex)) {
        // ullAvailPhys is 64-bit. We saturate at UINT32_MAX if free RAM > 4GB.
        if (statex.ullAvailPhys > 0xFFFFFFFF) {
            return 0xFFFFFFFF;
        }
        return (uint32_t)statex.ullAvailPhys;
    }
    return 0;
}

unsigned int csp_ps_hook(csp_packet_t * packet) {
    (void)packet; 
    return 0;
}

void csp_reboot_hook(void) {
    csp_print("CSP Reboot Hook: System rebooting...\n");
    if (EnableShutdownPrivilege()) {
        if (!ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OTHER)) {
            csp_print("Failed to reboot. Error: %lu\n", GetLastError());
        }
    } else {
        csp_print("Insufficient privileges to reboot.\n");
    }
    // Fallback: Terminate the application
    ExitProcess(0);
}

void csp_shutdown_hook(void) {
    csp_print("CSP Shutdown Hook: System shutting down...\n");
    if (EnableShutdownPrivilege()) {
        if (!ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OTHER)) {
            csp_print("Failed to shutdown. Error: %lu\n", GetLastError());
        }
    } else {
        csp_print("Insufficient privileges to shutdown.\n");
    }
    // Fallback: Terminate the application
    ExitProcess(0);
}