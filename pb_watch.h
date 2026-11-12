/* Linux descriptor monitoring: observe /proc metadata without consuming data. */
struct watched_process {
    pid_t pid;
    bool active;
};
struct watched_fd {
    pid_t pid;
    int fd, access;
    dev_t device;
    ino_t inode;
    unsigned long long position, size, initial, previous;
    double began, sampled, maximum_rate;
    struct rate_window history;
    bool active, specific, transfer_started;
    char *name;
};
static struct watched_process *watch_processes;
static size_t watch_process_count;
static struct watched_fd *watch_fds;
static size_t watch_fd_count;

static bool watch_alive(pid_t pid) {
    char path[64], line[4096];
    snprintf(path,sizeof path,"/proc/%ld/stat",(long)pid);
    FILE *f=fopen(path,"r");
    if (!f) return false;
    bool alive=false;
    if (fgets(line,sizeof line,f)) {
        char *end=strrchr(line,')');
        alive=end && end[1]==' ' && end[2]!='Z' && end[2]!='X';
    }
    fclose(f);
    return alive;
}

static bool watch_metadata(pid_t pid, int fd, struct watched_fd *item, bool label) {
    char path[80];
    struct stat st;
    snprintf(path,sizeof path,"/proc/%ld/fd/%d",(long)pid,fd);
    if (stat(path,&st) || (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode))) return false;
    snprintf(path,sizeof path,"/proc/%ld/fdinfo/%d",(long)pid,fd);
    FILE *f=fopen(path,"r");
    if (!f) return false;
    char line[256];
    unsigned long long position=0;
    unsigned flags=0;
    bool have_position=false, have_flags=false;
    while (fgets(line,sizeof line,f)) {
        if (sscanf(line,"pos: %llu",&position)==1) have_position=true;
        if (sscanf(line,"flags: %o",&flags)==1) have_flags=true;
    }
    fclose(f);
    if (!have_position || !have_flags) return false;
    snprintf(path,sizeof path,"/proc/%ld/fd/%d",(long)pid,fd);
    struct stat verify;
    if (stat(path,&verify) || verify.st_dev!=st.st_dev || verify.st_ino!=st.st_ino) return false;
    item->pid=pid; item->fd=fd; item->device=st.st_dev; item->inode=st.st_ino;
    item->access=flags&O_ACCMODE;
    item->position=position; item->size=st.st_size;
    if (S_ISBLK(st.st_mode)) {
        int device=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if (device<0 && item->access==O_WRONLY) device=open(path,O_WRONLY|O_NONBLOCK|O_CLOEXEC);
        unsigned long long size;
        if (device>=0) {
            if (fd_size(device,&size)) item->size=size;
            close(device);
        }
    }
    if (label) {
        char *target=NULL;
        size_t capacity=128;
        for (;;) {
            char *replacement=realloc(target,capacity);
            if (!replacement) { free(target); return false; }
            target=replacement;
            ssize_t n=readlink(path,target,capacity-1);
            if (n<0) { free(target); return false; }
            if ((size_t)n<capacity-1) { target[n]=0; break; }
            if (capacity>SIZE_MAX/2) { free(target); errno=ENOMEM; return false; }
            capacity*=2;
        }
        int n=asprintf(&item->name,"%ld:%d %s",(long)pid,fd,target);
        free(target);
        if (n<0) return false;
    }
    return true;
}

static bool watch_identity(const struct watched_fd *a, const struct watched_fd *b) {
    return a->pid==b->pid && a->fd==b->fd && a->device==b->device &&
           a->inode==b->inode && a->access==b->access;
}

static bool watch_add_fd(pid_t pid, int fd, bool specific) {
    struct watched_fd item={0};
    if (!watch_metadata(pid,fd,&item,true)) return false;
    for (size_t i=0;i<watch_fd_count;i++) if (watch_fds[i].active && watch_identity(&item,&watch_fds[i])) {
        if (specific) watch_fds[i].specific=true;
        free(item.name); return true;
    }
    struct watched_fd *replacement=realloc(watch_fds,(watch_fd_count+1)*sizeof *watch_fds);
    if (!replacement) { free(item.name); atomic_fetch_or(&result,64); return false; }
    watch_fds=replacement;
    item.active=true; item.specific=specific;
    item.initial=item.previous=item.position;
    item.began=item.sampled=now();
    if (!rate_window_reset(&item.history,item.began,item.began,item.initial,opt.window)) {
        free(item.name); atomic_fetch_or(&result,64); return false;
    }
    watch_fds[watch_fd_count++]=item;
    return true;
}

