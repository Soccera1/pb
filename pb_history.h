/* Display history grows to the requested fields; writes use a circular buffer. */
static size_t last_capacity, last_end, current_line_capacity, previous_line_capacity;
static atomic_size_t live_line;

static bool reserve_bytes(unsigned char **data, size_t *capacity, size_t wanted) {
    if (*capacity>=wanted) return true;
    size_t n=*capacity ? *capacity : 256;
    while (n<wanted) {
        if (n>SIZE_MAX/2) { n=wanted; break; }
        n*=2;
    }
    unsigned char *replacement=realloc(*data,n);
    if (!replacement) return false;
    *data=replacement; *capacity=n;
    return true;
}

static void copy_last(unsigned char *out, size_t wanted) {
    if (!wanted) return;
    size_t start=(last_end+last_capacity-wanted)%last_capacity;
    size_t first=last_capacity-start;
    if (first>wanted) first=wanted;
    memcpy(out,last_bytes+start,first);
    memcpy(out+first,last_bytes,wanted-first);
}

static bool reserve_last(size_t wanted) {
    if (last_capacity>=wanted) return true;
    unsigned char *replacement=NULL;
    size_t capacity=0;
    if (!reserve_bytes(&replacement,&capacity,wanted)) return false;
    copy_last(replacement,last_len);
    free(last_bytes);
    last_bytes=replacement; last_capacity=capacity; last_end=last_len;
    return true;
}

static void append_last(const unsigned char *data, size_t n) {
    if (!last_capacity) return;
    if (n>last_capacity) { data+=n-last_capacity; n=last_capacity; }
    size_t first=last_capacity-last_end;
    if (first>n) first=n;
    memcpy(last_bytes+last_end,data,first);
    memcpy(last_bytes,data+first,n-first);
    last_end=(last_end+n)%last_capacity;
    last_len=last_capacity-last_len<n ? last_capacity : last_len+n;
}

static bool append_lines(const unsigned char *data, size_t n, size_t capture) {
    if (!capture) return true;
    while (n) {
        const unsigned char *end=memchr(data,opt.null ? 0 : '\n',n);
        size_t length=end ? (size_t)(end-data) : n;
        size_t add=current_line_len<capture ? capture-current_line_len : 0;
        if (add>length) add=length;
        if (!reserve_bytes(&current_line,&current_line_capacity,current_line_len+add)) return false;
        if (add) memcpy(current_line+current_line_len,data,add);
        current_line_len+=add;
        data+=length; n-=length;
        if (end) {
            if (!reserve_bytes(&previous_line,&previous_line_capacity,current_line_len)) return false;
            if (current_line_len) memcpy(previous_line,current_line,current_line_len);
            previous_line_len=current_line_len; current_line_len=0;
            data++; n--;
        }
    }
    return true;
}
