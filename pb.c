/* pb - Progress Bar. Copyright Dave Eddy and contributors. MIT License. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <langinfo.h>
#include <linux/fs.h>
#include <wchar.h>
#include <sys/prctl.h>
#include <sys/file.h>
#include <sys/sysmacros.h>
#include <math.h>
#include <poll.h>
#include <regex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { PROGRESS=1, TIMER=2, ETA=4, FINETA=8, RATE=16, AVERAGE=32,
       BYTES=64, BUFFER=128, LAST=256 };
enum {
    CHANGE_DISPLAY=1ULL<<0, CHANGE_FORMAT=1ULL<<1, CHANGE_NAME=1ULL<<2,
    CHANGE_SIZE=1ULL<<3, CHANGE_LIMIT=1ULL<<4, CHANGE_BUFFER=1ULL<<5,
    CHANGE_INTERVAL=1ULL<<6, CHANGE_WIDTH=1ULL<<7, CHANGE_HEIGHT=1ULL<<8,
    CHANGE_LAST=1ULL<<9, CHANGE_BITS=1ULL<<10, CHANGE_SI=1ULL<<11,
    CHANGE_WINDOW=1ULL<<12, CHANGE_STYLE=1ULL<<13, CHANGE_GAUGE=1ULL<<14, CHANGE_NO_SPLICE=1ULL<<15, CHANGE_EXTRA=1ULL<<16
};
struct extra_display { bool window, process; char *format; };
static struct extra_display *extra_displays;
static size_t extra_count;
static bool utf8_output, conemu_active, proxy_progress;
static unsigned animation_step;
static size_t terminal_row_offset;
static char *title_region;
static size_t title_capacity;
static char **saved_arguments;
static int saved_argument_count;
static unsigned char *current_line, *previous_line;
static size_t current_line_len, previous_line_len;
static unsigned long long changes;
static pid_t remote_pid, query_pid;
static const char **watch_specs;
static size_t watch_count;
static bool transfer_option;
static int monitor_side=-1, monitor_index;
static char **monitor_command;
static const char *monitor_names[2], *monitor_formats[2], *monitor_pidfiles[2];
static unsigned monitor_name_count, monitor_format_count, monitor_pidfile_count;
static pid_t monitor_command_pid=-1, monitor_output_pid=-1;
static bool monitor_command_done, monitor_output_done, monitor_output_child;
static bool monitor_mutex_ready, monitor_output_failed, monitor_terminating;
static bool monitor_command_group;
static atomic_bool cancelled;
struct monitor_state {
    atomic_ullong bytes[2];
    atomic_llong paused;
    atomic_bool suspended;
    pid_t coordinator;
    pthread_mutex_t display_lock;
};
static struct monitor_state *monitor_state;
static _Atomic double live_limit;
static atomic_ullong live_buffer, live_size;
static atomic_bool live_size_set;
static atomic_size_t live_last;
static struct {
    unsigned display;
    bool numeric, quiet, bits, si, wait, gauge, lines, null, force;
    bool stop, sync, direct, sparse, discard, stats, size_set, large, cursor, conemu, no_splice;
    size_t width, height;
    int skip_errors;
    unsigned long long size, buffer, skip_block, pipe_size;
    double limit;
    size_t last;
    double interval, delay, window;
    const char *name, *format, *output, *pidfile, *style, *store;
} opt = { .buffer=128*1024, .interval=1, .window=30, .last=16,
          .style="plain" };
static atomic_bool done;
static atomic_ullong copied, units, buffered;
static atomic_int result;
static atomic_bool forwarding;
static atomic_ullong stored_units;
static int store_fd=-1;
static bool output_failed;
static bool pidfile_created;
static volatile sig_atomic_t interrupted, resized;
static int input_count, output_fd=STDOUT_FILENO;
static char **inputs;
static double started;
static pthread_mutex_t history_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t stage_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *last_bytes;
static size_t last_len;
static size_t g_columns=80;
static const char *GREEN="\033[32m", *DIM="\033[2m", *RST="\033[0m";
#include "pb_ui.h"

#include "pb_clock.h"

static void on_signal(int sig) {
    if (sig==SIGWINCH) resized=1;
    else if (sig==SIGTSTP || sig==SIGTTIN || sig==SIGTTOU) suspend_requested=sig;
    else if (sig==SIGCONT) { continued=1; suspend_requested=0; }
    else interrupted=sig;
}

static void error_path(const char *path, int code) {
    int saved=errno;
    fprintf(stderr, "pb: %s: %s\n", path, strerror(saved));
    atomic_fetch_or(&result, code);
}

static void usage(FILE *f) {
    fputs("Usage: pb [OPTION]... [FILE]...\n"
          "Concatenate files (or stdin) to stdout with progress monitoring.\n\n"
          "Display: -p --progress, -t --timer, -e --eta, -I --fineta,\n"
          "  -r --rate, -a --average-rate, -b --bytes, -T --buffer-percent,\n"
          "  -A --last-written NUM, -F --format FORMAT, -n --numeric, -q --quiet\n"
          "Modifiers: -8 --bits, -k --si, -W --wait, -D --delay-start SEC,\n"
          "  -s --size SIZE, -g --gauge, -l --line-mode, -0 --null,\n"
          "  -i --interval SEC, -m --average-rate-window SEC, -w --width WIDTH,\n"
          "  -H --height HEIGHT, -N --name NAME, -u --bar-style STYLE,\n"
          "  -v --stats, -f --force, -c --cursor, -9 --conemu,\n"
          "  -x --extra-display SPEC, --large\n"
          "Transfer: -o --output FILE, -L --rate-limit RATE, -B --buffer-size SIZE,\n"
          "  -C --no-splice, -J --pipe-buffer-size SIZE, -E --skip-errors,\n"
          "  -Z --error-skip-block SIZE, -S --stop-at-size, -Y --sync,\n"
          "  -K --direct-io, -O --sparse, -X --discard, -U --store-and-forward FILE\n"
          "Process: -d --watchfd PID[:FD]|=NAME|@LISTFILE,\n"
          "  -R --remote PID, -Q --query PID, -M --monitor in|out|both -- COMMAND\n"
          "Other: -P --pidfile FILE, -h --help, -V --version\n\n"
          "SIZE accepts K, M, G, T suffixes (1024; 1000 after --si).\n"
          "The original large display is used on terminals unless display switches\n"
          "select a compact display. Use --large to explicitly select it.\n", f);
}

static void bad_value(const char *value) {
    fprintf(stderr, "pb: invalid value: %s\n", value);
    exit(64);
}

static long double quantity(const char *s, bool suffix, bool decimal) {
    if (!s[0] || !isdigit((unsigned char)s[0])) bad_value(s);
    char *end;
    errno=0;
    long double n=strtold(s, &end);
    for (const char *p=s;p<end;p++) {
        if (!isdigit((unsigned char)*p) && !(*p=='.' && (suffix || decimal))) bad_value(s);
    }
    if (errno || !isfinite(n) || n < 0) bad_value(s);
    if (*end && suffix) {
        const char *p=strchr("KMGTPE", toupper((unsigned char)*end));
        if (!p) bad_value(s);
        int power=(int)(p-"KMGTPE")+1;
        for (int i=0; i<power; i++) n *= opt.si ? 1000 : 1024;
        end++;
        if (*end=='i') end++;
        if (*end=='B' || *end=='b') end++;
    } else if (n!=floorl(n) && !decimal) bad_value(s);
    if (*end || n > (long double)ULLONG_MAX) bad_value(s);
    return n;
}

static unsigned long long number(const char *s, bool suffix, bool decimal) {
    if (!suffix && !decimal) {
        if (!isdigit((unsigned char)s[0])) bad_value(s);
        char *end;
        errno=0;
        unsigned long long n=strtoull(s,&end,10);
        if (errno || *end) bad_value(s);
        return n;
    }
    return (unsigned long long)quantity(s,suffix,decimal);
}

static double seconds(const char *s, bool zero) {
    char *end;
    errno=0;
    double n=strtod(s, &end);
    for (const char *p=s;p<end;p++) if (!isdigit((unsigned char)*p) && *p!='.') bad_value(s);
    if (!s[0] || *end || errno || !isfinite(n) || n<0 || (!zero && n==0)) bad_value(s);
    return n;
}

static bool fd_size(int fd, unsigned long long *size);

static unsigned long long path_size_walk(const char *path, bool follow) {
    struct stat st;
    if (follow ? stat(path,&st) : lstat(path,&st)) { error_path(path,2); exit(2); }
    unsigned long long size=S_ISREG(st.st_mode) ? (unsigned long long)st.st_size : 0;
    if (follow && S_ISBLK(st.st_mode)) {
        int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if (fd<0 || !fd_size(fd,&size)) { error_path(path,2); if (fd>=0) close(fd); exit(2); }
        close(fd);
    }
    if (!S_ISDIR(st.st_mode)) return size;
    DIR *dir=opendir(path);
    if (!dir) { error_path(path, 2); exit(2); }
    struct dirent *ent;
    while ((ent=readdir(dir))) {
        if (!strcmp(ent->d_name,".") || !strcmp(ent->d_name,"..")) continue;
        char *child;
        if (asprintf(&child, "%s/%s", path, ent->d_name)<0) exit(64);
        unsigned long long n=path_size_walk(child,false);
        free(child);
        if (ULLONG_MAX-size<n) bad_value(path);
        size+=n;
    }
    closedir(dir);
    return size;
}

static unsigned long long path_size(const char *path) {
    return path_size_walk(path,true);
}

static void add_watch_spec(const char *spec) {
    const char **replacement=realloc(watch_specs,(watch_count+1)*sizeof *watch_specs);
    if (!replacement) exit(64);
    watch_specs=replacement;
    watch_specs[watch_count++]=spec;
}

static bool parse_extra_display(const char *spec, struct extra_display *display) {
    memset(display,0,sizeof *display);
    char *destinations=strdup(spec);
    if (!destinations) return false;
    char *colon=strchr(destinations,':');
    if (colon) {
        *colon++=0; display->format=strdup(colon);
        if (!display->format) { free(destinations); return false; }
    }
    char *save=NULL;
    bool valid=true;
    for (char *word=strtok_r(destinations,",",&save);word;word=strtok_r(NULL,",",&save)) {
        if (!strcmp(word,"window") || !strcmp(word,"windowtitle")) display->window=true;
        else if (!strcmp(word,"process") || !strcmp(word,"processtitle") || !strcmp(word,"proctitle") || !strcmp(word,"proc")) display->process=true;
        else valid=false;
    }
    free(destinations);
    if (!valid || (!display->window && !display->process)) {
        free(display->format); display->format=NULL; errno=EINVAL; return false;
    }
    return true;
}

static void free_extra_displays(struct extra_display *displays, size_t count) {
    for (size_t i=0;i<count;i++) free(displays[i].format);
    free(displays);
}

static void add_extra_display(const char *spec) {
    struct extra_display display;
    if (!parse_extra_display(spec,&display)) bad_value(spec);
    struct extra_display *replacement=realloc(extra_displays,(extra_count+1)*sizeof *extra_displays);
    if (!replacement) exit(64);
    extra_displays=replacement; extra_displays[extra_count++]=display;
}

static void release_arguments(void) {
    for (int i=0;i<saved_argument_count;i++) free(saved_arguments[i]);
    free(saved_arguments);
    free_extra_displays(extra_displays,extra_count);
}

/* Keep option strings independent from the original argv area used by ps(1). */
static char **preserve_arguments(int argc, char **argv) {
    title_region=argv[0];
    char *end=argv[0]+strlen(argv[0])+1;
    for (int i=1;i<argc && argv[i]==end;i++) end=argv[i]+strlen(argv[i])+1;
    title_capacity=(size_t)(end-title_region);
    saved_arguments=calloc((size_t)argc+1,sizeof *saved_arguments);
    if (!saved_arguments) exit(64);
    saved_argument_count=argc;
    for (int i=0;i<argc;i++) {
        saved_arguments[i]=strdup(argv[i]);
        if (!saved_arguments[i]) exit(64);
    }
    atexit(release_arguments);
    return saved_arguments;
}

