/* Versioned, chunked local control messages. No field/history size is hard-coded. */
#define IPC_VERSION 6
#define IPC_CHUNK 16384
struct ipc_request {
    unsigned version, operation;
    unsigned long long mask, size, buffer;
    double limit;
    unsigned display;
    size_t last, width, height, history, line, name_length, format_length, style_length, extra_length;
    bool bits, si, gauge, no_splice;
    double interval, window;
};
struct ipc_reply_header {
    unsigned version;
    int status;
    struct progress_view progress;
    unsigned long long bytes, buffer, capacity;
    bool lines, null, finished;
    double window;
    int engine;
    size_t history_length, previous_length;
};
struct ipc_reply { struct ipc_reply_header header; unsigned char *data; };
struct ipc_peer {
    int fd;
    struct ipc_request request;
    bool have_header, subscribed, closing;
    unsigned char *input, *output;
    size_t input_size, input_used, output_size, output_sent;
    double next, interval;
};
static struct ipc_peer *ipc_peers;
static size_t ipc_peer_count;
static unsigned long ipc_generation;
static int ipc_fd=-1, ipc_query_fd=-1;
static char ipc_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static char *remote_name, *remote_format, *remote_style;

static bool ipc_address(struct sockaddr_un *address, pid_t pid, bool create) {
    char directory[64];
    snprintf(directory,sizeof directory,"/tmp/pb-%lu",(unsigned long)getuid());
    if (create && mkdir(directory,0700) && errno!=EEXIST) return false;
    struct stat st;
    if (lstat(directory,&st)) return false;
    if (!S_ISDIR(st.st_mode) || st.st_uid!=getuid() || (st.st_mode&077)) { errno=EACCES; return false; }
    memset(address,0,sizeof *address);
    address->sun_family=AF_UNIX;
    snprintf(address->sun_path,sizeof address->sun_path,"%s/%ld.sock",directory,(long)pid);
    return true;
}

static void ipc_start(void) {
    struct sockaddr_un address;
    if (!ipc_address(&address,getpid(),true)) return;
    ipc_fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if (ipc_fd<0) return;
    struct stat st;
    if (!lstat(address.sun_path,&st) && st.st_uid==getuid() && S_ISSOCK(st.st_mode)) unlink(address.sun_path);
    if (bind(ipc_fd,(struct sockaddr *)&address,sizeof address) || listen(ipc_fd,16)) {
        close(ipc_fd); ipc_fd=-1; return;
    }
    snprintf(ipc_path,sizeof ipc_path,"%s",address.sun_path);
    chmod(ipc_path,0600);
}

static bool ipc_lengths(size_t a, size_t b, size_t c, size_t *total) {
    if (a>SIZE_MAX/2 || b>SIZE_MAX/2-a || c>SIZE_MAX/2-a-b) return false;
    *total=a+b+c;
    return true;
}

static bool ipc_valid_header(const struct ipc_request *r, size_t *length) {
    if (r->version!=IPC_VERSION || (r->operation!='R' && r->operation!='Q')) return false;
    if (!r->name_length || !r->format_length || !r->style_length ||
        !ipc_lengths(r->name_length,r->format_length,r->style_length,length)) return false;
    if (!r->extra_length || r->extra_length>SIZE_MAX/2-*length) return false;
    *length+=r->extra_length;
    if ((r->mask&CHANGE_BUFFER) && (!r->buffer || r->buffer>SIZE_MAX/2)) return false;
    if (r->last>SIZE_MAX/2 || r->width>SIZE_MAX/2 || r->height>SIZE_MAX/2 || r->history>SIZE_MAX/2 || r->line>SIZE_MAX/2) return false;
    if (!isfinite(r->limit) || r->limit<0) return false;
    if (!isfinite(r->interval) || r->interval<=0 || !isfinite(r->window) || r->window<=0) return false;
    return true;
}

static bool ipc_string(const unsigned char *text, size_t n) {
    return n && text[n-1]==0 && !memchr(text,0,n-1);
}

