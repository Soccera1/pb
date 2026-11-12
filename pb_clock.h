/* One active-time clock is shared by the copy and display threads. */
static atomic_llong paused_nanoseconds;
static atomic_ullong first_write_nanoseconds, stage_nanoseconds;
static volatile sig_atomic_t suspend_requested, continued;

static double wall_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec/1e9;
}

static double now(void) {
    long long pause=atomic_load(&paused_nanoseconds);
    if (monitor_state) pause+=atomic_load(&monitor_state->paused);
    return wall_now()-pause/1e9;
}

static void note_first_write(void) {
    if (!atomic_load(&first_write_nanoseconds)) {
        unsigned long long zero=0, timestamp=(unsigned long long)(now()*1e9);
        atomic_compare_exchange_strong(&first_write_nanoseconds,&zero,timestamp);
    }
}

static double transfer_origin(double current) {
    unsigned long long first=atomic_load(&first_write_nanoseconds);
    if (opt.wait) return first ? first/1e9 : current;
    unsigned long long stage=atomic_load(&stage_nanoseconds);
    return stage ? stage/1e9 : started;
}

/* Called only by the main thread, outside all transfer/display locks. */
static void handle_suspend(void) {
    if (!suspend_requested) {
        if (continued) { continued=0; resized=1; clearerr(stderr); }
        return;
    }
    suspend_requested=0; continued=0;
    double began=wall_now();
    long long shared_before=monitor_state ? atomic_load(&monitor_state->paused) : 0;
    bool coordinator=monitor_state && monitor_state->coordinator==getpid();
    if (coordinator) atomic_store(&monitor_state->suspended,true);
    if (monitor_command_pid>0 && !monitor_command_done)
        kill(monitor_command_group ? -monitor_command_pid : monitor_command_pid,SIGSTOP);
    if (monitor_output_pid>0 && !monitor_output_done) kill(monitor_output_pid,SIGSTOP);
    /* SIGSTOP also works for orphaned groups, unlike default SIGTSTP handling. */
    kill(getpid(),SIGSTOP);
    double paused=wall_now()-began;
    if (coordinator) {
        atomic_fetch_add(&monitor_state->paused,(long long)(paused*1e9));
        atomic_store(&monitor_state->suspended,false);
    } else {
        while (monitor_state && atomic_load(&monitor_state->suspended)) {
            if (kill(monitor_state->coordinator,0) && errno==ESRCH) break;
            struct timespec delay={.tv_sec=0,.tv_nsec=1000000};
            nanosleep(&delay,NULL);
        }
        long long shared=monitor_state ? atomic_load(&monitor_state->paused)-shared_before : 0;
        long long local=(long long)(paused*1e9)-shared;
        if (local>0) atomic_fetch_add(&paused_nanoseconds,local);
    }
    if (monitor_command_pid>0 && !monitor_command_done)
        kill(monitor_command_group ? -monitor_command_pid : monitor_command_pid,SIGCONT);
    if (monitor_output_pid>0 && !monitor_output_done) kill(monitor_output_pid,SIGCONT);
    continued=0; resized=1; clearerr(stderr);
}