static void arguments(int argc, char **argv) {
    static const struct option options[] = {
        {"progress",0,0,'p'}, {"timer",0,0,'t'}, {"eta",0,0,'e'},
        {"fineta",0,0,'I'}, {"rate",0,0,'r'}, {"average-rate",0,0,'a'},
        {"bytes",0,0,'b'}, {"buffer-percent",0,0,'T'}, {"last-written",1,0,'A'},
        {"format",1,0,'F'}, {"numeric",0,0,'n'}, {"quiet",0,0,'q'},
        {"bits",0,0,'8'}, {"si",0,0,'k'}, {"wait",0,0,'W'},
        {"delay-start",1,0,'D'}, {"size",1,0,'s'}, {"gauge",0,0,'g'},
        {"line-mode",0,0,'l'}, {"null",0,0,'0'}, {"interval",1,0,'i'},
        {"average-rate-window",1,0,'m'}, {"width",1,0,'w'}, {"height",1,0,'H'},
        {"name",1,0,'N'}, {"bar-style",1,0,'u'}, {"stats",0,0,'v'},
        {"cursor",0,0,'c'}, {"conemu",0,0,'9'}, {"extra-display",1,0,'x'}, {"force",0,0,'f'}, {"output",1,0,'o'},
        {"rate-limit",1,0,'L'}, {"buffer-size",1,0,'B'}, {"no-splice",0,0,'C'},
        {"pipe-buffer-size",1,0,'J'}, {"skip-errors",0,0,'E'},
        {"error-skip-block",1,0,'Z'}, {"stop-at-size",0,0,'S'}, {"sync",0,0,'Y'},
        {"direct-io",0,0,'K'}, {"sparse",0,0,'O'}, {"discard",0,0,'X'},
        {"store-and-forward",1,0,'U'}, {"pidfile",1,0,'P'}, {"help",0,0,'h'}, {"version",0,0,'V'},
        {"monitor",1,0,'M'}, {"watchfd",1,0,'d'}, {"remote",1,0,'R'}, {"query",1,0,'Q'}, {"large",0,0,256}, {0,0,0,0}
    };
    int c;
    while ((c=getopt_long(argc,argv,"pteIrabTA:F:nq8kWD:s:gl0i:m:w:H:N:u:vfc9x:o:L:B:CJ:EZ:SYKOXU:P:R:Q:d:M:hV",options,NULL))!=-1) {
        if (c>0 && c<256 && strchr("oLBCJEZSYKOXU",c)) transfer_option=true;
        switch (c) {
        case 'p': case 't': case 'e': case 'I': case 'r': case 'a':
        case 'b': case 'T': changes|=CHANGE_DISPLAY; break;
        case 'A': changes|=CHANGE_LAST|CHANGE_DISPLAY; break;
        case 'F': changes|=CHANGE_FORMAT; break;
        case 'N': changes|=CHANGE_NAME; break;
        case 's': changes|=CHANGE_SIZE; break;
        case 'L': changes|=CHANGE_LIMIT; break;
        case 'B': changes|=CHANGE_BUFFER; break;
        case 'i': changes|=CHANGE_INTERVAL; break;
        case 'w': changes|=CHANGE_WIDTH; break;
        case 'H': changes|=CHANGE_HEIGHT; break;
        case '8': changes|=CHANGE_BITS; break;
        case 'k': changes|=CHANGE_SI; break;
        case 'm': changes|=CHANGE_WINDOW; break;
        case 'u': changes|=CHANGE_STYLE; break;
        case 'g': changes|=CHANGE_GAUGE; break;
        case 'C': changes|=CHANGE_NO_SPLICE; break;
        case 'x': changes|=CHANGE_EXTRA; break;
        }
        switch (c) {
        case 'p': opt.display|=PROGRESS; break;
        case 't': opt.display|=TIMER; break;
        case 'e': opt.display|=ETA; break;
        case 'I': opt.display|=FINETA; break;
        case 'r': opt.display|=RATE; break;
        case 'a': opt.display|=AVERAGE; break;
        case 'b': opt.display|=BYTES; break;
        case 'T': opt.display|=BUFFER; opt.no_splice=true; break;
        case 'A': {
            unsigned long long n=number(optarg,false,false);
            if (n>SIZE_MAX/2) bad_value(optarg);
            opt.last=(size_t)n; opt.display|=LAST; opt.no_splice=true; break;
        }
        case 'F': if (monitor_format_count<2) monitor_formats[monitor_format_count++]=optarg; opt.format=optarg; break;
        case 'n': opt.numeric=true; break;
        case 'q': opt.quiet=true; break;
        case '8': opt.bits=true; break;
        case 'k': opt.si=true; break;
        case 'W': opt.wait=true; break;
        case 'D': opt.delay=seconds(optarg,true); break;
        case 's': opt.size=optarg[0]=='@' ? path_size(optarg+1) : number(optarg,true,false); opt.size_set=true; break;
        case 'g': opt.gauge=true; break;
        case 'l': opt.lines=true; break;
        case '0': opt.lines=opt.null=true; break;
        case 'i': opt.interval=seconds(optarg,false); break;
        case 'm': opt.window=number(optarg,false,false); if (!opt.window) bad_value(optarg); break;
        case 'w': case 'H': {
            unsigned long long n=number(optarg,false,false);
            if (!n || n>SIZE_MAX/2) bad_value(optarg);
            if (c=='w') opt.width=(size_t)n; else opt.height=(size_t)n;
            break;
        }
        case 'N': if (monitor_name_count<2) monitor_names[monitor_name_count++]=optarg; opt.name=optarg; break;
        case 'u': opt.style=optarg; if (strcmp(optarg,"plain") && strcmp(optarg,"block") && strcmp(optarg,"granular") && strcmp(optarg,"shaded")) bad_value(optarg); break;
        case 'v': opt.stats=true; break;
        case 'f': opt.force=true; break;
        case 'c': opt.cursor=true; break;
        case '9': opt.conemu=true; break;
        case 'x': add_extra_display(optarg); break;
        case 'o': opt.output=optarg; break;
        case 'L': opt.limit=(double)quantity(optarg,true,true); break;
        case 'B': opt.no_splice=true; opt.buffer=number(optarg,true,false); if (!opt.buffer || opt.buffer>SIZE_MAX/2) bad_value(optarg); break;
        case 'C': opt.no_splice=true; break;
        case 'J': opt.pipe_size=number(optarg,true,false); if (opt.pipe_size>INT_MAX) bad_value(optarg); break;
        case 'E': opt.skip_errors++; break;
        case 'Z': opt.skip_block=number(optarg,true,false); if (!opt.skip_block) bad_value(optarg); break;
        case 'S': opt.stop=true; break;
        case 'Y': opt.sync=true; break;
        case 'K': opt.direct=true; break;
        case 'O': opt.sparse=true; break;
        case 'X': opt.discard=true; break;
        case 'U': opt.store=optarg; break;
        case 'P': if (monitor_pidfile_count<2) monitor_pidfiles[monitor_pidfile_count++]=optarg; opt.pidfile=optarg; break;
        case 'M':
            if (!strcmp(optarg,"in") || !strcmp(optarg,"0")) monitor_side=0;
            else if (!strcmp(optarg,"out") || !strcmp(optarg,"1")) monitor_side=1;
            else if (!strcmp(optarg,"both") || !strcmp(optarg,"2")) monitor_side=2;
            else bad_value(optarg);
            break;
        case 'd': add_watch_spec(optarg); break;
        case 'R': case 'Q': {
            unsigned long long n=number(optarg,false,false);
            if (!n || n>INT_MAX) bad_value(optarg);
            if (c=='R') remote_pid=(pid_t)n; else query_pid=(pid_t)n;
            break;
        }
        case 256: opt.large=true; break;
        case 'h': usage(stdout); exit(0);
        case 'V': puts("pb 0.2.0\nMIT License. Copyright Dave Eddy and contributors."); exit(0);
        default: usage(stderr); exit(64);
        }
    }
    if (watch_count && (remote_pid || query_pid || transfer_option || opt.lines || monitor_side>=0)) {
        fputs("pb: cannot combine watch mode with remote/query, line mode, or transfer modifiers\n",stderr); exit(64);
    }
    if (remote_pid && query_pid) {
        fputs("pb: --remote and --query cannot be combined\n",stderr); exit(64);
    }
    if (opt.skip_block && !opt.skip_errors) {
        fputs("pb: --error-skip-block requires --skip-errors\n",stderr); exit(64);
    }
    if (!opt.display && !opt.format && !opt.numeric && !(changes&CHANGE_STYLE)) opt.large=true;
    if (!opt.display) opt.display=opt.numeric ? PROGRESS : PROGRESS|TIMER|ETA|RATE|BYTES;
    if (opt.numeric || opt.format || opt.cursor) opt.large=false;
    input_count=argc-optind;
    inputs=argv+optind;
    if ((remote_pid || query_pid) && input_count) {
        fputs("pb: files cannot be specified with --remote or --query\n",stderr); exit(64);
    }
    if (monitor_side>=0) {
        if (!input_count || opt.store || remote_pid || query_pid) {
            fputs("pb: --monitor requires a command and cannot be combined with store/remote/query modes\n",stderr); exit(64);
        }
        monitor_command=inputs;
        input_count=0; inputs=NULL;
        opt.pidfile=monitor_pidfiles[0];
        if (monitor_side==2) {
            opt.name=monitor_names[0] ? monitor_names[0] : "in";
            opt.format=monitor_formats[0];
            opt.large=false;
        }
    }
}