static int ipc_apply(const struct ipc_request *r, const unsigned char *payload) {
    const char *name=(const char *)payload;
    const char *fmt=name+r->name_length;
    const char *style=fmt+r->format_length;
    unsigned long long mask=r->mask;
    char *new_name=mask&CHANGE_NAME ? strdup(name) : NULL;
    char *new_format=mask&CHANGE_FORMAT ? strdup(fmt) : NULL;
    char *new_style=mask&CHANGE_STYLE ? strdup(style) : NULL;
    struct extra_display *new_extra=NULL;
    size_t new_count=0;
    int failure=0;
    if (((mask&CHANGE_NAME) && !new_name) || ((mask&CHANGE_FORMAT) && !new_format) ||
        ((mask&CHANGE_STYLE) && !new_style)) failure=ENOMEM;
    if (!failure && (mask&CHANGE_EXTRA)) {
        const char *specs=style+r->style_length;
        size_t offset=0;
        while (offset<r->extra_length && specs[offset]) {
            const char *end=memchr(specs+offset,0,r->extra_length-offset);
            if (!end) { failure=EINVAL; break; }
            struct extra_display display;
            if (!parse_extra_display(specs+offset,&display)) { failure=errno; break; }
            struct extra_display *replacement=realloc(new_extra,(new_count+1)*sizeof *new_extra);
            if (!replacement) { free(display.format); failure=ENOMEM; break; }
            new_extra=replacement; new_extra[new_count++]=display;
            offset=(size_t)(end-specs)+1;
        }
        if (!new_count && !failure) failure=EINVAL;
    }
    if (failure) {
        free(new_name); free(new_format); free(new_style); free_extra_displays(new_extra,new_count);
        return failure;
    }
    if (mask&CHANGE_DISPLAY) {
        opt.display=r->display; opt.large=false;
        if (r->display&(BUFFER|LAST)) atomic_store(&live_readwrite,true);
    }
    if (mask&CHANGE_FORMAT) { free(remote_format); remote_format=new_format; opt.format=remote_format; opt.large=false; }
    if (mask&CHANGE_NAME) { free(remote_name); remote_name=new_name; opt.name=remote_name; resized=1; }
    if (mask&CHANGE_STYLE) { free(remote_style); remote_style=new_style; opt.style=remote_style; opt.large=false; }
    if (mask&CHANGE_EXTRA) {
        free_extra_displays(extra_displays,extra_count);
        extra_displays=new_extra; extra_count=new_count;
    }
    if (mask&CHANGE_SIZE) {
        opt.size=r->size; opt.size_set=true;
        atomic_store(&live_size,r->size); atomic_store(&live_size_set,true);
    }
    if (mask&CHANGE_LIMIT) { opt.limit=r->limit; atomic_store(&live_limit,r->limit); }
    if (mask&CHANGE_BUFFER) { atomic_store(&live_readwrite,true); opt.buffer=r->buffer; atomic_store(&live_buffer,r->buffer); }
    if (mask&CHANGE_LAST) opt.last=r->last;
    if (mask&CHANGE_INTERVAL) opt.interval=r->interval;
    if (mask&CHANGE_WIDTH) { opt.width=r->width; resized=1; }
    if (mask&CHANGE_HEIGHT) opt.height=r->height;
    if (mask&CHANGE_BITS) opt.bits=r->bits;
    if (mask&CHANGE_SI) opt.si=r->si;
    if (mask&CHANGE_GAUGE) opt.gauge=r->gauge;
    if (mask&CHANGE_WINDOW) opt.window=r->window;
    if (mask&CHANGE_NO_SPLICE) atomic_store(&live_readwrite,true);
    atomic_store(&live_last,required_history());
    atomic_store(&live_line,required_line_history());
    ipc_generation++;
    return 0;
}

