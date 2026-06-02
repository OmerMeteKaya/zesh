//
// Created by mete on 27.04.2026.
//

#define _GNU_SOURCE
#include "../include/completions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **cd_frecency_list(const char *query, int limit, int *count_out);

typedef enum {
    DISTRO_UNKNOWN = 0,
    DISTRO_ARCH,        /* Arch, Manjaro, EndeavourOS, Garuda */
    DISTRO_DEBIAN,      /* Debian, Ubuntu, Mint, PopOS, Kali */
    DISTRO_FEDORA,      /* Fedora, RHEL, CentOS, Rocky, Alma */
    DISTRO_OPENSUSE,    /* openSUSE Leap, Tumbleweed */
    DISTRO_GENTOO,      /* Gentoo, Calculate */
    DISTRO_VOID,        /* Void Linux */
    DISTRO_ALPINE,      /* Alpine Linux */
    DISTRO_NIXOS,       /* NixOS */
    DISTRO_SLACKWARE,   /* Slackware */
    DISTRO_MACOS,       /* macOS */
    DISTRO_FREEBSD,     /* FreeBSD */
    DISTRO_OPENBSD,     /* OpenBSD */
} Distro;

static Distro current_distro = DISTRO_UNKNOWN;
static int distro_detected = 0;

static Distro detect_distro(void) {
    if (distro_detected) return current_distro;
    distro_detected = 1;

#ifdef __APPLE__
    current_distro = DISTRO_MACOS;
    return current_distro;
#endif

#ifdef __FreeBSD__
    current_distro = DISTRO_FREEBSD;
    return current_distro;
#endif

#ifdef __OpenBSD__
    current_distro = DISTRO_OPENBSD;
    return current_distro;
#endif

    /* Read /etc/os-release */
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) f = fopen("/usr/lib/os-release", "r");
    if (!f) { current_distro = DISTRO_UNKNOWN; return current_distro; }

    char line[256];
    char id[64] = {0};
    char id_like[128] = {0};

    while (fgets(line, sizeof(line), f)) {
        /* parse ID= */
        if (strncmp(line, "ID=", 3) == 0) {
            strncpy(id, line + 3, sizeof(id)-1);
            /* strip newline and quotes */
            int l = strlen(id);
            while (l > 0 && (id[l-1]=='\n'||id[l-1]=='"'||id[l-1]=='\''))
                id[--l] = '\0';
            if (id[0]=='"'||id[0]=='\'') memmove(id,id+1,strlen(id));
        }
        /* parse ID_LIKE= */
        if (strncmp(line, "ID_LIKE=", 8) == 0) {
            strncpy(id_like, line + 8, sizeof(id_like)-1);
            int l = strlen(id_like);
            while (l>0&&(id_like[l-1]=='\n'||id_like[l-1]=='"'||id_like[l-1]=='\''))
                id_like[--l] = '\0';
            if (id_like[0]=='"'||id_like[0]=='\'')
                memmove(id_like,id_like+1,strlen(id_like));
        }
    }
    fclose(f);

    /* Check ID and ID_LIKE for known distros */
    /* Helper: check if word appears in space-separated string */
    #define CONTAINS(haystack, needle) \
        (strstr(haystack, needle) != NULL)

    /* Arch-based */
    if (strcmp(id,"arch")==0 || strcmp(id,"manjaro")==0 ||
        strcmp(id,"endeavouros")==0 || strcmp(id,"garuda")==0 ||
        strcmp(id,"artix")==0 || strcmp(id,"cachyos")==0 ||
        CONTAINS(id_like,"arch")) {
        current_distro = DISTRO_ARCH;
    }
    /* Debian-based */
    else if (strcmp(id,"debian")==0 || strcmp(id,"ubuntu")==0 ||
             strcmp(id,"linuxmint")==0 || strcmp(id,"pop")==0 ||
             strcmp(id,"kali")==0 || strcmp(id,"elementary")==0 ||
             strcmp(id,"zorin")==0 || strcmp(id,"raspbian")==0 ||
             strcmp(id,"mx")==0 || strcmp(id,"parrot")==0 ||
             CONTAINS(id_like,"debian") || CONTAINS(id_like,"ubuntu")) {
        current_distro = DISTRO_DEBIAN;
    }
    /* Fedora-based */
    else if (strcmp(id,"fedora")==0 || strcmp(id,"rhel")==0 ||
             strcmp(id,"centos")==0 || strcmp(id,"rocky")==0 ||
             strcmp(id,"almalinux")==0 || strcmp(id,"nobara")==0 ||
             strcmp(id,"ol")==0 ||
             CONTAINS(id_like,"fedora") || CONTAINS(id_like,"rhel")) {
        current_distro = DISTRO_FEDORA;
    }
    /* openSUSE-based */
    else if (strcmp(id,"opensuse-leap")==0 ||
             strcmp(id,"opensuse-tumbleweed")==0 ||
             strcmp(id,"sles")==0 ||
             CONTAINS(id_like,"suse") || CONTAINS(id_like,"opensuse")) {
        current_distro = DISTRO_OPENSUSE;
    }
    /* Gentoo-based */
    else if (strcmp(id,"gentoo")==0 || strcmp(id,"calculate")==0 ||
             CONTAINS(id_like,"gentoo")) {
        current_distro = DISTRO_GENTOO;
    }
    /* Void */
    else if (strcmp(id,"void")==0) {
        current_distro = DISTRO_VOID;
    }
    /* Alpine */
    else if (strcmp(id,"alpine")==0) {
        current_distro = DISTRO_ALPINE;
    }
    /* NixOS */
    else if (strcmp(id,"nixos")==0) {
        current_distro = DISTRO_NIXOS;
    }
    /* Slackware */
    else if (strcmp(id,"slackware")==0) {
        current_distro = DISTRO_SLACKWARE;
    }
    else {
        current_distro = DISTRO_UNKNOWN;
    }

    #undef CONTAINS
    return current_distro;
}

