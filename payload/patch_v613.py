#!/usr/bin/env python3
# v6.1.3 hotfix patcher for payload/main.c (CRLF file, UTF-8)
import sys

PATH = "main.c"

with open(PATH, "r", encoding="utf-8", newline="") as f:
    src = f.read()

patches = []

# ---------------------------------------------------------------------------
# FIX 1: auto_unmount_deleted_games — remove the kernel-wedging deletes
# ---------------------------------------------------------------------------
patches.append((
    "auto_unmount: drop rmdir_recursive calls",
    """                // v6.1.2: use the same safe order as the unmount command —\r
                // unmount FIRST (above), then unregister (fire-and-forget —\r
                // the registry IPC can stall), then delete.\r
                unregister_title_async(e->d_name);\r
                \r
                char user_app_dir[PATH_MAX];\r
                snprintf(user_app_dir, sizeof(user_app_dir), \r
                         "/user/app/%s", e->d_name);\r
                rmdir_recursive(user_app_dir);\r
                \r
                char appmeta_dir[PATH_MAX];\r
                snprintf(appmeta_dir, sizeof(appmeta_dir), \r
                         "/user/appmeta/%s", e->d_name);\r
                rmdir_recursive(appmeta_dir);\r
                \r
                unmounted++;""",
    """                // v6.1.3: same policy as full_title_cleanup — NO deletes\r
                // here. ShellCore holds /user/app + /user/appmeta icons OPEN\r
                // (the pic0.dds home-background case, live test 2026-10-01);\r
                // every unlink()/rmdir() on them wedges the kernel and kills\r
                // the whole payload — this was the Mount-Games freezer. We\r
                // unmount + unregister only. The stale mount.lnk keeps the\r
                // entry visible in the game list so it can be unmounted later\r
                // (which unregisters it too).\r
                unregister_title_async(e->d_name);\r
                \r
                unmounted++;""",
))

# ---------------------------------------------------------------------------
# FIX 1b: update the full_title_cleanup comment to reflect the wider policy
# ---------------------------------------------------------------------------
patches.append((
    "full_title_cleanup comment: policy now global",
    """    // them. Leftovers are cleaned by the system or at the next Mount Games.""",
    """    // them. v6.1.3: the same no-delete policy now applies to the Mount\r
    // Games stale cleanup (auto_unmount_deleted_games) — zero deletes anywhere.""",
))

# ---------------------------------------------------------------------------
# FIX 2: process_game — fine-grained progress so a live test pinpoints the
#        exact freezing step
# ---------------------------------------------------------------------------
patches.append((
    "process_game: Checking message",
    """    // Copy title_id out if requested\r
    if (title_id_out && tid_size > 0) {""",
    """    {\r
        char chk[128];\r
        snprintf(chk, sizeof(chk), "Checking %s...", title_id);\r
        send_progress_message(chk);\r
    }\r
\r
    // Copy title_id out if requested\r
    if (title_id_out && tid_size > 0) {""",
))

patches.append((
    "process_game: pre-mount messages",
    """    mkdir(system_ex_app, 0755);\r
\r
    if (is_mounted(system_ex_app)) {\r
        unmount_guarded(system_ex_app, 0);\r
    }\r
\r
    if (mount_nullfs(game_path, system_ex_app)) {\r
        return -1;\r
    }""",
    """    mkdir(system_ex_app, 0755);\r
\r
    if (is_mounted(system_ex_app)) {\r
        {\r
            char chk[128];\r
            snprintf(chk, sizeof(chk), "%s: unmounting stale mount...", title_id);\r
            send_progress_message(chk);\r
        }\r
        unmount_guarded(system_ex_app, 0);\r
    }\r
\r
    {\r
        char chk[128];\r
        snprintf(chk, sizeof(chk), "%s: mounting game data...", title_id);\r
        send_progress_message(chk);\r
    }\r
    if (mount_nullfs(game_path, system_ex_app)) {\r
        {\r
            char chk[128];\r
            snprintf(chk, sizeof(chk), "%s: ERROR - mount failed", title_id);\r
            send_progress_message(chk);\r
        }\r
        return -1;\r
    }""",
))

patches.append((
    "process_game: post-mount messages",
    """    copy_dir(src_sce_sys, user_sce_sys);\r
    copy_sce_sys_to_appmeta(src_sce_sys, title_id);""",
    """    {\r
        char chk[128];\r
        snprintf(chk, sizeof(chk), "%s: copying sce_sys files...", title_id);\r
        send_progress_message(chk);\r
    }\r
    copy_dir(src_sce_sys, user_sce_sys);\r
    copy_sce_sys_to_appmeta(src_sce_sys, title_id);""",
))