static bool ipc_snapshot(struct ipc_peer *peer, bool final, int status) {
    struct ipc_reply_header reply={.version=IPC_VERSION,.status=status};
    if (!status) {
        reply.progress=view;
        pthread_mutex_lock(&stage_lock);
        bool stage=atomic_load(&forwarding);
        reply.bytes=atomic_load(&copied);
        if (!proxy_progress) {
            reply.progress.amount=atomic_load(&units);
            reply.progress.total=stage ? atomic_load(&stored_units) : opt.size;
            reply.progress.known=stage || opt.size_set;
        }
        double origin=transfer_origin(now());
        pthread_mutex_unlock(&stage_lock);
        if (!proxy_progress) reply.progress.elapsed=fmax(0,now()-origin);
        reply.progress.percent=reply.progress.total ? 100.0*reply.progress.amount/reply.progress.total : view.percent;
        reply.engine=atomic_load(&transfer_engine);
        reply.buffer=atomic_load(&buffered); reply.capacity=opt.buffer;
        reply.lines=opt.lines; reply.null=opt.null; reply.window=opt.window;
        reply.finished=final || atomic_load(&done);
    }
    pthread_mutex_lock(&history_lock);
    if (!status) {
        reply.history_length=last_len>peer->request.history ? peer->request.history : last_len;
        reply.previous_length=previous_line_len>peer->request.line ? peer->request.line : previous_line_len;
    }
    size_t length;
    if (!ipc_lengths(sizeof reply,reply.history_length,reply.previous_length,&length)) {
        pthread_mutex_unlock(&history_lock); return false;
    }
    unsigned char *frame=malloc(length);
    if (!frame) { pthread_mutex_unlock(&history_lock); return false; }
    memcpy(frame,&reply,sizeof reply);
    copy_last(frame+sizeof reply,reply.history_length);
    if (reply.previous_length) memcpy(frame+sizeof reply+reply.history_length,previous_line,reply.previous_length);
    pthread_mutex_unlock(&history_lock);
    free(peer->output); peer->output=frame; peer->output_size=length; peer->output_sent=0;
    return true;
}

static bool ipc_flush(struct ipc_peer *peer) {
    while (peer->output_sent<peer->output_size) {
        size_t n=peer->output_sent ? peer->output_size-peer->output_sent : sizeof(struct ipc_reply_header);
        if (peer->output_sent && n>IPC_CHUNK) n=IPC_CHUNK;
        ssize_t sent=send(peer->fd,peer->output+peer->output_sent,n,MSG_DONTWAIT|MSG_NOSIGNAL);
        if (sent<0 && errno==EINTR) continue;
        if (sent<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return true;
        if (sent!=(ssize_t)n) return false;
        peer->output_sent+=n;
    }
    free(peer->output); peer->output=NULL; peer->output_size=peer->output_sent=0;
    return true;
}

static void ipc_drop(struct ipc_peer *peer) {
    close(peer->fd); free(peer->input); free(peer->output);
    memset(peer,0,sizeof *peer); peer->fd=-1;
}

static bool ipc_receive(struct ipc_peer *peer) {
    if (!peer->have_header) {
        ssize_t n=recv(peer->fd,&peer->request,sizeof peer->request,MSG_DONTWAIT|MSG_TRUNC);
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)) return true;
        if (n==0) return false;
        if (n!=(ssize_t)sizeof peer->request || !ipc_valid_header(&peer->request,&peer->input_size)) {
            peer->closing=true;
            return ipc_snapshot(peer,false,EINVAL);
        }
        peer->have_header=true;
        peer->input=malloc(peer->input_size);
        if (!peer->input) { peer->closing=true; return ipc_snapshot(peer,false,ENOMEM); }
    }
    while (peer->input_used<peer->input_size) {
        size_t remaining=peer->input_size-peer->input_used;
        ssize_t n=recv(peer->fd,peer->input+peer->input_used,remaining,MSG_DONTWAIT|MSG_TRUNC);
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)) return true;
        if (n<=0 || (size_t)n>remaining) return false;
        peer->input_used+=(size_t)n;
    }
    struct ipc_request *r=&peer->request;
    const unsigned char *name=peer->input, *fmt=name+r->name_length, *style=fmt+r->format_length;
    if (!ipc_string(name,r->name_length) || !ipc_string(fmt,r->format_length) || !ipc_string(style,r->style_length) ||
        (strcmp((const char *)style,"plain") && strcmp((const char *)style,"block") &&
         strcmp((const char *)style,"granular") && strcmp((const char *)style,"shaded"))) {
        peer->closing=true; return ipc_snapshot(peer,false,EINVAL);
    }
    int status=0;
    if (r->operation=='R') { status=ipc_apply(r,peer->input); peer->closing=true; }
    else {
        peer->subscribed=true;
        peer->interval=fmax(.01,r->interval); peer->next=now()+peer->interval;
    }
    free(peer->input); peer->input=NULL;
    return ipc_snapshot(peer,false,status);
}

