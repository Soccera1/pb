/* Original pb large terminal display. Included by pb.c. */
enum Number {
	NUM_0 = 0,
	NUM_1,
	NUM_2,
	NUM_3,
	NUM_4,
	NUM_5,
	NUM_6,
	NUM_7,
	NUM_8,
	NUM_9,
	NUM_PERCENT,
	NUM_NONE,
};

static void print_big_number(int i) {
	static const char *font[][2] = {
		{"█▀█",
		 "█▄█"},

		{"▀█ ",
		 "▄█▄"},

		{"▀▀█",
		 "██▄"},

		{"▀▀█",
		 "▄██"},

		{"█ █",
		 " ▀█"},

		{"█▀▀",
		 "▄▄█"},

		{"█▀▀",
		 "█▄█"},

		{"▀▀█",
		 " █ "},

		{"█▀█",
		 "███"},

		{"█▀█",
		 "▀▀█"},

		{"▀▄▀",
		 "▀ ▀"},

		{"   ",
		 "   "},
	};
	fprintf(stderr, "%s", font[i][0]);
	fprintf(stderr, "\033[1B\033[3D");
	fprintf(stderr, "%s", font[i][1]);
	fprintf(stderr, "\033[1A\033[1C");
}

static void save_cursor(void) {
	fprintf(stderr, "\033[s");
}

static void restore_cursor(void) {
	fprintf(stderr, "\033[u");
}

static void draw_block_row(unsigned long long pct_copied) {
	const char *block_7_8 = "▉";
	for (size_t i = 0; i < g_columns - 2; i++) {
		int pct = (int)(100.0L * i / (g_columns - 2));

		if ((unsigned long long)pct < pct_copied) {
			fprintf(stderr, "%s", GREEN);
		} else {
			fprintf(stderr, "%s", DIM);
		}
		fprintf(stderr, "%s", block_7_8);
		fprintf(stderr, "%s", RST);
	}
}

static void draw_activity_row(unsigned step) {
    size_t length=g_columns-2;
    size_t travel=length>5 ? length-5 : 1;
    size_t position=step%(travel*2);
    if (position>travel) position=travel*2-position;
    for (size_t i=0;i<length;i++) {
        fputs(i>=position && i<position+5 ? GREEN : DIM,stderr);
        fputs("▉",stderr);
        fputs(RST,stderr);
    }
}

static void print_percent(unsigned long long pct) {
	enum Number hundreds;
	enum Number tens;
	enum Number ones;

	unsigned long long tmp;

	if (pct < 100) {
		hundreds = NUM_NONE;
	} else {
		tmp = pct / 100;
		tmp %= 10;
		hundreds = tmp;
	}

	if (pct < 10) {
		tens = NUM_NONE;
	} else {
		tmp = pct / 10;
		tmp %= 10;
		tens = tmp;
	}

	ones = pct % 10;

	print_big_number(hundreds);
	print_big_number(tens);
	print_big_number(ones);
	print_big_number(NUM_PERCENT);
}

static void repeat_text(const char *s, size_t times) {
	for (size_t i = 0; i < times; i++) {
		fprintf(stderr, "%s", s);
	}
}

static size_t print_ui_name(size_t available) {
    if (!opt.name || !*opt.name || available<3) return 0;
    size_t used=1, offset=0, length=strlen(opt.name);
    mbstate_t state={0};
    fputc(' ',stderr);
    while (offset<length) {
        wchar_t wc;
        size_t n=mbrtowc(&wc,opt.name+offset,length-offset,&state);
        if (n==(size_t)-1 || n==(size_t)-2 || !n) {
            memset(&state,0,sizeof state); n=1; wc='.';
        }
        int cells=wcwidth(wc);
        if (cells<0) cells=1;
        if (used+(size_t)cells+1>available) break;
        if (wc=='.' && (unsigned char)opt.name[offset]>=128) fputc('.',stderr);
        else fwrite(opt.name+offset,1,n,stderr);
        used+=(size_t)cells; offset+=n;
    }
    fputc(' ',stderr);
    return used+1;
}

static void draw_static_ui(void) {
	const char *WE = "─";
	const char *NS = "│";
	const char *SE = "┌";
	const char *NE = "└";
	const char *SW = "┐";
	const char *NW = "┘";

	restore_cursor();

	fprintf(stderr, "%s", GREEN);

	// open the box (progress bar section)
	fprintf(stderr, "%s", SE);
    size_t name_columns=print_ui_name(g_columns-2);
	repeat_text(WE, g_columns - 2-name_columns);
	fprintf(stderr, "%s", SW);
	fprintf(stderr, "\n");

	// put empty room in box
	for (int i = 0; i < 2; i++) {
		fprintf(stderr, "%s", NS);
		repeat_text(" ", g_columns - 2);
		fprintf(stderr, "%s", NS);
		fprintf(stderr, "\n");
	}

	// close the box
	fprintf(stderr, "%s", NE);
	repeat_text(WE, g_columns - 2);
	fprintf(stderr, "%s", NW);
	fprintf(stderr, "\n");

	fprintf(stderr, "%s", RST);

	// print the vertical bars (metadata section)
	fprintf(stderr, "%s", DIM);
	int spaces = (int)fmin(INT_MAX, (g_columns - 3) / 4);
	repeat_text(" ", spaces);
	fprintf(stderr, "%s", NS);
	repeat_text(" ", spaces);
	fprintf(stderr, "%s", NS);
	repeat_text(" ", spaces);
	fprintf(stderr, "%s", NS);
	repeat_text(" ", spaces);
	fprintf(stderr, "\n");

	fprintf(stderr, "%-*s", spaces, "");
	fprintf(stderr, "%s", NS);
	fprintf(stderr, "%-*s", spaces, " total");
	fprintf(stderr, "%s", NS);
	fprintf(stderr, "%-*s", spaces, " speed");
	fprintf(stderr, "%s", NS);
	fprintf(stderr, "%-*s", spaces, " eta");
	fprintf(stderr, "\n");
	fprintf(stderr, "%s", RST);
}

static void clear_old_screen(void) {
	restore_cursor();

	for (int i = 0; i < 6; i++) {
		fprintf(stderr, "\033[2K\033[1B");
	}
	fflush(stderr);
}

