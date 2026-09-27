/*
 * gonzod.c -- Gonzo system policy compiler
 *
 * Salvage of rookd. Same hardware-tested tunables. Different fence.
 *
 * gonzod owns topology, cgroup lanes, scheduler, cpufreq, IRQ
 * affinity, dirty writeback, and storage queues.
 *
 * gonzod does not own anonymous reclaim, ZRAM, swappiness, or
 * process discard. That is detritusd. It does not own page-cache
 * hold. That is gonzocache. It does not rewrite Xorg or audio
 * runit scripts. Binding live PIDs is enough; replacing the
 * display-server service is how the old tree fought itself.
 *
 * No daemon loop of its own. Policy at lifecycle edges:
 *   gonzod enable    boot / apply
 *   gonzod login     session bind
 *   gonzod reset     tear down + apply
 *   gonzod disable   tear down
 *   gonzod cpufreq   udev hook
 *   gonzod status
 *   gonzod install / uninstall
 *
 *   gcc -O2 -Wall -Wextra -Wno-unused-parameter -o gonzod gonzod.c
 */

#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>

#define GONZO_VERSION  "1.0.0"
#define GONZO_STATE    "/run/gonzo"
#define GONZO_CGROUP   "/sys/fs/cgroup/gonzo"
#define GONZO_LOG      "/var/log/gonzod.log"
#define GONZO_PERSIST  "/var/lib/gonzo"

typedef struct {
    int  ncpus;
    int  phys_cores;
    int  smt_enabled;
    int  numa_nodes;
    int  use_cpuset;
    char strategy[16];
    char interactive_cpus[64];
    char batch_cpus[64];
    char all_cpus[64];
    char storage_type[16];
    char root_disk[64];
} topology_t;

static topology_t g_topo;
static FILE *g_logfile = NULL;

static void rlog(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));

    va_list ap;
    va_start(ap, fmt);
    if (g_logfile) {
        fprintf(g_logfile, "[%s] ", ts);
        vfprintf(g_logfile, fmt, ap);
        fputc('\n', g_logfile);
        fflush(g_logfile);
    }
    va_end(ap);

    va_start(ap, fmt);
    fprintf(stderr, "[gonzod %s] ", ts);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void die(const char *msg)
{
    rlog("FATAL: %s", msg);
    exit(1);
}

static int write_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;
    ssize_t len = (ssize_t)strlen(value);
    int ok = (write(fd, value, (size_t)len) == len);
    close(fd);
    if (ok) rlog("  %s = %s", path, value);
    else    rlog("  Warning: write failed: %s", path);
    return ok ? 0 : -1;
}

static int write_sysctl(const char *dotted, const char *value)
{
    char path[256];
    snprintf(path, sizeof(path), "/proc/sys/%s", dotted);
    for (char *p = path + 9; *p; p++)
        if (*p == '.') *p = '/';
    return write_file(path, value);
}

static long read_long(const char *path, long defval)
{
    FILE *f = fopen(path, "r");
    if (!f) return defval;
    long v = defval;
    if (fscanf(f, "%ld", &v) != 1) v = defval;
    fclose(f);
    return v;
}

static int read_str(const char *path, char *buf, size_t sz)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)sz, f)) { fclose(f); return -1; }
    size_t l = strlen(buf);
    while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = '\0';
    fclose(f);
    return 0;
}

static void mkdirp(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static long memtotal_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128];
    long kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line + 9, "%ld", &kb);
            break;
        }
    }
    fclose(f);
    return kb;
}