static bool same_file(int a, int b) {
    struct stat x,y;
    return !fstat(a,&x) && !fstat(b,&y) && (S_ISREG(x.st_mode) || S_ISBLK(x.st_mode)) &&
           x.st_dev==y.st_dev && x.st_ino==y.st_ino;
}

#include "pb_io.h"

/* Compute totals without consuming pipes or changing stdin's file offset. */
static void infer_size(void) {
    if (opt.size_set) return;
    unsigned long long total=0;
    bool seen_stdin=false;
    for (int i=0;i<(input_count ? input_count : 1);i++) {
        const char *path=input_count ? inputs[i] : "-";
        bool standard=!strcmp(path,"-");
        if (standard && seen_stdin) continue;
        if (standard) seen_stdin=true;
        int fd=standard ? STDIN_FILENO : open(path,O_RDONLY|O_NONBLOCK);
        struct stat st;
        if (fd<0) return;
        unsigned long long file_size=0;
        bool valid=!fstat(fd,&st) && fd_size(fd,&file_size) && (!opt.lines || S_ISREG(st.st_mode));
        off_t pos=lseek(fd,0,SEEK_CUR);
        if (!valid || pos<0) { if (!standard) close(fd); return; }
        unsigned long long n=file_size>(unsigned long long)pos ? file_size-(unsigned long long)pos : 0;
        if (opt.lines) {
            unsigned char b[32768];
            off_t offset=pos;
            ssize_t len;
            n=0;
            while ((len=pread(fd,b,sizeof b,offset))>0) {
                for (ssize_t j=0;j<len;j++) if (b[j]==(opt.null ? 0 : '\n')) n++;
                offset+=len;
            }
            if (len<0) valid=false;
        }
        if (!standard) close(fd);
        if (!valid || ULLONG_MAX-total<n) return;
        total+=n;
    }
    opt.size=total;
    opt.size_set=true;
}

