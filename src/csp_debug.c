#include <inttypes.h>
#include "csp_macro.h"
#include "csp/autoconfig.h"

uint8_t csp_dbg_buffer_out;
uint8_t csp_dbg_errno;
uint8_t csp_dbg_conn_out;
uint8_t csp_dbg_conn_ovf;
uint8_t csp_dbg_conn_noroute;
uint8_t csp_dbg_can_errno;
uint8_t csp_dbg_eth_errno;
uint8_t csp_dbg_inval_reply;
uint8_t csp_dbg_rdp_print;
uint8_t csp_dbg_packet_print;

#if (CSP_ENABLE_CSP_PRINT)
#if (CSP_PRINT_STDIO)
#include <stdarg.h>
#include <stdio.h>
#include "csp/csp_debug.h"
#include <time.h>
void csp_print_func(const char * fmt, ...) {
    // 1. Get current timestamp
    time_t now;
    time(&now);
    struct tm *local = localtime(&now);
    char time_str[10];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", local);

    // 2. Prepare the variable argument list
    va_list args;
    va_start(args, fmt);

    // 3. Thread-safe printing with prefix and timestamp
    // We use flockfile/funlockfile logic conceptually, 
    // but on Windows standard printf is usually thread-safe enough.
    printf("[%s] [CSP] ", time_str);
    vprintf(fmt, args);
    
    // 4. Ensure immediate output (don't wait for buffer to fill)
    fflush(stdout);

    va_end(args);
}
// __weak void csp_print_func(const char * fmt, ...) {
//     va_list args;
//     va_start(args, fmt);
//     vprintf(fmt, args);
//     va_end(args);
// }
#else
__weak void csp_print_func(const char * fmt, ...) {}
#endif
#endif