static int proc_running(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "pgrep -x '%s' >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

/* ── Topology ──────────────────────────────────────────────────────── */

static void discover_topology(void)
{
    rlog("Topology discovery");
    memset(&g_topo, 0, sizeof(g_topo));

    g_topo.ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_topo.ncpus <= 0) g_topo.ncpus = 1;
    rlog("  Logical CPUs: %d", g_topo.ncpus);

    {
        char *p = g_topo.all_cpus;
        char *end = g_topo.all_cpus + sizeof(g_topo.all_cpus) - 1;
        for (int i = 0; i < g_topo.ncpus && p < end; i++) {
            if (i > 0 && p < end) *p++ = ',';
            int n = snprintf(p, (size_t)(end - p), "%d", i);
            if (n > 0) p += n;
        }
        *p = '\0';
    }

    {
        typedef struct { int pkg; int core; } phys_t;
        phys_t seen[256];
        int nseen = 0;
        g_topo.phys_cores = 0;
        for (int cpu = 0; cpu < g_topo.ncpus; cpu++) {
            char path[128];
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
            long pkg = read_long(path, 0);
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
            long core = read_long(path, cpu);
            int found = 0;
            for (int j = 0; j < nseen; j++) {
                if (seen[j].pkg == (int)pkg && seen[j].core == (int)core) {
                    found = 1;
                    break;
                }
            }
            if (!found && nseen < 256) {
                seen[nseen].pkg = (int)pkg;
                seen[nseen].core = (int)core;
                nseen++;
                g_topo.phys_cores++;
            }
        }
        if (g_topo.phys_cores == 0) g_topo.phys_cores = g_topo.ncpus;
    }

    g_topo.smt_enabled = (g_topo.ncpus > g_topo.phys_cores) ? 1 : 0;
    rlog("  Physical cores: %d  SMT: %d", g_topo.phys_cores, g_topo.smt_enabled);

    g_topo.numa_nodes = 0;
    {
        DIR *d = opendir("/sys/devices/system/node");
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strncmp(ent->d_name, "node", 4) == 0 &&
                    ent->d_name[4] >= '0' && ent->d_name[4] <= '9')
                    g_topo.numa_nodes++;
            }
            closedir(d);
        }
        if (g_topo.numa_nodes == 0) g_topo.numa_nodes = 1;
    }
    rlog("  NUMA nodes: %d", g_topo.numa_nodes);

    if (g_topo.phys_cores <= 3) {
        g_topo.use_cpuset = 0;
        snprintf(g_topo.strategy, sizeof(g_topo.strategy), "SHARED");
        snprintf(g_topo.interactive_cpus, sizeof(g_topo.interactive_cpus), "%s", g_topo.all_cpus);
        snprintf(g_topo.batch_cpus, sizeof(g_topo.batch_cpus), "%s", g_topo.all_cpus);
        rlog("  Strategy: SHARED");
    } else if (g_topo.phys_cores <= 7) {
        g_topo.use_cpuset = 1;
        snprintf(g_topo.strategy, sizeof(g_topo.strategy), "PARTITIONED");
        int interactive_n = g_topo.phys_cores / 2;
        if (interactive_n < 2) interactive_n = 2;
        typedef struct { int pkg; int core; } phys2_t;
        phys2_t picked[64];
        int npicked = 0;
        char *ip = g_topo.interactive_cpus;
        char *iend = ip + sizeof(g_topo.interactive_cpus) - 1;
        for (int cpu = 0; cpu < g_topo.ncpus && npicked < interactive_n; cpu++) {
            char path[128];
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
            long pkg = read_long(path, 0);
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
            long core = read_long(path, cpu);
            int found = 0;
            for (int j = 0; j < npicked; j++) {
                if (picked[j].pkg == (int)pkg && picked[j].core == (int)core) {
                    found = 1;
                    break;
                }
            }
            if (!found && npicked < 64) {
                picked[npicked].pkg = (int)pkg;
                picked[npicked].core = (int)core;
                npicked++;
                if (ip > g_topo.interactive_cpus && ip < iend) *ip++ = ',';
                int n = snprintf(ip, (size_t)(iend - ip), "%d", cpu);
                if (n > 0) ip += n;
            }
        }
        *ip = '\0';
        snprintf(g_topo.batch_cpus, sizeof(g_topo.batch_cpus), "%s", g_topo.all_cpus);
        rlog("  Strategy: PARTITIONED");
    } else {
        g_topo.use_cpuset = 1;
        snprintf(g_topo.strategy, sizeof(g_topo.strategy), "ISOLATED");
        int interactive_n = g_topo.phys_cores / 4;
        if (interactive_n < 2) interactive_n = 2;
        typedef struct { int pkg; int core; } phys3_t;
        phys3_t picked[64];
        int npicked = 0;
        char *ip = g_topo.interactive_cpus;
        char *iend = ip + sizeof(g_topo.interactive_cpus) - 1;
        for (int cpu = 0; cpu < g_topo.ncpus && npicked < interactive_n; cpu++) {
            char path[128];
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
            long pkg = read_long(path, 0);
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
            long core = read_long(path, cpu);
            int found = 0;
            for (int j = 0; j < npicked; j++) {
                if (picked[j].pkg == (int)pkg && picked[j].core == (int)core) {
                    found = 1;
                    break;
                }
            }
            if (!found && npicked < 64) {
                picked[npicked].pkg = (int)pkg;
                picked[npicked].core = (int)core;
                npicked++;
                if (ip > g_topo.interactive_cpus && ip < iend) *ip++ = ',';
                int n = snprintf(ip, (size_t)(iend - ip), "%d", cpu);
                if (n > 0) ip += n;
            }
        }
        *ip = '\0';
        char *bp = g_topo.batch_cpus;
        char *bend = bp + sizeof(g_topo.batch_cpus) - 1;
        for (int cpu = 0; cpu < g_topo.ncpus; cpu++) {
            char cpustr[16];
            snprintf(cpustr, sizeof(cpustr), "%d", cpu);
            int in_interactive = 0;
            const char *s = g_topo.interactive_cpus;
            while (*s) {
                const char *comma = strchr(s, ',');
                size_t len = comma ? (size_t)(comma - s) : strlen(s);
                if (len == strlen(cpustr) && strncmp(s, cpustr, len) == 0) {
                    in_interactive = 1;
                    break;
                }
                s = comma ? comma + 1 : s + len;
            }
            if (!in_interactive) {
                if (bp > g_topo.batch_cpus && bp < bend) *bp++ = ',';
                int n = snprintf(bp, (size_t)(bend - bp), "%d", cpu);
                if (n > 0) bp += n;
            }
        }
        *bp = '\0';
        if (g_topo.batch_cpus[0] == '\0')
            snprintf(g_topo.batch_cpus, sizeof(g_topo.batch_cpus), "%s", g_topo.all_cpus);
        rlog("  Strategy: ISOLATED");
    }

    rlog("  Interactive CPUs: %s", g_topo.interactive_cpus);
    rlog("  Batch CPUs: %s", g_topo.batch_cpus);

    snprintf(g_topo.storage_type, sizeof(g_topo.storage_type), "unknown");
    char root_dev[128] = {0};
    {
        FILE *f = popen("df / 2>/dev/null | awk 'NR==2{print $1}'", "r");
        if (f) {
            if (fgets(root_dev, sizeof(root_dev), f)) {
                size_t l = strlen(root_dev);
                while (l > 0 && (root_dev[l - 1] == '\n' || root_dev[l - 1] == '\r'))
                    root_dev[--l] = '\0';
            }
            pclose(f);
        }
    }
    if (root_dev[0]) {
        char dev[64] = {0};
        const char *bname = strrchr(root_dev, '/');
        bname = bname ? bname + 1 : root_dev;
        if (strncmp(bname, "nvme", 4) == 0) {
            snprintf(dev, sizeof(dev), "%s", bname);
            char *p = strrchr(dev, 'p');
            if (p && p > dev + 4) *p = '\0';
            snprintf(g_topo.storage_type, sizeof(g_topo.storage_type), "nvme");
        } else if (strncmp(bname, "mmcblk", 6) == 0) {
            snprintf(dev, sizeof(dev), "%s", bname);
            char *p = strrchr(dev, 'p');
            if (p && p > dev + 6) *p = '\0';
            snprintf(g_topo.storage_type, sizeof(g_topo.storage_type), "emmc");
        } else {
            snprintf(dev, sizeof(dev), "%s", bname);
            size_t l = strlen(dev);
            while (l > 0 && dev[l - 1] >= '0' && dev[l - 1] <= '9') dev[--l] = '\0';
            char rotpath[128];
            snprintf(rotpath, sizeof(rotpath), "/sys/block/%s/queue/rotational", dev);
            long rot = read_long(rotpath, 1);
            snprintf(g_topo.storage_type, sizeof(g_topo.storage_type), rot ? "hdd" : "ssd");
        }
        snprintf(g_topo.root_disk, sizeof(g_topo.root_disk), "%s", dev);
        rlog("  Root device: %s -> %s  storage_type=%s",
             root_dev, dev, g_topo.storage_type);
    }

    mkdirp(GONZO_STATE);
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/topology", GONZO_STATE);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f,
                "NCPUS=%d\nPHYS_CORES=%d\nSMT_ENABLED=%d\nNUMA_NODES=%d\n"
                "USE_CPUSET=%d\nSTRATEGY=%s\nINTERACTIVE_CPUS=%s\n"
                "BATCH_CPUS=%s\nSTORAGE_TYPE=%s\nROOT_DISK=%s\n",
                g_topo.ncpus, g_topo.phys_cores, g_topo.smt_enabled,
                g_topo.numa_nodes, g_topo.use_cpuset, g_topo.strategy,
                g_topo.interactive_cpus, g_topo.batch_cpus,
                g_topo.storage_type, g_topo.root_disk);
            fclose(f);
        }
    }
}