static bool ready(int fd, short events) {
    struct pollfd p={.fd=fd,.events=events};
    while (!interrupted && !atomic_load(&cancelled)) {
        int n=poll(&p,1,100);
        if (n>0) return true;
        if (n<0 && errno!=EINTR) return false;
    }
    errno=EINTR;
    return false;
}

#include "pb_history.h"

static unsigned long long record(const unsigned char *buf, size_t n) {
    size_t capture=atomic_load(&live_last);
    pthread_mutex_lock(&history_lock);
    bool history_ok=reserve_last(capture);
    if (history_ok && capture) append_last(buf,n);
    if (history_ok) history_ok=append_lines(buf,n,atomic_load(&live_line));
    pthread_mutex_unlock(&history_lock);
    if (!history_ok) { error_path("history allocation",64); atomic_store(&cancelled,true); }
    unsigned long long count=n;
    if (opt.lines) {
        count=0;
        for (size_t i=0;i<n;i++) if (buf[i]==(opt.null ? 0 : '\n')) count++;
    }
    note_first_write();
    if (monitor_state) atomic_fetch_add(&monitor_state->bytes[monitor_index],n);
    atomic_fetch_add(&copied,n);
    atomic_fetch_add(&units,count);
    return count;
}

static void throttle(double origin, unsigned long long amount, double limit) {
    if (!limit) return;
    double until=origin+(double)amount/limit;
    while (!interrupted && !atomic_load(&cancelled)) {
        double current=atomic_load(&live_limit);
        if (current!=limit) return;
        double left=until-wall_now();
        if (left<=0) break;
        if (left>0.05) left=0.05;
        struct timespec delay={.tv_sec=0,.tv_nsec=(long)(left*1e9)};
        nanosleep(&delay,NULL);
    }
}