static bool ipc_service(void) {
    if (ipc_fd<0) return false;
    bool changed=false;
    for (int i=0;i<8;i++) {
        int client=accept4(ipc_fd,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
        if (client<0) break;
#ifdef SO_PEERCRED
        struct ucred credentials;
        socklen_t size=sizeof credentials;
        if (getsockopt(client,SOL_SOCKET,SO_PEERCRED,&credentials,&size) || credentials.uid!=getuid()) { close(client); continue; }
#endif
        struct ipc_peer *replacement=realloc(ipc_peers,(ipc_peer_count+1)*sizeof *ipc_peers);
        if (!replacement) { close(client); continue; }
        ipc_peers=replacement;
        ipc_peers[ipc_peer_count++]=(struct ipc_peer){.fd=client};
    }
    size_t keep=0;
    for (size_t i=0;i<ipc_peer_count;i++) {
        struct ipc_peer *peer=&ipc_peers[i];
        struct pollfd poller={.fd=peer->fd,.events=0};
        bool alive=!(poll(&poller,1,0)>0 && (poller.revents&(POLLHUP|POLLERR|POLLNVAL)));
        if (alive && !peer->subscribed && !peer->closing) {
            alive=ipc_receive(peer);
            if (peer->closing && peer->request.operation=='R') changed=true;
        }
        if (alive && peer->subscribed && !peer->output && now()>=peer->next) {
            alive=ipc_snapshot(peer,false,0); peer->next=now()+peer->interval;
        }
        if (alive && peer->output) alive=ipc_flush(peer);
        if (!alive || (peer->closing && !peer->output)) ipc_drop(peer);
        else ipc_peers[keep++]=*peer;
    }
    ipc_peer_count=keep;
    return changed;
}

static void ipc_close(void) {
    for (size_t i=0;i<ipc_peer_count;i++) {
        struct ipc_peer *peer=&ipc_peers[i];
        double deadline=now()+.2;
        bool final=false;
        while (peer->subscribed && now()<deadline) {
            if (!peer->output && !final) { if (!ipc_snapshot(peer,true,0)) break; final=true; }
            if (!ipc_flush(peer) || (final && !peer->output)) break;
            struct pollfd wait={.fd=peer->fd,.events=POLLOUT};
            (void)poll(&wait,1,10);
        }
        ipc_drop(peer);
    }
    free(ipc_peers); ipc_peers=NULL; ipc_peer_count=0;
    if (ipc_fd>=0) close(ipc_fd);
    ipc_fd=-1;
    if (ipc_path[0]) unlink(ipc_path);
    ipc_path[0]=0;
    free(remote_name); free(remote_format); free(remote_style);
    remote_name=remote_format=remote_style=NULL;
}

static bool ipc_wait(int fd, short events, double deadline) {
    struct pollfd p={.fd=fd,.events=events};
    while (!interrupted && (!deadline || now()<deadline)) {
        handle_suspend();
        if (proxy_progress) (void)ipc_service();
        int n=poll(&p,1,50);
        if (n>0) return true;
        if (n<0 && errno!=EINTR) return false;
    }
    errno=interrupted ? EINTR : ETIMEDOUT;
    return false;
}

static bool ipc_send_packet(int fd, const void *data, size_t length, double deadline) {
    while (!interrupted) {
        ssize_t n=send(fd,data,length,MSG_DONTWAIT|MSG_NOSIGNAL);
        if (n==(ssize_t)length) return true;
        if (n<0 && errno==EINTR) continue;
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK) && ipc_wait(fd,POLLOUT,deadline)) continue;
        return false;
    }
    return false;
}