static int load_topology(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/topology", GONZO_STATE);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64], val[192];
        if (sscanf(line, "%63[^=]=%191[^\n]", key, val) != 2) continue;
        if      (!strcmp(key, "NCPUS"))            g_topo.ncpus = atoi(val);
        else if (!strcmp(key, "PHYS_CORES"))       g_topo.phys_cores = atoi(val);
        else if (!strcmp(key, "SMT_ENABLED"))      g_topo.smt_enabled = atoi(val);
        else if (!strcmp(key, "NUMA_NODES"))       g_topo.numa_nodes = atoi(val);
        else if (!strcmp(key, "USE_CPUSET"))       g_topo.use_cpuset = atoi(val);
        else if (!strcmp(key, "STRATEGY"))         snprintf(g_topo.strategy, sizeof(g_topo.strategy), "%s", val);
        else if (!strcmp(key, "INTERACTIVE_CPUS")) snprintf(g_topo.interactive_cpus, sizeof(g_topo.interactive_cpus), "%s", val);
        else if (!strcmp(key, "BATCH_CPUS"))       snprintf(g_topo.batch_cpus, sizeof(g_topo.batch_cpus), "%s", val);
        else if (!strcmp(key, "STORAGE_TYPE"))     snprintf(g_topo.storage_type, sizeof(g_topo.storage_type), "%s", val);
        else if (!strcmp(key, "ROOT_DISK"))        snprintf(g_topo.root_disk, sizeof(g_topo.root_disk), "%s", val);
    }
    fclose(f);
    if (g_topo.ncpus > 0 && g_topo.all_cpus[0] == '\0') {
        char *p = g_topo.all_cpus;
        char *end = g_topo.all_cpus + sizeof(g_topo.all_cpus) - 1;
        for (int i = 0; i < g_topo.ncpus && p < end; i++) {
            if (i > 0 && p < end) *p++ = ',';
            int n = snprintf(p, (size_t)(end - p), "%d", i);
            if (n > 0) p += n;
        }
        *p = '\0';
    }
    return 0;
}

