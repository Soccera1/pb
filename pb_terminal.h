/* Concurrent -c displays share a per-terminal row registry under flock(2). */
#define CURSOR_MAGIC UINT64_C(0x7062637572736f73)
struct cursor_owner { pid_t pid; size_t row; unsigned long long start; };
struct cursor_registry {
    uint64_t magic;
    size_t high, height;
    struct cursor_owner owners[];
};
static int cursor_fd=-1;
static struct cursor_registry *cursor_registry;
static unsigned long long cursor_start;
static size_t cursor_capacity;

static unsigned long long process_start(pid_t pid) {
    char path[64], line[4096];
    snprintf(path,sizeof path,"/proc/%ld/stat",(long)pid);
    FILE *f=fopen(path,"r");
    if (!f) return 0;
    char *read=fgets(line,sizeof line,f);
    fclose(f);
    if (!read) return 0;
    char *end=strrchr(line,')');
    if (!end || end[2]=='Z' || end[2]=='X') return 0;
    char *p=end+2;
    for (int field=3;field<22;field++) {
        while (*p && *p!=' ') p++;
        while (*p==' ') p++;
    }
    return strtoull(p,NULL,10);
}

static size_t terminal_height(void) {
    struct winsize ws;
    if (opt.height) return opt.height;
    if (!ioctl(STDERR_FILENO,TIOCGWINSZ,&ws) && ws.ws_row) return ws.ws_row;
    return 25;
}

