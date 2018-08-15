/*
 * Copyright (c) 2017-2018 Red Hat.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "series.h"
#include "pmapi.h"
#include "sds.h"
#include <uv.h>

typedef enum series_flags {
    PMSERIES_COLOUR	= (1<<0),	/* report in colour if possible */
    PMSERIES_FAST	= (1<<1),	/* load only the metric metadata */
    PMSERIES_FULLINDOM	= (1<<2),	/* report with pminfo(1) -I info */
    PMSERIES_FULLPMID	= (1<<3),	/* report with pminfo(1) -M info */
    PMSERIES_SERIESID	= (1<<4),	/* report with pminfo(1) -s info */
    PMSERIES_SOURCEID	= (1<<5),	/* report with pminfo(1) -S info */
    PMSERIES_NEED_EOL	= (1<<6),	/* need to eol-terminate output */
    PMSERIES_NEED_COMMA	= (1<<7),	/* need comma line separation */
    PMSERIES_INSTLABELS	= (1<<8),	/* labels by instance identifier */
    PMSERIES_ONLY_NAMES	= (1<<9),	/* report on label names only */
    PMSERIES_NEED_DESCS	= (1<<10),	/* output requires descs lookup */
    PMSERIES_NEED_INSTS	= (1<<11),	/* output requires insts lookup */

    PMSERIES_OPT_ALL	= (1<<16),	/* -a, --all option */
    PMSERIES_OPT_SOURCE = (1<<17),	/* -c, --context option */
    PMSERIES_OPT_DESC	= (1<<18),	/* -d, --desc option */
    PMSERIES_OPT_INSTS	= (1<<19),	/* -i, --instances option */
    PMSERIES_OPT_LABELS	= (1<<20),	/* -l, --labels option */
    PMSERIES_OPT_LOAD	= (1<<21),	/* -L, --load option */
    PMSERIES_OPT_METRIC	= (1<<22),	/* -m, --metric option */
    PMSERIES_OPT_QUERY	= (1<<23),	/* -q, --query option (default) */
} series_flags;

static series_flags	flags;
static sds		query;
static int		active;
static int		exitsts;

#define PMSERIES_META_OPTS	(PMSERIES_OPT_DESC | PMSERIES_OPT_INSTS | \
				 PMSERIES_OPT_LABELS | PMSERIES_OPT_METRIC)

typedef struct series_label {
    sds			name;
    sds			value;
} series_label;

typedef struct series_inst {
    sds			instid;		/* internal instance identifier */
    sds			name;		/* external instance identifier */
    sds			series;		/* instance series back-pointer */
    unsigned int	nlabels;	/* number of instance labels */
    series_label	*labels;	/* series (instance) labels */
} series_inst;

typedef struct series_data {
    int			status;		/* command exit status */
    series_flags	flags;		/* flags affecting reporting */

    pmSID		series;		/* current time series */
    pmSID		source;		/* current time series source */
    sds			type;		/* current time series (value) type */

    unsigned int	nlabels;	/* number of metric labels */
    series_label	*labels;	/* series (metric) labels */

    unsigned int        ninsts;		/* instances for the current series */
    series_inst		*insts;		/* instances for the current series */
    pmSID		*iseries;	/* series identifiers for instances */
} series_data;

#define series_data_endtopic(dp) 	((dp)->flags &= ~PMSERIES_NEED_COMMA)
#define series_data_endline(dp) 	((dp)->flags &= ~PMSERIES_NEED_EOL)

static series_data *
series_data_init(series_flags flags)
{
    series_data		*dp = calloc(1, sizeof(series_data));

    if (dp != NULL) {
	dp->series = sdsnewlen("", 40);
	dp->source = sdsnewlen("", 40);
	dp->flags = flags;
    }
    return dp;
}