/* ── Cgroups ───────────────────────────────────────────────────────── */

static void write_cg(const char *lane, const char *file, const char *val)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", GONZO_CGROUP, lane, file);
    write_file(path, val);
}

static void build_cgroups(void)
{
    rlog("Cgroup construction (%s)", g_topo.strategy);

    const char *controllers[] = { "cpu", "cpuset", "memory", "io", NULL };
    for (int i = 0; controllers[i]; i++) {
        char val[32];
        snprintf(val, sizeof(val), "+%s", controllers[i]);
        if (write_file("/sys/fs/cgroup/cgroup.subtree_control", val) < 0)
            rlog("  Warning: root subtree_control +%s failed", controllers[i]);
    }

    mkdirp(GONZO_CGROUP);
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/cgroup.subtree_control", GONZO_CGROUP);
        for (int i = 0; controllers[i]; i++) {
            char val[32];
            snprintf(val, sizeof(val), "+%s", controllers[i]);
            if (write_file(path, val) < 0)
                rlog("  Warning: gonzo subtree_control +%s failed", controllers[i]);
        }
    }

    const char *lanes[] = { "interactive", "batch", "maintenance", NULL };
    for (int i = 0; lanes[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", GONZO_CGROUP, lanes[i]);
        mkdirp(path);
    }

    write_cg("interactive", "cpu.weight", "600");
    write_cg("interactive", "io.weight",  "600");
    write_cg("batch",       "cpu.weight", "200");
    write_cg("batch",       "io.weight",  "200");
    write_cg("maintenance", "cpu.weight", "50");
    write_cg("maintenance", "io.weight",  "50");
    write_cg("interactive", "cpuset.cpus", g_topo.all_cpus);

    /*
     * memory.low, not memory.min. min is unreclaimable — detritusd
     * cannot discard pages the kernel has hard-guaranteed. low is a
     * preference. That is the fence.
     */
    {
        long kb = memtotal_kb();
        long bytes = kb * 1024L;
        char val[32];
        snprintf(val, sizeof(val), "%ld", bytes * 15 / 100);
        write_cg("interactive", "memory.low", val);
        snprintf(val, sizeof(val), "%ld", bytes * 80 / 100);
        write_cg("batch", "memory.high", val);
        snprintf(val, sizeof(val), "%ld", bytes * 40 / 100);
        write_cg("maintenance", "memory.high", val);
    }
    rlog("  Lanes built: interactive / batch / maintenance");
}

