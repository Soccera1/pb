/* Linux stream metadata and optional kernel-assisted transfer paths. */
static bool ready(int fd, short events);

enum transfer_engine { ENGINE_BUFFERED, ENGINE_SPLICE, ENGINE_COPY, ENGINE_PIPE };
static atomic_int transfer_engine;
static atomic_bool live_readwrite;

static bool fd_size(int fd, unsigned long long *size) {
    struct stat st;
    if (fstat(fd,&st)) return false;
    if (S_ISREG(st.st_mode)) { *size=st.st_size; return true; }
#ifdef BLKGETSIZE64
    if (S_ISBLK(st.st_mode)) {
        uint64_t bytes;
        if (!ioctl(fd,BLKGETSIZE64,&bytes)) { *size=bytes; return true; }
    }
#endif
    return false;
}

static bool pipe_fd(int fd) {
    struct stat st;
    return !fstat(fd,&st) && S_ISFIFO(st.st_mode);
}

static void configure_pipes(int in, int out) {
#ifdef F_SETPIPE_SZ
    if (opt.pipe_size) {
        if (pipe_fd(out)) (void)fcntl(out,F_SETPIPE_SZ,(int)opt.pipe_size);
    } else if (pipe_fd(in) && pipe_fd(out)) {
        int input_size=fcntl(in,F_GETPIPE_SZ), output_size=fcntl(out,F_GETPIPE_SZ);
        if (input_size>output_size && output_size>0) (void)fcntl(out,F_SETPIPE_SZ,input_size);
    }
#else
    (void)in; (void)out;
#endif
}

struct fast_transfer {
    bool enabled, copy, direct, buffered_pipe;
    int pipe[2];
    size_t pending;
};
enum fast_action { FAST_FALLBACK, FAST_BUFFERED, FAST_MOVED, FAST_EOF, FAST_RETRY, FAST_ERROR };
struct fast_result { enum fast_action action; ssize_t count; bool output_error; };

static bool unsupported_copy(int error) {
    return error==EINVAL || error==ENOSYS || error==EXDEV || error==EOPNOTSUPP ||
           error==EBADF || error==EPERM || error==EACCES;
}

static void fast_close(struct fast_transfer *state) {
    if (state->pipe[0]>=0) close(state->pipe[0]);
    if (state->pipe[1]>=0) close(state->pipe[1]);
    state->pipe[0]=state->pipe[1]=-1;
}

static struct fast_transfer fast_init(int in, int out, bool discard) {
    configure_pipes(in,out);
    struct fast_transfer state={.pipe={-1,-1}};
    state.enabled=!atomic_load(&live_readwrite) && !discard && !opt.lines && !opt.direct && !opt.sparse && !opt.skip_errors;
    struct stat input, output;
    if (fstat(in,&input) || fstat(out,&output)) state.enabled=false;
    else {
        state.copy=S_ISREG(input.st_mode) && S_ISREG(output.st_mode);
        state.direct=S_ISFIFO(input.st_mode) || S_ISFIFO(output.st_mode);
    }
    atomic_store(&transfer_engine,ENGINE_BUFFERED);
    return state;
}

static struct fast_result fast_count(ssize_t n, bool output_error) {
    if (n>0) return (struct fast_result){FAST_MOVED,n,false};
    if (!n) return (struct fast_result){FAST_EOF,0,false};
    if (errno==EINTR) return (struct fast_result){FAST_RETRY,0,false};
    if (errno==EAGAIN || errno==EWOULDBLOCK) {
        (void)poll(NULL,0,1);
        return (struct fast_result){FAST_RETRY,0,false};
    }
    return (struct fast_result){FAST_ERROR,-1,output_error};
}

