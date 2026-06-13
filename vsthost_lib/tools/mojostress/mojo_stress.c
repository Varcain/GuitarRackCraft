/* mojo_stress.c — standalone repro hunt for the wine+FEX "lost sync-IPC wake".
 *
 * Replicates Chromium Mojo ChannelWin mechanics in one process:
 *   "browser" side:  pipe end H1, IO thread pumping IOCP1 (overlapped reads;
 *                    每 complete 64-byte request -> overlapped reply write)
 *   "renderer" side: pipe end H2, IO thread pumping IOCP2 (overlapped reads;
 *                    each complete 64-byte reply -> ReleaseSemaphore)
 *   "renderer main": tight sendSync loop — overlapped write request, wait
 *                    semaphore 10s. Timeout = the PM spinner failure mode.
 *   "waker" thread:  hammers PostQueuedCompletionStatus into both IOCPs
 *                    (MessagePumpForIO::ScheduleWork analog).
 * Bursts of 200 back-to-back requests every 1000 iters mimic the FS/REGISTRY
 * catalog storm. On a lost wake it logs WHERE the message died (pipe? read
 * completion? semaphore?) and exits 1. Clean run exits 0.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC_SIZE   64
#define BURST_EVERY 1000
#define BURST_LEN   200
#define TOTAL_ITERS 200000
#define WAIT_MS     10000

static FILE* g_log;
static void logf_(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[512]; vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(stderr, "%s\n", buf);
    if (g_log) { fprintf(g_log, "%lu %s\n", GetTickCount(), buf); fflush(g_log); }
}

typedef struct {
    HANDLE pipe, iocp;
    const char* name;
    volatile LONG recs_in;      /* complete records parsed */
    volatile LONG reads_ok;     /* read completions */
    volatile LONG wakes;        /* PQCS wakes seen */
    int is_browser;             /* browser echoes; renderer signals sem */
    HANDLE reply_sem;
    char rdbuf[4096];
    OVERLAPPED rd_ov;
    char acc[REC_SIZE * 4]; int acc_len;
} Side;

static void arm_read(Side* s) {
    memset(&s->rd_ov, 0, sizeof s->rd_ov);
    if (!ReadFile(s->pipe, s->rdbuf, sizeof s->rdbuf, NULL, &s->rd_ov)
        && GetLastError() != ERROR_IO_PENDING) {
        logf_("%s: ReadFile arm failed %lu", s->name, GetLastError());
        ExitProcess(2);
    }
}

static void ov_write(HANDLE pipe, const void* data, DWORD len, const char* who) {
    OVERLAPPED ov; memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD wr = 0;
    if (!WriteFile(pipe, data, len, NULL, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) { logf_("%s: write failed %lu", who, GetLastError()); ExitProcess(2); }
        if (WaitForSingleObject(ov.hEvent, WAIT_MS) != WAIT_OBJECT_0) { logf_("%s: WRITE COMPLETION LOST", who); ExitProcess(1); }
    }
    GetOverlappedResult(pipe, &ov, &wr, FALSE);
    CloseHandle(ov.hEvent);
    if (wr != len) { logf_("%s: short write %lu/%lu", who, wr, len); ExitProcess(2); }
}

static DWORD WINAPI io_thread(LPVOID arg) {
    Side* s = (Side*)arg;
    arm_read(s);
    for (;;) {
        DWORD bytes = 0; ULONG_PTR key = 0; OVERLAPPED* pov = NULL;
        BOOL ok = GetQueuedCompletionStatus(s->iocp, &bytes, &key, &pov, INFINITE);
        if (key == 0xDEAD) return 0;
        if (key == 0xCAFE) { InterlockedIncrement(&s->wakes); continue; }   /* ScheduleWork wake */
        if (!ok || !pov) { logf_("%s: GQCS err %lu", s->name, GetLastError()); continue; }
        if (pov != &s->rd_ov) continue;   /* write completion packet — not ours to touch */
        InterlockedIncrement(&s->reads_ok);
        /* accumulate byte stream, parse 64-byte records */
        if ((int)bytes > 0) {
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
                    if (s->is_browser) ov_write(s->pipe, s->acc, REC_SIZE, "browser-reply");
                    else ReleaseSemaphore(s->reply_sem, 1, NULL);
                }
            }
        }
        arm_read(s);
    }
}

