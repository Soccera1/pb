/* Test-only I/O shim: deterministic read faults, EINTR, and partial writes. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ssize_t read(int fd, void *buf, size_t count) {
    static ssize_t (*real_read)(int, void *, size_t);
    if (!real_read) *(void **)(&real_read)=dlsym(RTLD_NEXT,"read");
    char link[64], path[4096];
    snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(link,path,sizeof path-1);
    if (n>0) {
        path[n]=0;
        if (strstr(path,"fault-input")) {
            off_t pos=lseek(fd,0,SEEK_CUR);
            if (pos>=8 && pos<12) { errno=EIO; return -1; }
            if (pos<8 && count>(size_t)(8-pos)) count=(size_t)(8-pos);
        }
    }
    return real_read(fd,buf,count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    static ssize_t (*real_write)(int, const void *, size_t);
    static int interrupted;
    if (!real_write) *(void **)(&real_write)=dlsym(RTLD_NEXT,"write");
    if (fd==STDOUT_FILENO && getenv("PB_TEST_PARTIAL")) {
        if (!interrupted++) { errno=EINTR; return -1; }
        if (count>7) count=7;
    }
    return real_write(fd,buf,count);
}

/* Simulate device metadata without accessing a real block device. */
#include <fcntl.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

static int named_fd(int fd, const char *name) {
    char link[64], path[4096];
    snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(link,path,sizeof path-1);
    if (n<0) return 0;
    path[n]=0;
    return strstr(path,name)!=NULL;
}

int fstat(int fd, struct stat *st) {
    static int (*real_fstat)(int, struct stat *);
    if (!real_fstat) *(void **)(&real_fstat)=dlsym(RTLD_NEXT,"fstat");
    if (named_fd(fd,"stat-error")) { errno=EIO; return -1; }
    int result=real_fstat(fd,st);
    if (!result && named_fd(fd,"fake-block")) { st->st_mode=(st->st_mode&~S_IFMT)|S_IFBLK; st->st_size=0; }
    if (!result && named_fd(fd,"large-blocksize")) st->st_blksize=32768;
    return result;
}

int ioctl(int fd, unsigned long request, ...) {
    static int (*real_ioctl)(int, unsigned long, ...);
    if (!real_ioctl) *(void **)(&real_ioctl)=dlsym(RTLD_NEXT,"ioctl");
    va_list args;
    va_start(args,request);
    void *arg=va_arg(args,void *);
    va_end(args);
    if (request==BLKGETSIZE64 && named_fd(fd,"fake-block")) {
        static int (*real_fstat)(int, struct stat *);
        if (!real_fstat) *(void **)(&real_fstat)=dlsym(RTLD_NEXT,"fstat");
        struct stat st;
        if (real_fstat(fd,&st)) return -1;
        *(unsigned long long *)arg=(unsigned long long)st.st_size;
        return 0;
    }
    return real_ioctl(fd,request,arg);
}

static void log_fast(const char *name) {
    const char *path=getenv("PB_TEST_FAST_LOG");
    if (!path) return;
    int fd=open(path,O_WRONLY|O_CREAT|O_APPEND,0600);
    if (fd<0) return;
    ssize_t written=write(fd,name,strlen(name));
    (void)written;
    close(fd);
}

ssize_t copy_file_range(int in, off_t *in_offset, int out, off_t *out_offset,
                        size_t count, unsigned flags) {
    static ssize_t (*real_copy)(int, off_t *, int, off_t *, size_t, unsigned);
    if (!real_copy) *(void **)(&real_copy)=dlsym(RTLD_NEXT,"copy_file_range");
    log_fast("copy\n");
    if (getenv("PB_TEST_COPY_UNSUPPORTED")) { errno=EXDEV; return -1; }
    return real_copy(in,in_offset,out,out_offset,count,flags);
}

ssize_t splice(int in, off_t *in_offset, int out, off_t *out_offset,
               size_t count, unsigned flags) {
    static ssize_t (*real_splice)(int, off_t *, int, off_t *, size_t, unsigned);
    if (!real_splice) *(void **)(&real_splice)=dlsym(RTLD_NEXT,"splice");
    log_fast("splice\n");
    if (getenv("PB_TEST_SPLICE_UNSUPPORTED")) { errno=EINVAL; return -1; }
    struct stat st;
    const char *gate=getenv("PB_TEST_DRAIN_WAITING");
    if (gate && !fstat(in,&st) && S_ISFIFO(st.st_mode) && access(gate,F_OK)) {
        log_fast("pending\n"); errno=EAGAIN; return -1;
    }
    if (getenv("PB_TEST_DRAIN_UNSUPPORTED") && !fstat(in,&st) && S_ISFIFO(st.st_mode)) {
        errno=EINVAL; return -1;
    }
    return real_splice(in,in_offset,out,out_offset,count,flags);
}


int stat(const char *path, struct stat *st) {
    static int (*real_stat)(const char *, struct stat *);
    if (!real_stat) *(void **)(&real_stat)=dlsym(RTLD_NEXT,"stat");
    int result=real_stat(path,st);
    char target[4096];
    int named=strstr(path,"fake-block")!=NULL;
    if (!named) {
        ssize_t n=readlink(path,target,sizeof target-1);
        if (n>0) { target[n]=0; named=strstr(target,"fake-block")!=NULL; }
    }
    if (!result && named) { st->st_mode=(st->st_mode&~S_IFMT)|S_IFBLK; st->st_size=0; }
    return result;
}


int fdatasync(int fd) {
    static int (*real_sync)(int);
    if (!real_sync) *(void **)(&real_sync)=dlsym(RTLD_NEXT,"fdatasync");
    if (named_fd(fd,"fake-block")) log_fast("sync\n");
    return real_sync(fd);
}

int close(int fd) {
    static int (*real_close)(int);
    if (!real_close) *(void **)(&real_close)=dlsym(RTLD_NEXT,"close");
    int fail=named_fd(fd,"close-error");
    int result=real_close(fd);
    if (!result && fail) { errno=EIO; return -1; }
    return result;
}

int posix_memalign(void **pointer, size_t alignment, size_t size) {
    static int (*real_allocate)(void **, size_t, size_t);
    if (!real_allocate) *(void **)(&real_allocate)=dlsym(RTLD_NEXT,"posix_memalign");
    if (getenv("PB_TEST_ALLOC_FAIL")) return ENOMEM;
    char message[80];
    snprintf(message,sizeof message,"allocate:%zu\n",size);
    const char *log=getenv("PB_TEST_ALLOC_LOG");
    if (log) {
        int fd=open(log,O_WRONLY|O_CREAT|O_APPEND,0600);
        if (fd>=0) { ssize_t n=write(fd,message,strlen(message)); (void)n; close(fd); }
    }
    return real_allocate(pointer,alignment,size);
}