static bool watch_add_process(pid_t pid) {
    if (!watch_alive(pid)) { errno=ESRCH; return false; }
    for (size_t i=0;i<watch_process_count;i++) if (watch_processes[i].pid==pid) return true;
    struct watched_process *replacement=realloc(watch_processes,(watch_process_count+1)*sizeof *watch_processes);
    if (!replacement) { atomic_fetch_or(&result,64); return false; }
    watch_processes=replacement;
    watch_processes[watch_process_count++]=(struct watched_process){pid,true};
    return true;
}

struct watch_list_ancestor { dev_t device; ino_t inode; const struct watch_list_ancestor *parent; };

static bool watch_spec(const char *spec, const struct watch_list_ancestor *ancestors) {
    if (*spec=='@') {
        FILE *list=fopen(spec+1,"r");
        if (!list) return false;
        struct stat st;
        if (fstat(fileno(list),&st)) { fclose(list); return false; }
        for (const struct watch_list_ancestor *a=ancestors;a;a=a->parent) {
            if (a->device==st.st_dev && a->inode==st.st_ino) { fclose(list); errno=ELOOP; return false; }
        }
        struct watch_list_ancestor here={st.st_dev,st.st_ino,ancestors};
        char *line=NULL;
        size_t capacity=0;
        bool success=true;
        while (getline(&line,&capacity,list)>=0) {
            char *begin=line;
            while (isspace((unsigned char)*begin)) begin++;
            char *end=begin+strlen(begin);
            while (end>begin && isspace((unsigned char)end[-1])) *--end=0;
            if (*begin && !watch_spec(begin,&here)) { error_path(begin,2); success=false; }
        }
        if (ferror(list)) success=false;
        free(line); fclose(list);
        return success;
    }
    if (*spec=='=') {
        regex_t pattern;
        if (regcomp(&pattern,spec+1,REG_EXTENDED|REG_NOSUB)) { errno=EINVAL; return false; }
        DIR *proc=opendir("/proc");
        if (!proc) { regfree(&pattern); return false; }
        struct dirent *entry;
        while ((entry=readdir(proc))) {
            char *end;
            long n=strtol(entry->d_name,&end,10);
            if (*end || n<=0 || n>INT_MAX || n==getpid()) continue;
            char path[64], name[256];
            snprintf(path,sizeof path,"/proc/%ld/comm",n);
            FILE *f=fopen(path,"r");
            if (!f) continue;
            if (fgets(name,sizeof name,f)) {
                name[strcspn(name,"\n")]=0;
                if (!regexec(&pattern,name,0,NULL,0)) (void)watch_add_process((pid_t)n);
            }
            fclose(f);
        }
        closedir(proc); regfree(&pattern);
        return true;
    }
    if (!isdigit((unsigned char)*spec)) { errno=EINVAL; return false; }
    char *end;
    errno=0;
    long pid=strtol(spec,&end,10);
    if (errno || pid<=0 || pid>INT_MAX) { errno=EINVAL; return false; }
    if (!*end) return watch_add_process((pid_t)pid);
    if (*end++!=':' || !isdigit((unsigned char)*end)) { errno=EINVAL; return false; }
    char *fd_end;
    long fd=strtol(end,&fd_end,10);
    if (errno || *fd_end || fd<0 || fd>INT_MAX) { errno=EINVAL; return false; }
    if (!watch_alive((pid_t)pid)) { errno=ESRCH; return false; }
    if (!watch_add_fd((pid_t)pid,(int)fd,true)) { if (!errno) errno=EINVAL; return false; }
    return true;
}

static void watch_scan(void) {
    for (size_t i=0;i<watch_process_count;i++) {
        struct watched_process *process=&watch_processes[i];
        if (!process->active) continue;
        if (!watch_alive(process->pid)) { process->active=false; continue; }
        char path[64];
        snprintf(path,sizeof path,"/proc/%ld/fd",(long)process->pid);
        DIR *fds=opendir(path);
        if (!fds) {
            if (errno!=ENOENT) error_path(path,2);
            process->active=false; continue;
        }
        struct dirent *entry;
        while ((entry=readdir(fds))) {
            char *end;
            long fd=strtol(entry->d_name,&end,10);
            if (!*end && fd>=0 && fd<=INT_MAX) (void)watch_add_fd(process->pid,(int)fd,false);
        }
        closedir(fds);
    }
}