static void
series_add_inst(series_data *dp, pmSID series, sds instid, sds instname)
{
    series_inst		*ip;
    pmSID		*isp;
    size_t		bytes;

    bytes = sizeof(sds) * (dp->ninsts + 1);
    if ((isp = realloc(dp->iseries, bytes)) != NULL) {
	bytes = sizeof(series_inst) * (dp->ninsts + 1);
	if ((ip = realloc(dp->insts, bytes)) != NULL) {
	    dp->insts = ip;
	    ip += dp->ninsts;
	    memset(ip, 0, sizeof(series_inst));
	    ip->instid = sdsdup(instid);
	    ip->name = sdsdup(instname);
	    ip->series = sdsdup(series);
	    dp->iseries = isp;
	    isp += dp->ninsts;
	    *isp = ip->series;
	    dp->ninsts++;
	} else {
	    fprintf(stderr, "%s: failed to allocate %" FMT_INT64 " bytes\n",
		    pmGetProgname(), (__int64_t)bytes);
	    free(isp);
	}
    } else {
	fprintf(stderr, "%s: failed to allocate %" FMT_INT64 " bytes\n",
		pmGetProgname(), (__int64_t)bytes);
    }
}

static void
series_del_insts(series_data *dp)
{
    series_inst		*ip;
    int			i;

    for (i = 0; i < dp->ninsts; i++) {
	ip = &dp->insts[i];
	sdsfree(ip->series);
	sdsfree(ip->instid);
	sdsfree(ip->name);
    }
    if (dp->insts)
	free(dp->insts);
    dp->ninsts = 0;
}

static void
series_data_free(series_data *dp)
{
    sdsfree(dp->series);
    sdsfree(dp->source);
    if (dp->type)
	sdsfree(dp->type);
    series_del_insts(dp);
}

static int
series_split(sds string, pmSID **series)
{
    size_t		length;
    int			nseries = 0;

    if (!string || !sdslen(string))
	return 0;
    length = strlen(string);
    if ((*series = sdssplitlen(string, length, ",", 1, &nseries)) == NULL)
	return -ENOMEM;
    return nseries;
}

static void
series_free(int nseries, pmSID *series)
{
    if (nseries) {
	while (--nseries)
	    sdsfree(series[nseries]);
	free(series);
    }
}

static series_inst *
series_get_inst(series_data *dp, sds series)
{
    series_inst		*ip;
    int			i;

    for (i = 0; i < dp->ninsts; i++) {
	ip = &dp->insts[i];
	if (sdscmp(series, ip->series) == 0)
	    return ip;
    }
    return NULL;
}

static int
series_next(series_data *dp, sds sid)
{
    if (strncmp(dp->series, sid, sdslen(sid)) != 0) {
	dp->flags &= ~PMSERIES_NEED_COMMA;
	if (dp->flags & PMSERIES_NEED_EOL) {
	    dp->flags &= ~PMSERIES_NEED_EOL;
	    putc('\n', stdout);
	}
	dp->series = sdscpylen(dp->series, sid, sdslen(sid));
	if (dp->source)
	    sdsclear(dp->source);
	if (dp->type)
	    sdsclear(dp->type);
	series_del_insts(dp);
	return 1;
    }
    return 0;
}

static void
on_series_info(pmloglevel level, sds message, void *arg)
{
    series_data		*dp = (series_data *)arg;
    int			colour = (dp->flags & PMSERIES_COLOUR);
    FILE		*fp = (level == PMLOG_INFO) ? stdout : stderr;

    return pmLogLevelPrint(fp, level, message, colour);
}

static const char *
series_type_phrase(const char *type_word)
{
    if (strcasecmp(type_word, "32") == 0)
	return "32-bit int";
    if (strcasecmp(type_word, "64") == 0)
	return "64-bit int";
    if (strcasecmp(type_word, "U32") == 0)
	return "32-bit unsigned int";
    if (strcasecmp(type_word, "U64") == 0)
	return "64-bit unsigned int";
    if (strcasecmp(type_word, "FLOAT") == 0)
	return "float";
    if (strcasecmp(type_word, "DOUBLE") == 0)
	return "double";
    if (strcasecmp(type_word, "STRING") == 0)
	return "string";
    if (strcasecmp(type_word, "AGGREGATE") == 0)
	return "aggregate";
    if (strcasecmp(type_word, "AGGREGATE_STATIC") == 0)
	return "aggregate static";
    if (strcasecmp(type_word, "EVENT") == 0)
	return "event record array";
    if (strcasecmp(type_word, "HIGHRES_EVENT") == 0)
	return "highres event record array";
    if (strcasecmp(type_word, "NO_SUPPORT") == 0)
	return "Not Supported";
    return "???";
}