/* Debian/Ubuntu */
static const char *apt_cmds[] = {
    "install","remove","purge","update","upgrade","full-upgrade",
    "autoremove","autoclean","clean","search","show","list",
    "depends","rdepends","policy","edit-sources","--help", NULL
};
static const char *dpkg_cmds[] = {
    "-i","--install","-r","--remove","-P","--purge",
    "-l","--list","-s","--status","-L","--listfiles",
    "-S","--search","--configure","--reconfigure",
    "--get-selections","--set-selections","--help", NULL
};
static const char *snap_cmds[] = {
    "install","remove","refresh","list","find","info",
    "enable","disable","connect","disconnect","run","help", NULL
};

/* Fedora/RHEL */
static const char *dnf_cmds[] = {
    "install","remove","update","upgrade","search","info","list",
    "check-update","autoremove","clean","history","repolist",
    "group","module","provides","download","reinstall","help", NULL
};
static const char *rpm_cmds[] = {
    "-i","--install","-U","--upgrade","-e","--erase",
    "-q","--query","-V","--verify","-l","--list",
    "-a","--all","--nodeps","--force","--help", NULL
};

/* openSUSE */
static const char *zypper_cmds[] = {
    "install","remove","update","upgrade","search","info","list-updates",
    "repos","addrepo","removerepo","refresh","clean","patches",
    "dist-upgrade","dup","help", NULL
};

/* Gentoo */
static const char *emerge_cmds[] = {
    "--sync","--update","--depclean","--search","--info",
    "--pretend","--ask","--verbose","--quiet","--oneshot",
    "--unmerge","--fetchonly","--newuse","--deep","--help", NULL
};

/* Void */
static const char *xbps_install_cmds[] = {
    "-S","--sync","-u","--update","-f","--force","--help", NULL
};
static const char *xbps_query_cmds[] = {
    "-l","--list-pkgs","-s","--search","-S","--show",
    "-f","--files","--help", NULL
};
static const char *xbps_remove_cmds[] = {
    "-R","--recursive","-O","--clean-cache","--help", NULL
};

/* Alpine */
static const char *apk_cmds[] = {
    "add","del","update","upgrade","search","info","list",
    "fix","fetch","cache","policy","audit","help", NULL
};

/* NixOS */
static const char *nix_cmds[] = {
    "build","develop","flake","profile","run","search","shell",
    "store","help", NULL
};
static const char *nixenv_cmds[] = {
    "-i","--install","-e","--uninstall","-u","--upgrade",
    "-q","--query","-A","--attr","--rollback","--list-generations",
    "--switch-generation","--help", NULL
};

/* macOS */
static const char *brew_cmds[] = {
    "install","uninstall","update","upgrade","search","info","list",
    "tap","untap","link","unlink","switch","pin","unpin",
    "services","doctor","cleanup","help", NULL
};
static const char *port_cmds[] = {
    "install","uninstall","update","upgrade","search","info","list",
    "activate","deactivate","clean","selfupdate","help", NULL
};

/* FreeBSD */
static const char *pkg_cmds[] = {
    "install","remove","update","upgrade","search","info","list",
    "autoremove","clean","check","audit","fetch","help", NULL
};

/* Slackware */
static const char *slackpkg_cmds[] = {
    "install","remove","upgrade","search","info","update",
    "show-changelog","help", NULL
};

/* Systemd (all distros) */
static const char *timedatectl_cmds[] = {
    "status","set-time","set-timezone","list-timezones",
    "set-ntp","help", NULL
};
static const char *hostnamectl_cmds[] = {
    "status","set-hostname","set-icon-name","set-chassis","help", NULL
};
static const char *localectl_cmds[] = {
    "status","set-locale","set-keymap","set-x11-keymap",
    "list-locales","list-keymaps","help", NULL
};
static const char *loginctl_cmds[] = {
    "list-sessions","list-users","list-seats","show-session",
    "show-user","lock-session","unlock-session","terminate-session",
    "kill-session","help", NULL
};

static const char *git_cmds[] = {
    "add", "branch", "checkout", "cherry-pick", "clone", "commit",
    "diff", "fetch", "init", "log", "merge", "mv", "pull", "push",
    "rebase", "remote", "reset", "restore", "rm", "show", "stash",
    "status", "switch", "tag", "--version", "--help", NULL
};

/* Helper: run_and_collect */
static char **run_and_collect(const char *shell_cmd, int *count_out) {
    /* runs shell_cmd via popen, returns malloc'd array of lines */
    FILE *fp = popen(shell_cmd, "r");
    if (!fp) { *count_out = 0; return NULL; }

    char **results = malloc(64 * sizeof(char *));
    if (!results) { pclose(fp); *count_out = 0; return NULL; }
    int count = 0, cap = 64;

    char line[512];
    while (fgets(line, sizeof(line), fp) && count < 60) {
        /* strip newline */
        int l = strlen(line);
        while (l > 0 && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l]='\0';
        if (l == 0) continue;
        if (count >= cap) {
            cap *= 2;
            char **tmp = realloc(results, cap * sizeof(char*));
            if (!tmp) break;
            results = tmp;
        }
        results[count++] = strdup(line);
    }
    pclose(fp);
    *count_out = count;
    if (count == 0) { free(results); return NULL; }
    return results;
}