static volatile LONG g_stop_waker;
static Side g_b, g_r;
static DWORD WINAPI waker_thread(LPVOID arg) {
    (void)arg;
    while (!g_stop_waker) {
        PostQueuedCompletionStatus(g_b.iocp, 0, 0xCAFE, NULL);
        PostQueuedCompletionStatus(g_r.iocp, 0, 0xCAFE, NULL);
        Sleep(0);
    }
    return 0;
}

int main(void) {
    g_log = fopen("C:\\mojo_stress.log", "w");
    logf_("mojo_stress start (pid %lu)", GetCurrentProcessId());

    HANDLE h1 = CreateNamedPipeA("\\\\.\\pipe\\mojostress",
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, NULL);
    if (h1 == INVALID_HANDLE_VALUE) { logf_("CreateNamedPipe failed %lu", GetLastError()); return 2; }
    OVERLAPPED cov; memset(&cov, 0, sizeof cov);
    cov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ConnectNamedPipe(h1, &cov);
    HANDLE h2 = CreateFileA("\\\\.\\pipe\\mojostress", GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h2 == INVALID_HANDLE_VALUE) { logf_("client CreateFile failed %lu", GetLastError()); return 2; }
    WaitForSingleObject(cov.hEvent, 5000);

    g_b.pipe = h1; g_b.name = "browser";  g_b.is_browser = 1;
    g_r.pipe = h2; g_r.name = "renderer"; g_r.is_browser = 0;
    g_r.reply_sem = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL);
    g_b.iocp = CreateIoCompletionPort(h1, NULL, 1, 0);
    g_r.iocp = CreateIoCompletionPort(h2, NULL, 2, 0);
    if (!g_b.iocp || !g_r.iocp) { logf_("IOCP create failed"); return 2; }

    CreateThread(NULL, 0, io_thread, &g_b, 0, NULL);
    CreateThread(NULL, 0, io_thread, &g_r, 0, NULL);
    CreateThread(NULL, 0, waker_thread, NULL, 0, NULL);

    char rec[REC_SIZE];
    DWORD t0 = GetTickCount();
    for (long i = 1; i <= TOTAL_ITERS; i++) {
        int burst = (i % BURST_EVERY) == 0 ? BURST_LEN : 1;
        for (int k = 0; k < burst; k++) {
            memset(rec, 0, sizeof rec);
            *(long*)rec = i; *(int*)(rec + 8) = k;
            ov_write(h2, rec, REC_SIZE, "renderer-send");
        }
        for (int k = 0; k < burst; k++) {
            if (WaitForSingleObject(g_r.reply_sem, WAIT_MS) != WAIT_OBJECT_0) {
                logf_("LOST WAKE at iter %ld burst-slot %d/%d", i, k, burst);
                logf_("counters: browser recs_in=%ld reads=%ld wakes=%ld | renderer recs_in=%ld reads=%ld wakes=%ld",
                      g_b.recs_in, g_b.reads_ok, g_b.wakes, g_r.recs_in, g_r.reads_ok, g_r.wakes);
                DWORD avail = 0;
                PeekNamedPipe(h2, NULL, 0, NULL, &avail, NULL);
                logf_("renderer pipe pending unread bytes: %lu", avail);
                PeekNamedPipe(h1, NULL, 0, NULL, &avail, NULL);
                logf_("browser pipe pending unread bytes: %lu", avail);
                if (WaitForSingleObject(g_r.reply_sem, WAIT_MS) == WAIT_OBJECT_0)
                    logf_("...late wake arrived on retry (delayed, not lost)");
                else
                    logf_("...still nothing after 2nd wait — wake is LOST for good");
                return 1;
            }
        }
        if ((i % 100000) == 0) {
            DWORD dt = GetTickCount() - t0;
            logf_("iter %ld ok (%.0f rt/s) b.recs=%ld r.recs=%ld wakes=%ld",
                  i, 1000.0 * (i + (double)i / BURST_EVERY * (BURST_LEN-1)) / (dt ? dt : 1),
                  g_b.recs_in, g_r.recs_in, g_b.wakes);
        }
    }
    g_stop_waker = 1;
    logf_("PASS: %d sync round-trips + bursts, zero lost wakes", TOTAL_ITERS);
    return 0;
}