static void bind_proc_to_lane(const char *name, const char *lane)
{
    char pgrep_cmd[256];
    snprintf(pgrep_cmd, sizeof(pgrep_cmd), "pgrep -x '%s' 2>/dev/null", name);
    FILE *f = popen(pgrep_cmd, "r");
    if (!f) return;
    char pidstr[16];
    while (fgets(pidstr, sizeof(pidstr), f)) {
        pid_t pid = (pid_t)atoi(pidstr);
        if (pid <= 0) continue;
        char cgpath[256];
        snprintf(cgpath, sizeof(cgpath), "%s/%s/cgroup.procs", GONZO_CGROUP, lane);
        char val[16];
        snprintf(val, sizeof(val), "%d", (int)pid);
        if (write_file(cgpath, val) == 0)
            rlog("  bind: %s [%d] -> %s", name, (int)pid, lane);
    }
    pclose(f);
}

static void bind_services(void)
{
    rlog("Service domain binding");
    bind_proc_to_lane("Xorg", "interactive");
    bind_proc_to_lane("pipewire", "interactive");
    bind_proc_to_lane("wireplumber", "interactive");
    bind_proc_to_lane("dbus-daemon", "interactive");
    bind_proc_to_lane("mate-session", "interactive");
    bind_proc_to_lane("mate-panel", "interactive");
    bind_proc_to_lane("mate-settings-daemon", "interactive");
    bind_proc_to_lane("marco", "interactive");
    bind_proc_to_lane("nemo-desktop", "interactive");
    bind_proc_to_lane("mate-system-monitor", "interactive");
    /* Reclaim must stay scheduled when the box is in trouble. */
    bind_proc_to_lane("detritusd", "interactive");
    /* Speculative file-cache hold yields first. */
    bind_proc_to_lane("gonzocache", "batch");
    bind_proc_to_lane("cron", "maintenance");
    bind_proc_to_lane("anacron", "maintenance");
    bind_proc_to_lane("rsyslogd", "maintenance");
    bind_proc_to_lane("syslog-ng", "maintenance");
}

static void bind_login(void)
{
    rlog("Login binding");
    bind_services();
}

/* ── Scheduler / cpufreq / IRQ ─────────────────────────────────────── */

static void set_scheduler(void)
{
    rlog("RT scheduler policy");
    struct utsname u;
    if (uname(&u) == 0) {
        if (strstr(u.version, "PREEMPT_RT") || strstr(u.version, "-rt"))
            rlog("  Kernel: PREEMPT_RT confirmed");
        else
            rlog("  PREEMPT_RT not confirmed — tunables applied anyway");
    }
    write_sysctl("kernel.sched_rt_period_us", "1000000");
    write_sysctl("kernel.sched_rt_runtime_us", "950000");
    write_sysctl("kernel.sched_rr_timeslice_ms", "4");
    write_sysctl("kernel.sched_cfs_bandwidth_slice_us", "3000");
    write_sysctl("kernel.sched_schedstats", "0");
}

static void tune_cpufreq(void)
{
    rlog("CPU frequency governor");
    const char *gov = "schedutil";
    int ncpus = g_topo.ncpus > 0 ? g_topo.ncpus : (int)sysconf(_SC_NPROCESSORS_ONLN);
    for (int i = 0; i < ncpus; i++) {
        char path[128];
        snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        write_file(path, gov);
        char minpath[128], minval[32];
        snprintf(minpath, sizeof(minpath),
            "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_min_freq", i);
        long hw_min = read_long(minpath, 0);
        if (hw_min > 0) {
            snprintf(minval, sizeof(minval), "%ld", hw_min);
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", i);
            write_file(path, minval);
        }
    }
    {
        const char *udev_path = "/etc/udev/rules.d/99-gonzod-cpufreq.rules";
        FILE *f = fopen(udev_path, "w");
        if (f) {
            fprintf(f,
                "# gonzod -- reapply CPU governor on power source change\n"
                "SUBSYSTEM==\"power_supply\", ATTR{type}==\"Mains\","
                " RUN+=\"/usr/local/sbin/gonzod cpufreq\"\n");
            fclose(f);
            rlog("  udev rule: %s", udev_path);
        }
    }
}

