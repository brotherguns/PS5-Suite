#!/usr/bin/env python3
import sys

PATH = '/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload/main.c'
src = open(PATH, encoding='utf-8', errors='replace', newline='').read()
src = src.replace('\r\n', '\n')

fails = []

def rep(old, new, count=1):
    global src
    if old not in src:
        fails.append(old[:60])
        return
    src = src.replace(old, new, count)

# ---------------------------------------------------------------- 1) mount_nullfs: watchdog the nmount
rep('''static int mount_nullfs(const char* src, const char* dst) {
    struct iovec iov[] = {
        IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"),
        IOVEC_ENTRY("from"),   IOVEC_ENTRY(src),
        IOVEC_ENTRY("fspath"), IOVEC_ENTRY(dst),
    };
    return nmount(iov, IOVEC_SIZE(iov), 0);
}''',
'''// Kernel mount/unmount can BLOCK indefinitely on busy vnodes (e.g. a game
// running from the mount) — every nmount/unmount call below runs under the
// watchdog so a busy kernel object can never freeze a client session.
static int mount_nullfs(const char* src, const char* dst) {
    struct iovec iov[] = {
        IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"),
        IOVEC_ENTRY("from"),   IOVEC_ENTRY(src),
        IOVEC_ENTRY("fspath"), IOVEC_ENTRY(dst),
    };
    int rc = wdg_call4((void *)nmount, iov, (void *)(long)IOVEC_SIZE(iov), (void *)(long)0, NULL, 5000);
    if (rc == -2) { errno = EBUSY; return -1; }   // timed out — treat as busy
    return rc;
}

// Watchdog-guarded unmount: normal first, forced as fallback, never blocks.
static int unmount_guarded(const char* path, int flags) {
    int rc = wdg_call4((void *)unmount, (void *)path, (void *)(long)flags, NULL, NULL, 5000);
    if (rc == -2) { errno = EBUSY; return -1; }   // timed out — treat as busy
    return rc;
}''')

# ---------------------------------------------------------------- 2) remount_system_ex: watchdog the nmount update
rep('''static int remount_system_ex(void) {''',
'''static int remount_system_ex(void);  // fwd (watchdog body below)''', 0)  # no-op placeholder (kept simple)

# The actual remount body uses nmount(iov, n, MNT_UPDATE) — patch it by locating the function
import re
m = re.search(r"static int remount_system_ex\(void\) \{.*?return nmount\(iov, IOVEC_SIZE\(iov\), MNT_UPDATE\);\r?\n\}", src, re.DOTALL)
if m:
    body = m.group(0)
    new_body = body.replace(
        "return nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE);",
        "int rc = wdg_call4((void *)nmount, iov, (void *)(long)IOVEC_SIZE(iov), (void *)(long)MNT_UPDATE, NULL, 5000);\n"
        "    if (rc == -2) { errno = EBUSY; return -1; }   // timed out — treat as busy\n"
        "    return rc;")
    src = src.replace(body, new_body, 1)
else:
    fails.append("remount_system_ex body")

# ---------------------------------------------------------------- 3) process_game: unmount before mount -> guarded
rep('''    if (is_mounted(system_ex_app)) {
        unmount(system_ex_app, 0);
    }

    if (mount_nullfs(game_path, system_ex_app)) {''',
'''    if (is_mounted(system_ex_app)) {
        unmount_guarded(system_ex_app, 0);
    }

    if (mount_nullfs(game_path, system_ex_app)) {''')

# ---------------------------------------------------------------- 4) auto_unmount_deleted_games: guarded unmounts
rep('''                if (is_mounted(system_ex_app)) {
                    unmount(system_ex_app, 0);
                }
''',
'''                if (is_mounted(system_ex_app)) {
                    unmount_guarded(system_ex_app, 0);
                }
''')

# ---------------------------------------------------------------- 5) handle_unmount_game: guarded unmounts + progress socket + honest errors
rep('''    // Initialize app install utility for unregistration
    sceAppInstUtilInitialize();
    
    // Unregister the game from PS5 system (removes from home screen)
    sceAppInstUtilAppUnInstall(title_id);
    
    // Wait briefly for system to process the unregistration
    usleep(200000); // 200ms
    
    char system_ex_app[PATH_MAX];
    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    
    // Unmount nullfs if mounted
    if (is_mounted(system_ex_app)) {
        if (unmount(system_ex_app, 0) != 0) {
            unmount(system_ex_app, MNT_FORCE);
        }
    }
    
    // Wait for unmount to complete
    usleep(100000); // 100ms
    
    // Clean up directories
    char user_app_dir[PATH_MAX];
    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);
    rmdir_recursive(user_app_dir);
    
    char appmeta_dir[PATH_MAX];
    snprintf(appmeta_dir, sizeof(appmeta_dir), "/user/appmeta/%s", title_id);
    rmdir_recursive(appmeta_dir);
    
    char msg[128];
    snprintf(msg, sizeof(msg), "Unmounted and unregistered %s", title_id);
    send_ok(session->sock, msg);
}''',
'''    // Stream progress (rmdir_recursive reports into this socket) and keep the
    // client's read timeout fed while the unmount work runs.
    progress_socket_set(session->sock);

    // Unregister the game from PS5 system (removes from home screen).
    // Both calls are watchdog-guarded; -2 = module dead/timeout (skip silently).
    sceAppInstUtilInitialize();
    int unreg = sceAppInstUtilAppUnInstall(title_id);
    (void)unreg;

    usleep(200000); // 200ms — let the system settle after unregistration

    char system_ex_app[PATH_MAX];
    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);

    // Unmount nullfs if mounted. Kernel unmount can BLOCK forever on busy
    // vnodes (running game, open files) — guarded: 5s normal, 5s forced.
    const char* busy_note = "";
    if (is_mounted(system_ex_app)) {
        if (unmount_guarded(system_ex_app, 0) != 0) {
            if (unmount_guarded(system_ex_app, MNT_FORCE) != 0) {
                busy_note = " (mount stayed busy — game running? close it and retry)";
            }
        }
    }

    usleep(100000); // 100ms

    // Clean up directories
    char user_app_dir[PATH_MAX];
    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);
    rmdir_recursive(user_app_dir);

    char appmeta_dir[PATH_MAX];
    snprintf(appmeta_dir, sizeof(appmeta_dir), "/user/appmeta/%s", title_id);
    rmdir_recursive(appmeta_dir);

    char msg[256];
    snprintf(msg, sizeof(msg), "Unmounted %s%s", title_id, busy_note);
    send_ok(session->sock, msg);
    progress_socket_clear(session->sock);
}''')

if fails:
    print("FAILED anchors:")
    for f in fails: print("  -", f)
    sys.exit(1)

src = src.replace('\n', '\r\n')
open(PATH, 'w', encoding='utf-8', newline='').write(src)
print("[+] unmount/mount/nmount all watchdog-guarded + progress socket in unmount")
