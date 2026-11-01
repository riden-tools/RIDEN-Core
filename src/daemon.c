#include "ipc.h"
#include "riden_core.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static atomic_int stopped;
static void stop(int sig) {
    (void)sig;
    atomic_store(&stopped, 1);
}
static unsigned number(const char *text, unsigned maximum) {
    char *end;
    errno = 0;
    unsigned long long n = strtoull(text, &end, 10);
    if (text[0] < '0' || text[0] > '9' || errno || *end || n > maximum) {
        fprintf(stderr, "Invalid numeric option: %s\n", text);
        exit(2);
    }
    return (unsigned)n;
}
/* ID,VSCALE,ISCALE,VMAX,IMAX,OVPMAX,OCPMAX, e.g. 60125,100,1000,61,6.1,62,6.2 */
static void model_option(const char *text) {
    char *end;
    double v[7];
    const char *p = text;
    for (int k = 0; k < 7; k++) {
        if ((*p < '0' || *p > '9') && *p != '.')
            goto bad;
        errno = 0;
        v[k] = strtod(p, &end);
        if (errno || end == p || *end != (k == 6 ? '\0' : ','))
            goto bad;
        p = end + 1;
    }
    if (v[0] < 1 || v[0] > 65535 || v[0] != (unsigned)v[0] || v[1] != (unsigned)v[1] ||
        v[2] != (unsigned)v[2] || v[1] > 65535 || v[2] > 65535)
        goto bad;
    rc_model_info m = {(uint16_t)v[0], NULL, (uint16_t)v[1], (uint16_t)v[2], v[3], v[4], v[5], v[6]};
    int e = rc_register_model(&m);
    if (e) {
        fprintf(stderr, "Cannot register model %s: %s\n", text,
                e == RC_BUSY ? "built-in model" : rc_error_string(e));
        exit(2);
    }
    return;
bad:
    fprintf(stderr, "Invalid --model-profile: %s\n", text);
    exit(2);
}
static int daemon_main(int argc, char **argv) {
    const char *path = NULL;
    unsigned port = 19606, baud = 115200;
    rc_config cfg = rc_default_config();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--service"))
            continue;
        else if (!strcmp(argv[i], "--allow-writes"))
            cfg.allow_writes = 1;
        else if (!strcmp(argv[i], "--allow-multiwrite"))
            cfg.allow_multiwrite = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "--device"))
            path = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--model-profile"))
            model_option(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--port"))
            port = number(argv[++i], UINT_MAX);
        else if (i + 1 < argc && !strcmp(argv[i], "--baud"))
            baud = number(argv[++i], UINT_MAX);
        else if (i + 1 < argc && !strcmp(argv[i], "--period-ms"))
            cfg.period_ms = number(argv[++i], UINT_MAX);
        else if (i + 1 < argc && !strcmp(argv[i], "--address")) {
            unsigned a = number(argv[++i], UINT_MAX);
            if (a < 1 || a > 247)
                return 2;
            cfg.address = (uint8_t)a;
        } else if (i + 1 < argc && !strcmp(argv[i], "--mode")) {
            const char *m = argv[++i];
            if (!strcmp(m, "hot")) {
                cfg.poll_start = 10;
                cfg.poll_count = 4;
            } else if (!strcmp(m, "status")) {
                cfg.poll_start = 4;
                cfg.poll_count = 16;
            } else if (!strcmp(m, "full")) {
                cfg.poll_start = 0;
                cfg.poll_count = 121;
            } else
                return 2;
        } else {
            fprintf(stderr, "Usage: riden-cored --device PATH [--port 19606] [--mode "
                            "hot|status|full] [--period-ms 0] [--baud 115200] [--address 1] "
                            "[--allow-writes] [--allow-multiwrite] "
                            "[--model-profile ID,VSCALE,ISCALE,VMAX,IMAX,OVP,OCP]\n");
            return 2;
        }
    }
    if (!path || port < 1 || port > 65535)
        return 2;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    rc_transport t;
    int e = rc_serial_open(path, baud, &t);
    if (e) {
        fprintf(stderr, "Cannot open %s: %s\n", path, rc_error_string(e));
        return 1;
    }
    rc_core *core = rc_create(&t, &cfg);
    if (!core) {
        rc_serial_close(&t);
        return 1;
    }
    fprintf(stderr, "RIDEN-Core 0.1.0: 127.0.0.1:%u; %s; mode=%u/%u; writes=%s\n", port, path,
            cfg.poll_start, cfg.poll_count, cfg.allow_writes ? "enabled" : "disabled");
    int status = rc_ipc_run(core, port, &stopped, cfg.allow_writes, cfg.allow_multiwrite);
    rc_destroy(core);
    rc_serial_close(&t);
    return status ? 1 : 0;
}

#ifdef _WIN32
static SERVICE_STATUS_HANDLE service_handle;
static void report_service(DWORD state, DWORD result) {
    SERVICE_STATUS s = {0};
    s.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    s.dwCurrentState = state;
    s.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    s.dwWin32ExitCode = result ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
    s.dwServiceSpecificExitCode = result;
    s.dwWaitHint = state == SERVICE_STOP_PENDING ? 5000 : 0;
    SetServiceStatus(service_handle, &s);
}
static int saved_argc;
static char **saved_argv;
static void WINAPI service_control(DWORD code) {
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        report_service(SERVICE_STOP_PENDING, 0);
        atomic_store(&stopped, 1);
    }
}
static void WINAPI service_main(DWORD argc, LPSTR *argv) {
    (void)argc;
    (void)argv;
    service_handle = RegisterServiceCtrlHandlerA("RIDEN-Core", service_control);
    if (!service_handle)
        return;
    report_service(SERVICE_RUNNING, 0);
    int result = daemon_main(saved_argc, saved_argv);
    report_service(SERVICE_STOPPED, (DWORD)result);
}
#endif
int main(int argc, char **argv) {
#ifdef _WIN32
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--service")) {
            saved_argc = argc;
            saved_argv = argv;
            SERVICE_TABLE_ENTRYA table[] = {{"RIDEN-Core", service_main}, {NULL, NULL}};
            return StartServiceCtrlDispatcherA(table) ? 0 : 1;
        }
#endif
    return daemon_main(argc, argv);
}