static void quiet_irqs(void)
{
    rlog("IRQ quieting");
    int moved = 0;
    FILE *f = fopen("/proc/interrupts", "r");
    if (!f) { rlog("  Cannot read /proc/interrupts"); return; }

    char line[512];
    int first = 1;
    while (fgets(line, sizeof(line), f)) {
        if (first) { first = 0; continue; }
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        int irq = atoi(line);
        if (irq <= 0) continue;
        *colon = ':';
        char *name = colon + 1;
        while (*name == ' ' || (*name >= '0' && *name <= '9')) name++;
        while (*name && *name != ' ') name++;
        while (*name == ' ') name++;
        size_t nlen = strlen(name);
        while (nlen > 0 && (name[nlen - 1] == '\n' || name[nlen - 1] == '\r' || name[nlen - 1] == ' '))
            name[--nlen] = '\0';

        int noisy = (strstr(name, "nvme") || strstr(name, "eth") ||
                     strstr(name, "enp") || strstr(name, "ens") ||
                     strstr(name, "eno") || strstr(name, "wlan") ||
                     strstr(name, "wifi") || strstr(name, "amdgpu") ||
                     strstr(name, "nvidia") || strstr(name, "i915") ||
                     strstr(name, "iwlwifi") || strstr(name, "radeon"));
        if (!noisy) continue;

        char affpath[64];
        snprintf(affpath, sizeof(affpath), "/proc/irq/%d/smp_affinity_list", irq);
        char cur[64];
        if (read_str(affpath, cur, sizeof(cur)) == 0 &&
            strcmp(cur, g_topo.batch_cpus) == 0)
            continue;

        const char *irq_target;
        char last_cpu[16];
        if (strcmp(g_topo.strategy, "SHARED") == 0) {
            snprintf(last_cpu, sizeof(last_cpu), "%d", g_topo.ncpus - 1);
            irq_target = last_cpu;
        } else {
            irq_target = g_topo.batch_cpus;
        }
        if (write_file(affpath, irq_target) == 0) {
            rlog("  Moved IRQ %d (%s) -> cpu%s", irq, name, irq_target);
            moved++;
        }
    }
    fclose(f);
    rlog("  Moved %d noisy IRQs", moved);
}

/* ── Writeback + storage. Not reclaim. ─────────────────────────────── */

static void calm_writeback(void)
{
    rlog("Writeback (storage-aware dirty policy)");
    int slow_storage = (strcmp(g_topo.storage_type, "emmc") == 0 ||
                        strcmp(g_topo.storage_type, "hdd") == 0);
    if (slow_storage) {
        rlog("  slow storage (%s) — batching policy", g_topo.storage_type);
        write_sysctl("vm.dirty_background_bytes", "33554432");
        write_sysctl("vm.dirty_bytes", "134217728");
        write_sysctl("vm.dirty_expire_centisecs", "1500");
        write_sysctl("vm.dirty_writeback_centisecs", "500");
    } else {
        rlog("  fast storage (%s) — trickle policy", g_topo.storage_type);
        write_sysctl("vm.dirty_background_bytes", "16777216");
        write_sysctl("vm.dirty_bytes", "67108864");
        write_sysctl("vm.dirty_expire_centisecs", "10");
        write_sysctl("vm.dirty_writeback_centisecs", "10");
    }
    write_sysctl("vm.stat_interval", "10");
    if (g_topo.numa_nodes <= 1)
        write_sysctl("kernel.numa_balancing", "0");
}

static void tune_storage(void)
{
    rlog("Storage queues");
    DIR *d = opendir("/sys/block");
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strncmp(ent->d_name, "zram", 4) == 0) continue;
        if (strncmp(ent->d_name, "loop", 4) == 0) continue;
        char p_queue[512], p_rot[512], p_ra[512], p_sched[512];
        snprintf(p_queue, sizeof(p_queue), "/sys/block/%s/queue", ent->d_name);
        snprintf(p_rot, sizeof(p_rot), "/sys/block/%s/queue/rotational", ent->d_name);
        snprintf(p_ra, sizeof(p_ra), "/sys/block/%s/queue/read_ahead_kb", ent->d_name);
        snprintf(p_sched, sizeof(p_sched), "/sys/block/%s/queue/scheduler", ent->d_name);
        if (access(p_queue, F_OK) != 0) continue;
        int slow = 0;
        if (strncmp(ent->d_name, "mmcblk", 6) == 0) slow = 1;
        else slow = (int)read_long(p_rot, 0);
        if (slow) {
            write_file(p_ra, "512");
            write_file(p_sched, "bfq");
        } else {
            write_file(p_ra, "256");
            write_file(p_sched, "none");
        }
    }
    closedir(d);
}

/* ── Status / lifecycle ────────────────────────────────────────────── */