patches.append((
    "process_game: registration messages",
    """    int reg_rc = register_title(title_id);\r
    if (reg_rc == -2 || reg_rc == -3) {""",
    """    {\r
        char chk[128];\r
        snprintf(chk, sizeof(chk), "%s: registering with system...", title_id);\r
        send_progress_message(chk);\r
    }\r
    int reg_rc = register_title(title_id);\r
    if (reg_rc == -2 || reg_rc == -3) {""",
))

# ---------------------------------------------------------------------------
# FIX 3: handle_mount_games — detached worker + heartbeat (same as unmount).
#        The mount_job_t typedef must precede the worker (C declares-before-use),
#        so this patch inserts it together with the renamed worker signature.
# ---------------------------------------------------------------------------
patches.append((
    "mount worker: typedef + rename signature",
    """void handle_mount_games(client_session_t *session) {\r
    // Stream RESP_PROGRESS lines to THIS client while the scan runs\r
    progress_socket_set(session->sock);\r
\r
    // Serialize concurrent mounts""",
    """// v6.1.3: job state shared by the mount worker and its heartbeat thread.\r
typedef struct {\r
    int client_sock;\r
    int done;              // atomic: heartbeat stops once the worker finishes\r
} mount_job_t;\r
\r
static void* mount_games_worker(void* arg) {\r
    mount_job_t* job = (mount_job_t*)arg;\r
    int sock = job->client_sock;\r
    // Stream RESP_PROGRESS lines to THIS client while the scan runs\r
    progress_socket_set(sock);\r
\r
    // Serialize concurrent mounts""",
))

patches.append((
    "mount worker: final send + heartbeat + new handler",
    """        send_notification("Game Mounter\\nNo games found to mount");\r
    }\r
\r
    send_ok(session->sock, response);\r
    progress_socket_clear(session->sock);\r
}""",
    """        send_notification("Game Mounter\\nNo games found to mount");\r
    }\r
\r
    // done BEFORE the final OK so the heartbeat can never interleave a\r
    // progress frame with the response (mirrors the unmount worker).\r
    __atomic_store_n(&job->done, 1, __ATOMIC_SEQ_CST);\r
    send_ok(sock, response);\r
    progress_socket_clear(sock);\r
}\r
\r
// Heartbeat thread: streams a RESP_PROGRESS every 3s so the client never sits\r
// on a silent socket past its read timeout while the mount grinds on.\r
static void* mount_games_heartbeat(void* arg) {\r
    mount_job_t* job = (mount_job_t*)arg;\r
    struct timespec one_sec = { 1, 0 };\r
    for (int i = 0; i < 40; i++) {              // 40 x 3s = 2 min, then give up\r
        for (int j = 0; j < 3; j++) {\r
            if (__atomic_load_n(&job->done, __ATOMIC_SEQ_CST)) return NULL;\r
            nanosleep(&one_sec, NULL);\r
        }\r
        if (__atomic_load_n(&job->done, __ATOMIC_SEQ_CST)) return NULL;\r
        send_progress_message("Mount Games still working...");\r
    }\r
    return NULL;\r
}\r
\r
// v6.1.3: Mount Games now runs on a detached worker with its own heartbeat\r
// thread (same pattern as the unmount path). Before, the whole scan ran on\r
// the command thread: a single blocked syscall (busy vnode, wedged dlopen)\r
// froze the command loop — no heartbeats, no other commands, payload looked\r
// dead and had to be killed from the Toolbox. Now the command loop returns\r
// immediately and RESP_PROGRESS lines keep the client fed.\r
void handle_mount_games(client_session_t *session) {\r
    mount_job_t* job = (mount_job_t*)malloc(sizeof(mount_job_t));\r
    if (!job) {\r
        send_error(session->sock, "Out of memory");\r
        return;\r
    }\r
    memset(job, 0, sizeof(*job));\r
    job->client_sock = session->sock;\r
\r
    // Immediate acknowledgement: the client hears from us within\r
    // milliseconds. The worker owns progress + final OK from here on.\r
    progress_socket_set(session->sock);\r
    send_progress_message("Starting Mount Games...");\r
\r
    pthread_attr_t at;\r
    pthread_attr_init(&at);\r
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);\r
    pthread_t hb_tid;\r
    bool hb_ok = (pthread_create(&hb_tid, &at, mount_games_heartbeat, job) == 0);\r
    if (!hb_ok) pthread_attr_destroy(&at);\r
\r
    pthread_t w_tid;\r
    if (pthread_create(&w_tid, hb_ok ? NULL : &at, mount_games_worker, job) != 0) {\r
        if (hb_ok) pthread_cancel(hb_tid);\r
        pthread_attr_destroy(&at);\r
        free(job);\r
        progress_socket_clear(session->sock);\r
        send_error(session->sock, "Failed to start mount worker");\r
        return;\r
    }\r
    if (hb_ok) pthread_attr_destroy(&at);\r
    // Session thread returns to the command loop immediately.\r
}""",
))

