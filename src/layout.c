#define _GNU_SOURCE
#include "ucraft.h"
#include "util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The ucraft system template: the directory layout and the base /etc files.
 *
 * This is configuration of the system, not software, so it is not a package: it lives inside
 * ucraft and is applied when a root filesystem is created (kit init, kit image build).
 * Applying it is idempotent and never overwrites anything that already exists.
 *
 * To change a default, edit the tables below. Packages create the directories of their own
 * files by themselves; this template only covers what belongs to no package. */

struct ldir  { const char *path; unsigned mode; };
struct llink { const char *path, *target; };
struct lfile { const char *path; unsigned mode; const char *content; };

static const struct ldir DIRS[] = {
    {"/boot", 0755},
    {"/dev", 0755},
    {"/etc", 0755},
    {"/home", 0755},
    {"/mnt", 0755},
    {"/opt", 0755},
    {"/proc", 0755},
    {"/root", 0700},
    {"/run", 0755},
    {"/srv", 0755},
    {"/sys", 0755},
    {"/tmp", 01777},
    {"/usr", 0755},
    {"/usr/bin", 0755},
    {"/usr/sbin", 0755},
    {"/usr/lib", 0755},
    {"/usr/include", 0755},
    {"/usr/share", 0755},
    {"/usr/src", 0755},
    {"/usr/local", 0755},
    {"/usr/local/bin", 0755},
    {"/usr/local/lib", 0755},
    {"/usr/local/include", 0755},
    {"/usr/local/share", 0755},
    {"/usr/local/src", 0755},
    {"/var", 0755},
    {"/var/cache", 0755},
    {"/var/lib", 0755},
    {"/var/log", 0755},
    {"/var/opt", 0755},
    {"/var/spool", 0755},
    {"/var/tmp", 01777},
};

/* usr-merge: /bin, /sbin and /lib live in /usr; /var/run is /run */
static const struct llink LINKS[] = {
    {"/bin", "usr/bin"},
    {"/sbin", "usr/sbin"},
    {"/lib", "usr/lib"},
    {"/var/run", "../run"},
    {"/etc/os-release", "../usr/lib/os-release"},
};

static const struct lfile FILES[] = {
    {"/etc/passwd", 0644,
    "root:x:0:0:root:/root:/bin/sh\n"
    "nobody:x:65534:65534:nobody:/:/bin/false\n"},
    {"/etc/group", 0644,
    "root:x:0:\n"
    "tty:x:5:\n"
    "wheel:x:10:\n"
    "nobody:x:65534:\n"},
    {"/etc/shadow", 0600,
    "root:!:0:0:99999:7:::\n"
    "nobody:!:0:0:99999:7:::\n"},
    {"/etc/hostname", 0644,
    "ucraft\n"},
    {"/etc/issue", 0644,
    "\n"
    "ucraft Linux \\n (\\l)\n"
    "\n"},
    {"/etc/profile", 0644,
    "# /etc/profile: system-wide setup for login shells\n"
    "export PATH=\"/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\"\n"
    "umask 022\n"},
    {"/etc/shells", 0644,
    "/bin/sh\n"
    "/bin/ash\n"},
    {"/etc/protocols", 0644,
    "# /etc/protocols: the IP protocols that programs look up by name\n"
    "ip\t0\tIP\n"
    "icmp\t1\tICMP\n"
    "igmp\t2\tIGMP\n"
    "ipip\t4\tIPIP\n"
    "tcp\t6\tTCP\n"
    "udp\t17\tUDP\n"
    "ipv6\t41\tIPv6\n"
    "gre\t47\tGRE\n"
    "esp\t50\tIPSEC-ESP\n"
    "ah\t51\tIPSEC-AH\n"
    "ipv6-icmp\t58\tIPv6-ICMP\n"
    "sctp\t132\tSCTP\n"},
    {"/etc/services", 0644,
    "# /etc/services: well-known ports that programs look up by name (the common ones only)\n"
    "ftp-data\t20/tcp\n"
    "ftp\t\t21/tcp\n"
    "ssh\t\t22/tcp\n"
    "telnet\t\t23/tcp\n"
    "smtp\t\t25/tcp\t\tmail\n"
    "domain\t\t53/tcp\n"
    "domain\t\t53/udp\n"
    "bootps\t\t67/udp\n"
    "bootpc\t\t68/udp\n"
    "tftp\t\t69/udp\n"
    "http\t\t80/tcp\t\twww\n"
    "pop3\t\t110/tcp\n"
    "ntp\t\t123/udp\n"
    "imap\t\t143/tcp\n"
    "snmp\t\t161/udp\n"
    "ldap\t\t389/tcp\n"
    "https\t\t443/tcp\n"
    "smtps\t\t465/tcp\n"
    "submission\t587/tcp\n"
    "ldaps\t\t636/tcp\n"
    "imaps\t\t993/tcp\n"
    "pop3s\t\t995/tcp\n"
    "mysql\t\t3306/tcp\n"
    "postgresql\t5432/tcp\n"
    "redis\t\t6379/tcp\n"
    "http-alt\t8080/tcp\n"},
    {"/usr/lib/os-release", 0644,
    "NAME=\"ucraft\"\n"
    "ID=ucraft\n"
    "PRETTY_NAME=\"ucraft Linux\"\n"
    "BUILD_ID=rolling\n"},
};

static char *under(const char *root, const char *path)
{
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    if (n == 1 && root[0] == '/') return xstrdup(path);   /* root is "/" */
    return xasprintf("%.*s%s", (int)n, root, path);
}

static void make_parent(const char *full)
{
    char *d = xstrdup(full);
    char *s = strrchr(d, '/');
    if (s && s != d) { *s = 0; mkdir_p(d, 0755); }
    free(d);
}

/* Creates what is missing under `root`; returns how many entries it created. */
int layout_apply(const char *root)
{
    int created = 0;
    for (size_t i = 0; i < sizeof DIRS / sizeof *DIRS; i++) {
        char *full = under(root, DIRS[i].path);
        if (!path_exists(full)) {
            mkdir_p(full, 0755);
            if (chmod(full, DIRS[i].mode) != 0) die("chmod %s: %s", full, strerror(errno));
            created++;
        }
        free(full);
    }
    for (size_t i = 0; i < sizeof LINKS / sizeof *LINKS; i++) {
        char *full = under(root, LINKS[i].path);
        if (!path_exists(full)) {
            make_parent(full);
            if (symlink(LINKS[i].target, full) != 0) die("symlink %s: %s", full, strerror(errno));
            created++;
        }
        free(full);
    }
    for (size_t i = 0; i < sizeof FILES / sizeof *FILES; i++) {
        char *full = under(root, FILES[i].path);
        if (!path_exists(full)) {
            make_parent(full);
            write_file_atomic(full, FILES[i].content, strlen(FILES[i].content));
            if (chmod(full, FILES[i].mode) != 0) die("chmod %s: %s", full, strerror(errno));
            created++;
        }
        free(full);
    }
    return created;
}