static void write_status(void)
{
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", localtime(&now));

    char sched[64] = "unknown";
    if (g_topo.root_disk[0]) {
        char path[128], raw[128];
        snprintf(path, sizeof(path), "/sys/block/%s/queue/scheduler", g_topo.root_disk);
        if (read_str(path, raw, sizeof(raw)) == 0) {
            char *lb = strchr(raw, '[');
            char *rb = lb ? strchr(lb, ']') : NULL;
            if (lb && rb) {
                size_t l = (size_t)(rb - lb - 1);
                if (l < sizeof(sched)) {
                    memcpy(sched, lb + 1, l);
                    sched[l] = '\0';
                }
            }
        }
    }

    mkdirp(GONZO_STATE);
    char path[256];
    snprintf(path, sizeof(path), "%s/status", GONZO_STATE);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "state=active\n"
        "version=%s\n"
        "strategy=%s\n"
        "applied=%s\n"
        "phys_cores=%d\n"
        "numa_nodes=%d\n"
        "interactive_cpus=%s\n"
        "batch_cpus=%s\n"
        "cpuset_partitioned=%d\n"
        "root_disk=%s\n"
        "storage_type=%s\n"
        "io_scheduler=%s\n"
        "detritusd=%s\n"
        "gonzocache=%s\n",
        GONZO_VERSION, g_topo.strategy, ts,
        g_topo.phys_cores, g_topo.numa_nodes,
        g_topo.interactive_cpus, g_topo.batch_cpus,
        g_topo.use_cpuset, g_topo.root_disk, g_topo.storage_type,
        sched,
        proc_running("detritusd") ? "running" : "absent",
        proc_running("gonzocache") ? "running" : "absent");
    fclose(f);
    rlog("Policy compiled.");
}

static void do_compile(void)
{
    discover_topology();
    build_cgroups();
    set_scheduler();
    tune_cpufreq();
    quiet_irqs();
    calm_writeback();
    tune_storage();
    bind_services();
    write_status();
}

static void do_reset(void)
{
    rlog("RESET: tearing down policy");
    const char *lanes[] = { "interactive", "batch", "maintenance", "background", NULL };
    for (int i = 0; lanes[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", GONZO_CGROUP, lanes[i]);
        rmdir(path);
    }
    rmdir(GONZO_CGROUP);
    write_sysctl("kernel.sched_rt_runtime_us", "950000");
    char path[256];
    snprintf(path, sizeof(path), "%s/status", GONZO_STATE); unlink(path);
    snprintf(path, sizeof(path), "%s/topology", GONZO_STATE); unlink(path);
    rlog("Reset complete");
}

static const char *find_runsvdir(void)
{
    static const char *dirs[] = {
        "/etc/service",
        "/var/service",
        "/etc/runit/runsvdir/default",
        "/etc/runit/runsvdir/current",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        struct stat st;
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode))
            return dirs[i];
    }
    return NULL;
}

static void do_install(void)
{
    rlog("Installing gonzod lifecycle hooks");
    mkdirp(GONZO_PERSIST);

    const char *resume_path = "/lib/elogind/system-sleep/90-gonzod";
    {
        FILE *f = fopen(resume_path, "w");
        if (f) {
            fprintf(f,
                "#!/bin/sh\n"
                "case \"$1\" in\n"
                "    post) /usr/local/sbin/gonzod reset >> %s 2>&1 & ;;\n"
                "esac\n", GONZO_LOG);
            fclose(f);
            chmod(resume_path, 0755);
            rlog("  elogind resume hook: %s", resume_path);
        }
    }

    {
        FILE *f = fopen("/etc/xdg/autostart/gonzod-login.desktop", "w");
        if (f) {
            fprintf(f,
                "[Desktop Entry]\n"
                "Type=Application\n"
                "Name=GonzoD Login Binding\n"
                "Exec=sudo /usr/local/sbin/gonzod login\n"
                "NoDisplay=true\n"
                "X-GNOME-AutoRestart=false\n");
            fclose(f);
            rlog("  XDG autostart: /etc/xdg/autostart/gonzod-login.desktop");
        }
    }

    {
        FILE *f = fopen("/etc/sudoers.d/gonzod-login", "w");
        if (f) {
            fprintf(f,
                "ALL ALL=(root) NOPASSWD: /usr/local/sbin/gonzod login\n");
            fclose(f);
            chmod("/etc/sudoers.d/gonzod-login", 0440);
            rlog("  sudoers rule: /etc/sudoers.d/gonzod-login");
        }
    }

    const char *runsvdir = find_runsvdir();
    if (!runsvdir) {
        rlog("  Warning: no runit runsvdir — skipping boot service");
    } else {
        char sv_dir[256];
        snprintf(sv_dir, sizeof(sv_dir), "%s/gonzod", runsvdir);
        mkdirp(sv_dir);
        char run_path[320];
        snprintf(run_path, sizeof(run_path), "%s/run", sv_dir);
        FILE *f = fopen(run_path, "w");
        if (f) {
            fputs("#!/bin/sh\n", f);
            fputs("exec 2>&1\n", f);
            fputs("/usr/local/sbin/gonzod enable\n", f);
            fputs("while true; do sleep 3600; done\n", f);
            fclose(f);
            chmod(run_path, 0755);
        }
        char finish_path[320];
        snprintf(finish_path, sizeof(finish_path), "%s/finish", sv_dir);
        f = fopen(finish_path, "w");
        if (f) {
            fputs("#!/bin/sh\nexit 0\n", f);
            fclose(f);
            chmod(finish_path, 0755);
        }
        rlog("  Runit boot service: %s", sv_dir);
    }
    rlog("Install complete. Run 'gonzod enable' to apply policy.");
}