# ---------------------------------------------------------------------------
# FIX 4: opt_module — never hold g_opt_mods_lock across dlopen, and never
#        queue a second dlopen behind a wedged one
# ---------------------------------------------------------------------------
patches.append((
    "opt_module: dlopen outside the lock + in-flight guard",
    """// dlopen a module once; returns NULL if unavailable on this loader.\r
static void *opt_module(opt_module_t m) {\r
    pthread_mutex_lock(&g_opt_mods_lock);\r
    if (!g_opt_mods_tried[m]) {\r
        g_opt_mods_tried[m] = 1;\r
        g_opt_mods[m] = dlopen(g_opt_mod_names[m], RTLD_LAZY);\r
    }\r
    void *h = g_opt_mods[m];\r
    pthread_mutex_unlock(&g_opt_mods_lock);\r
    return h;\r
}""",
    """// v6.1.3: dlopen() can WEDGE inside the loader (live test 2026-10-01: a\r
// background unreg thread stuck inside dlopen while the mount froze on\r
// g_opt_mods_lock behind it). Two hardenings:\r
//  1. never block on g_opt_mods_lock while a dlopen is in flight — fail fast\r
//     (callers map NULL to -3 "module unavailable", all watchdog-guarded);\r
//  2. a wedged dlopen marks the module tried+absent forever, so every later\r
//     call fails fast and no thread ever re-enters the loader.\r
static volatile int g_dlopen_in_progress = 0;\r
\r
// dlopen a module once; returns NULL if unavailable on this loader.\r
static void *opt_module(opt_module_t m) {\r
    if (__atomic_load_n(&g_dlopen_in_progress, __ATOMIC_SEQ_CST))\r
        return NULL;              // a dlopen is wedged/in-flight — do not queue\r
    pthread_mutex_lock(&g_opt_mods_lock);\r
    if (!g_opt_mods_tried[m]) {\r
        if (__atomic_load_n(&g_dlopen_in_progress, __ATOMIC_SEQ_CST)) {\r
            pthread_mutex_unlock(&g_opt_mods_lock);\r
            return NULL;\r
        }\r
        g_opt_mods_tried[m] = 1;  // set BEFORE dlopen: a wedge = absent forever\r
        __atomic_store_n(&g_dlopen_in_progress, 1, __ATOMIC_SEQ_CST);\r
        pthread_mutex_unlock(&g_opt_mods_lock);   // NEVER hold a lock across dlopen\r
        void *h = dlopen(g_opt_mod_names[m], RTLD_LAZY);\r
        __atomic_store_n(&g_dlopen_in_progress, 0, __ATOMIC_SEQ_CST);\r
        pthread_mutex_lock(&g_opt_mods_lock);\r
        g_opt_mods[m] = h;\r
    }\r
    void *h = g_opt_mods[m];\r
    pthread_mutex_unlock(&g_opt_mods_lock);\r
    return h;\r
}""",
))

# ---------------------------------------------------------------------------
# Version bump
# ---------------------------------------------------------------------------
patches.append((
    "system info version",
    '"server_version=6.1.2\\n"',
    '"server_version=6.1.3\\n"',
))

patches.append((
    "boot notification version",
    '"PS5 Upload Server v6.1.2: %s:%d - By Manos"',
    '"PS5 Upload Server v6.1.3: %s:%d - By Manos"',
))

# ---------------------------------------------------------------------------
# Apply with strict count checking — NOTHING is written unless every patch
# matches exactly once and all sanity checks pass.
# ---------------------------------------------------------------------------
failed = False
for name, old, new in patches:
    n = src.count(old)
    if n != 1:
        print(f"FAIL [{name}]: expected 1 match, found {n}")
        failed = True
if failed:
    sys.exit(1)

for name, old, new in patches:
    src = src.replace(old, new)
    print(f"OK   [{name}]")

# Sanity checks on the patched source (in memory).
start = src.index("static void* mount_games_worker(void* arg)")
end = src.index("static void* mount_games_heartbeat", start)
worker_body = src[start:end]
for bad in ("session->", "client_session_t"):
    if bad in worker_body:
        print(f"FAIL: worker body still references {bad}")
        sys.exit(1)

handler_start = src.index("void handle_mount_games(client_session_t *session)")
handler_body = src[handler_start:handler_start + 2000]
for need in ("mount_games_worker", "mount_games_heartbeat", "mount_job_t"):
    if need not in handler_body:
        print(f"FAIL: handler missing {need}")
        sys.exit(1)

# The worker body must come before the heartbeat and handler in the file
# (declaration order for the job type).
if not (start < end < handler_start):
    print("FAIL: worker/heartbeat/handler out of order")
    sys.exit(1)

with open(PATH, "w", encoding="utf-8", newline="") as f:
    f.write(src)

print("\nAll v6.1.3 patches applied cleanly.")