static void
series_load(pmSeriesSettings *settings, sds query, series_flags flags)
{
    series_data		*data;
    pmflags		meta = flags & PMSERIES_FAST? PMFLAG_METADATA : 0;

    data = series_data_init(flags);
    pmSeriesLoad(settings, query, meta, (void *)data);
}

static int
on_series_match(pmSID sid, void *arg)
{
    series_data		*dp = (series_data *)arg;

    if (series_next(dp, sid))
	printf("%s\n", sid);
    return 0;
}

static int
on_series_value(pmSID sid, pmSeriesValue *value, void *arg)
{
    series_inst		*ip;
    series_data		*dp = (series_data *)arg;
    sds			timestamp, series, data;
    int			need_free = 1;

    timestamp = value->timestamp;
    series = value->series;
    data = value->data;

    if (series_next(dp, sid))
	printf("\n%s\n", sid);

    if (dp->type == NULL)
	dp->type = sdsempty();
    if (strncmp(dp->type, "AGGREGATE", sizeof("AGGREGATE")-1) == 0)
	data = sdscatrepr(sdsempty(), data, sdslen(data));
    else if (strncmp(dp->type, "STRING", sizeof("STRING")-1) == 0)
	data = sdscatfmt(sdsempty(), "\"%S\"", data);
    else
	need_free = 0;

    if (sdscmp(series, sid) == 0)
	printf("    [%s] %s\n", timestamp, data);
    else if ((ip = series_get_inst(dp, series)) == NULL)
	printf("    [%s] %s %s\n", timestamp, data, series);
    else
	printf("    [%s] %s \"%s\"\n", timestamp, data, ip->name);

    if (need_free)
	sdsfree(data);
    return 0;
}

static void
series_query(pmSeriesSettings *settings, sds query, series_flags flags)
{
    series_data		*data;
    pmflags		meta = flags & PMSERIES_FAST? PMFLAG_METADATA : 0;

    data = series_data_init(flags);
    pmSeriesQuery(settings, query, meta, (void *)&data);
}

static int
on_series_desc(pmSID series, pmSeriesDesc *desc, void *arg)
{
    series_data		*dp = (series_data *)arg;
    static const char	*unknown = "???";
    unsigned int	domain, cluster, item, serial;
    pmInDom		indom_value = PM_IN_NULL;
    pmID		pmid_value = PM_ID_NULL;
    sds			indom, pmid, semantics, source, type, units;

    indom = desc->indom;
    pmid = desc->pmid;
    semantics = desc->semantics;
    source = desc->source;
    type = desc->type;
    units = desc->units;

    if (series_next(dp, series)) {
	dp->type = sdsnewlen(type, sdslen(type));
	printf("\n%s", series);
    } else {
	printf("   ");
    }
    dp->source = dp->source ?
		sdscpylen(dp->source, source, sdslen(source)) :
		sdsnewlen(source, sdslen(source));

    if (sscanf(pmid, "%u.%u.%u", &domain, &cluster, &item) == 3)
	pmid_value = pmID_build(domain, cluster, item);
    else if (strcmp(pmid, "none") == 0)
	pmid = "PM_ID_NULL";
    if (sscanf(indom, "%u.%u", &domain, &serial) == 2)
	indom_value = pmInDom_build(domain, serial);
    else if (strcmp(indom, "none") == 0)
	indom = "PM_INDOM_NULL";

    if (dp->flags & (PMSERIES_FULLPMID|PMSERIES_OPT_DESC))
	printf(" PMID: %s", pmid);
    if (dp->flags & PMSERIES_FULLPMID)
	printf(" = %u = 0x%x", pmid_value, pmid_value);
    if (dp->flags & (PMSERIES_FULLPMID|PMSERIES_OPT_DESC))
	printf("\n");
    if (dp->flags & PMSERIES_OPT_DESC) {
	printf("    Data Type: %s", series_type_phrase(type));
	if (strcmp(type, unknown) == 0)
	    printf(" (%s)", type);
    }
    if (dp->flags & (PMSERIES_FULLINDOM|PMSERIES_OPT_DESC))
	printf("  InDom: %s", indom);
    if (dp->flags & PMSERIES_FULLINDOM)
	printf(" = %u =", indom_value);
    if (dp->flags & (PMSERIES_FULLINDOM|PMSERIES_OPT_DESC))
	printf(" 0x%x\n", indom_value);
    if (dp->flags & PMSERIES_OPT_DESC) {
	printf("    Semantics: %s", semantics);
	if (strcmp(semantics, unknown) == 0)
	    printf(" (%s)", semantics);
	printf("  Units: %s\n", *units == '\0' ? "none" : units);
    }
    if (dp->flags & PMSERIES_SOURCEID)
	printf("    Source: %s\n", source);
    dp->flags &= ~PMSERIES_NEED_EOL;

    return 0;
}

