#!/usr/bin/env python3
import sys

PATH = '/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload/main.c'
src = open(PATH, encoding='utf-8', errors='replace', newline='').read().replace('\r\n', '\n')

fails = []
def rep(old, new, count=1):
    global src
    if old not in src:
        fails.append(old[:70])
        return
    src = src.replace(old, new, count)

# ---------------------------------------------------------------- 1) include kernel.h + elevation code before main()
rep('''int main() {
    // Initialize worker threads for async disk I/O
    init_workers();''',
'''// ============================================================================
// SELF-ELEVATION (the official ps5-payload-elfldr pattern, elfldr.c:411-433)
//
// Our payload runs as an un-privileged process spawned by the loader. Sce
// service IPC (AppInstUtil/UserService/LncUtil) is only answered for
// elevated processes — without elevation those calls block forever (seen as
// "Mount Games: No response" / "Unmount hangs"). The SDK exposes the exact
// kernel primitives the official elfldr uses to raise privileges, linked
// statically from crt1.o (no extra -l flags, loader-compatible):
//
//   kernel_get_root_vnode / kernel_set_proc_rootdir / kernel_set_proc_jaildir
//   kernel_set_ucred_uid   / kernel_set_ucred_caps
//
// We keep a backup of the original jaildir/rootdir and restore them when the
// payload shuts down (CMD_SHUTDOWN) so the console stays clean.
// ============================================================================
#include <ps5/kernel.h>
#include <unistd.h>

static intptr_t g_orig_jaildir = 0;
static intptr_t g_orig_rootdir = 0;
static int      g_elevated     = 0;

static void payload_self_elevate(void) {
    pid_t me = getpid();

    // Backup the current jail/root dirs for a clean restore on shutdown
    g_orig_jaildir = kernel_get_proc_jaildir(me);
    g_orig_rootdir = kernel_get_proc_rootdir(me);

    intptr_t rootvnode = kernel_get_root_vnode();
    if (!rootvnode) {
        send_notification("Elevation: root vnode unavailable (loader limits)");
        return;
    }

    // The exact elfldr_raise_privileges recipe:
    static const uint8_t caps_all[16] = {
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
    };

    if (kernel_set_proc_rootdir(me, rootvnode) != 0 ||
        kernel_set_proc_jaildir(me, 0)        != 0 ||
        kernel_set_ucred_uid(me, 0)           != 0 ||
        kernel_set_ucred_caps(me, caps_all)   != 0) {
        send_notification("Elevation failed — Sce services may be limited");
        return;
    }

    g_elevated = 1;
    send_notification("Privileges elevated — full features enabled");
}

static void payload_restore_privileges(void) {
    if (!g_elevated) return;
    pid_t me = getpid();
    if (g_orig_rootdir) kernel_set_proc_rootdir(me, g_orig_rootdir);
    if (g_orig_jaildir) kernel_set_proc_jaildir(me, g_orig_jaildir);
    g_elevated = 0;
}

int main() {
    // Elevate BEFORE anything else (elfldr does the same before spawning)
    payload_self_elevate();

    // Initialize worker threads for async disk I/O
    init_workers();''')

if fails:
    print("FAILED anchors:")
    for f in fails: print("  -", f)
    sys.exit(1)

open(PATH, 'w', encoding='utf-8', newline='').write(src.replace('\n', '\r\n'))
print("[+] self-elevation added (elfldr pattern) before init_workers in main()")
