/* mojo_stress2.c — v2: cross-process + Chromium skip-modes.
 * Parent ("browser"): named-pipe server, IOCP pump, echoes 64-byte records.
 * Child  ("renderer", spawned with inherited client handle): IOCP pump +
 *   main-thread sendSync loop (overlapped write -> semaphore wait).
 * Both ends use SetFileCompletionNotificationModes(SKIP_COMPLETION_PORT_ON_
 * SUCCESS | SKIP_SET_EVENT_ON_HANDLE) like mojo ChannelWin — synchronous
 * completions are handled inline, no IOCP packet. Run with --noskip to
 * disable (bisect lever). Exit 0 = pass, 1 = lost wake, 2 = setup error.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC_SIZE    64
#define BURST_EVERY 1000
#define BURST_LEN   200
#define TOTAL_ITERS 200000
#define WAIT_MS     10000

static FILE* g_log;
static const char* g_tag = "parent";
static void logf_(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[512]; vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(stderr, "[%s] %s\n", g_tag, buf);
    if (g_log) { fprintf(g_log, "%lu [%s] %s\n", GetTickCount(), g_tag, buf); fflush(g_log); }
}

typedef struct {
    HANDLE pipe, iocp;
    volatile LONG recs_in, reads_inline, reads_queued, wakes;
    int echo;
    HANDLE reply_sem;
    char rdbuf[4096];
    OVERLAPPED rd_ov;
    char acc[REC_SIZE]; int acc_len;
} Side;

static void ov_write(HANDLE pipe, const void* data, DWORD len, const char* who) {
    OVERLAPPED ov; memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD wr = 0;
    if (WriteFile(pipe, data, len, &wr, &ov)) {           /* sync success: no packet (skip mode) */
        CloseHandle(ov.hEvent);
        if (wr != len) { logf_("%s: short sync write %lu/%lu", who, wr, len); ExitProcess(2); }
        return;
    }
    if (GetLastError() != ERROR_IO_PENDING) { logf_("%s: write failed %lu", who, GetLastError()); ExitProcess(2); }
    if (WaitForSingleObject(ov.hEvent, WAIT_MS) != WAIT_OBJECT_0) { logf_("%s: WRITE COMPLETION LOST", who); ExitProcess(1); }
    GetOverlappedResult(pipe, &ov, &wr, FALSE);
    CloseHandle(ov.hEvent);
    if (wr != len) { logf_("%s: short write %lu/%lu", who, wr, len); ExitProcess(2); }
}

static volatile LONG g_first_rec_logged;
static void process_bytes(Side* s, DWORD bytes) {
    if (!g_first_rec_logged) { g_first_rec_logged = 1; logf_("first bytes received (%lu)", bytes); }
    int off = 0;
    while (off < (int)bytes) {
        int take = (int)bytes - off;
        int room = REC_SIZE - s->acc_len;
        if (take > room) take = room;
        memcpy(s->acc + s->acc_len, s->rdbuf + off, take);
        s->acc_len += take; off += take;
        if (s->acc_len == REC_SIZE) {
            s->acc_len = 0;
            InterlockedIncrement(&s->recs_in);
            if (s->echo) ov_write(s->pipe, s->acc, REC_SIZE, "echo");
            else ReleaseSemaphore(s->reply_sem, 1, NULL);
        }
    }
}

static void arm_read(Side* s) {
    for (;;) {
        DWORD n = 0;
        memset(&s->rd_ov, 0, sizeof s->rd_ov);
        if (ReadFile(s->pipe, s->rdbuf, sizeof s->rdbuf, &n, &s->rd_ov)) {
            InterlockedIncrement(&s->reads_inline);       /* sync success: handle inline, re-arm */
            process_bytes(s, n);
            continue;
        }
        if (GetLastError() == ERROR_IO_PENDING) return;
        logf_("ReadFile arm failed %lu", GetLastError());
        ExitProcess(2);
    }
}

static DWORD WINAPI io_thread(LPVOID arg) {
    Side* s = (Side*)arg;
    arm_read(s);
    for (;;) {
        DWORD bytes = 0; ULONG_PTR key = 0; OVERLAPPED* pov = NULL;
        BOOL ok = GetQueuedCompletionStatus(s->iocp, &bytes, &key, &pov, INFINITE);
        if (key == 0xCAFE) { InterlockedIncrement(&s->wakes); continue; }
        if (!ok || !pov) { logf_("GQCS err %lu", GetLastError()); continue; }
        if (pov != &s->rd_ov) continue;                   /* pending-write packet */
        InterlockedIncrement(&s->reads_queued);
        process_bytes(s, bytes);
        arm_read(s);
    }
}

static volatile LONG g_stop_waker;
static Side g_side;
static DWORD WINAPI waker_thread(LPVOID arg) {
    (void)arg;
    while (!g_stop_waker) {
        PostQueuedCompletionStatus(g_side.iocp, 0, 0xCAFE, NULL);
        Sleep(5);   /* Chromium ScheduleWork is self-throttled; ~200/s is generous */
    }
    return 0;
}

