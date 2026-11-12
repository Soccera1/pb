/* Common progress fields, format parsing, and terminal/process title output. */
struct format_token {
    const char *begin, *end;
    char *key;
    size_t width;
    bool variable;
};

static const char *field_alias(const char *key) {
    const struct { const char *long_name, *short_name; } aliases[]={
        {"progress","p"},{"timer","t"},{"eta","e"},{"fineta","I"},
        {"rate","r"},{"average-rate","a"},{"bytes","b"},{"transferred","b"},
        {"buffer-percent","T"},{"last-written","A"},{"previous-line","L"},{"name","N"}
    };
    for (size_t i=0;i<sizeof aliases/sizeof aliases[0];i++)
        if (!strcmp(key,aliases[i].long_name)) return aliases[i].short_name;
    return key;
}

static bool bar_field(const char *key) {
    return !strcmp(key,"p") || !strcmp(key,"progress-bar-only") ||
           !strcmp(key,"bar-plain") || !strcmp(key,"bar-block") ||
           !strcmp(key,"bar-granular") || !strcmp(key,"bar-shaded");
}

static struct format_token parse_token(const char *begin) {
    struct format_token token={.begin=begin};
    const char *p=begin+1;
    while (isdigit((unsigned char)*p)) {
        unsigned digit=(unsigned)(*p-'0');
        if (token.width>(SIZE_MAX/2-digit)/10) bad_value("format width overflow");
        token.width=token.width*10+digit;
        p++;
    }
    const char *key=p;
    size_t length=0;
    if (*p=='{') {
        key=++p;
        while (*p && *p!='}') p++;
        if (*p=='}') { length=(size_t)(p-key); p++; }
    } else if (*p) { length=1; p++; }
    token.end=p;
    if (length) {
        token.key=strndup(key,length);
        if (!token.key) exit(64);
        const char *alias=field_alias(token.key);
        token.variable=!token.width && (bar_field(alias) || !strcmp(alias,"L"));
    }
    return token;
}

static size_t capture_requirement(const char *fmt) {
    size_t capture=opt.last;
    if (!fmt) return capture;
    for (const char *p=fmt;*p;) {
        if (*p!='%') { p++; continue; }
        struct format_token token=parse_token(p);
        if (token.key && !strcmp(field_alias(token.key),"A")) {
            size_t n=token.width ? token.width : opt.last;
            if (n>capture) capture=n;
        }
        p=token.end;
        free(token.key);
    }
    return capture;
}

static size_t required_history(void) {
    size_t capture=capture_requirement(opt.format);
    for (size_t i=0;i<extra_count;i++) {
        size_t n=capture_requirement(extra_displays[i].format);
        if (n>capture) capture=n;
    }
    return capture;
}

static size_t line_requirement(const char *fmt) {
    size_t capture=0;
    if (!fmt) return 0;
    for (const char *p=fmt;*p;) {
        if (*p!='%') { p++; continue; }
        struct format_token token=parse_token(p);
        if (token.key && !strcmp(field_alias(token.key),"L")) {
            size_t n=token.width ? token.width : opt.width ? opt.width : g_columns;
            if (n>capture) capture=n;
        }
        p=token.end; free(token.key);
    }
    return capture;
}

static size_t required_line_history(void) {
    size_t capture=line_requirement(opt.format);
    for (size_t i=0;i<extra_count;i++) {
        size_t n=line_requirement(extra_displays[i].format);
        if (n>capture) capture=n;
    }
    /* Retain a transfer buffer's prefix for queries started after a line was written. */
    if (capture<opt.buffer) capture=(size_t)opt.buffer;
    return capture;
}

static bool terminal_colours(void) {
    const char *term=getenv("TERM");
    return term && *term && strcmp(term,"dumb");
}

static bool format_sgr_active;

static void sgr(FILE *f, const char *spec) {
    if (!terminal_colours()) return;
    static const struct { const char *name; int code; } codes[]={
        {"reset",0},{"none",0},{"bold",1},{"dim",2},{"italic",3},
        {"underscore",4},{"underline",4},{"blink",5},{"reverse",7},
        {"no-bold",22},{"no-dim",22},{"no-italic",23},
        {"no-underscore",24},{"no-underline",24},{"no-blink",25},{"no-reverse",27},
        {"black",30},{"red",31},{"green",32},{"brown",33},{"yellow",33},
        {"blue",34},{"magenta",35},{"cyan",36},{"white",37},{"default",39}
    };
    char *copy=strdup(spec);
    if (!copy) return;
    char *save=NULL;
    bool first=true;
    for (char *word=strtok_r(copy,",",&save);word;word=strtok_r(NULL,",",&save)) {
        int add=0;
        if (!strncmp(word,"fg-",3)) word+=3;
        else if (!strncmp(word,"bg-",3)) { add=10; word+=3; }
        int code=-1;
        for (size_t i=0;i<sizeof codes/sizeof codes[0];i++)
            if (!strcmp(word,codes[i].name)) { code=codes[i].code+add; break; }
        if (code<0 && isdigit((unsigned char)*word)) {
            char *end;
            long n=strtol(word,&end,10);
            if (!*end && n>=0 && n<=255) code=(int)n;
        }
        if (code<0) continue;
        fputs(first ? "\033[" : ";",f); first=false;
        fprintf(f,"%d",code);
        format_sgr_active=code!=0;
    }
    if (!first) fputc('m',f);
    free(copy);
}