/* Helper: get_git_branches */
static char **get_git_branches(const char *prefix, int *count_out) {
    /* git branch --format='%(refname:short)' 2>/dev/NULL */
    char cmd[512];
    if (prefix && *prefix)
        snprintf(cmd, sizeof(cmd),
            "git branch --format='%%(refname:short)' 2>/dev/NULL"
            " | grep '^%s'", prefix);
    else
        snprintf(cmd, sizeof(cmd),
            "git branch --format='%%(refname:short)' 2>/dev/NULL");
    return run_and_collect(cmd, count_out);
}

/* Helper: get_git_remotes */
static char **get_git_remotes(const char *prefix, int *count_out) {
    char cmd[512];
    if (prefix && *prefix)
        snprintf(cmd, sizeof(cmd),
            "git remote 2>/dev/NULL | grep '^%s'", prefix);
    else
        snprintf(cmd, sizeof(cmd), "git remote 2>/dev/NULL");
    return run_and_collect(cmd, count_out);
}

/* Helper: get_git_changed_files */
static char **get_git_changed_files(const char *prefix, int *count_out) {
    /* git status --short → lines like " M file.c" or "?? file.c" */
    char cmd[512];
    if (prefix && *prefix)
        snprintf(cmd, sizeof(cmd),
            "git status --short 2>/dev/NULL"
            " | awk '{print $NF}' | grep '^%s'", prefix);
    else
        snprintf(cmd, sizeof(cmd),
            "git status --short 2>/dev/NULL | awk '{print $NF}'");
    return run_and_collect(cmd, count_out);
}

/* Helper: get_git_stashes */
static char **get_git_stashes(const char *prefix, int *count_out) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "git stash list 2>/dev/NULL"
        " | awk -F: '{print $1}'");
    (void)prefix;
    return run_and_collect(cmd, count_out);
}

/* Universal tools */
static const char *kubectl_cmds[] = {
    "get","describe","apply","delete","create","edit","exec",
    "logs","port-forward","scale","rollout","config",
    "namespace","pod","service","deployment","--help", NULL
};
static const char *terraform_cmds[] = {
    "init","plan","apply","destroy","validate","fmt",
    "show","state","import","output","refresh","--help", NULL
};
static const char *ansible_cmds[] = {
    "-i","--inventory","-m","--module-name","-a","--args",
    "-b","--become","-u","--user","--ask-pass",
    "--ask-become-pass","-v","-vvv","--help", NULL
};
static const char *cmake_cmds[] = {
    "-S","-B","-G","-D","-DCMAKE_BUILD_TYPE=Debug",
    "-DCMAKE_BUILD_TYPE=Release","--build","--install",
    "--target","--config","--help", NULL
};
static const char *meson_cmds[] = {
    "setup","compile","install","test","dist",
    "configure","introspect","wrap","--help", NULL
};
static const char *gcloud_cmds[] = {
    "auth","compute","container","config","iam","run",
    "storage","sql","pubsub","functions","--help", NULL
};
static const char *aws_cmds[] = {
    "s3","ec2","iam","lambda","rds","eks","ecs","cloudformation",
    "configure","sts","ssm","--help", NULL
};
static const char *az_cmds[] = {  /* Azure CLI */
    "login","logout","account","vm","storage","network",
    "webapp","functionapp","aks","acr","group","--help", NULL
};
static const char *tmux_cmds[] = {
    "new","-s","attach","-t","detach","kill-session",
    "list-sessions","split-window","new-window",
    "select-pane","send-keys","--help", NULL
};
static const char *nvim_extra_cmds[] = {
    "--headless","--noplugin","-u","NONE",
    "+Lazy","--help", NULL
};
static const char *strace_cmds[] = {
    "-p","-e","-o","-f","-c","-T","-t","-tt",
    "-s","--help", NULL
};
static const char *perf_cmds[] = {
    "stat","record","report","top","list",
    "annotate","diff","bench","--help", NULL
};
static const char *objdump_cmds[] = {
    "-d","-D","-f","-h","-t","-r","-s",
    "--disassemble","--help", NULL
};
static const char *nm_cmds[] = {
    "-a","-g","-u","-l","-n","--defined-only",
    "--undefined-only","--help", NULL
};
static const char *ldd_cmds[] = {
    "-v","--version","--help", NULL
};
static const char *readelf_cmds[] = {
    "-a","-h","-l","-S","-s","-r","-d",
    "--symbols","--help", NULL
};
static const char *socat_cmds[] = {
    "TCP:","TCP-LISTEN:","UDP:","UNIX:","EXEC:",
    "STDIO","FILE:","PIPE:","--help", NULL
};
static const char *nmap_cmds[] = {
    "-sS","-sV","-sC","-O","-A","-p","-Pn",
    "--script","--open","-oN","-oX","--help", NULL
};
static const char *netstat_cmds[] = {
    "-t","-u","-l","-p","-n","-r","-s",
    "-a","--help", NULL
};
static const char *ss_cmds[] = {
    "-t","-u","-l","-p","-n","-a","-s",
    "-4","-6","--help", NULL
};
static const char *iptables_cmds[] = {
    "-A","-D","-I","-L","-F","-N","-X",
    "-t","--line-numbers","--help", NULL
};
static const char *openssl_cmds[] = {
    "genrsa","rsa","req","x509","pkcs12","s_client",
    "s_server","dgst","enc","rand","verify","--help", NULL
};
static const char *pass_cmds[] = {  /* password-store */
    "show","insert","generate","rm","mv","cp","edit",
    "git","ls","grep","--help", NULL
};
static const char *podman_cmds[] = {  /* same as docker */
    "run","ps","build","exec","stop","rm","rmi","images",
    "pull","push","logs","inspect","network","volume",
    "compose","--help", NULL
};
static const char *flatpak_cmds[] = {
    "install","uninstall","update","list","search","info",
    "run","enter","override","remote-add","remote-list",
    "repair","--help", NULL
};
static const char *snap_universal_cmds[] = {
    "install","remove","refresh","list","find","info",
    "enable","disable","connect","disconnect","run","help", NULL
};
static const char *virsh_cmds[] = {
    "list","start","shutdown","destroy","suspend","resume",
    "create","define","undefine","dumpxml","console",
    "snapshot-create","snapshot-list","--help", NULL
};
static const char *qemu_cmds[] = {
    "-m","-cpu","-smp","-hda","-hdb","-cdrom",
    "-boot","-net","-drive","-enable-kvm",
    "-nographic","-vnc","--help", NULL
};
static const char *ffprobe_cmds[] = {
    "-i","-v","-show_streams","-show_format",
    "-print_format","json","-select_streams","--help", NULL
};
static const char *convert_cmds[] = {  /* ImageMagick */
    "-resize","-quality","-format","-rotate","-flip",
    "-crop","-gravity","-blur","-sharpen","--help", NULL
};
static const char *sox_cmds[] = {
    "-t","-r","-c","-b","--norm","--effects-file",
    "trim","fade","rate","channels","--help", NULL
};
static const char *wget_cmds[] = {
    "-O","-q","-r","-np","--no-check-certificate",
    "-c","--mirror","--convert-links","--help", NULL
};
static const char *sed_cmds[] = {
    "-n","-e","-i","-r","-E","--quiet","--help", NULL
};
static const char *awk_cmds[] = {
    "-F","-v","-f","BEGIN","END","print","printf",
    "NR","NF","FS","RS","--help", NULL
};
static const char *jq_cmds[] = {
    "-r","-c","-n","-e","-s","-R",
    ".","keys","values","length","map",
    "select","has","type","--help", NULL
};
static const char *less_cmds[] = {
    "-N","-S","-R","-i","-F","-X",
    "+G","+F","--help", NULL
};
static const char *htop_cmds[] = {
    "-d","-u","-p","-s","--no-color","--help", NULL
};
static const char *ps_cmds[] = {
    "aux","auxf","-ef","-eo","--sort","--forest",
    "-u","-p","--help", NULL
};
static const char *lsof_cmds[] = {
    "-p","-u","-i","-n","-t","-c",
    "+D","-d","--help", NULL
};
static const char *df_cmds[] = {
    "-h","-H","-T","-i","-a","--total","--help", NULL
};
static const char *du_cmds[] = {
    "-h","-s","-a","--max-depth","--apparent-size",
    "-c","--exclude","--help", NULL
};
static const char *mount_cmds[] = {
    "-t","-o","loop","bind","remount",
    "-a","-l","--help", NULL
};
static const char *systemd_run_cmds[] = {
    "--unit","--scope","--slice","--user","--system",
    "--on-active","--on-calendar","--timer-property",
    "--pty","--same-dir","--help", NULL
};
/* macOS specific */
static const char *defaults_cmds[] = {
    "read","write","delete","domains","find",
    "rename","help", NULL
};
static const char *launchctl_cmds[] = {
    "load","unload","start","stop","list",
    "enable","disable","help", NULL
};
/* Android/Termux */
static const char *pkg_termux_cmds[] = {
    "install","uninstall","upgrade","update","list-installed",
    "list-packages","search","show","files","help", NULL
};
/* FreeBSD/OpenBSD */
static const char *service_cmds[] = {
    "start","stop","restart","status","enable","disable",
    "reload","onestart","onestop","help", NULL
};