static bool terminal_open(void) {
    if (cursor_fd>=0) return true;
    struct stat tty;
    if (fstat(STDERR_FILENO,&tty)) return false;
    const char *directory=getenv("TMPDIR");
    if (!directory || !*directory) directory=getenv("TMP");
    if (!directory || !*directory) directory="/tmp";
    char *path;
    if (asprintf(&path,"%s/pb-cursor-%lu-%lu-%lu",directory,(unsigned long)getuid(),
                 (unsigned long)major(tty.st_rdev),(unsigned long)minor(tty.st_rdev))<0) return false;
    cursor_fd=open(path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    free(path);
    if (cursor_fd<0) return false;
    struct stat st;
    if (fstat(cursor_fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=getuid() || (st.st_mode&077)) {
        close(cursor_fd); cursor_fd=-1; return false;
    }
    cursor_capacity=sizeof *cursor_registry;
    cursor_registry=calloc(1,cursor_capacity);
    if (!cursor_registry) { close(cursor_fd); cursor_fd=-1; return false; }
    cursor_start=process_start(getpid());
    return true;
}

static bool terminal_load(void) {
    if (flock(cursor_fd,LOCK_EX)) return false;
    struct cursor_registry header;
    ssize_t n=pread(cursor_fd,&header,sizeof header,0);
    bool valid=n==(ssize_t)sizeof header && header.magic==CURSOR_MAGIC &&
               header.high<=(SIZE_MAX/2-sizeof header)/sizeof(struct cursor_owner);
    size_t bytes=valid ? sizeof header+header.high*sizeof(struct cursor_owner) : sizeof header;
    if (cursor_capacity<bytes) {
        struct cursor_registry *replacement=realloc(cursor_registry,bytes);
        if (!replacement) { flock(cursor_fd,LOCK_UN); return false; }
        cursor_registry=replacement; cursor_capacity=bytes;
    }
    if (!valid || pread(cursor_fd,cursor_registry,bytes,0)!=(ssize_t)bytes) {
        memset(cursor_registry,0,cursor_capacity);
        cursor_registry->magic=CURSOR_MAGIC;
    }
    bool active=false;
    for (size_t i=0;i<cursor_registry->high;i++) {
        struct cursor_owner *owner=&cursor_registry->owners[i];
        if (owner->pid && process_start(owner->pid)!=owner->start) owner->pid=0;
        if (owner->pid) active=true;
    }
    if (!active) cursor_registry->high=0;
    cursor_registry->height=terminal_height();
    return true;
}

static bool terminal_store(void) {
    const unsigned char *data=(const unsigned char *)cursor_registry;
    size_t offset=0;
    size_t bytes=sizeof *cursor_registry+cursor_registry->high*sizeof(struct cursor_owner);
    while (offset<bytes) {
        ssize_t n=pwrite(cursor_fd,data+offset,bytes-offset,(off_t)offset);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return false;
        offset+=(size_t)n;
    }
    return true;
}

static ssize_t terminal_row(size_t slot) {
    return (ssize_t)cursor_registry->height-(ssize_t)cursor_registry->high+(ssize_t)slot+1;
}

/* Holds the registry lock until terminal_end, making each painted line atomic. */
static bool terminal_begin(void) {
    if (!terminal_open() || !terminal_load()) return false;
    size_t slot=cursor_registry->high;
    for (size_t i=0;i<cursor_registry->high;i++) {
        struct cursor_owner *owner=&cursor_registry->owners[i];
        if (owner->pid==getpid() && owner->start==cursor_start && owner->row==terminal_row_offset) { slot=i; break; }
    }
    if (slot==cursor_registry->high) {
        for (size_t i=0;i<cursor_registry->high;i++) if (!cursor_registry->owners[i].pid) { slot=i; break; }
        if (slot==cursor_registry->high) {
            if (slot>=(SIZE_MAX/2-sizeof *cursor_registry)/sizeof(struct cursor_owner)) { flock(cursor_fd,LOCK_UN); return false; }
            size_t bytes=sizeof *cursor_registry+(slot+1)*sizeof(struct cursor_owner);
            if (bytes>cursor_capacity) {
                struct cursor_registry *replacement=realloc(cursor_registry,bytes);
                if (!replacement) { flock(cursor_fd,LOCK_UN); return false; }
                cursor_registry=replacement; cursor_capacity=bytes;
            }
            cursor_registry->high++;
            fprintf(stderr,"\033[%zu;1H\n",cursor_registry->height);
        }
        cursor_registry->owners[slot]=(struct cursor_owner){getpid(),terminal_row_offset,cursor_start};
    }
    if (!terminal_store()) { flock(cursor_fd,LOCK_UN); return false; }
    ssize_t row=terminal_row(slot);
    if (row<1) { flock(cursor_fd,LOCK_UN); return false; }
    fprintf(stderr,"\033[%zd;1H",row);
    return true;
}

static void terminal_end(void) {
    fputs("\033[K",stderr);
    fprintf(stderr,"\033[%zu;1H",cursor_registry->height);
    fflush(stderr);
    flock(cursor_fd,LOCK_UN);
}

static void terminal_trim(size_t rows) {
    if (cursor_fd<0 || !terminal_load()) return;
    for (size_t i=0;i<cursor_registry->high;i++) {
        struct cursor_owner *owner=&cursor_registry->owners[i];
        if (owner->pid==getpid() && owner->row>=rows) {
            ssize_t row=terminal_row(i);
            if (row>0) fprintf(stderr,"\033[%zd;1H\033[K",row);
            owner->pid=0;
        }
    }
    terminal_store();
    fflush(stderr); flock(cursor_fd,LOCK_UN);
}

static void terminal_close(void) {
    if (cursor_fd>=0) {
        if (terminal_load()) {
            bool active=false;
            for (size_t i=0;i<cursor_registry->high;i++) {
                struct cursor_owner *owner=&cursor_registry->owners[i];
                if (owner->pid==getpid() && owner->start==cursor_start) owner->pid=0;
                if (owner->pid) active=true;
            }
            terminal_store();
            if (!active) fprintf(stderr,"\033[%zu;1H\n",cursor_registry->height);
            fflush(stderr); flock(cursor_fd,LOCK_UN);
        }
        close(cursor_fd); cursor_fd=-1;
        free(cursor_registry); cursor_registry=NULL;
    }
    if (conemu_active) { conemu(stderr,true); conemu_active=false; fflush(stderr); }
}
