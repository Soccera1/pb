/* Samples are independent from display intervals and bounded by the active window. */
struct rate_sample { double time; unsigned long long amount; };
struct rate_window {
    struct rate_sample *samples;
    size_t capacity, begin, end;
    double origin, window;
};
static struct rate_window rates;

static bool rate_window_reset(struct rate_window *state, double origin, double sample_time, unsigned long long amount, double window) {
    state->begin=state->end=0; state->origin=origin; state->window=window;
    if (!state->capacity) {
        state->samples=malloc(32*sizeof *state->samples);
        if (!state->samples) return false;
        state->capacity=32;
    }
    state->samples[state->end++]=(struct rate_sample){sample_time,amount};
    return true;
}

static bool rate_window_sample(struct rate_window *state, double time, unsigned long long amount, double origin, double window) {
    if (!state->capacity || origin!=state->origin) {
        if (!rate_window_reset(state,origin,origin,0,window)) return false;
    } else if (window!=state->window) {
        /* A newly selected window starts with the history available from that point. */
        if (!rate_window_reset(state,origin,time,amount,window)) return false;
    }
    if (state->end && amount<state->samples[state->end-1].amount) {
        if (!rate_window_reset(state,origin,time,amount,window)) return false;
    }
    if (state->end-state->begin>=2 && state->samples[state->end-1].amount==amount &&
        state->samples[state->end-2].amount==amount) {
        state->samples[state->end-1].time=time;
    } else {
        if (state->end==state->capacity && state->begin) {
            memmove(state->samples,state->samples+state->begin,(state->end-state->begin)*sizeof *state->samples);
            state->end-=state->begin; state->begin=0;
        }
        if (state->end==state->capacity) {
            if (state->capacity>SIZE_MAX/2/sizeof *state->samples) return false;
            size_t capacity=state->capacity*2;
            struct rate_sample *replacement=realloc(state->samples,capacity*sizeof *state->samples);
            if (!replacement) return false;
            state->samples=replacement; state->capacity=capacity;
        }
        state->samples[state->end++]=(struct rate_sample){time,amount};
    }
    double cutoff=time-window;
    while (state->end-state->begin>2 && state->samples[state->begin+1].time<=cutoff) state->begin++;
    return true;
}

static double rate_window_average(const struct rate_window *state, double time, unsigned long long amount) {
    if (!state->capacity || state->end==state->begin) return 0;
    struct rate_sample base=state->samples[state->begin];
    double cutoff=fmax(time-state->window,base.time);
    long double delta=amount>=base.amount ? (long double)(amount-base.amount) : 0;
    if (state->end-state->begin>1 && cutoff>base.time) {
        struct rate_sample next=state->samples[state->begin+1];
        if (next.time>base.time && next.amount>=base.amount)
            delta-=(long double)(next.amount-base.amount)*(cutoff-base.time)/(next.time-base.time);
    }
    double elapsed=time-cutoff;
    return elapsed>0 ? (double)fmaxl(0,delta)/elapsed : 0;
}

static bool rates_reset(double origin, double time, unsigned long long amount, double window) {
    return rate_window_reset(&rates,origin,time,amount,window);
}
static bool rates_sample(double time, unsigned long long amount, double origin, double window) {
    return rate_window_sample(&rates,time,amount,origin,window);
}
static double rates_average(double time, unsigned long long amount) {
    return rate_window_average(&rates,time,amount);
}