static bool ipc_send_request(int fd, const struct ipc_request *request) {
    /* Copy the entire payload before polling: local remote updates can change options. */
    struct ipc_request header=*request;
    char *payload=NULL;
    size_t length=0;
    FILE *stream=open_memstream(&payload,&length);
    if (!stream) return false;
    const char *name=opt.name ? opt.name : "", *fmt=opt.format ? opt.format : "";
    header.name_length=strlen(name)+1; header.format_length=strlen(fmt)+1; header.style_length=strlen(opt.style)+1;
    fwrite(name,1,header.name_length,stream); fwrite(fmt,1,header.format_length,stream);
    fwrite(opt.style,1,header.style_length,stream);
    size_t extra_start=header.name_length+header.format_length+header.style_length;
    for (size_t i=0;i<extra_count;i++) {
        if (extra_displays[i].window) fputs("window",stream);
        if (extra_displays[i].process) fputs(extra_displays[i].window ? ",process" : "process",stream);
        if (extra_displays[i].format) { fputc(':',stream); fputs(extra_displays[i].format,stream); }
        fputc(0,stream);
    }
    if (!extra_count) fputc(0,stream);
    if (fclose(stream)) { free(payload); return false; }
    header.extra_length=length-extra_start;
    double deadline=now()+2;
    bool success=ipc_send_packet(fd,&header,sizeof header,deadline);
    for (size_t offset=0;success && offset<length;) {
        size_t n=length-offset;
        if (n>IPC_CHUNK) n=IPC_CHUNK;
        success=ipc_send_packet(fd,payload+offset,n,deadline); offset+=n;
    }
    free(payload);
    return success;
}

static bool ipc_read_reply(int fd, struct ipc_reply *reply, double deadline) {
    struct ipc_reply_header header;
    if (!ipc_wait(fd,POLLIN,deadline)) return false;
    ssize_t n=recv(fd,&header,sizeof header,MSG_TRUNC);
    size_t length;
    if (n!=(ssize_t)sizeof header || header.version!=IPC_VERSION ||
        !ipc_lengths(header.history_length,header.previous_length,0,&length)) { errno=EPROTO; return false; }
    unsigned char *payload=length ? malloc(length) : NULL;
    if (length && !payload) return false;
    size_t used=0;
    while (used<length) {
        if (!ipc_wait(fd,POLLIN,deadline)) { free(payload); return false; }
        n=recv(fd,payload+used,length-used,MSG_TRUNC);
        if (n<=0 || (size_t)n>length-used) { free(payload); errno=EPROTO; return false; }
        used+=(size_t)n;
    }
    free(reply->data); reply->data=payload; reply->header=header;
    if (header.status) { errno=header.status; return false; }
    return true;
}

static bool ipc_exchange(pid_t pid, const struct ipc_request *request, struct ipc_reply *reply) {
    int fd=ipc_query_fd;
    if (fd<0) {
        struct sockaddr_un address;
        if (!ipc_address(&address,pid,false)) return false;
        fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
        if (fd<0) return false;
        if (connect(fd,(struct sockaddr *)&address,sizeof address) || !ipc_send_request(fd,request)) { close(fd); return false; }
    }
    bool query=request->operation=='Q';
    if (!ipc_read_reply(fd,reply,query ? 0 : now()+2)) { close(fd); ipc_query_fd=-1; return false; }
    if (query) {
        ipc_query_fd=fd;
        struct pollfd pending={.fd=fd,.events=POLLIN};
        while (!reply->header.finished && poll(&pending,1,0)>0 && (pending.revents&POLLIN)) {
            if (!ipc_read_reply(fd,reply,0)) break;
        }
    } else close(fd);
    return true;
}

static bool ipc_install_history(const struct ipc_reply *reply) {
    size_t n=reply->header.history_length;
    if (!reserve_last(n) || !reserve_bytes(&previous_line,&previous_line_capacity,reply->header.previous_length)) return false;
    if (n) memcpy(last_bytes,reply->data,n);
    last_len=n; last_end=last_capacity ? n%last_capacity : 0;
    previous_line_len=reply->header.previous_length;
    if (previous_line_len) memcpy(previous_line,reply->data+n,previous_line_len);
    return true;
}

