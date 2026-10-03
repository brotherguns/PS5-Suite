#!/usr/bin/env python3
import re, sys

PATH = '/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload/main.c'
src = open(PATH, encoding='utf-8', errors='replace', newline='').read()

# ---------------------------------------------------------------- 1) v1 watchdog + p_* globals + old wrappers -> v2 + one-liner wrappers
pat = re.compile(
    r"// -{20,}\r?\n// WATCHDOG WRAPPER.*?return wdg_call4\(\(void \*\)p_sceAppInstUtilAppUnInstall, \(void \*\)title_id, NULL, NULL, NULL, WDG_TIMEOUT_MS\);\r?\n\}",
    re.DOTALL)

new_block = '''// ----------------------------------------------------------------------------
// WATCHDOG WRAPPER (v2): resolve + call Sce APIs entirely inside a deadline
// thread. The previous version guarded only the CALL — but dlopen() of the
// service module itself can block in an un-privileged payload process, and it
// ran on the session thread, freezing the whole mount anyway. Now dlopen ->
// dlsym -> call all happen inside the watchdog thread under one deadline,
// with a polling wait (no reliance on pthread_timedjoin) and per-module
// "dead" memoization so a hanging service costs at most ONE timeout, ever.
// ----------------------------------------------------------------------------
typedef struct {
    opt_module_t  mod;
    const char   *sym;
    void         *args[4];
    int           done;     // set by the worker when the call returned
    int           result;
} wdg_call_t;

static int g_mod_dead[MOD_COUNT];   // 1 = this module already timed out once

static void *wdg_trampoline(void *arg) {
    wdg_call_t *c = (wdg_call_t *)arg;
    void *h = opt_module(c->mod);
    if (h) {
        void *fn = dlsym(h, c->sym);
        if (fn) {
            int (*fn4)(void *, void *, void *, void *) =
                (int (*)(void *, void *, void *, void *))fn;
            c->result = fn4(c->args[0], c->args[1], c->args[2], c->args[3]);
        } else {
            c->result = -3;     // module loaded but symbol missing
        }
    } else {
        c->result = -3;         // module unavailable on this loader
    }
    __atomic_store_n(&c->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

// Run mod!sym(a,b,c,d) with a hard deadline. Returns:
//   >=0 : the Sce function's return value
//   -2  : timed out (worker detached; result never arrives)
//   -3  : module or symbol unavailable on this loader
static int wdg_sce_call(opt_module_t mod, const char *sym,
                        void *a, void *b, void *c, void *d, int timeout_ms) {
    if (__atomic_load_n(&g_mod_dead[mod], __ATOMIC_SEQ_CST))
        return -2;              // already timed out once — fail fast forever

    wdg_call_t *call = (wdg_call_t *)malloc(sizeof(wdg_call_t));
    if (!call) return -3;
    call->mod = mod;
    call->sym = sym;
    call->args[0] = a; call->args[1] = b; call->args[2] = c; call->args[3] = d;
    call->done = 0;
    call->result = -3;

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    int rc = pthread_create(&tid, &at, wdg_trampoline, call);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        free(call);
        return -3;
    }

    int waited = 0;
    while (waited < timeout_ms) {
        if (__atomic_load_n(&call->done, __ATOMIC_SEQ_CST)) {
            int res = call->result;
            free(call);
            return res;
        }
        struct timespec ms20 = { 0, 20 * 1000 * 1000 };
        nanosleep(&ms20, NULL);
        waited += 20;
    }

    // Deadline hit: mark the module dead so every later call fails fast.
    // (The detached worker leaks its 48-byte state if it is stuck forever.)
    __atomic_store_n(&g_mod_dead[mod], 1, __ATOMIC_SEQ_CST);
    return -2;
}

#define WDG_TIMEOUT_MS 10000

// ---- Drop-in wrappers: same names/signatures as the original externs ----
// Every Sce-service call runs under the watchdog; call sites are unchanged.

static int sceKernelGetHwModelName(char *name) {
    return wdg_sce_call(MOD_KERNEL_SYS, "sceKernelGetHwModelName", name, NULL, NULL, NULL, WDG_TIMEOUT_MS);
}

static int sceKernelGetHwSerialNumber(char *serial) {
    return wdg_sce_call(MOD_KERNEL_SYS, "sceKernelGetHwSerialNumber", serial, NULL, NULL, NULL, WDG_TIMEOUT_MS);
}

static int sceLncUtilInitialize(void) {
    return wdg_sce_call(MOD_SYSTEMSERVICE, "sceLncUtilInitialize", NULL, NULL, NULL, NULL, WDG_TIMEOUT_MS);
}

static int sceLncUtilLaunchApp(const char *title_id, char *argv, void *param) {
    return wdg_sce_call(MOD_SYSTEMSERVICE, "sceLncUtilLaunchApp", (void *)title_id, argv, param, NULL, WDG_TIMEOUT_MS);
}

static int sceSystemServiceLaunchApp(const char *title_id, const char **argv, void *opts) {
    return wdg_sce_call(MOD_SYSTEMSERVICE, "sceSystemServiceLaunchApp", (void *)title_id, (void *)argv, opts, NULL, WDG_TIMEOUT_MS);
}

static int sceUserServiceInitialize(int *priority) {
    return wdg_sce_call(MOD_USERSERVICE, "sceUserServiceInitialize", priority, NULL, NULL, NULL, WDG_TIMEOUT_MS);
}

static int sceUserServiceGetForegroundUser(int *user_id) {
    return wdg_sce_call(MOD_USERSERVICE, "sceUserServiceGetForegroundUser", user_id, NULL, NULL, NULL, WDG_TIMEOUT_MS);
}

static int sceAppInstUtilInitialize(void) {
    return wdg_sce_call(MOD_APPINSTUTIL, "sceAppInstUtilInitialize", NULL, NULL, NULL, NULL, WDG_TIMEOUT_MS);
}

static int sceAppInstUtilAppInstallTitleDir(const char *title_id, const char *base_path, void *reserved) {
    return wdg_sce_call(MOD_APPINSTUTIL, "sceAppInstUtilAppInstallTitleDir", (void *)title_id, (void *)base_path, reserved, NULL, WDG_TIMEOUT_MS);
}

static int sceAppInstUtilAppUnInstall(const char *title_id) {
    return wdg_sce_call(MOD_APPINSTUTIL, "sceAppInstUtilAppUnInstall", (void *)title_id, NULL, NULL, NULL, WDG_TIMEOUT_MS);
}'''