static const char *pacman_cmds[] = {
    "-S", "-Ss", "-Si", "-Sw", "-Su", "-Syu", "-Syyu",
    "-R", "-Rs", "-Rns",
    "-Q", "-Qs", "-Qi", "-Ql", "-Qo", "-Qe", "-Qm",
    "-U", "-D", "--needed", "--noconfirm", "--help", NULL
};

static const char *systemctl_cmds[] = {
    "start", "stop", "restart", "reload", "enable", "disable",
    "status", "is-active", "is-enabled", "list-units", "list-timers",
    "daemon-reload", "poweroff", "reboot", "--help", NULL
};

static const char *journalctl_cmds[] = {
    "-f", "-e", "-x", "-u", "-b", "-k", "-n",
    "--since", "--until", "--list-boots",
    "--disk-usage", "--vacuum-size", "--vacuum-time",
    "--no-pager", "--help", NULL
};

static const char *docker_cmds[] = {
    "run", "ps", "build", "exec", "stop", "rm", "rmi", "images",
    "pull", "push", "logs", "inspect", "network", "volume",
    "compose", "--help", NULL
};

static const char *make_cmds[] = {
    "all", "clean", "install", "uninstall", "test", "debug",
    "release", "--help", NULL
};

static const char *npm_cmds[] = {
    "install", "uninstall", "run", "start", "test", "build",
    "init", "publish", "update", "list", "audit", "ci", "--help", NULL
};

static const char *grep_cmds[] = {
    "-r", "-i", "-v", "-n", "-l", "-c", "-A", "-B", "-C",
    "-E", "-F", "-P", "--include", "--exclude",
    "--color", "-w", "-x", "--help", NULL
};

static const char *find_cmds[] = {
    "-name", "-type", "-size", "-mtime", "-atime",
    "-exec", "-delete", "-print", "-maxdepth", "-mindepth",
    "-iname", "-user", "-group", "-perm", "--help", NULL
};