static bool all_zero(const unsigned char *p, size_t n) {
    for (size_t i=0;i<n;i++) if (p[i]) return false;
    return true;
}

static bool transfer(int fd, const char *path, int out, bool discard) {
    void *allocation=NULL;
    size_t capacity=atomic_load(&live_buffer);
    int err=posix_memalign(&allocation,4096,capacity);
    if (err) { errno=err; error_path("buffer allocation",64); return false; }
    unsigned char *buf=allocation;
    double origin=wall_now();
    unsigned long long moved=0;
    double previous_limit=atomic_load(&live_limit);
    unsigned errors=0;
    bool reported_error=false;
    bool success=true, sparse_tail=false;
    struct stat outstat={0};
    bool regular=!fstat(out,&outstat) && S_ISREG(outstat.st_mode);
    bool syncable=regular || S_ISBLK(outstat.st_mode);
    struct fast_transfer fast=fast_init(fd,out,discard);
    while (!interrupted && !atomic_load(&cancelled)) {
        size_t requested=atomic_load(&live_buffer);
        if (requested!=capacity) {
            void *replacement=NULL;
            int allocation_error=posix_memalign(&replacement,4096,requested);
            if (allocation_error) { errno=allocation_error; error_path("buffer allocation",64); success=false; break; }
            free(buf); buf=replacement; capacity=requested;
        }
        size_t want=capacity;
        double limit=atomic_load(&live_limit);
        if (limit!=previous_limit) { origin=wall_now(); moved=0; previous_limit=limit; }
        unsigned long long total=atomic_load(&live_size);
        bool known=atomic_load(&live_size_set);
        unsigned long long count=atomic_load(&units);
        if (opt.stop && known && count>=total) break;
        if (opt.stop && known && !opt.lines && total-count<want) want=total-count;
        if (limit) {
            unsigned long long chunk=(unsigned long long)(limit/20);
            if (!chunk) chunk=1;
            if (want>chunk) want=chunk;
        }
        struct fast_result step=fast_step(&fast,fd,out,buf,want);
        if (step.action==FAST_MOVED) {
            if (!moved) origin=wall_now();
            record_fast((size_t)step.count);
            moved+=(unsigned long long)step.count;
            if (opt.sync && syncable && fdatasync(out)) { error_path("fdatasync",16); output_failed=true; success=false; break; }
            throttle(origin,moved,limit);
            continue;
        }
        if (step.action==FAST_RETRY) continue;
        if (step.action==FAST_EOF) break;
        if (step.action==FAST_ERROR) {
            if (interrupted || atomic_load(&cancelled)) { success=false; break; }
            if (errno==EPIPE && step.output_error) {
                atomic_store(&cancelled,true); success=false; break;
            }
            error_path(step.output_error ? "write" : path,16);
            output_failed=step.output_error; success=false; break;
        }
        ssize_t got=step.count;
        if (step.action!=FAST_BUFFERED) {
            if (!ready(fd,POLLIN)) { success=false; break; }
            got=read(fd,buf,want);
        }
        if (got==0) break;
        if (got<0) {
            if (errno==EINTR || errno==EAGAIN) continue;
            if (!opt.skip_errors) { error_path(path,16); success=false; break; }
            if (opt.skip_errors==1 || !reported_error) error_path(path,16);
            reported_error=true;
            errors++;
            off_t offset=lseek(fd,0,SEEK_CUR);
            struct stat st;
            unsigned long long input_size=0;
            bool have_size=fd_size(fd,&input_size);
            if (offset<0 || fstat(fd,&st) || (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) || (have_size && (unsigned long long)offset>=input_size)) { success=false; break; }
            unsigned long long skip=opt.skip_block ? opt.skip_block-(unsigned long long)offset%opt.skip_block : (1ULL<<(errors>9 ? 9 : errors-1));
            if (skip>want) skip=want;
            if (have_size && skip>input_size-(unsigned long long)offset) skip=input_size-(unsigned long long)offset;
            if (lseek(fd,(off_t)skip,SEEK_CUR)<0) { error_path(path,16); success=false; break; }
            got=(ssize_t)skip;
            memset(buf,0,(size_t)got);
        } else errors=0;
        if (opt.stop && known && opt.lines) {
            unsigned long long left=total-count;
            for (ssize_t j=0;j<got;j++) {
                if (buf[j]==(opt.null ? 0 : '\n') && --left==0) { got=j+1; break; }
            }
        }
        atomic_store(&buffered,(unsigned long long)got);
        size_t offset=0;
        while (offset<(size_t)got && !interrupted && !atomic_load(&cancelled)) {
            ssize_t written;
            bool hole=false;
            if (discard) written=got-(ssize_t)offset;
            else if (opt.sparse && regular && lseek(out,0,SEEK_CUR)>=outstat.st_size && all_zero(buf+offset,(size_t)got-offset) &&
                     lseek(out,got-(off_t)offset,SEEK_CUR)>=0) {
                written=got-(ssize_t)offset;
                hole=true;
            } else {
                if (!ready(out,POLLOUT)) { output_failed=true; success=false; goto end; }
                size_t chunk=(size_t)got-offset;
                if (chunk>4096) chunk=4096;
                written=write(out,buf+offset,chunk);
            }
            if (written<0 && (errno==EINTR || errno==EAGAIN)) continue;
            if (written<=0) {
                if (written<0 && errno==EPIPE) {
                    atomic_store(&cancelled,true); success=false; goto end;
                }
                if (!written) errno=EIO;
                error_path("write",16); output_failed=true; success=false; goto end;
            }
            sparse_tail=hole;
            if (!moved) origin=wall_now();
            moved+=record(buf+offset,(size_t)written);
            offset+=(size_t)written;
            atomic_store(&buffered,(size_t)got-offset);
            if (opt.sync && syncable && !discard && fdatasync(out)) { error_path("fdatasync",16); output_failed=true; success=false; goto end; }
            throttle(origin,moved,limit);
        }
    }
end:
    if (sparse_tail) {
        off_t endpos=lseek(out,0,SEEK_CUR);
        struct stat st;
        if (endpos>=0 && !fstat(out,&st) && endpos>st.st_size && ftruncate(out,endpos)) {
            error_path("ftruncate",16); output_failed=true; success=false;
        }
    }
    fast_close(&fast);
    atomic_store(&buffered,0);
    free(buf);
    return success;
}