static struct fast_result fast_step(struct fast_transfer *state, int in, int out,
                                    unsigned char *buffer, size_t count) {
    if (atomic_load(&live_readwrite)) state->enabled=false;
    if (state->pending) {
        size_t want=state->pending<count ? state->pending : count;
        if (!state->buffered_pipe && state->enabled) {
            atomic_store(&transfer_engine,ENGINE_PIPE);
            if (!ready(out,POLLOUT)) return (struct fast_result){FAST_ERROR,-1,true};
            if (atomic_load(&live_readwrite)) return (struct fast_result){FAST_RETRY,0,false};
            ssize_t n=splice(state->pipe[0],NULL,out,NULL,want,SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
            if (n>=0) {
                state->pending-=(size_t)n;
                atomic_store(&buffered,state->pending);
                return fast_count(n,true);
            }
            if (!unsupported_copy(errno)) return fast_count(n,true);
        }
        /* Preserve pages already consumed from the input when splice is disabled. */
        state->buffered_pipe=true; state->enabled=false;
        atomic_store(&transfer_engine,ENGINE_BUFFERED);
        ssize_t n=read(state->pipe[0],buffer,want);
        if (n>0) { state->pending-=(size_t)n; return (struct fast_result){FAST_BUFFERED,n,false}; }
        if (!n) { errno=EIO; return (struct fast_result){FAST_ERROR,-1,false}; }
        return fast_count(n,false);
    }
    if (!state->enabled) {
        fast_close(state);
        atomic_store(&transfer_engine,ENGINE_BUFFERED);
        return (struct fast_result){FAST_FALLBACK,0,false};
    }
    if (state->copy) {
        atomic_store(&transfer_engine,ENGINE_COPY);
        ssize_t n=copy_file_range(in,NULL,out,NULL,count,0);
        if (n>=0 || !unsupported_copy(errno)) return fast_count(n,true);
        state->copy=false;
    }
    if (state->direct) {
        atomic_store(&transfer_engine,ENGINE_SPLICE);
        if (!ready(in,POLLIN) || !ready(out,POLLOUT)) return (struct fast_result){FAST_ERROR,-1,true};
        if (atomic_load(&live_readwrite)) return (struct fast_result){FAST_RETRY,0,false};
        ssize_t n=splice(in,NULL,out,NULL,count,SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
        if (n>=0 || !unsupported_copy(errno)) return fast_count(n,true);
        state->enabled=false;
        atomic_store(&transfer_engine,ENGINE_BUFFERED);
        return (struct fast_result){FAST_FALLBACK,0,false};
    }
    if (state->pipe[0]<0) {
        if (pipe2(state->pipe,O_CLOEXEC|O_NONBLOCK)) {
            state->enabled=false;
            atomic_store(&transfer_engine,ENGINE_BUFFERED);
            return (struct fast_result){FAST_FALLBACK,0,false};
        }
#ifdef F_SETPIPE_SZ
        if (opt.pipe_size) (void)fcntl(state->pipe[1],F_SETPIPE_SZ,(int)opt.pipe_size);
#endif
    }
    atomic_store(&transfer_engine,ENGINE_PIPE);
    if (!ready(in,POLLIN)) return (struct fast_result){FAST_ERROR,-1,false};
    if (atomic_load(&live_readwrite)) return (struct fast_result){FAST_RETRY,0,false};
    ssize_t n=splice(in,NULL,state->pipe[1],NULL,count,SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
    if (n>0) {
        state->pending=(size_t)n;
        atomic_store(&buffered,state->pending);
        return (struct fast_result){FAST_RETRY,0,false};
    }
    if (n==0 || !unsupported_copy(errno)) return fast_count(n,false);
    state->enabled=false;
    atomic_store(&transfer_engine,ENGINE_BUFFERED);
    return (struct fast_result){FAST_FALLBACK,0,false};
}

static void record_fast(size_t bytes) {
    note_first_write();
    if (monitor_state) atomic_fetch_add(&monitor_state->bytes[monitor_index],bytes);
    atomic_fetch_add(&copied,bytes);
    atomic_fetch_add(&units,bytes);
}