static void setup_side(HANDLE pipe, int echo, int skip) {
    g_side.pipe = pipe; g_side.echo = echo;
    if (skip && !SetFileCompletionNotificationModes(pipe,
            FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE))
        logf_("SetFileCompletionNotificationModes FAILED %lu (continuing without)", GetLastError());
    if (!echo) g_side.reply_sem = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL);
    g_side.iocp = CreateIoCompletionPort(pipe, NULL, 1, 0);
    if (!g_side.iocp) { logf_("IOCP failed %lu", GetLastError()); ExitProcess(2); }
    CreateThread(NULL, 0, io_thread, &g_side, 0, NULL);
    CreateThread(NULL, 0, waker_thread, NULL, 0, NULL);
}

int main(int argc, char** argv) {
    int child = argc >= 3 && !strcmp(argv[1], "child");
    int skip = 1;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--noskip")) skip = 0;

    if (child) {
        g_tag = "child";
        g_log = fopen("C:\\mojo_stress2_child.log", "w");
        HANDLE pipe = (HANDLE)(ULONG_PTR)strtoull(argv[2], NULL, 10);
        logf_("child start pipe=%p skip=%d", pipe, skip);
        DWORD avail=0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL))
            logf_("PeekNamedPipe on inherited handle FAILED %lu", GetLastError());
        else logf_("inherited handle alive, pending=%lu", avail);
        setup_side(pipe, 0, skip);
        logf_("setup done, entering loop");
        char rec[REC_SIZE];
        DWORD t0 = GetTickCount();
        for (long i = 1; i <= TOTAL_ITERS; i++) {
            int burst = (i % BURST_EVERY) == 0 ? BURST_LEN : 1;
            for (int k = 0; k < burst; k++) {
                memset(rec, 0, sizeof rec);
                *(long*)rec = i; *(int*)(rec + 8) = k;
                if (i == 1) logf_("first write...");
                ov_write(pipe, rec, REC_SIZE, "send");
                if (i == 1) logf_("first write done");
            }
            for (int k = 0; k < burst; k++) {
                if (WaitForSingleObject(g_side.reply_sem, WAIT_MS) != WAIT_OBJECT_0) {
                    logf_("LOST WAKE iter %ld slot %d/%d", i, k, burst);
                    logf_("recs=%ld inline=%ld queued=%ld wakes=%ld",
                          g_side.recs_in, g_side.reads_inline, g_side.reads_queued, g_side.wakes);
                    DWORD avail = 0;
                    PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL);
                    logf_("pipe pending unread: %lu", avail);
                    if (WaitForSingleObject(g_side.reply_sem, WAIT_MS) == WAIT_OBJECT_0)
                        logf_("late wake on retry (delayed not lost)");
                    else logf_("still nothing — wake LOST");
                    return 1;
                }
            }
            if ((i % 50000) == 0) {
                DWORD dt = GetTickCount() - t0;
                logf_("iter %ld ok recs=%ld inline=%ld queued=%ld (%lus)",
                      i, g_side.recs_in, g_side.reads_inline, g_side.reads_queued, dt / 1000);
            }
        }
        g_stop_waker = 1;
        logf_("CHILD PASS recs=%ld inline=%ld queued=%ld", g_side.recs_in, g_side.reads_inline, g_side.reads_queued);
        return 0;
    }

    g_log = fopen("C:\\mojo_stress2.log", "w");
    logf_("parent start skip=%d", skip);
    HANDLE h1 = CreateNamedPipeA("\\\\.\\pipe\\mojostress2",
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, NULL);
    if (h1 == INVALID_HANDLE_VALUE) { logf_("CreateNamedPipe failed %lu", GetLastError()); return 2; }
    OVERLAPPED cov; memset(&cov, 0, sizeof cov);
    cov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ConnectNamedPipe(h1, &cov);

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE h2 = CreateFileA("\\\\.\\pipe\\mojostress2", GENERIC_READ | GENERIC_WRITE,
        0, &sa, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h2 == INVALID_HANDLE_VALUE) { logf_("client CreateFile failed %lu", GetLastError()); return 2; }
    if (WaitForSingleObject(cov.hEvent, 5000) != WAIT_OBJECT_0) { logf_("connect timeout"); return 2; }

    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof cmd, "\"%s\" child %llu%s", exe,
             (unsigned long long)(ULONG_PTR)h2, skip ? "" : " --noskip");
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        logf_("CreateProcess failed %lu", GetLastError()); return 2;
    }
    logf_("child pid %lu spawned", pi.dwProcessId);
    setup_side(h1, 1, skip);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 99; GetExitCodeProcess(pi.hProcess, &code);
    g_stop_waker = 1;
    logf_("child exited %lu | parent recs=%ld inline=%ld queued=%ld wakes=%ld",
          code, g_side.recs_in, g_side.reads_inline, g_side.reads_queued, g_side.wakes);
    logf_(code == 0 ? "PASS" : "FAIL");
    return (int)code;
}