static void conemu(FILE *f, bool reset) {
    if (reset) fputs("\033]9;4;0;0\033\\",f);
    else if (view.total) fprintf(f,"\033]9;4;1;%.0f\033\\",floor(fmin(100,view.percent)));
    else fputs("\033]9;4;3;0\033\\",f);
    if (!reset) conemu_active=true;
}

static void numeric_amount(FILE *f) {
    char decimal[32];
    snprintf(decimal,sizeof decimal,"%llu",view.amount);
    if (opt.bits && !opt.lines) {
        unsigned carry=0;
        for (size_t i=strlen(decimal);i>0;i--) {
            unsigned n=(unsigned)(decimal[i-1]-'0')*8+carry;
            decimal[i-1]=(char)('0'+n%10); carry=n/10;
        }
        if (carry) fprintf(f,"%u",carry);
    }
    fputs(decimal,f);
}

static void progress_bar(FILE *f, const char *key, size_t width) {
    bool sides=!strcmp(key,"p");
    bool amount=view.total>0 || opt.gauge;
    size_t overhead=sides ? (amount ? 7 : 2) : 0;
    if (width<=overhead) return;
    size_t cells=width-overhead;
    if (sides) fputc('[',f);
    const char *style=!strncmp(key,"bar-",4) ? key+4 : opt.style;
    if (!utf8_output) style="plain";
    double filled=cells*fmin(100,fmax(0,view.percent))/100;
    size_t filled_cells=(size_t)floor(filled);
    size_t travel=cells>2 ? cells-2 : 1;
    size_t pos=animation_step%(2*travel);
    if (pos>travel) pos=2*travel-pos;
    for (size_t i=0;i<cells;i++) {
        if (!amount) {
            if (!strcmp(style,"plain")) fputc(i==pos ? '<' : i==pos+1 ? '=' : i==pos+2 ? '>' : ' ',f);
            else fputs(i>=pos && i<=pos+2 ? "█" : " ",f);
        } else if (!strcmp(style,"plain")) {
            fputc(filled_cells && i+1<filled_cells ? '=' : filled_cells && i+1==filled_cells ? '>' : ' ',f);
        } else if (!strcmp(style,"shaded")) {
            double part=filled-i;
            fputs(part>=1 ? "█" : part>=.75 ? "▓" : part>=.25 ? "▒" : "░",f);
        } else if (!strcmp(style,"granular")) {
            static const char *partial[]={" ","▏","▎","▍","▌","▋","▊","▉"};
            double part=filled-i;
            fputs(part>=1 ? "█" : part>0 ? partial[(int)(part*8)] : " ",f);
        } else fputs(i<filled ? "█" : " ",f);
    }
    if (sides) {
        fputc(']',f);
        if (amount) fprintf(f," %3.0f%%",floor(view.percent));
    }
}