static int
on_series_instance(pmSID series, sds name, void *arg)
{
    series_data		*dp = (series_data *)arg;

    if (!(dp->flags & PMSERIES_OPT_INSTS))
	return 0;
    if (series == NULL)	{	/* report all instance names */
	printf("%s\n", name);
	return 0;
    }
    if (series_next(dp, series))
	printf("\n%s", series);
    if (dp->flags & PMSERIES_NEED_COMMA)
	printf(", %s", name);
    else
	printf("    Instances: %s", name);
    dp->flags |= (PMSERIES_NEED_EOL | PMSERIES_NEED_COMMA);
    return 0;
}

static int
on_series_inst(pmSID sid, pmSeriesInst *inst, void *arg)
{
    series_data		*dp = (series_data *)arg;
    sds			instid, instname, series;

    instid = inst->instid;
    instname = inst->name;
    series = inst->series;

    if (series_next(dp, sid) && (dp->flags & PMSERIES_OPT_INSTS))
	printf("\n%s\n", sid);
    if (series_get_inst(dp, series) == NULL)
	series_add_inst(dp, series, instid, instname);
    return 0;
}

static int
series_instance_compare(const void *a, const void *b)
{
    series_inst		*ap = (series_inst *)a;
    series_inst		*bp = (series_inst *)b;

    if (sdscmp(ap->instid, bp->instid) != 0)
	return (int)(atoll(ap->instid) - atoll(bp->instid));
    return strcmp(ap->name, bp->name);
}

static void
series_instance_names(void *arg)
{
    series_data		*dp = (series_data *)arg;
    series_inst		*ip;
    pmSID		*isp = dp->iseries;
    int			i;

    qsort(dp->insts, dp->ninsts, sizeof(series_inst), series_instance_compare);
    for (i = 0; i < dp->ninsts; i++) {
	ip = &dp->insts[i];
	if (dp->flags & PMSERIES_OPT_INSTS)
	    printf("    inst [%s or \"%s\"] series %s\n",
		    ip->instid, ip->name, ip->series);
	isp[i] = ip->series;
    }
}

static int
series_labels_compare(const void *a, const void *b)
{
    series_label	*ap = (series_label *)a;
    series_label	*bp = (series_label *)b;

    return sdscmp(ap->name, bp->name);
}

static sds
series_labels_sort(sds s, unsigned int nlabels, series_label *labels)
{
    series_label	*lp;
    unsigned int	i;

    qsort(labels, nlabels, sizeof(series_label), series_labels_compare);

    s = sdscpylen(s, "{", 1);
    for (i = 0; i < nlabels; i++) {
	lp = &labels[i];
	s = sdscatfmt(s, "\"%S\":%S", lp->name, lp->value);
    }
    return sdscatfmt(s, "}");
}

static void
series_metric_labels(void *arg)
{
    series_data		*dp = (series_data *)arg;
    sds			labels;

    if (!(dp->flags & PMSERIES_ONLY_NAMES)) {
	labels = series_labels_sort(sdsempty(), dp->nlabels, dp->labels);
	if (sdslen(labels) > 2)
	    printf("    labels %s\n", labels);
	sdsfree(labels);
    }
}