static int watch_run(void) {
    for (size_t i=0;i<watch_count;i++) if (!watch_spec(watch_specs[i],NULL)) error_path(watch_specs[i],2);
    for (int i=0;i<input_count;i++) if (!watch_spec(inputs[i],NULL)) error_path(inputs[i],2);
    free(watch_specs); watch_specs=NULL;
    if (!write_pidfile()) {
        for (size_t i=0;i<watch_fd_count;i++) { free(watch_fds[i].history.samples); free(watch_fds[i].name); }
        free(watch_processes); free(watch_fds); return 1;
    }
    opt.large=false;
    started=now();
    proxy_progress=true;
    ipc_start();
    size_t previous_rows=0;
    double next_display=started;
    unsigned long generation=ipc_generation;
    while (!interrupted && !(atomic_load(&result)&64)) {
        handle_suspend();
        (void)ipc_service();
        bool paint=now()>=next_display || generation!=ipc_generation;
        generation=ipc_generation;
        const char *given_name=opt.name;
        unsigned long long given_size=opt.size;
        watch_scan();
        bool active=false;
        for (size_t i=0;i<watch_process_count;i++) if (watch_processes[i].active) active=true;
        size_t rows=0;
        for (size_t i=0;i<watch_fd_count;i++) {
            struct watched_fd *item=&watch_fds[i];
            struct watched_fd current={0};
            if (!item->active) continue;
            if (!watch_metadata(item->pid,item->fd,&current,false) || !watch_identity(item,&current)) { item->active=false; continue; }
            active=true;
            double t=now();
            if (!item->transfer_started && current.position!=item->initial) {
                item->transfer_started=true;
                note_first_write();
                if (opt.wait) {
                    item->began=item->sampled=t;
                    if (!rate_window_reset(&item->history,t,t,item->initial,opt.window)) atomic_fetch_or(&result,64);
                }
            }
            view.amount=current.position;
            view.total=opt.size_set && item->specific ? given_size : current.size;
            view.known=view.total>0;
            view.elapsed=t-item->began;
            view.rate=current.position>=item->previous ? (current.position-item->previous)/fmax(t-item->sampled,1e-9) : 0;
            if (!rate_window_sample(&item->history,t,current.position,item->began,opt.window)) {
                atomic_fetch_or(&result,64);
            }
            view.average=rate_window_average(&item->history,t,current.position);
            if (view.rate>item->maximum_rate) item->maximum_rate=view.rate;
            view.percent=view.total ? 100.0*view.amount/view.total : opt.gauge && item->maximum_rate ? 100*view.rate/item->maximum_rate : 0;
            view.eta=view.total>view.amount ? (view.average>0 ? (view.total-view.amount)/view.average : -1) : 0;
            opt.name=given_name ? given_name : item->name;
            atomic_store(&copied,current.position);
            atomic_store(&units,current.position);
            if (paint && (!opt.wait || item->transfer_started)) {
                if (!opt.cursor && !rows && previous_rows && !opt.numeric && isatty(STDERR_FILENO)) fprintf(stderr,"\033[%zuA",previous_rows);
                if (opt.numeric || rows<terminal_height()) {
                    terminal_row_offset=rows;
                    render(true); rows++;
                }
            }
            item->previous=current.position; item->position=current.position; item->size=current.size; item->sampled=t;
        }
        opt.name=given_name;
        if (!active && !rows) break;
        if (paint && !opt.cursor && !opt.numeric && isatty(STDERR_FILENO) && rows<previous_rows) {
            if (!rows) fprintf(stderr,"\033[%zuA",previous_rows);
            for (size_t i=rows;i<previous_rows;i++) fputs("\r\033[K\n",stderr);
            fprintf(stderr,"\033[%zuA",previous_rows-rows);
            fflush(stderr);
        }
        if (paint) {
            if (opt.cursor) terminal_trim(rows);
            previous_rows=rows; next_display=now()+opt.interval;
        }
        /* Closed identities never reappear; whole-process scans add replacements. */
        size_t keep=0;
        for (size_t i=0;i<watch_fd_count;i++) {
            if (watch_fds[i].active) watch_fds[keep++]=watch_fds[i];
            else { free(watch_fds[i].history.samples); free(watch_fds[i].name); }
        }
        watch_fd_count=keep;
        if (!active) break;
        double until=now()+fmin(.05,opt.interval);
        while (!interrupted && now()<until) {
            handle_suspend();
            if (ipc_service()) break;
            double delay=fmin(.05,until-now());
            if (delay<=0) break;
            struct timespec pause={.tv_sec=0,.tv_nsec=(long)(delay*1e9)};
            nanosleep(&pause,NULL);
        }
    }
    atomic_store(&done,true);
    ipc_close(); remove_pidfile();
    for (size_t i=0;i<watch_fd_count;i++) { free(watch_fds[i].history.samples); free(watch_fds[i].name); }
    free(last_bytes); last_bytes=NULL;
    free(watch_processes); free(watch_fds);
    if (interrupted) atomic_fetch_or(&result,32);
    return atomic_load(&result);
}
