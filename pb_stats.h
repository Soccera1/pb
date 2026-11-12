/* Stable byte-rate statistics shared by transfer and query displays. */
struct rate_statistics { unsigned long count; double mean, m2, minimum, maximum; };

static void statistics_add(struct rate_statistics *stats, double rate) {
    if (!stats->count || rate<stats->minimum) stats->minimum=rate;
    if (!stats->count || rate>stats->maximum) stats->maximum=rate;
    stats->count++;
    double difference=rate-stats->mean;
    stats->mean+=difference/stats->count;
    stats->m2+=difference*(rate-stats->mean);
}

static void statistics_show(const struct rate_statistics *stats) {
    if (!opt.stats) return;
    double scale=opt.bits ? 8 : 1;
    double deviation=stats->count ? sqrt(fmax(0,stats->m2/stats->count)) : 0;
    fprintf(stderr,"pb: rate min/mean/max/stddev = %.3f/%.3f/%.3f/%.3f %s/s\n",
            stats->minimum*scale,stats->mean*scale,stats->maximum*scale,deviation*scale,opt.bits ? "b" : "B");
}