static void
series_instance_labels(void *arg)
{
    series_data		*dp = (series_data *)arg;
    series_inst		*ip = NULL;
    sds			labels;
    unsigned int	i;

    if (!(dp->flags & PMSERIES_ONLY_NAMES)) {
	labels = sdsempty();
	for (i = 0; i < dp->ninsts; i++) {
	    ip = &dp->insts[i];
	    labels = series_labels_sort(labels, ip->nlabels, ip->labels);
	    printf("    inst [%s or \"%s\"] labels %s\n",
		    ip->instid, ip->name, labels);
	}
	sdsfree(labels);
    }
}

static void
series_add_labels(sds name, sds value,
		unsigned int *nlabelsp, series_label **labelsp)
{
    unsigned int	nlabels = *nlabelsp;
    series_label	*lp = *labelsp;
    size_t		bytes;

    bytes = sizeof(series_label) * (nlabels + 1);
    if ((lp = realloc(lp, bytes)) != NULL) {
	lp += nlabels;
	lp->name = sdsdup(name);
	lp->value = sdsdup(value);

	*labelsp = lp;
	*nlabelsp = nlabels + 1;
    } else {
	fprintf(stderr, "%s: failed to allocate %" FMT_INT64 " bytes\n",
		pmGetProgname(), (__int64_t)bytes);
    }
}

static int
on_series_label(pmSID series, sds label, void *arg)
{
    series_data		*dp = (series_data *)arg;

    if (series == NULL) {	/* report all label names */
	printf("%s\n", label);
	return 0;
    }
    if (!(dp->flags & PMSERIES_ONLY_NAMES))
	return 0;
    if (series_next(dp, series))
	printf("\n%s", series);
    if (dp->flags & PMSERIES_NEED_COMMA)
	printf(", %s", label);
    else
	printf("    Labels: %s", label);
    dp->flags |= (PMSERIES_NEED_COMMA | PMSERIES_NEED_EOL);
    return 0;
}

static int
on_series_labelmap(pmSID series, pmSeriesLabel *label, void *arg)
{
    series_inst		*ip = NULL;
    series_data		*dp = (series_data *)arg;
    sds			name, value;

    if (dp->flags & PMSERIES_INSTLABELS) {
	if ((ip = series_get_inst(dp, series)) == NULL)
	    return 0;
    } else if (series_next(dp, series) && !(dp->flags & PMSERIES_SERIESID)) {
	printf("\n%s\n", series);
    }

    name = label->name;
    value = label->value;
    if (dp->flags & PMSERIES_INSTLABELS)
	series_add_labels(name, value, &ip->nlabels, &ip->labels);
    else
	series_add_labels(name, value, &dp->nlabels, &dp->labels);
    return 0;
}

static int
on_series_metric(pmSID series, sds name, void *arg)
{
    series_data		*dp = (series_data *)arg;

    if (series == NULL) {	/* report all metric names */
	printf("%s\n", name);
	return 0;
    }
    if (series_next(dp, series))
	printf("\n%s", series);
    if (dp->flags & PMSERIES_NEED_COMMA)
	printf(", %s", name);
    else
	printf("    Metric: %s", name);
    dp->flags |= (PMSERIES_NEED_COMMA | PMSERIES_NEED_EOL);
    return 0;
}

static int
on_series_context(pmSID source, sds name, void *arg)
{
    series_data		*dp = (series_data *)arg;

    if (source == NULL)	{	/* report all metric sources */
	printf("%s\n", name);
	return 0;
    }
    dp->source = dp->source ?
		sdscpylen(dp->source, source, sdslen(source)) :
		sdsnewlen(source, sdslen(source));
    if (!dp->source || sdscmp(dp->source, source) != 0)
	printf("\n%s", source);
    if (dp->flags & PMSERIES_NEED_COMMA)
	printf(", %s", name);
    else
	printf("    Context: %s", name);
    dp->flags |= (PMSERIES_NEED_COMMA | PMSERIES_NEED_EOL);
    return 0;
}