static void do_uninstall(void)
{
    unlink("/lib/elogind/system-sleep/90-gonzod");
    unlink("/etc/xdg/autostart/gonzod-login.desktop");
    unlink("/etc/sudoers.d/gonzod-login");
    unlink("/etc/udev/rules.d/99-gonzod-cpufreq.rules");
    const char *paths[] = {
        "/etc/service/gonzod",
        "/var/service/gonzod",
        "/etc/runit/runsvdir/default/gonzod",
        "/etc/runit/runsvdir/current/gonzod",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        char p[320];
        snprintf(p, sizeof(p), "%s/run", paths[i]); unlink(p);
        snprintf(p, sizeof(p), "%s/finish", paths[i]); unlink(p);
        rmdir(paths[i]);
    }
    rlog("Lifecycle hooks removed.");
}

static void cmd_status(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/status", GONZO_STATE);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stdout, "gonzod: not active. Run 'gonzod enable' first.\n");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
        fputs(line, stdout);
    fclose(f);
}

static void usage(void)
{
    fprintf(stdout,
        "gonzod %s -- topology-aware system policy compiler\n\n"
        "Usage: gonzod <command>\n\n"
        "  enable      Discover topology and apply policy\n"
        "  reset       Tear down and reapply\n"
        "  login       Bind current session into cgroup lanes\n"
        "  disable     Remove policy\n"
        "  cpufreq     Reapply CPU governor (udev hook)\n"
        "  install     Wire lifecycle hooks\n"
        "  uninstall   Remove lifecycle hooks\n"
        "  status      Print current policy state\n\n"
        "Does not own ZRAM, swappiness, or discard (detritusd).\n"
        "Does not own page-cache hold (gonzocache).\n"
        "Does not rewrite Xorg or audio service scripts.\n",
        GONZO_VERSION);
}

int main(int argc, char *argv[])
{
    const char *cmd = (argc > 1) ? argv[1] : "help";
    g_logfile = fopen(GONZO_LOG, "a");

    int needs_root = !(strcmp(cmd, "status") == 0 ||
                       strcmp(cmd, "help") == 0 ||
                       strcmp(cmd, "--help") == 0 ||
                       strcmp(cmd, "-h") == 0);
    if (needs_root && getuid() != 0) {
        fprintf(stderr, "gonzod: must run as root\n");
        return 1;
    }

    rlog("gonzod %s -- %s", GONZO_VERSION, cmd);

    if (strcmp(cmd, "enable") == 0) {
        do_compile();
    } else if (strcmp(cmd, "reset") == 0) {
        do_reset();
        do_compile();
    } else if (strcmp(cmd, "login") == 0) {
        if (load_topology() != 0)
            die("Not active. Run 'gonzod enable' first.");
        bind_login();
    } else if (strcmp(cmd, "disable") == 0) {
        do_reset();
    } else if (strcmp(cmd, "cpufreq") == 0) {
        if (load_topology() != 0)
            g_topo.ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
        tune_cpufreq();
    } else if (strcmp(cmd, "status") == 0) {
        cmd_status();
    } else if (strcmp(cmd, "install") == 0) {
        do_install();
    } else if (strcmp(cmd, "uninstall") == 0) {
        do_uninstall();
    } else if (strcmp(cmd, "help") == 0 ||
               strcmp(cmd, "--help") == 0 ||
               strcmp(cmd, "-h") == 0) {
        usage();
    } else {
        fprintf(stderr, "gonzod: unknown command '%s'\n", cmd);
        usage();
        if (g_logfile) fclose(g_logfile);
        return 1;
    }

    if (g_logfile) fclose(g_logfile);
    return 0;
}
