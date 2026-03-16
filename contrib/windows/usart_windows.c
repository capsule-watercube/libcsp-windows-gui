#include <csp/drivers/usart.h>

#include <csp/csp_debug.h>
#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <stdio.h>
#include <csp/csp.h>

/* We store the real HANDLE alongside the int fd so that csp_usart_write
 * can use the correct 64-bit HANDLE value even on 64-bit Windows where
 * HANDLE does not fit in csp_usart_fd_t (int). */
typedef struct {
	csp_usart_callback_t rx_callback;
	void * user_data;
	csp_usart_fd_t fd;   /* int copy — used only as a key / for legacy compat */
	HANDLE real_fd;      /* actual 64-bit Windows HANDLE */
	HANDLE rx_thread;
	HANDLE rx_event;     /* overlapped event for RX */
	LONG isListening;
} usart_context_t;

/* One context per open port — for single-port use this is fine */
static usart_context_t * g_ctx = NULL;

static HANDLE mutexHandle = NULL;

void csp_usart_lock(void * driver_data) {
	WaitForSingleObject(mutexHandle, 100000);
}

void csp_usart_unlock(void * driver_data) {
	ReleaseMutex(mutexHandle);
}

static int openPort(const char * device, HANDLE * return_handle) {
    char win32_path[64];

    if (strncmp(device, "\\\\.\\", 4) == 0) {
        strncpy(win32_path, device, sizeof(win32_path));
    } else {
        snprintf(win32_path, sizeof(win32_path), "\\\\.\\%s", device);
    }

    /* FILE_FLAG_OVERLAPPED: enables truly concurrent RX and TX from separate
     * threads. Without it, synchronous ReadFile in the RX thread serialises
     * with WriteFile in the TX thread, causing WriteFile to block. */
    *return_handle = CreateFileA(win32_path,
                                 GENERIC_READ | GENERIC_WRITE,
                                 0,
                                 NULL,
                                 OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED,
                                 NULL);

    if (*return_handle == INVALID_HANDLE_VALUE) {
        csp_print("Failed to open port: [%s], error: %lu\n", win32_path, GetLastError());
        return CSP_ERR_INVAL;
    }

    return CSP_ERR_NONE;
}

static int configurePort(HANDLE fd, const csp_usart_conf_t * conf) {
    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(fd, &dcb)) {
        return CSP_ERR_INVAL;
    }

    dcb.BaudRate = (conf->baudrate > 0) ? conf->baudrate : 115200;
    dcb.ByteSize = (conf->databits > 0) ? (BYTE)conf->databits : 8;
    dcb.StopBits = (conf->stopbits == 2) ? TWOSTOPBITS : ONESTOPBIT;
    dcb.Parity   = (conf->paritysetting == 1) ? ODDPARITY :
                   (conf->paritysetting == 2) ? EVENPARITY : NOPARITY;
    dcb.fParity  = (dcb.Parity != NOPARITY);

    dcb.fBinary           = TRUE;
    dcb.fOutxCtsFlow      = FALSE;
    dcb.fOutxDsrFlow      = FALSE;
    dcb.fDtrControl       = DTR_CONTROL_DISABLE;
    dcb.fDsrSensitivity   = FALSE;
    dcb.fTXContinueOnXoff = FALSE;
    dcb.fOutX             = FALSE;
    dcb.fInX              = FALSE;
    dcb.fErrorChar        = FALSE;
    dcb.fNull             = FALSE;
    dcb.fRtsControl       = RTS_CONTROL_DISABLE;
    dcb.fAbortOnError     = FALSE;
    dcb.XonChar  = 17;
    dcb.XoffChar = 19;
    dcb.XonLim   = 2048;
    dcb.XoffLim  = 512;

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout         = 0;
    timeouts.ReadTotalTimeoutMultiplier  = 0;
    timeouts.ReadTotalTimeoutConstant    = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant   = 2000;
    SetCommTimeouts(fd, &timeouts);

    if (!SetCommState(fd, &dcb)) {
        csp_print("[CSP] SetCommState failed, error: %lu. (B:%lu S:%d P:%d D:%d)\n",
                  GetLastError(), dcb.BaudRate, dcb.StopBits, dcb.Parity, dcb.ByteSize);
        return CSP_ERR_INVAL;
    }
    
    if (!PurgeComm(fd, PURGE_RXCLEAR | PURGE_TXCLEAR)) {
        csp_print("[CSP] PurgeComm failed, error: %lu. (B:%lu S:%d P:%d D:%d)\n",
                  GetLastError(), dcb.BaudRate, dcb.StopBits, dcb.Parity, dcb.ByteSize);
        return CSP_ERR_INVAL;
    }

    return CSP_ERR_NONE;
}