static void
series_source(pmSeriesSettings *settings, sds query, series_flags flags)
{
    int			nsources, sts;
    char		msg[PM_MAXERRMSGLEN];
    pmSID		*sources = NULL;
    series_data		*data;

    if ((nsources = sts = series_split(query, &sources)) < 0) {
	fprintf(stderr, "%s: cannot find source identifiers in '%s': %s\n",
		pmGetProgname(), query, pmErrStr_r(sts, msg, sizeof(msg)));
    } else {
	data = series_data_init(flags);
	pmSeriesSources(settings, nsources, sources, (void *)&data);
	series_free(nsources, sources);
    }
}

/*
 * Finishing up interacting with the library via callbacks
 */

static void
on_series_done(int sts, void *arg)
{
    series_data		*dp = (series_data *)arg;
    char		msg[PM_MAXERRMSGLEN];

#if 0
    extern void pmSeriesDone(void);
    if (--active <= 0)
	pmSeriesDone();
#endif

    if (dp->flags & PMSERIES_NEED_EOL) {
	dp->flags &= ~PMSERIES_NEED_EOL;
	putc('\n', stdout);
    }
    if (sts < 0) {
	fprintf(stderr, "%s: %s\n", pmGetProgname(),
			pmErrStr_r(sts, msg, sizeof(msg)));
	exitsts = 1;
    }

}

static void
series_data_report(pmSeriesSettings *settings,
		int nseries, pmSID series, series_flags flags)
{
    series_data		*data;

    /* TODO: each of these may need to be under a separate uv_timer_t? */

    data = series_data_init(flags);
    if (nseries && series_next(data, series))
	printf("\n%s\n", series);

    if (flags & (PMSERIES_OPT_DESC|PMSERIES_NEED_DESCS)) {
	pmSeriesDescs(settings, nseries, &series, (void *)data);
	series_data_endtopic(data);
    }
    if (flags & PMSERIES_OPT_SOURCE) {
	pmSeriesSources(settings, 1, &data->source, (void *)data);
	series_data_endtopic(data);
    }
    if (flags & PMSERIES_OPT_METRIC) {
	pmSeriesMetrics(settings, nseries, &series, (void *)data);
	series_data_endtopic(data);
    }
    if (flags & PMSERIES_OPT_LABELS) {
	pmSeriesLabels(settings, nseries, &series, (void *)data);
	series_metric_labels(data);
	series_data_endtopic(data);
    }
    if (flags & (PMSERIES_OPT_INSTS|PMSERIES_NEED_INSTS)) {
	pmSeriesInstances(settings, nseries, &series, (void *)data);
	series_instance_names(data);
	series_data_endtopic(data);
    }
    /* report per-instance label information */
    if ((flags & PMSERIES_OPT_LABELS) && nseries != 0) {
	data->flags |= PMSERIES_INSTLABELS;
	pmSeriesLabels(settings, data->ninsts, data->iseries, (void *)data);
	series_instance_labels(data);
	series_data_endtopic(data);
    }
    series_data_endline(data);

    series_data_free(data);
}

static void
series_report(pmSeriesSettings *settings, sds query, series_flags flags)
{
    int			nseries, sts, i;
    char		msg[PM_MAXERRMSGLEN];
    pmSID		*series = NULL;

    if ((nseries = sts = series_split(query, &series)) < 0) {
	fprintf(stderr, "%s: no series identifiers in string '%s': %s\n",
		pmGetProgname(), query, pmErrStr_r(sts, msg, sizeof(msg)));
    } else {
	active = nseries ? nseries : 1;
	for (i = 0; i < nseries; i++)
	    series_data_report(settings, 1, series[i], flags);
	if (nseries == 0)	/* report all names, instances, labels, ... */
	    series_data_report(settings, 0, NULL, flags);
	series_free(nseries, series);
    }
}

