/* PS5 Suite Server - Custom High-Speed Protocol
 * By Manos
 * Port: 9113
 * Protocol: Custom binary for maximum speed
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ptrace.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <sys/wait.h>
#include <signal.h>
#include <setjmp.h>
#include <poll.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/ioctl.h>
#include <strings.h>
#include <sqlite3.h>
#include <dlfcn.h>

// Sony PS5 kernel functions for hardware monitoring
// IMPORTANT: keep this extern list restricted to symbols PROVEN to resolve at
// runtime in every loader context (etaHEN/elfldr). Extra imports that exist in
// the stub .so but NOT in the runtime libkernel variant kill the ELF at load.
extern int  sceKernelGetCpuTemperature(int *temperature);
extern int  sceKernelGetSocSensorTemperature(int sensor_id, int *temperature);
extern long sceKernelGetCpuFrequency(void);
extern int sceKernelGetSocPowerConsumption(uint32_t *power_mw);
extern size_t sceKernelGetDirectMemorySize(void);

// ============================================================================
// OPTIONAL SYSTEM APIs — resolved at RUNTIME via dlopen
//
// Restrictive payload loaders (older etaHEN ELFLDRs, webkit/BD-J loaders)
// only provide the "web" module set: libkernel_web + libSceLibcInternal +
// libSceNet. ELFs that link libkernel_sys / Sce services get REJECTED by
// such loaders ("Failed to load payload") before main() ever runs.
//
// Fix: link ONLY against the default (web) set and dlopen the extra modules
// here. If a module is missing, the affected feature degrades gracefully
// (default values / error responses) instead of the ELF failing to load.
// ============================================================================
// NOTE: The old dlopen/dlsym approach was removed in v6.2.0.
// All Sony APIs now use kernel_dynlib resolution with NIDs for reliability.
// This is the same method used by dump_installer and other proven tools.
// ============================================================================

// ============================================================================
// KERNEL DYNLIB RESOLUTION (dump_installer method)
// Uses kernel_dynlib_handle + kernel_dynlib_resolve with NIDs instead of
// dlopen/dlsym which doesn't work reliably on PS5 for Sony libraries.
// ============================================================================
#include <ps5/kernel.h>
#include <ps5/nid.h>
#include <ps5/mdbg.h>
#include <dlfcn.h>

// ============================================================================
// NID constants for Sony APIs (resolved via kernel_dynlib for reliability)
// NIDs generated with: prospero-nid <function_name>
// ============================================================================

// AppInstUtil NIDs (from dump_installer)
#define NID_APPINSTUTIL_INITIALIZE       "j+-2s3LOD7U"
#define NID_APPINSTUTIL_INSTALLTITLEDIR  "Wudg3Xe3heE"
#define NID_APPINSTUTIL_INSTALLALL       "1t5-jJHSvTs"
#define NID_APPINSTUTIL_UNINSTALL        "p0clsz-J-Gw"

// SystemService NIDs (sceLncUtil + sceSystemService)
#define NID_LNCUTIL_INITIALIZE           "f-Q8Nd33FBc"
#define NID_LNCUTIL_LAUNCHAPP            "+nRJUD-7qCk"
#define NID_SYSTEMSERVICE_LAUNCHAPP      "l4FB3wNa-Ac"

// UserService NIDs
#define NID_USERSERVICE_INITIALIZE       "j3YMu1MVNNo"
#define NID_USERSERVICE_GETFOREGROUNDUSER "eNb53LQJmIM"

// KernelSys NIDs (hardware info)
#define NID_KERNEL_GETHWMODELNAME        "JC7I7J1bllQ"
#define NID_KERNEL_GETHWSERIALNUMBER     "PgGve9EvhPE"

// Extended System Info NIDs — resolved at RUNTIME from the loaded kernel
// module. They cannot be extern-linked: symbols present in the stub .so but
// absent from the runtime libkernel variant make the ELF fail to launch.
#define NID_KERNEL_GETCPUTEMPERATURE     "qiL4fFObAxM"   // verification anchor
#define NID_KERNEL_LOADSTARTMODULE       "wzvqT4UqKX8"
#define NID_KERNEL_GETCPUUSAGEALL        "ToISj0q68Qg"
#define NID_KERNEL_GETCPUUSAGE           "ssmH9nMYO4o"
#define NID_KERNEL_AVAILABLEDIRECTMEMORYSIZE "C0f7TJcbfac"
#define NID_KERNEL_AVAILABLEFLEXIBLEMEMORYSIZE "aNz11fnnzi4"
#define NID_KERNEL_GETSYSTEMSWVERSION    "Mv1zUObHvXI"
#define NID_KERNEL_GETPRODUCTCODE        "pyAyDVpDOhw"
#define NID_KERNEL_GETPRODUCTSTR         "tlLu+KqzgDY"
#define NID_KERNEL_ICCGETHWINFO          "C+i9gJY4A1I"
#define NID_KERNEL_ICCGETTHERMALALERT    "zEmi6zvei2k"
#define NID_KERNEL_ICCGETPOWEROPERATINGTIME "PA6ZwQM5tNQ"
#define NID_KERNEL_ICCGETPOWERNUMBEROFBOOTSHUTDOWN "tCQzG0iC8zw"
#define NID_KERNEL_HWGETBDDRIVEINFO      "3B8820wLyCk"
#define NID_KERNEL_ICCGETBDPOWERSTATE    "zLEuSU+hl-w"
#define NID_KERNEL_GETMODULELIST         "IuxnUuXk6Bg"
#define NID_KERNEL_GETMODULEINFO         "kUpgrXIrz7Q"

// Sony service symbols — resolved via kernel_dynlib (dump_installer method)
// This is more reliable than static linking because it works even when the
// loader doesn't resolve all NEEDED deps.
extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilAppInstallAll(void *reserved);
extern int sceAppInstUtilAppUnInstall(const char *title_id);

// ============================================================================
// PKG Install structures (from ps5upload/etaHEN DPI)
// ============================================================================
#define PKG_NUM_LANGUAGES   30
#define PKG_NUM_IDS         64
#define PKG_SCENARIOID_SIZE 3
#define PKG_CONTENTID_SIZE  0x30
#define PKG_LANGUAGE_SIZE   8

typedef char pkg_scenario_id_t[PKG_SCENARIOID_SIZE];
typedef char pkg_language_t[PKG_LANGUAGE_SIZE];
typedef char pkg_content_id_t[PKG_CONTENTID_SIZE];

// Package info output structure
typedef struct {
    pkg_content_id_t content_id;
    int              content_type;
    int              content_platform;
} pkg_install_info_t;

// Meta info input structure
typedef struct {
    const char *uri;               // Package path or URL
    const char *ex_uri;            // Extra URI (usually empty)
    const char *playgo_scenario_id;
    const char *content_id;
    const char *content_name;      // Display name during install
    const char *icon_url;
} pkg_meta_info_t;

// PlayGo info structure
typedef struct {
    pkg_language_t    languages[PKG_NUM_LANGUAGES];
    pkg_scenario_id_t playgo_scenario_ids[PKG_NUM_IDS];
    pkg_content_id_t  content_ids[PKG_NUM_IDS];
    long              unknown[810];
} pkg_playgo_info_t;

// Extern for InstallByPackage (resolved via kernel_dynlib)
static int (*g_sceAppInstUtilInstallByPackage)(pkg_meta_info_t*, pkg_install_info_t*, pkg_playgo_info_t*) = NULL;
static int (*g_sceAppInstUtilGetInstallStatus)(const char*, void*) = NULL;
// etaHEN DPI: lets us ask Sony's parser whether a file is an installable pkg
// BEFORE InstallByPackage — distinguishes "bad pkg" from "bad URI/authid".
static int (*g_sceAppInstUtilGetContentIdFromPkg)(const char*, char*, bool*) = NULL;
static int g_pkginstutil_resolved = 0;
static pthread_mutex_t g_pkginstutil_lock = PTHREAD_MUTEX_INITIALIZER;

// Active PKG install tracking
static char g_active_pkg_content_id[PKG_CONTENTID_SIZE] = {0};
static int g_pkg_install_active = 0;

// ============================================================================
// AppInstUtil function pointers (resolved via kernel_dynlib for reliability)
static int (*g_sceAppInstUtilAppInstallTitleDir)(const char*, const char*, void*) = NULL;
static int g_appinstutil_resolved = 0;
static pthread_mutex_t g_appinstutil_lock = PTHREAD_MUTEX_INITIALIZER;

// SystemService externs (linked)
extern int sceLncUtilInitialize(void);
extern int sceLncUtilLaunchApp(const char *title_id, char *argv, void *param);
extern int sceSystemServiceLaunchApp(const char *title_id, const char **argv, void *opts);

// UserService externs (linked)
extern int sceUserServiceInitialize(void *priority);
extern int sceUserServiceGetForegroundUser(int *user_id);
extern int sceUserServiceGetLoginUserIdList(int userIdList[4]);
// libScePad — linked statically (-lScePad) so the kernel dynlinker loads the
// module at process start. dlopen'ing it later could hard-freeze the server.
extern int scePadInit(void);
extern int scePadOpen(int, int, int, void *);
extern int scePadGetHandle(int, int, int);
extern int scePadClose(int);
extern int scePadReadState(int, void *);
extern int scePadGetControllerInformation(int, void *);
extern int scePadSetLightBar(int, const void *);

// KernelSys function pointers
static int (*g_sceKernelGetHwModelName)(char*) = NULL;
static int (*g_sceKernelGetHwSerialNumber)(char*) = NULL;
static int g_kernelsys_resolved = 0;
static pthread_mutex_t g_kernelsys_lock = PTHREAD_MUTEX_INITIALIZER;

// Extended System Info function pointers (resolved at runtime from the kernel
// module loaded in our process — see find_kernel_module_handle).
static int (*g_sceKernelGetCpuUsageAll)(int*, int) = NULL;
static int (*g_sceKernelGetCpuUsage)(int, int*) = NULL;
// Real Orbis signatures (verified against ps5-hwinfo/shadPS4):
//   int  sceKernelGetCpuUsage(void *proc_stats_out, int32_t *count_inout)
//   int  sceKernelGetThreadName(uint32_t tid, char *out)
//   u64  sceKernelGetDirectMemorySize(void)
//   int  sceKernelAvailableDirectMemorySize(u64 start, u64 end, u64 align,
//                                           u64 *physOut, u64 *sizeOut)
static int (*g_sceKernelGetCpuUsageList)(void*, int32_t*) = NULL;
static int (*g_sceKernelGetThreadName)(uint32_t, char*) = NULL;
static uint64_t (*g_sceKernelGetDirectMemorySizeReal)(void) = NULL;
// int sceKernelGetPageTableStats(int vm, int type, int *total, int *free)
// int sceKernelConfiguredFlexibleMemorySize(u64 *out)  (or u64 fn(void))
static int (*g_sceKernelGetPageTableStats)(int, int, int*, int*) = NULL;
static uint64_t (*g_sceKernelConfiguredFlexibleMemorySize)(void) = NULL;
static size_t (*g_sceKernelAvailableDirectMemorySize)(void) = NULL;
static size_t (*g_sceKernelAvailableFlexibleMemorySize)(void) = NULL;
static int (*g_sceKernelGetSystemSwVersion)(char*, size_t) = NULL;
static int (*g_sceKernelGetProductCode)(char*, size_t) = NULL;
static int (*g_sceKernelGetProductStr)(char*, size_t) = NULL;
static int (*g_sceKernelIccGetHwInfo)(void*, size_t) = NULL;
static int (*g_sceKernelIccGetThermalAlert)(int*) = NULL;
static int (*g_sceKernelIccGetPowerOperatingTime)(uint64_t*) = NULL;
static int (*g_sceKernelIccGetPowerNumberOfBootShutdown)(uint32_t*, uint32_t*) = NULL;
static int (*g_sceKernelHwGetBdDriveInfo)(void*, size_t) = NULL;
static int (*g_sceKernelIccGetBDPowerState)(int*) = NULL;
static int (*g_sceKernelGetModuleList)(uint32_t*, size_t, size_t*) = NULL;
static int (*g_sceKernelGetModuleInfo)(uint32_t, void*, size_t) = NULL;
static int (*g_get_module_info_list)(int, void*, size_t*) = NULL;
static int (*g_sceKernelLoadStartModule)(const char*, size_t, const void*, uint32_t, void*, int*) = NULL;
static int g_extendedinfo_resolved = 0;
static pthread_mutex_t g_extendedinfo_lock = PTHREAD_MUTEX_INITIALIZER;

// ============================================================================
// Kernel-RPC serialization: kernel_dynlib_* talks to the kernel over a shared
// RPC pipe pair. Two threads interleaving requests on it corrupt the stream
// and the payload wedges. EVERY dynlib call goes through these wrappers.
// ============================================================================
static pthread_mutex_t g_krpc_lock = PTHREAD_MUTEX_INITIALIZER;

static int krpc_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle) {
    pthread_mutex_lock(&g_krpc_lock);
    int r = kernel_dynlib_handle(pid, basename, handle);
    pthread_mutex_unlock(&g_krpc_lock);
    return r;
}

static intptr_t krpc_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid) {
    pthread_mutex_lock(&g_krpc_lock);
    intptr_t p = kernel_dynlib_resolve(pid, handle, nid);
    pthread_mutex_unlock(&g_krpc_lock);
    return p;
}

static intptr_t krpc_dynlib_dlsym(pid_t pid, uint32_t handle, const char *sym) {
    pthread_mutex_lock(&g_krpc_lock);
    intptr_t p = kernel_dynlib_dlsym(pid, handle, sym);
    pthread_mutex_unlock(&g_krpc_lock);
    return p;
}

// A dynlib resolve returns a PS5 kernel error code (0x8xxxxxxx, ~2GB) instead
// of NULL when the NID isn't exported. Storing that into a function pointer
// makes `if (fp)` pass, and calling it SIGSEGVs the WHOLE payload process —
// even inside a watchdog thread. Validate against the module's own mapbase:
// a real symbol must lie inside the resolved module's mapped image.
static void *krpc_resolve_sym(pid_t pid, uint32_t handle, const char *nid) {
    intptr_t p = krpc_dynlib_resolve(pid, handle, nid);
    if (p <= 0) return NULL;

    pthread_mutex_lock(&g_krpc_lock);
    intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
    pthread_mutex_unlock(&g_krpc_lock);
    if (base <= 0) {
        // No mapbase available — fall back to rejecting kernel-errno-shaped
        // values and anything outside plausible user VA space.
        if (p < 0x100000000 || p > 0x7FFFFFFFFFFF) return NULL;
        return (void *)p;
    }
    // Modules are < 32 MB; reject anything not inside [base, base+32MB).
    if (p < base || p >= base + 0x2000000) return NULL;
    return (void *)p;
}

// Same validation for a resolve/dlsym result we already hold: reject the
// kernel-errno-shaped values (0x8xxxxxxx, ~2GB) that pass naive range checks
// and SIGSEGV the whole payload when called. Returns the usable pointer or 0.
static intptr_t krpc_ptr_ok(pid_t pid, uint32_t handle, intptr_t p) {
    if (p <= 0) return 0;
    pthread_mutex_lock(&g_krpc_lock);
    intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
    pthread_mutex_unlock(&g_krpc_lock);
    if (base > 0)
        return (p >= base && p < base + 0x2000000) ? p : 0;
    return (p >= 0x100000000 && p <= 0x7FFFFFFFFFFF) ? p : 0;
}

// dlsym-by-name with the same kernel-errno protection as krpc_resolve_sym.
static void *krpc_dlsym_checked(pid_t pid, uint32_t handle, const char *sym) {
    return (void *)(uintptr_t)krpc_ptr_ok(pid, handle,
        krpc_dynlib_dlsym(pid, handle, sym));
}

// Find the handle of the kernel module loaded in THIS process.
// libkernel is typically handle 0x2001 (websrv pt_resolve uses 0x1/0x2001).
// Verification: resolve sceKernelGetCpuTemperature's NID and compare with the
// address our extern import already resolved to — a match proves the handle.
// Keeps the probe list TINY: every resolve is a kernel-RPC round-trip, so a
// big scan here freezes the payload for seconds.
static uint32_t find_kernel_module_handle(void) {
    pid_t pid = getpid();
    intptr_t anchor = (intptr_t)&sceKernelGetCpuTemperature;

    static const uint32_t known[] = { 0x2001, 0x1, 0x2, 0x0 };
    for (unsigned i = 0; i < sizeof(known)/sizeof(known[0]); i++) {
        if (krpc_dynlib_resolve(pid, known[i], NID_KERNEL_GETCPUTEMPERATURE) == anchor)
            return known[i];
    }

    static const char *names[] = {
        "libkernel_web.sprx", "libkernel_web", "libkernel.sprx",
        "libkernel", "libkernel_sys.sprx", "libkernel_sys", NULL
    };
    uint32_t first_ok = 0;
    for (int i = 0; names[i]; i++) {
        uint32_t h = 0;
        if (krpc_dynlib_handle(pid, names[i], &h) != 0 || h == 0)
            continue;
        if (!first_ok) first_ok = h;
        if (krpc_dynlib_resolve(pid, h, NID_KERNEL_GETCPUTEMPERATURE) == anchor)
            return h;
    }
    return first_ok;
}

static void resolve_extendedinfo_functions(void);
void send_progress_message(const char *msg);

// ----------------------------------------------------------------------------
// Watchdog crash containment for DIRECT function pointers.
// A resolved pointer with the wrong ABI can SEGV inside a worker thread — a
// fatal signal would kill the WHOLE process, so every risky worker arms a
// siglongjmp landing pad and the handler bounces faulting threads back
// (marked crashed) instead of dying.
// ----------------------------------------------------------------------------
static __thread sigjmp_buf t_wdg_jmp;
static __thread volatile int t_wdg_armed = 0;

static void wdg_fault_handler(int sig, siginfo_t *info, void *uap) {
    (void)info; (void)uap;
    if (t_wdg_armed) {
        t_wdg_armed = 0;
        siglongjmp(t_wdg_jmp, sig);
    }
    // Not a watchdog worker — restore default behavior and re-raise.
    signal(sig, SIG_DFL);
    raise(sig);
}

static void wdg_install_handler(void) {
    static volatile int installed = 0;
    if (__atomic_exchange_n(&installed, 1, __ATOMIC_SEQ_CST)) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = wdg_fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
}

#define WDG_RESULT_CRASHED (-3)


// Helper: find an ALREADY-loaded module handle. Never loads anything —
// sceKernelLoadStartModule wedges the whole process in kernel IPC, so modules
// we need are statically linked (loader resolves them before main).
static uint32_t get_module_handle(const char *basename, const char *filename) {
    (void)filename;
    uint32_t handle = 0;
    if (krpc_dynlib_handle(getpid(), basename, &handle) == 0 && handle != 0) {
        return handle;
    }
    return 0;
}

// Resolve the extended kernel info functions from the loaded kernel module.
// Must run BEFORE any Sce-module load attempt, because get_module_handle
// relies on g_sceKernelLoadStartModule resolved here.
static void resolve_extendedinfo_functions(void) {
    if (g_extendedinfo_resolved) return;
    pthread_mutex_lock(&g_extendedinfo_lock);
    if (g_extendedinfo_resolved) {
        pthread_mutex_unlock(&g_extendedinfo_lock);
        return;
    }

    uint32_t h = find_kernel_module_handle();
    if (h != 0) {
        g_sceKernelLoadStartModule = (int (*)(const char*, size_t, const void*, uint32_t, void*, int*))
            (uintptr_t)krpc_resolve_sym(getpid(), h, NID_KERNEL_LOADSTARTMODULE);
        // Helper macro isn't available yet; resolve each symbol below.
        // Try NID first, then dlsym by name (some modules export plain names),
        // across both the found kernel module and libkernel_sys.
#define XRES(field, type, nid, sym) \
        do { \
            field = (type)(uintptr_t)krpc_resolve_sym(getpid(), h, nid); \
            uint32_t hs = 0; \
            int have_sys = (krpc_dynlib_handle(getpid(), "libkernel_sys.sprx", &hs) == 0 && hs); \
            if (!field && have_sys) \
                field = (type)(uintptr_t)krpc_resolve_sym(getpid(), hs, nid); \
            if (!field) { \
                intptr_t p = krpc_dynlib_dlsym(getpid(), h, sym); \
                uint32_t hh = h; \
                if (p <= 0 && have_sys) { p = krpc_dynlib_dlsym(getpid(), hs, sym); hh = hs; } \
                if ((p = krpc_ptr_ok(getpid(), hh, p))) \
                    field = (type)(uintptr_t)p; \
            } \
        } while (0)
        XRES(g_sceKernelGetCpuUsageAll, int (*)(int*, int), NID_KERNEL_GETCPUUSAGEALL, "sceKernelGetCpuUsageAll");
        XRES(g_sceKernelGetCpuUsage, int (*)(int, int*), NID_KERNEL_GETCPUUSAGE, "sceKernelGetCpuUsage");
        // Compute NIDs at runtime via nid_encode() for symbols whose NID
        // isn't in our table (nid_encode is linked into every payload).
        {
            char nbuf[12];
            nid_encode("sceKernelGetThreadName", nbuf);
            XRES(g_sceKernelGetThreadName, int (*)(uint32_t, char*), nbuf, "sceKernelGetThreadName");
            nid_encode("sceKernelGetCpuUsage", nbuf);
            XRES(g_sceKernelGetCpuUsageList, int (*)(void*, int32_t*), nbuf, "sceKernelGetCpuUsage");
            nid_encode("sceKernelGetDirectMemorySize", nbuf);
            XRES(g_sceKernelGetDirectMemorySizeReal, uint64_t (*)(void), nbuf, "sceKernelGetDirectMemorySize");
            // Memory APIs: libkernel_web versions are stubs that return
            // 0/ENOMEM for our process type — prefer libkernel_sys.
            uint32_t hs2 = 0;
            if (krpc_dynlib_handle(getpid(), "libkernel_sys.sprx", &hs2) == 0 && hs2) {
                nid_encode("sceKernelGetDirectMemorySize", nbuf);
                void *p = krpc_resolve_sym(getpid(), hs2, nbuf);
                if (p) g_sceKernelGetDirectMemorySizeReal = (uint64_t (*)(void))p;
                nid_encode("sceKernelAvailableDirectMemorySize", nbuf);
                p = krpc_resolve_sym(getpid(), hs2, nbuf);
                if (p) g_sceKernelAvailableDirectMemorySize = (size_t (*)(void))p;
                nid_encode("sceKernelAvailableFlexibleMemorySize", nbuf);
                p = krpc_resolve_sym(getpid(), hs2, nbuf);
                if (p) g_sceKernelAvailableFlexibleMemorySize = (size_t (*)(void))p;
                nid_encode("sceKernelGetModuleList", nbuf);
                p = krpc_resolve_sym(getpid(), hs2, nbuf);
                if (p) g_sceKernelGetModuleList = (int (*)(uint32_t*, size_t, size_t*))p;
                nid_encode("sceKernelGetModuleInfo", nbuf);
                p = krpc_resolve_sym(getpid(), hs2, nbuf);
                if (p) g_sceKernelGetModuleInfo = (int (*)(uint32_t, void*, size_t))p;
                nid_encode("sceKernelGetPageTableStats", nbuf);
                p = krpc_resolve_sym(getpid(), hs2, nbuf);
                if (p) g_sceKernelGetPageTableStats = (int (*)(int, int, int*, int*))p;
                nid_encode("sceKernelConfiguredFlexibleMemorySize", nbuf);
                p = krpc_resolve_sym(getpid(), hs2, nbuf);
                if (p) g_sceKernelConfiguredFlexibleMemorySize = (uint64_t (*)(void))p;
                // get_page_table_stats / get_module_info_list are plain-name
                // exports (not NID)
                intptr_t gp = krpc_dynlib_dlsym(getpid(), hs2, "get_page_table_stats");
                if (gp > 0) {
                    intptr_t mb = kernel_dynlib_mapbase_addr(getpid(), hs2);
                    if (mb > 0 && gp >= mb && gp < mb + 0x2000000)
                        g_sceKernelGetPageTableStats = (int (*)(int,int,int*,int*))gp;
                }
                gp = krpc_dynlib_dlsym(getpid(), hs2, "get_module_info_list");
                if (gp > 0) {
                    intptr_t mb = kernel_dynlib_mapbase_addr(getpid(), hs2);
                    if (mb > 0 && gp >= mb && gp < mb + 0x2000000)
                        g_get_module_info_list = (int (*)(int, void*, size_t*))gp;
                }
            }
        }
        XRES(g_sceKernelAvailableDirectMemorySize, size_t (*)(void), NID_KERNEL_AVAILABLEDIRECTMEMORYSIZE, "sceKernelAvailableDirectMemorySize");
        XRES(g_sceKernelAvailableFlexibleMemorySize, size_t (*)(void), NID_KERNEL_AVAILABLEFLEXIBLEMEMORYSIZE, "sceKernelAvailableFlexibleMemorySize");
        XRES(g_sceKernelGetSystemSwVersion, int (*)(char*, size_t), NID_KERNEL_GETSYSTEMSWVERSION, "sceKernelGetSystemSwVersion");
        XRES(g_sceKernelGetProductCode, int (*)(char*, size_t), NID_KERNEL_GETPRODUCTCODE, "sceKernelGetProductCode");
        XRES(g_sceKernelGetProductStr, int (*)(char*, size_t), NID_KERNEL_GETPRODUCTSTR, "sceKernelGetProductStr");
        XRES(g_sceKernelIccGetHwInfo, int (*)(void*, size_t), NID_KERNEL_ICCGETHWINFO, "sceKernelIccGetHwInfo");
        XRES(g_sceKernelIccGetThermalAlert, int (*)(int*), NID_KERNEL_ICCGETTHERMALALERT, "sceKernelIccGetThermalAlert");
        XRES(g_sceKernelIccGetPowerOperatingTime, int (*)(uint64_t*), NID_KERNEL_ICCGETPOWEROPERATINGTIME, "sceKernelIccGetPowerOperatingTime");
        XRES(g_sceKernelIccGetPowerNumberOfBootShutdown, int (*)(uint32_t*, uint32_t*), NID_KERNEL_ICCGETPOWERNUMBEROFBOOTSHUTDOWN, "sceKernelIccGetPowerNumberOfBootShutdown");
        XRES(g_sceKernelHwGetBdDriveInfo, int (*)(void*, size_t), NID_KERNEL_HWGETBDDRIVEINFO, "sceKernelHwGetBdDriveInfo");
        XRES(g_sceKernelIccGetBDPowerState, int (*)(int*), NID_KERNEL_ICCGETBDPOWERSTATE, "sceKernelIccGetBDPowerState");
        XRES(g_sceKernelGetModuleList, int (*)(uint32_t*, size_t, size_t*), NID_KERNEL_GETMODULELIST, "sceKernelGetModuleList");
        XRES(g_sceKernelGetModuleInfo, int (*)(uint32_t, void*, size_t), NID_KERNEL_GETMODULEINFO, "sceKernelGetModuleInfo");
#undef XRES
    }

    g_extendedinfo_resolved = 1;
    pthread_mutex_unlock(&g_extendedinfo_lock);
}

// ============================================================================
// Service functions are statically linked — no runtime module loading at all.
// ============================================================================
#define WDG_TIMEOUT_MS 10000
static int wdg_fn_call(void *fn, void *a, void *b, void *c, void *d, int timeout_ms);
static int read_sfo_string(const char* path, const char* want_key, char* out, size_t size);
static int appdb_direct_register(const char *title_id, const char *game_path);
static int appdb_direct_unregister(const char *title_id);
static int extract_json_string(const char* json, const char* key, char* out, size_t out_size);
static int extract_json_int(const char* json, const char* key, unsigned long long* out);
static int extract_json_object(const char* json, const char* key, char* out, size_t out_size);
static int get_game_name_from_json(const char* json_path, char* name, size_t size);

// Protocol responses (defined early — the PKG-install/fan handlers below
// must frame their replies; a bare send() desyncs the client for 120s)
#define RESP_OK 0x01
#define RESP_ERROR 0x02
#define RESP_DATA 0x03
#define RESP_READY 0x04
#define RESP_PROGRESS 0x05
void send_response(int sock, uint8_t response, const void *data, uint32_t data_len);
static ssize_t send_all(int sock, const void *buf, size_t len);
static void progress_socket_set(int sock);
static void progress_socket_clear(int sock);

// Resolve KernelSys functions (hardware info)
static void resolve_kernelsys_functions(void) {
    if (g_kernelsys_resolved) return;
    pthread_mutex_lock(&g_kernelsys_lock);
    if (g_kernelsys_resolved) {
        pthread_mutex_unlock(&g_kernelsys_lock);
        return;
    }
    
    uint32_t handle = get_module_handle("libkernel_sys.sprx", "libkernel_sys.sprx");
    if (handle != 0) {
        g_sceKernelGetHwModelName = (int (*)(char*))
            (uintptr_t)krpc_resolve_sym(getpid(), handle, NID_KERNEL_GETHWMODELNAME);
        g_sceKernelGetHwSerialNumber = (int (*)(char*))
            (uintptr_t)krpc_resolve_sym(getpid(), handle, NID_KERNEL_GETHWSERIALNUMBER);
    }
    
    g_kernelsys_resolved = 1;
    pthread_mutex_unlock(&g_kernelsys_lock);
}

// ============================================================================
// AppInstUtil Resolution (dump_installer method by EchoStretch)
// Uses kernel_dynlib_handle + kernel_dynlib_resolve with NIDs instead of
// static linking, which is more reliable across different loaders.
// ============================================================================
static void resolve_appinstutil(void) {
    pthread_mutex_lock(&g_appinstutil_lock);
    if (g_appinstutil_resolved) {
        pthread_mutex_unlock(&g_appinstutil_lock);
        return;
    }
    
    uint32_t handle = 0;
    if (!krpc_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
        g_sceAppInstUtilAppInstallTitleDir =
            (int (*)(const char*, const char*, void*))
            krpc_resolve_sym(-1, handle, NID_APPINSTUTIL_INSTALLTITLEDIR);
    }
    
    g_appinstutil_resolved = 1;
    pthread_mutex_unlock(&g_appinstutil_lock);
}

// ============================================================================
// PKG Install Resolution (ps5upload/etaHEN DPI style)
// Resolves sceAppInstUtilInstallByPackage and GetInstallStatus via kernel_dynlib
// ============================================================================
// NIDs for PKG install functions
#define NID_APPINSTUTIL_INSTALLBYPACKAGE "sceAppInstUtilInstallByPackage"
#define NID_APPINSTUTIL_GETINSTALLSTATUS "sceAppInstUtilGetInstallStatus"

static void resolve_pkginstutil(void) {
    pthread_mutex_lock(&g_pkginstutil_lock);
    if (g_pkginstutil_resolved) {
        pthread_mutex_unlock(&g_pkginstutil_lock);
        return;
    }
    
    // Try to resolve via dlsym first (simpler, works on most FW)
    void *handle = dlopen("/system/common/lib/libSceAppInstUtil.sprx", RTLD_LAZY);
    if (handle) {
        g_sceAppInstUtilInstallByPackage = 
            (int (*)(pkg_meta_info_t*, pkg_install_info_t*, pkg_playgo_info_t*))
            dlsym(handle, "sceAppInstUtilInstallByPackage");
        g_sceAppInstUtilGetInstallStatus =
            (int (*)(const char*, void*))
            dlsym(handle, "sceAppInstUtilGetInstallStatus");
        g_sceAppInstUtilGetContentIdFromPkg =
            (int (*)(const char*, char*, bool*))
            dlsym(handle, "sceAppInstUtilGetContentIdFromPkg");
    }
    
    g_pkginstutil_resolved = 1;
    pthread_mutex_unlock(&g_pkginstutil_lock);
}

// ============================================================================
// Localhost PKG responder
// FW>=11 consoles refuse packages not served by the console itself — the
// installer rejects local paths AND remote http URLs with 0x80B2116F.
// Serving the same bytes via http://127.0.0.1 makes the daemon accept them.
// This tiny responder supports HEAD + Range GETs (the daemon fetches the
// pkg header first, then the FIH footer, then ranged chunks).
// ============================================================================
#define PKG_HTTP_PORT 13801
static char g_pkg_serve_path[2048] = {0};      // file served at /local
static char g_pkg_serve_url[1024] = {0};       // remote URL proxied at /remote
static volatile int g_pkg_http_running = 0;
static volatile int g_pkg_http_port = 0;    // actual bound port (falls back if 13801 is taken)
static pthread_mutex_t g_pkg_http_lock = PTHREAD_MUTEX_INITIALIZER;

// Relay one remote-URL range request: open socket, forward the Range, stream
// the body back. Connection: close so we stop at EOF (no chunked parsing).
static void pkg_http_log(const char *fmt, ...);

// ---- Persistent upstream connection ---------------------------------------
// The installer daemon fetches the PKG as hundreds of sequential ~18MB range
// requests. Opening a fresh TCP connection per request pays a handshake +
// slow-start penalty each time. Keep the upstream socket open across
// requests (the PC server now supports keep-alive) and reuse it.
static int  g_pkg_up_sock = -1;
static char g_pkg_up_key[64] = {0};   // "host:port" the socket is connected to

static int pkg_up_connect(const char *host, int port) {
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) return -1;
    int rs = socket(AF_INET, SOCK_STREAM, 0);
    if (rs < 0) return -1;
    int np = 1; setsockopt(rs, SOL_SOCKET, SO_NOSIGPIPE, &np, sizeof(np));
    struct timeval ctv = {10, 0};
    setsockopt(rs, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
    setsockopt(rs, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
    int sz = 1024 * 1024;
    setsockopt(rs, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    setsockopt(rs, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
#ifdef TCP_NODELAY
    setsockopt(rs, IPPROTO_TCP, TCP_NODELAY, &np, sizeof(np));
#endif
    if (connect(rs, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(rs); return -1; }
    return rs;
}

static int pkg_proxy_remote(int csock, uint64_t rstart, uint64_t rend, int has_end) {
    char host[256]; int port = 80; char rpath[768];
    const char *u = g_pkg_serve_url;
    if (strncmp(u, "http://", 7) != 0) return -1;
    const char *hp = u + 7;
    // Tolerate IPv4-mapped IPv6 hosts ("::ffff:1.2.3.4") — strip the prefix so
    // the port colon is the only ':' before the path.
    if (strncmp(hp, "::ffff:", 7) == 0) hp += 7;
    const char *slash = strchr(hp, '/');
    if (!slash) return -1;
    int hlen = (int)(slash - hp);
    if (hlen <= 0 || hlen >= (int)sizeof(host)) return -1;
    const char *colon = memchr(hp, ':', hlen);
    if (colon) {
        port = atoi(colon + 1);
        hlen = (int)(colon - hp);
    }
    memcpy(host, hp, hlen); host[hlen] = 0;
    snprintf(rpath, sizeof(rpath), "%s", slash);

    char key[64]; snprintf(key, sizeof(key), "%s:%d", host, port);
    char req[1024];
    int qlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nRange: bytes=%llu-%llu\r\nConnection: keep-alive\r\n\r\n",
        rpath, host, (unsigned long long)rstart,
        (unsigned long long)(has_end ? rend : 0xFFFFFFFFFFFFFFFFULL));

    // Try to reuse the persistent upstream socket; fall back to a fresh
    // connect on any failure (once).
    int rs = -1, reused = 0;
    if (g_pkg_up_sock >= 0) {
        if (strcmp(g_pkg_up_key, key) == 0) { rs = g_pkg_up_sock; reused = 1; }
        else { close(g_pkg_up_sock); g_pkg_up_sock = -1; }
    }

    char *buf = malloc(512 * 1024);
    if (!buf) return -1;

    int status = 0; uint64_t remaining = 0; int body_bytes = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (rs < 0) {
            rs = pkg_up_connect(host, port);
            if (rs < 0) {
                if (attempt == 0) continue;
                pkg_http_log("  PROXY connect FAILED to %s:%d\n", host, port);
                free(buf); return -1;
            }
            pkg_http_log("  PROXY connect %s %s:%d\n", attempt ? "retry" : "OK", host, port);
        }
        if (send(rs, req, qlen, 0) != qlen) { close(rs); rs = -1; continue; }

        // Read upstream response headers.
        char hdr[4096]; int hn = 0;
        while (hn < (int)sizeof(hdr) - 1) {
            ssize_t n = recv(rs, hdr + hn, sizeof(hdr) - 1 - hn, 0);
            if (n <= 0) break;
            hn += n; hdr[hn] = 0;
            if (strstr(hdr, "\r\n\r\n")) break;
        }
        char *body = strstr(hdr, "\r\n\r\n");
        if (!body) { close(rs); rs = -1; continue; }   // dead reused socket → retry once
        body += 4;
        sscanf(hdr, "HTTP/%*s %d", &status);
        body_bytes = hn - (int)(body - hdr);

        // Content-Length tells us exactly when this response ends — required
        // to keep the socket reusable.
        remaining = UINT64_MAX;
        char *cl = strcasestr(hdr, "Content-Length:");
        if (cl) remaining = strtoull(cl + 15, NULL, 10);

        // Forward a minimal 200/206 response (daemon only cares about bytes).
        char *cr = strcasestr(hdr, "Content-Range:");
        char oh[512]; int on = snprintf(oh, sizeof(oh),
            "HTTP/1.1 %d OK\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n",
            status == 0 ? 206 : status);
        if (cl) { char v[64]; sscanf(cl + 15, " %63[^\r]", v); on += snprintf(oh + on, sizeof(oh) - on, "Content-Length: %s\r\n", v); }
        if (cr) { char v[96]; sscanf(cr + 14, " %95[^\r]", v); on += snprintf(oh + on, sizeof(oh) - on, "Content-Range: %s\r\n", v); }
        on += snprintf(oh + on, sizeof(oh) - on, "Connection: close\r\n\r\n");
        if (send_all(csock, oh, on) < 0) { close(rs); free(buf); g_pkg_up_sock = -1; return -1; }
        if (body_bytes > 0) {
            if (send_all(csock, body, body_bytes) < 0) { close(rs); free(buf); g_pkg_up_sock = -1; return -1; }
            if (remaining != UINT64_MAX) remaining = remaining > (uint64_t)body_bytes ? remaining - body_bytes : 0;
        }
        break;
    }
    if (rs < 0) { free(buf); return -1; }

    // Relay the body. Stop after Content-Length bytes so the socket stays
    // clean for reuse; if the peer closes early the socket is dead anyway.
    int ok = 1;
    while (remaining > 0) {
        size_t want = remaining > sizeof(buf[0]) * (512 * 1024) ? 512 * 1024 : (size_t)remaining;
        ssize_t n = recv(rs, buf, want, 0);
        if (n <= 0) { ok = 0; break; }
        if (send_all(csock, buf, n) < 0) { ok = 0; break; }
        if (remaining != UINT64_MAX) remaining -= (uint64_t)n;
        else if (n == 0) break;
    }
    free(buf);

    if (ok) {
        g_pkg_up_sock = rs;
        snprintf(g_pkg_up_key, sizeof(g_pkg_up_key), "%s", key);
        if (!reused) pkg_http_log("  PROXY upstream kept-alive\n");
    } else {
        close(rs); g_pkg_up_sock = -1;
        pkg_http_log("  PROXY upstream dropped mid-body\n");
    }
    return 0;
}

// Debug log for daemon requests — lets us see exactly which ranges the
// installer fetches and where the transfer stops.
static void pkg_http_log(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int fd = open("/data/pkg_http.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { write(fd, buf, strlen(buf)); close(fd); }
}

static void pkg_serve_one(int csock) {
    char req[2048]; int rn = 0;
    struct timeval tv = {15, 0};
    setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (rn < (int)sizeof(req) - 1) {
        ssize_t n = recv(csock, req + rn, sizeof(req) - 1 - rn, 0);
        if (n <= 0) { close(csock); return; }
        rn += n; req[rn] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    char method[8] = {0}, upath[256] = {0};
    sscanf(req, "%7s %255s", method, upath);

    uint64_t rstart = 0, rend = 0; int has_range = 0, has_end = 0;
    char *rl = strcasestr(req, "Range:");
    if (rl) {
        const char *p = strstr(rl, "bytes=");
        if (p) {
            has_range = 1;
            p += 6;
            rstart = strtoull(p, (char **)&p, 10);
            if (*p == '-') {
                p++;
                if (*p >= '0' && *p <= '9') { rend = strtoull(p, NULL, 10); has_end = 1; }
            }
        }
    }

    int is_head = (strcmp(method, "HEAD") == 0);
    int is_remote = (strncmp(upath, "/remote", 7) == 0);

    pkg_http_log("%s %s range=%d %llu-%llu port=%d serve_url=%s\n", method, upath, has_range,
                 (unsigned long long)rstart, (unsigned long long)rend,
                 g_pkg_http_port, is_remote ? g_pkg_serve_url : "-");

    if (is_remote) {
        if (!g_pkg_serve_url[0]) { send_all(csock, "HTTP/1.1 404\r\n\r\n", 15); close(csock); return; }
        pkg_proxy_remote(csock, rstart, rend, has_range ? has_end : 0);
        close(csock); return;
    }

    // /local → serve g_pkg_serve_path
    int fd = open(g_pkg_serve_path, O_RDONLY);
    if (fd < 0) { send_all(csock, "HTTP/1.1 404\r\n\r\n", 15); close(csock); return; }
    struct stat st; fstat(fd, &st);
    uint64_t fsize = (uint64_t)st.st_size;
    uint64_t start = has_range ? rstart : 0;
    uint64_t end = has_range ? (has_end ? rend : fsize - 1) : fsize - 1;
    if (end >= fsize) end = fsize - 1;
    uint64_t len = end - start + 1;

    char oh[512];
    int on = snprintf(oh, sizeof(oh),
        "HTTP/1.1 %s\r\nContent-Type: application/octet-stream\r\n"
        "Accept-Ranges: bytes\r\nContent-Length: %llu\r\n",
        has_range ? "206 Partial Content" : "200 OK",
        (unsigned long long)len);
    if (has_range)
        on += snprintf(oh + on, sizeof(oh) - on, "Content-Range: bytes %llu-%llu/%llu\r\n",
                       (unsigned long long)start, (unsigned long long)end, (unsigned long long)fsize);
    on += snprintf(oh + on, sizeof(oh) - on, "Connection: close\r\n\r\n");
    send_all(csock, oh, on);

    if (!is_head) {
        static uint8_t sbuf[256 * 1024];
        uint64_t off = start, rem = len;
        while (rem > 0) {
            ssize_t r = pread(fd, sbuf, rem > sizeof(sbuf) ? sizeof(sbuf) : rem, off);
            if (r <= 0) break;
            if (send_all(csock, sbuf, r) < 0) {
                pkg_http_log("  SEND FAILED at %llu/%llu\n",
                             (unsigned long long)(off - start), (unsigned long long)len);
                break;
            }
            off += r; rem -= r;
        }
    }
    close(fd); close(csock);
}

static void *pkg_http_thread(void *arg) {
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1, np = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(ls, SOL_SOCKET, SO_NOSIGPIPE, &np, sizeof(np));
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(0x7F000001); // 127.0.0.1 only — never on LAN
    // A stale payload instance may already own PKG_HTTP_PORT — walk forward
    // until we find a free one; the bound port is published in g_pkg_http_port.
    int bound = 0;
    for (int p = PKG_HTTP_PORT; p < PKG_HTTP_PORT + 10; p++) {
        sa.sin_port = htons(p);
        if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0) { bound = p; break; }
    }
    if (!bound || listen(ls, 8) != 0) {
        close(ls); g_pkg_http_running = 0; return NULL;
    }
    g_pkg_http_port = bound;
    pkg_http_log("PKG responder bound port=%d pid=%d\n", bound, (int)getpid());
    while (g_pkg_http_running) {
        int cs = accept(ls, NULL, NULL);
        if (cs < 0) { usleep(50000); continue; }
        setsockopt(cs, SOL_SOCKET, SO_NOSIGPIPE, &np, sizeof(np));
        pkg_serve_one(cs);
    }
    close(ls);
    return NULL;
}

// Ensure the responder is up and pointing at the right source.
static int pkg_http_point_at(const char *local_path, const char *remote_url) {
    pthread_mutex_lock(&g_pkg_http_lock);
    snprintf(g_pkg_serve_path, sizeof(g_pkg_serve_path), "%s", local_path ? local_path : "");
    snprintf(g_pkg_serve_url, sizeof(g_pkg_serve_url), "%s", remote_url ? remote_url : "");
    if (!g_pkg_http_running) {
        g_pkg_http_running = 1;
        pthread_t t; pthread_attr_t a;
        pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, pkg_http_thread, NULL) != 0) g_pkg_http_running = 0;
        pthread_attr_destroy(&a);
    }
    pthread_mutex_unlock(&g_pkg_http_lock);
    usleep(150000); // give the listener a beat to bind
    return g_pkg_http_running ? 0 : -1;
}

// ============================================================================
// PKG Install — forked-child isolation
// Under loaders without etaHEN (e.g. kstuff-light) the AppInstUtil daemon IPC
// can wedge in UNINTERRUPTIBLE kernel sleep: no signal lands, the lock it
// holds freezes the whole process, SIGKILL stays pending, the port stays
// bound — only a PS5 reboot recovers. Threads cannot contain that; a separate
// PROCESS can. So the entire daemon sequence (init → probe → install →
// status polling) runs in a forked child that reports progress lines over a
// pipe. If it wedges we leak one small process and report a clean timeout —
// the server and every other session stay alive.
// ============================================================================
typedef struct {
    int      active;                       // an install attempt is in flight/queued
    int      done;                         // terminal state reached
    unsigned error_code;                   // 0 = no error
    int      pct;                          // last known percent
    char     text[160];                    // human-readable phase
    char     content_id[PKG_CONTENTID_SIZE];
} pkg_state_t;
static pkg_state_t g_pkg_state = {0};
static pthread_mutex_t g_pkg_state_lock = PTHREAD_MUTEX_INITIALIZER;

// Get PKG install status — etaHEN's real struct: the API is 2-arg
// (content_id, SceAppInstallStatusInstalled*) and status is a STRING
// ("downloading", "promoting", "playable", "error"...), not an int.
typedef struct {
    int32_t error_code;
    int32_t version;
    char    description[512];
    char    type[9];
} PkgInstallErrorInfo;

typedef struct {
    char     status[16];
    char     src_type[8];
    uint32_t remain_time;
    uint64_t downloaded_size;
    uint64_t initial_chunk_size;
    uint64_t total_size;
    uint32_t promote_progress;
    PkgInstallErrorInfo error_info;
    int32_t  local_copy_percent;
    uint8_t  is_copy_only;
} PkgInstallStatusInstalled;

static void pkg_set_text(const char *fmt, ...) {
    char buf[160];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_pkg_state_lock);
    snprintf(g_pkg_state.text, sizeof(g_pkg_state.text), "%s", buf);
    pthread_mutex_unlock(&g_pkg_state_lock);
}

static void pkg_finish(int ok, unsigned err, const char *cid) {
    pthread_mutex_lock(&g_pkg_state_lock);
    g_pkg_state.done = 1;
    g_pkg_state.error_code = err;
    if (cid) snprintf(g_pkg_state.content_id, sizeof(g_pkg_state.content_id), "%s", cid);
    if (!ok) g_pkg_state.active = 0;      // terminal error — nothing queued
    pthread_mutex_unlock(&g_pkg_state_lock);
}

// ---- daemon availability gate --------------------------------------------
// Observed on hardware: under kstuff-light, ANY AppInstUtil daemon call
// (Initialize/InstallByPackage/GetInstallStatus/AppUnInstall) wedges the
// whole process in uninterruptible kernel sleep — SIGKILL can't even land.
// Under etaHEN the same calls work because its daemon initializes/holds the
// service context. etaHEN's daemon always listens on these unix sockets
// while alive, so their presence is the gate for EVERY daemon call.
static int etahen_present(void) {
    if (access("/system_tmp/etaHEN_crit_service", F_OK) == 0 ||
        access("/system_tmp/etaHEN_util_service", F_OK) == 0)
        return 1;
    return 0;
}

// Worker: the whole install sequence (init → probe → install → status poll)
// lives on this detached thread. Every daemon call goes through wdg_fn_call,
// which contains crashes, and all IPC-bound buffers are static so a
// watchdog-parked worker can never write into dead stack memory.
static void *pkg_install_worker(void *arg) {
    char *pkg_path = (char *)arg;
    resolve_pkginstutil();

    if (!g_sceAppInstUtilInstallByPackage || !g_sceAppInstUtilGetInstallStatus) {
        pkg_set_text("AppInstUtil not available");
        pkg_finish(0, 0xFFFFFFFF, NULL);
        free(pkg_path);
        return NULL;
    }
    if (!etahen_present()) {
        pkg_set_text("PKG install needs etaHEN running");
        pkg_finish(0, 0x80EE0005, NULL);
        free(pkg_path);
        return NULL;
    }

    pkg_set_text("initializing installer");
    int init_rc = wdg_fn_call((void*)sceAppInstUtilInitialize, NULL, NULL, NULL, NULL, 30000);
    if (init_rc != 0 && init_rc != 0x80990001) {
        if (init_rc == -2)
            pkg_set_text("daemon init timed out — run under etaHEN");
        pkg_finish(0, init_rc == -2 ? 0x80EE0006 : (unsigned)init_rc, NULL);
        free(pkg_path);
        return NULL;
    }

    // Static storage for everything handed to a guarded daemon call: if the
    // IPC wedges past the watchdog timeout, the parked worker keeps writing
    // to these — static memory stays valid forever, stack memory would not.
    static pkg_meta_info_t meta;
    static pkg_install_info_t pkg_info;
    static pkg_playgo_info_t playgo;
    static char probe_cid[PKG_CONTENTID_SIZE];
    static bool probe_is_app;
    static char file_uri[PATH_MAX + 8];
    memset(&meta, 0, sizeof(meta));
    memset(&pkg_info, 0, sizeof(pkg_info));
    memset(&playgo, 0, sizeof(playgo));

    // Pre-flight: does Sony's parser accept this file at all?
    if (g_sceAppInstUtilGetContentIdFromPkg && pkg_path[0] == '/') {
        memset(probe_cid, 0, sizeof(probe_cid));
        probe_is_app = false;
        pkg_set_text("probing package");
        int prc = wdg_fn_call((void*)g_sceAppInstUtilGetContentIdFromPkg,
                              (void*)pkg_path, probe_cid, &probe_is_app, NULL, 30000);
        pkg_set_text("probe cid='%s' app=%d rc=0x%08X",
                     probe_cid, (int)probe_is_app, (unsigned)prc);
    }

    // Display name in the PS5 Downloads notification = the package's own
    // filename (basename, .pkg stripped) instead of a generic suite label.
    // URL sources carry %XX escapes and a query string — decode and trim.
    static char content_name[256];
    const char *pbase = strrchr(pkg_path, '/');
    pbase = pbase ? pbase + 1 : pkg_path;
    snprintf(content_name, sizeof(content_name), "%s", pbase);
    char *q = strchr(content_name, '?');
    if (q) *q = '\0';
    // %XX decode in place (path segments never use '+' for space)
    {
        char *r = content_name, *w = content_name;
        while (*r) {
            if (*r == '%' && r[1] && r[2]) {
                int hi = r[1], lo = r[2];
                int hv = hi >= '0' && hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10;
                int lv = lo >= '0' && lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
                if (hv >= 0 && hv < 16 && lv >= 0 && lv < 16) {
                    *w++ = (char)((hv << 4) | lv); r += 3; continue;
                }
            }
            *w++ = *r++;
        }
        *w = 0;
    }
    char *ext = strrchr(content_name, '.');
    if (ext && strcasecmp(ext, ".pkg") == 0) *ext = '\0';
    if (!content_name[0]) snprintf(content_name, sizeof(content_name), "PKG Install");

    meta.uri = pkg_path;
    meta.ex_uri = "";
    meta.playgo_scenario_id = "";
    meta.content_id = "";
    meta.content_name = content_name;
    meta.icon_url = "";

    pkg_set_text("queueing install");
    int rc = wdg_fn_call((void*)g_sceAppInstUtilInstallByPackage, &meta, &pkg_info, &playgo, NULL, 60000);
    unsigned first_rc = (unsigned)rc;

    // FW>=11 refusal handling: the console declines pkgs not served by the
    // console itself (0x80B2116F on bare paths/remote http, 0x80B21106 on
    // file://). Route the bytes through our 127.0.0.1 responder and retry —
    // the daemon accepts console-local URLs (ps5upload proven on 13.60).
    if (rc != 0) {
        int is_local = (pkg_path[0] == '/');
        int is_http = (strncmp(pkg_path, "http://", 7) == 0 || strncmp(pkg_path, "https://", 8) == 0);

        if (is_local || is_http) {
            pkg_set_text("refused 0x%08X — serving via localhost", first_rc);
            if (pkg_http_point_at(is_local ? pkg_path : NULL,
                                  is_http ? pkg_path : NULL) == 0 && g_pkg_http_port) {
                snprintf(file_uri, sizeof(file_uri), "http://127.0.0.1:%d/%s",
                         g_pkg_http_port, is_local ? "local" : "remote");
                meta.uri = file_uri;
                rc = wdg_fn_call((void*)g_sceAppInstUtilInstallByPackage, &meta, &pkg_info, &playgo, NULL, 60000);
            } else {
                pkg_set_text("localhost responder failed to start");
            }
        }
        if (rc != 0 && is_local) {
            snprintf(file_uri, sizeof(file_uri), "file://%s", pkg_path);
            meta.uri = file_uri;
            pkg_set_text("retrying via file:// URI");
            rc = wdg_fn_call((void*)g_sceAppInstUtilInstallByPackage, &meta, &pkg_info, &playgo, NULL, 60000);
        }
    }

    if (rc != 0) {
        pkg_set_text("install refused (first=0x%08X last=0x%08X)", first_rc, (unsigned)rc);
        pkg_finish(0, (unsigned)rc, NULL);
        free(pkg_path);
        return NULL;
    }

    pthread_mutex_lock(&g_pkg_state_lock);
    memcpy(g_pkg_state.content_id, pkg_info.content_id, PKG_CONTENTID_SIZE);
    memcpy(g_active_pkg_content_id, pkg_info.content_id, PKG_CONTENTID_SIZE);
    g_pkg_install_active = 1;
    snprintf(g_pkg_state.text, sizeof(g_pkg_state.text), "install task accepted");
    pthread_mutex_unlock(&g_pkg_state_lock);
    free(pkg_path);

    // Poll the daemon ourselves (still this worker — sessions never touch the
    // IPC) so CMD_PKG_INSTALL_STATUS stays an instant state read.
    static char cid_buf[PKG_CONTENTID_SIZE];
    static PkgInstallStatusInstalled st;
    memcpy(cid_buf, pkg_info.content_id, PKG_CONTENTID_SIZE);
    int dead_polls = 0;
    for (int i = 0; i < 1800; i++) {          // up to ~60 min of polling
        memset(&st, 0, sizeof(st));
        int src = wdg_fn_call((void*)g_sceAppInstUtilGetInstallStatus,
                              cid_buf, &st, NULL, NULL, 4000);
        if (src == 0) {
            dead_polls = 0;
            st.status[15] = 0;
            int pct = st.total_size > 0
                ? (int)((st.downloaded_size * 100) / st.total_size)
                : (int)st.promote_progress;
            if (strcmp(st.status, "playable") == 0) {
                pkg_finish(1, 0, cid_buf);
                return NULL;
            }
            if (st.error_info.error_code != 0 ||
                strstr(st.status, "error") || strstr(st.status, "fail") ||
                strcmp(st.status, "none") == 0) {
                unsigned ec = st.error_info.error_code
                              ? (unsigned)st.error_info.error_code : 0x80EE0001;
                st.error_info.description[sizeof(st.error_info.description) - 1] = 0;
                st.src_type[sizeof(st.src_type) - 1] = 0;
                pkg_set_text("%s|src=%s|%.80s", st.status, st.src_type,
                             st.error_info.description);
                pkg_finish(0, ec, NULL);
                return NULL;
            }
            pthread_mutex_lock(&g_pkg_state_lock);
            g_pkg_state.pct = pct;
            snprintf(g_pkg_state.text, sizeof(g_pkg_state.text), "%s", st.status);
            pthread_mutex_unlock(&g_pkg_state_lock);
        } else {
            // Timed-out calls mark the function dead — subsequent polls are
            // instant no-ops, so give up after a handful and keep the last
            // known phase text.
            if (++dead_polls >= 5) {
                pkg_set_text("status queries unreachable");
                pkg_finish(0, 0x80EE0007, NULL);
                return NULL;
            }
        }
        sleep(2);
    }
    pkg_set_text("status polling timed out");
    pkg_finish(0, 0x80EE0002, NULL);
    return NULL;
}

static int handle_pkg_install(int sock, const char *pkg_path) {
    pthread_mutex_lock(&g_pkg_state_lock);
    if (g_pkg_state.active && !g_pkg_state.done) {
        pthread_mutex_unlock(&g_pkg_state_lock);
        send_response(sock, RESP_OK, "ERROR:install already running", 29);
        return -1;
    }
    memset(&g_pkg_state, 0, sizeof(g_pkg_state));
    g_pkg_state.active = 1;
    snprintf(g_pkg_state.text, sizeof(g_pkg_state.text), "starting");
    pthread_mutex_unlock(&g_pkg_state_lock);

    char *copy = strdup(pkg_path);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int ok = copy && pthread_create(&t, &a, pkg_install_worker, copy) == 0;
    pthread_attr_destroy(&a);
    if (!ok) {
        free(copy);
        pkg_finish(0, 0xFFFFFFFE, NULL);
        send_response(sock, RESP_OK, "ERROR:cannot start worker", 25);
        return -1;
    }
    send_response(sock, RESP_OK, "STARTED", 7);
    return 0;
}

static int handle_pkg_install_status(int sock) {
    pkg_state_t st;
    pthread_mutex_lock(&g_pkg_state_lock);
    st = g_pkg_state;
    pthread_mutex_unlock(&g_pkg_state_lock);

    // Terminal state first: a failed worker clears active+sets done, and the
    // client must see the error, not IDLE.
    char resp[256];
    if (st.done) {
        if (st.error_code) {
            snprintf(resp, sizeof(resp), "ERROR:0x%08X:%s", st.error_code, st.text);
        } else {
            snprintf(resp, sizeof(resp), "DONE:%s", st.content_id);
        }
        send_response(sock, RESP_OK, resp, strlen(resp));
        return 0;
    }
    if (!st.active) {
        send_response(sock, RESP_OK, "IDLE", 4);
        return 0;
    }

    // Active — the install worker polls the daemon itself; we never touch
    // the IPC from a session thread (it can wedge under loaders without
    // etaHEN). Just surface the worker's last reported phase.
    snprintf(resp, sizeof(resp), "PROGRESS:%s:%d", st.text[0] ? st.text : "working", st.pct);
    send_response(sock, RESP_OK, resp, strlen(resp));
    return 0;
}

// ============================================================================
// Fan Control (etaHEN/Elf Arsenal compatible)
// Uses /dev/icc_fan ioctl to set fan temperature threshold
// ============================================================================

// Forward declaration (defined later in file)
void send_notification(const char *msg);

#define ICC_FAN_DEVICE "/dev/icc_fan"
#define ICC_FAN_THRESHOLD_IOCTL 0xC01C8F07ul
#define ICC_FAN_GET_MANUAL_DUTY 0xC0068F06ul
#define FAN_MIN_C 30
#define FAN_MAX_C 90
#define FAN_DEFAULT_C 60
#define FAN_STATE_FILE "/data/ps5_suite_fan.cfg"

// The icc_fan driver has no "get threshold" ioctl — 0xC0068F06 reads the
// manual-duty register (fan speed), not the °C pin. So we track the last
// threshold WE set, persisted to a state file so it survives restarts.
// -1 = unknown (system-managed, never set by us).
static int g_fan_threshold = -1;

static void fan_state_load(void) {
    if (g_fan_threshold >= 0) return;
    FILE *f = fopen(FAN_STATE_FILE, "r");
    if (!f) return;
    int t = -1;
    if (fscanf(f, "%d", &t) == 1 && t >= FAN_MIN_C && t <= FAN_MAX_C)
        g_fan_threshold = t;
    fclose(f);
}

static void fan_state_save(int temp_c) {
    FILE *f = fopen(FAN_STATE_FILE, "w");
    if (!f) return;
    fprintf(f, "%d\n", temp_c);
    fclose(f);
}

// Get last-set fan threshold + live manual-duty register for diagnostics.
// Response: "OK:<threshold|-1>:<duty0..duty5>"
static int handle_fan_get_threshold(int sock) {
    fan_state_load();

    int duty_rc = -1;
    unsigned char duty[6] = {0};
    int fd = open(ICC_FAN_DEVICE, O_RDONLY);
    if (fd >= 0) {
        duty_rc = ioctl(fd, ICC_FAN_GET_MANUAL_DUTY, duty);
        close(fd);
    }

    char resp[128];
    int off = snprintf(resp, sizeof(resp), "OK:%d:", g_fan_threshold);
    if (duty_rc == 0)
        off += snprintf(resp + off, sizeof(resp) - off,
                        "%d,%d,%d,%d,%d,%d",
                        duty[0], duty[1], duty[2], duty[3], duty[4], duty[5]);
    send_response(sock, RESP_OK, resp, strlen(resp));
    return 0;
}

// Set fan threshold (30-90°C)
static int handle_fan_set_threshold(int sock, int temp_c) {
    // Clamp to valid range
    if (temp_c < FAN_MIN_C) temp_c = FAN_MIN_C;
    if (temp_c > FAN_MAX_C) temp_c = FAN_MAX_C;

    int fd = open(ICC_FAN_DEVICE, O_RDONLY);
    if (fd < 0) {
        char err[64];
        snprintf(err, sizeof(err), "ERROR:open:%d", errno);
        send_response(sock, RESP_OK, err, strlen(err));
        return -1;
    }

    // etaHEN exact layout: 10-byte buffer with threshold at offset 5
    unsigned char data[10] = {0, 0, 0, 0, 0, (unsigned char)temp_c, 0, 0, 0, 0};
    int rc = ioctl(fd, ICC_FAN_THRESHOLD_IOCTL, data);
    int eno = errno;
    close(fd);

    if (rc < 0) {
        char err[64];
        snprintf(err, sizeof(err), "ERROR:ioctl:%d", eno);
        send_response(sock, RESP_OK, err, strlen(err));
        return -1;
    }

    g_fan_threshold = temp_c;
    fan_state_save(temp_c);

    char resp[64];
    snprintf(resp, sizeof(resp), "OK:%d", temp_c);
    send_response(sock, RESP_OK, resp, strlen(resp));

    
    // Send notification to PS5 screen
    char notify[64];
    snprintf(notify, sizeof(notify), "Fan threshold set to %d°C", temp_c);
    send_notification(notify);
    
    return 0;
}

// Registration using the dump_installer model (by EchoStretch):
//   1. Resolve AppInstallTitleDir via kernel_dynlib (NID)
//   2. Call sceAppInstUtilInitialize (extern linked) - done in process_game
//   3. Call sceAppInstUtilAppUnInstall to clear stale registration - done in process_game
//   4. Try AppInstallTitleDir first
//   5. Fallback to AppInstallAll for FW 12.00+

// install_app - dump_installer style registration
// Returns 0 on success, negative on failure
static int install_app(const char *title_id, const char *base_path,
                       const char *game_path) {
    int ret;

    // AppInstUtil daemon IPC wedges the whole process under kstuff-light.
    // The registry row AppInstallAll would create is just sqlite rows in
    // app.db — write them directly instead (no IPC, same result).
    if (!etahen_present()) {
        send_progress_message("registering via app.db (etaHEN absent)");
        return appdb_direct_register(title_id, game_path);
    }

    // Resolve AppInstallTitleDir via kernel_dynlib (dump_installer method)
    resolve_appinstutil();
    
    // Try AppInstallTitleDir first (if resolved)
    if (g_sceAppInstUtilAppInstallTitleDir) {
        ret = g_sceAppInstUtilAppInstallTitleDir(title_id, base_path, 0);
        if (ret == 0) {
            send_progress_message("Used AppInstallTitleDir");
            return 0;
        }
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "AppInstallTitleDir failed: 0x%X", ret);
        send_progress_message(dbg);
    } else {
        send_progress_message("AppInstallTitleDir not available");
    }
    
    // Fallback to AppInstallAll (required for FW 12.00+)
    send_progress_message("Falling back to AppInstallAll...");
    ret = sceAppInstUtilAppInstallAll(0);
    if (ret == 0) {
        send_progress_message("Used AppInstallAll");
        return 0;
    }
    
    char dbg[128];
    snprintf(dbg, sizeof(dbg), "AppInstallAll failed: 0x%X", ret);
    send_progress_message(dbg);
    return ret;
}

// dump_installer style: Initialize + UnInstall are called BEFORE the copy
// operations (in process_game), so this function only does the install step.
// The watchdog wrapper ensures a stalled IPC burns at most one worker thread.
static int appinst_register_work(const char *title_id, const char *game_path) {
    // Just install - Initialize and UnInstall already done in process_game
    return install_app(title_id, "/user/app/", game_path);
}

static int register_title(const char *title_id, const char *game_path) {
    send_progress_message("dbg: AppInstUtil register...");
    int rc = wdg_fn_call((void*)appinst_register_work,
                         (void*)title_id, (void*)game_path, NULL, NULL,
                         WDG_TIMEOUT_MS);
    char dbg[128];
    snprintf(dbg, sizeof(dbg), "dbg: register rc=%d", rc);
    send_progress_message(dbg);
    return rc;
}

// ----------------------------------------------------------------------------
// Generic watchdog for DIRECT function pointers (kernel syscalls like
// nmount/unmount that are already linked into our ELF). Crash containment
// machinery (t_wdg_jmp / wdg_fault_handler / wdg_install_handler) is defined
// further up so the early module-load trampoline can use it too.
// ----------------------------------------------------------------------------

typedef struct {
    void *fn;
    void *args[4];
    int   done;
    int   result;
} wdg_fn_t;

static void *wdg_fn_trampoline(void *arg) {
    wdg_fn_t *c = (wdg_fn_t *)arg;
    int (*fn4)(void *, void *, void *, void *) =
        (int (*)(void *, void *, void *, void *))c->fn;
    if (sigsetjmp(t_wdg_jmp, 1) == 0) {
        t_wdg_armed = 1;
        c->result = fn4(c->args[0], c->args[1], c->args[2], c->args[3]);
        t_wdg_armed = 0;
    } else {
        t_wdg_armed = 0;
        c->result = WDG_RESULT_CRASHED;
    }
    __atomic_store_n(&c->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

// Returns the function's result, or -2 on timeout (worker detached).

// Dead-function registry: once a guarded call times out, its worker stays
// parked inside the kernel IPC forever. Re-calling it on the next refresh
// would just leak another thread AND block for the full timeout again —
// and these calls either answer in a few ms or never, so a function that
// timed out once is marked dead and skipped instantly from then on.
#define WDG_DEAD_MAX 32
static void *g_wdg_dead[WDG_DEAD_MAX];
static volatile int g_wdg_dead_count = 0;

static int wdg_is_dead(void *fn) {
    int n = g_wdg_dead_count;
    if (n > WDG_DEAD_MAX) n = WDG_DEAD_MAX;
    for (int i = 0; i < n; i++)
        if (g_wdg_dead[i] == fn) return 1;
    return 0;
}

static void wdg_mark_dead(void *fn) {
    int i = __atomic_fetch_add(&g_wdg_dead_count, 1, __ATOMIC_SEQ_CST);
    if (i < WDG_DEAD_MAX) g_wdg_dead[i] = fn;
}

static int wdg_fn_call(void *fn, void *a, void *b, void *c, void *d, int timeout_ms) {
    wdg_install_handler();
    if (wdg_is_dead(fn)) return -2;
    wdg_fn_t *call = (wdg_fn_t *)malloc(sizeof(wdg_fn_t));
    if (!call) return -2;
    call->fn = fn;
    call->args[0] = a; call->args[1] = b; call->args[2] = c; call->args[3] = d;
    call->done = 0;
    call->result = -2;

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    int rc = pthread_create(&tid, &at, wdg_fn_trampoline, call);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        free(call);
        return -2;
    }

    int waited = 0;
    while (waited < timeout_ms) {
        if (__atomic_load_n(&call->done, __ATOMIC_SEQ_CST)) {
            int res = call->result;
            free(call);
            if (res == WDG_RESULT_CRASHED) wdg_mark_dead(fn);
            return res;
        }
        struct timespec ms20 = { 0, 20 * 1000 * 1000 };
        nanosleep(&ms20, NULL);
        waited += 20;
    }
    wdg_mark_dead(fn);
    return -2;   // timed out — leak the 40-byte state, worker keeps running
}

// 64-bit variant for size_t/intptr_t-returning functions (memory sizes can
// exceed 4 GB — an int result would truncate). Timeout returns INTPTR_MIN.
typedef struct {
    void *fn;
    void *args[5];
    int      done;
    intptr_t result;
} wdg64_t;

static void *wdg64_trampoline(void *arg) {
    wdg64_t *c = (wdg64_t *)arg;
    intptr_t (*fn5)(void *, void *, void *, void *, void *) =
        (intptr_t (*)(void *, void *, void *, void *, void *))c->fn;
    if (sigsetjmp(t_wdg_jmp, 1) == 0) {
        t_wdg_armed = 1;
        c->result = fn5(c->args[0], c->args[1], c->args[2], c->args[3], c->args[4]);
        t_wdg_armed = 0;
    } else {
        t_wdg_armed = 0;
        c->result = (intptr_t)WDG_RESULT_CRASHED;
    }
    __atomic_store_n(&c->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static intptr_t wdg_fn_call64x(void *fn, void *a, void *b, void *c, void *d,
                               void *e, int timeout_ms) {
    wdg_install_handler();
    if (wdg_is_dead(fn)) return INTPTR_MIN;
    wdg64_t *call = (wdg64_t *)malloc(sizeof(wdg64_t));
    if (!call) return INTPTR_MIN;
    call->fn = fn;
    call->args[0] = a; call->args[1] = b; call->args[2] = c; call->args[3] = d;
    call->args[4] = e;
    call->done = 0;
    call->result = INTPTR_MIN;

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    int rc = pthread_create(&tid, &at, wdg64_trampoline, call);
    pthread_attr_destroy(&at);
    if (rc != 0) { free(call); return INTPTR_MIN; }

    int waited = 0;
    while (waited < timeout_ms) {
        if (__atomic_load_n(&call->done, __ATOMIC_SEQ_CST)) {
            intptr_t res = call->result;
            free(call);
            if (res == (intptr_t)WDG_RESULT_CRASHED) wdg_mark_dead(fn);
            return res;
        }
        struct timespec ms20 = { 0, 20 * 1000 * 1000 };
        nanosleep(&ms20, NULL);
        waited += 20;
    }
    wdg_mark_dead(fn);
    return INTPTR_MIN;   // timed out — state leaks, worker keeps running
}

static intptr_t wdg_fn_call64(void *fn, void *a, void *b, void *c, void *d, int timeout_ms) {
    return wdg_fn_call64x(fn, a, b, c, d, NULL, timeout_ms);
}

// ---------------------------------------------------------------------------
// Watchdog wrappers for the extended kernel-info calls.
// Every Sony service/kernel helper can IPC-stall inside a payload process —
// each call is capped at WDG_TIMEOUT_MS on a detached worker. Out-params go
// through rotating STATIC scratch slots so a leaked worker writes to memory
// that stays valid forever, never to a dead stack frame.
// ---------------------------------------------------------------------------
#define WDG_SCRATCH_SLOTS 32
#define WDG_INFO_TIMEOUT_MS 2500

// int fn(int*) — e.g. IccGetThermalAlert, IccGetBDPowerState
static int wdg_out_int(void *fn, int *out) {
    static int s_slots[WDG_SCRATCH_SLOTS];
    static volatile unsigned s_next = 0;
    unsigned i = __atomic_fetch_add(&s_next, 1, __ATOMIC_SEQ_CST) % WDG_SCRATCH_SLOTS;
    s_slots[i] = 0;
    int r = wdg_fn_call(fn, &s_slots[i], NULL, NULL, NULL, WDG_INFO_TIMEOUT_MS);
    if (r != -2) *out = s_slots[i];
    return r;
}

// int fn(uint64_t*) — e.g. IccGetPowerOperatingTime
static int wdg_out_u64(void *fn, uint64_t *out) {
    static uint64_t s_slots[WDG_SCRATCH_SLOTS];
    static volatile unsigned s_next = 0;
    unsigned i = __atomic_fetch_add(&s_next, 1, __ATOMIC_SEQ_CST) % WDG_SCRATCH_SLOTS;
    s_slots[i] = 0;
    int r = wdg_fn_call(fn, &s_slots[i], NULL, NULL, NULL, WDG_INFO_TIMEOUT_MS);
    if (r != -2) *out = s_slots[i];
    return r;
}

// int fn(char*, size_t) — e.g. GetSystemSwVersion, GetProductCode, GetProductStr
// Some variants write the string at a nonzero offset inside a fixed-size
// struct, so scan the buffer for the first printable run.
static int wdg_out_str(void *fn, char *buf, size_t n) {
    static char s_slots[WDG_SCRATCH_SLOTS][256];
    static volatile unsigned s_next = 0;
    unsigned i = __atomic_fetch_add(&s_next, 1, __ATOMIC_SEQ_CST) % WDG_SCRATCH_SLOTS;
    memset(s_slots[i], 0, sizeof(s_slots[i]));
    int r = wdg_fn_call(fn, s_slots[i], (void *)n, NULL, NULL, WDG_INFO_TIMEOUT_MS);
    if (r == -2 || r == WDG_RESULT_CRASHED) return r;
    for (int p = 0; p < 240; p++) {
        if (s_slots[i][p] >= 0x20 && s_slots[i][p] < 0x7f) {
            size_t k = 0;
            while (k + 1 < n && s_slots[i][p + k] >= 0x20 && s_slots[i][p + k] < 0x7f) {
                buf[k] = s_slots[i][p + k]; k++;
            }
            buf[k] = '\0';
            if (k) return r;
        }
        if (s_slots[i][p] == '\0' && p > 4) break;
    }
    return r;
}

// Some "string" getters actually fill a struct whose first field is a size
// and whose version text sits at an offset (e.g. SceKernelSwVersion =
// {u32 size; char str[28]}). Call into raw scratch, then locate the first
// printable run that looks like a version ("d.d", "9.00", ...).
static int wdg_out_version(void *fn, char *buf, size_t n) {
    static char s_slots[WDG_SCRATCH_SLOTS][256];
    static volatile unsigned s_next = 0;
    unsigned i = __atomic_fetch_add(&s_next, 1, __ATOMIC_SEQ_CST) % WDG_SCRATCH_SLOTS;
    memset(s_slots[i], 0, sizeof(s_slots[i]));
    // SceKernelSwVersion-style APIs require the caller to set the size field.
    s_slots[i][0] = (char)0x28; s_slots[i][4] = (char)0x28;
    int r = wdg_fn_call(fn, s_slots[i], (void *)sizeof(s_slots[i]), NULL, NULL,
                        WDG_INFO_TIMEOUT_MS);
    if (r != 0) return r;   // on error, keep the caller's default

    const char *s = s_slots[i];
    // Scan for the first digit that begins a dotted version run.
    for (int p = 0; p < 200; p++) {
        if (s[p] >= '0' && s[p] <= '9') {
            int q = p, dots = 0;
            while (q < 232 && ((s[q] >= '0' && s[q] <= '9') || s[q] == '.')) {
                if (s[q] == '.') dots++;
                q++;
            }
            if (dots >= 1 && q > p + 1) {
                size_t len = (size_t)(q - p);
                if (len >= n) len = n - 1;
                memcpy(buf, s + p, len);
                buf[len] = '\0';
                return r;
            }
        }
        if (s[p] == '\0') break;
    }
    // Fallback: copy the buffer's leading printable run verbatim.
    size_t k = 0;
    while (k + 1 < n && s[k] >= 0x20 && s[k] < 0x7F) { buf[k] = s[k]; k++; }
    buf[k] = '\0';
    return r;
}

// Memory-size getters come in two ABIs:
//   int fn(uint64_t *start, uint64_t *end, int *type, uint64_t *flags)  → r=0
//   size_t fn(void) / int fn(uint64_t *size_out)                        → r>0
// Call with ALL args as valid scratch pointers; unused ones are ignored,
// and a timed-out worker still writes to memory that stays valid forever.
int wdg_avail_mem_dbg(void *fn, size_t *out, intptr_t dbg[5]);
static int wdg_avail_mem(void *fn, size_t *out) {
    return wdg_avail_mem_dbg(fn, out, NULL);
}

// dbg (optional): receives {r, out1, out2, type, flags} for diagnosis.
// sceKernelAvailableFlexibleMemorySize(u64 *out) writes the size to arg1.
// sceKernelAvailableDirectMemorySize(start, end, align, *physOut, *sizeOut)
// needs a real search range — handled by wdg_avail_dmem below.
int wdg_avail_mem_dbg(void *fn, size_t *out, intptr_t dbg[5]) {
    static uint64_t s_a[WDG_SCRATCH_SLOTS];
    static uint64_t s_b[WDG_SCRATCH_SLOTS];
    static int      s_type[WDG_SCRATCH_SLOTS];
    static uint64_t s_flags[WDG_SCRATCH_SLOTS];
    static volatile unsigned s_next = 0;
    unsigned i = __atomic_fetch_add(&s_next, 1, __ATOMIC_SEQ_CST) % WDG_SCRATCH_SLOTS;
    s_a[i] = s_b[i] = s_flags[i] = 0;
    s_type[i] = 0;
    intptr_t r = wdg_fn_call64(fn, &s_a[i], &s_b[i], &s_type[i], &s_flags[i],
                               WDG_INFO_TIMEOUT_MS);
    if (dbg) {
        dbg[0] = r; dbg[1] = (intptr_t)s_a[i]; dbg[2] = (intptr_t)s_b[i];
        dbg[3] = s_type[i]; dbg[4] = (intptr_t)s_flags[i];
    }
    if (r == INTPTR_MIN) return -2;
    if (r < 0 || (r >= 0x80000000 && r <= 0x8002FFFF)) return (int)r;  // err
    if (r == 0) {
        // out-param variant: [start,end) range OR single size in s_a
        if (s_b[i] > s_a[i] && s_a[i] != 0) { *out = (size_t)(s_b[i] - s_a[i]); return 0; }
        if (s_a[i] > 0x10000)                 { *out = (size_t)s_a[i];          return 0; }
        return -1;
    }
    *out = (size_t)r;                            // direct size_t return
    return 0;
}

// sceKernelAvailableDirectMemorySize(u64 searchStart, u64 searchEnd,
//   u64 alignment, u64 *physAddrOut, u64 *sizeOut) — verified ABI.
// Query the full range for the largest available contiguous block.
static int wdg_avail_dmem(void *fn, size_t *out, intptr_t dbg[5]) {
    static uint64_t s_phys[WDG_SCRATCH_SLOTS];
    static uint64_t s_size[WDG_SCRATCH_SLOTS];
    static volatile unsigned s_next = 0;
    unsigned i = __atomic_fetch_add(&s_next, 1, __ATOMIC_SEQ_CST) % WDG_SCRATCH_SLOTS;
    s_phys[i] = s_size[i] = 0;
    // Try (0,0) first — Orbis treats a zero range as "anywhere".
    intptr_t r = wdg_fn_call64x(fn, 0, 0, 0,
                                &s_phys[i], &s_size[i], WDG_INFO_TIMEOUT_MS);
    if (r != 0 && r != INTPTR_MIN)
        r = wdg_fn_call64x(fn, 0, (void *)(intptr_t)-1, 0,
                           &s_phys[i], &s_size[i], WDG_INFO_TIMEOUT_MS);
    if (r != 0 && r != INTPTR_MIN)
        r = wdg_fn_call64x(fn, 0, (void *)(intptr_t)0x400000000LL, 0,
                           &s_phys[i], &s_size[i], WDG_INFO_TIMEOUT_MS);
    if (r == INTPTR_MIN) return -2;
    if (dbg) {
        dbg[0] = r; dbg[1] = (intptr_t)s_phys[i]; dbg[2] = (intptr_t)s_size[i];
        dbg[3] = 0; dbg[4] = 0;
    }
    if (r == 0 && s_size[i] > 0) { *out = (size_t)s_size[i]; return 0; }
    return (int)r;
}

// int fn(int, int*) — e.g. GetCpuUsage
static int wdg_cpuusage_one(void *fn, int core, int *out) {
    static int s_slots[WDG_SCRATCH_SLOTS];
    static volatile unsigned s_next = 0;
    unsigned i = __atomic_fetch_add(&s_next, 1, __ATOMIC_SEQ_CST) % WDG_SCRATCH_SLOTS;
    s_slots[i] = 0;
    int r = wdg_fn_call(fn, (void *)(intptr_t)core, &s_slots[i], NULL, NULL, WDG_INFO_TIMEOUT_MS);
    if (r != -2) *out = s_slots[i];
    return r;
}

// int fn(uint32_t*, size_t, size_t*) — e.g. GetModuleList.
// Orbis ABI: fn(handles, max_count, &actual_count).
// Some kernels instead want the count as an in/out pointer in arg2 —
// we try both and keep whichever yields a sane count.
static int wdg_module_list(void *fn, uint32_t *handles, size_t max, size_t *actual) {
    static uint32_t s_handles[WDG_SCRATCH_SLOTS][256];
    static size_t   s_actual[WDG_SCRATCH_SLOTS];
    static volatile unsigned s_next = 0;
    unsigned i = __atomic_fetch_add(&s_next, 1, __ATOMIC_SEQ_CST) % WDG_SCRATCH_SLOTS;
    s_actual[i] = 0;
    memset(s_handles[i], 0, sizeof(s_handles[i]));
    int r = wdg_fn_call(fn, s_handles[i], (void *)max, &s_actual[i], NULL, WDG_INFO_TIMEOUT_MS);
    if (r == 0 && (s_actual[i] == 0 || s_actual[i] > max)) {
        // try count-in/out-pointer variant: fn(handles, &count)
        size_t cnt = max;
        int r2 = wdg_fn_call(fn, s_handles[i], &cnt, NULL, NULL, WDG_INFO_TIMEOUT_MS);
        if (r2 == 0 && cnt > 0 && cnt <= max) { s_actual[i] = cnt; r = r2; }
    }
    if (r != -2 && r != WDG_RESULT_CRASHED) {
        size_t n = s_actual[i];
        if (n > max) n = max;
        memcpy(handles, s_handles[i], n * sizeof(uint32_t));
        *actual = n;
    }
    return r;
}

// int fn(uint32_t handle, SceKernelModuleInfo *info) — the struct's first
// field is a size_t "size" that MUST be preset to sizeof(SceKernelModuleInfo)
// = 0x160 by the caller (Orbis ABI, same convention as SceKernelSwVersion).
// Layout: size @0, name[256] @0x8, handle @0x108... code_base @0x108,
// code_size @0x110, data_base @0x118, data_size @0x120.
static int wdg_module_info(void *fn, uint32_t handle, void *info, size_t n) {
    static char s_slots[WDG_SCRATCH_SLOTS][512];
    static volatile unsigned s_next = 0;
    unsigned i = __atomic_fetch_add(&s_next, 1, __ATOMIC_SEQ_CST) % WDG_SCRATCH_SLOTS;
    memset(s_slots[i], 0, sizeof(s_slots[i]));
    *(uint64_t *)s_slots[i] = 0x160;
    int r = wdg_fn_call(fn, (void *)(uintptr_t)handle, s_slots[i], NULL, NULL,
                        WDG_INFO_TIMEOUT_MS);
    if (r != -2 && r != WDG_RESULT_CRASHED) memcpy(info, s_slots[i], n < 512 ? n : 512);
    return r;
}

// ============================================================================
// Drop-in wrappers using kernel_dynlib resolution (NID-based, reliable on PS5)
// All Sony APIs are resolved via NIDs and called with watchdog protection.
// ============================================================================

static int sceKernelGetHwModelName(char *name) {
    resolve_kernelsys_functions();
    if (!g_sceKernelGetHwModelName) return -3;
    return wdg_out_str((void*)g_sceKernelGetHwModelName, name, 1024);
}

static int sceKernelGetHwSerialNumber(char *serial) {
    resolve_kernelsys_functions();
    if (!g_sceKernelGetHwSerialNumber) return -3;
    return wdg_out_str((void*)g_sceKernelGetHwSerialNumber, serial, 1024);
}

// Service IPC stays watchdog-guarded: a stalled call burns one worker thread,
// never the server. Symbols are statically linked externs.
// Whole launch sequence on ONE watchdog thread — matches v6 semantics where
// UserService/LncUtil init + launch all ran on the caller's thread (the libs
// can keep thread-local IPC state).
typedef struct {
    const char *title_id;
    int fg_user;
    int ret, ret_null, ret_sys;
} launch_work_t;

static int launch_work(launch_work_t *w) {
    // UserService/LncUtil are daemon IPC too — they wedge the process under
    // kstuff-light exactly like AppInstUtil. Without etaHEN, refuse cleanly.
    if (!etahen_present()) {
        w->ret = w->ret_null = w->ret_sys = -1;
        return 0;
    }
    sceUserServiceInitialize(NULL);
    sceLncUtilInitialize();
    w->fg_user = 0;
    sceUserServiceGetForegroundUser(&w->fg_user);

    char lnc_param[1024];
    memset(lnc_param, 0, sizeof(lnc_param));
    *(uint32_t *)(lnc_param + 0x00) = sizeof(lnc_param);  // size
    *(int32_t  *)(lnc_param + 0x04) = w->fg_user;         // user_id
    *(uint32_t *)(lnc_param + 0x08) = 0;                  // app_opt
    *(uint64_t *)(lnc_param + 0x10) = 0;                  // crash_report
    *(uint64_t *)(lnc_param + 0x18) = 0;                  // check_flag

    w->ret = sceLncUtilLaunchApp(w->title_id, NULL, lnc_param);
    w->ret_null = 0;
    w->ret_sys  = 0;
    if (w->ret < 0) {
        w->ret_null = sceLncUtilLaunchApp(w->title_id, NULL, NULL);
        if (w->ret_null >= 0) w->ret = w->ret_null;
    }
    if (w->ret < 0) {
        w->ret_sys = sceSystemServiceLaunchApp(w->title_id, NULL, NULL);
        if (w->ret_sys == 0) w->ret = 0;
    }
    return 0;   // completion marker for wdg_fn_call
}

#include <sys/_iovec.h>
#include <sys/param.h>
#include <sys/uio.h>
#include <limits.h>

#define SERVER_PORT 9113
static int g_bound_port = SERVER_PORT;   // actual port (fallback when 9113 is wedged)
#define BUFFER_SIZE (16 * 1024 * 1024)  // 16MB for maximum throughput - matches socket buffers
#define MAX_PATH 2048
#define DISK_WORKER_COUNT 4
#define QUEUE_MAX_SIZE 32

// Game mounting defines
#define IOVEC_ENTRY(x) { (void*)(x), (x) ? strlen(x) + 1 : 0 }
#define IOVEC_SIZE(x)  (sizeof(x) / sizeof(struct iovec))
#define GAMES_BASE_PATH "/data/etaHEN/games"

// Per-file mutex hash map to prevent corruption during parallel uploads to SAME file
// Different files can write in parallel without blocking each other
typedef struct file_mutex_entry {
    char path[MAX_PATH];
    pthread_mutex_t mutex;
    int ref_count;
    struct file_mutex_entry *next;
} file_mutex_entry_t;

static file_mutex_entry_t *g_file_mutexes = NULL;
static pthread_mutex_t g_mutex_map_lock = PTHREAD_MUTEX_INITIALIZER;

// Get or create mutex for a specific file path
pthread_mutex_t* get_file_mutex(const char *path) {
    pthread_mutex_lock(&g_mutex_map_lock);
    
    // Search for existing mutex
    file_mutex_entry_t *entry = g_file_mutexes;
    while (entry) {
        if (strcmp(entry->path, path) == 0) {
            entry->ref_count++;
            pthread_mutex_unlock(&g_mutex_map_lock);
            return &entry->mutex;
        }
        entry = entry->next;
    }
    
    // Create new mutex for this file
    entry = (file_mutex_entry_t*)malloc(sizeof(file_mutex_entry_t));
    if (!entry) {
        pthread_mutex_unlock(&g_mutex_map_lock);
        return NULL;
    }
    
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
    pthread_mutex_init(&entry->mutex, NULL);
    entry->ref_count = 1;
    entry->next = g_file_mutexes;
    g_file_mutexes = entry;
    
    pthread_mutex_unlock(&g_mutex_map_lock);
    return &entry->mutex;
}

// Release mutex reference
void release_file_mutex(const char *path) {
    pthread_mutex_lock(&g_mutex_map_lock);
    
    file_mutex_entry_t **ptr = &g_file_mutexes;
    while (*ptr) {
        if (strcmp((*ptr)->path, path) == 0) {
            (*ptr)->ref_count--;
            if ((*ptr)->ref_count == 0) {
                file_mutex_entry_t *to_free = *ptr;
                *ptr = (*ptr)->next;
                pthread_mutex_destroy(&to_free->mutex);
                free(to_free);
            }
            break;
        }
        ptr = &(*ptr)->next;
    }
    
    pthread_mutex_unlock(&g_mutex_map_lock);
}

// Protocol commands
#define CMD_PING 0x01
#define CMD_LIST_STORAGE 0x02
#define CMD_LIST_DIR 0x03
#define CMD_CREATE_DIR 0x04
#define CMD_DELETE_FILE 0x05
#define CMD_DELETE_DIR 0x06
#define CMD_RENAME 0x07
#define CMD_COPY_FILE 0x08
#define CMD_MOVE_FILE 0x09
#define CMD_START_UPLOAD 0x10
#define CMD_UPLOAD_CHUNK 0x11
#define CMD_END_UPLOAD 0x12
#define CMD_DOWNLOAD_FILE 0x13
#define CMD_SHELL_OPEN 0x20
#define CMD_SHELL_EXEC 0x21
#define CMD_SHELL_INTERRUPT 0x22
#define CMD_SHELL_CLOSE 0x23
#define CMD_INDEX_START 0x40
#define CMD_INDEX_STATUS 0x41
#define CMD_SEARCH_INDEX 0x42
#define CMD_INDEX_CANCEL 0x43
#define CMD_MOUNT_GAMES 0x30
#define CMD_GET_FILE_INFO 0x31
#define CMD_GET_SYSTEM_INFO 0x32
#define CMD_VERIFY_FILE 0x33
#define CMD_GET_HW_INFO 0x34
#define CMD_GET_TEMPS 0x35
#define CMD_GET_RUNNING_APPS 0x36
#define CMD_KILL_APP 0x37
#define CMD_LAUNCH_BROWSER 0x38
#define CMD_GET_POWER_INFO 0x39
#define CMD_GET_GAME_LIST 0x3A
#define CMD_UNMOUNT_GAME 0x3B
#define CMD_GET_GAME_ICON 0x3C
#define CMD_GET_GAME_DETAILS 0x3D
#define CMD_GET_GAME_PIC 0x3E
#define CMD_LIST_SAVES 0x3F
#define CMD_LAUNCH_GAME 0x44
#define CMD_LIST_SCREENSHOTS 0x45
#define CMD_DELETE_SCREENSHOT 0x46
#define CMD_GET_EXTENDED_INFO 0x47
#define CMD_GET_CPU_USAGE 0x48
#define CMD_GET_MEMORY_INFO 0x49
#define CMD_GET_MODULE_LIST 0x4A

// New features (ps5upload-style)
#define CMD_PKG_INSTALL 0x50        // Install PKG from path or URL
#define CMD_PKG_INSTALL_STATUS 0x51 // Get install progress
#define CMD_FAN_GET_THRESHOLD 0x52  // Get fan threshold
#define CMD_FAN_SET_THRESHOLD 0x53  // Set fan threshold
#define CMD_SAVE_SCAN 0x60          // List save FILES (per image, garlic-style)
#define CMD_SAVE_MOUNT 0x61         // Mount save image decrypted at /data/save_mnt
#define CMD_SAVE_UNMOUNT 0x62       // Unmount + write modified image back
#define CMD_SAVE_MOUNT_STATUS 0x63  // What is mounted at /data/save_mnt
#define CMD_SAVE_DELETE 0x64        // Delete save image (+ .bin companion)
#define CMD_PROC_LIST 0x66          // pid|comm list of all processes
#define CMD_RESTART_UI 0x67         // Restart SceShellUI (renderer) → app.db re-read
#define CMD_SELF_UPDATE 0x68        // Stage/URL → hand new ELF to local loader
#define CMD_MEM_READ    0x69        // "pid|addr|len"      → RESP_DATA raw bytes
#define CMD_MEM_WRITE   0x6A        // "pid|addr|hexdata"  → RESP_OK written
#define CMD_MEM_REGIONS 0x6B        // "pid"               → RESP_DATA vm map lines
#define CMD_MEM_SEARCH  0x6C        // "pid|start|end|hex" → RESP_DATA match addrs
#define CMD_KLOG_READ   0x6D        // optional tail bytes  → RESP_DATA kmsg
#define CMD_MOUNT_GAME  0x6F        // "title_id" → mount a single game

// Payload self-update: the running server pushes a newer ELF to the local
// payload loader (elfldr raw-socket protocol — identical to what the PC
// client does during deploy). The spawned instance sweeps every stale
// "payload*" process and binds the port — no explicit handoff needed.
#define SUITE_VERSION      "7.2.3"
#define SUITE_DIR          "/data/ps5suite"
#define UPDATE_STAGE_PATH  "/data/ps5suite/update.elf"
#define UPDATE_MARKER      "/data/ps5suite/update.marker"
#define UPDATE_LOADER_PORT 9021
// 0x76-0x7F reserved (was used by removed experimental pad commands)

// App Manager v2 — real per-app info, suspend/resume/kill, coredump
#define CMD_APP_LIST_V2    0x70  // "pid|appid|title|comm|apptype|cpu_x100|susp\n"
#define CMD_APP_SUSPEND    0x71  // "appid" → sceLncUtilSuspendApp
#define CMD_APP_RESUME     0x72  // "appid" → sceLncUtilResumeApp
#define CMD_APP_KILL       0x73  // "appid" → ForceKillApp, fallback KillApp
#define CMD_APP_COREDUMP   0x74  // "appid" → KickCoredumpOnlyProcMem
#define CMD_NET_INFO       0x75  // → key=value lines + if|rx|tx counters
#define CMD_NET_SPEEDTEST  0x76  // → 16MB stream, client measures link throughput
#define CMD_POWER_ACTION   0x77  // "reboot"|"shutdown" → reboot() syscall
#define CMD_USB_LIST       0x78  // → usb mount lines "path|fstype|dev|total|free"
#define CMD_PAD_INFO       0x79  // → controller info key=value + raw hex dump
#define CMD_DISC_DUMP      0x7A  // "start"|"status"|"cancel" → disc → HDD copy
#define CMD_SCREENSHOT     0x7B  // → sceScreenShotCapture (screen grab)
#define CMD_NOTIFY         0x7C  // "text" → PS5 UI notification
#define CMD_PAD_ACTION     0x7D  // "lightbar|r,g,b" | "vibrate|l,s"
#define CMD_ICC_CONTROL    0x7E  // "led|n" "buzzer|n" "buzzervol|n" "buzzermute|n" "ledcolor|b,w,o"

#define CMD_SHUTDOWN 0xFF

// Check whether the peer address of a connected socket is a loopback one.
// Used to gate the CMD_SHUTDOWN kill switch to the console itself.
static bool is_local_connection(int sock) {
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    if (getpeername(sock, (struct sockaddr*)&ss, &slen) != 0)
        return false;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *in4 = (struct sockaddr_in*)&ss;
        uint32_t addr = ntohl(in4->sin_addr.s_addr);
        return (addr >> 24) == 127;  // 127.0.0.0/8
    }
    if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6*)&ss;
        return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
    }
    return false;
}

// Forward declaration
void send_progress_message(const char *msg);
static void payload_restore_privileges(void);   // defined in main() section

// Serialize mount-games runs: the g_mounted_ids duplicate list must not be
// reset while another client is mid-mount.
static pthread_mutex_t g_mounted_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_mounted_ids(void)   { pthread_mutex_lock(&g_mounted_lock); }
static void unlock_mounted_ids(void) { pthread_mutex_unlock(&g_mounted_lock); }

// ============================================================================
// GLOBAL STATE GUARDS (shared progress socket + counters)
// ============================================================================

// The background delete/mount threads report progress over the socket of the
// client that started them. g_client_sock is guarded by g_progress_mutex and
// ownership is transferred with claim/release so a recycled fd is never
// written to by a stale thread.
static int g_client_sock = 0;
static pthread_mutex_t g_progress_mutex = PTHREAD_MUTEX_INITIALIZER;
static void progress_socket_set(int sock) {
    pthread_mutex_lock(&g_progress_mutex);
    g_client_sock = sock;
    pthread_mutex_unlock(&g_progress_mutex);
}

static void progress_socket_clear(int sock) {
    pthread_mutex_lock(&g_progress_mutex);
    if (g_client_sock == sock) g_client_sock = 0;
    pthread_mutex_unlock(&g_progress_mutex);
}

// (RESP_* response codes are defined near the top of the file — needed by
// the early PKG-install/fan handlers)

// Kernel notification request — 3120-byte buffer, message at offset 45.
// When byte 44 (use_icon_uri) is set, the URI at offset 1069 supplies the
// icon, e.g. "cxml://psnotification/tex_icon_system" (PS4-Notify layout).
typedef struct notify_request {
    int32_t type;             // 0x00 = 0
    int32_t unk1[3];          // 0x04..0x0F
    int32_t target_id;        // 0x10 = -1 → all users
    int32_t unk2[5];          // 0x14..0x27
    int32_t unk3;             // 0x28 = 0
    uint8_t use_icon_uri;     // 0x2C
    char    message[3075];    // 0x2D — icon URI goes at absolute offset 1069
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);

// Supported game paths - internal, USB drives, and M.2 SSD
static const char* GAME_SCAN_PATHS[] = {
    "/data/etaHEN/games",    // Internal etaHEN storage
    "/mnt/usb0/games",       // USB drive 0
    "/mnt/usb1/games",       // USB drive 1
    "/mnt/usb2/games",       // USB drive 2
    "/mnt/usb3/games",       // USB drive 3
    "/mnt/ext0/games",       // M.2 SSD
};
#define NUM_GAME_SCAN_PATHS (sizeof(GAME_SCAN_PATHS) / sizeof(GAME_SCAN_PATHS[0]))

// Track mounted title IDs in a dynamically sized list (title IDs are always
// 9 chars + NUL, so no need for a fixed 256x12 array).
typedef struct mounted_id_node {
    char id[10];
    struct mounted_id_node *next;
} mounted_id_node_t;

static mounted_id_node_t *g_mounted_ids = NULL;
static int g_mounted_count = 0;

static int is_duplicate_title(const char* title_id) {
    for (mounted_id_node_t *n = g_mounted_ids; n; n = n->next) {
        if (strcmp(n->id, title_id) == 0)
            return 1;
    }
    return 0;
}

static void track_mounted_title(const char* title_id) {
    mounted_id_node_t *n = (mounted_id_node_t*)malloc(sizeof(mounted_id_node_t));
    if (!n) return;
    snprintf(n->id, sizeof(n->id), "%s", title_id);
    n->next = g_mounted_ids;
    g_mounted_ids = n;
    g_mounted_count++;
}

void send_notification_ex(const char *msg, const char *icon_uri) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    req.target_id = -1;
    strncpy(req.message, msg, sizeof(req.message) - 1);
    if (icon_uri) {
        req.use_icon_uri = 1;
        // icon URI lives at absolute offset 1069 (inside the message region)
        strncpy(req.message + (1069 - 45), icon_uri,
                sizeof(req.message) - (1069 - 45) - 1);
    }
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

void send_notification(const char *msg) {
    send_notification_ex(msg, NULL);
}

// ============================================================================
// Rich system notifications (the "pretty" right-side banner, etaHEN-style)
// ============================================================================
//
// sceKernelSendNotificationRequest only produces the small "Debug:" toast on
// the left. The nice banner is sceNotificationSend(userId, isLogged, json)
// from libSceNotification — resolved at RUNTIME so the ELF never depends on
// the module being present. Falls back to the debug toast when unavailable.

#include "icon_data.h"   // g_ps_icon_png / g_ps_icon_png_len (PS-symbols logo)

// Notification icons resolve under /user/data — the notification daemon
// cannot read /data (proven working path from the JB notification archive).
#define SUITE_ICON_DIR  "/user/data/notifi"
// NOTE: the notification daemon caches icons by URL — bump the filename when
// the icon changes or the old image keeps showing from cache.
#define SUITE_ICON_PATH "/user/data/notifi/ps5suite_logo.png"
#define SCE_NOTIF_SYSTEM_USER 0xFE

static int (*g_sceNotificationSend)(int, int, const char*);
static int g_notif_tried = 0;

static void resolve_sce_notif(void) {
    if (g_notif_tried) return;
    g_notif_tried = 1;
    static const char *paths[] = {
        "libSceNotification.sprx",
        "/system/common/lib/libSceNotification.sprx",
        "/system/vsh/libSceNotification.sprx",
        "/preinst2/common/lib/libSceNotification.sprx",
        "/preinst2/vsh/libSceNotification.sprx",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        void *h = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            g_sceNotificationSend = (int (*)(int,int,const char*))
                dlsym(h, "sceNotificationSend");
            if (g_sceNotificationSend) return;
        }
    }
    // kernel_dynlib fallback (module may be loaded under another context)
    uint32_t mod = 0;
    if (krpc_dynlib_handle(getpid(), "libSceNotification.sprx", &mod) == 0 && mod) {
        char nid[12];
        nid_encode("sceNotificationSend", nid);
        void *vp = krpc_resolve_sym(getpid(), mod, nid);
        if (vp) g_sceNotificationSend = (int (*)(int,int,const char*))vp;
    }
}

// Drop the embedded suite icon to /data so the notification service can
// reach it by file URL (rewritten each boot — it's ~700 bytes).
static void install_suite_icon(void) {
    mkdir(SUITE_ICON_DIR, 0755);
    FILE *f = fopen(SUITE_ICON_PATH, "wb");
    if (f) { fwrite(g_ps_icon_png, 1, g_ps_icon_png_len, f); fclose(f); }
}

void send_notification_pretty(const char *body, const char *sub) {
    resolve_sce_notif();
    char fallback[256];
    if (!g_sceNotificationSend) {
        snprintf(fallback, sizeof(fallback), "%s\n%s", body, sub);
        send_notification(fallback);
        return;
    }
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    char ts[40];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S.000Z", &tmv);
    char json[2048];
    snprintf(json, sizeof(json),
        "{\"rawData\":{"
        "\"viewTemplateType\":\"InteractiveToastTemplateB\","
        "\"channelType\":\"Downloads\","
        "\"useCaseId\":\"IDC\","
        "\"toastOverwriteType\":\"No\","
        "\"isImmediate\":true,"
        "\"priority\":100,"
        "\"viewData\":{"
        "\"icon\":{\"type\":\"Url\",\"parameters\":{\"url\":\"%s\",\"iconSize\":\"Square\"}},"
        "\"message\":{\"body\":\"%s\"},"
        "\"subMessage\":{\"body\":\"%s\"}"
        "},"
        "\"platformViews\":{\"previewDisabled\":{\"viewData\":{"
        "\"icon\":{\"type\":\"Url\",\"parameters\":{\"url\":\"%s\",\"iconSize\":\"Square\"}},"
        "\"message\":{\"body\":\"%s\"}"
        "}}}"
        "},"
        "\"createdDateTime\":\"%s\","
        "\"localNotificationId\":\"%ld\"}",
        SUITE_ICON_PATH, body, sub, SUITE_ICON_PATH, body, ts, (long)now);
    if (g_sceNotificationSend(SCE_NOTIF_SYSTEM_USER, 1, json) != 0) {
        snprintf(fallback, sizeof(fallback), "%s\n%s", body, sub);
        send_notification(fallback);
    }
}

// ============================================================================
// GAME MOUNTING FUNCTIONS
// ============================================================================

// Kernel mount/unmount can BLOCK indefinitely on busy vnodes (e.g. a game
// running from the mount) — every nmount/unmount call below runs under the
// watchdog so a busy kernel object can never freeze a client session.
// (wdg_call4 is defined in the OPTIONAL SYSTEM APIs section near the top.)
static int mount_nullfs(const char* src, const char* dst) {
    struct iovec iov[] = {
        IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"),
        IOVEC_ENTRY("from"),   IOVEC_ENTRY(src),
        IOVEC_ENTRY("fspath"), IOVEC_ENTRY(dst),
    };
    int rc = wdg_fn_call((void *)nmount, iov, (void *)(long)IOVEC_SIZE(iov), (void *)(long)0, NULL, 5000);
    if (rc == -2) { errno = EBUSY; return -1; }   // timed out — treat as busy
    return rc;
}

// Watchdog-guarded unmount: normal first, forced as fallback, never blocks.
static int unmount_guarded(const char* path, int flags) {
    int rc = wdg_fn_call((void *)unmount, (void *)path, (void *)(long)flags, NULL, NULL, 5000);
    if (rc == -2) { errno = EBUSY; return -1; }   // timed out — treat as busy
    return rc;
}

// Watchdog-guarded unlink/rmdir: on a busy vnode (e.g. ShellCore holding an
// appmeta icon open) unlink() can BLOCK forever inside the kernel — an
// unguarded rmdir_recursive would hang the whole unmount handler. With the
// guard, a stuck call is abandoned after 5s and we move on.
static int unlink_guarded(const char *path) {
    int rc = wdg_fn_call((void *)unlink, (void *)path, NULL, NULL, NULL, 2000);
    if (rc == -2) { errno = EBUSY; return -1; }   // timed out — worker detached
    return rc;
}

static int rmdir_guarded(const char *path) {
    int rc = wdg_fn_call((void *)rmdir, (void *)path, NULL, NULL, NULL, 2000);
    if (rc == -2) { errno = EBUSY; return -1; }
    return rc;
}

static int is_mounted(const char* path) {
    struct statfs sfs;
    if (statfs(path, &sfs) != 0)
        return 0;
    return strcmp(sfs.f_fstypename, "nullfs") == 0;
}

// ---------------- COPY DIRECTORY ----------------
// One reusable 2MB buffer shared by copy_dir() and copy_sce_sys_to_appmeta()
// (was malloc/free per file) 
static char* g_copy_buf = NULL;

// Copy a single file (dump_installer style)
static int copy_file(const char* src, const char* dst) {
    struct stat st;
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

    int fd_in = open(src, O_RDONLY);
    if (fd_in < 0) return -1;
    
    int fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode | 0600);
    if (fd_out < 0) { 
        close(fd_in); 
        return -1; 
    }

    if (!g_copy_buf) {
        g_copy_buf = (char*)malloc(2097152);
    }
    
    if (!g_copy_buf) {
        close(fd_in);
        close(fd_out);
        return -1;
    }

    size_t copied = 0;
    ssize_t n;
    while ((n = read(fd_in, g_copy_buf, 2097152)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(fd_out, g_copy_buf + off, (size_t)(n - off));
            if (w <= 0) break;
            off += w;
            copied += w;
        }
        if (off < n) break;
    }

    close(fd_in);
    close(fd_out);

    return (copied == (size_t)st.st_size) ? 0 : -1;
}

static int copy_dir(const char* src, const char* dst) {
    if (mkdir(dst, 0755) && errno != EEXIST) {
        return -1;
    }

    DIR* d = opendir(src);
    if (!d) return -1;

    if (!g_copy_buf) {
        g_copy_buf = (char*)malloc(2097152);
    }

    struct dirent* e;
    char ss[PATH_MAX], dd[PATH_MAX];
    struct stat st;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
        snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);

        if (stat(ss, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            copy_dir(ss, dd);
        } else {
            unlink(dd);
            
            int src_fd = open(ss, O_RDONLY);
            if (src_fd < 0) continue;
            
            int dst_fd = open(dd, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (dst_fd < 0) {
                close(src_fd);
                continue;
            }
            
            if (g_copy_buf) {
                ssize_t n;
                while ((n = read(src_fd, g_copy_buf, 2097152)) > 0) {
                    ssize_t off = 0;
                    while (off < n) {
                        ssize_t w = write(dst_fd, g_copy_buf + off, (size_t)(n - off));
                        if (w <= 0) break;
                        off += w;
                    }
                    if (off < n) break;
                }
            }
            
            close(src_fd);
            close(dst_fd);
        }
    }
    closedir(d);
    return 0;
}

// ---------------- COPY appmeta ----------------
static int is_appmeta_file(const char* name) {
    if (!strcasecmp(name, "param.json") ||
        !strcasecmp(name, "param.sfo"))
        return 1;

    const char* ext = strrchr(name, '.');
    if (!ext) return 0;

    return !strcasecmp(ext, ".png") ||
           !strcasecmp(ext, ".dds") ||
           !strcasecmp(ext, ".at9");
}

static int copy_sce_sys_to_appmeta(const char* src, const char* title_id) {
    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "/user/appmeta/%s", title_id);

    mkdir("/user/appmeta", 0777);
    mkdir(dst, 0755);

    DIR* d = opendir(src);
    if (!d) return -1;

    struct dirent* e;
    char ss[PATH_MAX], dd[PATH_MAX];
    struct stat st;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;

        if (!is_appmeta_file(e->d_name))
            continue;

        snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
        snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);

        if (stat(ss, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        unlink(dd);
        
        int src_fd = open(ss, O_RDONLY);
        if (src_fd < 0) continue;
        
        int dst_fd = open(dd, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst_fd < 0) {
            close(src_fd);
            continue;
        }
        
        if (g_copy_buf) {
            ssize_t n;
            while ((n = read(src_fd, g_copy_buf, 2097152)) > 0) {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(dst_fd, g_copy_buf + off, (size_t)(n - off));
                    if (w <= 0) break;
                    off += w;
                }
                if (off < n) break;
            }
        }
        
        close(src_fd);
        close(dst_fd);
    }

    closedir(d);
    return 0;
}

// ---------------- TROPHY & SND0 UPDATE (dump_installer style) ----------------
// Copy trophy2/npbind.dat, uds/npbind.dat, and param.json to system_data
static int update_trophy(const char* title_id, const char* src_sce_sys) {
    char dst_base[PATH_MAX];
    char src[PATH_MAX];
    char dst[PATH_MAX];

    snprintf(dst_base, sizeof(dst_base), "/system_data/priv/appmeta/%s", title_id);

    // Create only the needed subdirectories
    char trophy2_dir[PATH_MAX];
    char uds_dir[PATH_MAX];
    snprintf(trophy2_dir, sizeof(trophy2_dir), "%s/trophy2", dst_base);
    snprintf(uds_dir, sizeof(uds_dir), "%s/uds", dst_base);

    mkdir(dst_base, 0755);
    mkdir(trophy2_dir, 0755);
    mkdir(uds_dir, 0755);

    // 1. trophy2/npbind.dat
    snprintf(src, sizeof(src), "%s/trophy2/npbind.dat", src_sce_sys);
    snprintf(dst, sizeof(dst), "%s/trophy2/npbind.dat", dst_base);
    if (access(src, F_OK) == 0) {
        copy_file(src, dst);
    }

    // 2. uds/npbind.dat
    snprintf(src, sizeof(src), "%s/uds/npbind.dat", src_sce_sys);
    snprintf(dst, sizeof(dst), "%s/uds/npbind.dat", dst_base);
    if (access(src, F_OK) == 0) {
        copy_file(src, dst);
    }

    // 3. param.json
    snprintf(src, sizeof(src), "%s/param.json", src_sce_sys);
    snprintf(dst, sizeof(dst), "%s/param.json", dst_base);
    if (access(src, F_OK) == 0) {
        copy_file(src, dst);
    }

    return 0;
}

// Update snd0info in app.db (dump_installer style)
// This sets the sound path for the game in the system database.
static int update_snd0info(const char* title_id) {
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;
    int ret = -1;
    const char* db_path = "/system_data/priv/mms/app.db";
    const char* sql = "UPDATE tbl_contentinfo SET snd0info = '/user/appmeta/' || ?1 || '/snd0.at9' WHERE titleId = ?1;";

    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    
    sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
    sqlite3_step(stmt);  // ignore result, we don't care if row existed
    ret = sqlite3_changes(db);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ret;
}

// ============================================================================
// DIRECT app.db REGISTRATION (kstuff-light path)
// ============================================================================
// When etaHEN is absent, the AppInstUtil daemon IPC wedges the process, so
// the icon registration that AppInstallAll normally performs can't run.
// The registry is just /system_data/priv/mms/app.db though — local sqlite,
// no IPC. We insert the same rows the daemon writes (schema verified against
// a real etaHEN-Toolbox entry) into tbl_contentinfo + the per-user
// tbl_iconinfo_*/tbl_concepticoninfo_* tables.
// Returns 0 on success — the mount is then fully registered.
static int appdb_exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        char m[160];
        snprintf(m, sizeof(m), "appdb: %s", err ? err : "exec failed");
        send_progress_message(m);
        if (err) sqlite3_free(err);
    }
    return rc;
}

static char *slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 1024 * 1024) { fclose(f); return NULL; }
    char *b = (char *)malloc(len + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, len, f) != (size_t)len) { free(b); fclose(f); return NULL; }
    b[len] = 0;
    fclose(f);
    return b;
}

// Recursive size walk — stat-only, fast even for large trees.
static unsigned long long dir_size_bytes(const char *path, int depth) {
    if (depth > 16) return 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    unsigned long long total = 0;
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) total += dir_size_bytes(p, depth + 1);
        else if (S_ISREG(st.st_mode)) total += (unsigned long long)st.st_size;
    }
    closedir(d);
    return total;
}

// mode: 1 = register, 0 = unregister
static int appdb_register_work(int mode, const char *title_id,
                               const char *game_path) {
    sqlite3 *db = NULL;
    const char *db_path = "/system_data/priv/mms/app.db";

    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -10;
    }
    sqlite3_busy_timeout(db, 3000);

    // Per-user table suffix (e.g. tbl_iconinfo_0459853461) varies per
    // console/account — discover it from sqlite_master.
    char icon_tbl[64] = {0}, concept_tbl[64] = {0};
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table' AND (name LIKE 'tbl_iconinfo_%' OR name LIKE 'tbl_concepticoninfo_%')",
            -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *n = (const char *)sqlite3_column_text(st, 0);
            if (!n) continue;
            if (strncmp(n, "tbl_iconinfo_", 13) == 0)
                snprintf(icon_tbl, sizeof(icon_tbl), "%s", n);
            else if (strncmp(n, "tbl_concepticoninfo_", 20) == 0)
                snprintf(concept_tbl, sizeof(concept_tbl), "%s", n);
        }
    }
    sqlite3_finalize(st);

    char sql[16384];
    const char *now = "datetime('now','localtime')";

    // Always start from a clean slate for this title
    snprintf(sql, sizeof(sql),
             "DELETE FROM tbl_contentinfo WHERE titleId='%s';", title_id);
    appdb_exec(db, sql);
    if (icon_tbl[0]) {
        snprintf(sql, sizeof(sql),
                 "DELETE FROM %s WHERE titleId='%s';", icon_tbl, title_id);
        appdb_exec(db, sql);
    }
    if (concept_tbl[0]) {
        snprintf(sql, sizeof(sql),
                 "DELETE FROM %s WHERE primaryTitleId='%s';", concept_tbl, title_id);
        appdb_exec(db, sql);
    }

    if (!mode) {                      // unregister = deletes only
        sqlite3_close(db);
        return 0;
    }

    // ---- Parse the game's param.json for a daemon-complete row ----
    // The launch deeplink, contentId, conceptId, appType, age levels and the
    // real display name all come from sce_sys/param.json — without them the
    // home-screen icon is a dead entry (hub opens but "Play" errors out).
    char param_path[PATH_MAX];
    char *pj = NULL;
    if (game_path && game_path[0]) {
        snprintf(param_path, sizeof(param_path), "%s/sce_sys/param.json",
                 game_path);
        pj = slurp_file(param_path);
    }
    if (!pj) {
        snprintf(param_path, sizeof(param_path),
                 "/user/app/%s/sce_sys/param.json", title_id);
        pj = slurp_file(param_path);
    }

    char name[128];
    if (get_game_name_from_json(param_path, name, sizeof(name)) != 0)
        snprintf(name, sizeof(name), "%s", title_id);

    char content_id[64] = {0}, content_ver[32] = {0}, sdk_ver[32] = {0};
    char pubtool_ver[32] = {0}, version_uri[256] = {0};
    unsigned long long concept_id = 0, attr = 0, attr2 = 0, attr3 = 0;
    unsigned long long badge = 0, udp1 = 0, dl_size = 0, req_sys = 0;
    char age_level[2048] = {0}, game_intent[512] = {0};
    if (pj) {
        extract_json_string(pj, "contentId", content_id, sizeof(content_id));
        extract_json_string(pj, "contentVersion", content_ver, sizeof(content_ver));
        extract_json_string(pj, "sdkVersion", sdk_ver, sizeof(sdk_ver));
        extract_json_string(pj, "toolVersion", pubtool_ver, sizeof(pubtool_ver));
        extract_json_string(pj, "versionFileUri", version_uri, sizeof(version_uri));
        extract_json_int(pj, "conceptId", &concept_id);
        extract_json_int(pj, "attribute", &attr);
        extract_json_int(pj, "attribute2", &attr2);
        extract_json_int(pj, "attribute3", &attr3);
        extract_json_int(pj, "contentBadgeType", &badge);
        extract_json_int(pj, "userDefinedParam1", &udp1);
        extract_json_int(pj, "downloadDataSize", &dl_size);
        extract_json_int(pj, "requiredSystemSoftwareVersion", &req_sys);
        extract_json_object(pj, "ageLevel", age_level, sizeof(age_level));
        extract_json_object(pj, "gameIntent", game_intent, sizeof(game_intent));
        free(pj);
    }

    unsigned long long app_size = dl_size;
    if (game_path && game_path[0]) {
        unsigned long long sz = dir_size_bytes(game_path, 0);
        if (sz) app_size = sz;
    }
    int is_game = content_id[0] != '\0';

    // SQL-escape the name (quotes) and JSON-escape it (", \)
    char esc[256]; int ei = 0;
    for (const char *p = name; *p && ei < (int)sizeof(esc) - 2; p++) {
        if (*p == '\'') esc[ei++] = '\'';
        esc[ei++] = *p;
    }
    esc[ei] = 0;
    char jesc[256]; int ji = 0;
    for (const char *p = name; *p && ji < (int)sizeof(jesc) - 3; p++) {
        if (*p == '"' || *p == '\\') jesc[ji++] = '\\';
        jesc[ji++] = *p;
    }
    jesc[ji] = 0;

    char lcid[96];
    if (concept_id)
        snprintf(lcid, sizeof(lcid), "cid:scp:%016llx", concept_id);
    else
        snprintf(lcid, sizeof(lcid), "cid:local:%s", title_id);

    char deeplink[96];
    snprintf(deeplink, sizeof(deeplink), "psgm:play?id=%s", title_id);

    // Metadata dir: prefer /user/appmeta (real-game convention — the sce_sys
    // copy already lands there via copy_sce_sys_to_appmeta).
    char metadir[PATH_MAX];
    struct stat st_md;
    snprintf(metadir, sizeof(metadir), "/user/appmeta/%s", title_id);
    if (stat(metadir, &st_md) != 0 || !S_ISDIR(st_md.st_mode))
        snprintf(metadir, sizeof(metadir), "/user/app/%s/sce_sys", title_id);

    long ts = (long)time(NULL);
    char tmp[PATH_MAX];
    char icon_frag[PATH_MAX + 64], pic_frag[PATH_MAX + 64];
    char snd_frag[PATH_MAX + 64];
    snprintf(tmp, sizeof(tmp), "%s/icon0.png", metadir);
    if (stat(tmp, &st_md) == 0)
        snprintf(icon_frag, sizeof(icon_frag), "'%s?ts=%ld'", tmp, ts);
    else
        snprintf(icon_frag, sizeof(icon_frag),
                 "'/user/app/%s/sce_sys/icon0.png?ts=%ld'", title_id, ts);
    snprintf(tmp, sizeof(tmp), "%s/pic0.dds", metadir);
    if (stat(tmp, &st_md) == 0)
        snprintf(pic_frag, sizeof(pic_frag), "'%s?ts=%ld'", tmp, ts);
    else strcpy(pic_frag, "NULL");
    snprintf(tmp, sizeof(tmp), "%s/snd0.at9", metadir);
    if (stat(tmp, &st_md) == 0)
        snprintf(snd_frag, sizeof(snd_frag), "'%s?ts=%ld'", tmp, ts);
    else strcpy(snd_frag, "NULL");

    char age_frag[2200];
    if (age_level[0]) snprintf(age_frag, sizeof(age_frag), "'%s'", age_level);
    else strcpy(age_frag, "NULL");
    char cid_frag[80];
    if (content_id[0]) snprintf(cid_frag, sizeof(cid_frag), "'%s'", content_id);
    else strcpy(cid_frag, "NULL");

    char nowstr[32];
    {
        time_t t = time(NULL);
        struct tm *tmv = localtime(&t);
        strftime(nowstr, sizeof(nowstr), "%Y-%m-%d %H:%M:%S.000", tmv);
    }

    // Daemon-style AppInfoJson — same shape the real appinst writes.
    // JCAT keeps the write offset inside the buffer even if a field is
    // unexpectedly large (snprintf returns the would-be length).
    char json[4608];
    int jn = 0;
#define JCAT(...) do { \
    int _rem = (int)sizeof(json) - jn; \
    if (_rem > 0) { \
        jn += snprintf(json + jn, (size_t)_rem, __VA_ARGS__); \
        if (jn > (int)sizeof(json)) jn = (int)sizeof(json); \
    } \
} while (0)
    JCAT(
        "{\"#_access_index\":0,\"#_contents_status\":0,"
        "\"#_install_time\":\"%s\",\"#_last_access_time\":\"%s\","
        "\"#_mtime\":\"%s\",\"#_promote_time\":\"%s\","
        "\"#_size\":%llu,\"#_size_app_only\":%llu,",
        nowstr, nowstr, nowstr, nowstr, app_size, app_size);
    if (concept_id)
        JCAT(
            "\"#catalog_concept_id\":%llu,\"#catalog_content_badge_type\":%llu,"
            "\"#catalog_content_version\":\"%s\",\"#catalog_ps_platform\":0,"
            "\"#catalog_title\":\"%s\",\"#catalog_title_01\":\"%s\",",
            concept_id, badge,
            content_ver[0] ? content_ver : "01.000.000", jesc, jesc);
    JCAT(
        "\"#exit_type\":0,\"APP_TYPE\":%d,\"ATTRIBUTE\":%llu,\"ATTRIBUTE2\":%llu,"
        "\"ATTRIBUTE3\":%llu,\"ATTRIBUTE4\":0,\"ATTRIBUTE_EXE\":0,"
        "\"ATTRIBUTE_INTERNAL\":0,\"CATEGORY_TYPE\":0,",
        is_game ? 1 : 0, attr, attr2, attr3);
    if (concept_id)
        JCAT(
            "\"CONCEPT_ID\":%llu,", concept_id);
    if (content_id[0])
        JCAT(
            "\"CONTENT_ID\":\"%s\",", content_id);
    if (content_ver[0])
        JCAT(
            "\"CONTENT_VERSION\":\"%s\",", content_ver);
    JCAT(
        "\"CONTENT_BADGE_TYPE\":%llu,\"DEEPLINK_URI\":\"%s\",\"DISPLAYLOCATION\":138,"
        "\"DISP_LOCATION_1\":0,\"DISP_LOCATION_2\":0,\"DOWNLOAD_DATA_SIZE\":%llu,"
        "\"DOWNLOAD_DATA_SIZE_2\":0,\"GAME_INTENT\":%s,"
        "\"HUBAPP_URI\":\"pshome:gamehub?titleId=%s\",\"MASS_SIZE\":0,"
        "\"METADATA_ID\":\"prior:internal:0\",\"NOTICE_SCREEN_VERSION\":0,"
        "\"PARENTAL_LEVEL\":0,\"SERVICE_LAUNCH_BUTTON_KEY_CODE\":0,",
        badge, deeplink, dl_size,
        game_intent[0] ? game_intent
                       : "{\"permittedIntents\":[{\"intentType\":\"launchActivity\"}]}",
        title_id);
    if (pubtool_ver[0])
        JCAT(
            "\"PUBTOOL_VERSION\":\"%s\",", pubtool_ver);
    if (sdk_ver[0])
        JCAT(
            "\"SDK_VERSION\":\"%s\",", sdk_ver);
    if (req_sys)
        JCAT(
            "\"SYSTEM_VER_PPR\":%llu,", req_sys);
    JCAT(
        "\"TITLE\":\"%s\",\"TITLE_01\":\"%s\",\"TITLE_ID\":\"%s\","
        "\"USER_DEFINED_PARAM_1\":%llu,\"USER_DEFINED_PARAM_2\":0,"
        "\"USER_DEFINED_PARAM_3\":0,\"USER_DEFINED_PARAM_4\":0,",
        jesc, jesc, title_id, udp1);
    if (version_uri[0])
        JCAT(
            "\"VERSION_FILE_URI\":\"%s\",", version_uri);
    JCAT(
        "\"_app_format_type\":1,\"_contents_ext_type\":0,\"_contents_location\":2,"
        "\"_current_slot\":0,\"_disable_live_detail\":0,\"_external_hdd_app_status\":0,"
        "\"_hdd_location\":0,\"_install_status\":0,\"_install_sub_status\":1,"
        "\"_large_pkg_type\":0,\"_local_concept_id\":\"%s\",\"_m2_device_id\":0,"
        "\"_metadata_path\":\"%s\",\"_not_install_sub_status\":0,"
        "\"_org_path\":\"/user/app/%s\",\"_path_changeinfo_info\":0,\"_path_info\":0,"
        "\"_path_info_2\":0,\"_primary_title_sort\":196613,\"_ps_platform\":0,"
        "\"_size_other_hdd\":0,\"_sort_priority\":100,\"_uninstallable\":1,"
        "\"_view_category\":0}",
        lcid, metadir, title_id);

    snprintf(sql, sizeof(sql),
        "INSERT INTO tbl_contentinfo ("
        "titleId,contentId,titleName,metaDataPath,lastAccessTime,contentStatus,"
        "contentLocation,sortPriority,pathInfo,lastAccessIndex,dispLocation,"
        "uninstallable,pathInfo2,size,promoteTime,installTime,viewCategory,"
        "hddLocation,externalHddAppStatus,mTime,sizeOtherHdd,appFormatType,"
        "categoryType,pprAttribute3,pprAttribute4,attributeInternal,conceptId,"
        "localConceptId,pprDeeplinkUri,pprHubAppUri,pprAgeLevel,platform,"
        "installStatus,snd0Info,icon0Info,pic0Info,discStatus,isTitleDiscInserted,"
        "appType,primaryTitleSort,appinfoContentType,contentBadgeType,"
        "is3rdPartyVideoApp,packageLocation,AppInfoJson) VALUES ("
        "'%s',%s,'%s','%s',%s,0,2,"
        "100,0,0,138,1,0,"
        "%llu,%s,%s,'game',0,0,"
        "%s,0,1,0,0,0,"
        "0,%llu,'%s','%s','pshome:gamehub?titleId=%s',%s,"
        "0,0,%s,%s,%s,0,0,"
        "%d,196613,1,%llu,"
        "0,1,'%s');",
        title_id, cid_frag, esc, metadir, now,
        app_size, now, now,
        now,
        concept_id, lcid, deeplink, title_id, age_frag,
        snd_frag, icon_frag, pic_frag,
        is_game ? 1 : 0, badge,
        json);
    if (appdb_exec(db, sql) != SQLITE_OK) { sqlite3_close(db); return -11; }

    if (icon_tbl[0]) {
        snprintf(sql, sizeof(sql),
            "INSERT INTO %s (titleId,titleName,localConceptId,conceptId,"
            "lastAccessTime,lastAccessIndex,recentActivityDatePlayedOrInstalled,"
            "installedDate,installStatus,dispLocation,visible,deeplinkUri,"
            "hubAppUri,platform,appDrmType,primaryTitleSort,discStatus,"
            "contentBadgeType,contentId) VALUES ("
            "'%s','%s','%s',%llu,%s,0,%s,%s,2,138,1,"
            "'%s','pshome:gamehub?titleId=%s',0,%d,4295163909,0,%llu,%s);",
            icon_tbl, title_id, esc, lcid, concept_id, now, now, now,
            deeplink, title_id, is_game ? 5 : 0, badge, cid_frag);
        appdb_exec(db, sql);
    }

    if (concept_tbl[0]) {
        snprintf(sql, sizeof(sql),
            "INSERT INTO %s (localConceptId,validFlag,conceptId,conceptName,"
            "primaryTitleId,primaryTitleName,lastInteractedTime,dispLocation,"
            "promoteTime,deeplinkUri,hubAppUri,metaDataPath,icon0Info) VALUES ("
            "'%s',1,%llu,'%s','%s','%s',%s,1162,%s,'%s',"
            "'pshome:gamehub?titleId=%s','%s',%s);",
            concept_tbl, lcid, concept_id, esc, title_id, esc, now, now,
            deeplink, title_id, metadir, icon_frag);
        appdb_exec(db, sql);
    }

    sqlite3_close(db);
    return 0;
}

// Register/unregister a title directly in app.db — used ONLY when etaHEN is
// absent (daemon IPC would wedge). Watchdog-guarded like every risky call.
typedef struct { int mode; char title_id[16]; char game_path[PATH_MAX]; int rc; }
    appdb_job_t;
static int appdb_job_work(appdb_job_t *j) {
    j->rc = appdb_register_work(j->mode, j->title_id, j->game_path);
    return 0;
}
static int appdb_direct_register(const char *title_id, const char *game_path) {
    appdb_job_t *j = (appdb_job_t *)calloc(1, sizeof(*j));
    if (!j) return -1;
    j->mode = 1;
    snprintf(j->title_id, sizeof(j->title_id), "%s", title_id);
    if (game_path) snprintf(j->game_path, sizeof(j->game_path), "%s", game_path);
    int wrc = wdg_fn_call((void*)appdb_job_work, j, NULL, NULL, NULL, 15000);
    int rc = (wrc == 0) ? j->rc : -2;
    if (wrc != -2) free(j);   // timeout → parked worker owns it
    return rc;
}
static int appdb_direct_unregister(const char *title_id) {
    appdb_job_t *j = (appdb_job_t *)calloc(1, sizeof(*j));
    if (!j) return -1;
    j->mode = 0;
    snprintf(j->title_id, sizeof(j->title_id), "%s", title_id);
    int wrc = wdg_fn_call((void*)appdb_job_work, j, NULL, NULL, NULL, 15000);
    int rc = (wrc == 0) ? j->rc : -2;
    if (wrc != -2) free(j);
    return rc;
}

// ---------------- JSON HELPER ----------------
static int extract_json_string(const char* json, const char* key,
                               char* out, size_t out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char* p = strstr(json, search);
    if (!p) return -1;

    p = strchr(p + strlen(search), ':');
    if (!p) return -1;

    while (*++p && isspace(*p));
    if (*p != '"') return -1;
    p++;

    size_t i = 0;
    while (i < out_size - 1 && p[i] && p[i] != '"') {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return 0;
}

// Extract an integer value for "key" — handles both bare numbers and
// quoted values (e.g. "requiredSystemSoftwareVersion":"0x1240...").
// Returns 0 on success. Hex ("0x…") is accepted via base-0 strtoull.
static int extract_json_int(const char* json, const char* key,
                            unsigned long long* out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return -1;
    p = strchr(p + strlen(search), ':');
    if (!p) return -1;
    while (*++p && isspace(*p));
    if (*p == '"') {
        char tmp[64]; size_t i = 0;
        p++;
        while (i < sizeof(tmp) - 1 && p[i] && p[i] != '"') {
            tmp[i] = p[i];
            i++;
        }
        tmp[i] = 0;
        *out = strtoull(tmp, NULL, 0);
        return 0;
    }
    if (*p == '-' || isdigit((unsigned char)*p)) {
        *out = strtoull(p, NULL, 0);
        return 0;
    }
    return -1;
}

// Extract a raw nested JSON object for "key" (e.g. "ageLevel":{...}) —
// copies "{...}" including braces into out. Depth counter handles nesting.
static int extract_json_object(const char* json, const char* key,
                               char* out, size_t out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return -1;
    p = strchr(p + strlen(search), ':');
    if (!p) return -1;
    while (*++p && isspace(*p));
    if (*p != '{') return -1;

    int depth = 0, in_str = 0;
    size_t i = 0;
    for (const char* q = p; *q; q++) {
        char c = *q;
        if (in_str) {
            if (c == '\\' && q[1]) { if (i < out_size - 1) out[i++] = c; q++; c = *q; if (i < out_size - 1) out[i++] = c; continue; }
            if (c == '"') in_str = 0;
        } else {
            if (c == '"') in_str = 1;
            else if (c == '{') depth++;
            else if (c == '}') { depth--; if (depth == 0) { if (i < out_size - 1) out[i++] = c; break; } }
        }
        if (i < out_size - 1) out[i++] = c;
    }
    out[i] = 0;
    return (depth == 0) ? 0 : -1;
}

// ---------------- SFO READER FOR PS4 ----------------
typedef struct {
    uint16_t key_offset;
    uint16_t type;
    uint32_t size;
    uint32_t max_size;
    uint32_t data_offset;
} sfo_entry_t;

// Generic SFO string lookup — reads `want_key` ("TITLE_ID", "TITLE", ...)
static int read_sfo_string(const char* path, const char* want_key,
                           char* out, size_t size)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    uint32_t magic, version, key_off, data_off, count;
    if (fread(&magic, 4, 1, f) != 1 ||
        fread(&version, 4, 1, f) != 1 ||
        fread(&key_off, 4, 1, f) != 1 ||
        fread(&data_off, 4, 1, f) != 1 ||
        fread(&count, 4, 1, f) != 1) {
        fclose(f);
        return -1;
    }

    if (magic != 0x46535000) {
        fclose(f);
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        sfo_entry_t entry;
        if (fseek(f, 0x14 + i * sizeof(sfo_entry_t), SEEK_SET) != 0) continue;
        if (fread(&entry, sizeof(sfo_entry_t), 1, f) != 1) continue;

        char key[128] = {};
        if (fseek(f, key_off + entry.key_offset, SEEK_SET) != 0) continue;
        if (fread(key, 1, sizeof(key) - 1, f) <= 0) continue;

        for (int k = 0; k < sizeof(key); k++) {
            if (key[k] == '\0' || !isprint(key[k])) {
                key[k] = '\0';
                break;
            }
        }

        if (strcmp(key, want_key) == 0) {
            if (fseek(f, data_off + entry.data_offset, SEEK_SET) != 0) continue;
            size_t rlen = (entry.size < size - 1) ? entry.size : size - 1;
            if (fread(out, 1, rlen, f) <= 0) continue;
            out[rlen] = '\0';

            // trim trailing NUL/space only — UTF-8 names must survive
            for (int j = rlen - 1; j >= 0; j--) {
                if (out[j] == '\0' || out[j] == ' ' || out[j] == '\t')
                    out[j] = '\0';
                else
                    break;
            }

            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return -1;
}

static int read_title_id_from_sfo(const char* path,
                                 char* title_id,
                                 size_t size)
{
    return read_sfo_string(path, "TITLE_ID", title_id, size);
}

// ---------------- GET GAME REGION ----------------
static const char* get_game_region(const char* title_id) {
    if (!title_id || strlen(title_id) < 4) return "Unknown";
    
    if (strncmp(title_id, "PPSA", 4) == 0) {
        char region_code = title_id[4];
        switch (region_code) {
            case '0': return "US";
            case '1': return "EU";
            case '2': return "JP";
            case '3': return "Asia";
            case '4': return "UK";
            case '5': return "KR";
            default: return "World";
        }
    }
    
    if (strncmp(title_id, "CUSA", 4) == 0) {
        char region_code = title_id[4];
        switch (region_code) {
            case '0': return "US";
            case '1': return "EU";
            case '2': return "JP";
            case '3': return "Asia";
            case '4': return "UK";
            case '5': return "KR";
            default: return "World";
        }
    }
    
    if (strncmp(title_id, "NPXS", 4) == 0) return "System";
    if (strncmp(title_id, "NPWR", 4) == 0) return "World";
    
    return "Unknown";
}

// ---------------- GET GAME NAME ----------------
static int get_game_name_from_json(const char* json_path, char* name, size_t size) {
    FILE* f = fopen(json_path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 1024 * 1024) {
        fclose(f);
        return -1;
    }

    char* buf = (char*)malloc(len + 1);
    if (!buf) { fclose(f); return -1; }

    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    if (extract_json_string(buf, "contentName", name, size) == 0) {
        name[strcspn(name, "\r\n")] = '\0';
        free(buf);
        return 0;
    }
    
    if (extract_json_string(buf, "titleName", name, size) == 0) {
        name[strcspn(name, "\r\n")] = '\0';
        free(buf);
        return 0;
    }
    
    const char* title_search = strstr(buf, "\"titleName\"");
    if (title_search) {
        const char* colon = strchr(title_search, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t' || *colon == '\n' || *colon == '\r') colon++;
            if (*colon == '\"') {
                colon++;
                const char* end_quote = strchr(colon, '\"');
                if (end_quote && (end_quote - colon) < (long)size) {
                    memcpy(name, colon, end_quote - colon);
                    name[end_quote - colon] = '\0';
                    free(buf);
                    return 0;
                }
            }
        }
    }

    free(buf);
    return -1;
}

// ---------------- GET TITLE_ID ----------------
static int get_title_id_from_dir(const char* game_dir, char* title_id, size_t size) {
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/sce_sys/param.json", game_dir);
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (len > 0 && len < 1024 * 1024) {
            char* buf = (char*)malloc(len + 1);
            if (buf) {
                fread(buf, 1, len, f);
                buf[len] = '\0';

                if (extract_json_string(buf, "titleId", title_id, size) == 0 ||
                    extract_json_string(buf, "title_id", title_id, size) == 0) {
                    title_id[strcspn(title_id, "\r\n")] = '\0';
                    free(buf);
                    fclose(f);
                    return 0;
                }
                free(buf);
            }
        }
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", game_dir);
    return read_title_id_from_sfo(path, title_id, size);
}

// ---------------- PATCH DRM (PS5 only) ----------------
static int fix_application_drm_type(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 1024 * 1024) {
        fclose(f);
        return -1;
    }

    char* buf = (char*)malloc(len + 1);
    if (!buf) { fclose(f); return -1; }

    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    const char* key = "\"applicationDrmType\"";
    char* p = strstr(buf, key);
    if (!p) { free(buf); return 0; }

    char* colon = strchr(p + strlen(key), ':');
    char* q1 = colon ? strchr(colon, '"') : NULL;
    char* q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q1 || !q2) { free(buf); return -1; }

    if ((q2 - q1 - 1) == strlen("standard") &&
        !strncmp(q1 + 1, "standard", strlen("standard"))) {
        free(buf);
        return 0;
    }

    size_t new_len = (q1 - buf) + 1 + strlen("standard") + 1 + strlen(q2 + 1);
    char* out = (char*)malloc(new_len + 1);
    if (!out) { free(buf); return -1; }

    memcpy(out, buf, q1 - buf + 1);
    memcpy(out + (q1 - buf + 1), "standard", strlen("standard"));
    strcpy(out + (q1 - buf + 1 + strlen("standard")), q2);

    f = fopen(path, "wb");
    if (!f) { free(buf); free(out); return -1; }

    fwrite(out, 1, strlen(out), f);
    fclose(f);

    free(buf);
    free(out);
    return 1;
}

// ---------------- CHECK IF ALREADY MOUNTED ----------------
static int is_game_already_mounted(const char* title_id, const char* game_path) {
    char mount_lnk_path[PATH_MAX];
    char system_ex_app[PATH_MAX];
    
    snprintf(mount_lnk_path, sizeof(mount_lnk_path), 
             "/user/app/%s/mount.lnk", title_id);
    
    FILE* f = fopen(mount_lnk_path, "r");
    if (f) {
        char existing_path[PATH_MAX] = {};
        if (fgets(existing_path, sizeof(existing_path), f)) {
            existing_path[strcspn(existing_path, "\r\n")] = '\0';
            fclose(f);
            
            if (strcmp(existing_path, game_path) == 0) {
                snprintf(system_ex_app, sizeof(system_ex_app),
                         "/system_ex/app/%s", title_id);
                if (is_mounted(system_ex_app)) {
                    return 1;
                }
            }
        } else {
            fclose(f);
        }
    }
    
    return 0;
}

// Full title cleanup (dump_installer order) — defined after the unmount handler.
static int full_title_cleanup(const char *title_id, void (*progress)(const char *));
static void unregister_title_async(const char *title_id);

// ---------------- PROCESS ONE GAME ----------------
static int process_game(const char* game_path, char* game_name_out, size_t name_size, char* title_id_out, size_t tid_size) {
    char title_id[12] = {};
    char game_name[256] = "Unknown Game";
    char system_ex_app[PATH_MAX];
    char user_app_dir[PATH_MAX];
    char src_sce_sys[PATH_MAX];
    char mount_lnk_path[PATH_MAX];
    char param_json_path[PATH_MAX];

    // Check sce_sys directory exists
    char sce_sys_check[PATH_MAX];
    snprintf(sce_sys_check, sizeof(sce_sys_check), "%s/sce_sys", game_path);
    struct stat sce_st;
    if (stat(sce_sys_check, &sce_st) != 0 || !S_ISDIR(sce_st.st_mode)) {
        return -1;
    }

    if (get_title_id_from_dir(game_path, title_id, sizeof(title_id))) {
        return -1;
    }

    // Duplicate detection
    if (is_duplicate_title(title_id)) {
        return 3;
    }

    {
        char chk[128];
        snprintf(chk, sizeof(chk), "Checking %s...", title_id);
        send_progress_message(chk);
    }

    // Copy title_id out if requested
    if (title_id_out && tid_size > 0) {
        strncpy(title_id_out, title_id, tid_size - 1);
        title_id_out[tid_size - 1] = '\0';
    }

    snprintf(param_json_path, sizeof(param_json_path),
             "%s/sce_sys/param.json", game_path);
    
    if (get_game_name_from_json(param_json_path, game_name, sizeof(game_name)) != 0) {
        snprintf(game_name, sizeof(game_name), "%s", title_id);
    }
    
    const char* region = get_game_region(title_id);
    
    if (game_name_out && name_size > 0) {
        snprintf(game_name_out, name_size, "%s [%s]", game_name, region);
    }
    
    if (is_game_already_mounted(title_id, game_path)) {
        // Under kstuff a previous attempt may have mounted fine but never
        // written the registry row (daemon IPC unavailable) — the insert is
        // idempotent, so re-register on every remount check.
        if (!etahen_present()) {
            appdb_direct_register(title_id, game_path);
        }
        track_mounted_title(title_id);
        return 2;
    }

    fix_application_drm_type(param_json_path);

    snprintf(system_ex_app, sizeof(system_ex_app),
             "/system_ex/app/%s", title_id);

    mkdir(system_ex_app, 0755);

    if (is_mounted(system_ex_app)) {
        {
            char chk[128];
            snprintf(chk, sizeof(chk), "%s: unmounting stale mount...", title_id);
            send_progress_message(chk);
        }
        unmount_guarded(system_ex_app, 0);
    }

    {
        char chk[128];
        snprintf(chk, sizeof(chk), "%s: mounting game data...", title_id);
        send_progress_message(chk);
    }
    if (mount_nullfs(game_path, system_ex_app)) {
        {
            char chk[128];
            snprintf(chk, sizeof(chk), "%s: ERROR - mount failed", title_id);
            send_progress_message(chk);
        }
        return -1;
    }

    // ========================================================================
    // dump_installer EXACT sequence (lines 409-439 of EchoStretch's main.c):
    // 1. Initialize + UnInstall FIRST (before any copy)
    // 2. update_trophy (before copy)
    // 3. mkdir /user/app + sce_sys
    // 4. copy_dir + copy_sce_sys_to_appmeta
    // 5. install_app LAST (after all copies)
    // ========================================================================

    snprintf(src_sce_sys, sizeof(src_sce_sys),
             "%s/sce_sys", game_path);

    // Step 1: Initialize + UnInstall (dump_installer: lines 411-412)
    {
        char chk[128];
        snprintf(chk, sizeof(chk), "%s: fixing config...", title_id);
        send_progress_message(chk);
    }
    // Daemon calls only exist under etaHEN — under kstuff-light this IPC
    // wedges the whole process. Skip cleanly and let the mount proceed
    // without the registry fix (icon may be stale, console stays alive).
    if (etahen_present()) {
        wdg_fn_call((void*)sceAppInstUtilInitialize, NULL, NULL, NULL, NULL, 30000);
        char *tid = strdup(title_id);   // leaked if the call parks — by design
        int urc = wdg_fn_call((void*)sceAppInstUtilAppUnInstall, (void*)tid, NULL, NULL, NULL, 60000);
        if (urc != -2) free(tid);
    } else {
        char chk[128];
        snprintf(chk, sizeof(chk), "%s: etaHEN absent — skipping daemon unregister", title_id);
        send_progress_message(chk);
    }

    // Step 2: Update trophy and sound data (dump_installer: lines 413-414)
    update_trophy(title_id, src_sce_sys);
    update_snd0info(title_id);

    // Step 3: Create directories (dump_installer: lines 426-430)
    snprintf(user_app_dir, sizeof(user_app_dir),
             "/user/app/%s", title_id);
    char user_sce_sys[PATH_MAX];
    snprintf(user_sce_sys, sizeof(user_sce_sys),
             "%s/sce_sys", user_app_dir);

    mkdir(user_app_dir, 0755);
    mkdir(user_sce_sys, 0755);

    // Step 4: Copy files (dump_installer: lines 433-435)
    {
        char chk[128];
        snprintf(chk, sizeof(chk), "%s: copying sce_sys files...", title_id);
        send_progress_message(chk);
    }
    copy_dir(src_sce_sys, user_sce_sys);
    copy_sce_sys_to_appmeta(src_sce_sys, title_id);

    // Step 5: Install app (dump_installer: line 439)
    // This calls install_app which tries AppInstallTitleDir first,
    // then falls back to AppInstallAll for FW 12.00+
    {
        char chk[128];
        snprintf(chk, sizeof(chk), "%s: installing...", title_id);
        send_progress_message(chk);
    }
    int reg_rc = register_title(title_id, game_path);
    {
        char chk[128];
        snprintf(chk, sizeof(chk), "%s: registration result=%d", title_id, reg_rc);
        send_progress_message(chk);
    }
    if (reg_rc == -2 || reg_rc == -3 || reg_rc == -4) {
        send_notification(reg_rc == -4
            ? "Mount: etaHEN not running — daemon IPC unavailable\nGame mounted without home-screen entry"
            : "Mount: registration unavailable (loader limits)\nGame mounted without home-screen entry");
    } else if (reg_rc != 0) {
        // v6.1.2: registration failed — the mount is NOT usable (there is no
        // second entry point that ever re-mounts). Clean up in the PROVEN
        // order (unmount → unregister → delete) and report a real failure.
        full_title_cleanup(title_id, NULL);
        return -1;
    }

    // Write mount.lnk AFTER registration to track the source path
    snprintf(mount_lnk_path, sizeof(mount_lnk_path), 
             "/user/app/%s/mount.lnk", title_id);

    FILE* lnk_file = fopen(mount_lnk_path, "w");
    if (lnk_file) {
        fprintf(lnk_file, "%s", game_path);
        fclose(lnk_file);
    }

    track_mounted_title(title_id);
    return 0;
}

// Forward declaration for rmdir_recursive (defined later in file)
int rmdir_recursive(const char *path);

// ---------------- AUTO UNMOUNT DELETED GAMES ----------------
static int auto_unmount_deleted_games(void) {
    DIR* d = opendir("/user/app");
    if (!d) return 0;

    int unmounted = 0;
    struct dirent* e;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;

        if ((strncmp(e->d_name, "CUSA", 4) != 0 && 
             strncmp(e->d_name, "PPSA", 4) != 0) || 
            strlen(e->d_name) != 9)
            continue;

        char mount_lnk[PATH_MAX];
        snprintf(mount_lnk, sizeof(mount_lnk), "/user/app/%s/mount.lnk", e->d_name);

        FILE* f = fopen(mount_lnk, "r");
        if (!f) continue;

        char game_path[PATH_MAX] = {};
        if (fgets(game_path, sizeof(game_path), f)) {
            game_path[strcspn(game_path, "\r\n")] = '\0';
            fclose(f);

            struct stat st;
            if (stat(game_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                char system_ex_app[PATH_MAX];                snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", e->d_name);
                
                if (is_mounted(system_ex_app)) {
                    unmount_guarded(system_ex_app, 0);
                }

                // Registry row first (background), then dirs — same model as
                // full_title_cleanup: without AppUnInstall the dead icon
                // stays on the home screen forever.
                unregister_title_async(e->d_name);
                {
                    char ua[PATH_MAX], am[PATH_MAX];
                    snprintf(ua, sizeof(ua), "/user/app/%s", e->d_name);
                    snprintf(am, sizeof(am), "/user/appmeta/%s", e->d_name);
                    rmdir_recursive(ua);
                    rmdir_recursive(am);
                }

                unmounted++;
            }
        } else {
            fclose(f);
        }
    }

    closedir(d);
    return unmounted;
}

// ============================================================================
// GAME MOUNTING FUNCTIONS - END
// ============================================================================

typedef struct {
    int sock;
    int upload_fd;  // File descriptor for direct write (faster than FILE*)
    pthread_mutex_t *file_mutex;  // Per-file mutex
    char upload_path[MAX_PATH];
    uint64_t upload_size;
    uint64_t upload_received;
    uint64_t current_offset;  // Current write offset for pwrite() - set by START_UPLOAD
    // Shell session
    FILE *shell_pipe;
    pid_t shell_pid;
    bool shell_active;
    char shell_cwd[MAX_PATH];
    bool upload_aborted;  // Set when a chunk write fails - server then rejects further chunks
} client_session_t;

// Cap concurrent client sessions: each thread allocates an 8MB command
// buffer, so unbounded connection storms could exhaust PS5 memory. The
// desktop client uses up to 24 parallel uploads, so the cap sits above that
// (same worst-case memory as v6.0) and only rejects real connection floods.
#define MAX_CLIENT_SESSIONS 96  // 28 task conns + up to 16 chunk workers + UI needs headroom
static int g_active_sessions = 0;

// Filesystem index entry (in-memory, no SQLite for simplicity).
// Paths/names are heap-allocated with just enough room: a fixed MAX_PATH+name
// struct cost ~2.3KB per entry which exceeded 450MB RAM on 200k-file consoles.
typedef struct index_entry {
    char *path;
    char *name;
    uint64_t size;
    time_t mtime;
    bool is_dir;
    struct index_entry *next;
} index_entry_t;

// Index state
typedef struct {
    index_entry_t *entries;
    int total_files;
    int total_dirs;
    bool indexing;
    bool ready;
    pthread_mutex_t mutex;
    pthread_t thread;
} index_state_t;

static index_state_t g_index = {0};

// Disk write job for queue
typedef struct write_job {
    uint8_t *data;
    size_t len;
    FILE *fp;
    struct write_job *next;
} write_job_t;

// Job queue (producer-consumer pattern)
typedef struct {
    write_job_t *head;
    write_job_t *tail;
    size_t count;
    size_t max;
    int closed;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} job_queue_t;

// Global queue and workers
static job_queue_t g_queue;
static pthread_t g_workers[DISK_WORKER_COUNT];
static int g_workers_initialized = 0;

// Queue operations
void queue_init(job_queue_t *q, size_t max) {
    memset(q, 0, sizeof(*q));
    q->max = max;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

int queue_push(job_queue_t *q, write_job_t *job) {
    pthread_mutex_lock(&q->mutex);
    while (!q->closed && q->count >= q->max) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }
    if (q->closed) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    job->next = NULL;
    if (!q->tail) {
        q->head = job;
        q->tail = job;
    } else {
        q->tail->next = job;
        q->tail = job;
    }
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

write_job_t *queue_pop(job_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    while (!q->closed && q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }
    if (q->count == 0 && q->closed) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    write_job_t *job = q->head;
    q->head = job->next;
    if (!q->head) {
        q->tail = NULL;
    }
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return job;
}

// Disk worker thread
void *disk_worker(void *arg) {
    (void)arg;
    while (1) {
        write_job_t *job = queue_pop(&g_queue);
        if (!job) break;
        
        if (job->fp && job->data && job->len > 0) {
            fwrite(job->data, 1, job->len, job->fp);
            // No fflush here - let setvbuf handle buffering
        }
        
        free(job->data);
        free(job);
    }
    return NULL;
}

void init_workers() {
    if (g_workers_initialized) return;
    
    queue_init(&g_queue, QUEUE_MAX_SIZE);
    for (int i = 0; i < DISK_WORKER_COUNT; i++) {
        pthread_create(&g_workers[i], NULL, disk_worker, NULL);
    }
    g_workers_initialized = 1;
}

// Reliable send - loops until all bytes are sent or error
static ssize_t send_all(int sock, const void *buf, size_t len) {
    const uint8_t *ptr = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, ptr + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return (ssize_t)sent;
}

// Send response - combined header+data in single send for speed
void send_response(int sock, uint8_t response, const void *data, uint32_t data_len) {
    // Combine header and data into single buffer for single send()
    size_t total_len = 5 + data_len;
    uint8_t *combined = malloc(total_len);
    if (!combined) {
        // Fallback to separate sends
        uint8_t header[5];
        header[0] = response;
        memcpy(header + 1, &data_len, 4);
        send_all(sock, header, 5);
        if (data && data_len > 0) {
            send_all(sock, data, data_len);
        }
        return;
    }
    
    combined[0] = response;
    memcpy(combined + 1, &data_len, 4);
    if (data && data_len > 0) {
        memcpy(combined + 5, data, data_len);
    }
    
    send_all(sock, combined, total_len);
    free(combined);
}

// Send OK response
void send_ok(int sock, const char *msg) {
    uint32_t len = msg ? strlen(msg) : 0;
    send_response(sock, RESP_OK, msg, len);
}

// Send error response
void send_error(int sock, const char *msg) {
    uint32_t len = msg ? strlen(msg) : 0;
    send_response(sock, RESP_ERROR, msg, len);
}

// System load averages (1m, 5m, 15m) via the getloadavg() libc call
// (available in the ps5-payload-sdk since v0.39). Returns 0 on success.
static int get_load_averages(double loads[3]) {
    return getloadavg(loads, 3);
}

// ============================================================================
// CRC32 (standard reflected polynomial 0xEDB88320) for file verification
// ============================================================================
static uint32_t g_crc_table[256];
static int g_crc_table_ready = 0;

static uint32_t crc32_compute(const uint8_t *data, size_t len, uint32_t crc) {
    for (size_t i = 0; i < len; i++)
        crc = g_crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

// Fast CRC of an entire file (streamed, low memory)
static int file_crc32(const char *path, uint32_t *out_crc, uint64_t *out_size) {
    if (!g_crc_table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            g_crc_table[i] = c;
        }
        g_crc_table_ready = 1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    // 256KB on the stack overflows the session thread's stack and kills the
    // whole process (VERIFY_FILE crash) — use a heap buffer instead.
    uint8_t *buf = malloc(256 * 1024);
    if (!buf) { close(fd); return -1; }

    uint32_t crc = 0xFFFFFFFFu;
    uint64_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf, 256 * 1024)) > 0) {
        crc = crc32_compute(buf, (size_t)n, crc);
        total += (uint64_t)n;
    }
    free(buf);
    close(fd);
    if (n < 0) return -1;

    *out_crc = crc ^ 0xFFFFFFFFu;
    if (out_size) *out_size = total;
    return 0;
}

// Normalize path by removing double slashes
void normalize_path(char *path) {
    char *src = path;
    char *dst = path;
    int prev_slash = 0;
    
    while (*src) {
        if (*src == '/') {
            if (!prev_slash) {
                *dst++ = *src;
            }
            prev_slash = 1;
        } else {
            *dst++ = *src;
            prev_slash = 0;
        }
        src++;
    }
    *dst = '\0';
}

// Recursive directory creation
int mkdir_recursive(const char *path) {
    char tmp[MAX_PATH];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    normalize_path(tmp);  // Remove any double slashes
    len = strlen(tmp);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                return -1;
            }
            chmod(tmp, 0777);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        return -1;
    }
    chmod(tmp, 0777);
    return 0;
}

// Global deletion progress counter (protected by g_progress_mutex)
static int g_delete_count = 0;
static int g_total_files = 0;
static time_t g_last_notify = 0;

// Global scan counter for progress updates (reset before each scan)
static int g_scan_count = 0;
static time_t g_last_scan_notify = 0;

// Count files in directory recursively with progress updates
int count_files_recursive(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    char child[MAX_PATH];
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            count++;  // Count the directory itself
            count += count_files_recursive(child);  // Count its contents
        } else {
            count++;
            g_scan_count++;
            
            // Send progress every 500 files or every 3 seconds during counting
            time_t now = time(NULL);
            if (g_scan_count % 500 == 0 || (now - g_last_scan_notify) >= 3) {
                char msg[256];
                snprintf(msg, sizeof(msg), "📊 Scanning... found %d files so far", g_scan_count);
                send_progress_message(msg);
                g_last_scan_notify = now;
            }
        }
    }
    closedir(dir);
    return count;
}

// Send progress message to client (thread-safe)
// The client sockets run with NO send timeout (infinite) for bulk transfers —
// so a blocking send_all() here could hold g_progress_mutex forever when the
// client died mid-operation. poll() first: if the socket is not writable
// within 300 ms, drop this message and detach the socket so later messages
// stop trying instantly.
void send_progress_message(const char *msg) {
    pthread_mutex_lock(&g_progress_mutex);
    int s = g_client_sock;
    if (s > 0) {
        struct pollfd pfd;
        pfd.fd = s;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, 300) == 1 && (pfd.revents & POLLOUT)) {
            uint8_t header[5];
            header[0] = RESP_PROGRESS;
            uint32_t len = strlen(msg) + 1;
            memcpy(header + 1, &len, 4);
            if (send_all(s, header, 5) < 0 || send_all(s, msg, len) < 0)
                g_client_sock = 0;
        } else {
            g_client_sock = 0;   // dead or wedged — stop blocking on it
        }
    }
    pthread_mutex_unlock(&g_progress_mutex);
}

// Send a response header (+optional payload) to a socket under the progress
// lock. Ownership of g_client_sock is transferred via claim/release, so a
// recycled fd is never written to by a stale background thread.
static void send_resp_locked(int sock, uint8_t resp, const void *data, uint32_t data_len) {
    pthread_mutex_lock(&g_progress_mutex);
    if (sock > 0 && g_client_sock == sock) {
        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, 300) == 1 && (pfd.revents & POLLOUT)) {
            uint8_t header[5];
            header[0] = resp;
            memcpy(header + 1, &data_len, 4);
            if (send_all(sock, header, 5) < 0 ||
                (data && data_len > 0 && send_all(sock, data, data_len) < 0))
                g_client_sock = 0;
        } else {
            g_client_sock = 0;
        }
    }
    pthread_mutex_unlock(&g_progress_mutex);
}

// Recursive directory deletion with progress reporting
int rmdir_recursive(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }

    struct dirent *entry;
    char child[MAX_PATH];
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            rmdir_recursive(child);
        } else {
            unlink_guarded(child);   // never let a busy vnode hang the delete
            g_delete_count++;
            
            // Send progress every 50 files or every 2 seconds
            time_t now = time(NULL);
            if (g_delete_count % 50 == 0 || (now - g_last_notify) >= 2) {
                int percentage = (g_total_files > 0) ? (g_delete_count * 100 / g_total_files) : 0;
                char msg[256];
                snprintf(msg, sizeof(msg), "🗑️ Deleting... %d/%d files (%d%%)", 
                         g_delete_count, g_total_files, percentage);
                send_progress_message(msg);
                g_last_notify = now;
            }
        }
    }
    closedir(dir);
    return rmdir_guarded(path);
}

// Handle PING
void handle_ping(client_session_t *session) {
    send_ok(session->sock, "PONG");
}

#include <sys/disk.h>

static pthread_mutex_t g_storage_mutex = PTHREAD_MUTEX_INITIALIZER;

// Read the disk's GPT to enumerate ALL partitions — the system partitions
// (update, preinst2, eap, vswap, OS images...) are never mounted in our
// namespace so statfs can't see them, but the GPT always lists them.
// Returns media size; *non_user_out = total size of all partitions except
// the largest one (which is /user); *overhang_out = unallocated space.
static int g_disk_media = 0;  // 0=untried, -1=failed, >0=bytes (cache)
static uint64_t g_non_user_parts = 0, g_disk_overhang = 0, g_media_bytes = 0;

static char g_disk_dbg[160];   // which devices were tried / what failed

// kern.geom.conftxt dumps the whole GEOM topology (every provider and its
// mediasize) through sysctl — no raw /dev access needed. Each line is:
//   <level> <class> <name> <mediasize> <sectorsize> ...
// Level-0 DISK lines are physical disks; deeper PART lines under them are
// the partitions (mounted or not). Returns >0 on success.
static int geom_probe(void) {
    size_t sz = 0;
    if (sysctlbyname("kern.geom.conftxt", NULL, &sz, NULL, 0) != 0 ||
        sz < 64 || sz > (1u << 20))
        return -1;
    char *txt = malloc(sz + 1);
    if (!txt) return -1;
    if (sysctlbyname("kern.geom.conftxt", txt, &sz, NULL, 0) != 0) {
        free(txt);
        return -1;
    }
    txt[sz] = 0;

    // Short signature into the debug field so we can see what came back.
    {
        char sig[44];
        int si = 0;
        for (char *p = txt; *p && si < (int)sizeof(sig) - 1; p++) {
            if (*p == '\n') sig[si++] = '/';
            else if (*p >= ' ' && *p < 127) sig[si++] = *p;
        }
        sig[si] = 0;
        snprintf(g_disk_dbg, sizeof(g_disk_dbg), "GEOM:%s", sig);
    }

    struct { char name[64]; uint64_t media, parts, biggest; int used; } disks[16];
    memset(disks, 0, sizeof(disks));
    int cur = -1;
    for (char *ln = strtok(txt, "\n"); ln; ln = strtok(NULL, "\n")) {
        int lv; char cls[32], nm[64]; unsigned long long ms;
        if (sscanf(ln, "%d %31s %63s %llu", &lv, cls, nm, &ms) < 4) continue;
        if (lv == 0 && !strcmp(cls, "DISK")) {
            for (cur = 0; cur < 16 && disks[cur].used; cur++);
            if (cur < 16) {
                disks[cur].used = 1;
                disks[cur].media = ms;
                strncpy(disks[cur].name, nm, sizeof(disks[cur].name) - 1);
            } else {
                cur = -1;
            }
        } else if (cur >= 0 && lv > 0 && !strcmp(cls, "PART")) {
            disks[cur].parts += ms;
            if (ms > disks[cur].biggest) disks[cur].biggest = ms;
        }
    }
    free(txt);

    // Internal disk = the one holding the biggest partition (that's /user).
    int best = -1;
    for (int i = 0; i < 16; i++)
        if (disks[i].used && (best < 0 || disks[i].biggest > disks[best].biggest))
            best = i;
    if (best < 0 || !disks[best].parts || !disks[best].biggest) return -1;

    strncat(g_disk_dbg, disks[best].name,
            sizeof(g_disk_dbg) - strlen(g_disk_dbg) - 1);
    g_media_bytes    = disks[best].media;
    g_non_user_parts = disks[best].parts - disks[best].biggest;
    g_disk_overhang  = (disks[best].media > disks[best].parts)
                       ? disks[best].media - disks[best].parts : 0;
    g_disk_media = 1;
    return 1;
}

static int disk_gpt_probe(void) {
    if (g_disk_media != 0) return g_disk_media;
    g_disk_media = -1;
    g_disk_dbg[0] = 0;

    // Candidate device names: sysctl kern.disks (real disk list) + guesses
    char devs[24][32]; int ndev = 0;
    {
        char names[256] = {0}; size_t nl = sizeof(names);
        if (sysctlbyname("kern.disks", names, &nl, NULL, 0) != 0) {
            snprintf(g_disk_dbg + strlen(g_disk_dbg),
                     sizeof(g_disk_dbg) - strlen(g_disk_dbg), "K%d", errno);
        } else {
            for (char *t = strtok(names, " "); t && ndev < 20; t = strtok(NULL, " ")) {
                // keep only internal SSD-looking names, skip dm/md/usb-ish
                if (!strncmp(t, "da", 2) || !strncmp(t, "nv", 2) ||
                    !strncmp(t, "ad", 2) || !strncmp(t, "nd", 2) ||
                    !strncmp(t, "mmcsd", 5)) {
                    snprintf(devs[ndev++], 32, "/dev/%s", t);
                    strncat(g_disk_dbg, t, 12); strncat(g_disk_dbg, " ", 1);
                }
            }
        }
    }
    // Orbis devfs: partitions appear as /dev/ssd0.<name> (ssd0.system,
    // ssd0.user, ...) and the whole disk as /dev/ssd0. Enumerate /dev — it is
    // plain devfs, so readdir shows every provider node even when the mounts
    // only reference a few of them.
    {
        DIR *dd = opendir("/dev");
        if (dd) {
            struct dirent *de;
            while ((de = readdir(dd)) && ndev < 24) {
                if (strncmp(de->d_name, "ssd0", 4)) continue;
                snprintf(devs[ndev], 32, "/dev/%s", de->d_name);
                strncat(g_disk_dbg, de->d_name, 12);
                strncat(g_disk_dbg, ",", 1);
                ndev++;
            }
            closedir(dd);
        } else {
            snprintf(g_disk_dbg + strlen(g_disk_dbg),
                     sizeof(g_disk_dbg) - strlen(g_disk_dbg), "D%d", errno);
        }
    }
    const char *guess[] = { "/dev/ssd0", "/dev/ssd0.user",
                            "/dev/da0", "/dev/nvd0", "/dev/sflash0", NULL };
    for (int i = 0; guess[i] && ndev < 24; i++) {
        int dup = 0;
        for (int j = 0; j < ndev; j++) if (!strcmp(devs[j], guess[i])) dup = 1;
        if (!dup) snprintf(devs[ndev++], 32, "%s", guess[i]);
    }

    uint64_t parts_sum = 0, parts_max = 0, disk_media = 0;
    for (int i = 0; i < ndev; i++) {
        int fd = open(devs[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            snprintf(g_disk_dbg + strlen(g_disk_dbg),
                     sizeof(g_disk_dbg) - strlen(g_disk_dbg), "(%d)", errno);
            continue;
        }
        off_t media = 0;
        u_int secsize = 512;
        if (ioctl(fd, DIOCGMEDIASIZE, &media) != 0 || media <= 0) { close(fd); continue; }
        ioctl(fd, DIOCGSECTORSIZE, &secsize);
        // Partition node (ssd0.<name>): account its size directly — the GPT
        // parse below will fail for it, but the sum over all ssd0.* nodes
        // gives the same partition accounting.
        if (strstr(devs[i], "ssd0.")) {
            parts_sum += (uint64_t)media;
            if ((uint64_t)media > parts_max) parts_max = (uint64_t)media;
        } else {
            disk_media = (uint64_t)media;
        }
        uint64_t sectors = (uint64_t)media / secsize;
        // backup GPT header is at the last LBA; entries sit in the 32
        // sectors right before it
        size_t tail = 34 * secsize;
        uint8_t *buf = malloc(tail);
        if (!buf) { close(fd); continue; }
        ssize_t got = pread(fd, buf, tail, (off_t)((sectors - 34) * secsize));
        close(fd);
        if (got != (ssize_t)tail) { free(buf); continue; }
        uint8_t *hdr = buf + 32 * secsize;  // backup header at last sector
        if (memcmp(hdr, "EFI PART", 8) != 0) { free(buf); continue; }
        uint64_t entries_lba, first_lba;
        uint32_t nent, entsz;
        memcpy(&entries_lba, hdr + 72, 8);
        memcpy(&first_lba,  hdr + 40, 8);
        memcpy(&nent,  hdr + 80, 4);
        memcpy(&entsz, hdr + 84, 4);
        if (nent == 0 || nent > 1024 || entsz < 128) { free(buf); continue; }
        uint64_t total_part = 0, biggest = 0;
        uint8_t *ent = buf;  // entries start at sectors-34
        for (uint32_t e = 0; e < nent && (size_t)(e + 1) * entsz <= 32 * secsize; e++) {
            uint8_t *en = ent + (size_t)e * entsz;
            uint64_t type0;
            memcpy(&type0, en, 8);
            if (type0 == 0) continue;   // unused entry
            uint64_t fl, ll;
            memcpy(&fl, en + 32, 8);
            memcpy(&ll, en + 40, 8);
            if (ll < fl) continue;
            uint64_t sz = (ll - fl + 1) * secsize;
            total_part += sz;
            if (sz > biggest) biggest = sz;
        }
        free(buf);
        if (!total_part || !biggest) continue;
        g_non_user_parts = total_part - biggest;
        g_disk_overhang  = ((uint64_t)media > total_part) ? (uint64_t)media - total_part : 0;
        g_disk_media = (int)1;   // success flag (media kept separately below)
        g_media_bytes = (uint64_t)media;
        return 1;
    }
    // No whole-disk GPT — but ssd0.* partition nodes may still have answered
    // DIOCGMEDIASIZE. Sum them; biggest partition is /user.
    if (parts_max) {
        g_media_bytes    = disk_media ? disk_media : parts_sum;
        g_non_user_parts = parts_sum - parts_max;
        g_disk_overhang  = (disk_media > parts_sum) ? disk_media - parts_sum : 0;
        g_disk_media = 1;
        return 1;
    }
    return -1;
}

// The GPT probe does raw /dev open+ioctl+pread which may block forever on
// some device nodes (optical drive, locked disks). Run it on a detached
// thread exactly once; the storage handler reads only the cached result and
// never waits on the probe itself.
static volatile int g_probe_state = 0;  // 0=idle, 1=running, 2=finished
static void *gpt_probe_worker(void *arg) {
    (void)arg;
    if (geom_probe() <= 0)
        disk_gpt_probe();
    g_probe_state = 2;
    return NULL;
}
static void gpt_probe_start_async(void) {
    if (g_probe_state != 0) return;
    g_probe_state = 1;
    pthread_t pt;
    if (pthread_create(&pt, NULL, gpt_probe_worker, NULL) == 0)
        pthread_detach(pt);
    else
        g_probe_state = 0;
}

void handle_list_storage(client_session_t *session) {
    // Static response buffer: handle_get_system_info() reads it from another
    // thread, so concurrent list-storage calls must not clobber each other.
    static char g_storage_response_str[4096];
    pthread_mutex_lock(&g_storage_mutex);
    struct statfs sf;
    
    // PS5 Settings "Console Storage" formula:
    //   Total = (/user total - /user reserved) + /system_data total + /system_ex total
    //   Free  = /user bavail + /system_data bavail + /system_ex bavail
    //
    // Real PS5 example:
    //   /user      total=872.69 GiB, reserved=92.53 GiB, bavail=346.93 GiB
    //   /system_data total=7.98 GiB, bavail=6.83 GiB
    //   /system_ex   total=1.50 GiB, bavail=0.15 GiB
    //   → Total = 780.16 + 7.98 + 1.50 = 789.64 GiB = 847.9 GB (PS5 shows 848 GB) ✓
    
    if (statfs("/user", &sf) != 0) {
        send_error(session->sock, "statfs /user failed");
        return;
    }
    uint64_t u_blksz  = sf.f_bsize;
    uint64_t u_total  = (uint64_t)sf.f_blocks * u_blksz;
    uint64_t u_bfree  = (uint64_t)sf.f_bfree  * u_blksz;
    uint64_t u_bavail = (sf.f_bavail > 0) ? (uint64_t)sf.f_bavail * u_blksz : u_bfree;
    uint64_t u_rsvd   = (u_bfree > u_bavail) ? (u_bfree - u_bavail) : 0;
    uint64_t u_displayable = u_total - u_rsvd;
    
    uint64_t sd_total = 0;
    if (statfs("/system_data", &sf) == 0)
        sd_total = (uint64_t)sf.f_blocks * sf.f_bsize;
    
    uint64_t sx_total = 0;
    if (statfs("/system_ex", &sf) == 0)
        sx_total = (uint64_t)sf.f_blocks * sf.f_bsize;

    // Other visible system partitions — fallback for the GPT probe
    uint64_t vis_sys = 0;
    const char *sys_parts[] = { "/system", "/preinst", "/system_tmp",
        "/system_data/eap/rodata", "/update", "/preinst2", "/safemode",
        "/minvsn_update", NULL };
    for (int i = 0; sys_parts[i]; i++)
        if (statfs(sys_parts[i], &sf) == 0)
            vis_sys += (uint64_t)sf.f_blocks * sf.f_bsize;

    // PS5 "Console Storage" convention, verified against Settings > Storage:
    //   Total = /user(total - minfree) - non-/user partitions
    //   Free  = /user bavail - minfree - non-/user partitions + unallocated
    //   Used  = Total - Free
    // The non-/user partitions (update, preinst2, eap, vswap, OS images)
    // are read from the disk's GPT; fall back to the visible mounts.
    uint64_t nonuser = 0, overhang = 0;
    gpt_probe_start_async();
    if (g_probe_state == 2 && g_disk_media > 0) {
        nonuser  = g_non_user_parts;
        overhang = g_disk_overhang;
    } else {
        nonuser = sd_total + sx_total + vis_sys;
    }

    uint64_t total_bytes = (u_displayable > nonuser) ? u_displayable - nonuser : u_displayable;
    uint64_t free_bytes = 0;
    if (u_bavail > u_rsvd + nonuser - overhang)
        free_bytes = u_bavail - u_rsvd - nonuser + overhang;
    uint64_t used_bytes  = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;
    uint64_t reserved_bytes = u_rsvd + nonuser;

    // Installed games/apps — the REAL figure the PS5 Settings shows comes
    // from app.db tbl_contentinfo.size (bytes per registered title).
    uint64_t games_size = 0;
    {
        sqlite3 *db = NULL;
        if (sqlite3_open("/system_data/priv/mms/app.db", &db) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 2000);
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT COALESCE(SUM(size),0) FROM tbl_contentinfo",
                    -1, &st, NULL) == SQLITE_OK &&
                sqlite3_step(st) == SQLITE_ROW)
                games_size = (uint64_t)sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            sqlite3_close(db);
        }
    }

    // User data = used space not attributed to installed titles (saves,
    // captures, system blobs) — derived, never fabricated.
    uint64_t mounted_games_size = games_size;
    uint64_t user_data_size = (used_bytes > games_size) ? used_bytes - games_size : 0;

    snprintf(g_storage_response_str, sizeof(g_storage_response_str),
             "%llu|%llu|%llu|%llu|%llu|%llu|%llu|%s|",
             (unsigned long long)total_bytes,
             (unsigned long long)free_bytes,
             (unsigned long long)free_bytes,
             (unsigned long long)reserved_bytes,
             (unsigned long long)mounted_games_size,
             (unsigned long long)user_data_size,
             (unsigned long long)free_bytes,
             "Console Storage");
    
    send_response(session->sock, RESP_DATA, g_storage_response_str, strlen(g_storage_response_str));
    pthread_mutex_unlock(&g_storage_mutex);
}

// Handle LIST_DIR - Optimized version using d_type only (no stat for dirs)
void handle_list_dir(client_session_t *session, const char *path) {
    char norm_path[MAX_PATH];
    snprintf(norm_path, sizeof(norm_path), "%s", path);
    normalize_path(norm_path);
    
    DIR *dir = opendir(norm_path);
    if (!dir) {
        int32_t count = 0;
        send_response(session->sock, RESP_DATA, &count, 4);
        return;
    }
    
    // Start at 256KB and GROW if a directory needs more room instead of
    // silently truncating the listing (directories with many/long names).
    size_t buf_size = 256 * 1024;
    uint8_t *buffer = malloc(buf_size);
    if (!buffer) {
        closedir(dir);
        int32_t count = 0;
        send_response(session->sock, RESP_DATA, &count, 4);
        return;
    }
    
    uint8_t *ptr = buffer + 4;
    int32_t entry_count = 0;
    
    struct dirent *entry;
    char full_path[MAX_PATH];
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        uint16_t name_len = (uint16_t)strlen(entry->d_name);
        size_t needed = 1 + 2 + name_len + 8 + 8;
        
        if ((size_t)(ptr - buffer) + needed > buf_size) {
            // Try to grow the buffer (up to 32MB) rather than dropping entries
            if (buf_size >= 32 * 1024 * 1024) {
                break;  // Absurdly large directory - hard cap reached
            }
            size_t used = (size_t)(ptr - buffer);
            size_t new_size = buf_size * 2;
            uint8_t *grown = realloc(buffer, new_size);
            if (!grown) break;
            buffer = grown;
            buf_size = new_size;
            ptr = buffer + used;
        }
        
        // Determine type and get file size
        uint8_t type = 0;
        uint64_t size = 0;
        uint64_t timestamp = 0;
        
        snprintf(full_path, sizeof(full_path), "%s/%s", norm_path, entry->d_name);
        struct stat st;
        
        if (entry->d_type == DT_DIR) {
            type = 1;
        } else if (entry->d_type == DT_UNKNOWN) {
            // d_type not supported on this filesystem - use stat() as fallback
            if (stat(full_path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    type = 1;
                } else {
                    size = st.st_size;
                    timestamp = st.st_mtime;
                }
            }
        } else {
            // Regular file - get size
            if (stat(full_path, &st) == 0) {
                size = st.st_size;
                timestamp = st.st_mtime;
            }
        }
        
        *ptr++ = type;
        memcpy(ptr, &name_len, 2);
        ptr += 2;
        memcpy(ptr, entry->d_name, name_len);
        ptr += name_len;
        memcpy(ptr, &size, 8);
        ptr += 8;
        memcpy(ptr, &timestamp, 8);
        ptr += 8;
        
        entry_count++;
    }
    
    closedir(dir);
    memcpy(buffer, &entry_count, 4);
    send_response(session->sock, RESP_DATA, buffer, (uint32_t)(ptr - buffer));
    free(buffer);
}

// Handle CREATE_DIR
void handle_create_dir(client_session_t *session, const char *path) {
    if (mkdir_recursive(path) == 0) {
        send_ok(session->sock, "Directory created");
    } else {
        send_error(session->sock, "Failed to create directory");
    }
}

// Handle DELETE_FILE
void handle_delete_file(client_session_t *session, const char *path) {
    char normalized_path[MAX_PATH];
    snprintf(normalized_path, sizeof(normalized_path), "%s", path);
    normalize_path(normalized_path);
    
    if (unlink(normalized_path) == 0) {
        send_ok(session->sock, "File deleted");
    } else {
        send_error(session->sock, "Failed to delete file");
    }
}

// Background deletion thread data
typedef struct {
    char path[MAX_PATH];
    int client_sock;
} delete_thread_data_t;

// Background deletion thread
void* delete_thread_func(void* arg) {
    delete_thread_data_t* data = (delete_thread_data_t*)arg;
    
    // Reset ALL progress counters
    g_delete_count = 0;
    g_scan_count = 0;
    g_last_notify = time(NULL);
    g_last_scan_notify = time(NULL);
    progress_socket_set(data->client_sock);
    
    // Count total files first
    char start_msg[256];
    snprintf(start_msg, sizeof(start_msg), "📊 Scanning folder: %s", data->path);
    send_progress_message(start_msg);
    
    g_total_files = count_files_recursive(data->path);
    
    if (g_total_files == 0) {
        char empty_msg[256];
        snprintf(empty_msg, sizeof(empty_msg), "⚠️ Folder is empty or already deleted");
        send_progress_message(empty_msg);
        
        // Still try to delete the empty folder itself
        rmdir(data->path);
        
        // Send final OK response even for empty folders
        send_resp_locked(g_client_sock, RESP_OK, NULL, 0);
        progress_socket_clear(data->client_sock);
        free(data);
        return NULL;
    }
    
    char count_msg[256];
    snprintf(count_msg, sizeof(count_msg), "📊 Total: %d files to delete", g_total_files);
    send_progress_message(count_msg);
    
    // Start deletion
    char del_msg[256];
    snprintf(del_msg, sizeof(del_msg), "🗑️ Starting deletion...");
    send_progress_message(del_msg);
    
    // Perform deletion in background
    int result = rmdir_recursive(data->path);
    
    // Send completion message
    if (result == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "✅ Deleted %d files (100%%)", g_delete_count);
        send_progress_message(msg);
        send_notification(msg);
        
        // Send final OK response to signal completion
        send_resp_locked(g_client_sock, RESP_OK, NULL, 0);
        
        // Wait for data to be sent before cleanup
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 200000000; // 200ms
        nanosleep(&ts, NULL);
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "❌ Failed to delete folder (%d files removed)", g_delete_count);
        send_progress_message(msg);
        
        // Send error response
        send_resp_locked(g_client_sock, RESP_ERROR, NULL, 0);
        
        // Wait for data to be sent before cleanup
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 200000000; // 200ms
        nanosleep(&ts, NULL);
    }
    
    progress_socket_clear(data->client_sock);
    free(data);
    return NULL;
}

// Handle DELETE_DIR - BUG FIX: Async deletion with progress reporting
void handle_delete_dir(client_session_t *session, const char *path) {
    // DO NOT send OK immediately - let background thread handle all responses
    // This prevents "Unexpected response: Data" error
    
    // Create background thread for deletion
    delete_thread_data_t* data = malloc(sizeof(delete_thread_data_t));
    if (data) {
        strncpy(data->path, path, MAX_PATH - 1);
        data->path[MAX_PATH - 1] = '\0';
        data->client_sock = session->sock;
        
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        
        if (pthread_create(&thread, &attr, delete_thread_func, data) != 0) {
            // Thread creation failed, delete synchronously and send response
            free(data);
            progress_socket_set(session->sock);
            int result = rmdir_recursive(path);
            if (result == 0) {
                send_ok(session->sock, "Folder deleted");
            } else {
                send_error(session->sock, "Failed to delete folder");
            }
            progress_socket_clear(session->sock);
        }
        
        pthread_attr_destroy(&attr);
    } else {
        // Malloc failed, delete synchronously and send response
        progress_socket_set(session->sock);
        int result = rmdir_recursive(path);
        if (result == 0) {
            send_ok(session->sock, "Folder deleted");
        } else {
            send_error(session->sock, "Failed to delete folder");
        }
        progress_socket_clear(session->sock);
    }
}

// Handle RENAME
void handle_rename(client_session_t *session, const uint8_t *data, uint32_t data_len) {
    const char *old_path = (const char *)data;
    uint32_t old_len = strlen(old_path);
    if (old_len + 2 > data_len) {
        send_error(session->sock, "Invalid rename request");
        return;
    }
    const char *new_path = (const char *)(data + old_len + 1);
    
    char norm_old[MAX_PATH], norm_new[MAX_PATH];
    snprintf(norm_old, sizeof(norm_old), "%s", old_path);
    snprintf(norm_new, sizeof(norm_new), "%s", new_path);
    normalize_path(norm_old);
    normalize_path(norm_new);
    
    if (rename(norm_old, norm_new) == 0) {
        send_ok(session->sock, "Renamed successfully");
    } else {
        send_error(session->sock, "Failed to rename");
    }
}

// Handle COPY_FILE
void handle_copy_file(client_session_t *session, const uint8_t *data, uint32_t data_len) {
    const char *src = (const char *)data;
    uint32_t src_len = strlen(src);
    if (src_len + 2 > data_len) {
        send_error(session->sock, "Invalid copy request");
        return;
    }
    const char *dst = (const char *)(data + src_len + 1);
    
    char norm_src[MAX_PATH], norm_dst[MAX_PATH];
    snprintf(norm_src, sizeof(norm_src), "%s", src);
    snprintf(norm_dst, sizeof(norm_dst), "%s", dst);
    normalize_path(norm_src);
    normalize_path(norm_dst);
    
    int src_fd = open(norm_src, O_RDONLY);
    if (src_fd < 0) {
        send_error(session->sock, "Cannot open source file");
        return;
    }
    
    int dst_fd = open(norm_dst, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (dst_fd < 0) {
        close(src_fd);
        send_error(session->sock, "Cannot create destination file");
        return;
    }
    
    char *buf = malloc(BUFFER_SIZE);
    if (!buf) {
        close(src_fd);
        close(dst_fd);
        send_error(session->sock, "Memory allocation failed");
        return;
    }
    
    ssize_t n;
    int success = 1;
    while ((n = read(src_fd, buf, BUFFER_SIZE)) > 0) {
        if (write(dst_fd, buf, n) != n) {
            success = 0;
            break;
        }
    }
    
    free(buf);
    close(src_fd);
    close(dst_fd);
    chmod(norm_dst, 0777);
    
    if (success) {
        send_ok(session->sock, "File copied");
    } else {
        send_error(session->sock, "Failed to copy file");
    }
}

// Handle MOVE_FILE
void handle_move_file(client_session_t *session, const uint8_t *data, uint32_t data_len) {
    const char *src = (const char *)data;
    uint32_t src_len = strlen(src);
    if (src_len + 2 > data_len) {
        send_error(session->sock, "Invalid move request");
        return;
    }
    const char *dst = (const char *)(data + src_len + 1);
    
    char norm_src[MAX_PATH], norm_dst[MAX_PATH];
    snprintf(norm_src, sizeof(norm_src), "%s", src);
    snprintf(norm_dst, sizeof(norm_dst), "%s", dst);
    normalize_path(norm_src);
    normalize_path(norm_dst);
    
    if (rename(norm_src, norm_dst) == 0) {
        send_ok(session->sock, "File moved");
    } else {
        send_error(session->sock, "Failed to move file");
    }
}

// Handle START_UPLOAD (with optional chunk offset for parallel upload)
void handle_start_upload(client_session_t *session, const uint8_t *data, uint32_t data_len) {
    if (session->upload_fd >= 0) {
        close(session->upload_fd);
        session->upload_fd = -1;
        // Release previous file mutex to prevent leak
        if (session->file_mutex) {
            release_file_mutex(session->upload_path);
            session->file_mutex = NULL;
        }
    }
    
    // Parse path, size, and optional offset
    const char *path = (const char *)data;
    uint32_t path_len = strlen(path);
    if (path_len + 9 > data_len) {
        send_error(session->sock, "Invalid upload request");
        return;
    }
    
    // Normalize path to remove double slashes
    char norm_path[MAX_PATH];
    snprintf(norm_path, sizeof(norm_path), "%s", path);
    normalize_path(norm_path);
    
    uint64_t file_size;
    memcpy(&file_size, data + path_len + 1, 8);
    
    // Check for optional offset (for chunked parallel upload)
    uint64_t chunk_offset = 0;
    if (path_len + 17 <= data_len) {
        memcpy(&chunk_offset, data + path_len + 9, 8);
    }
    
    // Create parent directories
    char parent[MAX_PATH];
    strncpy(parent, norm_path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *last_slash = strrchr(parent, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir_recursive(parent);
    }
    
    // Get per-file mutex for this specific file
    session->file_mutex = get_file_mutex(norm_path);
    if (!session->file_mutex) {
        send_error(session->sock, "Cannot allocate file mutex");
        return;
    }
    
    // CRITICAL: Lock mutex BEFORE opening file to prevent race condition
    // when multiple threads try to create the same file simultaneously
    pthread_mutex_lock(session->file_mutex);
    
    // Open file for writing using direct syscalls (faster than FILE*)
    // For chunked uploads, we need to pre-allocate the file on first chunk
    if (chunk_offset > 0) {
        // Subsequent chunk: open existing file
        session->upload_fd = open(norm_path, O_WRONLY);
        if (session->upload_fd >= 0) {
            // Seek to chunk offset
            lseek(session->upload_fd, chunk_offset, SEEK_SET);
        }
    } else {
        // First chunk or small file: create new file
        session->upload_fd = open(norm_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
        if (session->upload_fd >= 0 && file_size > 100 * 1024 * 1024) {
            // Large file - pre-allocate full size for chunked upload
            if (lseek(session->upload_fd, file_size - 1, SEEK_SET) < 0 || write(session->upload_fd, "", 1) != 1) {
                // Pre-allocation failed - likely disk full
                close(session->upload_fd);
                session->upload_fd = -1;
                pthread_mutex_unlock(session->file_mutex);
                release_file_mutex(norm_path);
                session->file_mutex = NULL;
                unlink(norm_path); // Remove partial file
                send_error(session->sock, "Disk full - cannot pre-allocate file");
                return;
            }
            
            // Seek back to beginning
            lseek(session->upload_fd, 0, SEEK_SET);
        }
    }
    
    pthread_mutex_unlock(session->file_mutex);
    
    if (session->upload_fd < 0) {
        int err = errno;
        release_file_mutex(norm_path);
        session->file_mutex = NULL;
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "Cannot create file: errno=%d (%s)", err, strerror(err));
        send_error(session->sock, ebuf);
        return;
    }
    
    strncpy(session->upload_path, norm_path, sizeof(session->upload_path) - 1);    session->upload_size = file_size;
    session->upload_received = chunk_offset;
    session->current_offset = chunk_offset;  // Set write offset for pwrite()
    session->upload_aborted = false;
    
    // Increase socket receive buffer for this upload session
    int huge_buf = 16 * 1024 * 1024; // 16MB receive buffer - matches download optimization
    setsockopt(session->sock, SOL_SOCKET, SO_RCVBUF, &huge_buf, sizeof(huge_buf));
    
    send_response(session->sock, RESP_READY, NULL, 0);
}

// Handle UPLOAD_CHUNK
void handle_upload_chunk(client_session_t *session, const uint8_t *data, uint32_t data_len) {
    if (session->upload_fd < 0 || !session->file_mutex) {
        send_error(session->sock, "No upload in progress");
        return;
    }
    
    // After a failed write we are desynced (the client already pushed bytes
    // it thinks we consumed). Fail fast and hard-close the connection so both
    // sides resynchronize with a fresh socket instead of garbage.
    if (session->upload_aborted) {
        send_error(session->sock, "Upload aborted (previous write failed)");
        close(session->sock);
        session->sock = -1;
        return;
    }
    
    // Use pwrite() for TRUE PARALLEL WRITES - no mutex needed!
    // pwrite() is thread-safe and writes to specific offset without moving file position
    // This allows multiple connections to write different chunks simultaneously
    ssize_t written = pwrite(session->upload_fd, data, data_len, session->current_offset);
    
    if (written != data_len) {
        int err = errno;
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "Write failed: errno=%d (%s)", err, strerror(err));
        send_error(session->sock, ebuf);
        close(session->upload_fd);
        session->upload_fd = -1;
        release_file_mutex(session->upload_path);
        session->file_mutex = NULL;
        session->upload_aborted = true;
        return;
    }
    
    // Update current offset for next write
    session->current_offset += written;
    
    // Update total received bytes for progress tracking
    session->upload_received += written;
    
    // No response - zero blocking for maximum speed
}

// Handle END_UPLOAD
void handle_end_upload(client_session_t *session) {
    if (session->upload_fd < 0) {
        send_error(session->sock, "No upload in progress");
        return;
    }
    
    // Direct close syscall (no buffering to flush)
    close(session->upload_fd);
    session->upload_fd = -1;
    
    if (session->file_mutex) {
        release_file_mutex(session->upload_path);
        session->file_mutex = NULL;
    }
    
    chmod(session->upload_path, 0777);
    
    send_ok(session->sock, "Upload complete");
}

// Handle DOWNLOAD_FILE
void handle_download_file(client_session_t *session, const char *path) {
    char norm_path[MAX_PATH];
    snprintf(norm_path, sizeof(norm_path), "%s", path);
    normalize_path(norm_path);
    
    int fd = open(norm_path, O_RDONLY);
    if (fd < 0) {
        send_error(session->sock, "Cannot open file");
        return;
    }
    
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        send_error(session->sock, "Cannot stat file");
        return;
    }
    
    // Send file size first
    uint64_t file_size = st.st_size;
    send_response(session->sock, RESP_DATA, &file_size, sizeof(file_size));
    
    // Manual read/write loop for maximum sustained throughput
    // FreeBSD sendfile has TCP congestion issues with large files
    // Shared 16MB read buffer, allocated once (was per-call malloc before).
    static uint8_t *g_dl_buf = NULL;
    if (!g_dl_buf) {
        g_dl_buf = malloc(BUFFER_SIZE);
        if (!g_dl_buf) {
            close(fd);
            send_error(session->sock, "Out of memory");
            return;
        }
    }
    
    ssize_t n;
    while ((n = read(fd, g_dl_buf, BUFFER_SIZE)) > 0) {
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t s = send(session->sock, g_dl_buf + sent, n - sent, 0);
            if (s <= 0) {
                close(fd);
                return;
            }
            sent += s;
        }
    }
    
    close(fd);
}

// Handle SHELL_OPEN - Initialize shell session
// ============================================================================
// FILESYSTEM INDEXING SYSTEM
// ============================================================================

// Add entry to index
static char *index_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char*)malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

void index_add_entry(const char *path, const char *name, uint64_t size, time_t mtime, bool is_dir) {
    index_entry_t *entry = (index_entry_t*)malloc(sizeof(index_entry_t));
    if (!entry) return;
    entry->path = index_strdup(path);
    entry->name = index_strdup(name);
    if (!entry->path || !entry->name) {
        free(entry->path);
        free(entry->name);
        free(entry);
        return;
    }
    entry->size = size;
    entry->mtime = mtime;
    entry->is_dir = is_dir;
    
    pthread_mutex_lock(&g_index.mutex);
    entry->next = g_index.entries;
    g_index.entries = entry;
    if (is_dir) {
        g_index.total_dirs++;
    } else {
        g_index.total_files++;
    }
    pthread_mutex_unlock(&g_index.mutex);
}

// Clear index
void index_clear() {
    pthread_mutex_lock(&g_index.mutex);
    index_entry_t *entry = g_index.entries;
    while (entry) {
        index_entry_t *next = entry->next;
        free(entry->path);
        free(entry->name);
        free(entry);
        entry = next;
    }
    g_index.entries = NULL;
    g_index.total_files = 0;
    g_index.total_dirs = 0;
    pthread_mutex_unlock(&g_index.mutex);
}

// Recursive filesystem scan
void index_scan_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        // Log error but continue
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char fullpath[MAX_PATH];
        // Handle root path correctly (avoid double slash)
        if (strcmp(path, "/") == 0) {
            snprintf(fullpath, sizeof(fullpath), "/%s", entry->d_name);
        } else {
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
        }
        
        struct stat st;
        if (stat(fullpath, &st) == 0) {
            bool is_dir = S_ISDIR(st.st_mode);
            index_add_entry(fullpath, entry->d_name, st.st_size, st.st_mtime, is_dir);
            
            // Recurse into subdirectories (skip special dirs)
            if (is_dir) {
                // Skip problematic directories that may cause hangs
                if (strcmp(entry->d_name, "dev") != 0 &&
                    strcmp(entry->d_name, "proc") != 0 &&
                    strcmp(entry->d_name, "sys") != 0) {
                    index_scan_directory(fullpath);
                }
            }
        }
    }
    closedir(dir);
}

// Indexing thread
void* index_thread_func(void* arg) {
    const char **paths = (const char**)arg;
    
    pthread_mutex_lock(&g_index.mutex);
    g_index.indexing = true;
    g_index.ready = false;
    pthread_mutex_unlock(&g_index.mutex);
    
    // Clear old index
    index_clear();
    
    // Scan all provided paths
    for (int i = 0; paths[i] != NULL; i++) {
        index_scan_directory(paths[i]);
    }
    
    pthread_mutex_lock(&g_index.mutex);
    g_index.indexing = false;
    g_index.ready = true;
    pthread_mutex_unlock(&g_index.mutex);
    
    // Free individual strdup'd path strings, then the array itself
    for (int i = 0; paths[i] != NULL; i++) {
        free((void*)paths[i]);
    }
    free(paths);
    return NULL;
}

// Case-insensitive character comparison
static inline char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

// Simple wildcard matching (supports * and ?) - case insensitive
bool wildcard_match(const char *pattern, const char *str) {
    // Collapse consecutive '*' so patterns like "*a*a*a*a*" cannot blow the
    // recursion up exponentially (a hostile/typo query used to freeze the
    // client-handler thread).
    while (*pattern && *str) {
        if (*pattern == '*') {
            while (pattern[1] == '*') pattern++;
            pattern++;
            if (!*pattern) return true;
            while (*str) {
                if (wildcard_match(pattern, str)) return true;
                str++;
            }
            return false;
        } else if (*pattern == '?' || to_lower(*pattern) == to_lower(*str)) {
            pattern++;
            str++;
        } else {
            return false;
        }
    }
    while (*pattern == '*') pattern++;  // trailing stars match empty
    return *pattern == '\0' && *str == '\0';
}

// Parse size filter (e.g., ">1GB", "<100MB")
bool parse_size_filter(const char *filter, int64_t *min_size, int64_t *max_size) {
    if (!filter || strlen(filter) == 0) return false;
    
    char op = filter[0];
    if (op != '>' && op != '<') return false;
    
    char *endptr;
    double value = strtod(filter + 1, &endptr);
    if (endptr == filter + 1) return false;
    
    // Parse unit (KB, MB, GB)
    int64_t multiplier = 1;
    if (strcasecmp(endptr, "KB") == 0) {
        multiplier = 1024;
    } else if (strcasecmp(endptr, "MB") == 0) {
        multiplier = 1024 * 1024;
    } else if (strcasecmp(endptr, "GB") == 0) {
        multiplier = 1024 * 1024 * 1024;
    }
    
    int64_t size = (int64_t)(value * multiplier);
    
    if (op == '>') {
        *min_size = size;
    } else {
        *max_size = size;
    }
    
    return true;
}

// Search index with query
void handle_search_index(client_session_t *session, const char *query) {
    if (!g_index.ready) {
        send_error(session->sock, "Index not ready. Start indexing first.");
        return;
    }
    
    // Parse query: "*.pkg size:>1GB"
    char name_pattern[256];
    int64_t min_size = 0;
    int64_t max_size = INT64_MAX;
    bool has_pattern = false;
    
    // Simple query parser
    char query_copy[1024];
    strncpy(query_copy, query, sizeof(query_copy) - 1);
    query_copy[sizeof(query_copy) - 1] = '\0';
    
    char *token = strtok(query_copy, " ");
    while (token) {
        if (strncmp(token, "size:", 5) == 0) {
            parse_size_filter(token + 5, &min_size, &max_size);
        } else {
            strncpy(name_pattern, token, sizeof(name_pattern) - 1);
            name_pattern[sizeof(name_pattern) - 1] = '\0';
            has_pattern = true;
        }
        token = strtok(NULL, " ");
    }
    
    // If no pattern provided, default to "*" (match all)
    if (!has_pattern) {
        strcpy(name_pattern, "*");
    }
    
    // Search through index.
    // Copy matching entries into a small array under the lock, then send them
    // AFTER releasing it: blocking send() calls under the mutex used to freeze
    // the indexing thread mid-scan.
    typedef struct {
        const char *path;
        const char *name;
        uint64_t size;
        time_t mtime;
        uint8_t is_dir;
    } search_hit_t;
    
    search_hit_t *hits = (search_hit_t*)malloc(sizeof(search_hit_t) * 1000);
    if (!hits) {
        send_error(session->sock, "Out of memory");
        return;
    }
    int result_count = 0;
    
    pthread_mutex_lock(&g_index.mutex);
    
    index_entry_t *entry = g_index.entries;
    while (entry && result_count < 1000) {  // Limit to 1000 results
        // Match name pattern (search in both name and full path)
        bool name_match = wildcard_match(name_pattern, entry->name);
        bool path_match = wildcard_match(name_pattern, entry->path);
        
        if (!name_match && !path_match) {
            entry = entry->next;
            continue;
        }
        
        // Match size filter
        if (entry->size < min_size || entry->size > max_size) {
            entry = entry->next;
            continue;
        }
        
        hits[result_count].path   = entry->path;
        hits[result_count].name   = entry->name;
        hits[result_count].size   = entry->size;
        hits[result_count].mtime  = entry->mtime;
        hits[result_count].is_dir = entry->is_dir ? 1 : 0;
        result_count++;
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_index.mutex);
    
    // Send results outside the lock (pointers stay valid: index is only
    // cleared when re-indexing starts, and a fresh search re-reads the list).
    for (int i = 0; i < result_count; i++) {
        uint8_t resp = RESP_DATA;
        send(session->sock, &resp, 1, 0);
        
        uint32_t path_len = (uint32_t)strlen(hits[i].path);
        uint32_t name_len = (uint32_t)strlen(hits[i].name);
        
        send(session->sock, &path_len, 4, 0);
        send(session->sock, hits[i].path, path_len, 0);
        send(session->sock, &name_len, 4, 0);
        send(session->sock, hits[i].name, name_len, 0);
        send(session->sock, &hits[i].size, 8, 0);
        send(session->sock, &hits[i].mtime, 8, 0);
        send(session->sock, &hits[i].is_dir, 1, 0);
    }
    
    free(hits);
    
    char msg[128];
    snprintf(msg, sizeof(msg), "Found %d results", result_count);
    send_ok(session->sock, msg);
}

// Start indexing
void handle_index_start(client_session_t *session, const char *paths_str) {
    if (g_index.indexing) {
        send_error(session->sock, "Indexing already in progress");
        return;
    }
    
    // Parse paths (comma-separated)
    const char **paths = (const char**)malloc(sizeof(char*) * 16);
    int path_count = 0;
    
    char paths_copy[1024];
    strncpy(paths_copy, paths_str, sizeof(paths_copy) - 1);
    paths_copy[sizeof(paths_copy) - 1] = '\0';
    
    char *token = strtok(paths_copy, ",");
    while (token && path_count < 15) {
        // Trim whitespace
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';
        
        paths[path_count++] = strdup(token);
        token = strtok(NULL, ",");
    }
    paths[path_count] = NULL;
    
    // Start indexing thread
    if (pthread_create(&g_index.thread, NULL, index_thread_func, paths) != 0) {
        send_error(session->sock, "Failed to start indexing thread");
        for (int i = 0; i < path_count; i++) {
            free((void*)paths[i]);
        }
        free(paths);
        return;
    }
    
    pthread_detach(g_index.thread);
    send_ok(session->sock, "Indexing started");
}

// Get index status
void handle_index_status(client_session_t *session) {
    pthread_mutex_lock(&g_index.mutex);
    
    char status[256];
    if (g_index.indexing) {
        snprintf(status, sizeof(status), "Indexing: %d files, %d dirs", 
                 g_index.total_files, g_index.total_dirs);
    } else if (g_index.ready) {
        snprintf(status, sizeof(status), "Ready: %d files, %d dirs indexed", 
                 g_index.total_files, g_index.total_dirs);
    } else {
        snprintf(status, sizeof(status), "Not started");
    }
    
    pthread_mutex_unlock(&g_index.mutex);
    
    send_ok(session->sock, status);
}

// ============================================================================
// GAME MOUNTING HANDLER
// ============================================================================

// ============================================================================
// FILE INFO / SYSTEM INFO / FILE VERIFICATION (commands 0x31/0x32/0x33)
// ============================================================================

// Forward declarations: sysctl helpers live in the hardware section below
static int sysctl_get_string(const char *name, char *buf, size_t buflen);
static int sysctl_get_uint64(const char *name, uint64_t *val);

// Handle GET_FILE_INFO (0x31) - response: size|mtime|atime|mode|is_dir|is_link
void handle_get_file_info(client_session_t *session, const char *path) {
    char norm_path[MAX_PATH];
    snprintf(norm_path, sizeof(norm_path), "%s", path ? path : "");
    normalize_path(norm_path);

    struct stat st;
    if (stat(norm_path, &st) != 0) {
        send_error(session->sock, "Cannot stat file");
        return;
    }

    struct stat lst;
    int is_link = (lstat(norm_path, &lst) == 0 && S_ISLNK(lst.st_mode)) ? 1 : 0;

    char response[256];
    snprintf(response, sizeof(response), "%lld|%lld|%lld|%o|%d|%d",
             (long long)st.st_size,
             (long long)st.st_mtime,
             (long long)st.st_atime,
             (unsigned)(st.st_mode & 0777),
             S_ISDIR(st.st_mode) ? 1 : 0,
             is_link);
    send_response(session->sock, RESP_DATA, response, strlen(response));
}

// Handle GET_SYSTEM_INFO (0x32) - key=value lines consumed by both clients
void handle_get_system_info(client_session_t *session) {
    char response[1024];
    int off = 0;

    char hostname[128] = {0};
    sysctl_get_string("kern.hostname", hostname, sizeof(hostname));
    if (!hostname[0]) snprintf(hostname, sizeof(hostname), "PS5");

    uint64_t physmem = 0;
    sysctl_get_uint64("hw.physmem", &physmem);
    if (physmem == 0) physmem = (uint64_t)sceKernelGetDirectMemorySize();

    struct timeval boottime;
    size_t bt_len = sizeof(boottime);
    long uptime = 0;
    if (sysctlbyname("kern.boottime", &boottime, &bt_len, NULL, 0) == 0 && boottime.tv_sec > 0) {
        struct timeval now;
        gettimeofday(&now, NULL);
        if (now.tv_sec > boottime.tv_sec) uptime = now.tv_sec - boottime.tv_sec;
    }

    // Storage totals - reuse handle_list_storage() aggregation
    uint64_t total_bytes = 0, free_bytes = 0;
    pthread_mutex_lock(&g_storage_mutex);
    struct statfs sf;
    if (statfs("/user", &sf) == 0) {
        uint64_t u_blksz  = sf.f_bsize;
        uint64_t u_total  = (uint64_t)sf.f_blocks * u_blksz;
        uint64_t u_bfree  = (uint64_t)sf.f_bfree  * u_blksz;
        uint64_t u_bavail = (sf.f_bavail > 0) ? (uint64_t)sf.f_bavail * u_blksz : u_bfree;
        total_bytes += u_total - ((u_bfree > u_bavail) ? (u_bfree - u_bavail) : 0);
        free_bytes  += u_bavail;
    }
    if (statfs("/system_data", &sf) == 0)
        free_bytes += (sf.f_bavail > 0) ? (uint64_t)sf.f_bavail * sf.f_bsize : (uint64_t)sf.f_bfree * sf.f_bsize;
    if (statfs("/system_ex", &sf) == 0)
        free_bytes += (sf.f_bavail > 0) ? (uint64_t)sf.f_bavail * sf.f_bsize : (uint64_t)sf.f_bfree * sf.f_bsize;

    int mounted_count = 0;
    lock_mounted_ids();
    mounted_count = g_mounted_count;
    unlock_mounted_ids();

    pthread_mutex_lock(&g_index.mutex);
    int idx_files = g_index.total_files;
    int idx_dirs  = g_index.total_dirs;
    bool idx_ready = g_index.ready;
    pthread_mutex_unlock(&g_index.mutex);
    pthread_mutex_unlock(&g_storage_mutex);

    off += snprintf(response + off, sizeof(response) - off,
        "hostname=%s\n"
        "server_version=" SUITE_VERSION "\n"
        "protocol_version=1\n"
        "total_memory=%llu\n"
        "storage_total=%llu\n"
        "storage_free=%llu\n"
        "mounted_games=%d\n"
        "index_ready=%d\n"
        "index_files=%d\n"
        "index_dirs=%d\n"
        "server_uptime=%ld\n",
        hostname,
        (unsigned long long)physmem,
        (unsigned long long)total_bytes,
        (unsigned long long)free_bytes,
        mounted_count,
        idx_ready ? 1 : 0,
        idx_files,
        idx_dirs,
        uptime);

    send_response(session->sock, RESP_DATA, response, off);
}

// Handle VERIFY_FILE (0x33) - response: CRC32|size (8-char hex, compatible with client)
void handle_verify_file(client_session_t *session, const char *path) {
    char norm_path[MAX_PATH];
    snprintf(norm_path, sizeof(norm_path), "%s", path ? path : "");
    normalize_path(norm_path);

    uint32_t crc = 0;
    uint64_t size = 0;
    if (file_crc32(norm_path, &crc, &size) != 0) {
        send_error(session->sock, "Cannot read file for verification");
        return;
    }

    char response[64];
    snprintf(response, sizeof(response), "%08x|%llu", crc, (unsigned long long)size);
    send_response(session->sock, RESP_DATA, response, strlen(response));
}

static int remount_system_ex(void) {
    struct iovec iov[] = {
        IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
        IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
        IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
        IOVEC_ENTRY("async"),     { NULL, 0 },
        IOVEC_ENTRY("ignoreacl"), { NULL, 0 },
    };
    int rc = wdg_fn_call((void *)nmount, iov, (void *)(long)IOVEC_SIZE(iov), (void *)(long)MNT_UPDATE, NULL, 5000);
    if (rc == -2) { errno = EBUSY; return -1; }   // timed out — treat as busy
    return rc;
}

// v6.1.3: job state shared by the mount worker and its heartbeat thread.
typedef struct {
    int client_sock;
    int done;              // atomic: heartbeat stops once the worker finishes
    char title_filter[16]; // empty = mount all; else only dirs starting with it
} mount_job_t;

// The real scan/mount/registration body — runs inside the crash guard of
// mount_games_worker so a faulting Sce call can never take the server down.
static void mount_games_body(mount_job_t* job, int sock);

static void* mount_games_worker(void* arg) {
    mount_job_t* job = (mount_job_t*)arg;
    int sock = job->client_sock;
    // Stream RESP_PROGRESS lines to THIS client while the scan runs
    progress_socket_set(sock);

    wdg_install_handler();
    if (sigsetjmp(t_wdg_jmp, 1) == 0) {
        t_wdg_armed = 1;
        mount_games_body(job, sock);
        t_wdg_armed = 0;
    } else {
        // Crash path: release locks this thread may have been holding so the
        // process stays responsive for the next command.
        t_wdg_armed = 0;
        unlock_mounted_ids();
        pthread_mutex_unlock(&g_extendedinfo_lock);
        __atomic_store_n(&job->done, 1, __ATOMIC_SEQ_CST);
        send_resp_locked(sock, RESP_ERROR, "Mount worker crashed (contained)", 31);
        progress_socket_clear(sock);
    }
    // job intentionally NOT freed: the heartbeat thread may still be reading
    // job->done (pre-existing pattern, tiny leak).
    return NULL;
}

// Find the SceShellUI renderer pid (-1 if absent). NEVER matches
// SceShellCore — killing THAT crashes the session (proven).
static pid_t find_shellui(void) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t size = 0;
    pid_t found = -1;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) == 0 && size > 0) {
        size += size / 8;
        char *buf = malloc(size);
        if (buf) {
            if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
                for (char *p = buf; p + sizeof(int) <= buf + size; ) {
                    struct kinfo_proc *ki = (struct kinfo_proc *)p;
                    if (ki->ki_structsize <= 0) break;
                    p += ki->ki_structsize;
                    if (strncmp(ki->ki_comm, "SceShellUI", 10) == 0 &&
                        ki->ki_comm[10] == '\0') {
                        found = ki->ki_pid;
                        break;
                    }
                }
            }
            free(buf);
        }
    }
    return found;
}

// Bounce the UI renderer: the compositor respawns it and the fresh instance
// re-reads app.db — needed under kstuff where we register titles by direct
// DB writes (no daemon IPC = no live UI notification). Screen flashes for
// ~10s; this process is unaffected.
static void restart_shellui(void) {
    pid_t p = find_shellui();
    if (p > 0) kill(p, SIGKILL);
}

static void mount_games_body(mount_job_t* job, int sock) {
    // Serialize concurrent mounts: g_mounted_ids is reset here and consumed by
    // process_game() - two simultaneous Mount Games runs would corrupt it.
    lock_mounted_ids();

    // Free previous tracking list and start fresh
    mounted_id_node_t *n = g_mounted_ids;
    while (n) { mounted_id_node_t *nx = n->next; free(n); n = nx; }
    g_mounted_ids = NULL;
    g_mounted_count = 0;
    unlock_mounted_ids();
    
    // (AppInstUtil Initialize happens lazily inside register_title() — one
    // shot, watchdog-guarded.)
    
    // Remount /system_ex as writable
    remount_system_ex();
    
    int cleaned = auto_unmount_deleted_games();

    int mounted_count = 0;
    int skipped_count = 0;
    int failed_count = 0;
    int duplicate_count = 0;
    int total_games = 0;
    int current_game = 0;
    struct stat st;
    
    // Store mounted game names for response (up to 20)
    char mounted_games[20][300];
    int stored_names = 0;

    // First pass: count total games across all paths
    for (int pi = 0; pi < (int)NUM_GAME_SCAN_PATHS; pi++) {
        if (stat(GAME_SCAN_PATHS[pi], &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        DIR* d = opendir(GAME_SCAN_PATHS[pi]);
        if (!d) continue;
        struct dirent* e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (job->title_filter[0] &&
                strncmp(e->d_name, job->title_filter, strlen(job->title_filter)) != 0)
                continue;
            char gp[PATH_MAX];
            snprintf(gp, sizeof(gp), "%s/%s", GAME_SCAN_PATHS[pi], e->d_name);
            if (stat(gp, &st) == 0 && S_ISDIR(st.st_mode)) total_games++;
        }
        closedir(d);
    }

    // Send progress: starting
    {
        char prog_msg[256];
        snprintf(prog_msg, sizeof(prog_msg), "Scanning %d games across %d locations...", 
                 total_games, (int)NUM_GAME_SCAN_PATHS);
        send_notification(prog_msg);
    }

    // Second pass: process all games
    for (int pi = 0; pi < (int)NUM_GAME_SCAN_PATHS; pi++) {
        const char* base_path = GAME_SCAN_PATHS[pi];
        if (stat(base_path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        
        DIR* d = opendir(base_path);
        if (!d) continue;
        
        struct dirent* e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            
            char game_path[PATH_MAX];
            snprintf(game_path, sizeof(game_path), "%s/%s", base_path, e->d_name);
            if (stat(game_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (job->title_filter[0] &&
                strncmp(e->d_name, job->title_filter, strlen(job->title_filter)) != 0)
                continue;

            current_game++;
            char game_name[256] = {};
            char title_id[12] = {};
            int result = process_game(game_path, game_name, sizeof(game_name), title_id, sizeof(title_id));
            
            if (result == 0) {
                // Successfully mounted
                if (stored_names < 20) {
                    snprintf(mounted_games[stored_names], sizeof(mounted_games[0]), "%s", game_name);
                    stored_names++;
                }
                mounted_count++;
                // Send PS5 notification for each mounted game
                char notify_msg[512];
                snprintf(notify_msg, sizeof(notify_msg), "Mounting %d/%d (%d%%)\n%s", 
                         current_game, total_games, 
                         total_games > 0 ? (current_game * 100) / total_games : 0,
                         game_name);
                send_notification(notify_msg);
            }

            // STREAMING PROGRESS: push a RESP_PROGRESS line to the client for
            // every processed game (mounted/skipped/duplicate/failed). The new
            // clients rely on these to keep their read timeout alive during
            // long mounts; old clients simply ignore unknown progress lines.
            {
                char prog[384];
                snprintf(prog, sizeof(prog), "%d/%d: %s", current_game, total_games,
                         game_name[0] ? game_name : title_id);
                send_progress_message(prog);
            }

            if (result == 2) {
                skipped_count++;
            } else if (result == 3) {
                duplicate_count++;
            } else if (result != 0) {
                failed_count++;
            }
        }
        closedir(d);
    }

    // Build response message for client
    char response[8192];
    int off = 0;
    
    if (cleaned > 0) {
        off += snprintf(response + off, sizeof(response) - off, 
                       "Cleaned: %d deleted game(s)\n", cleaned);
    }
    
    off += snprintf(response + off, sizeof(response) - off,
                   "New mounts: %d\n", mounted_count);
    
    if (mounted_count > 0 && stored_names > 0) {
        off += snprintf(response + off, sizeof(response) - off, "Mounted games:\n");
        for (int i = 0; i < stored_names && off < (int)sizeof(response) - 100; i++) {
            off += snprintf(response + off, sizeof(response) - off,
                          "  %s\n", mounted_games[i]);
        }
    }
    
    off += snprintf(response + off, sizeof(response) - off,
                   "Already mounted: %d\n", skipped_count);
    if (duplicate_count > 0) {
        off += snprintf(response + off, sizeof(response) - off,
                       "Duplicates skipped: %d\n", duplicate_count);
    }
    off += snprintf(response + off, sizeof(response) - off,
                   "Failed: %d\nTotal active: %d",
                   failed_count, mounted_count + skipped_count);

    // Send final PS5 notification
    if (mounted_count > 0) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "Game Mounter\nMounted %d new game(s)\n%d already mounted",
                 mounted_count, skipped_count);
        send_notification(msg);
    } else if (skipped_count > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Game Mounter\nAll %d game(s) already mounted", skipped_count);
        send_notification(msg);
    } else {
        send_notification("Game Mounter\nNo games found to mount");
    }

    // done BEFORE the final OK so the heartbeat can never interleave a
    // progress frame with the response (mirrors the unmount worker).
    __atomic_store_n(&job->done, 1, __ATOMIC_SEQ_CST);
    send_ok(sock, response);
    progress_socket_clear(sock);

    // Direct-DB registration bypasses the daemon, so nothing tells the UI
    // that the registry changed — bounce SceShellUI so new icons appear (and
    // removed ones disappear) without a console reboot.
    if (!etahen_present() &&
        (mounted_count + skipped_count + cleaned) > 0) {
        send_notification("Refreshing home screen...");
        usleep(500 * 1000);
        restart_shellui();
    }
}

// Heartbeat thread: streams a RESP_PROGRESS every 3s so the client never sits
// on a silent socket past its read timeout while the mount grinds on.
static void* mount_games_heartbeat(void* arg) {
    mount_job_t* job = (mount_job_t*)arg;
    struct timespec one_sec = { 1, 0 };
    for (int i = 0; i < 40; i++) {              // 40 x 3s = 2 min, then give up
        for (int j = 0; j < 3; j++) {
            if (__atomic_load_n(&job->done, __ATOMIC_SEQ_CST)) return NULL;
            nanosleep(&one_sec, NULL);
        }
        if (__atomic_load_n(&job->done, __ATOMIC_SEQ_CST)) return NULL;
        send_progress_message("Mount Games still working...");
    }
    return NULL;
}

// v6.1.3: Mount Games now runs on a detached worker with its own heartbeat
// thread (same pattern as the unmount path). Before, the whole scan ran on
// the command thread: a single blocked syscall (busy vnode, wedged dlopen)
// froze the command loop — no heartbeats, no other commands, payload looked
// dead and had to be killed from the Toolbox. Now the command loop returns
// immediately and RESP_PROGRESS lines keep the client fed.
static void spawn_mount_job(client_session_t *session, const char *title_filter) {
    mount_job_t* job = (mount_job_t*)malloc(sizeof(mount_job_t));
    if (!job) {
        send_error(session->sock, "Out of memory");
        return;
    }
    memset(job, 0, sizeof(*job));
    job->client_sock = session->sock;
    if (title_filter) snprintf(job->title_filter, sizeof(job->title_filter), "%s", title_filter);

    // Immediate acknowledgement: the client hears from us within
    // milliseconds. The worker owns progress + final OK from here on.
    progress_socket_set(session->sock);
    send_progress_message(job->title_filter[0] ? "Starting Mount Game..." : "Starting Mount Games...");

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t hb_tid;
    bool hb_ok = (pthread_create(&hb_tid, &at, mount_games_heartbeat, job) == 0);
    if (!hb_ok) pthread_attr_destroy(&at);

    pthread_t w_tid;
    if (pthread_create(&w_tid, hb_ok ? NULL : &at, mount_games_worker, job) != 0) {
        if (hb_ok) pthread_cancel(hb_tid);
        pthread_attr_destroy(&at);
        free(job);
        progress_socket_clear(session->sock);
        send_error(session->sock, "Failed to start mount worker");
        return;
    }
    if (hb_ok) pthread_attr_destroy(&at);
    // Session thread returns to the command loop immediately.
}

void handle_mount_games(client_session_t *session) {
    spawn_mount_job(session, NULL);
}

// MOUNT_GAME — same worker pipeline filtered to a single title id.
void handle_mount_game(client_session_t *session, const char *title_id) {
    if (!title_id || strlen(title_id) < 4) {
        send_error(session->sock, "Usage: title_id");
        return;
    }
    spawn_mount_job(session, title_id);
}

// ============================================================================
// HARDWARE & SYSTEM INFO FUNCTIONS
// ============================================================================

// Handle GET_GAME_LIST - Get list of all mounted games with details
void handle_get_game_list(client_session_t *session) {
    char *response = malloc(65536);  // 64KB for game list
    if (!response) {
        send_error(session->sock, "Out of memory");
        return;
    }
    
    int off = 0;
    int game_count = 0;
    
    // Scan /user/app for mounted games (those with mount.lnk)
    DIR *d = opendir("/user/app");
    if (!d) {
        send_error(session->sock, "Cannot open /user/app");
        free(response);
        return;
    }
    
    struct dirent *e;
    while ((e = readdir(d)) && off < 60000) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        
        // Check if it's a valid title ID format (CUSA/PPSA + 5 digits)
        if ((strncmp(e->d_name, "CUSA", 4) != 0 && 
             strncmp(e->d_name, "PPSA", 4) != 0) || 
            strlen(e->d_name) != 9)
            continue;
        
        // Check if mount.lnk exists (indicates it's a mounted game)
        char mount_lnk[PATH_MAX];
        snprintf(mount_lnk, sizeof(mount_lnk), "/user/app/%s/mount.lnk", e->d_name);
        
        FILE *f = fopen(mount_lnk, "r");
        if (!f) continue;  // Not a mounted game
        
        char game_path[PATH_MAX];
        memset(game_path, 0, sizeof(game_path));
        if (!fgets(game_path, sizeof(game_path), f)) {
            fclose(f);
            continue;
        }
        fclose(f);
        game_path[strcspn(game_path, "\r\n")] = '\0';
        
        // Get game name from param.json
        char param_json[PATH_MAX];
        snprintf(param_json, sizeof(param_json), "%s/sce_sys/param.json", game_path);
        char game_name[256] = "Unknown";
        get_game_name_from_json(param_json, game_name, sizeof(game_name));
        
        // Get game size (approximate from eboot.bin)
        char eboot_path[PATH_MAX];
        snprintf(eboot_path, sizeof(eboot_path), "%s/eboot.bin", game_path);
        struct stat st;
        uint64_t game_size = 0;
        if (stat(eboot_path, &st) == 0) {
            game_size = st.st_size;
        }
        
        // Get region
        const char *region = get_game_region(e->d_name);
        
        // Check if nullfs mount is active
        char system_ex_app[PATH_MAX];
        snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", e->d_name);
        int is_active = is_mounted(system_ex_app);
        
        // Format: title_id|name|path|size|region|active
        off += snprintf(response + off, 65536 - off,
            "%s|%s|%s|%llu|%s|%d\n",
            e->d_name, game_name, game_path, 
            (unsigned long long)game_size, region, is_active);
        
        game_count++;
    }
    closedir(d);

    // Pass 2: games on disk that are NOT mounted — scan the game storage
    // dirs so the client can list (and mount) them individually.
    for (int pi = 0; pi < (int)NUM_GAME_SCAN_PATHS && off < 60000; pi++) {
        const char *base = GAME_SCAN_PATHS[pi];
        DIR *gd = opendir(base);
        if (!gd) continue;
        struct dirent *ge;
        while ((ge = readdir(gd)) && off < 60000) {
            if (!strcmp(ge->d_name, ".") || !strcmp(ge->d_name, "..")) continue;

            char game_path[PATH_MAX];
            snprintf(game_path, sizeof(game_path), "%s/%s", base, ge->d_name);
            struct stat gst;
            if (stat(game_path, &gst) != 0 || !S_ISDIR(gst.st_mode)) continue;

            // Derive a title id from the dir name: "PPSA12345-app" or "PPSA12345"
            char tid[16];
            snprintf(tid, sizeof(tid), "%s", ge->d_name);
            char *dash = strstr(tid, "-app");
            if (dash) *dash = '\0';
            if ((strncmp(tid, "CUSA", 4) != 0 && strncmp(tid, "PPSA", 4) != 0) ||
                strlen(tid) != 9)
                continue;

            // Skip anything already registered (reported in pass 1)
            char ml[PATH_MAX];
            snprintf(ml, sizeof(ml), "/user/app/%s/mount.lnk", tid);
            FILE *lf = fopen(ml, "r");
            if (lf) { fclose(lf); continue; }

            // Must actually look like a game dir
            char param_json[PATH_MAX];
            snprintf(param_json, sizeof(param_json), "%s/sce_sys/param.json", game_path);
            struct stat pst;
            if (stat(param_json, &pst) != 0) continue;

            char game_name[256] = "Unknown";
            get_game_name_from_json(param_json, game_name, sizeof(game_name));

            char eboot_path[PATH_MAX];
            snprintf(eboot_path, sizeof(eboot_path), "%s/eboot.bin", game_path);
            uint64_t game_size = 0;
            if (stat(eboot_path, &gst) == 0) game_size = gst.st_size;

            const char *region = get_game_region(tid);

            off += snprintf(response + off, 65536 - off,
                "%s|%s|%s|%llu|%s|0\n",
                tid, game_name, game_path,
                (unsigned long long)game_size, region);
            game_count++;
        }
        closedir(gd);
    }

    if (game_count == 0) {
        strcpy(response, "NO_GAMES\n");
        off = strlen(response);
    }
    
    send_response(session->sock, RESP_DATA, response, off);
    free(response);
}

// Handle GET_GAME_ICON - Send icon0.png binary for a given title_id
void handle_get_game_icon(client_session_t *session, const char *title_id) {
    if (!title_id || strlen(title_id) == 0) {
        send_error(session->sock, "No title ID provided");
        return;
    }
    
    // Validate title ID format
    if ((strncmp(title_id, "CUSA", 4) != 0 && 
         strncmp(title_id, "PPSA", 4) != 0) || 
        strlen(title_id) != 9) {
        send_error(session->sock, "Invalid title ID format");
        return;
    }
    
    // Try multiple icon paths (prefer appmeta, then user/app, then game source)
    char icon_path[PATH_MAX];
    FILE *f = NULL;
    
    // 1. /user/appmeta/{title_id}/icon0.png
    snprintf(icon_path, sizeof(icon_path), "/user/appmeta/%s/icon0.png", title_id);
    f = fopen(icon_path, "rb");
    
    // 2. /user/app/{title_id}/sce_sys/icon0.png
    if (!f) {
        snprintf(icon_path, sizeof(icon_path), "/user/app/%s/sce_sys/icon0.png", title_id);
        f = fopen(icon_path, "rb");
    }
    
    // 3. Follow mount.lnk to the original game path
    if (!f) {
        char mount_lnk[PATH_MAX];
        snprintf(mount_lnk, sizeof(mount_lnk), "/user/app/%s/mount.lnk", title_id);
        FILE *lnk = fopen(mount_lnk, "r");
        if (lnk) {
            char game_path[PATH_MAX] = {0};
            if (fgets(game_path, sizeof(game_path), lnk)) {
                game_path[strcspn(game_path, "\r\n")] = '\0';
                snprintf(icon_path, sizeof(icon_path), "%s/sce_sys/icon0.png", game_path);
                f = fopen(icon_path, "rb");
            }
            fclose(lnk);
        }
    }
    
    // 4. Scan /user/home/*/savedata_prospero_meta/user/{title_id}/ for any *_icon0.png
    //    This covers saves for games that are no longer installed/mounted.
    if (!f) {
        DIR *home_dir = opendir("/user/home");
        if (home_dir) {
            struct dirent *user_ent;
            while ((user_ent = readdir(home_dir)) && !f) {
                if (!strcmp(user_ent->d_name, ".") || !strcmp(user_ent->d_name, "..")) continue;
                
                char meta_dir[PATH_MAX];
                snprintf(meta_dir, sizeof(meta_dir),
                         "/user/home/%s/savedata_prospero_meta/user/%s",
                         user_ent->d_name, title_id);
                
                DIR *md = opendir(meta_dir);
                if (!md) continue;
                
                struct dirent *file_ent;
                while ((file_ent = readdir(md))) {
                    // Pick any file ending in "_icon0.png"
                    size_t nlen = strlen(file_ent->d_name);
                    if (nlen > 10 && 
                        strstr(file_ent->d_name, "icon0.png") != NULL) {
                        snprintf(icon_path, sizeof(icon_path),
                                 "%s/%s", meta_dir, file_ent->d_name);
                        f = fopen(icon_path, "rb");
                        if (f) break;
                    }
                }
                closedir(md);
            }
            closedir(home_dir);
        }
    }

    // 5. Unmounted games on disk — scan the game storage dirs for
    //    {title_id} or {title_id}-app and read sce_sys/icon0.png.
    if (!f) {
        for (int pi = 0; pi < (int)NUM_GAME_SCAN_PATHS && !f; pi++) {
            DIR *gd = opendir(GAME_SCAN_PATHS[pi]);
            if (!gd) continue;
            struct dirent *ge;
            while ((ge = readdir(gd)) && !f) {
                if (!strcmp(ge->d_name, ".") || !strcmp(ge->d_name, "..")) continue;
                if (strncmp(ge->d_name, title_id, strlen(title_id)) != 0) continue;
                snprintf(icon_path, sizeof(icon_path),
                         "%s/%s/sce_sys/icon0.png", GAME_SCAN_PATHS[pi], ge->d_name);
                f = fopen(icon_path, "rb");
            }
            closedir(gd);
        }
    }

    if (!f) {
        send_error(session->sock, "Icon not found");
        return;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long icon_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (icon_size <= 0 || icon_size > 2 * 1024 * 1024) {  // Max 2MB
        fclose(f);
        send_error(session->sock, "Invalid icon size");
        return;
    }
    
    char *icon_data = malloc(icon_size);
    if (!icon_data) {
        fclose(f);
        send_error(session->sock, "Out of memory");
        return;
    }
    
    size_t read_bytes = fread(icon_data, 1, icon_size, f);
    fclose(f);
    
    if ((long)read_bytes != icon_size) {
        free(icon_data);
        send_error(session->sock, "Failed to read icon");
        return;
    }
    
    send_response(session->sock, RESP_DATA, icon_data, icon_size);
    free(icon_data);
}

// Handle GET_GAME_PIC - Send pic0.png or pic1.png binary for a given title_id
// Request format: "TITLE_ID:PIC_TYPE" where PIC_TYPE is '0' for pic0 or '1' for pic1
void handle_get_game_pic(client_session_t *session, const char *request) {
    if (!request || strlen(request) < 11) {  // Need at least TITLE_ID:X
        send_error(session->sock, "Invalid request format");
        return;
    }
    
    // Parse title_id and pic type
    char title_id[16] = {0};
    char pic_type = '0';
    
    // Find colon separator
    const char *colon = strchr(request, ':');
    if (!colon || (colon - request) != 9) {
        send_error(session->sock, "Invalid request format (expected TITLE_ID:TYPE)");
        return;
    }
    
    strncpy(title_id, request, 9);
    title_id[9] = '\0';
    pic_type = colon[1];
    
    // Validate title ID
    if ((strncmp(title_id, "CUSA", 4) != 0 && 
         strncmp(title_id, "PPSA", 4) != 0) || 
        strlen(title_id) != 9) {
        send_error(session->sock, "Invalid title ID format");
        return;
    }
    
    // Validate pic type
    if (pic_type != '0' && pic_type != '1') {
        send_error(session->sock, "Invalid pic type (use 0 or 1)");
        return;
    }
    
    char pic_filename[16];
    snprintf(pic_filename, sizeof(pic_filename), "pic%c.png", pic_type);
    
    // Try multiple paths
    char pic_path[PATH_MAX];
    FILE *f = NULL;
    
    // 1. /user/app/{title_id}/sce_sys/picN.png
    snprintf(pic_path, sizeof(pic_path), "/user/app/%s/sce_sys/%s", title_id, pic_filename);
    f = fopen(pic_path, "rb");
    
    // 2. /user/appmeta/{title_id}/picN.png
    if (!f) {
        snprintf(pic_path, sizeof(pic_path), "/user/appmeta/%s/%s", title_id, pic_filename);
        f = fopen(pic_path, "rb");
    }
    
    // 3. Follow mount.lnk to original source
    if (!f) {
        char mount_lnk[PATH_MAX];
        snprintf(mount_lnk, sizeof(mount_lnk), "/user/app/%s/mount.lnk", title_id);
        FILE *lnk = fopen(mount_lnk, "r");
        if (lnk) {
            char game_path[PATH_MAX] = {0};
            if (fgets(game_path, sizeof(game_path), lnk)) {
                game_path[strcspn(game_path, "\r\n")] = '\0';
                snprintf(pic_path, sizeof(pic_path), "%s/sce_sys/%s", game_path, pic_filename);
                f = fopen(pic_path, "rb");
            }
            fclose(lnk);
        }
    }
    
    if (!f) {
        send_error(session->sock, "Picture not found");
        return;
    }
    
    fseek(f, 0, SEEK_END);
    long pic_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // pic1 can be larger (up to 8MB, 3840x2160)
    if (pic_size <= 0 || pic_size > 16 * 1024 * 1024) {
        fclose(f);
        send_error(session->sock, "Invalid picture size");
        return;
    }
    
    char *pic_data = malloc(pic_size);
    if (!pic_data) {
        fclose(f);
        send_error(session->sock, "Out of memory");
        return;
    }
    
    size_t read_bytes = fread(pic_data, 1, pic_size, f);
    fclose(f);
    
    if ((long)read_bytes != pic_size) {
        free(pic_data);
        send_error(session->sock, "Failed to read picture");
        return;
    }
    
    send_response(session->sock, RESP_DATA, pic_data, pic_size);
    free(pic_data);
}

// Helper: Read text file contents into buffer (returns bytes read, 0 on error)
static size_t read_file_text(const char *path, char *buf, size_t buflen) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, buflen - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

// Helper: Recursively calculate directory size
static uint64_t dir_size_recursive(const char *path) {
    uint64_t total = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        
        if (S_ISDIR(st.st_mode)) {
            total += dir_size_recursive(full);
        } else if (S_ISREG(st.st_mode)) {
            total += st.st_size;
        }
    }
    closedir(d);
    return total;
}

// Handle GET_GAME_DETAILS - Send detailed info about a game (version, size, paths, etc)
void handle_get_game_details(client_session_t *session, const char *title_id) {
    if (!title_id || strlen(title_id) == 0) {
        send_error(session->sock, "No title ID provided");
        return;
    }
    
    if ((strncmp(title_id, "CUSA", 4) != 0 && 
         strncmp(title_id, "PPSA", 4) != 0) || 
        strlen(title_id) != 9) {
        send_error(session->sock, "Invalid title ID format");
        return;
    }
    
    // Read mount.lnk to get game source path
    char mount_lnk[PATH_MAX];
    snprintf(mount_lnk, sizeof(mount_lnk), "/user/app/%s/mount.lnk", title_id);
    char game_path[PATH_MAX] = {0};
    FILE *lnk = fopen(mount_lnk, "r");
    if (lnk) {
        if (fgets(game_path, sizeof(game_path), lnk)) {
            game_path[strcspn(game_path, "\r\n")] = '\0';
        }
        fclose(lnk);
    }
    
    char response[8192];
    int off = 0;
    
    // Game name from param.json
    char param_json[PATH_MAX];
    snprintf(param_json, sizeof(param_json), "%s/sce_sys/param.json", game_path);
    char game_name[256] = "Unknown";
    get_game_name_from_json(param_json, game_name, sizeof(game_name));
    
    // Read full param.json contents
    char param_content[4096] = {0};
    read_file_text(param_json, param_content, sizeof(param_content));
    
    // Escape newlines in param_content for single-line response
    for (char *p = param_content; *p; p++) {
        if (*p == '\n') *p = ' ';
        else if (*p == '\r') *p = ' ';
    }
    
    // Total game size (recursive)
    uint64_t total_size = 0;
    if (game_path[0]) {
        total_size = dir_size_recursive(game_path);
    }
    
    // EBoot size
    char eboot_path[PATH_MAX];
    snprintf(eboot_path, sizeof(eboot_path), "%s/eboot.bin", game_path);
    struct stat st;
    uint64_t eboot_size = 0;
    if (stat(eboot_path, &st) == 0) {
        eboot_size = st.st_size;
    }
    
    // Mount status
    char system_ex_app[PATH_MAX];
    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    int is_active = is_mounted(system_ex_app);
    
    // Get modification time of game directory (install date)
    time_t install_date = 0;
    if (game_path[0] && stat(game_path, &st) == 0) {
        install_date = st.st_mtime;
    }
    
    // Format timestamps
    char install_date_str[64] = "Unknown";
    if (install_date > 0) {
        struct tm *tm = localtime(&install_date);
        if (tm) {
            strftime(install_date_str, sizeof(install_date_str), "%Y-%m-%d %H:%M:%S", tm);
        }
    }
    
    // Region
    const char *region = get_game_region(title_id);
    
    off += snprintf(response + off, sizeof(response) - off,
        "title_id=%s\n"
        "name=%s\n"
        "path=%s\n"
        "region=%s\n"
        "total_size=%llu\n"
        "eboot_size=%llu\n"
        "install_date=%s\n"
        "is_active=%d\n"
        "param_json=%s\n",
        title_id,
        game_name,
        game_path,
        region,
        (unsigned long long)total_size,
        (unsigned long long)eboot_size,
        install_date_str,
        is_active,
        param_content);
    
    send_response(session->sock, RESP_DATA, response, off);
}

// Handle LIST_SAVES - Scan /user/home/*/savedata/* and return list of saves
// Format: title_id|user_id|save_path|total_size|mtime_unix\n...
void handle_list_saves(client_session_t *session) {
    char *response = malloc(65536);
    if (!response) {
        send_error(session->sock, "Out of memory");
        return;
    }
    
    int off = 0;
    int save_count = 0;
    
    DIR *home = opendir("/user/home");
    if (!home) {
        send_error(session->sock, "Cannot open /user/home");
        free(response);
        return;
    }
    
    // PS5 saves are in `savedata_prospero`, PS4 legacy in `savedata`
    const char *save_dirs[] = { "savedata_prospero", "savedata" };
    
    struct dirent *user_entry;
    while ((user_entry = readdir(home)) && off < 60000) {
        if (!strcmp(user_entry->d_name, ".") || !strcmp(user_entry->d_name, ".."))
            continue;
        
        // Try both savedata dirs for this user
        for (int si = 0; si < 2; si++) {
            char savedata_path[PATH_MAX];
            snprintf(savedata_path, sizeof(savedata_path),
                     "/user/home/%s/%s", user_entry->d_name, save_dirs[si]);
            
            DIR *sdir = opendir(savedata_path);
            if (!sdir) continue;
            
            struct dirent *save_entry;
            while ((save_entry = readdir(sdir)) && off < 60000) {
                if (!strcmp(save_entry->d_name, ".") || !strcmp(save_entry->d_name, ".."))
                    continue;
                
                // Filter by CUSA/PPSA title_id format (9 chars)
                if ((strncmp(save_entry->d_name, "CUSA", 4) != 0 && 
                     strncmp(save_entry->d_name, "PPSA", 4) != 0) || 
                    strlen(save_entry->d_name) != 9) {
                    continue;
                }
                
                char full_save_path[PATH_MAX];
                snprintf(full_save_path, sizeof(full_save_path),
                         "%s/%s", savedata_path, save_entry->d_name);
                
                struct stat st;
                if (stat(full_save_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    continue;
                }
                
                uint64_t save_size = dir_size_recursive(full_save_path);
                
                off += snprintf(response + off, 65536 - off,
                    "%s|%s|%s|%llu|%lld\n",
                    save_entry->d_name,      // title_id
                    user_entry->d_name,      // user_id
                    full_save_path,          // save_path
                    (unsigned long long)save_size,
                    (long long)st.st_mtime);
                
                save_count++;
            }
            closedir(sdir);
        }
    }
    closedir(home);
    
    if (save_count == 0) {
        strcpy(response, "NO_SAVES\n");
        off = strlen(response);
    }
    
    send_response(session->sock, RESP_DATA, response, off);
    free(response);
}

// ============================================================================
// SAVE MANAGER (garlic-savemgr style, adapted for the suite protocol)
//
// Scans individual save IMAGE files (not just title dirs), mounts them
// DECRYPTED at SAVE_MNT via sceFsMountSaveData so the regular file commands
// (LIST_DIR / DOWNLOAD / UPLOAD / DELETE / RENAME) can browse and edit the
// plaintext contents, then unmounts — the modified image is written back to
// its original location.
//
// sceFs* symbols are resolved at RUNTIME (dlopen, kernel_dynlib fallback)
// because libSceFsInternalForVsh isn't guaranteed to be a resolvable NEEDED
// dep under every loader — hard-linking it would kill the ELF under
// restrictive loaders.
// ============================================================================
#define SAVE_MNT        "/data/save_mnt"
#define SAVE_LOCAL_DIR  "/data/save_files"
#define PFS_DECRYPT_KEK 0xc0845302
#define SAVE_KEYBUF_LEN 0x200

typedef struct { uint8_t reserved; char *budgetid; } save_mount_opt_t;

static int (*g_sceFsInitMountSaveDataOpt)(save_mount_opt_t*) = NULL;
static int (*g_sceFsInitUmountSaveDataOpt)(save_mount_opt_t*) = NULL;
static int (*g_sceFsMountSaveData)(save_mount_opt_t*, const char*, const char*, void*) = NULL;
static int (*g_sceFsUmountSaveData)(save_mount_opt_t*, const char*, void*, int) = NULL;
static int g_savefs_resolved = 0;

static pthread_mutex_t g_save_mtx = PTHREAD_MUTEX_INITIALIZER;
static int  g_save_mounted = 0;
static int  g_save_is_ps4 = 0;
static char g_save_src[PATH_MAX] = {0};     // original image path
static char g_save_local[PATH_MAX] = {0};   // image actually mounted (staging copy under /data)
static save_mount_opt_t g_save_mopt;
static uint8_t *g_save_keybuf = NULL;       // heap — survives parked wdg workers

static int resolve_savefs(void) {
    if (g_savefs_resolved)
        return g_sceFsMountSaveData != NULL && g_sceFsUmountSaveData != NULL;

    // Try dlopen first — loads the module if absent and gives plain dlsym.
    static const char *paths[] = {
        "/system/vsh/libSceFsInternalForVsh.sprx",
        "/system/common/lib/libSceFsInternalForVsh.sprx",
        "/preinst2/vsh/libSceFsInternalForVsh.sprx",
        "/preinst2/common/lib/libSceFsInternalForVsh.sprx",
        "libSceFsInternalForVsh.sprx",
        NULL
    };
    void *h = NULL;
    const char *used = NULL;
    for (int i = 0; paths[i] && !h; i++) {
        h = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
        if (h) used = paths[i];
    }
    if (h) {
        g_sceFsInitMountSaveDataOpt  = (int (*)(save_mount_opt_t*))dlsym(h, "sceFsInitMountSaveDataOpt");
        g_sceFsInitUmountSaveDataOpt = (int (*)(save_mount_opt_t*))dlsym(h, "sceFsInitUmountSaveDataOpt");
        g_sceFsMountSaveData         = (int (*)(save_mount_opt_t*, const char*, const char*, void*))dlsym(h, "sceFsMountSaveData");
        g_sceFsUmountSaveData        = (int (*)(save_mount_opt_t*, const char*, void*, int))dlsym(h, "sceFsUmountSaveData");
    }

    // Fallback: module may already be loaded in our process under a different
    // loader context — find its handle and resolve by NID (nid_encode works
    // for any symbol name).
    if (!g_sceFsMountSaveData) {
        uint32_t mod = 0;
        if (krpc_dynlib_handle(getpid(), "libSceFsInternalForVsh.sprx", &mod) == 0 && mod) {
            char nid[12];
            void *vp;
            nid_encode("sceFsInitMountSaveDataOpt", nid);
            vp = krpc_resolve_sym(getpid(), mod, nid);
            if (vp) g_sceFsInitMountSaveDataOpt = (void*)vp;
            nid_encode("sceFsInitUmountSaveDataOpt", nid);
            vp = krpc_resolve_sym(getpid(), mod, nid);
            if (vp) g_sceFsInitUmountSaveDataOpt = (void*)vp;
            nid_encode("sceFsMountSaveData", nid);
            vp = krpc_resolve_sym(getpid(), mod, nid);
            if (vp) g_sceFsMountSaveData = (void*)vp;
            nid_encode("sceFsUmountSaveData", nid);
            vp = krpc_resolve_sym(getpid(), mod, nid);
            if (vp) g_sceFsUmountSaveData = (void*)vp;
        }
    }

    g_savefs_resolved = 1;
    (void)used;
    return g_sceFsMountSaveData && g_sceFsInitMountSaveDataOpt &&
           g_sceFsUmountSaveData && g_sceFsInitUmountSaveDataOpt;
}

// Read 0x60 bytes at `off` from `path` into dst. Returns 0 on success.
static int save_pread60(const char *path, off_t off, uint8_t *dst) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = pread(fd, dst, 0x60, off);
    close(fd);
    return n == 0x60 ? 0 : -1;
}

// Decrypt the 0x20-byte PFS key: caller has placed the encrypted blob at
// keybuf+0x20; after the ioctl the key sits at keybuf+0x60.
// Heap buffer — the ioctl writes beyond the declared struct bounds on some
// FWs and would smash a stack frame.
static int save_decrypt_key(uint8_t *keybuf) {
    int fd = open("/dev/pfsmgr", O_RDWR);
    if (fd < 0) return -1;
    int r = ioctl(fd, PFS_DECRYPT_KEK, keybuf);
    close(fd);
    return r;
}

// Core mount. Runs under g_save_mtx. Returns 0 or -1 with g_save_err filled.
static char g_save_err[256] = {0};
static int save_mount_internal(const char *src) {
    g_save_err[0] = 0;
    if (g_save_mounted) { snprintf(g_save_err, sizeof(g_save_err), "Another save is already mounted at %s", SAVE_MNT); return -1; }

    // Stale mountpoint from a previous run? Clear it first.
    {
        struct stat ma, mb;
        if (stat(SAVE_MNT, &ma) == 0 && stat("/data", &mb) == 0 && ma.st_dev != mb.st_dev) {
            resolve_savefs();
            if (g_sceFsUmountSaveData && g_sceFsInitUmountSaveDataOpt) {
                save_mount_opt_t uopt;
                memset(&uopt, 0, sizeof(uopt));
                g_sceFsInitUmountSaveDataOpt(&uopt);
                signal(SIGPIPE, SIG_DFL);
                wdg_fn_call((void*)g_sceFsUmountSaveData, &uopt, (void*)SAVE_MNT, NULL, NULL, 20000);
                sync();
            }
        }
    }

    if (!resolve_savefs()) {
        snprintf(g_save_err, sizeof(g_save_err), "libSceFsInternalForVsh not available on this system");
        return -1;
    }

    // PS4 image detection (garlic: first byte 0x01 => PS4 sealed-key format)
    int fd = open(src, O_RDONLY);
    if (fd < 0) { snprintf(g_save_err, sizeof(g_save_err), "Cannot open %s", src); return -1; }
    uint8_t b0 = 0;
    if (read(fd, &b0, 1) != 1) { close(fd); snprintf(g_save_err, sizeof(g_save_err), "Empty save image"); return -1; }
    close(fd);
    int is_ps4 = (b0 == 0x01);

    // Stage the image under /data when it lives elsewhere — mounting straight
    // from savedata_prospero fails with EPIPE on several FWs (garlic does the
    // same copy dance).
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    const char *local = src;
    char staged[PATH_MAX];
    if (strncmp(src, "/data/", 6) != 0) {
        mkdir(SAVE_LOCAL_DIR, 0755);
        snprintf(staged, sizeof(staged), "%s/%s", SAVE_LOCAL_DIR, base);
        if (copy_file(src, staged) != 0) {
            snprintf(g_save_err, sizeof(g_save_err), "Failed to stage save to %s", staged);
            return -1;
        }
        chmod(staged, 0755);
        // PS4 companion sealed-key file rides along
        if (is_ps4) {
            char bin_src[PATH_MAX], bin_dst[PATH_MAX];
            int dlen = (int)(base - src);  // includes trailing '/'
            if (dlen > 0 && dlen < (int)sizeof(bin_src) - 1) {
                memcpy(bin_src, src, dlen);
                snprintf(bin_src + dlen, sizeof(bin_src) - dlen, "%s.bin", base);
                snprintf(bin_dst, sizeof(bin_dst), "%s/%s.bin", SAVE_LOCAL_DIR, base);
                struct stat bst;
                if (stat(bin_src, &bst) == 0)
                    copy_file(bin_src, bin_dst);
            }
        }
        local = staged;
    }

    if (!g_save_keybuf) g_save_keybuf = calloc(1, SAVE_KEYBUF_LEN);
    if (!g_save_keybuf) { snprintf(g_save_err, sizeof(g_save_err), "Out of memory"); return -1; }
    memset(g_save_keybuf, 0, SAVE_KEYBUF_LEN);

    // Encrypted key blob -> keybuf+0x00 (the pfsmgr ioctl reads input at 0
    // and writes the 0x20-byte key at +0x60 — garlic's exact layout).
    if (is_ps4) {
        char bin_path[PATH_MAX];
        snprintf(bin_path, sizeof(bin_path), "%s/%s.bin", SAVE_LOCAL_DIR, base);
        if (strncmp(local, "/data/", 6) != 0) {  // src already under /data
            int dlen = (int)(base - src);
            if (dlen > 0 && dlen < (int)sizeof(bin_path) - 1) {
                memcpy(bin_path, src, dlen);
                snprintf(bin_path + dlen, sizeof(bin_path) - dlen, "%s.bin", base);
            }
        }
        if (save_pread60(bin_path, 0, g_save_keybuf) != 0) {
            snprintf(g_save_err, sizeof(g_save_err), "Missing/unreadable PS4 sealed key: %s", bin_path);
            return -1;
        }
    } else {
        if (save_pread60(local, 0x800, g_save_keybuf) != 0) {
            snprintf(g_save_err, sizeof(g_save_err), "Cannot read key blob at offset 0x800");
            return -1;
        }
    }

    if (save_decrypt_key(g_save_keybuf) != 0) {
        snprintf(g_save_err, sizeof(g_save_err), "pfsmgr key decryption failed (/dev/pfsmgr unavailable?)");
        return -1;
    }
    uint8_t *key = g_save_keybuf + 0x60;

    // Mount — garlic swaps to authid 0x4800000000000010 for this call. Try
    // with our ShellCore authid first; only swap on failure, and restore
    // immediately (the authid is process-wide).
    memset(&g_save_mopt, 0, sizeof(g_save_mopt));
    g_sceFsInitMountSaveDataOpt(&g_save_mopt);
    g_save_mopt.budgetid = "system";

    mkdir(SAVE_MNT, 0777);
    signal(SIGPIPE, SIG_DFL);   // mount may raise SIGPIPE — don't die on it

    send_progress_message("save: calling sceFsMountSaveData");
    int r = wdg_fn_call((void*)g_sceFsMountSaveData, &g_save_mopt, (void*)local,
                        (void*)SAVE_MNT, key, 45000);
    if (r < 0 && r != -2) {
        // Retry under garlic's save-manager authid — some FWs gate the mount
        // to that credential specifically.
        pid_t me = getpid();
        uint64_t old_authid = kernel_get_ucred_authid(me);
        kernel_set_ucred_authid(me, 0x4800000000000010ULL);
        send_progress_message("save: retrying with save-mgr authid");
        r = wdg_fn_call((void*)g_sceFsMountSaveData, &g_save_mopt, (void*)local,
                        (void*)SAVE_MNT, key, 45000);
        kernel_set_ucred_authid(me, old_authid);
    }

    // Positive return = mount handle (success); negative = error. Verify the
    // mountpoint is really on a different fs — a small positive ret can also
    // be a kernel errno on some FWs.
    if (r >= 0) {
        struct stat ma, mb;
        if (stat(SAVE_MNT, &ma) == 0 && stat("/data", &mb) == 0 && ma.st_dev == mb.st_dev)
            r = -1;   // not actually mounted
    }
    if (r < 0) {
        if (local != src) unlink(local);
        snprintf(g_save_err, sizeof(g_save_err), "sceFsMountSaveData failed: %d (local=%s)", r, local);
        return -1;
    }

    g_save_mounted = 1;
    g_save_is_ps4 = is_ps4;
    snprintf(g_save_src, sizeof(g_save_src), "%s", src);
    snprintf(g_save_local, sizeof(g_save_local), "%s", local);
    return 0;
}

static int save_umount_internal(void) {
    g_save_err[0] = 0;
    int had_state = g_save_mounted;
    if (!had_state) {
        // Best-effort: something may still be mounted at SAVE_MNT from a
        // previous run. Check it's actually a mountpoint by probing dev.
        struct stat a, b;
        if (stat(SAVE_MNT, &a) != 0 || stat("/data", &b) != 0 || a.st_dev == b.st_dev) {
            snprintf(g_save_err, sizeof(g_save_err), "No save is mounted");
            return -1;
        }
    }
    if (!resolve_savefs()) {
        snprintf(g_save_err, sizeof(g_save_err), "libSceFsInternalForVsh not available");
        return -1;
    }

    save_mount_opt_t uopt;
    memset(&uopt, 0, sizeof(uopt));
    g_sceFsInitUmountSaveDataOpt(&uopt);
    signal(SIGPIPE, SIG_DFL);
    int r = wdg_fn_call((void*)g_sceFsUmountSaveData, &uopt, (void*)SAVE_MNT, NULL, NULL, 45000);
    sync();

    if (had_state) {
        // Write the (possibly modified) staged image back over the original.
        if (strcmp(g_save_local, g_save_src) != 0) {
            if (copy_file(g_save_local, g_save_src) != 0)
                snprintf(g_save_err, sizeof(g_save_err),
                         "Unmounted but write-back failed (%s -> %s)", g_save_local, g_save_src);
            unlink(g_save_local);
        }
        // Keep the staged PS4 key file out of the staging dir too
        const char *base = strrchr(g_save_local, '/');
        if (base) {
            char leftover[PATH_MAX];
            snprintf(leftover, sizeof(leftover), "%s%s.bin", SAVE_LOCAL_DIR, base);
            unlink(leftover);
        }
    }
    rmdir(SAVE_MNT);

    g_save_mounted = 0;
    g_save_src[0] = g_save_local[0] = 0;
    g_save_is_ps4 = 0;
    return r;
}

// SAVE_SCAN — file-level list, garlic style.
// Line: path|title_id|save_name|user|size|is_ps4|has_bin
void handle_save_scan(client_session_t *session) {
    char *response = malloc(131072);
    if (!response) { send_error(session->sock, "Out of memory"); return; }
    int off = 0, count = 0;

    DIR *home = opendir("/user/home");
    if (!home) { send_error(session->sock, "Cannot open /user/home"); free(response); return; }

    const char *save_dirs[] = { "savedata_prospero", "savedata" };
    struct dirent *ue;
    while ((ue = readdir(home)) && off < 120000) {
        if (ue->d_name[0] == '.') continue;
        for (int si = 0; si < 2; si++) {
            char sroot[PATH_MAX];
            snprintf(sroot, sizeof(sroot), "/user/home/%s/%s", ue->d_name, save_dirs[si]);
            DIR *sdir = opendir(sroot);
            if (!sdir) continue;
            struct dirent *te;
            while ((te = readdir(sdir)) && off < 120000) {
                if (te->d_name[0] == '.') continue;
                if ((strncmp(te->d_name, "CUSA", 4) && strncmp(te->d_name, "PPSA", 4)) ||
                    strlen(te->d_name) != 9) continue;
                char tdir[PATH_MAX];
                snprintf(tdir, sizeof(tdir), "%s/%s", sroot, te->d_name);
                struct stat tst;
                if (stat(tdir, &tst) != 0 || !S_ISDIR(tst.st_mode)) continue;

                DIR *td = opendir(tdir);
                if (!td) continue;
                struct dirent *fe;
                while ((fe = readdir(td)) && off < 120000) {
                    if (fe->d_name[0] == '.') continue;
                    int nlen = (int)strlen(fe->d_name);
                    if (nlen > 4 && !strcmp(fe->d_name + nlen - 4, ".bin")) continue;  // key file, not a save

                    char fpath[PATH_MAX];
                    snprintf(fpath, sizeof(fpath), "%s/%s", tdir, fe->d_name);
                    struct stat fst;
                    if (stat(fpath, &fst) != 0 || !S_ISREG(fst.st_mode)) continue;

                    // In the PS4-legacy `savedata` dir, sdimg_* files are only
                    // real saves when the sealed .bin key companion exists
                    // (garlic's filter). PS5 `savedata_prospero` images are
                    // self-contained — no companion needed, name irrelevant.
                    int has_bin = 0;
                    {
                        char binp[PATH_MAX];
                        snprintf(binp, sizeof(binp), "%s.bin", fpath);
                        struct stat bst;
                        if (stat(binp, &bst) == 0) has_bin = 1;
                    }
                    if (si == 1 && !strncmp(fe->d_name, "sdimg_", 6) && !has_bin)
                        continue;

                    // Detect PS4 vs PS5 by first byte (garlic heuristic)
                    int is_ps4 = 0;
                    int ffd = open(fpath, O_RDONLY);
                    if (ffd >= 0) { uint8_t bb = 0; if (read(ffd, &bb, 1) == 1 && bb == 0x01) is_ps4 = 1; close(ffd); }

                    off += snprintf(response + off, 131072 - off,
                        "%s|%s|%s|%s|%llu|%d|%d\n",
                        fpath, te->d_name, fe->d_name, ue->d_name,
                        (unsigned long long)fst.st_size, is_ps4, has_bin);
                    count++;
                }
                closedir(td);
            }
            closedir(sdir);
        }
    }
    closedir(home);
    if (count == 0) { strcpy(response, "NO_SAVES\n"); off = strlen(response); }
    send_response(session->sock, RESP_DATA, response, off);
    free(response);
}

// SAVE_MOUNT — data = full save image path. Mounts it decrypted at SAVE_MNT.
void handle_save_mount(client_session_t *session, const char *path) {
    if (!path || !*path) { send_error(session->sock, "No save path provided"); return; }
    // Sanity: only allow mounting save-image files, not arbitrary paths.
    if (strncmp(path, "/user/home/", 11) && strncmp(path, "/data/", 6) &&
        strncmp(path, "/mnt/usb", 8)) {
        send_error(session->sock, "Refusing to mount a non-save path");
        return;
    }
    send_progress_message("save: mounting...");
    pthread_mutex_lock(&g_save_mtx);
    int r = save_mount_internal(path);
    pthread_mutex_unlock(&g_save_mtx);
    if (r == 0) {
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "MOUNTED|%s|%s|%d", g_save_src, SAVE_MNT, g_save_is_ps4);
        send_response(session->sock, RESP_DATA, msg, strlen(msg));
        send_notification("Save mounted (decrypted) at /data/save_mnt");
    } else {
        send_error(session->sock, g_save_err[0] ? g_save_err : "Mount failed");
    }
}

// SAVE_UNMOUNT — unmount + write the modified image back.
void handle_save_unmount(client_session_t *session) {
    send_progress_message("save: unmounting & writing back...");
    pthread_mutex_lock(&g_save_mtx);
    int r = save_umount_internal();
    pthread_mutex_unlock(&g_save_mtx);
    if (r == 0) {
        if (g_save_err[0])
            send_response(session->sock, RESP_DATA, g_save_err, strlen(g_save_err));
        else
            send_response(session->sock, RESP_DATA, "UNMOUNTED", 9);
    } else {
        send_error(session->sock, g_save_err[0] ? g_save_err : "Unmount failed");
    }
}

// SAVE_MOUNT_STATUS — what (if anything) is mounted at SAVE_MNT.
void handle_save_mount_status(client_session_t *session) {
    char msg[PATH_MAX + 32];
    pthread_mutex_lock(&g_save_mtx);
    if (g_save_mounted)
        snprintf(msg, sizeof(msg), "MOUNTED|%s|%s|%d", g_save_src, g_save_local, g_save_is_ps4);
    else
        snprintf(msg, sizeof(msg), "IDLE");
    pthread_mutex_unlock(&g_save_mtx);
    send_response(session->sock, RESP_DATA, msg, strlen(msg));
}

// PROC_LIST — enumerate processes via KERN_PROC_PROC; "pid|comm\n" lines.
void handle_proc_list(client_session_t *session) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) {
        send_error(session->sock, "sysctl failed");
        return;
    }
    size += size / 8;
    char *buf = malloc(size);
    if (!buf) { send_error(session->sock, "no memory"); return; }
    char *out = malloc(size / 4 + 4096);
    if (!out) { free(buf); send_error(session->sock, "no memory"); return; }
    int off = 0;
    if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
        for (char *p = buf; p + sizeof(int) <= buf + size; ) {
            struct kinfo_proc *ki = (struct kinfo_proc *)p;
            if (ki->ki_structsize <= 0) break;
            p += ki->ki_structsize;
            off += snprintf(out + off, size / 4 + 4096 - off,
                            "%d|%s\n", ki->ki_pid, ki->ki_comm);
        }
    }
    send_response(session->sock, RESP_DATA, out, off);
    free(out);
    free(buf);
}

// RESTART_UI — kill ONLY SceShellUI (the WebKit UI renderer, pid 59 here).
// The compositor respawns it and the fresh instance re-reads app.db, so our
// direct-registered game icons appear live. Never touches SceShellCore —
// killing THAT crashes the whole session (proven). Screen flashes while the
// UI reloads (~10-20s); this process is unaffected.
void handle_restart_ui(client_session_t *session) {
    pid_t found = find_shellui();
    if (found < 0) {
        send_error(session->sock, "SceShellUI not found");
        return;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "OK:restarting SceShellUI (pid %d)", found);
    send_response(session->sock, RESP_OK, msg, strlen(msg));
    usleep(300 * 1000);           // let the response reach the wire first
    kill(found, SIGKILL);
}

// ============================================================================
// SELF_UPDATE — the payload replaces itself with a newer ELF.
//
// The takeover path already exists and is proven by every PC-side deploy:
// a freshly spawned instance sweeps every stale "payload*" process
// (kill_stale_instances, also forced via UPDATE_MARKER below) and then binds
// the port. So updating = feed the new ELF to the local payload loader
// (elfldr raw-socket protocol) exactly the way SendPayloadAsync does.
// The incoming instance kills this one — the client just reconnects.
//
// Arg: "<elf_path>" | "<elf_path>|<loader_port>" | "http://url[|<port>]"
// Reply: RESP_OK "UPDATE_LAUNCHED|<new_version>|<loader_port>"
// ============================================================================

// Read an ELF, sanity-check it is a loadable x86-64 binary, and pull the
// embedded "server_version=" string so we can report what we're swapping to.
static int self_update_verify(const char *path, char *ver, size_t ver_sz) {
    if (ver && ver_sz) ver[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 32 * 1024 || sz > 64 * 1024 * 1024) { fclose(f); return -2; }
    uint8_t *buf = malloc(sz);
    if (!buf) { fclose(f); return -3; }
    int rc = -4;
    if (fread(buf, 1, sz, f) == (size_t)sz) {
        // ELF64 header: magic, 64-bit class, ET_EXEC/ET_DYN, EM_X86_64
        if (sz > 20 && buf[0] == 0x7f && buf[1] == 'E' &&
            buf[2] == 'L' && buf[3] == 'F' &&
            buf[4] == 2 && buf[17] == 0 && buf[19] == 0 &&
            (buf[16] == 2 || buf[16] == 3) && buf[18] == 0x3e) {
            rc = 0;
            if (ver && ver_sz) {
                const char *tag = "server_version=";
                size_t tl = strlen(tag);
                for (long i = 0; i + (long)tl < sz; i++) {
                    if (memcmp(buf + i, tag, tl) == 0) {
                        size_t j = 0;
                        while (i + tl + j < (size_t)sz &&
                               buf[i + tl + j] >= '0' && buf[i + tl + j] <= 'z' &&
                               j + 1 < ver_sz) {
                            ver[j] = buf[i + tl + j];
                            j++;
                        }
                        ver[j] = 0;
                        break;
                    }
                }
            }
        } else rc = -5;
    }
    free(buf);
    fclose(f);
    return rc;
}

// Minimal HTTP/1.0 GET — plain http only (LAN servers, PC-hosted files).
// Follows a single http:// redirect. HTTPS needs libSceSsl — later phase.
static int http_get_to_file(const char *url, const char *dest, int depth) {
    if (depth > 1) return -1;
    const char *p = url + 7;   // past "http://"
    char host[256], path[1024] = "/";
    int port = 80;
    const char *slash = strchr(p, '/');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    if (hl == 0 || hl >= sizeof(host)) return -2;
    memcpy(host, p, hl);
    host[hl] = 0;
    if (slash) snprintf(path, sizeof(path), "%s", slash);
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535) port = 80;
    }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -3;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { freeaddrinfo(res); return -4; }
    int cres = connect(s, res->ai_addr, (socklen_t)res->ai_addrlen);
    freeaddrinfo(res);
    if (cres != 0) { close(s); return -5; }

    char req[1600];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (send(s, req, rl, 0) <= 0) { close(s); return -6; }

    // Read the whole response (payload ELFs are small), then split headers.
    size_t cap = 1u << 20, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { close(s); return -7; }
    for (;;) {
        if (len == cap) {
            if (cap >= (64u << 20)) break;
            cap <<= 1;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        ssize_t n = recv(s, buf + len, cap - len, 0);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(s);

    uint8_t *body = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (!memcmp(buf + i, "\r\n\r\n", 4)) { body = buf + i + 4; break; }
    }
    if (!body || len < 12 || memcmp(buf, "HTTP/1", 6)) { free(buf); return -8; }
    int status = atoi((char *)buf + 9);
    if (status >= 300 && status < 400) {
        char loc[1200] = {0};
        for (char *c = (char *)buf; c < (char *)body; c++) {
            if (!strncasecmp(c, "Location:", 9)) {
                c += 9;
                while (*c == ' ') c++;
                size_t j = 0;
                while (c[j] && c[j] != '\r' && j + 1 < sizeof(loc)) { loc[j] = c[j]; j++; }
                break;
            }
        }
        free(buf);
        if (loc[0] && !strncmp(loc, "http://", 7)) return http_get_to_file(loc, dest, depth + 1);
        return -9;
    }
    if (status != 200) { free(buf); return -10; }

    FILE *f = fopen(dest, "wb");
    if (!f) { free(buf); return -11; }
    size_t wrote = fwrite(body, 1, len - (size_t)(body - buf), f);
    fclose(f);
    free(buf);
    return wrote ? 0 : -12;
}

// Push the staged ELF to the local payload loader — same wire format as the
// PC client: connect, stream raw bytes, close.
static int push_file_to_loader(const char *path, int port) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { fclose(f); return -2; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(s); fclose(f); return -3;
    }
    int rc = 0;
    uint8_t *buf = malloc(65536);
    if (!buf) rc = -4;
    size_t n;
    while (rc == 0 && buf && (n = fread(buf, 1, 65536, f)) > 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = send(s, buf + off, n - off, 0);
            if (w <= 0) { rc = -5; break; }
            off += (size_t)w;
        }
    }
    free(buf);
    fclose(f);
    close(s);
    return rc;
}

void handle_self_update(client_session_t *session, const char *arg) {
    if (!arg || !*arg) { send_error(session->sock, "No ELF path or URL provided"); return; }

    // "<src>|<loader_port>" — the port suffix is optional.
    char src[1400];
    int loader_port = UPDATE_LOADER_PORT;
    snprintf(src, sizeof(src), "%s", arg);
    char *bar = strrchr(src, '|');
    if (bar) {
        int p = atoi(bar + 1);
        if (p > 0 && p < 65536) loader_port = p;
        *bar = 0;
    }

    mkdir(SUITE_DIR, 0755);   // fine if it already exists

    char staged[PATH_MAX];
    if (!strncmp(src, "http://", 7)) {
        snprintf(staged, sizeof(staged), "%s", UPDATE_STAGE_PATH);
        int rc = http_get_to_file(src, staged, 0);
        if (rc != 0) {
            char m[128];
            snprintf(m, sizeof(m), "Download failed (rc=%d)", rc);
            send_error(session->sock, m);
            return;
        }
    } else {
        snprintf(staged, sizeof(staged), "%s", src);
    }

    char new_ver[32];
    int vrc = self_update_verify(staged, new_ver, sizeof(new_ver));
    if (vrc != 0) {
        char m[200];
        snprintf(m, sizeof(m), "Not a loadable ELF (rc=%d): %s", vrc, staged);
        send_error(session->sock, m);
        return;
    }

    // Handoff marker for the incoming instance: it forces a full stale sweep
    // (the old instance may hold a FALLBACK port, invisible to the bind-fail
    // path) and produces the "updated" notification. Timestamped — a marker
    // left by a failed update is ignored after 2 minutes.
    {
        FILE *mf = fopen(UPDATE_MARKER, "w");
        if (mf) {
            fprintf(mf, "ts=%lld\nprev=%s\nnext=%s\n",
                    (long long)time(NULL), SUITE_VERSION,
                    new_ver[0] ? new_ver : "unknown");
            fclose(mf);
        }
    }

    // Try the supplied loader port, then the common elfldr ports.
    int ports[3] = { loader_port, 9021, 9020 };
    int used = 0;
    for (int i = 0; i < 3; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) if (ports[j] == ports[i]) dup = 1;
        if (dup) continue;
        if (push_file_to_loader(staged, ports[i]) == 0) { used = ports[i]; break; }
    }
    if (!used) {
        unlink(UPDATE_MARKER);
        send_error(session->sock,
            "No local payload loader answered (tried supplied/9021/9020)");
        return;
    }

    char m[200];
    snprintf(m, sizeof(m), "UPDATE_LAUNCHED|%s|port=%d",
             new_ver[0] ? new_ver : "unknown", used);
    send_response(session->sock, RESP_OK, m, strlen(m));
    // The spawned instance sweeps stale "payload*" processes — including this
    // one — then takes the port. Nothing left to do; the connection dies here.
}

// ============================================================================
// MEMORY EDITOR — ptrace/PT_IO read-write-search on arbitrary processes.
// Elevated (uid 0 + ShellCore authid) so PT_ATTACH works on most targets.
// Attach → I/O → detach per call; search holds the attach for its whole scan.
// ============================================================================

static int mem_attach(pid_t pid) {
    if (ptrace(PT_ATTACH, pid, 0, 0) != 0) return -1;
    if (waitpid(pid, NULL, 0) < 0) { ptrace(PT_DETACH, pid, 0, 0); return -2; }
    return 0;
}

static int mem_io(pid_t pid, uint64_t addr, void *buf, size_t len, int op) {
    struct ptrace_io_desc iod;
    memset(&iod, 0, sizeof(iod));
    iod.piod_op   = op;
    iod.piod_offs = (void *)(uintptr_t)addr;
    iod.piod_addr = buf;
    iod.piod_len  = len;
    if (ptrace(PT_IO, pid, (caddr_t)&iod, 0) != 0) return -1;
    return (int)iod.piod_len;   // kernel reports bytes actually transferred
}

static void mem_detach(pid_t pid) { ptrace(PT_DETACH, pid, 0, 0); }

// "pid|field2|field3" — split helper returning pointers past each '|'.
static const char *mem_next_field(const char *p) {
    const char *b = strchr(p, '|');
    return b ? b + 1 : NULL;
}

// memmem substitute — prospero libc lacks it. First-byte skip + memcmp.
static uint8_t *mem_find(uint8_t *hay, size_t haylen, const uint8_t *pat, size_t plen) {
    if (plen == 0 || haylen < plen) return NULL;
    uint8_t *end = hay + haylen - plen + 1;
    for (uint8_t *p = hay; p < end; ) {
        p = memchr(p, pat[0], (size_t)(end - p));
        if (!p) return NULL;
        if (!memcmp(p + 1, pat + 1, plen - 1)) return p;
        p++;
    }
    return NULL;
}

static int hex_decode(const char *hex, uint8_t *out, size_t out_sz) {
    size_t n = 0;
    while (hex[0] && hex[1] && n < out_sz) {
        unsigned v;
        if (sscanf(hex, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    return (int)n;
}

void handle_mem_read(client_session_t *session, const char *arg) {
    const char *a2 = mem_next_field(arg), *a3 = a2 ? mem_next_field(a2) : NULL;
    if (!a2 || !a3) { send_error(session->sock, "Usage: pid|addr|len"); return; }
    pid_t pid = (pid_t)strtol(arg, NULL, 0);
    uint64_t addr = strtoull(a2, NULL, 0);
    long len = strtol(a3, NULL, 0);
    if (pid <= 0 || len <= 0 || len > 256 * 1024) {
        send_error(session->sock, "Bad pid/len (len ≤ 256KB)");
        return;
    }
    uint8_t *buf = malloc(len);
    if (!buf) { send_error(session->sock, "no memory"); return; }

    int got = -3;
    if (mem_attach(pid) == 0) {
        got = mem_io(pid, addr, buf, (size_t)len, PIOD_READ_D);
        mem_detach(pid);
    }
    if (got > 0) {
        send_response(session->sock, RESP_DATA, buf, (size_t)got);
    } else {
        send_error(session->sock, "Read failed (attach or unmapped address)");
    }
    free(buf);
}

void handle_mem_write(client_session_t *session, const char *arg) {
    const char *a2 = mem_next_field(arg), *a3 = a2 ? mem_next_field(a2) : NULL;
    if (!a2 || !a3) { send_error(session->sock, "Usage: pid|addr|hexdata"); return; }
    pid_t pid = (pid_t)strtol(arg, NULL, 0);
    uint64_t addr = strtoull(a2, NULL, 0);
    if (pid <= 0) { send_error(session->sock, "Bad pid"); return; }
    if (strlen(a3) > 128 * 1024) { send_error(session->sock, "Write data too large (≤64KB)"); return; }

    uint8_t *buf = malloc(strlen(a3) / 2 + 1);
    if (!buf) { send_error(session->sock, "no memory"); return; }
    int n = hex_decode(a3, buf, strlen(a3) / 2 + 1);
    if (n <= 0) { free(buf); send_error(session->sock, "Empty/invalid hex data"); return; }

    int rc = -3;
    if (mem_attach(pid) == 0) {
        rc = mem_io(pid, addr, buf, (size_t)n, PIOD_WRITE_D);
        mem_detach(pid);
    }
    free(buf);
    if (rc > 0) {
        char m[64];
        snprintf(m, sizeof(m), "OK:wrote %d bytes @ 0x%llx", rc, (unsigned long long)addr);
        send_response(session->sock, RESP_OK, m, strlen(m));
    } else {
        send_error(session->sock, "Write failed (attach or protected page)");
    }
}

void handle_mem_regions(client_session_t *session, const char *arg) {
    pid_t pid = (pid_t)strtol(arg, NULL, 0);
    if (pid <= 0) { send_error(session->sock, "Usage: pid"); return; }

    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, pid };
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) {
        send_error(session->sock, "vmmap failed (pid?)");
        return;
    }
    size += size / 4;
    char *buf = malloc(size);
    if (!buf) { send_error(session->sock, "no memory"); return; }
    if (sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
        free(buf);
        send_error(session->sock, "vmmap read failed");
        return;
    }

    size_t cap = size + 4096;
    char *out = malloc(cap);
    if (!out) { free(buf); send_error(session->sock, "no memory"); return; }
    int off = 0;
    for (char *p = buf; p + sizeof(int) <= buf + size && off < (int)cap - 512; ) {
        struct kinfo_vmentry *kv = (struct kinfo_vmentry *)p;
        if (kv->kve_structsize <= 0) break;
        p += kv->kve_structsize;
        char prot[4] = "---";
        if (kv->kve_protection & PROT_READ)  prot[0] = 'r';
        if (kv->kve_protection & PROT_WRITE) prot[1] = 'w';
        if (kv->kve_protection & PROT_EXEC)  prot[2] = 'x';
        off += snprintf(out + off, cap - off, "%llx-%llx|%s|%lld|%s\n",
            (unsigned long long)kv->kve_start, (unsigned long long)kv->kve_end,
            prot, (long long)(kv->kve_end - kv->kve_start), kv->kve_path);
    }
    send_response(session->sock, RESP_DATA, out, (size_t)off);
    free(out);
    free(buf);
}

void handle_mem_search(client_session_t *session, const char *arg) {
    const char *a2 = mem_next_field(arg),
              *a3 = a2 ? mem_next_field(a2) : NULL,
              *a4 = a3 ? mem_next_field(a3) : NULL;
    if (!a2 || !a3 || !a4) { send_error(session->sock, "Usage: pid|start|end|hexpattern"); return; }
    pid_t pid = (pid_t)strtol(arg, NULL, 0);
    uint64_t start = strtoull(a2, NULL, 0), end = strtoull(a3, NULL, 0);
    if (pid <= 0 || end <= start || end - start > (512ull << 20)) {
        send_error(session->sock, "Bad range (≤512MB)");
        return;
    }
    uint8_t pat[64];
    int plen = hex_decode(a4, pat, sizeof(pat));
    if (plen <= 0) { send_error(session->sock, "Empty/invalid hex pattern"); return; }

    if (mem_attach(pid) != 0) { send_error(session->sock, "Attach failed"); return; }

    const size_t CHUNK = 256 * 1024;
    uint8_t *buf = malloc(CHUNK + 63);
    size_t out_cap = 96 * 1024;
    char *out = malloc(out_cap);
    int off = 0, found = 0, truncated = 0;
    time_t deadline = time(NULL) + 30;
    size_t tail = 0;   // bytes carried over from the previous chunk

    for (uint64_t pos = start; pos < end && buf && out && off < (int)out_cap - 64; ) {
        size_t want = (size_t)((end - pos) > CHUNK ? CHUNK : (end - pos));
        int got = mem_io(pid, pos, buf + tail, want, PIOD_READ_D);
        if (got > 0) {
            size_t span = tail + (size_t)got;
            uint8_t *scan = buf, *hit;
            while ((hit = mem_find(scan, span - (size_t)(scan - buf), pat, plen)) != NULL) {
                off += snprintf(out + off, out_cap - off, "0x%llx\n",
                    (unsigned long long)(pos - tail + (hit - buf)));
                found++;
                if (found >= 1024) { truncated = 1; break; }
                scan = hit + 1;
            }
            // carry tail so a pattern spanning the boundary is still caught
            tail = (span >= (size_t)plen - 1) ? (size_t)plen - 1 : span;
            memcpy(buf, buf + span - tail, tail);
        } else {
            tail = 0;   // read failed (unmapped gap) — no cross-gap carry
        }
        pos += want;
        if (truncated || time(NULL) > deadline) { if (!truncated) truncated = 2; break; }
    }
    mem_detach(pid);
    if (truncated == 1) off += snprintf(out + off, out_cap - off, "TRUNCATED\n");
    else if (truncated == 2) off += snprintf(out + off, out_cap - off, "TIMEOUT\n");
    if (found == 0 && off == 0)
        off += snprintf(out + off, out_cap - off, "NO_MATCHES\n");
    if (out) send_response(session->sock, RESP_DATA, out, (size_t)off);
    free(out);
    free(buf);
}

// ============================================================================
// APP MANAGER v2 — real per-app info + suspend/resume/kill/coredump.
// Sony APIs resolved at RUNTIME by NID on already-loaded modules: a missing
// export degrades the feature instead of breaking ELF launch. LncUtil calls
// are daemon IPC → they go through wdg_fn_call (kernel-IPC wedge protection)
// behind the etahen_present() gate, same as install/launch.
// ============================================================================

typedef struct {
    uint32_t app_id;
    uint64_t unknown1;
    uint32_t app_type;
    char     title_id[10];
    char     unknown2[0x3c];
} app_info_t;

static int (*g_sceKernelGetAppInfo)(pid_t, app_info_t*) = NULL;
static int (*g_sceLncUtilSuspendApp)(int, uint32_t, void*) = NULL;
static int (*g_sceLncUtilResumeApp)(int, uint32_t, void*) = NULL;
static int (*g_sceLncUtilIsAppSuspended)(int) = NULL;
static int (*g_sceLncUtilKillApp)(int) = NULL;
static int (*g_sceLncUtilForceKillApp)(int) = NULL;
static int (*g_sceLncUtilKickCoredumpOnlyProcMem)(int) = NULL;
static int g_appmgr_resolved = 0;
static pthread_mutex_t g_appmgr_lock = PTHREAD_MUTEX_INITIALIZER;

static void resolve_appmgr_functions(void) {
    if (g_appmgr_resolved) return;
    pthread_mutex_lock(&g_appmgr_lock);
    if (g_appmgr_resolved) { pthread_mutex_unlock(&g_appmgr_lock); return; }

    char nid[12];
    // pid → app mapping lives in libkernel_sys (the _web variant stubs it)
    uint32_t kh = find_kernel_module_handle();
    {
        uint32_t hs = 0;
        if (krpc_dynlib_handle(getpid(), "libkernel_sys.sprx", &hs) == 0 && hs)
            kh = hs;
    }
    if (kh) {
        nid_encode("sceKernelGetAppInfo", nid);
        void *p = krpc_resolve_sym(getpid(), kh, nid);
        if (p) g_sceKernelGetAppInfo = (int (*)(pid_t, app_info_t*))p;
    }

    // libSceSystemService is already NEEDED by this ELF (LaunchApp import)
    // → the module is loaded; resolve only, never load.
    uint32_t sh = 0;
    static const char *svc_names[] = {
        "libSceSystemService.sprx", "libSceSystemService",
        "libSceSystemServiceCore.sprx", NULL
    };
    for (int i = 0; svc_names[i] && !sh; i++)
        if (krpc_dynlib_handle(getpid(), svc_names[i], &sh) != 0) sh = 0;
    if (sh) {
#define SVC_RES(field, name, type)                                          \
        do {                                                                \
            nid_encode(name, nid);                                          \
            void *p_ = krpc_resolve_sym(getpid(), sh, nid);                 \
            if (p_) field = (type)p_;                                       \
        } while (0)
        SVC_RES(g_sceLncUtilSuspendApp, "sceLncUtilSuspendApp", int (*)(int, uint32_t, void*));
        SVC_RES(g_sceLncUtilResumeApp, "sceLncUtilResumeApp", int (*)(int, uint32_t, void*));
        SVC_RES(g_sceLncUtilIsAppSuspended, "sceLncUtilIsAppSuspended", int (*)(int));
        SVC_RES(g_sceLncUtilKillApp, "sceLncUtilKillApp", int (*)(int));
        SVC_RES(g_sceLncUtilForceKillApp, "sceLncUtilForceKillApp", int (*)(int));
        SVC_RES(g_sceLncUtilKickCoredumpOnlyProcMem, "sceLncUtilKickCoredumpOnlyProcMem", int (*)(int));
#undef SVC_RES
    }

    g_appmgr_resolved = 1;
    pthread_mutex_unlock(&g_appmgr_lock);
}

// Daemon-IPC call wrapper: 3 args max, watchdog-contained, etaHEN-gated.
static int lnc_call3(void *fn, int a, uint32_t b, void *c) {
    if (!fn) return -1000;
    if (!etahen_present()) return -1001;
    return wdg_fn_call(fn, (void*)(intptr_t)a, (void*)(uintptr_t)b, c, NULL, 15000);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Per-process CPU% computed from ki_runtime deltas between list calls —
// ki_pctcpu is not populated by the Orbis scheduler (always 0), but
// ki_runtime (µs of CPU consumed, all threads) works: cpu% = Δruntime/Δwall.
typedef struct { pid_t pid; uint64_t runtime_us; } applist_cpu_t;
static applist_cpu_t g_applist_cpu[512];
static int g_applist_cpu_n = 0;
static uint64_t g_applist_cpu_ms = 0;
static pthread_mutex_t g_applist_lock = PTHREAD_MUTEX_INITIALIZER;

// GetAppInfo results don't change while a process lives — cache keyed on
// pid+start-time (guards against pid reuse) so repeat refreshes stay fast.
typedef struct {
    pid_t    pid;
    long     start_sec;
    app_info_t info;
} appinfo_cache_t;
static appinfo_cache_t g_appinfo_cache[512];
static int g_appinfo_cache_n = 0;
static int g_appinfo_cache_head = 0;

static int appinfo_lookup(pid_t pid, long start_sec, app_info_t *out) {
    if (!g_sceKernelGetAppInfo) return -1;
    for (int i = 0; i < g_appinfo_cache_n; i++) {
        if (g_appinfo_cache[i].pid == pid &&
            g_appinfo_cache[i].start_sec == start_sec) {
            *out = g_appinfo_cache[i].info;
            return 0;
        }
    }
    app_info_t ai;
    memset(&ai, 0, sizeof(ai));
    int ok = (g_sceKernelGetAppInfo(pid, &ai) == 0);
    if (!ok) ai.app_id = 0xFFFFFFFFu;   // negative-cache marker — still cached
    int slot = (g_appinfo_cache_n < 512)
        ? g_appinfo_cache_n++
        : (g_appinfo_cache_head++ % 512);
    g_appinfo_cache[slot].pid = pid;
    g_appinfo_cache[slot].start_sec = start_sec;
    g_appinfo_cache[slot].info = ai;
    *out = ai;
    return ok ? 0 : -1;
}

void handle_app_list_v2(client_session_t *session) {
    resolve_appmgr_functions();

    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) {
        send_error(session->sock, "sysctl failed");
        return;
    }
    size += size / 8;
    char *buf = malloc(size);
    char *out = malloc(size / 2 + 8192);
    if (!buf || !out) { free(buf); free(out); send_error(session->sock, "no memory"); return; }

    int off = 0;
    off += snprintf(out + off, size / 2 + 8192 - off, "appinfo=%s\n",
        g_sceKernelGetAppInfo ? "ok" : "unavailable");
    if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
        // snapshot pass 1: current runtime per pid
        uint64_t now = now_ms();
        applist_cpu_t *cur = malloc(sizeof(applist_cpu_t) * 512);
        int cur_n = 0;

        for (char *p = buf; p + sizeof(int) <= buf + size && off < (int)(size / 2 + 8192) - 256; ) {
            struct kinfo_proc *ki = (struct kinfo_proc *)p;
            if (ki->ki_structsize <= 0) break;
            p += ki->ki_structsize;

            app_info_t ai;
            if (appinfo_lookup(ki->ki_pid, ki->ki_start.tv_sec, &ai) != 0)
                continue;
            if (ai.app_id == 0xFFFFFFFFu ||
                (ai.app_id == 0 && ai.title_id[0] == '\0'))
                continue;   // kernel/system procs — not apps
            ai.title_id[9] = '\0';

            if (cur_n < 512) { cur[cur_n].pid = ki->ki_pid; cur[cur_n].runtime_us = ki->ki_runtime; cur_n++; }

            // cpu% = Δruntime / Δwall since previous sample (×100 for 2 decimals)
            unsigned cpu_x100 = 0;
            pthread_mutex_lock(&g_applist_lock);
            uint64_t dt_ms = (now > g_applist_cpu_ms) ? now - g_applist_cpu_ms : 0;
            if (dt_ms >= 500) {
                for (int i = 0; i < g_applist_cpu_n; i++) {
                    if (g_applist_cpu[i].pid == ki->ki_pid) {
                        uint64_t d_us = (ki->ki_runtime > g_applist_cpu[i].runtime_us)
                            ? ki->ki_runtime - g_applist_cpu[i].runtime_us : 0;
                        cpu_x100 = (unsigned)((d_us * 10000ull) / (dt_ms * 1000ull));
                        break;
                    }
                }
            }
            pthread_mutex_unlock(&g_applist_lock);

            // SSTOP flag is free (ki_stat); per-app IsAppSuspended daemon
            // calls were removed — they were the source of multi-second lag.
            int susp = (ki->ki_stat == 4) ? 1 : 0;
            off += snprintf(out + off, size / 2 + 8192 - off,
                "%d|%u|%s|%s|%u|%u|%d\n",
                ki->ki_pid, ai.app_id, ai.title_id, ki->ki_comm,
                ai.app_type, cpu_x100, susp);
        }

        pthread_mutex_lock(&g_applist_lock);
        memcpy(g_applist_cpu, cur, sizeof(applist_cpu_t) * cur_n);
        g_applist_cpu_n = cur_n;
        g_applist_cpu_ms = now;
        pthread_mutex_unlock(&g_applist_lock);
        free(cur);
    }
    send_response(session->sock, RESP_DATA, out, off);
    free(out);
    free(buf);
}

// "appid|pid" — LncUtil suspend/resume only works for ShellCore-managed app
// states, so the reliable path is process signals: SIGSTOP freezes every
// thread of the game (same mechanism the debugger uses), SIGCONT resumes.
void handle_app_suspend(client_session_t *session, const char *arg) {
    int appid = (int)strtol(arg ? arg : "", NULL, 0);
    const char *p2 = arg ? mem_next_field(arg) : NULL;
    pid_t pid = p2 ? (pid_t)strtol(p2, NULL, 0) : 0;
    resolve_appmgr_functions();
    int sig = pid > 0 ? kill(pid, SIGSTOP) : -1;
    int lnc = appid > 0 ? lnc_call3((void*)g_sceLncUtilSuspendApp, appid, 0, NULL) : -1000;
    char m[160];
    snprintf(m, sizeof(m), "suspend: sigstop=%s lnc=%d",
        sig == 0 ? "ok" : strerror(errno), lnc);
    if (sig == 0 || lnc == 0) send_response(session->sock, RESP_OK, m, strlen(m));
    else                      send_error(session->sock, m);
}

void handle_app_resume(client_session_t *session, const char *arg) {
    int appid = (int)strtol(arg ? arg : "", NULL, 0);
    const char *p2 = arg ? mem_next_field(arg) : NULL;
    pid_t pid = p2 ? (pid_t)strtol(p2, NULL, 0) : 0;
    resolve_appmgr_functions();
    int sig = pid > 0 ? kill(pid, SIGCONT) : -1;
    int lnc = appid > 0 ? lnc_call3((void*)g_sceLncUtilResumeApp, appid, 0, NULL) : -1000;
    char m[160];
    snprintf(m, sizeof(m), "resume: sigcont=%s lnc=%d",
        sig == 0 ? "ok" : strerror(errno), lnc);
    if (sig == 0 || lnc == 0) send_response(session->sock, RESP_OK, m, strlen(m));
    else                      send_error(session->sock, m);
}

void handle_app_coredump(client_session_t *session, const char *arg) {
    int appid = (int)strtol(arg ? arg : "", NULL, 0);
    if (appid <= 0) { send_error(session->sock, "Usage: appid|pid"); return; }
    resolve_appmgr_functions();
    int rc = lnc_call3((void*)g_sceLncUtilKickCoredumpOnlyProcMem, appid, 0, NULL);
    char m[192];
    if (rc == -1000)      snprintf(m, sizeof(m), "coredump unavailable on this module");
    else if (rc == -1001) snprintf(m, sizeof(m), "coredump needs etaHEN (daemon IPC)");
    else if (rc == 0)     snprintf(m, sizeof(m), "OK:coredump kicked appid=%d — writes process mem to the system dump dir", appid);
    else                  snprintf(m, sizeof(m), "coredump appid=%d rc=%d (SCE err)", appid, rc);
    if (rc == 0) send_response(session->sock, RESP_OK, m, strlen(m));
    else         send_error(session->sock, m);
}

void handle_app_kill(client_session_t *session, const char *arg) {
    int appid = (int)strtol(arg ? arg : "", NULL, 0);
    const char *p2 = arg ? mem_next_field(arg) : NULL;
    pid_t pid = p2 ? (pid_t)strtol(p2, NULL, 0) : 0;
    if (appid <= 0 && pid <= 0) { send_error(session->sock, "Usage: appid|pid"); return; }
    resolve_appmgr_functions();
    int rc = appid > 0 ? lnc_call3((void*)g_sceLncUtilForceKillApp, appid, 0, NULL) : -1000;
    if (rc != 0 && g_sceLncUtilKillApp && appid > 0)
        rc = lnc_call3((void*)g_sceLncUtilKillApp, appid, 0, NULL);
    // Ultimate fallback — a signal the process cannot refuse.
    if (rc != 0 && pid > 0)
        rc = (kill(pid, SIGKILL) == 0) ? 0 : -errno;
    char m[160];
    snprintf(m, sizeof(m), "kill appid=%d pid=%d rc=%d", appid, pid, rc);
    if (rc == 0) send_response(session->sock, RESP_OK, m, strlen(m));
    else         send_error(session->sock, m);
}

// ============================================================================
// NETWORK INFO — sceNetCtl (linked) for connection state / WiFi / NAT +
// getifaddrs if_data for per-interface byte counters (live bandwidth).
// ============================================================================
extern int sceNetCtlInit(void);
extern int sceNetCtlGetState(int *state);
extern int sceNetCtlGetInfo(int code, void *info);
// NOTE: sceNetCtlGetNatInfo is intentionally unused — it performs a real STUN
// lookup against Sony servers (unreachable without PSN) and hangs inside the
// library, blocking/crashing the process even from a detached worker thread.

#define NETCTL_INFO_DEVICE        1
#define NETCTL_INFO_ETHER_ADDR    2
#define NETCTL_INFO_MTU           3
#define NETCTL_INFO_LINK          4
#define NETCTL_INFO_BSSID         5
#define NETCTL_INFO_SSID          6
#define NETCTL_INFO_WIFI_SECURITY 7
#define NETCTL_INFO_RSSI          8
#define NETCTL_INFO_IP_CONFIG     9
#define NETCTL_INFO_IP_ADDRESS    12
#define NETCTL_INFO_NETMASK       13
#define NETCTL_INFO_DEFAULT_ROUTE 14
#define NETCTL_INFO_PRIMARY_DNS   15
#define NETCTL_INFO_SECONDARY_DNS 16

// Every sceNetCtl call goes through wdg_fn_call: the API talks to the netctl
// service over IPC and GetNatInfo performs a real STUN lookup — either can
// block for many seconds or crash. A timed-out function is marked "dead" and
// skipped instantly on later calls.
#define NETCTL_CALL_TIMEOUT_MS 3000
#define NETCTL_CACHE_MS        30000

static int g_netctl_inited = -1;   // -1 unknown, 0 unavailable, 1 ok

// SceNetCtlInfo is a union (~1KB): GetInfo writes the WHOLE union regardless
// of code, so the buffer must be union-sized or the call smashes the stack.
#define NETCTL_INFO_BUFSZ 2048

static int netctl_getinfo(int code, void *buf) {
    return wdg_fn_call((void*)sceNetCtlGetInfo, (void*)(intptr_t)code, buf,
                       NULL, NULL, NETCTL_CALL_TIMEOUT_MS);
}

static void netctl_str(char *out, int *off, int cap, int code, const char *key) {
    static char buf[NETCTL_INFO_BUFSZ];
    memset(buf, 0, sizeof(buf));
    if (netctl_getinfo(code, buf) != 0) return;
    buf[63] = 0;
    *off += snprintf(out + *off, cap - *off, "%s=%s\n", key, buf);
}
static void netctl_int(char *out, int *off, int cap, int code, const char *key) {
    static char buf[NETCTL_INFO_BUFSZ];
    memset(buf, 0, sizeof(buf));
    if (netctl_getinfo(code, buf) != 0) return;
    int v;
    memcpy(&v, buf, sizeof(v));
    *off += snprintf(out + *off, cap - *off, "%s=%d\n", key, v);
}
static void netctl_mac(char *out, int *off, int cap, int code, const char *key) {
    static char buf[NETCTL_INFO_BUFSZ];
    memset(buf, 0, sizeof(buf));
    if (netctl_getinfo(code, buf) != 0) return;
    *off += snprintf(out + *off, cap - *off, "%s=%02x:%02x:%02x:%02x:%02x:%02x\n",
        key, buf[0]&0xff, buf[1]&0xff, buf[2]&0xff, buf[3]&0xff, buf[4]&0xff, buf[5]&0xff);
}

// Cached blob of the netctl-derived fields — refreshed at most every 30s so
// per-tick polls stay cheap (getifaddrs counters are always fresh).
static char      g_netctl_blob[4096];
static int       g_netctl_blob_len = 0;
static long long g_netctl_blob_time = 0;

static void netctl_refresh_blob(void) {
    int off = 0;
    g_netctl_blob[0] = 0;

    if (g_netctl_inited < 0)
        g_netctl_inited =
            (wdg_fn_call((void*)sceNetCtlInit, NULL, NULL, NULL, NULL,
                         NETCTL_CALL_TIMEOUT_MS) == 0) ? 1 : 0;

    if (!g_netctl_inited) {
        off += snprintf(g_netctl_blob + off, sizeof(g_netctl_blob) - off,
                        "netctl=unavailable\n");
    } else {
        char *out = g_netctl_blob;
        int cap = sizeof(g_netctl_blob);
        int state = -1;
        if (wdg_fn_call((void*)sceNetCtlGetState, &state, NULL, NULL, NULL,
                        NETCTL_CALL_TIMEOUT_MS) == 0)
            off += snprintf(out + off, cap - off, "state=%d\n", state);  // 0=off,1=connecting,2=online
        netctl_int(out, &off, cap, NETCTL_INFO_DEVICE,   "device");
        netctl_mac(out, &off, cap, NETCTL_INFO_ETHER_ADDR, "mac");
        netctl_int(out, &off, cap, NETCTL_INFO_MTU,      "mtu");
        netctl_int(out, &off, cap, NETCTL_INFO_LINK,     "link");
        netctl_str(out, &off, cap, NETCTL_INFO_SSID,     "ssid");
        netctl_mac(out, &off, cap, NETCTL_INFO_BSSID,    "bssid");
        netctl_int(out, &off, cap, NETCTL_INFO_WIFI_SECURITY, "wifi_sec");
        netctl_int(out, &off, cap, NETCTL_INFO_RSSI,     "rssi");
        netctl_int(out, &off, cap, NETCTL_INFO_IP_CONFIG,"ip_config");
        // Orbis info-code numbering differs from PS4 docs: emit every IPv4-
        // shaped field for codes 12..20 (fN=a.b.c.d) and let the client
        // classify ip/netmask/gateway/dns. ip= stays at code 12.
        netctl_str(out, &off, cap, NETCTL_INFO_IP_ADDRESS,   "ip");
        for (int code = 13; code <= 20; code++) {
            static char buf[NETCTL_INFO_BUFSZ];
            memset(buf, 0, sizeof(buf));
            if (netctl_getinfo(code, buf) != 0) continue;
            buf[63] = 0;
            // keep only strict a.b.c.d values — skips junk/proxy fields
            int d1 = 0, d2 = 0, d3 = 0, d4 = 0; char tail = 0;
            if (sscanf(buf, "%d.%d.%d.%d%c", &d1, &d2, &d3, &d4, &tail) == 4 &&
                d1 <= 255 && d2 <= 255 && d3 <= 255 && d4 <= 255 &&
                d1 >= 0 && d2 >= 0 && d3 >= 0 && d4 >= 0)
                off += snprintf(out + off, cap - off, "f%d=%s\n", code, buf);
        }
        // NAT/STUN is not reachable without PSN — reported as unavailable.
    }

    g_netctl_blob_len = off;
    g_netctl_blob_time = now_ms();
}

// Interface byte counters: getifaddrs AF_LINK first (standard BSD), then the
// routing-socket sysctl NET_RT_IFLIST as a fallback — Orbis getifaddrs may
// return no AF_LINK entries, but the kernel still answers the sysctl.
static void net_if_counters(char *out, int *off, int cap) {
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK || !p->ifa_data)
                continue;
            struct if_data *ifd = (struct if_data *)p->ifa_data;
            if (*off < cap - 80)
                *off += snprintf(out + *off, cap - *off, "if=%s|rx=%llu|tx=%llu\n",
                    p->ifa_name,
                    (unsigned long long)ifd->ifi_ibytes,
                    (unsigned long long)ifd->ifi_obytes);
        }
        freeifaddrs(ifa);
        if (strstr(out, "if=")) return;
    }

    int mib[6] = { CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST, 0 };
    size_t len = 0;
    if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0 || len == 0 || len > (1 << 20))
        return;
    char *buf = malloc(len);
    if (!buf) return;
    if (sysctl(mib, 6, buf, &len, NULL, 0) == 0) {
        for (char *p = buf, *end = buf + len; p < end; ) {
            struct if_msghdr *ifm = (struct if_msghdr *)p;
            if (ifm->ifm_msglen == 0) break;
            if (ifm->ifm_type == RTM_IFINFO) {
                char name[16] = "?";
                struct sockaddr_dl *sdl =
                    (struct sockaddr_dl *)(p + sizeof(struct if_msghdr));
                if ((char *)sdl < end && sdl->sdl_family == AF_LINK &&
                    sdl->sdl_nlen < sizeof(name)) {
                    memcpy(name, sdl->sdl_data, sdl->sdl_nlen);
                    name[sdl->sdl_nlen] = 0;
                }
                if (*off < cap - 80)
                    *off += snprintf(out + *off, cap - *off,
                        "if=%s|rx=%llu|tx=%llu\n", name,
                        (unsigned long long)ifm->ifm_data.ifi_ibytes,
                        (unsigned long long)ifm->ifm_data.ifi_obytes);
            }
            p += ifm->ifm_msglen;
        }
    }
    free(buf);
}

void handle_net_info(client_session_t *session) {
    char *out = malloc(4096);
    if (!out) { send_error(session->sock, "no memory"); return; }
    int off = 0;

    if (g_netctl_blob_len == 0 || now_ms() - g_netctl_blob_time > NETCTL_CACHE_MS)
        netctl_refresh_blob();
    memcpy(out, g_netctl_blob, g_netctl_blob_len);
    off = g_netctl_blob_len;

    // Interface byte counters (live bandwidth source — client computes rate).
    // getifaddrs/sysctl can hang on Orbis — run them under the watchdog too.
    struct { char *out; int *off; int cap; } nc = { out, &off, 4096 };
    wdg_fn_call((void*)net_if_counters, nc.out, nc.off, (void*)(intptr_t)nc.cap,
                NULL, 5000);

    send_response(session->sock, RESP_DATA, out, off);
    free(out);
}

// ============================================================================
// LINK SPEED TEST — streams 16MB to the client; client measures receive rate.
// ============================================================================
#define SPEEDTEST_BYTES (16u * 1024 * 1024)
void handle_net_speedtest(client_session_t *session) {
    uint8_t *buf = malloc(SPEEDTEST_BYTES);
    if (!buf) { send_error(session->sock, "out of memory"); return; }
    memset(buf, 0x5A, SPEEDTEST_BYTES);
    send_response(session->sock, RESP_DATA, buf, SPEEDTEST_BYTES);
    free(buf);
}

// ============================================================================
// KERNEL LOG — read the kernel message buffer (dmesg equivalent) via
// sysctl kern.msgbuf. Arg = optional tail size in bytes (0 = whole buffer).
// ============================================================================
#define KLOG_MAX (512 * 1024)
void handle_klog_read(client_session_t *session, const char *arg) {
    long want = (arg && *arg) ? strtol(arg, NULL, 0) : 0;
    size_t sz = 0;
    if (sysctlbyname("kern.msgbuf", NULL, &sz, NULL, 0) != 0 || sz == 0) {
        send_error(session->sock, "kern.msgbuf unavailable on this kernel");
        return;
    }
    if (sz > KLOG_MAX) sz = KLOG_MAX;
    char *buf = malloc(sz + 1);
    if (!buf) { send_error(session->sock, "Out of memory"); return; }
    size_t got = sz;
    if (sysctlbyname("kern.msgbuf", buf, &got, NULL, 0) != 0 || got == 0) {
        free(buf);
        send_error(session->sock, "kern.msgbuf read failed (denied?)");
        return;
    }
    size_t off = 0;
    if (want > 0 && (size_t)want < got) off = got - (size_t)want;
    send_response(session->sock, RESP_DATA, buf + off, got - off);
    free(buf);
}


// ============================================================================
// POWER ACTION — reboot/shutdown via the reboot() syscall. We run as uid 0
// after elevation; it is a plain syscall (no daemon IPC) so it works under
// every loader. The actual reboot runs on a detached thread a few hundred ms
// later so the RESP_OK reaches the client before the console goes down.
// ============================================================================
static void *reboot_thread(void *arg) {
    int how = (int)(intptr_t)arg;
    usleep(400 * 1000);
    sync();
    reboot(how);
    return NULL;    // unreachable unless reboot() failed
}

void handle_power_action(client_session_t *session, const char *arg) {
    int how;
    if (arg && !strcmp(arg, "reboot")) {
        how = RB_AUTOBOOT;
    } else if (arg && !strcmp(arg, "shutdown")) {
        how = RB_HALT | RB_POWEROFF;
    } else {
        send_error(session->sock, "usage: reboot|shutdown");
        return;
    }
    send_response(session->sock, RESP_OK, arg, strlen(arg));
    pthread_t t;
    if (pthread_create(&t, NULL, reboot_thread, (void *)(intptr_t)how) == 0)
        pthread_detach(t);
}

// ============================================================================
// USB LIST — mounted USB drives. Enumerated through getfsstat so it works
// without any Sony storage daemon IPC: /mnt/usb* paths, /dev/da* nodes, and
// removable filesystems (exfat/msdosfs) all count.
// ============================================================================
void handle_usb_list(client_session_t *session) {
    char *out = malloc(4096);
    if (!out) { send_error(session->sock, "Out of memory"); return; }
    int off = 0;
    struct statfs *mnt = NULL;
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n > 0) {
        mnt = malloc((size_t)n * sizeof(*mnt));
        if (mnt && getfsstat(mnt, (long)(n * sizeof(*mnt)), MNT_NOWAIT) > 0) {
            for (int i = 0; i < n && off < 3900; i++) {
                struct statfs *m = &mnt[i];
                int usb = !strncmp(m->f_mntonname, "/mnt/usb", 8) ||
                          !strncmp(m->f_mntfromname, "/dev/da", 7) ||
                          !strncmp(m->f_mntfromname, "/dev/nvd", 8) ||
                          strstr(m->f_mntfromname, "usb") != NULL ||
                          !strcmp(m->f_fstypename, "msdosfs") ||
                          !strcmp(m->f_fstypename, "exfat");
                if (!usb) continue;
                off += snprintf(out + off, 4096 - off, "%s|%s|%s|%llu|%llu\n",
                    m->f_mntonname, m->f_fstypename, m->f_mntfromname,
                    (unsigned long long)m->f_blocks * m->f_bsize,
                    (unsigned long long)m->f_bavail * m->f_bsize);
            }
        }
        free(mnt);
    }
    if (off == 0) off = snprintf(out, 4096, "NONE\n");
    send_response(session->sock, RESP_DATA, out, off);
    free(out);
}

// ============================================================================
// PAD INFO — DualSense status through libScePad. Resolved at runtime like the
// save-fs functions (the lib may not be mapped in a bare payload); the whole
// call sequence runs inside one watchdog worker because scePad* does daemon
// IPC that wedges under kstuff-light. The controller-information struct
// differs between SDK revisions, so we also return a raw hex dump for offset
// mapping (battery level/charge live somewhere in there).
// ============================================================================
static int (*g_scePadInit)(void) = NULL;
static int (*g_scePadOpen)(int, int, int, void *) = NULL;
static int (*g_scePadGetHandle)(int, int, int) = NULL;
static int (*g_scePadReadState)(int, void *) = NULL;
static int (*g_scePadGetControllerInformation)(int, void *) = NULL;
static int (*g_scePadClose)(int) = NULL;
static int g_pad_resolved = 0;

static int resolve_pad(void) {
    if (!g_pad_resolved) {
        // Statically linked — symbols always exist, zero runtime resolution.
        g_scePadInit                     = scePadInit;
        g_scePadOpen                     = scePadOpen;
        g_scePadGetHandle                = scePadGetHandle;
        g_scePadReadState                = scePadReadState;
        g_scePadGetControllerInformation = scePadGetControllerInformation;
        g_scePadClose                    = scePadClose;
        g_pad_resolved = 1;
    }
    return 1;
}

// ============================================================================
// SHELLCORE REMOTE PAD BRIDGE — libScePad refuses payload processes (no UI
// session: every scePadOpen returns 0x809B0081). SceShellCore owns the pad
// session, so we borrow its address space: PT_ATTACH, resolve libScePad inside
// the remote process through kernel RPC, hijack the stopped thread for a
// bounded remote call (pushed fake return address -> int3 gadget -> SIGTRAP),
// then restore its registers and copy results back with PT_IO. Same technique
// as the HID-Dumper payload. The pad handle lives inside ShellCore — it is
// acquired once, cached, never closed.
// ============================================================================
// Remote memory goes through the kernel RPC copy primitives — PT_IO and
// mdbg writes are both denied against system processes, but the kernel's
// own proc_copyin/copyout work under the debugger authid.
static int rpc_read(pid_t pid, intptr_t addr, void *buf, size_t len) {
    pthread_mutex_lock(&g_krpc_lock);
    int r = kernel_proc_copyout(pid, addr, buf, len);
    pthread_mutex_unlock(&g_krpc_lock);
    return r;
}
static int rpc_write(pid_t pid, intptr_t addr, const void *buf, size_t len) {
    pthread_mutex_lock(&g_krpc_lock);
    int r = kernel_proc_copyin(pid, buf, addr, len);
    pthread_mutex_unlock(&g_krpc_lock);
    return r;
}

static pid_t proc_find_name(const char *comm) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || !len) return -1;
    uint8_t *buf = malloc(len);
    pid_t found = -1;
    if (buf && sysctl(mib, 4, buf, &len, NULL, 0) == 0) {
        size_t sz = ((struct kinfo_proc *)buf)->ki_structsize;
        for (uint8_t *p = buf; p < buf + len && sz; p += sz) {
            struct kinfo_proc *ki = (struct kinfo_proc *)p;
            if (!strcmp(ki->ki_comm, comm)) { found = ki->ki_pid; break; }
        }
    }
    free(buf);
    return found;
}

typedef struct {
    volatile int dead;  // a call timed out — stop issuing remote calls
    pid_t pid;
    struct reg bak;
    uint8_t bak_guard[512];  // kernel reg dump may exceed our struct reg
    intptr_t int3;      // remote int3 gadget address
    intptr_t scratch;   // remote scratch buffer (deep below the stopped frame)
} rpc_sess_t;

static char g_scp_dbg[200];

// Any 0xcc byte inside executable text works as an int3 gadget — compilers pad
// between functions with int3, so the first hit is usually a few hundred
// bytes past the seed.
static intptr_t rpc_find_int3(pid_t pid, intptr_t seed) {
    uint8_t buf[4096];
    for (intptr_t a = seed; a < seed + 0x40000; a += (intptr_t)sizeof(buf)) {
        if (rpc_read(pid, a, buf, sizeof(buf)) != 0) break;
        uint8_t *p = memchr(buf, 0xcc, sizeof(buf));
        if (p) return a + (p - buf);
    }
    return 0;
}

static int rpc_attach(rpc_sess_t *s, pid_t pid, intptr_t int3_seed) {
    memset(s, 0, sizeof(*s));
    s->pid = pid;
    if (ptrace(PT_ATTACH, pid, 0, 0) != 0) return -(0x100 + errno);
    if (waitpid(pid, NULL, 0) < 0) { mem_detach(pid); return -(0x200 + errno); }
    if (ptrace(PT_GETREGS, pid, (caddr_t)&s->bak, 0) != 0) {
        mem_detach(pid);
        return -(0x300 + errno);
    }
    s->scratch = s->bak.r_rsp - 0x8000;  // remote stack, below any live frame
    s->int3 = rpc_find_int3(pid, int3_seed);
    if (!s->int3) {
        uint8_t tbuf[64];
        int got = rpc_read(pid, int3_seed, tbuf, sizeof(tbuf));
        snprintf(g_scp_dbg + strlen(g_scp_dbg),
                 sizeof(g_scp_dbg) - strlen(g_scp_dbg),
                 " rsp=%lx io=%d", (long)s->bak.r_rsp, got);
        mem_detach(pid);
        return -3;
    }
    return 0;
}

// Bounded remote call: write an int3 return address under the stopped thread's
// stack pointer, run the function, wait for the trap (<=3s), restore the
// thread exactly as we found it. Between calls the process stays attached and
// stopped, so rpc_call is cheap to repeat.
__attribute__((noinline))
static long rpc_call(rpc_sess_t *s, intptr_t fn,
                     long a1, long a2, long a3, long a4, long a5, long a6) {
    if (s->dead) {
        snprintf(g_scp_dbg + strlen(g_scp_dbg),
                 sizeof(g_scp_dbg) - strlen(g_scp_dbg),
                 " PREDEAD fn=%lx d=%d", (long)fn, s->dead);
        return -0x7777;
    }
    s->dead = 1;  // re-purposed as stage marker until the call completes
    uint64_t retaddr = (uint64_t)s->int3;
    int wrc = rpc_write(s->pid, s->bak.r_rsp - 8, &retaddr, 8);
    if (wrc != 0) {
        snprintf(g_scp_dbg + strlen(g_scp_dbg),
                 sizeof(g_scp_dbg) - strlen(g_scp_dbg),
                 " wrc=%d@%lx", wrc, (long)(s->bak.r_rsp - 8));
        return -0x1001;
    }
    struct reg j = s->bak;
    j.r_rip = fn;
    j.r_rsp = s->bak.r_rsp - 8;
    j.r_rdi = a1; j.r_rsi = a2; j.r_rdx = a3;
    j.r_rcx = a4; j.r_r8 = a5; j.r_r9 = a6;
    if (ptrace(PT_SETREGS, s->pid, (caddr_t)&j, 0) != 0) { s->dead = 2; return -0x1002; }
    if (ptrace(PT_CONTINUE, s->pid, (caddr_t)1, 0) != 0) { s->dead = 3; return -0x1003; }
    s->dead = 4;  // continued — waiting for the trap now

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int st = 0;
        if (waitpid(s->pid, &st, WNOHANG) == s->pid && WIFSTOPPED(st)) {
            struct reg cur;
            memset(&cur, 0, sizeof(cur));
            ptrace(PT_GETREGS, s->pid, (caddr_t)&cur, 0);
            snprintf(g_scp_dbg + strlen(g_scp_dbg),
                     sizeof(g_scp_dbg) - strlen(g_scp_dbg),
                     " ev=%d@%lx rax=%lx", WSTOPSIG(st), (long)cur.r_rip,
                     (long)cur.r_rax);
            if (WSTOPSIG(st) == SIGTRAP && cur.r_rip == s->int3 + 1) {
                long rax = (long)cur.r_rax;
                ptrace(PT_SETREGS, s->pid, (caddr_t)&s->bak, 0);
                s->dead = 0;
                return rax;
            }
            // Some other thread event — swallow it and keep waiting.
            ptrace(PT_CONTINUE, s->pid, (caddr_t)1, 0);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((t1.tv_sec - t0.tv_sec) * 1000 +
            (t1.tv_nsec - t0.tv_nsec) / 1000000 > 3000) {
            // The remote call blocked (lock contention inside ShellCore).
            // Force-stop, restore the hijacked thread, mark session dead.
            kill(s->pid, SIGSTOP);
            int st2 = 0;
            waitpid(s->pid, &st2, 0);
            ptrace(PT_SETREGS, s->pid, (caddr_t)&s->bak, 0);
            return -0x1004;
        }
        usleep(1000);
    }
}

static void rpc_detach(rpc_sess_t *s) {
    ptrace(PT_SETREGS, s->pid, (caddr_t)&s->bak, 0);
    mem_detach(s->pid);
}

static intptr_t scp_sym(pid_t pid, const char *name) {
    uint32_t h = 0;
    if (krpc_dynlib_handle(pid, "libScePad.sprx", &h) != 0 || !h) return 0;
    return (intptr_t)krpc_dlsym_checked(pid, h, name);
}

static intptr_t scp_sym_mod(pid_t pid, const char *mod, const char *name) {
    uint32_t h = 0;
    if (krpc_dynlib_handle(pid, mod, &h) != 0 || !h) return 0;
    return (intptr_t)krpc_dlsym_checked(pid, h, name);
}

static pid_t g_scp_pid = -1;
static int   g_scp_handle = -1;
static rpc_sess_t g_scp_sess;              // static: survives stack corruption
static pthread_mutex_t g_scp_mtx = PTHREAD_MUTEX_INITIALIZER;

// One attach session against SceShellCore. op: 0 = read state+info,
// 1 = set lightbar (params = 4B rgba). Returns the remote call rc (or a negative bridge error).
static long scp_session(int op, const uint8_t *params, size_t plen,
                        uint8_t *state_out, uint8_t *info_out) {
    // SceShellCore for state; SceShellUI for lightbar (foreground ownership).
    pid_t pid = proc_find_name(op ? "SceShellUI" : "SceShellCore");
    if (pid < 0 && op) pid = proc_find_name("SceShellCore");
    if (pid < 0) return -10;
    intptr_t fn_open  = scp_sym(pid, "scePadOpen");
    intptr_t fn_geth  = scp_sym(pid, "scePadGetHandle");
    intptr_t fn_read  = scp_sym(pid, "scePadReadState");
    intptr_t fn_info  = scp_sym(pid, "scePadGetControllerInformation");
    intptr_t fn_light = scp_sym(pid, "scePadSetLightBar");
    intptr_t seed = fn_read ? fn_read : (fn_open ? fn_open : 0);
    snprintf(g_scp_dbg, sizeof(g_scp_dbg),
             "pid=%d open=%lx geth=%lx read=%lx info=%lx lb=%lx",
             (int)pid, (long)fn_open, (long)fn_geth, (long)fn_read,
             (long)fn_info, (long)fn_light);
    if (!seed) return -11;

    // PT_ATTACH on a system process needs the debugger authid in our ucred —
    // swap it in for the duration of the session, then restore.
    pid_t me = getpid();
    uint64_t old_authid = kernel_get_ucred_authid(me);
    kernel_set_ucred_authid(me, 0x4800000000010003ULL);

    pthread_mutex_lock(&g_scp_mtx);
    rpc_sess_t *sp = &g_scp_sess;
    int arc = rpc_attach(sp, pid, seed);
    snprintf(g_scp_dbg + strlen(g_scp_dbg),
             sizeof(g_scp_dbg) - strlen(g_scp_dbg),
             " arc=%d dead0=%d int3=%lx scr=%lx",
             arc, sp->dead, (long)sp->int3, (long)sp->scratch);
    if (arc != 0) {
        pthread_mutex_unlock(&g_scp_mtx);
        kernel_set_ucred_authid(me, old_authid);
        return arc;
    }

    // Acquire the pad handle inside ShellCore once. Single open only — a
    // multi-open sweep is what historically crashed ShellCore in hidDumper.
    if (g_scp_pid != pid || g_scp_handle < 0) {
        g_scp_pid = pid;
        g_scp_handle = -1;
    }
    if (g_scp_handle < 0) {
        // Ask ShellCore's own UserService who is logged in — its session is
        // the real one (our process gets an empty list).
        int uids[8] = {0};
        intptr_t fn_login = scp_sym_mod(pid, "libSceUserService.sprx",
                                        "sceUserServiceGetLoginUserIdList");
        intptr_t fn_fg = scp_sym_mod(pid, "libSceUserService.sprx",
                                     "sceUserServiceGetForegroundUser");
        if (fn_login && !sp->dead) {
            rpc_call(sp, fn_login, sp->scratch, 0, 0, 0, 0, 0);
            rpc_read(pid, sp->scratch, uids, sizeof(uids));
        }
        if (fn_fg && !sp->dead) {
            rpc_call(sp, fn_fg, sp->scratch + 0x40, 0, 0, 0, 0, 0);
            rpc_read(pid, sp->scratch + 0x40, &uids[4], 4);
        }
        uids[5] = 0x10000000;
        snprintf(g_scp_dbg + strlen(g_scp_dbg),
                 sizeof(g_scp_dbg) - strlen(g_scp_dbg),
                 " uids=%08x %08x %08x %08x %08x",
                 uids[0], uids[1], uids[2], uids[3], uids[4]);

        long h = -1, h2 = -100;
        for (int i = 0; i <= 5 && h <= 0 && !sp->dead; i++) {
            int u = uids[i];
            if (!u) continue;
            long g = fn_geth ? rpc_call(sp, fn_geth, u, 0, 0, 0, 0, 0) : -1;
            snprintf(g_scp_dbg + strlen(g_scp_dbg),
                     sizeof(g_scp_dbg) - strlen(g_scp_dbg),
                     " g%x=%lx", u, g);
            if (g > 0 && g < 0x80000000) { h = g; break; }
            if (fn_open) {
                g = rpc_call(sp, fn_open, u, 0, 0, 0, 0, 0);
                snprintf(g_scp_dbg + strlen(g_scp_dbg),
                         sizeof(g_scp_dbg) - strlen(g_scp_dbg),
                         " o%x=%lx", u, g);
                if (g > 0 && g < 0x80000000) h = g;
            }
        }
        if (h > 0) { g_scp_handle = (int)h; g_scp_pid = pid; }
        (void)h2;
    }

    long rc = g_scp_handle;
    if (g_scp_handle >= 0 && !sp->dead) {
        if (op != 0 && params && plen) {
            if (rpc_write(pid, sp->scratch, params, plen) == 0) {
                if (fn_light) rc = rpc_call(sp, fn_light, g_scp_handle, sp->scratch, 0, 0, 0, 0);
            } else rc = -0x1005;
        }
        if (state_out && !sp->dead) {
            rc = rpc_call(sp, fn_read, g_scp_handle, sp->scratch, 0, 0, 0, 0);
            rpc_read(pid, sp->scratch, state_out, 256);
        }
        if (info_out && fn_info && !sp->dead) {
            rpc_call(sp, fn_info, g_scp_handle, sp->scratch, 0, 0, 0, 0);
            rpc_read(pid, sp->scratch, info_out, 256);
        }
        if (rc < 0 && (rc & 0xffff0000) == 0x80920000)
            g_scp_handle = -1;  // handle died — re-acquire next session
    }
    rpc_detach(sp);
    pthread_mutex_unlock(&g_scp_mtx);
    kernel_set_ucred_authid(me, old_authid);
    return rc;
}

typedef struct {
    int rc_init, rc_open, user, handle, rc_info, rc_state, borrowed;
    int remote;   // 1 when the handle lives inside SceShellCore
    int tries[24]; // per-attempt rc for each (uid,type) attempt
    uint8_t info[256];
    uint8_t state[256];
    int login_uids[4];
    int login_n;
    int ctrlp;
} pad_work_t;

// Tries scePadOpen across a (user, portType) matrix and records every
// attempt's rc so the log shows WHY open fails (bad uid vs bad type vs none).
// Candidates: the real logged-in user ids first, then the known fallbacks.
// Falls back to scePadGetHandle — a pad already opened by another process
// (ShellUI) can't be re-opened, but its handle is still usable. Borrowed
// handles must NOT be closed.
static int pad_open_any(int fg_user, const int *logins, int login_n,
                        int *handle, int tries[24], int *borrowed) {
    // PS5 user ids are 0x10000000-style handles, not small integers.
    int uids[16];
    int un = 0;
    for (int i = 0; i < login_n && un < 16; i++) uids[un++] = logins[i];
    int fallbacks[] = { fg_user, 0x10000000, 0x10000001, 0x10000002,
                        -1, 1, 2, 0 };
    for (unsigned i = 0; i < sizeof(fallbacks)/sizeof(fallbacks[0]) && un < 16; i++)
        uids[un++] = fallbacks[i];
    int n = 0;
    *handle = -1; *borrowed = 0;
    for (int t = 0; t < 2 && *handle < 0; t++)
        for (int i = 0; i < un && *handle < 0; i++) {
            int rc = g_scePadOpen(uids[i], t, 0, NULL);
            if (n < 24) tries[n++] = rc;
            if (rc >= 0) *handle = rc;
        }
    if (*handle < 0 && g_scePadGetHandle) {
        for (int t = 0; t < 2 && *handle < 0; t++)
            for (int i = 0; i < un && *handle < 0; i++) {
                int rc = g_scePadGetHandle(uids[i], t, 0);
                if (n < 24) tries[n++] = rc;
                if (rc >= 0) { *handle = rc; *borrowed = 1; }
            }
    }
    return *handle;
}

static int pad_work(pad_work_t *w) {
    // scePadInit is owned by ShellCore and fails in payloads — harmless, but
    // keep the rc so the log shows it.
    w->rc_init = g_scePadInit ? g_scePadInit() : 0;
    w->user = 0;
    sceUserServiceGetForegroundUser(&w->user);
    memset(w->login_uids, 0, sizeof(w->login_uids));
    w->login_n = sceUserServiceGetLoginUserIdList(w->login_uids);
    if (w->login_n < 0) w->login_n = 0;
    if (w->login_n > 4) w->login_n = 4;
    w->ctrlp = access("/dev/ctrlp", F_OK) == 0;
    pad_open_any(w->user, w->login_uids, w->login_n,
                 &w->handle, w->tries, &w->borrowed);
    w->rc_open = w->handle;
    if (w->handle < 0) {
        // The pad service won't hand a payload a handle — borrow SceShellCore's
        // pad session over ptrace instead (HID-Dumper technique).
        memset(w->info, 0, sizeof(w->info));
        memset(w->state, 0, sizeof(w->state));
        long rrc = scp_session(0, NULL, 0, w->state, w->info);
        if (g_scp_handle >= 0) {
            w->handle = g_scp_handle;
            w->borrowed = 1;
            w->remote = 1;
            w->rc_info = 0;
            w->rc_state = (int)rrc;
            w->rc_open = w->handle;
            return 0;
        }
        w->rc_open = (int)rrc;
        return 0;
    }
    memset(w->info, 0, sizeof(w->info));
    if (g_scePadGetControllerInformation)
        w->rc_info = g_scePadGetControllerInformation(w->handle, w->info);
    memset(w->state, 0, sizeof(w->state));
    if (g_scePadReadState)
        w->rc_state = g_scePadReadState(w->handle, w->state);
    if (!w->borrowed && g_scePadClose) g_scePadClose(w->handle);
    return 0;
}

// resolve_pad() does dlopen + kernel RPCs — both can park. It must NEVER run
// on the command thread: one hang there freezes every later command. Run the
// resolve inside the watchdog worker instead so a park only costs 8s.
static int pad_full_work(pad_work_t *w) {
    if (!resolve_pad()) { w->rc_open = -99; return 0; }
    return pad_work(w);
}

void handle_pad_info(client_session_t *session) {
    char *out = malloc(2048);
    if (!out) { send_error(session->sock, "Out of memory"); return; }
    int off = 0;
    if (!etahen_present()) {
        off = snprintf(out, 2048, "error=pad service needs etaHEN\n");
    } else {
        pad_work_t w;
        memset(&w, 0, sizeof(w));
        int wrc = wdg_fn_call((void *)pad_full_work, &w, NULL, NULL, NULL, 8000);
        if (w.rc_open == -99) {
            off = snprintf(out, 2048, "error=libScePad unavailable\n");
        } else
        if (wrc != 0) {
            off = snprintf(out, 2048, "error=pad ipc timeout\n");
        } else {
            off += snprintf(out + off, 2048 - off,
                "user=%d\nhandle=%d\ninit=%d\ninfo=%d\nstate=%d\nopen=%d\nremote=%d\nctrlp=%d\nlogins=",
                w.user, w.handle, w.rc_init, w.rc_info, w.rc_state,
                w.rc_open, w.remote, w.ctrlp);
            for (int i = 0; i < w.login_n; i++)
                off += snprintf(out + off, 2048 - off, "%08x ", (unsigned)w.login_uids[i]);
            off += snprintf(out + off, 2048 - off, "\ndbg=%s\ntries=", g_scp_dbg);
            for (int i = 0; i < 24 && off < 1700; i++)
                off += snprintf(out + off, 2048 - off, "%08x ", (unsigned)w.tries[i]);
            off += snprintf(out + off, 2048 - off, "\n");
            if (w.handle >= 0) {
                off += snprintf(out + off, 2048 - off, "infohex=");
                for (int i = 0; i < 128 && off < 1700; i++)
                    off += snprintf(out + off, 2048 - off, "%02x", w.info[i]);
                off += snprintf(out + off, 2048 - off, "\nstatehex=");
                for (int i = 0; i < 64 && off < 1950; i++)
                    off += snprintf(out + off, 2048 - off, "%02x", w.state[i]);
                off += snprintf(out + off, 2048 - off, "\n");
            }
        }
    }
    send_response(session->sock, RESP_DATA, out, off);
    free(out);
}

// ============================================================================
// DISC DUMP — copy an inserted disc (/mnt/disc*) to /user/data/disc_dumps on
// a background thread. Pure filesystem work — no daemon IPC — so it is safe
// under every loader. Progress is polled through "status"; "cancel" stops it.
// ============================================================================
static volatile int g_dump_active = 0, g_dump_cancel = 0;
static uint64_t g_dump_done = 0, g_dump_total = 0;
static int g_dump_files = 0;
static char g_dump_file[1024] = {0};
static char g_dump_err[256] = {0};
static char g_dump_dest[1024] = {0};

static int find_disc_mount(char *out, size_t n) {
    static const char *cand[] = {
        "/mnt/disc", "/mnt/disc0", "/mnt/disc1", "/disc", "/mnt/cdrom", NULL
    };
    for (int i = 0; cand[i]; i++) {
        DIR *d = opendir(cand[i]);
        if (d) { closedir(d); snprintf(out, n, "%s", cand[i]); return 0; }
    }
    return -1;
}

static int mkdir_p(const char *path) {
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755);
}

static void count_tree(const char *src, uint64_t *bytes) {
    DIR *d = opendir(src);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && !g_dump_cancel) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[2048];
        snprintf(p, sizeof(p), "%s/%s", src, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) count_tree(p, bytes);
        else *bytes += (uint64_t)st.st_size;
    }
    closedir(d);
}

static void dump_tree(const char *src, const char *dst) {
    if (g_dump_cancel) return;
    DIR *d = opendir(src);
    if (!d) return;
    mkdir_p(dst);
    struct dirent *e;
    uint8_t *buf = malloc(256 * 1024);
    while ((e = readdir(d)) && !g_dump_cancel) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sp[2048], dp[2048];
        snprintf(sp, sizeof(sp), "%s/%s", src, e->d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst, e->d_name);
        struct stat st;
        if (stat(sp, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { dump_tree(sp, dp); continue; }
        int in = open(sp, O_RDONLY);
        if (in < 0) continue;
        int out = open(dp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) { close(in); continue; }
        snprintf(g_dump_file, sizeof(g_dump_file), "%s", e->d_name);
        g_dump_files++;
        ssize_t r;
        while ((r = read(in, buf, 256 * 1024)) > 0) {
            if (g_dump_cancel) break;
            ssize_t w = 0;
            while (w < r) {
                ssize_t x = write(out, buf + w, r - w);
                if (x <= 0) break;
                w += x;
            }
            g_dump_done += (uint64_t)r;
        }
        close(in);
        close(out);
    }
    free(buf);
    closedir(d);
}

static void *disc_dump_worker(void *arg) {
    char *src = (char *)arg;
    count_tree(src, &g_dump_total);
    dump_tree(src, g_dump_dest);
    g_dump_active = 0;
    free(src);
    return NULL;
}

void handle_disc_dump(client_session_t *session, const char *arg) {
    if (!arg || !*arg) { send_error(session->sock, "usage: start|status|cancel"); return; }
    if (!strcmp(arg, "status")) {
        char st[1600];
        int n = snprintf(st, sizeof(st), "active=%d|done=%llu|total=%llu|files=%d|file=%s|err=%s|dest=%s",
            g_dump_active, (unsigned long long)g_dump_done,
            (unsigned long long)g_dump_total, g_dump_files, g_dump_file,
            g_dump_err, g_dump_dest);
        send_response(session->sock, RESP_DATA, st, n);
        return;
    }
    if (!strcmp(arg, "cancel")) {
        g_dump_cancel = 1;
        send_response(session->sock, RESP_OK, "cancelled", 9);
        return;
    }
    if (!strcmp(arg, "start")) {
        if (g_dump_active) { send_error(session->sock, "dump already running"); return; }
        char src[64];
        if (find_disc_mount(src, sizeof(src)) != 0) {
            send_error(session->sock, "no disc mounted (insert a disc)");
            return;
        }
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        snprintf(g_dump_dest, sizeof(g_dump_dest), "/user/data/disc_dumps/%s", base);
        g_dump_done = g_dump_total = 0;
        g_dump_files = 0;
        g_dump_err[0] = g_dump_file[0] = 0;
        g_dump_cancel = 0;
        g_dump_active = 1;
        pthread_t t;
        char *arg_copy = strdup(src);
        if (pthread_create(&t, NULL, disc_dump_worker, arg_copy) != 0) {
            g_dump_active = 0;
            free(arg_copy);
            send_error(session->sock, "thread start failed");
            return;
        }
        pthread_detach(t);
        send_response(session->sock, RESP_OK, g_dump_dest, strlen(g_dump_dest));
        return;
    }
    send_error(session->sock, "usage: start|status|cancel");
}

// ============================================================================
// SCREENSHOT — sceScreenShotCapture() asks ShellUI to grab whatever is on
// screen right now. Daemon IPC → etaHEN guard + watchdog worker.
// ============================================================================
static int (*g_sceScreenShotCapture)(void) = NULL;
static int g_ss_resolved = 0;

static int resolve_screenshot(void) {
    if (g_ss_resolved) return g_sceScreenShotCapture != NULL;
    static const char *paths[] = {
        "/system/common/lib/libSceScreenShot.sprx",
        "/system/priv/lib/libSceScreenShot.sprx",
        "/preinst2/common/lib/libSceScreenShot.sprx",
        "libSceScreenShot.sprx",
        NULL
    };
    void *h = NULL;
    for (int i = 0; paths[i] && !h; i++)
        h = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
    if (h) g_sceScreenShotCapture = (void *)dlsym(h, "sceScreenShotCapture");
    if (!g_sceScreenShotCapture) {
        uint32_t mod = 0;
        if (krpc_dynlib_handle(getpid(), "libSceScreenShot.sprx", &mod) == 0 && mod)
            g_sceScreenShotCapture = krpc_dlsym_checked(getpid(), mod, "sceScreenShotCapture");
    }
    g_ss_resolved = 1;
    return g_sceScreenShotCapture != NULL;
}

// Resolve inside the watchdog worker — dlopen/krpc calls can park and must
// never run on the command thread (a park there freezes the whole server).
static int ss_work(int *rc) {
    if (!resolve_screenshot()) { *rc = -98; return 0; }
    *rc = g_sceScreenShotCapture();
    return 0;
}

void handle_screenshot(client_session_t *session) {
    char out[160];
    if (!etahen_present()) {
        send_error(session->sock, "screenshot needs etaHEN (ShellUI IPC)");
        return;
    }
    int rc = -1;
    int wrc = wdg_fn_call((void *)ss_work, &rc, NULL, NULL, NULL, 8000);
    int n = snprintf(out, sizeof(out), "rc=%d wdg=%d", rc, wrc);
    if (rc == 0)
        send_response(session->sock, RESP_OK, out, n);
    else
        send_response(session->sock, RESP_ERROR, out, n);
}

// ============================================================================
// NOTIFY — push a text notification to the PS5 UI (same kernel API the boot
// messages already use; plain syscall, no daemon).
// ============================================================================
void handle_notify(client_session_t *session, const char *arg) {
    if (!arg || !*arg) { send_error(session->sock, "empty message"); return; }
    send_notification(arg);
    send_response(session->sock, RESP_OK, "sent", 4);
}

// ============================================================================
// PAD ACTION — lightbar RGB on the first connected DualSense.
// Reuses the pad resolver; the open→set→close runs in one watchdog worker.
// ============================================================================
static int (*g_scePadSetLightBar)(int, const void *) = NULL;

static int resolve_pad_actions(void) {
    resolve_pad();
    g_scePadSetLightBar = scePadSetLightBar;
    return 1;
}

typedef struct {
    int r, g, b, a;      // lightbar color
    int rc, handle, borrowed;
} pad_action_work_t;

static int pad_action_work(pad_action_work_t *w) {
    int user = 0;
    sceUserServiceGetForegroundUser(&user);
    int logins[4];
    memset(logins, 0, sizeof(logins));
    int login_n = sceUserServiceGetLoginUserIdList(logins);
    if (login_n < 0) login_n = 0;
    if (login_n > 4) login_n = 4;
    int tries[24];
    memset(tries, 0, sizeof(tries));
    pad_open_any(user, logins, login_n, &w->handle, tries, &w->borrowed);
    if (w->handle < 0) {
        // Same story as pad info — run the action inside SceShellCore.
        uint8_t params[4] = {0};
        params[0] = (uint8_t)w->r; params[1] = (uint8_t)w->g;
        params[2] = (uint8_t)w->b; params[3] = (uint8_t)w->a;
        long rc = scp_session(1, params, 4, NULL, NULL);
        w->rc = (int)rc;
        w->handle = g_scp_handle;
        w->borrowed = 1;
        return 0;
    }
    w->borrowed = 0;
    uint8_t color[4] = {(uint8_t)w->r, (uint8_t)w->g, (uint8_t)w->b, (uint8_t)w->a};
    w->rc = g_scePadSetLightBar(w->handle, color);
    return 0;
}

// Resolve inside the watchdog worker — see pad_full_work note above.
static int pad_action_full_work(pad_action_work_t *w) {
    if (!resolve_pad_actions()) { w->rc = -99; return 0; }
    return pad_action_work(w);
}

void handle_pad_action(client_session_t *session, const char *arg) {
    if (!arg || !*arg) { send_error(session->sock, "usage: lightbar|r,g,b"); return; }
    if (!etahen_present()) { send_error(session->sock, "pad needs etaHEN"); return; }

    pad_action_work_t w;
    memset(&w, 0, sizeof(w));
    if (sscanf(arg, "lightbar|%d,%d,%d", &w.r, &w.g, &w.b) == 3) {
        w.a = 255;
    } else {
        send_error(session->sock, "usage: lightbar|r,g,b");
        return;
    }
    int wrc = wdg_fn_call((void *)pad_action_full_work, &w, NULL, NULL, NULL, 8000);
    char out[96];
    int n = snprintf(out, sizeof(out), "rc=%d wdg=%d handle=%d", w.rc, wrc, w.handle);
    if (w.rc == 0)
        send_response(session->sock, RESP_OK, out, n);
    else
        send_response(session->sock, RESP_ERROR, out, n);
}

// ============================================================================
// ICC INDICATOR LED — discovered live on the PS5 (via iccprobe/iccraw):
//   /dev/icc_indicator
//   0x40829505  get_dynamic_led  -> reads the 130-byte state struct
//   0x80829504  set_dynamic_led  -> writes the same struct (ICC validates)
//   0x8001950c  dim setting      (0..2 — same as sceKernelIccSetDynamicLedDim)
//   0x8001950e  enable flag      (0=off, 1=on — verified on hardware)
//   0x80019501  set_buzzer
// State struct: bytes 0-3 = header (00 00 | count u16), then 6-byte records
//   { u8 channel_id; u8 flag=1; u8 unk; u8 level; u8 unk2; u8 mode }
// Channel ids verified live: 0x01=blue, 0x11=white, 0x21=amber.
// Records float inside the blob — scan for (id,0x01) pairs, never offsets.
// ============================================================================
#define ICC_INDICATOR_DEV  "/dev/icc_indicator"
#define ICC_LED_STATE_SIZE 130
#define ICC_CH_BLUE   0x01
#define ICC_CH_WHITE  0x11
#define ICC_CH_AMBER  0x21

static pthread_mutex_t g_led_mtx = PTHREAD_MUTEX_INITIALIZER;

static int icc_dyn_get(uint8_t *st) {
    int fd = open(ICC_INDICATOR_DEV, O_RDWR);
    if (fd < 0) return -errno;
    int r = ioctl(fd, 0x40829505ul, st);
    int rc = r ? -errno : 0;
    close(fd);
    return rc;
}

static int icc_dyn_set(const uint8_t *st) {
    int fd = open(ICC_INDICATOR_DEV, O_RDWR);
    if (fd < 0) return -errno;
    int r = ioctl(fd, 0x80829504ul, (void *)st);
    int rc = r ? -errno : 0;
    close(fd);
    return rc;
}

static int icc_flag(uint8_t v) {
    int fd = open(ICC_INDICATOR_DEV, O_RDWR);
    if (fd < 0) return -errno;
    uint8_t b[8] = { v, 0 };
    int r = ioctl(fd, 0x8001950eul, b);
    int rc = r ? -errno : 0;
    close(fd);
    return rc;
}

// Find a channel's 6-byte record inside the state blob; -1 if absent.
static int icc_chan_off(const uint8_t *st, uint8_t id) {
    for (int i = 2; i + 5 < ICC_LED_STATE_SIZE; i++)
        if (st[i] == id && st[i + 1] == 0x01)
            return i;
    return -1;
}

// Read-modify-write the LED state. Levels are 0-255, -1 = leave channel.
// All-zero is rejected by the ICC — callers wanting "off" use icc_flag(0).
static int icc_set_levels(int blue, int white, int amber) {
    pthread_mutex_lock(&g_led_mtx);
    uint8_t st[ICC_LED_STATE_SIZE];
    int rc = icc_dyn_get(st);
    if (rc == 0) {
        const uint8_t ids[3] = { ICC_CH_BLUE, ICC_CH_WHITE, ICC_CH_AMBER };
        const int lv[3] = { blue, white, amber };
        int patched = 0;
        for (int i = 0; i < 3; i++) {
            if (lv[i] < 0) continue;
            int o = icc_chan_off(st, ids[i]);
            if (o >= 0) { st[o + 3] = (uint8_t)lv[i]; patched = 1; }
        }
        rc = patched ? icc_dyn_set(st) : -95;
    }
    pthread_mutex_unlock(&g_led_mtx);
    return rc;
}

// Effects run as a detached thread that keeps rewriting the state: the
// dynamic-LED ioctls only carry static levels, so animations are driven in
// software. It also re-asserts periodically in case ShellUI reclaims the LED.
typedef struct { const char *name; int mode; int period_ms;
                 uint8_t lvl[3]; uint8_t lvl2[3]; } led_fx_t;
// mode 0=static hold, 1=breathe lvl2..lvl triangle, 2=alternate lvl<->lvl2
static const led_fx_t g_led_fx[] = {
    { "white",            0, 0,    {0x00,0xff,0x00}, {0,0,0} },
    { "white-dim",        0, 0,    {0x00,0x40,0x00}, {0,0,0} },
    { "blue",             0, 0,    {0xff,0x00,0x00}, {0,0,0} },
    { "orange",           0, 0,    {0x00,0x00,0xff}, {0,0,0} },
    { "purple",           0, 0,    {0xa0,0x00,0xff}, {0,0,0} },
    { "pink",             0, 0,    {0x40,0x30,0xff}, {0,0,0} },
    { "sunrise",          2, 6000, {0x00,0x00,0xff}, {0x00,0xff,0x00} },
    { "blue-breathe",     1, 4000, {0xff,0x00,0x00}, {0x00,0x00,0x00} },
    { "white-breathe",    1, 4000, {0x00,0xff,0x00}, {0x00,0x00,0x00} },
    { "orange-breathe",   1, 4000, {0x00,0x00,0xff}, {0x00,0x00,0x00} },
    { "pink-breathe",     1, 4000, {0x40,0x30,0xff}, {0x00,0x00,0x00} },
    { "pink-breathe-fast",1, 1500, {0x40,0x30,0xff}, {0x00,0x00,0x00} },
    { "blue-to-richblue", 1, 3000, {0xff,0x00,0x00}, {0x40,0x00,0x00} },
    { "blue-white-anim",  2, 2000, {0xff,0x00,0x00}, {0x00,0xff,0x00} },
};

static volatile int g_ledfx_run = 0;
static led_fx_t g_ledfx;

static void led_fx_stop(void) {
    g_ledfx_run = 0;
    // give a running thread a moment to exit its ioctl before we reuse the dev
    for (int i = 0; i < 20 && g_ledfx_run != -1; i++) usleep(20000);
}

static void *led_fx_thread(void *unused) {
    (void)unused;
    int elapsed = 0;
    while (g_ledfx_run == 1) {
        const led_fx_t *fx = &g_ledfx;
        uint8_t lv[3];
        int hold;
        if (fx->mode == 1) {
            int per = fx->period_ms > 100 ? fx->period_ms : 100;
            int ph = elapsed % per;
            float tri = ph < per / 2 ? (float)ph / (per / 2)
                                     : (float)(per - ph) / (per / 2);
            for (int i = 0; i < 3; i++)
                lv[i] = (uint8_t)(fx->lvl2[i] + (fx->lvl[i] - fx->lvl2[i]) * tri);
            hold = 80000;
        } else if (fx->mode == 2) {
            int ph = (elapsed / (fx->period_ms > 1 ? fx->period_ms / 2 : 1)) & 1;
            const uint8_t *src = ph ? fx->lvl2 : fx->lvl;
            memcpy(lv, src, sizeof(lv));
            hold = 80000;
        } else {
            memcpy(lv, fx->lvl, sizeof(lv));
            hold = 500000;  // static: just re-assert every 500ms
        }
        if (icc_set_levels(lv[0], lv[1], lv[2]) != 0)
            usleep(400000);
        usleep(hold);
        elapsed += 80;
    }
    g_ledfx_run = -1;  // thread exited
    return NULL;
}

static int led_fx_start(const led_fx_t *fx) {
    led_fx_stop();
    g_ledfx = *fx;
    g_ledfx_run = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, led_fx_thread, NULL) != 0) {
        g_ledfx_run = 0;
        return -1;
    }
    pthread_detach(t);
    return 0;
}

// ============================================================================
// ICC CONTROL — hardware beeper + power LED through libkernel ICC functions.
// Verified by live-hardware research (ps5-beeper-LED-Controller): all four
// symbols resolve from libkernel via kernel_dynlib and take a plain int.
//   sceKernelIccSetBuzzer(0..3)           — beep pattern (once)
//   sceKernelIccSetBuzzerDimSetting(0..2) — beeper volume (persistent)
//   sceKernelIccSetBuzzerOffSetting(0/1)  — beeper mute (persistent)
//   sceKernelIccSetDynamicLedDimSetting(0..2) — LED brightness (persistent)
// Per-channel color and effects go through /dev/icc_indicator's
// get/set_dynamic_led ioctls (see the ICC INDICATOR LED section above).
// ============================================================================
static int (*g_iccSetBuzzer)(int) = NULL;
static int (*g_iccSetBuzzerDim)(int) = NULL;
static int (*g_iccSetBuzzerOff)(int) = NULL;
static int (*g_iccSetLedDim)(int) = NULL;
static int g_icc_resolved = 0;

static intptr_t krpc_resolve_any(const char *sym) {
    // libkernel exports live under handle 1 for our process (same trick the
    // beeper-LED research used); also probe the kernel module handles. Every
    // result goes through mapbase validation — kernel errno codes otherwise
    // look like valid pointers and SIGSEGV the whole process when called.
    void *p = krpc_dlsym_checked(getpid(), 1, sym);
    if (p) return (intptr_t)p;
    static const char *mods[] = {
        "libkernel_sys.sprx", "libkernel_web.sprx", "libkernel.sprx", NULL
    };
    for (int i = 0; mods[i]; i++) {
        uint32_t h = 0;
        if (krpc_dynlib_handle(getpid(), mods[i], &h) == 0 && h) {
            p = krpc_dlsym_checked(getpid(), h, sym);
            if (p) return (intptr_t)p;
        }
    }
    return 0;
}

static void resolve_icc(void) {
    if (g_icc_resolved) return;
    g_iccSetBuzzer    = (void *)krpc_resolve_any("sceKernelIccSetBuzzer");
    g_iccSetBuzzerDim = (void *)krpc_resolve_any("sceKernelIccSetBuzzerDimSetting");
    g_iccSetBuzzerOff = (void *)krpc_resolve_any("sceKernelIccSetBuzzerOffSetting");
    g_iccSetLedDim    = (void *)krpc_resolve_any("sceKernelIccSetDynamicLedDimSetting");
    g_icc_resolved = 1;
}

// Bounded ioctl probe for icc_indicator: PS5 renumbered the PS4 ioctls.
// Sweep group 0x95, nr 0..15, with IOC_IN and IOC_INOUT at the common arg
// sizes — IOC_VOID is deliberately skipped (PS4's standby/shutdown cmds
// live there). Anything not returning ENOTTY is a real command; hits are
// reported as cmd=errno pairs. IN cmds get a zeroed 256-byte buffer, which
// for a setter just writes zeros to LED state — harmless.
static void icc_probe_sweep(char *out, size_t n) {
    static const int sizes[] = { 1, 4, 8, 26, 64, 128, 130, 256 };
    int off = 0;
    int fd = open(ICC_INDICATOR_DEV, O_RDWR);
    if (fd < 0) { snprintf(out, n, "open failed errno=%d", errno); return; }
    uint8_t *buf = calloc(1, 256);
    for (unsigned dir = 0; dir < 2; dir++) {
        uint32_t dirbits = dir ? 0xC0000000u : 0x80000000u;  // INOUT : IN
        for (int nr = 0; nr < 16; nr++)
            for (unsigned si = 0; si < sizeof(sizes)/sizeof(sizes[0]); si++) {
                uint32_t cmd = dirbits | ((uint32_t)sizes[si] << 16)
                             | (0x95u << 8) | (uint32_t)nr;
                memset(buf, 0, 256);
                errno = 0;
                int r = ioctl(fd, cmd, buf);
                int e = r == 0 ? 0 : errno;
                if (r == 0 || e != ENOTTY)
                    off += snprintf(out + off, n - off, "%08x=%d ",
                                    (unsigned)cmd, e);
                if (off > (int)n - 40) goto done;
            }
    }
done:
    free(buf);
    close(fd);
    if (off == 0) snprintf(out, n, "no hits");
}

// resolve_icc() issues kernel RPCs that can park — run it plus the call
// inside a watchdog worker so the command thread never blocks here.
typedef struct { char sub[32]; char opt[1050]; int v, v2, v3; int rc;
                 char probe[1024]; } icc_work_t;

// "iccraw|0xCMDHEX:deadbeef..." — send an arbitrary hex arg buffer to an
// arbitrary ioctl on /dev/icc_indicator. Debug command: lets us discover the
// real argument layout without rebuilding for every guess. Replies with
// rc + errno + hexdump of the buffer after the call (for IOC_OUT cmds).
static int hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void icc_raw_ioctl(icc_work_t *w, const char *spec) {
    // spec = "<cmd_hex>:<data_hex>"
    char *colon = spec ? strchr(spec, ':') : NULL;
    unsigned long cmd = strtoul(spec ? spec : "", NULL, 0);
    w->probe[0] = 0;
    if (!colon || !cmd) { snprintf(w->probe, sizeof(w->probe), "bad spec"); w->rc = -97; return; }
    const char *hx = colon + 1;
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    size_t blen = 0;
    while (hx[0] && hx[1] && blen < sizeof(buf)) {
        int hi = hex_nib(hx[0]), lo = hex_nib(hx[1]);
        if (hi < 0 || lo < 0) break;
        buf[blen++] = (uint8_t)((hi << 4) | lo);
        hx += 2;
    }
    int fd = open(ICC_INDICATOR_DEV, O_RDWR);
    if (fd < 0) { snprintf(w->probe, sizeof(w->probe), "open errno=%d", errno); w->rc = -1; return; }
    errno = 0;
    int r = ioctl(fd, cmd, buf);
    int e = r ? errno : 0;
    close(fd);
    int off = snprintf(w->probe, sizeof(w->probe), "r=%d errno=%d out=", r, e);
    for (size_t i = 0; i < 160 && off < (int)sizeof(w->probe) - 3; i++)
        off += snprintf(w->probe + off, sizeof(w->probe) - off, "%02x", buf[i]);
    w->rc = r;
}

static int icc_work(icc_work_t *w) {
    resolve_icc();
    w->rc = -99;
    int v = w->v, v2 = w->v2, v3 = w->v3;
    if (!strcmp(w->sub, "iccprobe")) {
        icc_probe_sweep(w->probe, sizeof(w->probe));
        w->rc = 0;
        return 0;
    }
    if (!strcmp(w->sub, "iccraw")) {
        icc_raw_ioctl(w, w->opt);
        return 0;
    }
    if (!strcmp(w->sub, "led")) {
        if (g_iccSetLedDim) w->rc = g_iccSetLedDim(v);
    } else if (!strcmp(w->sub, "buzzer")) {
        if (g_iccSetBuzzer) w->rc = g_iccSetBuzzer(v);
    } else if (!strcmp(w->sub, "buzzervol")) {
        if (g_iccSetBuzzerDim) w->rc = g_iccSetBuzzerDim(v);
    } else if (!strcmp(w->sub, "buzzermute")) {
        if (g_iccSetBuzzerOff) w->rc = g_iccSetBuzzerOff(v);
    } else if (!strcmp(w->sub, "ledcolor")) {
        // ledcolor|b,w,o — live channel levels (0x01 blue / 0x11 white /
        // 0x21 amber). All-zero means "off" -> drop the enable flag.
        if (v <= 0 && v2 <= 0 && v3 <= 0) {
            led_fx_stop();
            w->rc = icc_flag(0);
        } else {
            led_fx_t fx = { "custom", 0, 0,
                            { (uint8_t)(v>0?v:0), (uint8_t)(v2>0?v2:0),
                              (uint8_t)(v3>0?v3:0) }, {0,0,0} };
            w->rc = icc_flag(1);
            if (w->rc == 0) w->rc = led_fx_start(&fx);
        }
    } else if (!strcmp(w->sub, "ledeffect")) {
        if (!strcmp(w->opt, "off")) {
            led_fx_stop();
            w->rc = icc_flag(0);
        } else if (!strcmp(w->opt, "auto")) {
            // hand the indicator back to ShellUI
            led_fx_stop();
            w->rc = icc_flag(1);
        } else {
            const led_fx_t *fx = NULL;
            for (size_t i = 0; i < sizeof(g_led_fx)/sizeof(g_led_fx[0]); i++)
                if (!strcmp(g_led_fx[i].name, w->opt)) { fx = &g_led_fx[i]; break; }
            if (!fx) w->rc = -98;
            else {
                w->rc = icc_flag(1);
                if (w->rc == 0) w->rc = led_fx_start(fx);
            }
        }
    }
    return 0;
}

void handle_icc_control(client_session_t *session, const char *arg) {
    icc_work_t w;
    memset(&w, 0, sizeof(w));
    w.v = w.v2 = w.v3 = -1;
    sscanf(arg ? arg : "", "%31[^|]|%1049s", w.sub, w.opt);
    sscanf(w.opt, "%d,%d,%d", &w.v, &w.v2, &w.v3);

    int wrc = wdg_fn_call((void *)icc_work, &w, NULL, NULL, NULL, 8000);
    char out[1536];
    int n = snprintf(out, sizeof(out), "%s rc=%d wdg=%d (buzz=%p dim=%p off=%p led=%p dev=%d fx=%d)",
        w.sub, w.rc, wrc, (void*)g_iccSetBuzzer, (void*)g_iccSetBuzzerDim,
        (void*)g_iccSetBuzzerOff, (void*)g_iccSetLedDim,
        access(ICC_INDICATOR_DEV, F_OK) == 0, g_ledfx_run);
    if (w.probe[0] && n < (int)sizeof(out) - 20)
        n += snprintf(out + n, sizeof(out) - n, " probe=[%s]", w.probe);
    if (w.rc == 0)
        send_response(session->sock, RESP_OK, out, n);
    else
        send_response(session->sock, RESP_ERROR, out, n);
}

// SAVE_DELETE — delete a save image + companion .bin from the console.
void handle_save_delete(client_session_t *session, const char *path) {
    if (!path || !*path) { send_error(session->sock, "No save path provided"); return; }
    pthread_mutex_lock(&g_save_mtx);
    if (g_save_mounted && !strcmp(g_save_src, path)) {
        pthread_mutex_unlock(&g_save_mtx);
        send_error(session->sock, "Save is mounted — unmount it first");
        return;
    }
    pthread_mutex_unlock(&g_save_mtx);

    if (strncmp(path, "/user/home/", 11) && strncmp(path, "/mnt/usb", 8)) {
        send_error(session->sock, "Refusing to delete a non-save path");
        return;
    }
    if (unlink(path) != 0) {
        send_error(session->sock, "Delete failed (file missing or protected)");
        return;
    }
    char binp[PATH_MAX];
    snprintf(binp, sizeof(binp), "%s.bin", path);
    unlink(binp);   // companion sealed key — fine if absent
    send_response(session->sock, RESP_DATA, "DELETED", 7);
    send_notification("Save image deleted");
}

// Handle LAUNCH_GAME - Launch an installed/mounted game by title ID
void handle_launch_game(client_session_t *session, const char *title_id) {
    if (!title_id || strlen(title_id) == 0) {
        send_error(session->sock, "No title ID provided");
        return;
    }
    
    // Validate title ID (CUSA/PPSA + 5 digits)
    if ((strncmp(title_id, "CUSA", 4) != 0 &&
         strncmp(title_id, "PPSA", 4) != 0) ||
        strlen(title_id) != 9) {
        send_error(session->sock, "Invalid title ID format");
        return;
    }
    
    // One guarded call runs the whole init+launch sequence on a single worker
    // thread (v6 same-thread semantics) — a stalled service call burns the
    // worker, never the session. The job lives in static storage: if the
    // launch IPC parks the worker past the watchdog window it may still write
    // results — stack memory here would be long dead. title_id is copied in
    // so it stays valid even after the session buffer is gone.
    static launch_work_t w;
    static char w_title_id[16];
    memset(&w, 0, sizeof(w));
    snprintf(w_title_id, sizeof(w_title_id), "%s", title_id);
    w.title_id = w_title_id;
    int wrc = wdg_fn_call((void*)launch_work, &w, NULL, NULL, NULL, WDG_TIMEOUT_MS);
    int ret = (wrc == 0) ? w.ret : -100 - wrc;

    char msg[512];
    if (ret >= 0) {
        snprintf(msg, sizeof(msg), "Launched %s (app_id=%d)", title_id, ret);
        send_ok(session->sock, msg);
    } else {
        snprintf(msg, sizeof(msg),
                 "Launch failed. fg_user=0x%x  lnc_param=0x%x  lnc_null=0x%x  sys=0x%x",
                 w.fg_user, ret, w.ret_null, w.ret_sys);
        send_error(session->sock, msg);
    }
}

// Helper: scan a directory tree for images up to 2 levels deep and append to response.
// Returns number of screenshots added. (Limited depth to avoid runaway scans.)
static int scan_images_recursive(const char *path, int depth, char *response, int *off, int max_len) {
    if (depth > 5) return 0;  // av_contents/thumbnails/photo/NPXS40087/NPXS40087/{bucket}/file = 5 levels
    
    DIR *d = opendir(path);
    if (!d) return 0;
    
    int added = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        
        struct stat st;
        if (stat(full, &st) != 0) continue;
        
        if (S_ISDIR(st.st_mode)) {
            added += scan_images_recursive(full, depth + 1, response, off, max_len);
        } else if (S_ISREG(st.st_mode)) {
            size_t nlen = strlen(e->d_name);
            int is_image =
                (nlen > 4 && (
                    !strcasecmp(e->d_name + nlen - 4, ".jpg") ||
                    !strcasecmp(e->d_name + nlen - 4, ".png") ||
                    !strcasecmp(e->d_name + nlen - 4, ".bmp"))) ||
                (nlen > 5 && !strcasecmp(e->d_name + nlen - 5, ".jpeg"));
            
            if (!is_image) continue;
            
            int written = snprintf(response + *off, max_len - *off,
                                   "%s|%s|%llu|%lld\n",
                                   full, e->d_name,
                                   (unsigned long long)st.st_size,
                                   (long long)st.st_mtime);
            if (written > 0 && *off + written < max_len) {
                *off += written;
                added++;
            }
        }
    }
    closedir(d);
    return added;
}

// Handle DELETE_SCREENSHOT - Delete all files belonging to a PS5 screenshot.
//
// PS5 screenshot layout (current firmware):
//   /user/av_contents/photo/NPXS40087/NPXS40087/{bucket}/YYYYMMDD_hhmmss_xxxxxxxx.dat   (encrypted original)
//   /user/av_contents/photo/NPXS40087/NPXS40087/{bucket}/YYYYMMDD_hhmmss_xxxxxxxx.meta  (metadata)
//   /user/av_contents/thumbnails/photo/NPXS40087/NPXS40087/{bucket}/YYYYMMDD_hhmmss_xxxxxxxx.jpg.jpeg
//                                                                         (visible thumbnail - what the client lists)
//
// The client usually sends us the .jpg.jpeg path (what it saw).
// We need to delete all three files: thumbnail + .dat + .meta.
void handle_delete_screenshot(client_session_t *session, const char *full_path) {
    if (!full_path || strlen(full_path) == 0) {
        send_error(session->sock, "No path provided");
        return;
    }
    
    // Safety: only allow paths under known screenshot roots
    if (strncmp(full_path, "/user/av_contents/", 18) != 0 &&
        strncmp(full_path, "/user/home/", 11) != 0) {
        send_error(session->sock, "Path not in allowed screenshot directory");
        return;
    }
    
    int removed = 0;
    
    // 1. Delete whatever file the client pointed us at
    if (unlink(full_path) == 0) removed++;
    
    // 2. Derive sibling paths.
    //    Case A: input is thumbnail  "…/thumbnails/photo/…/xxx.jpg.jpeg"
    //            → delete "…/photo/…/xxx.dat" and "…/photo/…/xxx.meta"
    //    Case B: input is original   "…/photo/…/xxx.dat" or ".meta"
    //            → delete corresponding thumbnail "…/thumbnails/photo/…/xxx.jpg.jpeg"
    //            → also delete the sibling (.dat/.meta pair)
    
    const char *thumb_root = strstr(full_path, "/av_contents/thumbnails/photo/");
    const char *orig_root  = strstr(full_path, "/av_contents/photo/");
    // (orig_root will also match if thumb_root is present; we handle that via precedence)
    
    // Extract directory + basename for transformations
    char dir_part[PATH_MAX] = {0};
    char base_part[PATH_MAX] = {0};
    const char *last_slash = strrchr(full_path, '/');
    if (!last_slash) {
        char m[64];
        snprintf(m, sizeof(m), "Deleted %d file(s)", removed);
        send_ok(session->sock, m);
        return;
    }
    size_t dir_len = last_slash - full_path;
    memcpy(dir_part, full_path, dir_len);
    dir_part[dir_len] = '\0';
    strncpy(base_part, last_slash + 1, sizeof(base_part) - 1);
    
    // Strip any known extension suffix to get the base stem
    char stem[PATH_MAX] = {0};
    strncpy(stem, base_part, sizeof(stem) - 1);
    const char *suffixes[] = { ".jpg.jpeg", ".jpeg", ".jpg", ".png", ".dat", ".meta" };
    for (size_t i = 0; i < sizeof(suffixes)/sizeof(suffixes[0]); i++) {
        size_t slen = strlen(suffixes[i]);
        size_t blen = strlen(stem);
        if (blen > slen && !strcasecmp(stem + blen - slen, suffixes[i])) {
            stem[blen - slen] = '\0';
            break;
        }
    }
    
    char sibling[PATH_MAX];
    
    if (thumb_root) {
        // Input is a thumbnail. Build original dir by removing "thumbnails/" segment.
        size_t keep = thumb_root - full_path;  // up to "/av_contents"
        char orig_dir[PATH_MAX];
        snprintf(orig_dir, sizeof(orig_dir), "%.*s/av_contents/photo/%s",
                 (int)keep, full_path,
                 dir_part + keep + strlen("/av_contents/thumbnails/photo/"));
        
        // Delete .dat and .meta siblings
        snprintf(sibling, sizeof(sibling), "%s/%s.dat", orig_dir, stem);
        if (unlink(sibling) == 0) removed++;
        snprintf(sibling, sizeof(sibling), "%s/%s.meta", orig_dir, stem);
        if (unlink(sibling) == 0) removed++;
    } else if (orig_root) {
        // Input is an original (.dat/.meta). Delete the paired file and the thumbnail.
        snprintf(sibling, sizeof(sibling), "%s/%s.dat", dir_part, stem);
        if (strcmp(sibling, full_path) != 0 && unlink(sibling) == 0) removed++;
        snprintf(sibling, sizeof(sibling), "%s/%s.meta", dir_part, stem);
        if (strcmp(sibling, full_path) != 0 && unlink(sibling) == 0) removed++;
        
        // Build thumbnail dir by inserting "thumbnails/" before "photo/"
        size_t keep = orig_root - full_path;  // up to "/av_contents"
        char thumb_dir[PATH_MAX];
        snprintf(thumb_dir, sizeof(thumb_dir), "%.*s/av_contents/thumbnails/photo/%s",
                 (int)keep, full_path,
                 dir_part + keep + strlen("/av_contents/photo/"));
        
        snprintf(sibling, sizeof(sibling), "%s/%s.jpg.jpeg", thumb_dir, stem);
        if (unlink(sibling) == 0) removed++;
        snprintf(sibling, sizeof(sibling), "%s/%s.jpeg", thumb_dir, stem);
        if (unlink(sibling) == 0) removed++;
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Deleted %d file(s)", removed);
    send_ok(session->sock, msg);
}

// Handle LIST_SCREENSHOTS - Scan PS5 screenshot directories
// Structure: /user/av_contents/photo/NPXS40087/{TITLE_ID}/*.jpg|png
// Format: full_path|filename|size|mtime_unix\n...
void handle_list_screenshots(client_session_t *session) {
    const int BUF_SIZE = 262144;  // 256 KB for many screenshots
    char *response = malloc(BUF_SIZE);
    if (!response) {
        send_error(session->sock, "Out of memory");
        return;
    }
    response[0] = '\0';
    int off = 0;
    int count = 0;
    
    // Primary PS5 screenshot locations.
    // NOTE: The *visible* JPEG thumbnails live under /thumbnails/photo/…/.jpg.jpeg
    // The encrypted originals (.dat + .meta) live under /photo/…/ but aren't
    // directly viewable, so we list thumbnails instead.
    const char *roots[] = {
        "/user/av_contents/thumbnails/photo",  // visible JPEG thumbnails (primary)
        "/user/av_contents/thumbnails/video",  // video thumbnails
        "/user/av_contents/photo",             // fallback: any direct .jpg/.png originals
        "/user/av_contents/video",
        "/user/av_contents/sdr",
        "/user/av_contents/extra",
    };
    
    for (size_t i = 0; i < sizeof(roots)/sizeof(roots[0]); i++) {
        count += scan_images_recursive(roots[i], 0, response, &off, BUF_SIZE);
    }
    
    // Fallback: legacy user home paths
    if (count == 0) {
        DIR *home = opendir("/user/home");
        if (home) {
            struct dirent *user_ent;
            while ((user_ent = readdir(home))) {
                if (!strcmp(user_ent->d_name, ".") || !strcmp(user_ent->d_name, "..")) continue;
                
                const char *subdirs[] = { "screenshot", "images/screenshot", "shared" };
                for (size_t si = 0; si < sizeof(subdirs)/sizeof(subdirs[0]); si++) {
                    char ss_path[PATH_MAX];
                    snprintf(ss_path, sizeof(ss_path), "/user/home/%s/%s",
                             user_ent->d_name, subdirs[si]);
                    count += scan_images_recursive(ss_path, 0, response, &off, BUF_SIZE);
                }
            }
            closedir(home);
        }
    }
    
    if (count == 0) {
        strcpy(response, "NO_SCREENSHOTS\n");
        off = strlen(response);
    }
    
    send_response(session->sock, RESP_DATA, response, off);
    free(response);
}

// Unmount work executed on a detached thread so the session can keep feeding
// the client heartbeats while kernel calls block. v6.1.1 fix: the 120s client
// read-timeout hit here because rmdir_recursive could stall forever on busy
// vnodes with nobody sending anything on the socket.
typedef struct {
    char title_id[16];
    int  client_sock;
    int  done;              // atomic: heartbeat stops sending once this is set
} unmount_job_t;

static void unmount_body(unmount_job_t* job);

static void* unmount_worker(void* arg) {
    unmount_job_t* job = (unmount_job_t*)arg;
    int sock = job->client_sock;

    progress_socket_set(sock);

    wdg_install_handler();
    if (sigsetjmp(t_wdg_jmp, 1) == 0) {
        t_wdg_armed = 1;
        unmount_body(job);
        t_wdg_armed = 0;
    } else {
        // Crash path: keep the process alive and release locks we might hold.
        t_wdg_armed = 0;
        pthread_mutex_unlock(&g_extendedinfo_lock);
        char msg[256];
        snprintf(msg, sizeof(msg), "Unmount of %s crashed (contained)", job->title_id);
        __atomic_store_n(&job->done, 1, __ATOMIC_SEQ_CST);
        send_resp_locked(sock, RESP_ERROR, msg, (uint32_t)strlen(msg) + 1);
        progress_socket_clear(sock);
    }
    free(job);
    return NULL;
}

static void unmount_body(unmount_job_t* job) {
    const char* title_id = job->title_id;
    int sock = job->client_sock;

    // game_mounter model — unmount the nullfs FIRST, then delete the
    // /user/app + /user/appmeta dirs. No registry IPC at all.
    int busy = full_title_cleanup(title_id, send_progress_message);

    char msg[256];
    if (busy) {
        snprintf(msg, sizeof(msg), "Unmounted %s (mount stayed busy — game running? close it and retry)", title_id);
    } else {
        snprintf(msg, sizeof(msg), "Unmounted %s", title_id);
    }
    // done must go up BEFORE the OK so the heartbeat can never emit a progress
    // frame after the final response (would corrupt the client's next read).
    __atomic_store_n(&job->done, 1, __ATOMIC_SEQ_CST);
    send_resp_locked(sock, RESP_OK, msg, (uint32_t)strlen(msg) + 1);
    progress_socket_clear(sock);

    // Same auto-refresh as mount — under kstuff the direct-DB unregister runs
    // on a detached worker, so give it a moment to delete the row before the
    // UI re-reads it (otherwise the dead icon survives this refresh).
    if (!etahen_present()) {
        send_notification("Refreshing home screen...");
        usleep(2500 * 1000);
        restart_shellui();
    }
}

// Heartbeat thread: streams a RESP_PROGRESS every 3s so the client never sits
// on a silent socket past its read timeout while the unmount grinds on.
static void* unmount_heartbeat(void* arg) {
    unmount_job_t* job = (unmount_job_t*)arg;   // same allocation, not freed here
    struct timespec one_sec = { 1, 0 };
    for (int i = 0; i < 40; i++) {              // 40 x 3s = 2 min, then give up
        for (int j = 0; j < 3; j++) {
            if (__atomic_load_n(&job->done, __ATOMIC_SEQ_CST)) return NULL;
            nanosleep(&one_sec, NULL);
        }
        if (__atomic_load_n(&job->done, __ATOMIC_SEQ_CST)) return NULL;
        send_progress_message("Still working... (busy files can slow this down)");
    }
    return NULL;
}

// Handle UNMOUNT_GAME - Unmount a specific game by title ID
void handle_unmount_game(client_session_t *session, const char *title_id) {
    if (!title_id || strlen(title_id) == 0) {
        send_error(session->sock, "No title ID provided");
        return;
    }
    
    // Validate title ID format
    if ((strncmp(title_id, "CUSA", 4) != 0 && 
         strncmp(title_id, "PPSA", 4) != 0) || 
        strlen(title_id) != 9) {
        send_error(session->sock, "Invalid title ID format");
        return;
    }

    unmount_job_t* job = (unmount_job_t*)malloc(sizeof(unmount_job_t));
    if (!job) {
        send_error(session->sock, "Out of memory");
        return;
    }
    memset(job, 0, sizeof(*job));
    snprintf(job->title_id, sizeof(job->title_id), "%s", title_id);
    job->client_sock = session->sock;

    // Immediate acknowledgement: the client hears from us within milliseconds.
    progress_socket_set(session->sock);
    send_progress_message("Starting unmount...");

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t hb_tid;
    bool hb_ok = (pthread_create(&hb_tid, &at, unmount_heartbeat, job) == 0);
    if (!hb_ok) pthread_attr_destroy(&at);

    pthread_t w_tid;
    if (pthread_create(&w_tid, hb_ok ? NULL : &at, unmount_worker, job) != 0) {
        if (hb_ok) pthread_cancel(hb_tid);
        pthread_attr_destroy(&at);
        free(job);
        progress_socket_clear(session->sock);
        send_error(session->sock, "Failed to start unmount worker");
        return;
    }
    if (hb_ok) pthread_attr_destroy(&at);
    // Session thread returns to the command loop immediately — the workers own
    // the response now (progress heartbeats + final OK).
}

// Fire-and-forget unregister: the home screen reads its icon list from the
// app.db registry (populated by AppInstallAll at mount time), NOT from the
// filesystem — deleting /user/app alone leaves a dead icon forever.
// sceAppInstUtilAppUnInstall is the only call that removes the registry row,
// but it can block ~10s while ShellCore commits, so it runs on its own
// detached thread: the unmount returns immediately and the icon disappears
// whenever the registry answers. The call itself still goes through
// wdg_fn_call so a faulting IPC is contained to one leaked worker.
typedef struct { char title_id[16]; } unreg_job_t;
static void* unregister_worker(void* arg) {
    unreg_job_t* j = (unreg_job_t*)arg;
    char *tid = (char*)malloc(16);
    if (!tid) { free(j); return NULL; }
    snprintf(tid, 16, "%s", j->title_id);
    free(j);
    if (!etahen_present()) {
        // kstuff-light: no daemon IPC — delete the registry rows directly.
        appdb_direct_unregister(tid);
        free(tid);
        return NULL;
    }
    wdg_fn_call((void*)sceAppInstUtilInitialize, NULL, NULL, NULL, NULL, 30000);
    int rc = wdg_fn_call((void*)sceAppInstUtilAppUnInstall, (void*)tid, NULL, NULL, NULL, 60000);
    if (rc != -2) free(tid);   // timeout → parked worker may still read it
    return NULL;
}
static void unregister_title_async(const char* title_id) {
    unreg_job_t* j = (unreg_job_t*)malloc(sizeof(*j));
    if (!j) return;
    snprintf(j->title_id, sizeof(j->title_id), "%s", title_id);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    if (pthread_create(&tid, &at, unregister_worker, j) != 0) free(j);
    pthread_attr_destroy(&at);
}

// Full cleanup of one title — dump_installer order (v6.1.2 proven):
//   1. unmount the nullfs overlay first (AppUnInstall on a LIVE mount hangs),
//   2. kick off AppUnInstall in the background (removes the registry row →
//      the home-screen icon actually disappears),
//   3. delete /user/app/<TID> and /user/appmeta/<TID> recursively — clears
//      mount.lnk so our own game list is accurate right away.
// Every delete goes through guarded unlink/rmdir (busy vnode = one watchdog
// deadline, never a wedge). Returns 0 on success.
static int full_title_cleanup(const char *title_id, void (*progress)(const char *)) {
    char system_ex_app[PATH_MAX];
    char user_app_dir[PATH_MAX];
    char appmeta_dir[PATH_MAX];
    int rc = 0;

    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);
    snprintf(appmeta_dir, sizeof(appmeta_dir), "/user/appmeta/%s", title_id);

    // 1) Unmount the nullfs overlay FIRST (guarded: 5s normal + 5s forced).
    if (progress) progress("Unmounting game data...");
    if (is_mounted(system_ex_app)) {
        if (unmount_guarded(system_ex_app, 0) != 0) {
            if (unmount_guarded(system_ex_app, MNT_FORCE) != 0) {
                rc = -1;   // stayed busy (game running?) — keep going anyway
            }
        }
    }
    usleep(100000); // 100ms

    // 2) Remove the registry row in the background — this is what actually
    //    takes the icon off the home screen (filesystem deletes alone don't).
    if (progress) progress("Removing game from home screen...");
    unregister_title_async(title_id);

    // 3) Delete the registration dirs (clears mount.lnk for our game list).
    if (progress) progress("Cleaning up game folders...");
    rmdir_recursive(user_app_dir);
    rmdir_recursive(appmeta_dir);

    if (progress) progress("Finalizing...");
    return rc;
}

// Helper: read a sysctl string value
static int sysctl_get_string(const char *name, char *buf, size_t buflen) {
    size_t len = buflen;
    if (sysctlbyname(name, buf, &len, NULL, 0) == 0) {
        buf[len < buflen ? len : buflen - 1] = '\0';
        return 0;
    }
    return -1;
}

// Helper: read a sysctl integer value
static int sysctl_get_int(const char *name, int *val) {
    size_t len = sizeof(int);
    return sysctlbyname(name, val, &len, NULL, 0);
}

// Helper: read a sysctl uint64 value
static int sysctl_get_uint64(const char *name, uint64_t *val) {
    size_t len = sizeof(uint64_t);
    return sysctlbyname(name, val, &len, NULL, 0);
}

// Numeric-MIB sysctl — sysctlbyname on PS5 lacks several hw.* names, but the
// numeric OIDs often still work. Falls back between them.
static int sysctl_mib_uint64(int mib0, int mib1, uint64_t *val) {
    int mib[2] = { mib0, mib1 };
    size_t len = sizeof(uint64_t);
    return sysctl(mib, 2, val, &len, NULL, 0);
}

// Cached direct-memory total — filled by whichever path reads it first
// (hw_info extern call is proven to work on-device).
static size_t g_direct_total_cache = 0;
static uint64_t g_physmem_cache = 0;   // populated by MEM_INFO page-table stats

// Cache static hardware info (never changes). We read it once at first call
// and serve the cached copy forever after — this avoids hitting the kernel
// APIs on every refresh.
static pthread_mutex_t g_hwinfo_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_hwinfo_response[4096];
static int  g_hwinfo_len = 0;
static int  g_hwinfo_valid = 0;

// Handle GET_HW_INFO - static info served from one-shot cache
void handle_get_hw_info(client_session_t *session) {
    pthread_mutex_lock(&g_hwinfo_lock);
    
    if (!g_hwinfo_valid) {
        char model_name[1024] = {0};
        char serial[1024] = {0};
        
        // One-time reads of static kernel hw info
        if (sceKernelGetHwModelName(model_name) != 0 || model_name[0] == '\0') {
            strcpy(model_name, "PlayStation 5");
        }
        if (sceKernelGetHwSerialNumber(serial) != 0 || serial[0] == '\0') {
            strcpy(serial, "N/A");
        }
        
        char hw_machine[256] = {0};
        char ostype[64] = {0};
        char osrelease[64] = {0};
        sysctl_get_string("hw.machine", hw_machine, sizeof(hw_machine));
        sysctl_get_string("kern.ostype", ostype, sizeof(ostype));
        sysctl_get_string("kern.osrelease", osrelease, sizeof(osrelease));

        // kern.osrelease on Orbis literally reports "0.0-prototype" — Sony
        // never bumped the FreeBSD version string. Show the real OS name +
        // firmware instead.
        {
            pthread_mutex_lock(&g_krpc_lock);
            uint32_t fw = kernel_get_fw_version();
            pthread_mutex_unlock(&g_krpc_lock);
            if (fw) {
                snprintf(ostype, sizeof(ostype), "Orbis OS");
                snprintf(osrelease, sizeof(osrelease), "%x.%02x",
                         (fw >> 24) & 0xFF, (fw >> 16) & 0xFF);
            }
        }
        
        int ncpu = 0;
        sysctl_get_int("hw.ncpu", &ncpu);
        
        // Physical RAM — try PS5-specific API first (ABI-agnostic wrapper),
        // then sysctl chain.
        uint64_t physmem = 0;
        {
            size_t dt = 0;
            if (wdg_avail_mem((void *)&sceKernelGetDirectMemorySize, &dt) == 0 && dt)
                physmem = dt;
        }
        if (physmem != 0) g_direct_total_cache = physmem;
        if (physmem == 0) sysctl_get_uint64("hw.physmem",  &physmem);
        if (physmem == 0) sysctl_get_uint64("hw.realmem",  &physmem);
        if (physmem == 0) sysctl_get_uint64("hw.usermem",  &physmem);
        if (physmem == 0) {
            int pagesize = 0;
            uint64_t page_count = 0;
            sysctl_get_int("hw.pagesize", &pagesize);
            if (sysctl_get_uint64("vm.stats.vm.v_page_count", &page_count) == 0 &&
                pagesize > 0 && page_count > 0) {
                physmem = page_count * (uint64_t)pagesize;
            }
        }
        // If every dynamic source failed, use the page-table value cached
        // by MEM_INFO (real physical RAM on this kernel).
        if (physmem == 0) physmem = g_physmem_cache;
        if (physmem == 0) physmem = g_direct_total_cache;
        
        g_hwinfo_len = snprintf(g_hwinfo_response, sizeof(g_hwinfo_response),
            "model=%s\n"
            "serial=%s\n"
            "has_wlan_bt=1\n"
            "has_optical_out=0\n"
            "hw_model=%s\n"
            "hw_machine=%s\n"
            "os=%s %s\n"
            "ncpu=%d\n"
            "physmem=%llu\n",
            model_name, serial, model_name, hw_machine,
            ostype, osrelease, ncpu, (unsigned long long)physmem);
        g_hwinfo_valid = 1;
    }
    
    // Copy out under lock
    char out[4096];
    int out_len = g_hwinfo_len;
    if (out_len > (int)sizeof(out)) out_len = (int)sizeof(out);
    memcpy(out, g_hwinfo_response, out_len);
    pthread_mutex_unlock(&g_hwinfo_lock);
    
    send_response(session->sock, RESP_DATA, out, out_len);
}

// Serialize sensor reads across concurrent client sessions to avoid
// hitting non-reentrant Sony kernel APIs from multiple threads.
static pthread_mutex_t g_sensor_lock = PTHREAD_MUTEX_INITIALIZER;

// Small short-lived cache (1 second) so rapid-fire polls don't hammer the APIs.
static struct {
    time_t last_read;
    int cpu_temp;
    int soc_temp;
    long cpu_freq_mhz;
    uint32_t power_mw;
    int valid;
} g_sensor_cache = {0};

// Handle GET_TEMPS - Uses verified Sony kernel APIs (new SDK 2026)
void handle_get_temps(client_session_t *session) {
    char response[1024];
    int off = 0;
    
    int cpu_temp = 0;
    int soc_temp = 0;
    long cpu_freq_mhz = 0;
    uint32_t power_mw = 0;
    
    pthread_mutex_lock(&g_sensor_lock);
    
    time_t now = time(NULL);
    if (g_sensor_cache.valid && (now - g_sensor_cache.last_read) < 1) {
        // Serve from cache - avoids hammering the kernel APIs
        cpu_temp     = g_sensor_cache.cpu_temp;
        soc_temp     = g_sensor_cache.soc_temp;
        cpu_freq_mhz = g_sensor_cache.cpu_freq_mhz;
        power_mw     = g_sensor_cache.power_mw;
    } else {
        // Fresh read. Only the APIs that appear in the official SDK sample.
        // Each call is watchdog-guarded: these are kernel/ICC IPC paths that
        // can stall in a payload process, and the shared sensor lock would
        // otherwise wedge every later request.
        int rc;
        
        rc = wdg_out_int((void *)&sceKernelGetCpuTemperature, &cpu_temp);
        if (rc != 0) cpu_temp = 0;
        
        // SocSensorTemperature(int sensor_id, int* out) — sensor 0
        rc = wdg_cpuusage_one((void *)&sceKernelGetSocSensorTemperature, 0, &soc_temp);
        if (rc != 0) soc_temp = 0;
        
        intptr_t freq_hz = wdg_fn_call64((void *)&sceKernelGetCpuFrequency,
                                         NULL, NULL, NULL, NULL, WDG_TIMEOUT_MS);
        cpu_freq_mhz = (freq_hz > 0 && freq_hz != INTPTR_MIN)
                     ? (long)freq_hz / (1000 * 1000) : 0;
        if (cpu_freq_mhz <= 0) {
            uint64_t tsc_freq = 0;
            if (sysctl_get_uint64("machdep.tsc_freq", &tsc_freq) == 0 && tsc_freq > 0) {
                cpu_freq_mhz = (long)(tsc_freq / 1000000ULL);
            }
        }
        
        // Power consumption API — call at most once every 5 seconds.
        // Separate tracker so the 1s sensor cache doesn't force a re-read of this API.
        static time_t last_power_read = 0;
        static uint32_t last_power_mw = 0;
        if ((now - last_power_read) >= 5) {
            int pw = 0;
            if (wdg_out_int((void *)&sceKernelGetSocPowerConsumption, &pw) == 0) {
                // Reject abnormal readings (> 500 W is impossible for PS5)
                if (pw >= 0 && (uint32_t)pw < 500000) last_power_mw = (uint32_t)pw;
            }
            last_power_read = now;
        }
        power_mw = last_power_mw;
        
        // Update cache
        g_sensor_cache.last_read    = now;
        g_sensor_cache.cpu_temp     = cpu_temp;
        g_sensor_cache.soc_temp     = soc_temp;
        g_sensor_cache.cpu_freq_mhz = cpu_freq_mhz;
        g_sensor_cache.power_mw     = power_mw;
        g_sensor_cache.valid        = 1;
    }
    
    pthread_mutex_unlock(&g_sensor_lock);
    
    // Real system load via getloadavg() (SDK v0.39+) - map 1/5/15-minute
    // load averages onto the per-core usage bars of the clients.
    double loads[3] = {0, 0, 0};
    int have_loads = (get_load_averages(loads) == 3);
    
    off += snprintf(response + off, sizeof(response) - off,
        "cpu_temp=%d\n"
        "soc_temp=%d\n"
        "cpu_freq_mhz=%ld\n"
        "soc_clock_mhz=0\n"
        "soc_power_mw=%u\n",
        cpu_temp,
        soc_temp,
        cpu_freq_mhz,
        power_mw);
    
    for (int i = 0; i < 8; i++) {
        // Show the 1/5/15m load on the first three bars, then the 1m load
        // (capped at 8.0 = 800%) on the rest; 0 when unavailable.
        double v = have_loads
            ? ((i == 0) ? loads[0] : (i == 1) ? loads[1] : (i == 2) ? loads[2] : loads[0])
            : 0.0;
        int pct = (int)(v * 100.0);
        if (pct < 0) pct = 0;
        if (pct > 800) pct = 800;
        off += snprintf(response + off, sizeof(response) - off,
            "cpu_usage_%d=%d\n", i, pct);
    }
    
    send_response(session->sock, RESP_DATA, response, off);
}

// Handle GET_EXTENDED_INFO - Get extended system information
void handle_get_extended_info(client_session_t *session) {
    char response[4096];
    int off = 0;
    
    // Resolve the extended kernel functions from the loaded kernel module
    resolve_extendedinfo_functions();
    
    // System/Firmware version — kernel_get_fw_version() reads the version
    // straight from kernel memory (kernel RPC), no Sony IPC to stall on.
    char fw_version[64] = "Unknown";
    char prospero_version[64] = "Unknown";
    {
        pthread_mutex_lock(&g_krpc_lock);
        uint32_t fw = kernel_get_fw_version();
        pthread_mutex_unlock(&g_krpc_lock);
        if (fw) {
            // Encoded BCD in the top half: 0x13600007 -> "13.60"
            snprintf(fw_version, sizeof(fw_version), "%x.%02x",
                     (fw >> 24) & 0xFF, (fw >> 16) & 0xFF);
        }
    }
    if (g_sceKernelGetSystemSwVersion && strcmp(fw_version, "Unknown") == 0)
        wdg_out_version((void *)g_sceKernelGetSystemSwVersion, fw_version, sizeof(fw_version));
    strcpy(prospero_version, fw_version);
    
    // Product info — product code/str are plain strings, not version structs.
    char product_code[64] = "Unknown";
    char product_str[128] = "PlayStation 5";
    int dbg_pc = -3, dbg_ps = -3;
    char pc_raw[64], ps_raw[64];
    memset(pc_raw, 0, sizeof(pc_raw)); memset(ps_raw, 0, sizeof(ps_raw));
    if (g_sceKernelGetProductCode) {
        dbg_pc = wdg_fn_call((void *)g_sceKernelGetProductCode, pc_raw,
                             (void *)sizeof(pc_raw), NULL, NULL, WDG_INFO_TIMEOUT_MS);
        // Try single-arg variant if the buffer stayed empty.
        int printable = 0;
        for (int p = 0; p < 64; p++) if (pc_raw[p] >= 0x20 && pc_raw[p] < 0x7f) printable++;
        if (!printable && dbg_pc != -2 && dbg_pc != WDG_RESULT_CRASHED) {
            memset(pc_raw, 0, sizeof(pc_raw));
            int r2 = wdg_fn_call((void *)g_sceKernelGetProductCode, pc_raw,
                                 NULL, NULL, NULL, WDG_INFO_TIMEOUT_MS);
            dbg_pc = (dbg_pc * 1000) + r2;
        }
    }
    if (g_sceKernelGetProductStr)
        dbg_ps = wdg_fn_call((void *)g_sceKernelGetProductStr, ps_raw,
                             (void *)sizeof(ps_raw), NULL, NULL, WDG_INFO_TIMEOUT_MS);
    for (int p = 0; p < 48 && product_code[0] == 'U'; p++)
        if (pc_raw[p] >= 0x20 && pc_raw[p] < 0x7f) { snprintf(product_code, sizeof(product_code), "%.40s", pc_raw + p); break; }
    for (int p = 0; p < 48 && ps_raw[0]; p++)
        if (ps_raw[p] >= 0x20 && ps_raw[p] < 0x7f) { snprintf(product_str, sizeof(product_str), "%.80s", ps_raw + p); break; }
    
    // ICC Hardware info - Operating time
    uint64_t operating_time_sec = 0;
    int dbg_opt = -3;
    if (g_sceKernelIccGetPowerOperatingTime)
        dbg_opt = wdg_out_u64((void *)g_sceKernelIccGetPowerOperatingTime, &operating_time_sec);
    // Fallback: uptime since boot via kern.boottime (real value — the ICC
    // lifetime counter isn't populated in payload context).
    if (operating_time_sec == 0) {
        struct timeval bt;
        size_t bt_len = sizeof(bt);
        if (sysctlbyname("kern.boottime", &bt, &bt_len, NULL, 0) == 0 && bt.tv_sec > 0) {
            struct timeval now;
            gettimeofday(&now, NULL);
            if (now.tv_sec > bt.tv_sec)
                operating_time_sec = (uint64_t)(now.tv_sec - bt.tv_sec);
        }
    }
    
    // Boot/shutdown counts — verified ABI is int fn(uint64_t *out) returning
    // the packed pair in one u64 (ps5-hwinfo).
    uint32_t boot_count = 0, shutdown_count = 0;
    uint64_t bs_raw = 0;
    if (g_sceKernelIccGetPowerNumberOfBootShutdown) {
        if (wdg_out_u64((void *)g_sceKernelIccGetPowerNumberOfBootShutdown, &bs_raw) == 0) {
            // Elf-arsenal treats the whole u64 as one counter; if the low
            // word is zero the real layout is probably {pad, count} — pick
            // whichever half is nonzero.
            uint32_t lo = (uint32_t)(bs_raw & 0xFFFFFFFFu);
            uint32_t hi = (uint32_t)(bs_raw >> 32);
            boot_count     = lo;
            shutdown_count = hi;
            if (!boot_count && !hi) boot_count = (uint32_t)bs_raw;
            // Some FWs pack {pad, count} — the only nonzero half is the real
            // power-cycle counter, mirror it into boot_count for the UI.
            if (!boot_count && hi) boot_count = hi;
            if (!shutdown_count && lo) shutdown_count = lo;
        }
    }
    
    // Thermal alert
    int thermal_alert = 0;
    if (g_sceKernelIccGetThermalAlert)
        wdg_out_int((void *)g_sceKernelIccGetThermalAlert, &thermal_alert);
    
    // BD Drive state
    int bd_power_state = -1;
    if (g_sceKernelIccGetBDPowerState)
        wdg_out_int((void *)g_sceKernelIccGetBDPowerState, &bd_power_state);
    
    off += snprintf(response + off, sizeof(response) - off,
        "firmware_version=%s\n"
        "prospero_version=%s\n"
        "product_code=%s\n"
        "product_str=%s\n"
        "total_operating_time_sec=%llu\n"
        "total_operating_time_hours=%llu\n"
        "boot_count=%u\n"
        "shutdown_count=%u\n"
        "thermal_alert=%d\n"
        "bd_drive_power=%d\n"
        "dbg_pc=%d dbg_ps=%d dbg_opt=%d dbg_bs=%llu\n"
        "dbg_pc_raw=%02x%02x%02x%02x.%02x%02x%02x%02x\n"
        "dbg_ps_raw=%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x\n"
        "dbg_ps_str=%.32s\n",
        fw_version,
        prospero_version,
        product_code,
        product_str,
        (unsigned long long)operating_time_sec,
        (unsigned long long)(operating_time_sec / 3600),
        boot_count,
        shutdown_count,
        thermal_alert,
        bd_power_state,
        dbg_pc, dbg_ps, dbg_opt, (unsigned long long)bs_raw,
        (unsigned char)pc_raw[0], (unsigned char)pc_raw[1], (unsigned char)pc_raw[2], (unsigned char)pc_raw[3],
        (unsigned char)pc_raw[8], (unsigned char)pc_raw[9], (unsigned char)pc_raw[10], (unsigned char)pc_raw[11],
        (unsigned char)ps_raw[0], (unsigned char)ps_raw[1], (unsigned char)ps_raw[2], (unsigned char)ps_raw[3],
        (unsigned char)ps_raw[8], (unsigned char)ps_raw[9], (unsigned char)ps_raw[10], (unsigned char)ps_raw[11],
        (unsigned char)ps_raw[16], (unsigned char)ps_raw[17], (unsigned char)ps_raw[18], (unsigned char)ps_raw[19],
        ps_raw);
    
    send_response(session->sock, RESP_DATA, response, off);
}

// ---- CPU usage via sceKernelGetCpuUsage -------------------------------
// Verified ABI (ps5-hwinfo): fills an array of proc_stats_t
//   { u32 pid; u32 tid; i64 user_sec; i64 user_usec; i64 sys_sec; i64 sys_usec }
// Per-core busy% is computed from the kernel's per-CPU idle threads
// (named "SceIdleCpuN"): busy% = 100 * (1 - idle_delta / wall_delta).
typedef struct { int64_t tv_sec, tv_usec; } orbis_tv_t;
typedef struct {
    uint32_t pid, tid;
    orbis_tv_t user, sys;
} cpu_proc_stat_t;   // 40 bytes

#define CPU_SAMPLE_MAX 3072
#define CPU_LANES 16

static cpu_proc_stat_t *g_cpu_prev = NULL;
static cpu_proc_stat_t *g_cpu_cur  = NULL;
static int32_t          g_cpu_prev_n = 0;
static struct timespec  g_cpu_prev_wall;
static uint32_t         g_idle_tid[CPU_LANES];
static int              g_idle_resolved = 0;
static char             g_idle_names[24][64];
static int              g_idle_names_n = 0;

static double cpu_stat_secs(const cpu_proc_stat_t *p) {
    return (double)(p->user.tv_sec + p->sys.tv_sec)
         + (double)(p->user.tv_usec + p->sys.tv_usec) / 1e6;
}

static int cpu_take_sample(cpu_proc_stat_t *buf, int32_t *count,
                           struct timespec *wall) {
    int32_t cap = CPU_SAMPLE_MAX;
    intptr_t r = wdg_fn_call64((void *)g_sceKernelGetCpuUsageList,
                               buf, &cap, NULL, NULL, WDG_INFO_TIMEOUT_MS);
    if (r != 0 || cap <= 0 || cap > CPU_SAMPLE_MAX) return -1;
    *count = cap;
    clock_gettime(CLOCK_MONOTONIC, wall);
    return 0;
}

static void cpu_resolve_idle(const cpu_proc_stat_t *buf, int32_t count) {
    char name[64];
    int found = 0;
    memset(g_idle_tid, 0, sizeof(g_idle_tid));
    // GetThreadName is a plain libkernel syscall (no ICC/mailbox IPC), so
    // call it directly — a per-thread watchdog spawn over ~1200 threads
    // would take seconds. Stop once every lane is mapped.
    int (*getname)(uint32_t, char*) = g_sceKernelGetThreadName;
    for (int32_t i = 0; i < count; i++) {
        int lane = -1;
        memset(name, 0, sizeof(name));
        if (getname(buf[i].tid, name) != 0)
            continue;
        int rv = !strcmp(name, "SceIdleCpuRv");
        // Catch anything idle-ish for diagnostics (kernel idle threads on
        // some FWs are named differently, e.g. "SceIdleThreadN").
        if (strstr(name, "Idle") || strstr(name, "idle") || strstr(name, "IDLE")) {
            if (g_idle_names_n < 24)
                snprintf(g_idle_names[g_idle_names_n++], 64, "%s", name);
        }
        if ((sscanf(name, "SceIdleCpu%d", &lane) == 1 ||
             sscanf(name, "SceIdleThread%d", &lane) == 1 ||
             sscanf(name, "SceIdle%d", &lane) == 1) &&
            lane >= 0 && lane < CPU_LANES && !g_idle_tid[lane]) {
            g_idle_tid[lane] = buf[i].tid;
            found++;
        } else if (rv && !g_idle_tid[13]) {
            g_idle_tid[13] = buf[i].tid;
            found++;
        }
        if (found >= CPU_LANES) break;
    }
    if (found) g_idle_resolved = 1;
}

void handle_get_cpu_usage(client_session_t *session) {
    static pthread_mutex_t cpuu_lock = PTHREAD_MUTEX_INITIALIZER;
    char response[640];
    int off = 0;
    int usage[CPU_LANES] = {0};
    int nlanes = 8;
    int dbg_rc = -9;
    int32_t cur_n = 0;
    struct timespec cur_wall = {0, 0};

    resolve_extendedinfo_functions();

    pthread_mutex_lock(&cpuu_lock);
    if (!g_cpu_prev) g_cpu_prev = malloc(CPU_SAMPLE_MAX * sizeof(*g_cpu_prev));
    if (!g_cpu_cur)  g_cpu_cur  = malloc(CPU_SAMPLE_MAX * sizeof(*g_cpu_cur));
    if (!g_cpu_prev || !g_cpu_cur) {
        pthread_mutex_unlock(&cpuu_lock);
        send_error(session->sock, "Out of memory");
        return;
    }

    if (!g_sceKernelGetCpuUsageList) {
        pthread_mutex_unlock(&cpuu_lock);
        send_error(session->sock, "CPU usage API unavailable");
        return;
    }

    dbg_rc = cpu_take_sample(g_cpu_cur, &cur_n, &cur_wall);
    if (dbg_rc == 0) {
        if (!g_idle_resolved && g_sceKernelGetThreadName)
            cpu_resolve_idle(g_cpu_cur, cur_n);

        if (g_cpu_prev_n > 0 && g_idle_resolved) {
            double wall = (double)(cur_wall.tv_sec - g_cpu_prev_wall.tv_sec)
                + (double)(cur_wall.tv_nsec - g_cpu_prev_wall.tv_nsec) / 1e9;
            if (wall > 0) {
                for (int lane = 0; lane < CPU_LANES; lane++) {
                    uint32_t tid = g_idle_tid[lane];
                    if (!tid) continue;
                    const cpu_proc_stat_t *pi = NULL, *ci = NULL;
                    for (int32_t i = 0; i < g_cpu_prev_n && !pi; i++)
                        if (g_cpu_prev[i].tid == tid) pi = &g_cpu_prev[i];
                    for (int32_t i = 0; i < cur_n && !ci; i++)
                        if (g_cpu_cur[i].tid == tid) ci = &g_cpu_cur[i];
                    if (!pi || !ci) continue;
                    double idle_d = cpu_stat_secs(ci) - cpu_stat_secs(pi);
                    double ratio = idle_d / wall;
                    if (ratio < 0) ratio = 0;
                    if (ratio > 1) ratio = 1;
                    usage[lane] = (int)((1.0 - ratio) * 100.0);
                }
            }
        }

        // rotate cur -> prev
        cpu_proc_stat_t *tmp = g_cpu_prev;
        g_cpu_prev = g_cpu_cur; g_cpu_cur = tmp;
        g_cpu_prev_n = cur_n;
        g_cpu_prev_wall = cur_wall;
    }
    pthread_mutex_unlock(&cpuu_lock);

    int total = 0, shown = 0;
    for (int i = 0; i < nlanes; i++) {
        if (g_idle_tid[i]) {
            total += usage[i];
            shown++;
        }
        off += snprintf(response + off, sizeof(response) - off,
            "core%d=%d\n", i, usage[i]);
    }
    off += snprintf(response + off, sizeof(response) - off,
        "average=%d\ndbg_rc=%d dbg_n=%d dbg_idle=%d\n",
        shown ? total / shown : 0, dbg_rc, (int)cur_n, g_idle_resolved);
    for (int i = 0; i < g_idle_names_n && off < (int)sizeof(response) - 80; i++)
        off += snprintf(response + off, sizeof(response) - off, "dbg_idle_name=%s\n", g_idle_names[i]);

    send_response(session->sock, RESP_DATA, response, off);
}

// Handle GET_MEMORY_INFO - Get detailed memory information
void handle_get_memory_info(client_session_t *session) {
    char response[3072];
    int off = 0;
    
    resolve_extendedinfo_functions();
    
    // Direct memory (main GDDR6). The real Orbis ABI may be
    // int fn(uint64_t *size) rather than size_t fn(void) — pass the extern
    // through the ABI-agnostic wrapper so both work.
    intptr_t dbg_dm[5] = {0}, dbg_am[5] = {0}, dbg_fx[5] = {0};
    intptr_t dbg_dms_r = INTPTR_MIN;
    size_t direct_total = 0;
    // u64 sceKernelGetDirectMemorySize(void) — direct return value.
    // Try extern import first, then the NID-resolved libkernel_sys export.
    dbg_dms_r = wdg_fn_call64((void *)&sceKernelGetDirectMemorySize,
                              NULL, NULL, NULL, NULL, WDG_INFO_TIMEOUT_MS);
    if (dbg_dms_r == INTPTR_MIN || dbg_dms_r == 0) {
        if (g_sceKernelGetDirectMemorySizeReal)
            dbg_dms_r = wdg_fn_call64((void *)g_sceKernelGetDirectMemorySizeReal,
                                      NULL, NULL, NULL, NULL, WDG_INFO_TIMEOUT_MS);
    }
    if (dbg_dms_r != INTPTR_MIN && dbg_dms_r > 0 &&
        !(dbg_dms_r >= 0x80000000 && dbg_dms_r <= 0x8002FFFF))
        direct_total = (size_t)dbg_dms_r;
    if (direct_total == 0) {
        uint64_t v = 0;
        if (sysctl_get_uint64("hw.physmem", &v) != 0)
            sysctl_get_uint64("hw.realmem", &v);
        direct_total = (size_t)v;
    }
    if (direct_total == 0)
        direct_total = g_direct_total_cache;
    else
        g_direct_total_cache = direct_total;
    size_t direct_available = 0;
    if (g_sceKernelAvailableDirectMemorySize)
        wdg_avail_dmem((void *)g_sceKernelAvailableDirectMemorySize,
                       &direct_available, dbg_am);
    
    // Flexible memory
    size_t flexible_available = 0;
    if (g_sceKernelAvailableFlexibleMemorySize)
        wdg_avail_mem_dbg((void *)g_sceKernelAvailableFlexibleMemorySize,
                          &flexible_available, dbg_fx);
    
    // System RAM via get_page_table_stats(vm=0, type=1) — the same source
    // ShellCore-style monitors use (ps5-hwinfo). type=1 is CPU/system RAM,
    // type=2 is GPU. Returns MB units.
    uint64_t physmem = 0, usermem = 0, freemem = 0;
    intptr_t dbg_pts[8] = {0};
    uint64_t cpu_pt_total = 0, cpu_pt_free = 0;
    uint64_t gpu_pt_total = 0, gpu_pt_free = 0;
    if (g_sceKernelGetPageTableStats) {
        static int s_pt_total[WDG_SCRATCH_SLOTS], s_pt_free[WDG_SCRATCH_SLOTS];
        static volatile unsigned s_pt_next = 0;
        unsigned i = __atomic_fetch_add(&s_pt_next, 1, __ATOMIC_SEQ_CST) % WDG_SCRATCH_SLOTS;
        // type=1 → CPU page-table pool (system RAM)
        s_pt_total[i] = s_pt_free[i] = 0;
        intptr_t r = wdg_fn_call64((void *)g_sceKernelGetPageTableStats,
                                   (void *)0, (void *)1, &s_pt_total[i], &s_pt_free[i],
                                   WDG_INFO_TIMEOUT_MS);
        dbg_pts[0] = r; dbg_pts[1] = s_pt_total[i]; dbg_pts[2] = s_pt_free[i];
        if (r == 0 && s_pt_total[i] > 0) {
            cpu_pt_total = (uint64_t)s_pt_total[i] * 1024ULL * 1024ULL;
            cpu_pt_free  = (uint64_t)s_pt_free[i]  * 1024ULL * 1024ULL;
        }
        // type=2 → GPU page-table pool
        s_pt_total[i] = s_pt_free[i] = 0;
        r = wdg_fn_call64((void *)g_sceKernelGetPageTableStats,
                          (void *)0, (void *)2, &s_pt_total[i], &s_pt_free[i],
                          WDG_INFO_TIMEOUT_MS);
        dbg_pts[3] = r; dbg_pts[4] = s_pt_total[i]; dbg_pts[5] = s_pt_free[i];
        if (r == 0 && s_pt_total[i] > 0) {
            gpu_pt_total = (uint64_t)s_pt_total[i] * 1024ULL * 1024ULL;
            gpu_pt_free  = (uint64_t)s_pt_free[i]  * 1024ULL * 1024ULL;
        }
        // The PS5 has unified GDDR6: CPU pool + GPU pool = physical total.
        // Both numbers come from the API — nothing hardcoded.
        physmem = cpu_pt_total + gpu_pt_total;
        freemem = cpu_pt_free + gpu_pt_free;
        usermem = cpu_pt_total;   // CPU pool = system-usable RAM
        g_physmem_cache = physmem;
    }
    // Configured flexible memory total (u64 return) — what the system is
    // allowed to hand out.
    intptr_t dbg_cfx = INTPTR_MIN;
    if (g_sceKernelConfiguredFlexibleMemorySize) {
        dbg_cfx = wdg_fn_call64((void *)g_sceKernelConfiguredFlexibleMemorySize,
                                NULL, NULL, NULL, NULL, WDG_INFO_TIMEOUT_MS);
        if (dbg_cfx > 0 && !(dbg_cfx >= 0x80000000 && dbg_cfx <= 0x8002FFFF) &&
            physmem == 0)
            physmem = (uint64_t)dbg_cfx;
    }
    // sysctl fallbacks — ps5-hwinfo gets total RAM from
    // vm.stats.vm.v_page_count * hw.pagesize on this kernel.
    uint64_t v_page_count = 0, v_free_count = 0, pagesize = 0;
    int ps = 0;
    int rc_ps = sysctl_get_int("hw.pagesize", &ps);
    pagesize = (uint64_t)ps;
    int rc_vpc = sysctl_get_uint64("vm.stats.vm.v_page_count", &v_page_count);
    if (physmem == 0 && rc_vpc == 0 && v_page_count > 0 && pagesize > 0)
        physmem = v_page_count * pagesize;
    int rc_phys = -1, rc_user = -1;
    if (physmem == 0) {
        rc_phys = sysctl_get_uint64("hw.physmem", &physmem);
        if (rc_phys != 0) rc_phys = sysctl_mib_uint64(6, 5, &physmem);
        rc_user = sysctl_get_uint64("hw.usermem", &usermem);
        if (rc_user != 0) rc_user = sysctl_mib_uint64(6, 6, &usermem);
    }
    int rc_free = sysctl_get_uint64("vm.stats.vm.v_free_count", &v_free_count);
    uint64_t v_wire0 = 0, v_wire10 = 0;
    int rc_wire0  = sysctl_get_uint64("vm.stats.vm0.v_wire_count",  &v_wire0);
    int rc_wire10 = sysctl_get_uint64("vm.stats.vm10.v_wire_count", &v_wire10);
    if (rc_free == 0 && pagesize > 0) {
        freemem = v_free_count * pagesize;
    }
    
    uint64_t dbg_authid = kernel_get_ucred_authid(getpid());
    // In payload context there is no game "direct memory" budget — the
    // page-table CPU pool IS the real system RAM view. Report it through
    // the direct_* fields too so the UI bar shows real usage.
    size_t disp_total = direct_total ? direct_total : (size_t)physmem;
    size_t disp_free  = (size_t)freemem;
    size_t disp_used  = disp_total > disp_free ? disp_total - disp_free : 0;
    int disp_pct = disp_total ? (int)((disp_used * 100) / disp_total) : 0;
    off += snprintf(response + off, sizeof(response) - off,
        "direct_total=%zu\n"
        "direct_available=%zu\n"
        "direct_used=%zu\n"
        "direct_used_percent=%d\n"
        "flexible_available=%zu\n"
        "physical_total=%llu\n"
        "user_memory=%llu\n"
        "free_memory=%llu\n"
        "cpu_pool_total=%llu\n"
        "cpu_pool_free=%llu\n"
        "gpu_pool_total=%llu\n"
        "gpu_pool_free=%llu\n"
        "dbg_physmem_rc=%d dbg_usermem_rc=%d dbg_ps_rc=%d dbg_free_rc=%d dbg_vfree=%llu\n"
        "dbg_vpc=%lld,%llu dbg_wire=%d,%d,%llu,%llu\n"
        "dbg_ptrs=%p,%p,%p,%p dbg_authid=%llx dbg_dms_r=%lld\n"
        "dbg_pts=%lld,%lld,%lld,%lld,%lld,%lld dbg_cfx=%lld\n"
        "dbg_dm=%lld,%lld,%lld,%lld,%lld\n"
        "dbg_am=%lld,%lld,%lld,%lld,%lld\n"
        "dbg_fx=%lld,%lld,%lld,%lld,%lld\n",
        disp_total,
        disp_free,
        disp_used,
        disp_pct,
        flexible_available,
        (unsigned long long)physmem,
        (unsigned long long)usermem,
        (unsigned long long)freemem,
        (unsigned long long)cpu_pt_total,
        (unsigned long long)cpu_pt_free,
        (unsigned long long)gpu_pt_total,
        (unsigned long long)gpu_pt_free,
        rc_phys, rc_user, rc_ps, rc_free,
        (unsigned long long)v_free_count,
        (long long)rc_vpc, (unsigned long long)v_page_count,
        rc_wire0, rc_wire10,
        (unsigned long long)v_wire0, (unsigned long long)v_wire10,
        (void*)g_sceKernelGetDirectMemorySizeReal,
        (void*)g_sceKernelAvailableDirectMemorySize,
        (void*)g_sceKernelAvailableFlexibleMemorySize,
        (void*)g_sceKernelGetCpuUsageList,
        (unsigned long long)dbg_authid, (long long)dbg_dms_r,
        (long long)dbg_pts[0], (long long)dbg_pts[1], (long long)dbg_pts[2],
        (long long)dbg_pts[3], (long long)dbg_pts[4], (long long)dbg_pts[5],
        (long long)dbg_cfx,
        (long long)dbg_dm[0], (long long)dbg_dm[1], (long long)dbg_dm[2],
        (long long)dbg_dm[3], (long long)dbg_dm[4],
        (long long)dbg_am[0], (long long)dbg_am[1], (long long)dbg_am[2],
        (long long)dbg_am[3], (long long)dbg_am[4],
        (long long)dbg_fx[0], (long long)dbg_fx[1], (long long)dbg_fx[2],
        (long long)dbg_fx[3], (long long)dbg_fx[4]);

    // Numeric sysctl MIB scan — sysctlbyname() on this kernel lacks most
    // hw.*/vm.* names, but numeric OIDs may still be wired. Scan CTL_HW (6)
    // and CTL_VM (2) subnodes and report which ones return sane data.
    off += snprintf(response + off, sizeof(response) - off, "dbg_scan=");
    for (int top = 2; top <= 6; top += 4) {          /* CTL_VM=2, CTL_HW=6 */
        for (int sub = 0; sub < 32; sub++) {
            int mib[2] = { top, sub };
            uint64_t buf[8]; size_t len = sizeof(buf);
            memset(buf, 0, sizeof(buf));
            if (sysctl(mib, 2, buf, &len, NULL, 0) == 0 && len > 0) {
                off += snprintf(response + off, sizeof(response) - off,
                    "%d.%d=%llx(%zu);", top, sub,
                    (unsigned long long)buf[0], len);
                if (off > (int)sizeof(response) - 200) break;
            }
        }
    }
    off += snprintf(response + off, sizeof(response) - off, "\n");

    send_response(session->sock, RESP_DATA, response, off);
}

// Module info structure (simplified)
typedef struct {
    char name[256];
    uint64_t base_addr;
    uint64_t size;
} simple_module_info_t;

// Handle GET_MODULE_LIST - Get list of loaded modules
void handle_get_module_list(client_session_t *session) {
    char *response = malloc(32768);
    if (!response) {
        send_error(session->sock, "Out of memory");
        return;
    }
    
    int off = 0;
    off += snprintf(response + off, 32768 - off, "modules=[\n");
    
    resolve_extendedinfo_functions();
    
    // Try to get module list
    uint32_t handles[256];
    size_t actual_count = 0;
    
    int list_rc = -3;
    if (g_sceKernelGetModuleList)
        list_rc = wdg_module_list((void *)g_sceKernelGetModuleList, handles, 256, &actual_count);
    size_t sce_count = actual_count;  // handles[] is only valid up to this
    // Payload processes only register their own eboot in the dynlib module
    // list — sceKernelGetModuleList legitimately returns count=1. To show
    // which system libraries are actually mapped, probe known basenames via
    // kernel_dynlib_handle (returns a handle only if the module is loaded).
    intptr_t dbg_mil[4] = {0};
    if (actual_count <= 1) {
        static const char *known_libs[] = {
            "libkernel_sys.sprx", "libkernel_web.sprx", "libkernel.sprx",
            "libSceLibcInternal.sprx", "libSceNet.sprx", "libSceNetCtl.sprx",
            "libSceSysmodule.sprx", "libSceUserService.sprx",
            "libSceVideoOut.sprx", "libSceGnmDriver.so",
            "libScePad.sprx", "libSceAudioOut.sprx",
            "libSceAppInstUtil.sprx", "libSceSystemService.sprx",
            "libSceHttp.sprx", "libSceSsl.sprx",
            "libSceRegMgr.sprx", "libSceFreeType.sprx",
            "libSceJson2.sprx", "libSceDbg.sprx",
            "libSceGameLiveStreaming.sprx", "libScePngDec.sprx",
            NULL
        };
        int added = 0;
        for (int k = 0; known_libs[k] && off < 30000; k++) {
            uint32_t hmod = 0;
            if (krpc_dynlib_handle(getpid(), known_libs[k], &hmod) == 0 && hmod) {
                off += snprintf(response + off, 32768 - off,
                    "  {\"id\":%u,\"name\":\"%s\",\"loaded\":1},\n", hmod, known_libs[k]);
                added++;
            }
        }
        if (added) actual_count += added;
    }
    if (list_rc == 0) {
        int timeouts = 0;
        for (size_t i = 0; i < sce_count && i < 256 && off < 30000 && timeouts < 3; i++) {
            char info_buf[512] = {0};
            int r = g_sceKernelGetModuleInfo
                ? wdg_module_info((void *)g_sceKernelGetModuleInfo, handles[i], info_buf, sizeof(info_buf))
                : -3;
            if (r == -2) { timeouts++; continue; }
            if (r == 0) {
                // SceKernelModuleInfo: size_t size @0, char name[256] @8,
                // then handle/segment descriptors. Skip all-zero entries.
                char *name = info_buf + 8;
                name[255] = '\0';
                if (name[0] != '\0') {
                    off += snprintf(response + off, 32768 - off,
                        "  {\"id\":%u,\"name\":\"%s\"},\n", handles[i], name);
                }
            }
        }
    } else {
        // Fallback: scan /system/common/lib and /system/priv/lib
        const char *lib_paths[] = {
            "/system/common/lib",
            "/system/priv/lib",
            "/system/sys/lib",
            NULL
        };
        
        for (int p = 0; lib_paths[p] && off < 30000; p++) {
            DIR *d = opendir(lib_paths[p]);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) && off < 30000) {
                    if (strstr(e->d_name, ".sprx") || strstr(e->d_name, ".so")) {
                        off += snprintf(response + off, 32768 - off,
                            "  {\"id\":0,\"name\":\"%s\",\"path\":\"%s\"},\n",
                            e->d_name, lib_paths[p]);
                    }
                }
                closedir(d);
            }
        }
    }
    
    // Remove trailing comma and close array
    if (off > 2 && response[off-2] == ',') {
        off -= 2;
        off += snprintf(response + off, 32768 - off, "\n");
    }
    off += snprintf(response + off, 32768 - off, "]\n");
    off += snprintf(response + off, 32768 - off, "count=%zu\n", actual_count);
    off += snprintf(response + off, 32768 - off, "dbg_list_rc=%d dbg_list_ptr=%p dbg_info_ptr=%p dbg_mil=%lld,%lld,%lld,%lld dbg_mil_ptr=%p\n",
                    list_rc, (void*)g_sceKernelGetModuleList, (void*)g_sceKernelGetModuleInfo,
                    (long long)dbg_mil[0], (long long)dbg_mil[1],
                    (long long)dbg_mil[2], (long long)dbg_mil[3],
                    (void*)g_get_module_info_list);
    
    send_response(session->sock, RESP_DATA, response, off);
    free(response);
}

// Handle GET_POWER_INFO - Get power/uptime and real power consumption
void handle_get_power_info(client_session_t *session) {
    char response[1024];
    int off = 0;
    
    // Get uptime via kern.boottime
    struct timeval boottime;
    size_t bt_len = sizeof(boottime);
    uint64_t uptime_sec = 0;
    int bt_rc = sysctlbyname("kern.boottime", &boottime, &bt_len, NULL, 0);
    
    if (bt_rc == 0 && boottime.tv_sec > 0) {
        struct timeval now;
        gettimeofday(&now, NULL);
        if (now.tv_sec > boottime.tv_sec) {
            uptime_sec = (uint64_t)(now.tv_sec - boottime.tv_sec);
        }
    }
    
    // Fallback: use clock() or time-based estimate
    if (uptime_sec == 0) {
        // Simple fallback using time(NULL) - epoch seconds
        // Not real uptime but at least non-zero
        uptime_sec = 0;  // Leave as 0 if we can't determine
    }
    
    uint64_t hours = uptime_sec / 3600;
    uint64_t minutes = (uptime_sec % 3600) / 60;
    
    // Use uptime only (safe) - no Sony kernel calls
    off += snprintf(response + off, sizeof(response) - off,
        "operating_time_sec=%llu\n"
        "operating_time_hours=%llu\n"
        "operating_time_minutes=%llu\n"
        "boot_count=0\n"
        "power_consumption_mw=0\n",
        (unsigned long long)uptime_sec,
        (unsigned long long)hours,
        (unsigned long long)minutes);
    
    send_response(session->sock, RESP_DATA, response, off);
}

// Handle GET_RUNNING_APPS - Get list of running applications
void handle_get_running_apps(client_session_t *session) {
    char *response = malloc(16384);
    if (!response) {
        send_error(session->sock, "Out of memory");
        return;
    }
    
    int off = 0;
    
    // Scan /mnt/sandbox for running apps
    DIR *d = opendir("/mnt/sandbox");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && off < 15000) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            
            // Extract title ID from sandbox name (format: PPSA12345_000 or CUSA12345_000)
            char title_id[16] = "";
            if ((strncmp(e->d_name, "PPSA", 4) == 0 || strncmp(e->d_name, "CUSA", 4) == 0) &&
                strlen(e->d_name) >= 9) {
                strncpy(title_id, e->d_name, 9);
                title_id[9] = '\0';
            } else {
                continue;
            }
            
            // Format: pid=X|name=Y|title_id=Z|app_id=W
            off += snprintf(response + off, 16384 - off,
                "pid=0|name=%s|title_id=%s|app_id=0\n", e->d_name, title_id);
        }
        closedir(d);
    }
    
    if (off == 0) {
        strcpy(response, "No apps running\n");
        off = strlen(response);
    }
    
    send_response(session->sock, RESP_DATA, response, off);
    free(response);
}

// Handle KILL_APP - Kill a running application
void handle_kill_app(client_session_t *session, const char *title_id) {
    if (!title_id || strlen(title_id) == 0) {
        send_error(session->sock, "No title ID provided");
        return;
    }
    
    // Placeholder - actual app killing requires SceSystemService calls
    char msg[128];
    snprintf(msg, sizeof(msg), "Kill not implemented for %s", title_id);
    send_error(session->sock, msg);
}

// Handle LAUNCH_BROWSER - Open the PS5 browser
void handle_launch_browser(client_session_t *session, const char *url) {
    // Placeholder
    send_error(session->sock, "Browser launch not implemented");
}

// ============================================================================
// SHELL TERMINAL
// ============================================================================

void handle_shell_open(client_session_t *session) {
    if (session->shell_active) {
        send_error(session->sock, "Shell already active");
        return;
    }
    
    // Initialize shell state
    session->shell_active = true;
    strcpy(session->shell_cwd, "/data");
    session->shell_pipe = NULL;
    session->shell_pid = 0;
    
    send_ok(session->sock, "Shell session opened");
}

// Built-in ls command
void builtin_ls(client_session_t *session, const char *path) {
    const char *target = (path && strlen(path) > 0) ? path : session->shell_cwd;
    
    DIR *dir = opendir(target);
    if (!dir) {
        send_error(session->sock, "Cannot open directory");
        return;
    }
    
    struct dirent *entry;
    char output[256];
    
    while ((entry = readdir(dir)) != NULL) {
        snprintf(output, sizeof(output), "%s\n", entry->d_name);
        
        uint8_t resp = RESP_DATA;
        uint32_t data_len = strlen(output);
        send(session->sock, &resp, 1, 0);
        send(session->sock, &data_len, 4, 0);
        send(session->sock, output, data_len, 0);
    }
    
    closedir(dir);
    send_ok(session->sock, "");
}

// Built-in pwd command
void builtin_pwd(client_session_t *session) {
    char output[MAX_PATH + 1];
    snprintf(output, sizeof(output), "%s\n", session->shell_cwd);
    
    uint8_t resp = RESP_DATA;
    uint32_t data_len = strlen(output);
    send(session->sock, &resp, 1, 0);
    send(session->sock, &data_len, 4, 0);
    send(session->sock, output, data_len, 0);
    
    send_ok(session->sock, "");
}

// Resolve ".." and "." components in a path in-place
static void resolve_path(char *path) {
    char *parts[128];
    int depth = 0;
    char tmp[MAX_PATH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    
    char *token = strtok(tmp, "/");
    while (token) {
        if (strcmp(token, "..") == 0) {
            if (depth > 0) depth--;
        } else if (strcmp(token, ".") != 0 && strlen(token) > 0) {
            parts[depth++] = token;
        }
        token = strtok(NULL, "/");
    }
    
    // Rebuild path
    path[0] = '\0';
    for (int i = 0; i < depth; i++) {
        strcat(path, "/");
        strcat(path, parts[i]);
    }
    if (path[0] == '\0') {
        strcpy(path, "/");
    }
}

// Built-in cd command
void builtin_cd(client_session_t *session, const char *path) {
    char new_path[MAX_PATH];
    
    if (!path || strlen(path) == 0 || strcmp(path, "~") == 0) {
        strcpy(new_path, "/data");
    } else if (path[0] == '/') {
        strncpy(new_path, path, sizeof(new_path) - 1);
        new_path[sizeof(new_path) - 1] = '\0';
    } else {
        snprintf(new_path, sizeof(new_path), "%s/%s", session->shell_cwd, path);
    }
    
    // Resolve ".." and "." components
    resolve_path(new_path);
    
    // Check if directory exists
    DIR *dir = opendir(new_path);
    if (!dir) {
        send_error(session->sock, "Directory not found");
        return;
    }
    closedir(dir);
    
    // Update current directory (safe copy with bounds check)
    strncpy(session->shell_cwd, new_path, sizeof(session->shell_cwd) - 1);
    session->shell_cwd[sizeof(session->shell_cwd) - 1] = '\0';
    send_ok(session->sock, "");
}

// Built-in cat command
void builtin_cat(client_session_t *session, const char *path) {
    if (!path || strlen(path) == 0) {
        send_error(session->sock, "Usage: cat <file>");
        return;
    }
    
    char full_path[MAX_PATH];
    if (path[0] == '/') {
        strcpy(full_path, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", session->shell_cwd, path);
    }
    
    FILE *fp = fopen(full_path, "r");
    if (!fp) {
        send_error(session->sock, "Cannot open file");
        return;
    }
    
    char buffer[4096];
    size_t total_sent = 0;
    
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        size_t len = strlen(buffer);
        if (len > 0) {
            uint8_t resp = RESP_DATA;
            uint32_t data_len = len;
            send(session->sock, &resp, 1, 0);
            send(session->sock, &data_len, 4, 0);
            send(session->sock, buffer, data_len, 0);
            
            total_sent += len;
            if (total_sent > 1024 * 1024) break; // Max 1MB
        }
    }
    
    fclose(fp);
    send_ok(session->sock, "");
}

// Built-in mkdir command
void builtin_mkdir(client_session_t *session, const char *path) {
    if (!path || strlen(path) == 0) {
        send_error(session->sock, "Usage: mkdir <directory>");
        return;
    }
    
    char full_path[MAX_PATH];
    if (path[0] == '/') {
        strcpy(full_path, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", session->shell_cwd, path);
    }
    
    if (mkdir(full_path, 0777) == 0) {
        send_ok(session->sock, "Directory created");
    } else {
        send_error(session->sock, "Failed to create directory");
    }
}

// Built-in rm command
void builtin_rm(client_session_t *session, const char *path) {
    if (!path || strlen(path) == 0) {
        send_error(session->sock, "Usage: rm <file>");
        return;
    }
    
    char full_path[MAX_PATH];
    if (path[0] == '/') {
        strcpy(full_path, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", session->shell_cwd, path);
    }
    
    if (unlink(full_path) == 0) {
        send_ok(session->sock, "File deleted");
    } else {
        send_error(session->sock, "Failed to delete file");
    }
}

// Built-in rmdir command
void builtin_rmdir(client_session_t *session, const char *path) {
    if (!path || strlen(path) == 0) {
        send_error(session->sock, "Usage: rmdir <directory>");
        return;
    }
    
    char full_path[MAX_PATH];
    if (path[0] == '/') {
        strcpy(full_path, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", session->shell_cwd, path);
    }
    
    if (rmdir(full_path) == 0) {
        send_ok(session->sock, "Directory deleted");
    } else {
        send_error(session->sock, "Failed to delete directory");
    }
}

// Built-in touch command
void builtin_touch(client_session_t *session, const char *path) {
    if (!path || strlen(path) == 0) {
        send_error(session->sock, "Usage: touch <file>");
        return;
    }
    
    char full_path[MAX_PATH];
    if (path[0] == '/') {
        strcpy(full_path, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", session->shell_cwd, path);
    }
    
    FILE *fp = fopen(full_path, "a");
    if (fp) {
        fclose(fp);
        send_ok(session->sock, "File created/updated");
    } else {
        send_error(session->sock, "Failed to create file");
    }
}

// Built-in echo command
void builtin_echo(client_session_t *session, const char *text) {
    if (!text) text = "";
    
    char output[MAX_PATH + 2];
    snprintf(output, sizeof(output), "%s\n", text);
    
    uint8_t resp = RESP_DATA;
    uint32_t data_len = strlen(output);
    send(session->sock, &resp, 1, 0);
    send(session->sock, &data_len, 4, 0);
    send(session->sock, output, data_len, 0);
    send_ok(session->sock, "");
}

// Built-in cp command
void builtin_cp(client_session_t *session, const char *args) {
    if (!args || strlen(args) == 0) {
        send_error(session->sock, "Usage: cp <source> <destination>");
        return;
    }
    
    char args_copy[MAX_PATH * 2];
    strncpy(args_copy, args, sizeof(args_copy) - 1);
    
    char *src = strtok(args_copy, " \t");
    char *dst = strtok(NULL, " \t\n");
    
    if (!src || !dst) {
        send_error(session->sock, "Usage: cp <source> <destination>");
        return;
    }
    
    char src_path[MAX_PATH], dst_path[MAX_PATH];
    if (src[0] == '/') strcpy(src_path, src);
    else snprintf(src_path, sizeof(src_path), "%s/%s", session->shell_cwd, src);
    
    if (dst[0] == '/') strcpy(dst_path, dst);
    else snprintf(dst_path, sizeof(dst_path), "%s/%s", session->shell_cwd, dst);
    
    FILE *src_fp = fopen(src_path, "rb");
    if (!src_fp) {
        send_error(session->sock, "Cannot open source file");
        return;
    }
    
    FILE *dst_fp = fopen(dst_path, "wb");
    if (!dst_fp) {
        fclose(src_fp);
        send_error(session->sock, "Cannot create destination file");
        return;
    }
    
    char buffer[8192];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src_fp)) > 0) {
        fwrite(buffer, 1, bytes, dst_fp);
    }
    
    fclose(src_fp);
    fclose(dst_fp);
    send_ok(session->sock, "File copied");
}

// Built-in mv command
void builtin_mv(client_session_t *session, const char *args) {
    if (!args || strlen(args) == 0) {
        send_error(session->sock, "Usage: mv <source> <destination>");
        return;
    }
    
    char args_copy[MAX_PATH * 2];
    strncpy(args_copy, args, sizeof(args_copy) - 1);
    
    char *src = strtok(args_copy, " \t");
    char *dst = strtok(NULL, " \t\n");
    
    if (!src || !dst) {
        send_error(session->sock, "Usage: mv <source> <destination>");
        return;
    }
    
    char src_path[MAX_PATH], dst_path[MAX_PATH];
    if (src[0] == '/') strcpy(src_path, src);
    else snprintf(src_path, sizeof(src_path), "%s/%s", session->shell_cwd, src);
    
    if (dst[0] == '/') strcpy(dst_path, dst);
    else snprintf(dst_path, sizeof(dst_path), "%s/%s", session->shell_cwd, dst);
    
    if (rename(src_path, dst_path) == 0) {
        send_ok(session->sock, "File moved/renamed");
    } else {
        send_error(session->sock, "Failed to move file");
    }
}

// Built-in stat command
void builtin_stat(client_session_t *session, const char *path) {
    if (!path || strlen(path) == 0) {
        send_error(session->sock, "Usage: stat <file>");
        return;
    }
    
    char full_path[MAX_PATH];
    if (path[0] == '/') {
        strcpy(full_path, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", session->shell_cwd, path);
    }
    
    struct stat st;
    if (stat(full_path, &st) != 0) {
        send_error(session->sock, "Cannot stat file");
        return;
    }
    
    char output[512];
    snprintf(output, sizeof(output),
        "File: %s\n"
        "Size: %lld bytes\n"
        "Type: %s\n"
        "Permissions: %o\n",
        path,
        (long long)st.st_size,
        S_ISDIR(st.st_mode) ? "Directory" : S_ISREG(st.st_mode) ? "Regular file" : "Other",
        st.st_mode & 0777);
    
    uint8_t resp = RESP_DATA;
    uint32_t data_len = strlen(output);
    send(session->sock, &resp, 1, 0);
    send(session->sock, &data_len, 4, 0);
    send(session->sock, output, data_len, 0);
    send_ok(session->sock, "");
}

// Built-in chmod command
void builtin_chmod(client_session_t *session, const char *args) {
    if (!args || strlen(args) == 0) {
        send_error(session->sock, "Usage: chmod <mode> <file>");
        return;
    }
    
    char args_copy[MAX_PATH];
    strncpy(args_copy, args, sizeof(args_copy) - 1);
    
    char *mode_str = strtok(args_copy, " \t");
    char *path = strtok(NULL, " \t\n");
    
    if (!mode_str || !path) {
        send_error(session->sock, "Usage: chmod <mode> <file>");
        return;
    }
    
    int mode = strtol(mode_str, NULL, 8);
    
    char full_path[MAX_PATH];
    if (path[0] == '/') {
        strcpy(full_path, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", session->shell_cwd, path);
    }
    
    if (chmod(full_path, mode) == 0) {
        send_ok(session->sock, "Permissions changed");
    } else {
        send_error(session->sock, "Failed to change permissions");
    }
}

// Handle SHELL_EXEC - Execute command and stream output
void handle_shell_exec(client_session_t *session, const char *command) {
    if (!session->shell_active) {
        send_error(session->sock, "Shell not active");
        return;
    }
    
    if (!command || strlen(command) == 0) {
        send_error(session->sock, "Empty command");
        return;
    }
    
    // Parse command and arguments
    char cmd_copy[MAX_PATH];
    strncpy(cmd_copy, command, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';
    
    char *cmd = strtok(cmd_copy, " \t\n");
    char *arg = strtok(NULL, "\n");
    
    if (!cmd) {
        send_error(session->sock, "Empty command");
        return;
    }
    
    // Handle built-in commands
    if (strcmp(cmd, "ls") == 0) {
        builtin_ls(session, arg);
    } else if (strcmp(cmd, "pwd") == 0) {
        builtin_pwd(session);
    } else if (strcmp(cmd, "cd") == 0) {
        builtin_cd(session, arg);
    } else if (strcmp(cmd, "cat") == 0) {
        builtin_cat(session, arg);
    } else if (strcmp(cmd, "mkdir") == 0) {
        builtin_mkdir(session, arg);
    } else if (strcmp(cmd, "rm") == 0) {
        builtin_rm(session, arg);
    } else if (strcmp(cmd, "rmdir") == 0) {
        builtin_rmdir(session, arg);
    } else if (strcmp(cmd, "touch") == 0) {
        builtin_touch(session, arg);
    } else if (strcmp(cmd, "echo") == 0) {
        builtin_echo(session, arg);
    } else if (strcmp(cmd, "cp") == 0) {
        builtin_cp(session, arg);
    } else if (strcmp(cmd, "mv") == 0) {
        builtin_mv(session, arg);
    } else if (strcmp(cmd, "stat") == 0) {
        builtin_stat(session, arg);
    } else if (strcmp(cmd, "chmod") == 0) {
        builtin_chmod(session, arg);
    } else if (strcmp(cmd, "help") == 0) {
        const char *help_text = 
            "PS5 Shell Terminal - Available Commands:\n"
            "\n"
            "FILE OPERATIONS:\n"
            "  ls [path]         - List directory contents\n"
            "  cat <file>        - Display file contents\n"
            "  touch <file>      - Create empty file\n"
            "  rm <file>         - Delete file\n"
            "  cp <src> <dst>    - Copy file\n"
            "  mv <src> <dst>    - Move/rename file\n"
            "  stat <file>       - Show file information\n"
            "  chmod <mode> <f>  - Change file permissions\n"
            "\n"
            "DIRECTORY OPERATIONS:\n"
            "  pwd               - Print working directory\n"
            "  cd [path]         - Change directory\n"
            "  mkdir <dir>       - Create directory\n"
            "  rmdir <dir>       - Delete empty directory\n"
            "\n"
            "UTILITIES:\n"
            "  echo <text>       - Print text\n"
            "  help              - Show this help\n"
            "\n"
            "TIPS:\n"
            "  - Use absolute paths (/data/file) or relative (file)\n"
            "  - Press UP/DOWN arrows for command history\n"
            "  - Type 'cd' or 'cd ~' to go to /data\n";
        
        uint8_t resp = RESP_DATA;
        uint32_t data_len = strlen(help_text);
        send(session->sock, &resp, 1, 0);
        send(session->sock, &data_len, 4, 0);
        send(session->sock, help_text, data_len, 0);
        send_ok(session->sock, "");
    } else {
        send_error(session->sock, "Command not found. Type 'help' for available commands.");
    }
}

// Handle SHELL_INTERRUPT - Not implemented (would need fork/exec for proper signal handling)
void handle_shell_interrupt(client_session_t *session) {
    send_error(session->sock, "Interrupt not supported in this implementation");
}

// Handle SHELL_CLOSE - Close shell session
void handle_shell_close(client_session_t *session) {
    if (!session->shell_active) {
        send_error(session->sock, "Shell not active");
        return;
    }
    
    session->shell_active = false;
    session->shell_pipe = NULL;
    session->shell_pid = 0;
    
    send_ok(session->sock, "Shell session closed");
}

// Handle client
void *client_thread(void *arg) {
    client_session_t *session = (client_session_t *)arg;
    // +1: the buffer is reused across commands and handlers treat `data` as a
    // C string — without a terminator, strlen() reads a stale byte from the
    // previous command (visible as a garbage char appended to notifications).
    uint8_t *buffer = malloc(BUFFER_SIZE + 1);
    
    if (!buffer) {
        close(session->sock);
        free(session);
        __sync_fetch_and_sub(&g_active_sessions, 1);
        return NULL;
    }
    
    // Initialize upload_fd to -1 (not open)
    session->upload_fd = -1;
    session->upload_aborted = false;
    
    // No socket timeout - connection stays open indefinitely until client disconnects
    
    while (1) {
        // Read command header (5 bytes: 1 cmd + 4 data_len)
        uint8_t header[5];
        ssize_t n = recv(session->sock, header, 5, MSG_WAITALL);
        if (n != 5) {
            break;
        }
        
        uint8_t cmd = header[0];
        uint32_t data_len;
        memcpy(&data_len, header + 1, 4);
        
        // Read data if present
        uint8_t *data = NULL;
        if (data_len > 0) {
            if (data_len > BUFFER_SIZE) {
                send_error(session->sock, "Data too large");
                break;
            }
            data = buffer;
            ssize_t received = 0;
            while (received < data_len) {
                n = recv(session->sock, data + received, data_len - received, 0);
                if (n <= 0) {
                    break;
                }
                received += n;
            }
            if (received != data_len) {
                break;
            }
            data[data_len] = 0;
        }
        
        // Handle command
        switch (cmd) {
            case CMD_PING:
                handle_ping(session);
                break;
            case CMD_LIST_STORAGE:
                handle_list_storage(session);
                break;
            case CMD_LIST_DIR:
                if (data) {
                    handle_list_dir(session, (const char *)data);
                }
                break;
            case CMD_CREATE_DIR:
                if (data) {
                    handle_create_dir(session, (const char *)data);
                }
                break;
            case CMD_DELETE_FILE:
                if (data) {
                    handle_delete_file(session, (const char *)data);
                }
                break;
            case CMD_DELETE_DIR:
                if (data) {
                    handle_delete_dir(session, (const char *)data);
                }
                break;
            case CMD_RENAME:
                if (data) {
                    handle_rename(session, data, data_len);
                }
                break;
            case CMD_COPY_FILE:
                if (data) {
                    handle_copy_file(session, data, data_len);
                }
                break;
            case CMD_MOVE_FILE:
                if (data) {
                    handle_move_file(session, data, data_len);
                }
                break;
            case CMD_START_UPLOAD:
                if (data) {
                    handle_start_upload(session, data, data_len);
                }
                break;
            case CMD_UPLOAD_CHUNK:
                if (data) {
                    handle_upload_chunk(session, data, data_len);
                }
                break;
            case CMD_END_UPLOAD:
                handle_end_upload(session);
                break;
            case CMD_DOWNLOAD_FILE:
                if (data) {
                    handle_download_file(session, (const char *)data);
                }
                break;
            case CMD_GET_FILE_INFO:
                if (data) {
                    handle_get_file_info(session, (const char *)data);
                }
                break;
            case CMD_GET_SYSTEM_INFO:
                handle_get_system_info(session);
                break;
            case CMD_VERIFY_FILE:
                if (data) {
                    handle_verify_file(session, (const char *)data);
                }
                break;
            case CMD_MOUNT_GAMES:
                handle_mount_games(session);
                break;
            case CMD_MOUNT_GAME:
                if (data) handle_mount_game(session, (const char *)data);
                else send_error(session->sock, "Usage: title_id");
                break;
            case CMD_GET_HW_INFO:
                handle_get_hw_info(session);
                break;
            case CMD_GET_TEMPS:
                handle_get_temps(session);
                break;
            case CMD_GET_GAME_LIST:
                handle_get_game_list(session);
                break;
            case CMD_UNMOUNT_GAME:
                if (data) {
                    handle_unmount_game(session, (const char *)data);
                }
                break;
            case CMD_GET_GAME_ICON:
                if (data) {
                    handle_get_game_icon(session, (const char *)data);
                } else {
                    send_error(session->sock, "No title ID provided");
                }
                break;
            case CMD_GET_GAME_DETAILS:
                if (data) {
                    handle_get_game_details(session, (const char *)data);
                } else {
                    send_error(session->sock, "No title ID provided");
                }
                break;
            case CMD_GET_GAME_PIC:
                if (data) {
                    handle_get_game_pic(session, (const char *)data);
                } else {
                    send_error(session->sock, "No request data provided");
                }
                break;
            case CMD_LIST_SAVES:
                handle_list_saves(session);
                break;
            case CMD_SAVE_SCAN:
                handle_save_scan(session);
                break;
            case CMD_SAVE_MOUNT:
                if (data) {
                    handle_save_mount(session, (const char *)data);
                } else {
                    send_error(session->sock, "No save path provided");
                }
                break;
            case CMD_SAVE_UNMOUNT:
                handle_save_unmount(session);
                break;
            case CMD_SAVE_MOUNT_STATUS:
                handle_save_mount_status(session);
                break;
            case CMD_SAVE_DELETE:
                if (data) {
                    handle_save_delete(session, (const char *)data);
                } else {
                    send_error(session->sock, "No save path provided");
                }
                break;
            case CMD_PROC_LIST:
                handle_proc_list(session);
                break;
            case CMD_RESTART_UI:
                handle_restart_ui(session);
                break;
            case CMD_SELF_UPDATE:
                if (data) {
                    handle_self_update(session, (const char *)data);
                } else {
                    send_error(session->sock, "No ELF path or URL provided");
                }
                break;
            case CMD_MEM_READ:
                if (data) handle_mem_read(session, (const char *)data);
                else send_error(session->sock, "Usage: pid|addr|len");
                break;
            case CMD_MEM_WRITE:
                if (data) handle_mem_write(session, (const char *)data);
                else send_error(session->sock, "Usage: pid|addr|hexdata");
                break;
            case CMD_MEM_REGIONS:
                if (data) handle_mem_regions(session, (const char *)data);
                else send_error(session->sock, "Usage: pid");
                break;
            case CMD_MEM_SEARCH:
                if (data) handle_mem_search(session, (const char *)data);
                else send_error(session->sock, "Usage: pid|start|end|hexpattern");
                break;
            case CMD_KLOG_READ:
                handle_klog_read(session, data ? (const char *)data : "");
                break;
            case CMD_APP_LIST_V2:
                handle_app_list_v2(session);
                break;
            case CMD_APP_SUSPEND:
                if (data) handle_app_suspend(session, (const char *)data);
                else send_error(session->sock, "Usage: app_id");
                break;
            case CMD_APP_RESUME:
                if (data) handle_app_resume(session, (const char *)data);
                else send_error(session->sock, "Usage: app_id");
                break;
            case CMD_APP_KILL:
                if (data) handle_app_kill(session, (const char *)data);
                else send_error(session->sock, "Usage: app_id");
                break;
            case CMD_APP_COREDUMP:
                if (data) handle_app_coredump(session, (const char *)data);
                else send_error(session->sock, "Usage: app_id");
                break;
            case CMD_NET_INFO:
                handle_net_info(session);
                break;
            case CMD_NET_SPEEDTEST:
                handle_net_speedtest(session);
                break;
            case CMD_POWER_ACTION:
                if (data) handle_power_action(session, (const char *)data);
                else send_error(session->sock, "Usage: reboot|shutdown");
                break;
            case CMD_USB_LIST:
                handle_usb_list(session);
                break;
            case CMD_PAD_INFO:
                handle_pad_info(session);
                break;
            case CMD_DISC_DUMP:
                if (data) handle_disc_dump(session, (const char *)data);
                else send_error(session->sock, "Usage: start|status|cancel");
                break;
            case CMD_SCREENSHOT:
                handle_screenshot(session);
                break;
            case CMD_NOTIFY:
                if (data) handle_notify(session, (const char *)data);
                else send_error(session->sock, "No message provided");
                break;
            case CMD_PAD_ACTION:
                if (data) handle_pad_action(session, (const char *)data);
                else send_error(session->sock, "Usage: lightbar|r,g,b | vibrate|l,s");
                break;
            case CMD_ICC_CONTROL:
                if (data) handle_icc_control(session, (const char *)data);
                else send_error(session->sock, "Usage: led|n buzzer|n buzzervol|n buzzermute|n ledcolor|b,w,o");
                break;
            case CMD_LAUNCH_GAME:
                if (data) {
                    handle_launch_game(session, (const char *)data);
                } else {
                    send_error(session->sock, "No title ID provided");
                }
                break;
            case CMD_LIST_SCREENSHOTS:
                handle_list_screenshots(session);
                break;
            case CMD_DELETE_SCREENSHOT:
                if (data) {
                    handle_delete_screenshot(session, (const char *)data);
                } else {
                    send_error(session->sock, "No path provided");
                }
                break;
            case CMD_GET_EXTENDED_INFO:
                handle_get_extended_info(session);
                break;
            case CMD_GET_CPU_USAGE:
                handle_get_cpu_usage(session);
                break;
            case CMD_GET_MEMORY_INFO:
                handle_get_memory_info(session);
                break;
            case CMD_GET_MODULE_LIST:
                handle_get_module_list(session);
                break;
            case CMD_GET_POWER_INFO:
                handle_get_power_info(session);
                break;
            case CMD_GET_RUNNING_APPS:
                handle_get_running_apps(session);
                break;
            case CMD_KILL_APP:
                if (data) {
                    handle_kill_app(session, (const char *)data);
                }
                break;
            case CMD_LAUNCH_BROWSER:
                if (data) {
                    handle_launch_browser(session, (const char *)data);
                }
                break;
            case CMD_SHELL_OPEN:
                handle_shell_open(session);
                break;
            case CMD_SHELL_EXEC:
                if (data) {
                    handle_shell_exec(session, (const char *)data);
                }
                break;
            case CMD_SHELL_INTERRUPT:
                handle_shell_interrupt(session);
                break;
            case CMD_SHELL_CLOSE:
                handle_shell_close(session);
                break;
            case CMD_INDEX_START:
                if (data) {
                    handle_index_start(session, (const char *)data);
                }
                break;
            case CMD_INDEX_STATUS:
                handle_index_status(session);
                break;
            case CMD_SEARCH_INDEX:
                if (data) {
                    handle_search_index(session, (const char *)data);
                }
                break;
            case CMD_INDEX_CANCEL:
                send_error(session->sock, "Index cancel not implemented yet");
                break;
            
            // ============================================================
            // New features (ps5upload-style)
            // ============================================================
            case CMD_PKG_INSTALL:
                if (data) {
                    handle_pkg_install(session->sock, (const char *)data);
                } else {
                    send_error(session->sock, "PKG path required");
                }
                break;
            case CMD_PKG_INSTALL_STATUS:
                handle_pkg_install_status(session->sock);
                break;
            case CMD_FAN_GET_THRESHOLD:
                handle_fan_get_threshold(session->sock);
                break;
            case CMD_FAN_SET_THRESHOLD:
                if (data && data_len >= 1) {
                    int temp = (int)data[0];
                    handle_fan_set_threshold(session->sock, temp);
                } else {
                    send_error(session->sock, "Temperature value required");
                }
                break;
            
            case CMD_SHUTDOWN:
                // SECURITY: only the console itself (localhost) may kill the
                // server. Reject everyone else - they just get an error.
                if (!is_local_connection(session->sock)) {
                    send_error(session->sock, "SHUTDOWN only allowed from localhost");
                    break;
                }
                send_ok(session->sock, "Shutting down");
                payload_restore_privileges();  // clean restore of jail/root dirs
                free(buffer);
                close(session->sock);
                if (session->upload_fd >= 0) {
                    close(session->upload_fd);
                    if (session->file_mutex) {
                        release_file_mutex(session->upload_path);
                    }
                }
                free(session);
                exit(0);
            default:
                send_error(session->sock, "Unknown command");
                break;
        }
    }
    
    free(buffer);
    // A failed chunk write hard-closes the socket (upload_aborted) and marks
    // ownership of the shared progress socket gone - clean it up here.
    if (session->sock >= 0) {
        progress_socket_clear(session->sock);
        close(session->sock);
    }
    if (session->upload_fd >= 0) {
        close(session->upload_fd);
        if (session->file_mutex) {
            release_file_mutex(session->upload_path);
        }
    }
    free(session);
    __sync_fetch_and_sub(&g_active_sessions, 1);
    return NULL;
}

// ============================================================================
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
// Note: <ps5/kernel.h> already included at top of file

static intptr_t g_orig_jaildir = 0;
static intptr_t g_orig_rootdir = 0;
static int      g_elevated     = 0;

// Kill whatever process currently owns the port-9113 listener. A wedged copy
// of this payload (frozen inside kernel IPC) can never answer the SHUTDOWN
// takeover command, so every reload silently died on bind() — the "payload
// doesn't work" bug.
//
// kern.proc.filedesc is blocked in payload context, and scanning kernel
// memory for socket structs is UNSAFE (a dangling kernel pointer inside a
// struct can panic the box). Instead we brute-force kernel_overlap_sockets()
// across every fd of each stale payload.elf process: overlapping our
// throwaway socket into the victim's fd slot drops a reference on the
// original file -> the listener closes -> the port frees even if the process
// can never exit. Worst case on a non-socket fd is an fdrop of a file the
// process was about to lose anyway. No kernel memory reads at all.

static void kill_stale_instances(void) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) return;
    size += size / 8;   /* processes come and go between the two calls */
    char *buf = malloc(size);
    if (!buf) return;

    pid_t self = getpid();

    if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
        for (char *p = buf; p + sizeof(int) <= buf + size; ) {
            struct kinfo_proc *ki = (struct kinfo_proc *)p;
            if (ki->ki_structsize <= 0) break;
            p += ki->ki_structsize;
            if (ki->ki_pid == self) continue;
            // Stale copies of this server always run as "payload.elf" under
            // elfldr — scanning every process would be slow and pointless.
            if (strncmp(ki->ki_comm, "payload", 7) != 0) continue;
            for (int fd = 0; fd < 64; fd++) {
                int tmp = socket(AF_INET, SOCK_STREAM, 0);
                if (tmp < 0) break;
                kernel_overlap_sockets(ki->ki_pid, tmp, fd);
                close(tmp);
            }
            kill(ki->ki_pid, SIGKILL);
        }
    }
    free(buf);
}

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

    // ShellCore authid — THE key unlock: Sce service IPC (AppInstUtil,
    // LncUtil, UserService) is only answered for ShellCore-level callers.
    // With any other authid those calls block forever inside the kernel
    // (the "payload wedged" bug). Same value EchoStretch's ps5-app-dumper
    // uses for launches.
    kernel_set_ucred_authid(me, 0x4801000000000013ULL);

    g_elevated = 1;
}

static void payload_restore_privileges(void) {
    if (!g_elevated) return;
    pid_t me = getpid();
    if (g_orig_rootdir) kernel_set_proc_rootdir(me, g_orig_rootdir);
    if (g_orig_jaildir) kernel_set_proc_jaildir(me, g_orig_jaildir);
    g_elevated = 0;
}

// UDP auto-discovery responder. The client broadcasts "PS5SUITE_DISCOVER" to
// the subnet broadcast address; we answer "PS5SUITE|<bound_port>|v7.0.0" from
// our real IP so the client can connect without typing an address.
// The discovery responder binds its UDP socket to the SAME port number as our
// TCP listener (g_bound_port). UDP and TCP port spaces are independent, and
// each launch gets a fresh TCP port via the takeover/fallback logic, so this
// UDP port is fresh too — robust to leftover/zombie instances that may still
// hold a fixed discovery port.
static void *discovery_thread(void *arg) {
    (void)arg;
    FILE *dl = fopen("/data/ps5_suite_discovery.log", "w");
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        if (dl) { fprintf(dl, "socket failed errno=%d\n", errno); fclose(dl); }
        return NULL;
    }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(g_bound_port);
    if (bind(sock, (struct sockaddr*)&a, sizeof(a)) < 0) {
        if (dl) { fprintf(dl, "bind failed errno=%d port=%d\n", errno, g_bound_port); fclose(dl); }
        close(sock);
        return NULL;
    }
    if (dl) { fprintf(dl, "listening on %d\n", g_bound_port); fflush(dl); }

    char buf[256];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr*)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        if (dl) { fprintf(dl, "rx %zd bytes: %.20s\n", n, buf); fflush(dl); }
        if (strncmp(buf, "PS5SUITE_DISCOVER", 17) != 0) continue;
        char resp[96];
        int rl = snprintf(resp, sizeof(resp), "PS5SUITE|%d|v" SUITE_VERSION, g_bound_port);
        sendto(sock, resp, rl, 0, (struct sockaddr*)&from, fl);
    }
    return NULL;
}

int main() {
    // Elevate BEFORE anything else (elfldr does the same before spawning).
    // NOTE: before this the process is JAILED — /data is not visible, so the
    // boot log can only be opened after elevation.
    payload_self_elevate();

    // Boot log — survives crashes, readable via FTP afterwards
    FILE *blog = fopen("/data/ps5_suite_boot.log", "w");
    if (blog) { fputs("main: elevated ok\n", blog); fflush(blog); }

    // Self-update handoff: the outgoing instance leaves UPDATE_MARKER when it
    // pushes a new ELF to the loader. Finding a fresh marker means WE are the
    // update — sweep every stale payload process NOW (not only on bind
    // failure) so an old instance parked on a fallback port dies too.
    char update_prev[32] = "";
    {
        FILE *mf = fopen(UPDATE_MARKER, "r");
        if (mf) {
            char line[128];
            time_t ts = 0;
            while (fgets(line, sizeof(line), mf)) {
                if (!strncmp(line, "ts=", 3)) ts = (time_t)atoll(line + 3);
                else if (!strncmp(line, "prev=", 5)) sscanf(line + 5, "%31s", update_prev);
            }
            fclose(mf);
            unlink(UPDATE_MARKER);
            if (ts > 0 && time(NULL) - ts <= 120) {
                if (blog) { fputs("main: update marker — sweeping stale instances\n", blog); fflush(blog); }
                kill_stale_instances();
            } else {
                update_prev[0] = '\0';   // stale marker — normal boot
            }
        }
    }

    // Initialize worker threads for async disk I/O
    init_workers();
    if (blog) { fputs("main: workers ok\n", blog); fflush(blog); }
    
    // Initialize index system
    pthread_mutex_init(&g_index.mutex, NULL);
    g_index.entries = NULL;
    g_index.total_files = 0;
    g_index.total_dirs = 0;
    g_index.indexing = false;
    g_index.ready = false;
    
    int server_sock;
    struct sockaddr_in server_addr;
    
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Prevent SIGPIPE
    int no_sigpipe = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
    
    // 16MB buffers for maximum throughput
    int buf_size = 16 * 1024 * 1024;
    setsockopt(server_sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(server_sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        if (blog) { fputs("main: bind failed, killing stale\n", blog); fflush(blog); }
        // Port taken — a stale/wedged copy still owns it. Kill it by fd-owner
        // scan + socket overlap (a frozen process can't answer SHUTDOWN).
        kill_stale_instances();
        if (blog) { fputs("main: stale killed, retry bind\n", blog); fflush(blog); }
        {
            int retry2 = socket(AF_INET, SOCK_STREAM, 0);
            setsockopt(retry2, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            setsockopt(retry2, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
            if (bind(retry2, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
                close(server_sock);
                server_sock = retry2;
                goto bound_ok;
            }
            close(retry2);
        }
        // Port taken = an older copy of this server is still running (payloads
        // have no exit path of their own). The SHUTDOWN command is allowed from
        // localhost — so we send it to the old instance, wait for it to release
        // the port, and take over. Without this, every re-sent payload died
        // silently here (no notification, no server) and the OLD code kept
        // serving — that is exactly the "nothing works after payload load" bug.
        int kill_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (kill_sock >= 0) {
            struct sockaddr_in lo;
            memset(&lo, 0, sizeof(lo));
            lo.sin_family = AF_INET;
            lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            lo.sin_port = htons(SERVER_PORT);
            // The wedged listener can't answer: a blocking connect() stalls
            // ~75s on a full SYN backlog and a blocking recv() hangs forever.
            // Use non-blocking connect + poll, and a bounded recv.
            int fl = fcntl(kill_sock, F_GETFL, 0);
            fcntl(kill_sock, F_SETFL, fl | O_NONBLOCK);
            int conn_ok = 0;
            if (connect(kill_sock, (struct sockaddr*)&lo, sizeof(lo)) == 0) {
                conn_ok = 1;
            } else if (errno == EINPROGRESS) {
                struct pollfd pfd = { kill_sock, POLLOUT, 0 };
                if (poll(&pfd, 1, 400) > 0) {
                    int soerr = 0; socklen_t sl = sizeof(soerr);
                    getsockopt(kill_sock, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                    conn_ok = (soerr == 0);
                }
            }
            if (conn_ok) {
                uint8_t hdr[5] = { CMD_SHUTDOWN, 0, 0, 0, 0 };
                send(kill_sock, hdr, 5, 0);
                uint8_t resp[5];
                struct pollfd rpfd = { kill_sock, POLLIN, 0 };
                if (poll(&rpfd, 1, 300) > 0)
                    (void)recv(kill_sock, resp, sizeof(resp), 0);
                close(kill_sock);
                struct timespec ts200 = { 0, 200 * 1000 * 1000 };
                for (int i = 0; i < 10; i++) {   // up to 2s
                    nanosleep(&ts200, NULL);
                    int retry = socket(AF_INET, SOCK_STREAM, 0);
                    if (retry < 0) break;
                    setsockopt(retry, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                    int rc = bind(retry, (struct sockaddr*)&server_addr, sizeof(server_addr));
                    close(retry);
                    if (rc == 0) break;
                }
            } else {
                close(kill_sock);
            }
        }
        // Retry our own bind — the port should be free now.
        server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock < 0) return 1;
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(server_sock, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
        setsockopt(server_sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
        setsockopt(server_sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
        if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            // The stale process is wedged in uninterruptible kernel IPC —
            // SIGKILL is pending but can never land, so its listener keeps
            // the port forever. Instead of dying (and forcing a PS5 reboot),
            // fall back to a secondary port. The client scans 9113+ range.
            int bound_port = 0;
            for (int p = SERVER_PORT + 1; p <= SERVER_PORT + 5 && bound_port == 0; p++) {
                close(server_sock);
                server_sock = socket(AF_INET, SOCK_STREAM, 0);
                if (server_sock < 0) break;
                setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                setsockopt(server_sock, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
                server_addr.sin_port = htons(p);
                if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0)
                    bound_port = p;
            }
            if (bound_port == 0) {
                if (blog) { fputs("main: bind FAILED on all ports\n", blog); fclose(blog); }
                send_notification("PS5 Suite: no free port — reboot PS5");
                close(server_sock);
                return 1;
            }
            g_bound_port = bound_port;
            if (blog) { fprintf(blog, "main: fallback bound port %d\n", bound_port); fflush(blog); }
            FILE *pf = fopen("/data/ps5_suite_port.txt", "w");
            if (pf) { fprintf(pf, "%d\n", bound_port); fclose(pf); }
            goto bound_ok;
        }
        if (blog) { fputs("main: bind ok after takeover\n", blog); fflush(blog); }
    }

bound_ok:
    // Record the bound port — diagnostic for FTP inspection (the client
    // discovers the port by scanning 9113..9118, it can't read this file).
    {
        FILE *pf = fopen("/data/ps5_suite_port.txt", "w");
        if (pf) { fprintf(pf, "%d\n", g_bound_port); fclose(pf); }
    }
    // Increase backlog to handle multiple parallel connections (up to 128)
    if (listen(server_sock, 128) < 0) {
        if (blog) { fputs("main: listen FAILED\n", blog); fclose(blog); }
        close(server_sock);
        return 1;
    }
    if (blog) { fputs("main: LISTENING ok\n", blog); fclose(blog); }

    // UDP discovery responder — client broadcasts "PS5SUITE_DISCOVER" and we
    // reply "PS5SUITE|<bound_port>" so it learns IP+port automatically.
    {
        pthread_t dt;
        pthread_attr_t dat;
        pthread_attr_init(&dat);
        pthread_attr_setdetachstate(&dat, PTHREAD_CREATE_DETACHED);
        pthread_create(&dt, &dat, discovery_thread, NULL);
        pthread_attr_destroy(&dat);
    }

    // Get IP address
    char ip_str[INET_ADDRSTRLEN] = "0.0.0.0";
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
                inet_ntop(AF_INET, &addr->sin_addr, ip_str, INET_ADDRSTRLEN);
                if (strcmp(ip_str, "127.0.0.1") != 0) {
                    break;
                }
            }
        }
        freeifaddrs(ifaddr);
    }
    
    install_suite_icon();
    char msg[128];
    char sub[96];
    if (update_prev[0])
        snprintf(msg, sizeof(msg), "PS5 Suite updated: v%s → v" SUITE_VERSION, update_prev);
    else
        snprintf(msg, sizeof(msg), "Welcome to PS5 Suite v" SUITE_VERSION);
    snprintf(sub, sizeof(sub), "Server ready — %s:%d", ip_str, g_bound_port);
    send_notification_pretty(msg, sub);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            continue;
        }
        
        // Aggressive TCP socket options for sustained high speed
        setsockopt(client_sock, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
        
        // Increase buffers to 16MB for maximum throughput
        int large_buf = 16 * 1024 * 1024;
        setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, &large_buf, sizeof(large_buf));
        setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, &large_buf, sizeof(large_buf));
        
        // TCP optimizations - TCP_NODELAY for immediate send
        int nodelay = 1;
        setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        
        // TCP_MAXSEG: only lower the MSS on WiFi networks (where PMTUD
        // problems are common); on wired gigabit jumbo frames are common and
        // forcing 1460 reduced throughput.
        // int maxseg = 1460;
        // setsockopt(client_sock, IPPROTO_TCP, TCP_MAXSEG, &maxseg, sizeof(maxseg));
        
        // NOTE: Removed TCP_NOPUSH - it was causing buffering delays!
        
        // CRITICAL: Unlimited timeout for files of ANY size
        // Keepalive will detect and close dead connections (~25s)
        // This allows 50GB+ files to upload without timeout issues
        struct timeval tv;
        tv.tv_sec = 0;  // 0 = unlimited timeout
        tv.tv_usec = 0;
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        
        // CRITICAL: Aggressive keepalive to prevent connection drops on large files
        int keepalive = 1;
        setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        
        // Set keepalive parameters (FreeBSD/PS5)
        int keepidle = 10;   // Start keepalive after 10 seconds of idle
        int keepintvl = 5;   // Send keepalive every 5 seconds
        int keepcnt = 3;     // Drop connection after 3 failed keepalives
        setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
        setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
        setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
        
        client_session_t *session = malloc(sizeof(client_session_t));
        if (!session) {
            close(client_sock);
            continue;
        }
        
        memset(session, 0, sizeof(client_session_t));
        session->sock = client_sock;
        
        // Reject connections beyond the session cap (client retries backoff)
        if (g_active_sessions >= MAX_CLIENT_SESSIONS) {
            const char *busy_msg = "Too many concurrent connections - retry shortly";
            uint8_t header[5];
            header[0] = RESP_ERROR;
            uint32_t mlen = (uint32_t)strlen(busy_msg) + 1;
            memcpy(header + 1, &mlen, 4);
            send_all(client_sock, header, 5);
            send_all(client_sock, busy_msg, mlen);
            usleep(50000);  // let the rejection reach the wire before closing
            close(client_sock);
            free(session);
            continue;
        }
        
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        
        if (pthread_create(&thread, &attr, client_thread, session) != 0) {
            close(client_sock);
            free(session);
        } else {
            __sync_fetch_and_add(&g_active_sessions, 1);
        }
        
        pthread_attr_destroy(&attr);
    }
    
    close(server_sock);
    return 0;
}