static void choose_default_buffer(void) {
    if (changes&CHANGE_BUFFER) return;
    size_t size=400*1024;
    for (int i=0;i<(input_count ? input_count : 1);i++) {
        const char *path=input_count ? inputs[i] : "-";
        bool standard=!strcmp(path,"-");
        int fd=standard ? STDIN_FILENO : open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if (fd<0) continue;
        struct stat st;
        if (!fstat(fd,&st) && st.st_blksize>0)
            size=st.st_blksize>=16384 ? 512*1024 : (size_t)st.st_blksize*32;
        if (!standard) close(fd);
        break;
    }
    opt.buffer=size;
}

static bool write_pidfile(void) {
    if (!opt.pidfile) return true;
    FILE *f=fopen(opt.pidfile,"w");
    if (!f) { error_path(opt.pidfile,1); return false; }
    int failed=fprintf(f,"%ld\n",(long)getpid())<0;
    if (fclose(f)) failed=1;
    if (failed) { unlink(opt.pidfile); atomic_fetch_or(&result,1); return false; }
    pidfile_created=true;
    return true;
}

static void remove_pidfile(void) {
    if (pidfile_created && unlink(opt.pidfile)) error_path(opt.pidfile,1);
    pidfile_created=false;
}

static void *worker(void *unused) {
    (void)unused;
    int out=store_fd>=0 ? store_fd : output_fd;
    for (int i=0;i<(input_count ? input_count : 1) && !interrupted && !atomic_load(&cancelled);i++) {
        const char *path=input_count ? inputs[i] : "-";
        bool standard=!strcmp(path,"-");
        int flags=O_RDONLY|O_NONBLOCK;
#ifdef O_DIRECT
        if (opt.direct) flags|=O_DIRECT;
#endif
        int fd=standard ? STDIN_FILENO : open(path,flags);
        if (fd<0) { error_path(path,2); continue; }
        struct stat input_status;
        if (fstat(fd,&input_status)) {
            error_path(path,2);
            if (!standard && close(fd)) error_path(path,8);
            continue;
        }
        if (same_file(fd,out)) {
            fprintf(stderr,"pb: %s: input file is output file\n",path);
            atomic_fetch_or(&result,4);
        } else if (!transfer(fd,path,out,opt.discard && store_fd<0)) {
            if (!standard && close(fd)) error_path(path,8);
            if (output_failed || interrupted || (atomic_load(&result)&64)) break;
            continue;
        }
        if (!standard && close(fd)) error_path(path,8);
        if (opt.stop && atomic_load(&live_size_set) && atomic_load(&units)>=atomic_load(&live_size)) break;
    }
    if (store_fd>=0 && !interrupted && !output_failed && !(atomic_load(&result)&(4|8|64))) {
        if (lseek(store_fd,0,SEEK_SET)<0) error_path(opt.store,16);
        else {
            pthread_mutex_lock(&stage_lock);
            atomic_store(&stage_nanoseconds,(unsigned long long)(now()*1e9));
            atomic_store(&first_write_nanoseconds,0);
            atomic_store(&stored_units,atomic_load(&units));
            atomic_store(&copied,0);
            atomic_store(&units,0);
            atomic_store(&forwarding,true);
            pthread_mutex_unlock(&stage_lock);
            transfer(store_fd,opt.store,output_fd,opt.discard);
        }
    }
    atomic_store(&done,true);
    return NULL;
}

static void human(char *s, size_t len, double value, bool rate) {
    const char *suffixes=opt.si ? "kMGTPE" : "KMGTPE";
    double base=opt.si ? 1000 : 1024;
    int power=0;
    if (opt.bits && !opt.lines) value*=8;
    while (value>=base && power<6) { value/=base; power++; }
    char suffix[16]="";
    if (power) snprintf(suffix,sizeof suffix,"%c%s",suffixes[power-1],opt.si ? "" : "i");
    snprintf(s,len,"%.3g %s%s%s",value,suffix,opt.lines ? "" : opt.bits ? "b" : "B",rate ? "/s" : "");
}

static void duration(char *s, size_t len, double secs) {
    unsigned long long n=secs>0 && secs<(double)ULLONG_MAX ? (unsigned long long)secs : 0;
    snprintf(s,len,"%llu:%02llu:%02llu",n/3600,n/60%60,n%60);
}

static struct progress_view {
    unsigned long long amount, total;
    bool known;
    double elapsed, rate, average, percent, eta, ratio;
} view;

#include "pb_rates.h"
#include "pb_stats.h"
#include "pb_format.h"