static pmSeriesSettings settings = {
    .on_match		= on_series_match,
    .on_desc		= on_series_desc,
    .on_inst		= on_series_inst,
    .on_labelmap	= on_series_labelmap,
    .on_instance	= on_series_instance,
    .on_context		= on_series_context,
    .on_metric		= on_series_metric,
    .on_value		= on_series_value,
    .on_label		= on_series_label,
    .on_info		= on_series_info,
    .on_done		= on_series_done,
};

static void
pmseries_request(uv_timer_t *arg)
{
    (void)arg;
    if (flags & PMSERIES_OPT_LOAD)
	series_load(&settings, query, flags);
    else if (flags & PMSERIES_OPT_QUERY)
	series_query(&settings, query, flags);
    else if ((flags & PMSERIES_OPT_SOURCE) && !(flags & PMSERIES_META_OPTS))
	series_source(&settings, query, flags);
    else
	series_report(&settings, query, flags);
}

static int
pmseries_execute(void)
{
    uv_timer_t		request;
    uv_loop_t		*loop = (uv_loop_t *)settings.events;

    uv_timer_init(loop, &request);
    uv_timer_start(&request, pmseries_request, 0, 0);
    uv_run(loop, UV_RUN_DEFAULT);
    fprintf(stderr, "Clean exit\n");
    return exitsts;
}

static int
pmseries_overrides(int opt, pmOptions *opts)
{
    switch (opt) {
    case 'a':
    case 'h':
    case 'L':
    case 's':
    case 'S':
    case 'n':
    case 'p':
	return 1;
    }
    return 0;
}

static pmLongOptions longopts[] = {
    PMAPI_OPTIONS_HEADER("General Options"),
    { "all", 0, 'a', 0, "report all metadata (-dilmsS) for time series" },
    { "contexts", 0, 'c', 0, "report context names for a time series source" },
    { "desc", 0, 'd', 0, "metric descriptor for time series" },
    { "instances", 0, 'i', 0, "instance identifiers for time series" },
    { "labels", 0, 'l', 0, "list all labels for time series" },
    { "load", 0, 'L', 0, "load time series values and metadata" },
    { "metrics", 0, 'm', 0, "metric names for time series" },
    { "query", 0, 'q', 0, "perform a time series query (default)" },
    { "port", 1, 'p', "N", "Connect to Redis instance on this TCP/IP port" },
    { "host", 1, 'h', "HOST", "Connect to Redis instance using host specification" },
    PMAPI_OPTIONS_HEADER("Reporting Options"),
    PMOPT_DEBUG,
    { "fast", 0, 'F', 0, "query or load series metadata, not values" },
    { "fullpmid", 0, 'M', 0, "print PMID in verbose format" },
    { "fullindom", 0, 'I', 0, "print InDom in verbose format" },
    { "names", 0, 'n', 0, "print label names only, not values" },
    { "source", 0, 'S', 0, "print the source for each time series" },
    { "series", 0, 's', 0, "print the series for each instance" },
    PMOPT_VERSION,
    PMOPT_HELP,
    PMAPI_OPTIONS_END
};

static pmOptions opts = {
    .flags = PM_OPTFLAG_BOUNDARIES,
    .short_options = "acdD:Fh:iIlLmMnqp:sSV?",
    .long_options = longopts,
    .short_usage = "[options] [query ... | series ... | source ...]",
    .override = pmseries_overrides,
};