/* Returns false for unknown fields so the original sequence can be preserved. */
static bool field(FILE *f, const char *original, size_t width) {
    const char *key=field_alias(original);
    char s[128];
    if (!strcmp(key,"b")) {
        if (opt.numeric) numeric_amount(f);
        else { human(s,sizeof s,view.amount,false); fputs(s,f); }
    } else if (!strcmp(key,"t")) {
        if (opt.numeric) fprintf(f,"%.4f",view.elapsed);
        else { duration(s,sizeof s,view.elapsed); fputs(s,f); }
    } else if (!strcmp(key,"r") || !strcmp(key,"a")) {
        bool average=key[0]=='a';
        double rate=average ? view.average : view.rate;
        if (opt.numeric) fprintf(f,"%.4f",rate*(opt.bits && !opt.lines ? 8 : 1));
        else { human(s,sizeof s,rate,true); fprintf(f,average ? "(%s)" : "[%s]",s); }
    } else if (!strcmp(key,"e") || !strcmp(key,"I")) {
        if (!view.total) return true;
        if (view.eta<0 || !isfinite(view.eta)) {
            fputs(key[0]=='I' ? "FIN --:--" : "ETA --:--:--",f);
        } else if (key[0]=='I') {
            long double target=(long double)time(NULL)+ceill(view.eta);
            long double maximum=sizeof(time_t)<sizeof(long long) ? INT_MAX : LLONG_MAX;
            struct tm tm;
            if (target>maximum || target<0 || !localtime_r(&(time_t){(time_t)target},&tm) ||
                !strftime(s,sizeof s,view.eta>21600 ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S",&tm)) {
                fputs("FIN --:--",f);
            } else fprintf(f,"FIN %s",s);
        } else { duration(s,sizeof s,view.eta); fprintf(f,"ETA %s",s); }
    } else if (!strcmp(key,"progress-amount-only")) {
        if (opt.numeric) fprintf(f,"%.0f",view.total ? floor(view.percent) : (double)animation_step);
        else if (view.total || opt.gauge) fprintf(f,"%3.0f%%",floor(view.percent));
    } else if (bar_field(key)) {
        progress_bar(f,key,width);
    } else if (!strcmp(key,"progress-conemu")) {
        conemu(f,false);
    } else if (!strncmp(key,"sgr:",4)) {
        sgr(f,key+4);
    } else if (!strcmp(key,"N")) {
        if (opt.name) fprintf(f,"%9s:",opt.name);
    } else if (!strcmp(key,"T")) {
        int engine=atomic_load(&transfer_engine);
        if (engine==ENGINE_SPLICE) fputs("{----}",f);
        else if (engine==ENGINE_COPY) fputs("{--->}",f);
        else if (engine==ENGINE_PIPE) fputs("{-||-}",f);
        else fprintf(f,"{%3.0f%%}",100.0*atomic_load(&buffered)/opt.buffer);
    } else if (!strcmp(key,"A")) {
        pthread_mutex_lock(&history_lock);
        size_t n=width>0 ? (size_t)width : opt.last;
        size_t have=atomic_load(&transfer_engine)!=ENGINE_BUFFERED ? 0 : n>last_len ? last_len : n;
        for (size_t i=have;i<n;i++) fputc('.',f);
                size_t start=have ? (last_end+last_capacity-have)%last_capacity : 0;
        for (size_t i=0;i<have;i++) {
            unsigned char c=last_bytes[(start+i)%last_capacity];
            fputc(isprint(c) ? c : '.',f);
        }
        pthread_mutex_unlock(&history_lock);
    } else if (!strcmp(key,"L")) {
        pthread_mutex_lock(&history_lock);
        size_t n=width>0 ? (size_t)width : 0;
        size_t have=atomic_load(&transfer_engine)!=ENGINE_BUFFERED ? 0 : n>previous_line_len ? previous_line_len : n;
        for (size_t i=0;i<have;i++) fputc(isprint(previous_line[i]) ? previous_line[i] : '.',f);
        for (size_t i=have;i<n;i++) fputc(' ',f);
        pthread_mutex_unlock(&history_lock);
    } else if (!strcmp(key,"ratio")) {
        if (monitor_state || view.ratio>0) {
            if (view.ratio>=1 || view.ratio<=0) fprintf(f,"%.4g:1",view.ratio);
            else fprintf(f,"1:%.4g",1/view.ratio);
        }
    } else if (!strcmp(key,"%")) fputc('%',f);
    else return false;
    return true;
}

/* Count terminal cells while ignoring CSI and OSC sequences. */
static size_t visible_columns(const char *text, size_t length) {
    size_t columns=0;
    mbstate_t state={0};
    for (size_t i=0;i<length;) {
        unsigned char c=(unsigned char)text[i];
        if (c==27 && i+1<length) {
            if (text[i+1]=='[') {
                i+=2;
                while (i<length && !((unsigned char)text[i]>=0x40 && (unsigned char)text[i]<=0x7e)) i++;
                if (i<length) i++;
                continue;
            }
            if (text[i+1]==']') {
                i+=2;
                while (i<length && text[i]!=7 && !(text[i]==27 && i+1<length && text[i+1]=='\\')) i++;
                if (i<length) i+=text[i]==7 ? 1 : 2;
                continue;
            }
        }
        wchar_t wc;
        size_t n=mbrtowc(&wc,text+i,length-i,&state);
        if (n==(size_t)-1 || n==(size_t)-2 || !n) { memset(&state,0,sizeof state); i++; columns++; }
        else { int cells=wcwidth(wc); if (cells>0) columns+=(size_t)cells; i+=n; }
    }
    return columns;
}

static void format(FILE *f, const char *fmt) {
    format_sgr_active=false;
    size_t fixed=0, variable_count=0;
    /* Measure fixed fields once; variable fields are assigned remaining cells. */
    struct piece { char *text; size_t length; struct format_token token; };
    struct piece *pieces=NULL;
    size_t count=0;
    for (const char *p=fmt;*p;) {
        struct piece piece={0};
        if (*p!='%') {
            const char *start=p;
            while (*p && *p!='%') p++;
            piece.text=strndup(start,(size_t)(p-start));
            piece.length=(size_t)(p-start);
        } else {
            piece.token=parse_token(p); p=piece.token.end;
            if (piece.token.variable) variable_count++;
            else {
                FILE *stream=open_memstream(&piece.text,&piece.length);
                if (!stream) exit(64);
                if (!piece.token.key || !field(stream,piece.token.key,piece.token.width))
                    fwrite(piece.token.begin,1,(size_t)(piece.token.end-piece.token.begin),stream);
                if (fclose(stream)) exit(64);
            }
        }
        if (piece.text) fixed+=visible_columns(piece.text,piece.length);
        struct piece *replacement=realloc(pieces,(count+1)*sizeof *pieces);
        if (!replacement) exit(64);
        pieces=replacement; pieces[count++]=piece;
    }
    size_t available=(size_t)g_columns>fixed ? (size_t)g_columns-fixed : 0;
    for (size_t i=0;i<count;i++) {
        if (pieces[i].token.variable) {
            size_t width=variable_count ? available/variable_count : 0;
            if (width) field(f,pieces[i].token.key,width);
            available-=width; variable_count--;
        } else if (pieces[i].text) fwrite(pieces[i].text,1,pieces[i].length,f);
        free(pieces[i].text); free(pieces[i].token.key);
    }
    free(pieces);
    if (format_sgr_active) fputs("\033[0m",f);
}

static char *default_format(void) {
    char *text=NULL;
    size_t length=0;
    FILE *f=open_memstream(&text,&length);
    if (!f) exit(64);
    bool space=false;
    if (opt.numeric) {
        if (opt.display&TIMER) { fputs("%t",f); space=true; }
        if (opt.display&BYTES) { fputs(space ? " %b" : "%b",f); space=true; }
        if (opt.display&RATE) { fputs(space ? " %r" : "%r",f); space=true; }
        if (!(opt.display&(BYTES|RATE))) fputs(space ? " %{progress-amount-only}" : "%{progress-amount-only}",f);
    } else {
        if (opt.name) { fputs("%N",f); space=true; }
        const struct { unsigned flag; const char *key; } fields[]={
            {BYTES,"%b"},{TIMER,"%t"},{RATE,"%r"},{AVERAGE,"%a"},
            {BUFFER,"%T"},{LAST,"%A"},{PROGRESS,"%p"},{ETA,"%e"},{FINETA,"%I"}
        };
        for (size_t i=0;i<sizeof fields/sizeof fields[0];i++) if (opt.display&fields[i].flag) {
            if ((fields[i].flag&(ETA|FINETA)) && !view.total) continue;
            if (space) fputc(' ',f);
            fputs(fields[i].key,f); space=true;
        }
    }
    if (monitor_state && monitor_index==1) fputs(" %{ratio}",f);
    if (fclose(f)) exit(64);
    return text;
}

static char *plain_title(const char *text) {
    char *plain=malloc(strlen(text)+1);
    if (!plain) return NULL;
    size_t out=0;
    for (size_t i=0;text[i];) {
        if (text[i]==27 && text[i+1]=='[') {
            i+=2;
            while (text[i] && !((unsigned char)text[i]>=0x40 && (unsigned char)text[i]<=0x7e)) i++;
            if (text[i]) i++;
        } else if (text[i]==27 && text[i+1]==']') {
            i+=2;
            while (text[i] && text[i]!=7 && !(text[i]==27 && text[i+1]=='\\')) i++;
            if (text[i]) i+=text[i]==7 ? 1 : 2;
        } else {
            unsigned char c=(unsigned char)text[i++];
            if (c>=32 && c!=127) plain[out++]=(char)c;
        }
    }
    plain[out]=0;
    return plain;
}

static void extra_render(const char *default_fmt) {
    for (size_t i=0;i<extra_count;i++) {
        char *text=NULL;
        size_t length=0;
        FILE *f=open_memstream(&text,&length);
        if (!f) continue;
        format(f,extra_displays[i].format ? extra_displays[i].format : default_fmt);
        if (fclose(f)) { free(text); continue; }
        char *title=plain_title(text);
        free(text);
        if (!title) continue;
        if (extra_displays[i].window) fprintf(stderr,"\033]0;%s\007",title);
        if (extra_displays[i].process) {
            if (title_capacity) {
                memset(title_region,0,title_capacity);
                snprintf(title_region,title_capacity,"pb -- %s",title);
            }
        }
        free(title);
    }
}