static void large_render(bool initial) {
    if (initial) { fputs("\n\n\n\n\n\n\033[6A",stderr); save_cursor(); draw_static_ui(); }
    if (resized) { clear_old_screen(); draw_static_ui(); resized=0; }
    unsigned long long pct=view.percent>100 ? 100 : (unsigned long long)view.percent;
    restore_cursor(); fputs("\033[1B\033[1C",stderr);
    if (view.total || opt.gauge) draw_block_row(pct); else draw_activity_row(animation_step);
    restore_cursor(); fputs("\033[2B\033[1C",stderr);
    if (view.total || opt.gauge) draw_block_row(pct); else draw_activity_row(animation_step);
    restore_cursor(); fputs("\033[4B\033[1C",stderr);
    fputs(GREEN,stderr);
    if (view.total || opt.gauge) print_percent(pct);
    else {
        for (int i=0;i<4;i++) print_big_number(NUM_NONE);
        fputs("\033[16D    working     ",stderr);
    }
    int spaces=(int)fmin(INT_MAX,(g_columns-3)/4);
    char count[128],speed[128],eta[128];
    human(count,sizeof count,view.amount,false);
    human(speed,sizeof speed,view.rate,true);
    duration(eta,sizeof eta,view.eta);
    fprintf(stderr,"\033[%dC%-*s\033[3C%-*s\033[3C%-*s%s",spaces-15,spaces-2,count,spaces-2,speed,spaces-2,view.known && view.eta>=0 ? eta : "--:--",RST);
    restore_cursor(); fputs("\033[6B",stderr);
}

#include "pb_terminal.h"

static void render(bool final) {
    static bool large_displayed;
    if ((atomic_load(&result)&64) || opt.quiet || (!opt.numeric && !opt.force && !isatty(STDERR_FILENO))) return;
    if (!opt.numeric && !opt.force && isatty(STDERR_FILENO)) {
        pid_t foreground=tcgetpgrp(STDERR_FILENO);
        if (foreground>=0 && foreground!=getpgrp()) return;
    }
    if ((opt.wait && !atomic_load(&copied) && !atomic_load(&first_write_nanoseconds)) || now()-started<opt.delay) return;
    struct winsize ws;
    if (opt.width) g_columns=opt.width;
    else if (!ioctl(STDERR_FILENO,TIOCGWINSZ,&ws) && ws.ws_col) g_columns=ws.ws_col;
    if (atomic_load(&copied)) animation_step+=2;
    char *generated=opt.format ? NULL : default_format();
    const char *fmt=opt.format ? opt.format : generated;
    bool large=opt.large && utf8_output && g_columns>=80 && terminal_height()>=7;
    if (large_displayed && !large) {
        clear_old_screen(); restore_cursor(); large_displayed=false;
    }
    if (monitor_state) {
        int lock_error=pthread_mutex_lock(&monitor_state->display_lock);
#ifdef __linux__
        if (lock_error==EOWNERDEAD) pthread_mutex_consistent(&monitor_state->display_lock);
        else
#endif
        if (lock_error) { free(generated); return; }
    }
    bool cursor=opt.cursor && !opt.numeric && isatty(STDERR_FILENO);
    bool positioned=cursor && terminal_begin();
    bool monitor_cursor=monitor_state && !opt.cursor && !opt.numeric && isatty(STDERR_FILENO);
    if (monitor_cursor) {
        restore_cursor();
        if (monitor_index) fputs("\033[1B",stderr);
    }
    if (!cursor || positioned) {
        if (opt.conemu) conemu(stderr,false);
        if (large) { large_render(!large_displayed); large_displayed=true; }
        else {
            if (!opt.numeric && !positioned) fputc('\r',stderr);
            format(stderr,fmt);
            if (positioned) terminal_end();
            else if (monitor_cursor) { fputs("\033[K",stderr); restore_cursor(); fputs("\033[2B",stderr); }
            else if (opt.numeric || final) fputc('\n',stderr);
            else if (isatty(STDERR_FILENO)) fputs("\033[K",stderr);
        }
    }
    extra_render(fmt);
    fflush(stderr);
    if (monitor_state) pthread_mutex_unlock(&monitor_state->display_lock);
    free(generated);
}

#include "pb_monitor.h"
#include "pb_ipc.h"
#include "pb_watch.h"

int main(int argc, char **argv) {
    setlocale(LC_ALL,"");
    const char *codeset=nl_langinfo(CODESET);
    utf8_output=!strcasecmp(codeset,"UTF-8") || !strcasecmp(codeset,"UTF8");
    argv=preserve_arguments(argc,argv);
    arguments(argc,argv);
    atexit(terminal_close);
    if (!remote_pid && !query_pid && !watch_count && monitor_side<0) infer_size();
    struct sigaction sa={.sa_handler=on_signal};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);
    sigaction(SIGHUP,&sa,NULL); sigaction(SIGWINCH,&sa,NULL);
    sigaction(SIGTSTP,&sa,NULL); sigaction(SIGTTIN,&sa,NULL);
    sigaction(SIGTTOU,&sa,NULL); sigaction(SIGCONT,&sa,NULL);
    signal(SIGPIPE,SIG_IGN);
    if (remote_pid || query_pid) return ipc_client();
    if (watch_count) return watch_run();
    choose_default_buffer();
    atomic_store(&live_readwrite,opt.no_splice);
    atomic_store(&live_limit,opt.limit);
    atomic_store(&live_buffer,opt.buffer);
    atomic_store(&live_size,opt.size);
    atomic_store(&live_size_set,opt.size_set);
    atomic_store(&live_last,required_history());
    if (opt.output && strcmp(opt.output,"-")) {
        output_fd=open(opt.output,O_WRONLY|O_CREAT,0666);
        if (output_fd<0) { error_path(opt.output,2); return atomic_load(&result); }
        for (int i=0;i<(input_count ? input_count : 1);i++) {
            const char *path=input_count ? inputs[i] : "-";
            int fd=!strcmp(path,"-") ? STDIN_FILENO : open(path,O_RDONLY|O_NONBLOCK);
            bool same=fd>=0 && same_file(fd,output_fd);
            if (fd>=0 && fd!=STDIN_FILENO) close(fd);
            if (same) { fputs("pb: input file is output file\n",stderr); close(output_fd); return 4; }
        }
    }
    if (!opt.size_set && !opt.lines) {
        struct stat output;
        unsigned long long size;
        if (!fstat(output_fd,&output) && S_ISBLK(output.st_mode) && fd_size(output_fd,&size)) {
            opt.size=size; opt.size_set=true; opt.stop=true;
            atomic_store(&live_size,size); atomic_store(&live_size_set,true);
        }
    }
    if (opt.store) {
        if (!strcmp(opt.store,"-")) {
            char *path;
            const char *tmpdir=getenv("TMPDIR");
            if (!tmpdir || !*tmpdir) tmpdir="/tmp";
            if (asprintf(&path,"%s/pb-store-XXXXXX",tmpdir)<0) return 64;
            store_fd=mkstemp(path);
            if (store_fd>=0) unlink(path);
            free(path);
        } else store_fd=open(opt.store,O_RDWR|O_CREAT,0600);
        if (store_fd<0) { error_path(opt.store,2); goto cleanup; }
        if (same_file(store_fd,output_fd)) {
            fputs("pb: store file is output file\n",stderr);
            atomic_fetch_or(&result,4); goto cleanup;
        }
        for (int i=0;i<(input_count ? input_count : 1);i++) {
            const char *path=input_count ? inputs[i] : "-";
            int fd=!strcmp(path,"-") ? STDIN_FILENO : open(path,O_RDONLY|O_NONBLOCK);
            bool same=fd>=0 && same_file(fd,store_fd);
            if (fd>=0 && fd!=STDIN_FILENO) close(fd);
            if (same) {
                fputs("pb: store file is input file\n",stderr);
                atomic_fetch_or(&result,4); goto cleanup;
            }
        }
        if (ftruncate(store_fd,0)) { error_path(opt.store,16); goto cleanup; }
    }
    if (output_fd!=STDOUT_FILENO) {
        struct stat st;
        if (fstat(output_fd,&st)) { error_path(opt.output,2); goto cleanup; }
        if (S_ISREG(st.st_mode) && ftruncate(output_fd,0)) { error_path(opt.output,16); goto cleanup; }
    }
    if (monitor_side>=0) {
        if (!monitor_setup()) { atomic_fetch_or(&result,1); goto cleanup; }
        infer_size();
        choose_default_buffer();
        atomic_store(&live_buffer,opt.buffer);
        atomic_store(&live_size,opt.size);
        atomic_store(&live_size_set,opt.size_set);
    }
