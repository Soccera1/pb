/* Set up a command pipeline before starting any transfer threads. */
static void monitor_close_pipe(int pipefd[2]) {
    for (int i=0;i<2;i++) if (pipefd[i]>=0) close(pipefd[i]);
    pipefd[0]=pipefd[1]=-1;
}

static bool monitor_setup(void) {
    int incoming[2]={-1,-1}, outgoing[2]={-1,-1};
    bool input=monitor_side!=1, output=monitor_side!=0;
    monitor_command_group=!isatty(STDIN_FILENO) && !isatty(STDERR_FILENO);
    int final_output=output_fd;
    if ((input && pipe2(incoming,O_CLOEXEC)) || (output && pipe2(outgoing,O_CLOEXEC))) {
        perror("pb: monitor pipe"); monitor_close_pipe(incoming); monitor_close_pipe(outgoing); return false;
    }
    if (monitor_side==2) {
        monitor_state=mmap(NULL,sizeof *monitor_state,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
        if (monitor_state==MAP_FAILED) { monitor_state=NULL; goto failed; }
        atomic_init(&monitor_state->bytes[0],0); atomic_init(&monitor_state->bytes[1],0);
        atomic_init(&monitor_state->paused,0); atomic_init(&monitor_state->suspended,false);
        monitor_state->coordinator=getpid();
        pthread_mutexattr_t attr;
        int err=pthread_mutexattr_init(&attr);
        if (err) { errno=err; goto failed; }
        err=pthread_mutexattr_setpshared(&attr,PTHREAD_PROCESS_SHARED);
#ifdef __linux__
        if (!err) err=pthread_mutexattr_setrobust(&attr,PTHREAD_MUTEX_ROBUST);
#endif
        if (!err) err=pthread_mutex_init(&monitor_state->display_lock,&attr);
        pthread_mutexattr_destroy(&attr);
        if (err) { errno=err; goto failed; }
        monitor_mutex_ready=true;
        if (!opt.quiet && !opt.numeric && !opt.cursor && isatty(STDERR_FILENO)) {
            fputs("\n\n\033[2A",stderr); save_cursor(); fflush(stderr);
        }
    }
    monitor_command_pid=fork();
    if (monitor_command_pid<0) goto failed;
    if (!monitor_command_pid) {
        if (monitor_command_group) setpgid(0,0);
        signal(SIGPIPE,SIG_DFL);
        if ((input && dup2(incoming[0],STDIN_FILENO)<0) ||
            (output && dup2(outgoing[1],STDOUT_FILENO)<0) ||
            (!output && final_output!=STDOUT_FILENO && dup2(final_output,STDOUT_FILENO)<0)) {
            perror("pb: monitor dup2"); _exit(127);
        }
        monitor_close_pipe(incoming); monitor_close_pipe(outgoing);
        if (final_output!=STDOUT_FILENO) close(final_output);
        execvp(monitor_command[0],monitor_command);
        perror("pb: exec"); _exit(127);
    }
    if (monitor_command_group) (void)setpgid(monitor_command_pid,monitor_command_pid);
    if (incoming[0]>=0) { close(incoming[0]); incoming[0]=-1; }
    if (outgoing[1]>=0) { close(outgoing[1]); outgoing[1]=-1; }
    if (monitor_side==2) {
        monitor_output_pid=fork();
        if (monitor_output_pid<0) goto failed;
        if (!monitor_output_pid) {
            monitor_output_child=true;
            monitor_command_pid=monitor_output_pid=-1;
            monitor_index=1;
            close(incoming[1]); incoming[1]=-1;
            if (dup2(outgoing[0],STDIN_FILENO)<0) { perror("pb: monitor output dup2"); _exit(1); }
            close(outgoing[0]); outgoing[0]=-1;
            output_fd=final_output;
            opt.name=monitor_names[1] ? monitor_names[1] : "out";
            opt.format=monitor_formats[1] ? monitor_formats[1] : monitor_formats[0];
            opt.pidfile=monitor_pidfiles[1];
            /* Command output is a new stream, with an unknown inferred size. */
            return true;
        }
        close(outgoing[0]); outgoing[0]=-1;
    } else if (output) {
        if (dup2(outgoing[0],STDIN_FILENO)<0) goto failed;
        close(outgoing[0]); outgoing[0]=-1;
    }
    if (input) {
        output_fd=incoming[1]; incoming[1]=-1;
        if (final_output!=STDOUT_FILENO) close(final_output);
    }
    return true;
failed:
    perror("pb: monitor");
    monitor_close_pipe(incoming); monitor_close_pipe(outgoing);
    if (monitor_command_pid>0) kill(monitor_command_group ? -monitor_command_pid : monitor_command_pid,SIGTERM);
    if (monitor_output_pid>0) kill(monitor_output_pid,SIGTERM);
    return false;
}

static void monitor_poll(void) {
    int status;
    if (monitor_command_pid>0 && !monitor_command_done) {
        pid_t child=waitpid(monitor_command_pid,&status,WNOHANG);
        if (child==monitor_command_pid || (child<0 && errno==ECHILD)) {
            monitor_command_done=true;
            /* Do not wait for stdin after the consumer exits. */
            if (monitor_side!=1) atomic_store(&cancelled,true);
        }
    }
    if (monitor_output_pid>0 && !monitor_output_done) {
        pid_t child=waitpid(monitor_output_pid,&status,WNOHANG);
        if (child==monitor_output_pid) {
            monitor_output_done=true;
            atomic_store(&cancelled,true);
            if (WIFEXITED(status)) {
                int child_status=WEXITSTATUS(status);
                if (monitor_terminating && !interrupted) child_status&=~32;
                atomic_fetch_or(&result,child_status);
                monitor_output_failed=child_status!=0;
            } else if (WIFSIGNALED(status) && !interrupted) {
                atomic_fetch_or(&result,1); monitor_output_failed=true;
            }
            if (monitor_output_failed && !monitor_command_done && monitor_command_pid>0)
                kill(monitor_command_group ? -monitor_command_pid : monitor_command_pid,SIGTERM);
        } else if (child<0 && errno==ECHILD) monitor_output_done=true;
    }
}

static void monitor_finish(void) {
    if (monitor_side<0) return;
    double signalled=0;
    if (monitor_output_pid<=0) monitor_output_done=true;
    while ((!monitor_command_done && monitor_command_pid>0) || !monitor_output_done) {
        handle_suspend();
        monitor_poll();
        if (interrupted || monitor_output_failed || atomic_load(&result)) {
            if (!signalled) {
                monitor_terminating=true;
                if (!monitor_command_done && monitor_command_pid>0) kill(monitor_command_group ? -monitor_command_pid : monitor_command_pid,SIGTERM);
                if (!monitor_output_done) kill(monitor_output_pid,SIGTERM);
                signalled=now();
            } else if (now()-signalled>1) {
                if (!monitor_command_done && monitor_command_pid>0) kill(monitor_command_group ? -monitor_command_pid : monitor_command_pid,SIGKILL);
                if (!monitor_output_done) kill(monitor_output_pid,SIGKILL);
            }
        }
        struct timespec pause={.tv_sec=0,.tv_nsec=10000000};
        nanosleep(&pause,NULL);
    }
    if (monitor_state) {
        if (!monitor_output_child && monitor_mutex_ready) pthread_mutex_destroy(&monitor_state->display_lock);
        munmap(monitor_state,sizeof *monitor_state); monitor_state=NULL;
    }
}