static int ipc_client(void) {
    struct ipc_request request={
        .version=IPC_VERSION,.operation=remote_pid ? 'R' : 'Q',.mask=changes,
        .size=opt.size,.limit=opt.limit,.buffer=opt.buffer,.display=opt.display,
        .last=opt.last,.width=opt.width,.height=opt.height,.bits=opt.bits,
        .si=opt.si,.gauge=opt.gauge,.no_splice=opt.no_splice,.interval=remote_pid ? opt.interval : fmin(.05,opt.interval),.window=opt.window,
        .history=required_history(),.line=required_line_history(),
        .name_length=strlen(opt.name ? opt.name : "")+1,
        .format_length=strlen(opt.format ? opt.format : "")+1,.style_length=strlen(opt.style)+1
    };
    struct ipc_reply reply={0};
    pid_t pid=remote_pid ? remote_pid : query_pid;
    if (!ipc_exchange(pid,&request,&reply)) {
        fprintf(stderr,"pb: %ld: %s\n",(long)pid,strerror(errno)); free(reply.data); return 1;
    }
    if (remote_pid) { free(reply.data); return 0; }
    if (reply.header.lines!=opt.lines || reply.header.null!=opt.null || reply.header.window!=opt.window) {
        fputs("pb: query line mode, delimiter, and average window must match the target\n",stderr);
        close(ipc_query_fd); ipc_query_fd=-1; free(reply.data); return 1;
    }
    started=now();
    proxy_progress=true;
    if (!write_pidfile()) { close(ipc_query_fd); ipc_query_fd=-1; free(reply.data); return 1; }
    ipc_start();
    struct rate_statistics statistics={0};
    unsigned long long previous_bytes=reply.header.bytes;
    double previous_elapsed=reply.header.progress.elapsed, next=started+opt.interval;
    unsigned long long previous_amount=reply.header.progress.amount;
    bool finished=false;
    int status=0;
    unsigned long generation=ipc_generation;
    while (!interrupted) {
        handle_suspend();
        (void)ipc_service();
        if (generation!=ipc_generation) {
            generation=ipc_generation;
            next=now();
            size_t history=required_history(), line=required_line_history();
            if (history>request.history || line>request.line) {
                close(ipc_query_fd); ipc_query_fd=-1;
                request.history=history; request.line=line;
            }
        }
        if (reply.header.lines!=opt.lines || reply.header.null!=opt.null || reply.header.window!=opt.window) {
            fputs("pb: query settings no longer match the target\n",stderr); status=1; break;
        }
        double t=now();
        if (t>=next || reply.header.finished) {
            view=reply.header.progress;
            double elapsed=view.elapsed>=previous_elapsed ? view.elapsed-previous_elapsed : view.elapsed;
            view.rate=(view.amount>=previous_amount ? view.amount-previous_amount : view.amount)/fmax(elapsed,1e-9);
            view.eta=view.total>view.amount ? (view.average>0 ? (view.total-view.amount)/view.average : -1) : 0;
            unsigned long long byte_delta=reply.header.bytes>=previous_bytes ? reply.header.bytes-previous_bytes : reply.header.bytes;
            statistics_add(&statistics,byte_delta/fmax(elapsed,1e-9));
            previous_bytes=reply.header.bytes;
            atomic_store(&units,view.amount);
            atomic_store(&copied,reply.header.bytes); atomic_store(&buffered,reply.header.buffer);
            if (reply.header.bytes) note_first_write();
            atomic_store(&transfer_engine,reply.header.engine); opt.buffer=reply.header.capacity;
            if (!ipc_install_history(&reply)) { status=64; break; }
            render(reply.header.finished);
            previous_amount=view.amount; previous_elapsed=view.elapsed; next=t+opt.interval;
        }
        if (reply.header.finished) { finished=true; break; }
        double pause=fmin(.05,fmax(0,next-now()));
        struct timespec delay={.tv_sec=0,.tv_nsec=(long)(pause*1e9)};
        nanosleep(&delay,NULL);
        if (!ipc_exchange(pid,&request,&reply)) { finished=true; break; }
    }
    if (finished && !reply.header.finished) { view=reply.header.progress; render(true); }
    statistics_show(&statistics);
    atomic_store(&done,true);
    ipc_close(); remove_pidfile();
    if (ipc_query_fd>=0) close(ipc_query_fd);
    ipc_query_fd=-1;
    free(reply.data); free(last_bytes); free(previous_line);
    last_bytes=previous_line=NULL; last_capacity=last_len=last_end=previous_line_len=previous_line_capacity=0;
    return interrupted ? 32 : status;
}