static const char *curl_cmds[] = {
    "-X", "-H", "-d", "-o", "-O", "-L", "-I",
    "-u", "-k", "-s", "-v", "--json",
    "--compressed", "--help", NULL
};

static const char *gcc_cmds[] = {
    "-o", "-c", "-g", "-O0", "-O1", "-O2", "-O3",
    "-Wall", "-Wextra", "-std=c99", "-std=c11",
    "-I", "-L", "-l", "-shared", "-fPIC",
    "-DDEBUG", "--help", NULL
};

static const char *python_m_cmds[] = {
    "venv", "pip", "http.server", "json.tool",
    "unittest", "pytest", "pdb", NULL
};

static const char *valgrind_cmds[] = {
    "--leak-check=full", "--show-leak-kinds=all",
    "--track-origins=yes", "--verbose",
    "--log-file=", "--tool=memcheck",
    "--tool=callgrind", "--help", NULL
};

static const char *ffmpeg_cmds[] = {
    "-i", "-c", "-c:v", "-c:a", "-vf", "-af",
    "-b:v", "-b:a", "-r", "-s", "-t", "-ss",
    "-map", "-y", "--help", NULL
};

static const char *ssh_cmds[] = {
    "-l", "-p", "-i", "-L", "-R", "-D", "-N", "-f", "-v", NULL
};
static const char *systemd_analyze_cmds[] = {
    "time", "blame", "critical-chain", "plot", "dot",
    "verify", "security", NULL
};

static const char *ip_cmds[] = {
    "addr", "link", "route", "neigh", "rule",
    "tunnel", "maddr", "monitor", "--help", NULL
};

static const char *cargo_cmds[] = {
    "build", "run", "test", "check", "clean", "doc", "new",
    "init", "add", "remove", "update", "publish", "--help", NULL
};

static const char *pip_cmds[] = {
    "install", "uninstall", "list", "show", "search", "freeze",
    "download", "wheel", "check", "--help", NULL
};

static const char *tar_cmds[] = {
    "-czf", "-xzf", "-cjf", "-xjf", "-cxf", "-xxf",
    "-tf", "-cf", "-xf", "-v", "--help", NULL
};

static const char *vim_cmds[] = {
    "-R", "-n", "-b", "-d", "-u", "+", "--help", NULL
};

static const char *python_cmds[] = {
    "-c", "-m", "-i", "-u", "-v",
    "-W", "--version", "--help", NULL
};

static const char *gdb_cmds[] = {
    "-q", "-batch", "-ex", "-x", "-p",
    "--args", "--help", NULL
};

static const char *rsync_cmds[] = {
    "-a", "-v", "-z", "-r", "-u", "-n",
    "--delete", "--exclude", "--include",
    "--progress", "--partial", "-e", "--help", NULL
};

static const char *yay_cmds[] = {
    "-S", "-Ss", "-Si", "-Su", "-Syu", "-Syyu",
    "-R", "-Rs", "-Rns",
    "-Q", "-Qs", "-Qi", "-Ql",
    "--aur", "--repo", "--devel", "--needed",
    "--noconfirm", "--help", NULL
};

typedef struct {
    const char *cmd;
    const char **subcmds;
} CmdTable;

static const CmdTable cmd_table[] = {
    { "kubectl",       kubectl_cmds       },
    { "terraform",     terraform_cmds     },
    { "ansible",       ansible_cmds       },
    { "cmake",         cmake_cmds         },
    { "meson",         meson_cmds         },
    { "gcloud",        gcloud_cmds        },
    { "aws",           aws_cmds           },
    { "az",            az_cmds            },
    { "tmux",          tmux_cmds          },
    { "strace",        strace_cmds        },
    { "perf",          perf_cmds          },
    { "objdump",       objdump_cmds       },
    { "nm",            nm_cmds            },
    { "readelf",       readelf_cmds       },
    { "socat",         socat_cmds         },
    { "nmap",          nmap_cmds          },
    { "netstat",       netstat_cmds       },
    { "ss",            ss_cmds            },
    { "iptables",      iptables_cmds      },
    { "openssl",       openssl_cmds       },
    { "pass",          pass_cmds          },
    { "podman",        podman_cmds        },
    { "flatpak",       flatpak_cmds       },
    { "snap",          snap_universal_cmds},
    { "virsh",         virsh_cmds         },
    { "ffprobe",       ffprobe_cmds       },
    { "convert",       convert_cmds       },
    { "wget",          wget_cmds          },
    { "sed",           sed_cmds           },
    { "awk",           awk_cmds           },
    { "jq",            jq_cmds            },
    { "less",          less_cmds          },
    { "htop",          htop_cmds          },
    { "ps",            ps_cmds            },
    { "lsof",          lsof_cmds          },
    { "df",            df_cmds            },
    { "du",            du_cmds            },
    { "mount",         mount_cmds         },
    { "defaults",      defaults_cmds      },
    { "launchctl",     launchctl_cmds     },
    { "service",       service_cmds       },
    { "git",             git_cmds              },
    { "systemctl",       systemctl_cmds        },
    { "systemd-analyze", systemd_analyze_cmds  },
    { "journalctl",      journalctl_cmds       },
    { "docker",          docker_cmds           },
    { "ip",              ip_cmds               },
    { "make",            make_cmds             },
    { "cargo",           cargo_cmds            },
    { "npm",             npm_cmds              },
    { "pip",             pip_cmds              },
    { "pip3",            pip_cmds              },
    { "grep",            grep_cmds             },
    { "find",            find_cmds             },
    { "tar",             tar_cmds              },
    { "curl",            curl_cmds             },
    { "vim",             vim_cmds              },
    { "nvim",            vim_cmds              },
    { "gcc",             gcc_cmds              },
    { "g++",             gcc_cmds              },
    { "python",          python_cmds           },
    { "python3",         python_cmds           },
    { "gdb",             gdb_cmds              },
    { "valgrind",        valgrind_cmds         },
    { "ffmpeg",          ffmpeg_cmds           },
    { "rsync",           rsync_cmds            },
    { "ssh",             ssh_cmds              },
    { NULL, NULL }
};