#ifdef O_DIRECT
    if (opt.direct) {
        int flags=fcntl(STDIN_FILENO,F_GETFL);
        if (flags>=0) fcntl(STDIN_FILENO,F_SETFL,flags|O_DIRECT);
        flags=fcntl(output_fd,F_GETFL);
        if (flags>=0) fcntl(output_fd,F_SETFL,flags|O_DIRECT);
    }
#endif
#ifdef F_SETPIPE_SZ
    if (opt.pipe_size) (void)fcntl(output_fd,F_SETPIPE_SZ,(int)opt.pipe_size);
#endif
    if (!write_pidfile()) goto cleanup;
    atomic_store(&live_last,required_history());
    atomic_store(&live_line,required_line_history());
    if (!reserve_last(atomic_load(&live_last))) { error_path("history allocation",64); goto cleanup; }
    started=now();
    atomic_store(&stage_nanoseconds,(unsigned long long)(started*1e9));
    if (!rates_reset(started,started,0,opt.window)) { error_path("rate history allocation",64); goto cleanup; }
    ipc_start();
    pthread_t thread;
    int err=pthread_create(&thread,NULL,worker,NULL);
    if (err) { errno=err; error_path("pthread_create",64); goto cleanup; }
    double previous_time=started, next=started+opt.interval;
    unsigned long long previous_amount=0;
    double max_rate=0;
    struct rate_statistics statistics={0};
    unsigned long long previous_bytes=0;
    bool was_forwarding=false;
    do {
        handle_suspend();
        monitor_poll();
        bool settings_changed=ipc_service();
        double t=now();
        if (settings_changed) next=t;
        bool final=atomic_load(&done);
        pthread_mutex_lock(&stage_lock);
        bool stage=atomic_load(&forwarding);
        unsigned long long byte_count=atomic_load(&copied);
        view.amount=atomic_load(&units);
        double origin=transfer_origin(now());
        pthread_mutex_unlock(&stage_lock);
        t=now();
        if (stage && !was_forwarding) {
            previous_amount=0; previous_bytes=0; previous_time=t;
            was_forwarding=true; next=t;
        }
        view.total=stage ? atomic_load(&stored_units) : opt.size;
        view.known=stage || opt.size_set;
        view.elapsed=fmax(0,t-origin);
        if (!rates_sample(t,view.amount,origin,opt.window)) {
            error_path("rate history allocation",64); atomic_store(&cancelled,true);
        }
        view.average=rates_average(t,view.amount);
        if (t>=next || final) {
            view.rate=(view.amount-previous_amount)/fmax(t-previous_time,1e-9);
            if (view.rate>max_rate) max_rate=view.rate;
            view.percent=view.total ? 100.0*view.amount/view.total : opt.gauge && max_rate ? 100*view.rate/max_rate : 0;
            view.eta=view.total>view.amount ? (view.average>0 ? (view.total-view.amount)/view.average : -1) : 0;
            if (monitor_state) {
                unsigned long long own=atomic_load(&monitor_state->bytes[monitor_index]);
                unsigned long long other=atomic_load(&monitor_state->bytes[1-monitor_index]);
                view.ratio=own ? (double)other/own : 0;
            }
            render(final);
            double byte_rate=(byte_count-previous_bytes)/fmax(t-previous_time,1e-9);
            statistics_add(&statistics,byte_rate);
            previous_bytes=byte_count;
            previous_time=t; previous_amount=view.amount; next=t+opt.interval;
        }
        if (final) break;
        struct timespec pause={.tv_sec=0,.tv_nsec=10000000};
        nanosleep(&pause,NULL);
    } while (true);
    pthread_join(thread,NULL);
    statistics_show(&statistics);
cleanup:
    ipc_close();
    if (interrupted) atomic_fetch_or(&result,32);
    if (store_fd>=0 && close(store_fd)) error_path("store close",8);
    if (output_fd!=STDOUT_FILENO && close(output_fd)) error_path("output close",8);
    if (monitor_side>=0) close(STDIN_FILENO);
    monitor_finish();
    remove_pidfile();
    free(last_bytes); free(current_line); free(previous_line);
    free(rates.samples);
    return atomic_load(&result);
}