src2, n1 = pat.subn(lambda m: new_block.replace('\n', m.group(0)[:0] or '\n'), src, count=1)
if n1 != 1:
    print("FATAL: watchdog block pattern not matched"); sys.exit(1)
# restore CRLF consistency: the file is CRLF; subn inserted LF-only text. Normalize new_block region:
# (simplest: normalize whole file to CRLF)
src2 = src2.replace('\r\n', '\n').replace('\n', '\r\n')
src = src2

# ---------------------------------------------------------------- 2) process_game: treat -3 like -2 (module unavailable -> best effort)
old = "    if (reg_rc == -2) {"
new = "    if (reg_rc == -2 || reg_rc == -3) {"
if old not in src:
    print("FATAL: reg_rc anchor not found"); sys.exit(1)
src = src.replace(old, new, 1)

# ---------------------------------------------------------------- 3) handle_mount_games: wire the progress socket
old = "void handle_mount_games(client_session_t *session) {\r\n    // Serialize concurrent mounts"
new = ("void handle_mount_games(client_session_t *session) {\r\n"
       "    // Stream RESP_PROGRESS lines to THIS client while the scan runs\r\n"
       "    progress_socket_set(session->sock);\r\n\r\n"
       "    // Serialize concurrent mounts")
if old not in src:
    print("FATAL: mount handler anchor not found"); sys.exit(1)
src = src.replace(old, new, 1)

# ---------------------------------------------------------------- 4) clear the progress socket on mount exit
old = "    send_ok(session->sock, response);\r\n}\r\n\r\n// ============================================================================\r\n// HARDWARE & SYSTEM INFO FUNCTIONS"
new = ("    send_ok(session->sock, response);\r\n"
       "    progress_socket_clear(session->sock);\r\n"
       "}\r\n\r\n"
       "// ============================================================================\r\n"
       "// HARDWARE & SYSTEM INFO FUNCTIONS")
if old not in src:
    print("FATAL: mount exit anchor not found"); sys.exit(1)
src = src.replace(old, new, 1)

open(PATH, 'w', encoding='utf-8', newline='').write(src)
print("[+] patch applied: v2 watchdog + progress socket wiring + reg_rc -3 handling")