char **get_subcommands(const char *cmd, const char *word, int *count_out) {
    *count_out = 0;
    if (!cmd) return NULL;

    Distro distro = detect_distro();

    /* First check distro-specific commands */
    const char **subcmds = NULL;

    /* Package managers — distro specific */
    if (strcmp(cmd, "apt") == 0 || strcmp(cmd, "apt-get") == 0) {
        if (distro == DISTRO_DEBIAN) subcmds = apt_cmds;
        else return NULL;  /* not Debian-based, don't show */
    }
    else if (strcmp(cmd, "dpkg") == 0) {
        if (distro == DISTRO_DEBIAN) subcmds = dpkg_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "snap") == 0) {
        if (distro == DISTRO_DEBIAN) subcmds = snap_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "dnf") == 0 || strcmp(cmd, "yum") == 0) {
        if (distro == DISTRO_FEDORA) subcmds = dnf_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "rpm") == 0) {
        if (distro == DISTRO_FEDORA) subcmds = rpm_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "zypper") == 0) {
        if (distro == DISTRO_OPENSUSE) subcmds = zypper_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "emerge") == 0) {
        if (distro == DISTRO_GENTOO) subcmds = emerge_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "xbps-install") == 0) {
        if (distro == DISTRO_VOID) subcmds = xbps_install_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "xbps-query") == 0) {
        if (distro == DISTRO_VOID) subcmds = xbps_query_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "xbps-remove") == 0) {
        if (distro == DISTRO_VOID) subcmds = xbps_remove_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "apk") == 0) {
        if (distro == DISTRO_ALPINE) subcmds = apk_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "nix") == 0) {
        if (distro == DISTRO_NIXOS) subcmds = nix_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "nix-env") == 0) {
        if (distro == DISTRO_NIXOS) subcmds = nixenv_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "brew") == 0) {
        if (distro == DISTRO_MACOS) subcmds = brew_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "port") == 0) {
        if (distro == DISTRO_MACOS) subcmds = port_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "pkg") == 0) {
        if (distro == DISTRO_FREEBSD) subcmds = pkg_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "slackpkg") == 0) {
        if (distro == DISTRO_SLACKWARE) subcmds = slackpkg_cmds;
        else return NULL;
    }
    /* Arch — always show for arch-based */
    else if (strcmp(cmd, "pacman") == 0) {
        if (distro == DISTRO_ARCH) subcmds = pacman_cmds;
        else return NULL;
    }
    else if (strcmp(cmd, "yay") == 0 || strcmp(cmd, "paru") == 0) {
        if (distro == DISTRO_ARCH) subcmds = yay_cmds;
        else return NULL;
    }
    /* Systemd tools — available on most Linux distros */
    else if (strcmp(cmd, "timedatectl") == 0) subcmds = timedatectl_cmds;
    else if (strcmp(cmd, "hostnamectl") == 0) subcmds = hostnamectl_cmds;
    else if (strcmp(cmd, "localectl") == 0)   subcmds = localectl_cmds;
    else if (strcmp(cmd, "loginctl") == 0)    subcmds = loginctl_cmds;
    /* Common tools — existing tables */
    else {
        /* fall through to existing cmd_table lookup */
        for (int i = 0; cmd_table[i].cmd; i++) {
            if (strcmp(cmd_table[i].cmd, cmd) == 0) {
                subcmds = cmd_table[i].subcmds;
                break;
            }
        }
    }

    if (!subcmds) return NULL;

    /* filter by word prefix and return */
    int wlen = word ? strlen(word) : 0;
    int total = 0;
    for (int i = 0; subcmds[i]; i++)
        if (wlen == 0 || strncmp(subcmds[i], word, wlen) == 0) total++;
    if (total == 0) return NULL;

    char **results = malloc(total * sizeof(char *));
    if (!results) return NULL;
    int count = 0;
    for (int i = 0; subcmds[i] && count < total; i++)
        if (wlen == 0 || strncmp(subcmds[i], word, wlen) == 0)
            results[count++] = strdup(subcmds[i]);

    *count_out = count;
    return results;
}
static int ssh_has_dup(char **arr, int count, const char *h) {
    for (int i = 0; i < count; i++)
        if (arr[i] && strcmp(arr[i], h) == 0) return 1;
    return 0;
}

static int ssh_is_dup(char **arr, int count, const char *h) {
    if (!arr || !h) return 0;
    for (int i = 0; i < count; i++)
        if (arr[i] && strcmp(arr[i], h) == 0) return 1;
    return 0;
}