static unsigned WINAPI usart_rx_thread(void * params) {
    usart_context_t * ctx = params;
    uint8_t cbuf[400];
    DWORD bytesRead;
    OVERLAPPED ov = {0};
    ov.hEvent = ctx->rx_event;

    while (ctx->isListening) {
        ResetEvent(ov.hEvent);
        bytesRead = 0;

        if (!ReadFile(ctx->real_fd, cbuf, sizeof(cbuf), &bytesRead, &ov)) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                /* Wait up to 10 ms for data, then loop to check isListening */
                DWORD waitResult = WaitForSingleObject(ov.hEvent, 10);
                if (waitResult == WAIT_OBJECT_0) {
                    if (!GetOverlappedResult(ctx->real_fd, &ov, &bytesRead, FALSE)) {
                        DWORD ioErr = GetLastError();
                        if (ioErr != ERROR_OPERATION_ABORTED && ioErr != 0) {
                            csp_print("[CSP] RX GetOverlappedResult error: %lu\n", ioErr);
                        }
                        continue;
                    }
                } else {
                    /* Timeout or error — cancel and retry */
                    CancelIo(ctx->real_fd);
                    continue;
                }
            } else if (err != ERROR_OPERATION_ABORTED && err != 0) {
                csp_print("[CSP] ReadFile error: %lu\n", err);
                Sleep(5);
                continue;
            } else {
                continue;
            }
        }

        if (bytesRead > 0) {
            ctx->rx_callback(ctx->user_data, cbuf, bytesRead, NULL);
        }
    }

    return 0;
}

int csp_usart_write(csp_usart_fd_t fd, const void * data, size_t data_length) {
    /* Resolve the real HANDLE from the global context.
     * csp_usart_fd_t is int, which cannot safely hold a 64-bit HANDLE. */
    HANDLE real_fd = (g_ctx != NULL) ? g_ctx->real_fd : (HANDLE)(uintptr_t)(unsigned int)fd;

    OVERLAPPED ov = {0};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == NULL) {
        csp_print("[CSP] WriteFile: failed to create event, error: %lu\n", GetLastError());
        return CSP_ERR_TX;
    }

    DWORD bytesActual = 0;
    BOOL ok = WriteFile(real_fd, data, (DWORD)data_length, &bytesActual, &ov);

    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            /* Wait for the write to complete (up to 2 seconds) */
            if (WaitForSingleObject(ov.hEvent, 2000) == WAIT_OBJECT_0) {
                GetOverlappedResult(real_fd, &ov, &bytesActual, FALSE);
            } else {
                CancelIo(real_fd);
                csp_print("[CSP] WriteFile timed out\n");
                CloseHandle(ov.hEvent);
                return CSP_ERR_TX;
            }
        } else {
            csp_print("[CSP] WriteFile failed: error=%lu\n", err);
            CloseHandle(ov.hEvent);
            return CSP_ERR_TX;
        }
    }

    CloseHandle(ov.hEvent);
    return (int)bytesActual;
}

int csp_usart_open(const csp_usart_conf_t * conf, csp_usart_callback_t rx_callback, void * user_data, csp_usart_fd_t * return_fd) {

	if (mutexHandle == NULL) {
		mutexHandle = CreateMutex(NULL, FALSE, FALSE);
	}

	HANDLE real_fd;
	int res = openPort(conf->device, &real_fd);
	if (res != CSP_ERR_NONE) {
		return res;
	}

	res = configurePort(real_fd, conf);
	if (res != CSP_ERR_NONE) {
		CloseHandle(real_fd);
		return res;
	}

	usart_context_t * ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		csp_print("%s: Error allocating context, device: [%s]\n", __func__, conf->device);
		CloseHandle(real_fd);
		return CSP_ERR_NOMEM;
	}

	ctx->rx_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (ctx->rx_event == NULL) {
		csp_print("[CSP] Failed to create RX event, error: %lu\n", GetLastError());
		CloseHandle(real_fd);
		free(ctx);
		return CSP_ERR_DRIVER;
	}

	ctx->rx_callback = rx_callback;
	ctx->user_data   = user_data;
	ctx->real_fd     = real_fd;
	ctx->fd          = (csp_usart_fd_t)(uintptr_t)real_fd; /* best-effort int copy */
	ctx->isListening = 1;
	g_ctx            = ctx;

	uintptr_t ret = _beginthreadex(NULL, 0, usart_rx_thread, ctx, 0, NULL);
	csp_print("[CSP] RX thread started: %p\n", (void *)ret);
	if (ret == 0) {
		CloseHandle(ctx->real_fd);
		CloseHandle(ctx->rx_event);
		free(ctx);
		return CSP_ERR_DRIVER;
	}
    ctx->rx_thread = (HANDLE)ret;
	if (return_fd) {
		*return_fd = ctx->fd;
	}

	return CSP_ERR_NONE;
}