int
main(int argc, char *argv[])
{
    int			c, sts;
    const char		*split = ",";
    const char		*space = " ";
    char		*hostname = "localhost";
    unsigned int	port = 6379;

    while ((c = pmGetOptions(argc, argv, &opts)) != EOF) {
	switch (c) {

	case 'a':	/* command line contains series identifiers */
	    flags |= (PMSERIES_OPT_ALL | PMSERIES_SOURCEID | PMSERIES_SERIESID);
	    break;

	case 'c':	/* command line contains source identifiers */
	    flags |= PMSERIES_OPT_SOURCE;
	    break;

	case 'd':	/* command line contains series identifiers */
	    flags |= PMSERIES_OPT_DESC;
	    break;

	case 'F':	/* perform metadata-only --load, or --query */
	    flags |= PMSERIES_FAST;
	    break;

        case 'h':
	    hostname = opts.optarg;
	    break;

	case 'i':	/* command line contains series identifiers */
	    flags |= PMSERIES_OPT_INSTS;
	    break;

	case 'I':	/* full form InDom reporting, ala pminfo -I */
	    flags |= PMSERIES_FULLINDOM;
	    break;

	case 'l':	/* command line contains series identifiers */
	    flags |= PMSERIES_OPT_LABELS;
	    break;

	case 'L':	/* command line contains source load string */
	    flags |= PMSERIES_OPT_LOAD;
	    split = space;
	    break;

	case 'm':	/* command line contains series identifiers */
	    flags |= PMSERIES_OPT_METRIC;
	    break;

	case 'M':	/* full form PMID reporting, ala pminfo -M */
	    flags |= PMSERIES_FULLPMID;
	    break;

	case 'n':	/* report label names only, not values */
	    flags |= PMSERIES_ONLY_NAMES;
	    break;

        case 'p':	/* Redis port to connect to */
	    port = (unsigned int)strtol(opts.optarg, NULL, 10);
	    break;

	case 'q':	/* command line contains query string */
	    flags |= PMSERIES_OPT_QUERY;
	    split = space;
	    break;

	case 'S':	/* report source identifiers, ala pminfo -S */
	    flags |= PMSERIES_SOURCEID;
	    break;

	case 's':	/* report series identifiers, ala pminfo -s */
	    flags |= PMSERIES_SERIESID;
	    break;

	default:
	    opts.errors++;
	    break;
	}
    }

    if (flags & PMSERIES_OPT_ALL)
	flags |= PMSERIES_META_OPTS;

    if ((flags & PMSERIES_OPT_LOAD) &&
	(flags & (PMSERIES_META_OPTS | PMSERIES_OPT_SOURCE))) {
	pmprintf("%s: error - cannot use load and reporting options together\n",
			pmGetProgname());
	opts.errors++;
    }
    if ((flags & PMSERIES_OPT_LOAD) && (flags & PMSERIES_OPT_QUERY)) {
	pmprintf("%s: error - cannot use load and querying options together\n",
			pmGetProgname());
	opts.errors++;
    }
    if ((flags & PMSERIES_OPT_QUERY) &&
	(flags & (PMSERIES_META_OPTS | PMSERIES_OPT_SOURCE))) {
	pmprintf("%s: error - cannot use query and metadata options together\n",
			pmGetProgname());
	opts.errors++;
    }

    if (flags & (PMSERIES_FULLPMID | PMSERIES_FULLINDOM))
	flags |= PMSERIES_NEED_DESCS;
    if (flags & (PMSERIES_SOURCEID | PMSERIES_SERIESID))
	flags |= PMSERIES_NEED_DESCS;

    if (flags & PMSERIES_OPT_LABELS)
	flags |= PMSERIES_NEED_INSTS;

    if (!(flags & (PMSERIES_META_OPTS|PMSERIES_OPT_LOAD|PMSERIES_OPT_SOURCE)))
	if (!(flags & (PMSERIES_NEED_DESCS|PMSERIES_NEED_INSTS)))
	    flags |= PMSERIES_OPT_QUERY;	/* default is to query */

    if (opts.optind == argc && (flags & PMSERIES_OPT_QUERY)) {
	pmprintf("%s: error - no --query string provided\n", pmGetProgname());
	opts.errors++;
    }

    if (opts.errors || (opts.flags & PM_OPTFLAG_EXIT)) {
	sts = !(opts.flags & PM_OPTFLAG_EXIT);
	pmUsageMessage(&opts);
	exit(sts);
    }

    if (pmLogLevelIsTTY())
	flags |= PMSERIES_COLOUR;

    if (opts.optind == argc)
	query = sdsempty();
    else
	query = sdsjoin(&argv[opts.optind], argc - opts.optind, (char *)split);

    settings.hostspec = sdscatprintf(sdsempty(), "%s:%u", hostname, port);
    settings.events = (void *)uv_default_loop();

    pmseries_execute();

    sdsfree(query);
    return exitsts;
}