char **get_dynamic_completions(const char *cmdline, int cursor_pos,
                                int *count_out) {
    *count_out = 0;
    if (!cmdline || !*cmdline) return NULL;

    /* ── parse cmdline → words[] ─────────────────────────────── */
    char line[4096];
    strncpy(line, cmdline, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    char *words[32];
    int   nwords = 0;
    memset(words, 0, sizeof(words));

    char *p = line;




    while (*p && nwords < 31) {
        while (*p == ' ') p++;
        for (int _i = 0; _i < nwords; _i++)
            if (!words[_i]) { *count_out = 0; return NULL; }
        if (!*p) break;
        words[nwords++] = p;
        if (nwords == 0 || !words[0]) return NULL;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }

    if (nwords == 0 || !words[0]) return NULL;

    /* ── current word + effective_words ─────────────────────── */
    int ends_with_space = (cmdline[strlen(cmdline) - 1] == ' ');
    const char *current_word = ends_with_space ? ""
                             : (nwords > 0 ? words[nwords - 1] : "");
    int effective_words = ends_with_space ? nwords : nwords - 1;

    /* ── NULL-safe word accessor ─────────────────────────────── */
#define W(i) ((i) < nwords && words[(i)] ? words[(i)] : "")

    /* ── cd frecency ─────────────────────────────────────────── */
    if (strcmp(W(0), "cd") == 0) {
        extern char **cd_frecency_list(const char *q, int lim, int *n);
        int fcount = 0;
        char **flist = cd_frecency_list(
            (current_word && *current_word) ? current_word : "",
            16, &fcount);
        if (flist && fcount > 0) { *count_out = fcount; return flist; }
        free(flist);
        return NULL;
    }

    if (effective_words == 0) return NULL;
    /* ── process name completion ─────────────────────────────── */
    if (strcmp(W(0), "pkill") == 0 || strcmp(W(0), "killall") == 0 ||
        strcmp(W(0), "renice") == 0) {
        char cmd[512];
        if (current_word && *current_word)
            snprintf(cmd, sizeof(cmd),
                "ps -eo comm= 2>/dev/null | sort -u | grep '^%s'",
                current_word);
        else
            snprintf(cmd, sizeof(cmd),
                "ps -eo comm= 2>/dev/null | sort -u | head -20");
        return run_and_collect(cmd, count_out);
    }

    if (strcmp(W(0), "kill") == 0) {
        char cmd[512];
        if (current_word && *current_word)
            snprintf(cmd, sizeof(cmd),
                "ps -eo pid=,comm= 2>/dev/null | grep '%s'",
                current_word);
        else
            snprintf(cmd, sizeof(cmd),
                "ps -eo pid=,comm= 2>/dev/null | head -20");
        return run_and_collect(cmd, count_out);
    }

    /* ── systemctl service completion ───────────────────────── */
    if (strcmp(W(0), "systemctl") == 0 && effective_words >= 1) {
        const char *sub = W(1);
        if (*sub && (strcmp(sub, "start")   == 0 ||
                     strcmp(sub, "stop")    == 0 ||
                     strcmp(sub, "restart") == 0 ||
                     strcmp(sub, "status")  == 0 ||
                     strcmp(sub, "enable")  == 0 ||
                     strcmp(sub, "disable") == 0 ||
                     strcmp(sub, "reload")  == 0)) {
            char cmd[512];
            if (current_word && *current_word)
                snprintf(cmd, sizeof(cmd),
                    "systemctl list-units --all --no-legend 2>/dev/null"
                    " | awk '{print $1}' | grep '^%s'", current_word);
            else
                snprintf(cmd, sizeof(cmd),
                    "systemctl list-units --all --no-legend 2>/dev/null"
                    " | awk '{print $1}' | head -20");
            return run_and_collect(cmd, count_out);
        }
    }

    /* ── ssh hostname + key completion ──────────────────────── */
    if (strcmp(W(0), "ssh") == 0) {
        if (effective_words >= 1 && strcmp(W(effective_words - 1), "-i") == 0) {
            char cmd[512];
            if (current_word && *current_word)
                snprintf(cmd, sizeof(cmd),
                    "ls ~/.ssh/ 2>/dev/null | grep -v '\\.pub$'"
                    " | grep '^%s'", current_word);
            else
                snprintf(cmd, sizeof(cmd),
                    "ls ~/.ssh/ 2>/dev/null | grep -v '\\.pub$'");
            return run_and_collect(cmd, count_out);
        }

        if (effective_words >= 1) {
            const char *prev = W(effective_words - 1);
            if (strcmp(prev, "-p") == 0 || strcmp(prev, "-l") == 0 ||
                strcmp(prev, "-o") == 0 || strcmp(prev, "-b") == 0 ||
                strcmp(prev, "-c") == 0 || strcmp(prev, "-e") == 0 ||
                strcmp(prev, "-w") == 0)
                return NULL;
        }

        int host_seen = 0;
        for (int wi = 1; wi < effective_words; wi++) {
            if (!words[wi]) continue;
            if (words[wi][0] != '-') { host_seen = 1; break; }
            if (strcmp(words[wi], "-p") == 0 || strcmp(words[wi], "-l") == 0 ||
                strcmp(words[wi], "-i") == 0 || strcmp(words[wi], "-o") == 0 ||
                strcmp(words[wi], "-b") == 0 || strcmp(words[wi], "-c") == 0 ||
                strcmp(words[wi], "-e") == 0 || strcmp(words[wi], "-w") == 0)
                wi++;
        }
        if (host_seen) return NULL;

        char **all = malloc(256 * sizeof(char *));
        if (!all) return NULL;
        int total = 0;

        {
            char config_sources[2048];
            strncpy(config_sources, "~/.ssh/config",
                    sizeof(config_sources) - 1);
            FILE *gl = popen("ls ~/.ssh/config.d/*.conf 2>/dev/null", "r");
            if (gl) {
                char cline[512];
                while (fgets(cline, sizeof(cline), gl)) {
                    int cl = strlen(cline);
                    while (cl > 0 && (cline[cl-1]=='\n'||cline[cl-1]=='\r'))
                        cline[--cl] = '\0';
                    if (cl > 0) {
                        strncat(config_sources, " ",
                                sizeof(config_sources)
                                - strlen(config_sources) - 1);
                        strncat(config_sources, cline,
                                sizeof(config_sources)
                                - strlen(config_sources) - 1);
                    }
                }
                pclose(gl);
            }

            char cmd[4096];
            snprintf(cmd, sizeof(cmd),
                "grep -h '^[[:space:]]*Host[[:space:]]'"
                " %s 2>/dev/null"
                " | awk '{for(i=2;i<=NF;i++) print $i}'"
                " | grep -v '[*?]'",
                config_sources);

            int ccount = 0;
            char **clist = run_and_collect(cmd, &ccount);
            for (int ci = 0; ci < ccount && total < 240; ci++) {
                if (!clist[ci]) continue;
                if (current_word && *current_word &&
                    strncmp(clist[ci], current_word,
                            strlen(current_word)) != 0) {
                    free(clist[ci]); continue;
                }
                if (!ssh_is_dup(all, total, clist[ci]))
                    all[total++] = clist[ci];
                else
                    free(clist[ci]);
            }
            free(clist);
        }

        /* known_hosts */
        {
            char cmd[512];
            if (current_word && *current_word)
                snprintf(cmd, sizeof(cmd),
                    "awk '!/^@/ && !/^#/ {"
                    " n=split($1,a,\",\");"
                    " for(i=1;i<=n;i++) if(a[i]!~/^\\|/) print a[i]}'"
                    " ~/.ssh/known_hosts 2>/dev/null | grep '^%s'",
                    current_word);
            else
                snprintf(cmd, sizeof(cmd),
                    "awk '!/^@/ && !/^#/ {"
                    " n=split($1,a,\",\");"
                    " for(i=1;i<=n;i++) if(a[i]!~/^\\|/) print a[i]}'"
                    " ~/.ssh/known_hosts 2>/dev/null");

            int kcount = 0;
            char **klist = run_and_collect(cmd, &kcount);
            for (int ki = 0; ki < kcount && total < 255; ki++) {
                if (!klist[ki]) continue;
                if (!ssh_is_dup(all, total, klist[ki]))
                    all[total++] = klist[ki];
                else
                    free(klist[ki]);
            }
            free(klist);
        }

        if (total == 0) { free(all); return NULL; }
        *count_out = total;
        return all;
    }

    /* ── man page completion ─────────────────────────────────── */
    if (strcmp(W(0), "man") == 0 && current_word && *current_word) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "man -k '' 2>/dev/null | awk '{print $1}'"
            " | grep '^%s' | head -20", current_word);
        return run_and_collect(cmd, count_out);
    }

    /* ── env variable completion ─────────────────────────────── */
    if (strcmp(W(0), "export") == 0 || strcmp(W(0), "unset") == 0) {
        char cmd[512];
        if (current_word && *current_word)
            snprintf(cmd, sizeof(cmd),
                "env 2>/dev/null | cut -d= -f1 | grep '^%s'",
                current_word);
        else
            snprintf(cmd, sizeof(cmd),
                "env 2>/dev/null | cut -d= -f1 | head -20");
        return run_and_collect(cmd, count_out);
    }

    /* ── git subcommand completions ──────────────────────────── */
    if (strcmp(W(0), "git") != 0) return NULL;

    if (effective_words == 1)
        return get_subcommands("git", current_word, count_out);

    const char *subcmd = W(1);
    const char *prefix = current_word ? current_word : "";

    if (strcmp(subcmd, "add") == 0)
        return get_git_changed_files(prefix, count_out);

    if (strcmp(subcmd, "checkout") == 0 || strcmp(subcmd, "switch")  == 0 ||
        strcmp(subcmd, "merge")    == 0 || strcmp(subcmd, "rebase")  == 0)
        return get_git_branches(prefix, count_out);

    if (strcmp(subcmd, "branch") == 0 && effective_words >= 2) {
        for (int i = 2; i < effective_words; i++) {
            if (!words[i]) continue;
            if (strcmp(words[i], "-d") == 0 || strcmp(words[i], "-D") == 0 ||
                strcmp(words[i], "-m") == 0)
                return get_git_branches(prefix, count_out);
        }
    }

    if (strcmp(subcmd, "push")  == 0 || strcmp(subcmd, "pull")  == 0 ||
        strcmp(subcmd, "fetch") == 0) {
        if (effective_words == 2) return get_git_remotes(prefix, count_out);
        if (effective_words == 3) return get_git_branches(prefix, count_out);
    }

    if (strcmp(subcmd, "stash") == 0 && effective_words >= 2) {
        const char *stash_sub = W(2);
        if (strcmp(stash_sub, "pop")   == 0 || strcmp(stash_sub, "drop")  == 0 ||
            strcmp(stash_sub, "show")  == 0 || strcmp(stash_sub, "apply") == 0)
            return get_git_stashes(prefix, count_out);

        static const char *stash_cmds[] = {
            "push","pop","list","show","drop","clear",
            "apply","branch","create","store", NULL
        };
        int wlen = stash_sub ? strlen(stash_sub) : 0;
        int total2 = 0;
        for (int i = 0; stash_cmds[i]; i++)
            if (!wlen || strncmp(stash_cmds[i], stash_sub, wlen) == 0)
                total2++;
        if (total2 > 0) {
            char **r = malloc(total2 * sizeof(char *));
            if (!r) return NULL;
            int c = 0;
            for (int i = 0; stash_cmds[i] && c < total2; i++)
                if (!wlen || strncmp(stash_cmds[i], stash_sub, wlen) == 0)
                    r[c++] = strdup(stash_cmds[i]);
            *count_out = c;
            return r;
        }
    }

#undef W
    return NULL;
}
