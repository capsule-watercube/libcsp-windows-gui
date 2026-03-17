#include <csp/drivers/usart.h>

#include <csp/csp_debug.h>
#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <csp/csp.h>

/* csp_usart_fd_t is HANDLE on Windows (see usart.h) */

typedef struct {
	csp_usart_callback_t rx_callback;
	void * user_data;
	csp_usart_fd_t fd;
	HANDLE rx_thread;
	LONG isListening;
	LONG tx_locked;   /* 1 while TX is in progress — RX thread waits */
} usart_context_t;

static HANDLE mutexHandle = NULL;
static usart_context_t * g_ctx = NULL;  /* for lock/unlock access to tx_locked */

void csp_usart_lock(void * driver_data) {
	WaitForSingleObject(mutexHandle, INFINITE);
	if (g_ctx) {
		InterlockedExchange(&g_ctx->tx_locked, 1);
		/* MAXDWORD interval timeout makes ReadFile return in <1 ms;
		 * 5 ms is enough for the RX thread to notice and exit */
		Sleep(5);
	}
}

void csp_usart_unlock(void * driver_data) {
	if (g_ctx) {
		InterlockedExchange(&g_ctx->tx_locked, 0);
	}
	ReleaseMutex(mutexHandle);
}

static csp_usart_fd_t openPort(const char * device) {
	char win32_path[64];
	if (strncmp(device, "\\\\.\\", 4) == 0) {
		strncpy(win32_path, device, sizeof(win32_path));
	} else {
		snprintf(win32_path, sizeof(win32_path), "\\\\.\\%s", device);
	}
	csp_usart_fd_t fd = CreateFileA(win32_path, GENERIC_READ | GENERIC_WRITE,
	                                0, NULL, OPEN_EXISTING, 0, NULL);
	if (fd == INVALID_HANDLE_VALUE) {
		csp_print("Failed to open port: [%s], error: %lu\n", win32_path, GetLastError());
	}
	return fd;
}

static int configurePort(csp_usart_fd_t fd, const csp_usart_conf_t * conf) {
	DCB dcb = {0};
	dcb.DCBlength = sizeof(dcb);
	if (!GetCommState(fd, &dcb)) {
		return CSP_ERR_INVAL;
	}
	dcb.BaudRate          = (conf->baudrate > 0) ? conf->baudrate : 115200;
	dcb.ByteSize          = (conf->databits > 0) ? (BYTE)conf->databits : 8;
	dcb.StopBits          = (conf->stopbits == 2) ? TWOSTOPBITS : ONESTOPBIT;
	dcb.Parity            = (conf->paritysetting == 1) ? ODDPARITY :
	                        (conf->paritysetting == 2) ? EVENPARITY : NOPARITY;
	dcb.fParity           = (dcb.Parity != NOPARITY);
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
	dcb.XonChar           = 17;
	dcb.XoffChar          = 19;
	dcb.XonLim            = 2048;
	dcb.XoffLim           = 512;

	COMMTIMEOUTS timeouts = {0};
	/* MAXDWORD interval timeout: ReadFile returns immediately with whatever
	 * is in the RX buffer (even 0 bytes), letting the RX thread check
	 * tx_locked without ever blocking. */
	timeouts.ReadIntervalTimeout         = MAXDWORD;
	timeouts.ReadTotalTimeoutMultiplier  = 0;
	timeouts.ReadTotalTimeoutConstant    = 0;
	timeouts.WriteTotalTimeoutMultiplier = 1;
	timeouts.WriteTotalTimeoutConstant   = 100;
	SetCommTimeouts(fd, &timeouts);

	if (!SetCommState(fd, &dcb)) {
		csp_print("[CSP] SetCommState failed, error: %lu\n", GetLastError());
		return CSP_ERR_INVAL;
	}
	PurgeComm(fd, PURGE_RXCLEAR | PURGE_TXCLEAR);
	return CSP_ERR_NONE;
}

static unsigned WINAPI usart_rx_thread(void * params) {
	usart_context_t * ctx = params;
	uint8_t cbuf[400];
	DWORD bytesRead;

	while (ctx->isListening) {
		/* Wait while TX is in progress */
		if (InterlockedCompareExchange(&ctx->tx_locked, 0, 0)) {
			Sleep(1);
			continue;
		}

		bytesRead = 0;
		ReadFile(ctx->fd, cbuf, sizeof(cbuf), &bytesRead, NULL);

		if (bytesRead > 0) {
			ctx->rx_callback(ctx->user_data, cbuf, bytesRead, NULL);
		} else {
			Sleep(1);  /* no data — yield CPU */
		}
	}
	return 0;
}

int csp_usart_write(csp_usart_fd_t fd, const void * data, size_t data_length) {
	DWORD bytesActual = 0;
	if (!WriteFile(fd, data, (DWORD)data_length, &bytesActual, NULL)) {
		csp_print("[CSP] WriteFile failed: error=%lu\n", GetLastError());
		return CSP_ERR_TX;
	}
	return (int)bytesActual;
}

int csp_usart_open(const csp_usart_conf_t * conf, csp_usart_callback_t rx_callback,
                   void * user_data, csp_usart_fd_t * return_fd) {
	if (mutexHandle == NULL) {
		mutexHandle = CreateMutex(NULL, FALSE, FALSE);
	}

	csp_usart_fd_t fd = openPort(conf->device);
	if (fd == INVALID_HANDLE_VALUE) {
		return CSP_ERR_INVAL;
	}

	if (configurePort(fd, conf) != CSP_ERR_NONE) {
		CloseHandle(fd);
		return CSP_ERR_INVAL;
	}

	usart_context_t * ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		csp_print("%s: Error allocating context, device: [%s]\n", __func__, conf->device);
		CloseHandle(fd);
		return CSP_ERR_NOMEM;
	}

	ctx->rx_callback = rx_callback;
	ctx->user_data   = user_data;
	ctx->fd          = fd;
	ctx->isListening = 1;
	ctx->tx_locked   = 0;
	g_ctx            = ctx;

	uintptr_t ret = _beginthreadex(NULL, 0, usart_rx_thread, ctx, 0, NULL);
	if (ret == 0) {
		CloseHandle(fd);
		free(ctx);
		return CSP_ERR_DRIVER;
	}
	ctx->rx_thread = (HANDLE)ret;

	if (return_fd) {
		*return_fd = fd;
	}

	return CSP_ERR_NONE;
}

void csp_usart_close(void) {
    if (g_ctx == NULL) {
        return;
    }

    /* Signal RX thread to stop */
    InterlockedExchange(&g_ctx->isListening, 0);

    /* Wait for RX thread to finish */
    if (g_ctx->rx_thread != NULL) {
        WaitForSingleObject(g_ctx->rx_thread, 2000);
        CloseHandle(g_ctx->rx_thread);
        g_ctx->rx_thread = NULL;
    }

    /* Close the COM port */
    if (g_ctx->fd != INVALID_HANDLE_VALUE) {
        PurgeComm(g_ctx->fd, PURGE_RXCLEAR | PURGE_TXCLEAR);
        CloseHandle(g_ctx->fd);
        g_ctx->fd = INVALID_HANDLE_VALUE;
    }

    free(g_ctx);
    g_ctx = NULL;
}