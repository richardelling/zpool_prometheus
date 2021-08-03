/*
 * Gather top-level ZFS pool, resilver/scan statistics, and latency
 * histograms then print using prometheus line protocol
 * usage: [pool_name]
 *
 * To integrate into a real-world deployment prometheus expects to see
 * the results hosted by an HTTP server. In keeping with the UNIX
 * philosophy and knowing that writing HTTP servers in C is a real PITA,
 * the HTTP server is left as an exercise for the deployment team: use
 * Nginx, Apache, or whatever framework works for your team.
 *
 * Alternatively, it is possible to have a scheduled job (eg cron) that places
 * the output in the directory configured for node_exporter's textfile
 * collector.
 *
 * NOTE: libzfs is an unstable interface. YMMV.
 *
 * Copyright 2018-2019 Richard Elling
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/fs/zfs.h>
#include <time.h>
#include <libzfs.h>
#include <string.h>
#include <libnvpair.h>

#define	COMMAND_NAME	"zpool_prometheus"
#define	POOL_MEASUREMENT	"zpool_stats"
#define	SCAN_MEASUREMENT	"zpool_scan_stats"
#define	POOL_LATENCY_MEASUREMENT	"zpool_latency"
#define	POOL_QUEUE_MEASUREMENT	"zpool_vdev"
#define	MIN_LAT_INDEX		10  /* minimum latency index 10 = 1024ns */
#define	POOL_IO_SIZE_MEASUREMENT	"zpool_req"
#define	MIN_SIZE_INDEX		9  /* minimum size index 9 = 512 bytes */

nvlist_t *metric_names = NULL;  /* list of metric names with help/type */

/*
 * in cases where ZFS is installed, but not the ZFS dev environment, copy in
 * the needed definitions from libzfs_impl.h
 */
#ifndef _LIBZFS_IMPL_H
struct zpool_handle {
	libzfs_handle_t *zpool_hdl;
	zpool_handle_t *zpool_next;
	char zpool_name[ZFS_MAX_DATASET_NAME_LEN];
	int zpool_state;
	size_t zpool_config_size;
	nvlist_t *zpool_config;
	nvlist_t *zpool_old_config;
	nvlist_t *zpool_props;
	diskaddr_t zpool_start_block;
};
#endif

/*
 * though the prometheus docs don't seem to mention how to handle strange
 * characters for labels, we'll try a conservative approach and filter as if
 * the pool name is an unknowable string.
 *
 * caller is responsible for freeing result
 */
char *
escape_string(char *s) {
	char *c, *d;
	char *t = (char *) malloc(ZFS_MAX_DATASET_NAME_LEN << 1);
	if (t == NULL) {
		fprintf(stderr, "error: cannot allocate memory\n");
		exit(1);
	}

	for (c = s, d = t; *c != '\0'; c++, d++) {
		switch (*c) {
			case '"':
			case '\\':
				*d++ = '\\';
			default:
				*d = *c;
		}
	}
	*d = '\0';
	return (t);
}

/*
 * print help or type values for a given metric name
 */
void
print_help_type(char *metric_name, char *help, char *type) {
	char *strval;
	if (nvlist_lookup_string(metric_names, metric_name, &strval) != 0) {
		if (help != NULL)
			(void) printf("# HELP %s %s\n", metric_name, help);
		if (type != NULL)
			(void) printf("# TYPE %s %s\n", metric_name, type);
		if (nvlist_add_string(metric_names, metric_name, "") != 0) {
			fprintf(stderr, "error: cannot allocate memory\n");
			exit(1);
		}
	}
}

/*
 * As of early 2019, prometheus only has a float64 data type.
 * This is unfortunate because ZFS uses mostly uint64 data type.
 * For high-speed systems or slow-speed systems that have been up
 * for a long time, these counters will overflow the significand,
 * causing queries that take derivatives or differences to seemingly
 * fail. Since most of these counters are only counting up, we can
 * mask them to fit in the significand and reset to zero when full.
 * Queries that then use non-negative derivatives (a best practice)
 * will handle it nicely.
 */
#define SIGNIFICANT_BITS 52

void
print_prom_u64(char *prefix, char *metric, char *label, uint64_t value,
               char *help, char *type) {
	char metric_name[200];
	uint64_t mask = ((1ULL << SIGNIFICANT_BITS) - 1);

	(void) snprintf(metric_name, sizeof(metric_name), "%s_%s", prefix,
	    metric);
	print_help_type(metric_name, help, type);
	if (label != NULL)
		(void) printf("%s{%s} %"PRIu64"\n", metric_name, label,
		    value & mask);
	else
		(void) printf("%s %"PRIu64"\n", metric_name, value & mask);
}

/*
 * doubles are native data type for prometheus, send them through unimpeded
 */
void
print_prom_d(char *prefix, char *metric, char *label, double value,
             char *help, char *type) {
	char metric_name[200];
	(void) snprintf(metric_name, sizeof(metric_name), "%s_%s", prefix,
	    metric);
	print_help_type(metric_name, help, type);
	if (label != NULL)
		(void) printf("%s{%s} %f\n", metric_name, label, value);
	else
		(void) printf("%s %f\n", metric_name, value);
}

/*
 * print_scan_status() prints the details as often seen in the "zpool status"
 * output. However, unlike the zpool command, which is intended for humans,
 * this output is suitable for long-term tracking in prometheus.
 */
int
print_scan_status(nvlist_t *nvroot, const char *pool_name) {
	uint_t c;
	int64_t elapsed;
	uint64_t examined, issued, pass_exam, pass_scanned, pass_issued, paused_time, paused_ts, scan_rate, issue_rate, total, rate, scanned, to_issue;
	uint64_t remaining_time;
	pool_scan_stat_t *ps = NULL;
	double scan_pct_done, issue_pct_done;
	char *state[DSS_NUM_STATES] = {"none", "scanning", "finished",
	                               "canceled"};
	char *func = "unknown_function";
	char *p = SCAN_MEASUREMENT;
	char l[2 * ZFS_MAX_DATASET_NAME_LEN];  /* prometheus label */

	(void) nvlist_lookup_uint64_array(nvroot,
	    ZPOOL_CONFIG_SCAN_STATS,
	    (uint64_t **) &ps, &c);

	/*
	 * ignore if there are no stats or state is bogus
	 */
	if (ps == NULL || ps->pss_state >= DSS_NUM_STATES ||
	    ps->pss_func >= POOL_SCAN_FUNCS)
		return (0);

	switch (ps->pss_func) {
		case POOL_SCAN_NONE:
			func = "none_requested";
			break;
		case POOL_SCAN_SCRUB:
			func = "scrub";
			break;
		case POOL_SCAN_RESILVER:
			func = "resilver";
			break;
#ifdef POOL_SCAN_REBUILD
		case POOL_SCAN_REBUILD:
				func = "rebuild";
				break;
#endif
		default:
			func = "scan";
	}

	scanned = ps->pss_examined;
	issued = ps->pss_issued;
	pass_issued = ps->pss_pass_issued;
	total = ps->pss_to_examine;

	/* overall progress */
	issue_pct_done = 0.0;
	scan_pct_done = 0.0;

	if (ps->pss_to_examine > 0)
		scan_pct_done = 100.0 * scanned / total;
		issue_pct_done = 100.0 * issued / total;

#ifdef EZFS_SCRUB_PAUSED
	paused_ts = ps->pss_pass_scrub_pause;
			paused_time = ps->pss_pass_scrub_spent_paused;
#else
	paused_ts = 0;
	paused_time = 0;
#endif

	/* calculations for this pass */
	if (ps->pss_state == DSS_SCANNING) {
		elapsed = time(NULL) - ps->pss_pass_start - paused_time;
		elapsed = (elapsed > 0) ? elapsed : 1;
		scan_rate = pass_scanned / elapsed;
		issue_rate = pass_issued / elapsed;
		remaining_time = (issue_rate != 0 && total >= issued) ? ((total - issued) / issue_rate) : UINT64_MAX;
		to_issue = total - issued;
		
	} else {
		elapsed = ps->pss_end_time - ps->pss_pass_start - paused_time;
		elapsed = (elapsed > 0) ? elapsed : 1;
		scan_rate = 0;
		issue_rate = 0;
		remaining_time = 0;
		to_issue = 0;
	}
	scan_rate = scan_rate ? scan_rate : 1;
	issue_rate = issue_rate ? issue_rate : 1;

	(void) snprintf(l, sizeof(l), "name=\"%s\",state=\"%s\"", pool_name,
	    state[ps->pss_state]);
	print_prom_u64(p, "start_ts_seconds", l, ps->pss_start_time,
	    "scan start timestamp (epoch)", "gauge");
	print_prom_u64(p, "end_ts_seconds", l, ps->pss_end_time,
	    "scan end timestamp (epoch)", "gauge");
	print_prom_u64(p, "pause_ts_seconds", l, paused_ts,
	    "scan paused at timestamp (epoch)", "gauge");
	print_prom_u64(p, "paused_seconds", l, paused_time,
	    "scan pause duration", "gauge");
	print_prom_u64(p, "remaining_time_seconds", l, remaining_time,
	    "estimate of examination time remaining", "gauge");
	print_prom_u64(p, "errors", l, ps->pss_errors,
	    "errors detected during scan)", "counter");
	print_prom_u64(p, "examined_bytes", l, scanned,
	    "bytes examined", "counter");
	print_prom_u64(p, "issued_bytes", l, issued,
	    "bytes issued", "counter");
	print_prom_u64(p, "examined_pass_bytes", l, scanned,
	    "bytes examined for this pass", "counter");
	print_prom_u64(p, "issued_pass_bytes", l, pass_issued,
	    "bytes issued for this pass", "counter");
	print_prom_d(p, "percent_examined_done_ratio", l, scan_pct_done,
	    "percent of bytes examined", "gauge");
	print_prom_d(p, "percent_issued_done_ratio", l, issue_pct_done,
	    "percent of bytes issued", "gauge");
	print_prom_u64(p, "examined_bytes_per_second", l, scan_rate,
	    "examination rate over current pass", "gauge");
	print_prom_u64(p, "issued_bytes_per_second", l, issue_rate,
	    "issue rate over current pass", "gauge");
	print_prom_u64(p, "to_examine_bytes", l, total,
	    "total bytes to scan", "gauge");
	print_prom_u64(p, "to_issue_bytes", l, to_issue,
	    "bytes remaining to issue", "gauge");

	return (0);
}

/*
 *
 */
char *
get_vdev_name(nvlist_t *nvroot, const char *parent_name) {
	static char vdev_name[256];
	char *vdev_type = NULL;
	uint64_t vdev_id = 0;

	if (nvlist_lookup_string(nvroot, ZPOOL_CONFIG_TYPE,
	    &vdev_type) != 0) {
		vdev_type = "unknown";
	}
	if (nvlist_lookup_uint64(
	    nvroot, ZPOOL_CONFIG_ID, &vdev_id) != 0) {
		vdev_id = -1ULL;
	}
	if (parent_name == NULL) {
		(void) snprintf(vdev_name, sizeof(vdev_name), "%s",
		    vdev_type);
	} else {
		(void) snprintf(vdev_name, sizeof(vdev_name),
		    "%s/%s-%lu",
		    parent_name, vdev_type, vdev_id);
	}
	return (vdev_name);
}

/*
 * get a string suitable for prometheus label that describes this vdev
 *
 * By default only the vdev hierarchical name is shown, separated by '/'
 * If the vdev has an associated path, which is typical of leaf vdevs,
 * then the path is added.
 * It would be nice to have the devid instead of the path, but under
 * Linux we cannot be sure a devid will exist and we'd rather have
 * something than nothing, so we'll use path instead.
 */
char *
get_vdev_desc(nvlist_t *nvroot, const char *parent_name) {
	char vdev_desc[256];
	char *vdev_type = NULL;
	uint64_t vdev_id = 0;
	char vdev_value[256];
	char *vdev_path = NULL;
	char vdev_path_value[256];
	static char res[512];

	if (nvlist_lookup_string(nvroot, ZPOOL_CONFIG_TYPE, &vdev_type) != 0) {
		vdev_type = "unknown";
	}
	if (nvlist_lookup_uint64(nvroot, ZPOOL_CONFIG_ID, &vdev_id) != 0) {
		vdev_id = -1ULL;
	}
	if (nvlist_lookup_string(
	    nvroot, ZPOOL_CONFIG_PATH, &vdev_path) != 0) {
		vdev_path = NULL;
	}

	if (parent_name == NULL) {
		(void) snprintf(vdev_value, sizeof(vdev_value), "vdev=\"%s\"",
		    vdev_type);
	} else {
		(void) snprintf(vdev_value, sizeof(vdev_value),
		    "vdev=\"%s/%s-%"PRIu64"\"",
		    parent_name, vdev_type, vdev_id);
	}
	if (vdev_path == NULL) {
		vdev_path_value[0] = '\0';
	} else {
		(void) snprintf(vdev_path_value, sizeof(vdev_path_value),
		    ",path=\"%s\"", vdev_path);
	}
	(void) snprintf(res, sizeof(res), "%s%s", vdev_value, vdev_path_value);
	return (res);
}

/*
 * vdev latency stats are histograms stored as nvlist arrays of uint64.
 * Latency stats include the ZIO scheduler classes plus lower-level
 * vdev latencies.
 *
 * In many cases, the top-level "root" view obscures the underlying
 * top-level vdev operations. For example, if a pool has a log, special,
 * or cache device, then each can behave very differently. It is useful
 * to see how each is responding.
 */
int
print_vdev_latency_stats(nvlist_t *nvroot, const char *pool_name,
                         const char *parent_name) {
	uint_t c, end;
	nvlist_t *nv, *nv_ex;
	uint64_t *lat_array;
	uint64_t sum;
	char *p = POOL_LATENCY_MEASUREMENT;
	char s[2 * ZFS_MAX_DATASET_NAME_LEN];
	char t[2 * ZFS_MAX_DATASET_NAME_LEN];
	char metric_name[2 * ZFS_MAX_DATASET_NAME_LEN];
	char *vdev_desc = NULL;

	/* short_names become part of the metric name */
	struct lat_lookup {
		char *name;
		char *short_name;
	};
	struct lat_lookup lat_type[] = {
		{ZPOOL_CONFIG_VDEV_TOT_R_LAT_HISTO,	"total_read"},
		{ZPOOL_CONFIG_VDEV_TOT_W_LAT_HISTO,	"total_write"},
		{ZPOOL_CONFIG_VDEV_DISK_R_LAT_HISTO,	"disk_read"},
		{ZPOOL_CONFIG_VDEV_DISK_W_LAT_HISTO,	"disk_write"},
		{ZPOOL_CONFIG_VDEV_SYNC_R_LAT_HISTO,	"sync_read"},
		{ZPOOL_CONFIG_VDEV_SYNC_W_LAT_HISTO,	"sync_write"},
		{ZPOOL_CONFIG_VDEV_ASYNC_R_LAT_HISTO,	"async_read"},
		{ZPOOL_CONFIG_VDEV_ASYNC_W_LAT_HISTO,	"async_write"},
		{ZPOOL_CONFIG_VDEV_SCRUB_LAT_HISTO,	"scrub"},
#ifdef ZPOOL_CONFIG_VDEV_TRIM_LAT_HISTO
		{ZPOOL_CONFIG_VDEV_TRIM_LAT_HISTO,	"trim"},
#endif
		{NULL,					NULL}
	};

	if (nvlist_lookup_nvlist(nvroot,
	    ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
		return (6);
	}

	vdev_desc = get_vdev_desc(nvroot, parent_name);

	for (int i = 0; lat_type[i].name; i++) {
		if (nvlist_lookup_uint64_array(nv_ex,
		    lat_type[i].name, (uint64_t **) &lat_array, &c) != 0) {
			fprintf(stderr, "error: can't get %s\n",
			    lat_type[i].name);
			return (3);
		}
		/* count */
		sum = 0;
		end = c - 1;
		(void) snprintf(metric_name, sizeof(metric_name),
		  "%s_%s_seconds", p, lat_type[i].name);
		print_help_type(metric_name, "latency distribution",
		    "histogram");

		for (int j = 0; j <= end; j++) {
			sum += lat_array[j];
			(void) snprintf(s, sizeof(s),
			    "%s_seconds_bucket", lat_type[i].name);

			if (j >= MIN_LAT_INDEX && j < end) {
				(void) snprintf(t, sizeof(t),
				    "name=\"%s\",%s,le=\"%0.6f\"",
				    pool_name, vdev_desc,
				    (float) (1ULL << j) * 1e-9);
				print_prom_u64(p, s, t, sum,
				    NULL, NULL);
			}
			if (j == end) {
				(void) snprintf(t, sizeof(t),
				    "name=\"%s\",%s,le=\"+Inf\"",
				    pool_name, vdev_desc);
				print_prom_u64(p, s, t, sum,
				    NULL, NULL);

				/* TODO: zpool code update to include sum */
				(void) snprintf(s, sizeof(s),
				    "%s_seconds_sum", lat_type[i].name);
				(void) snprintf(t, sizeof(t),
				    "name=\"%s\",%s",
				    pool_name, vdev_desc);
				print_prom_u64(p, s, t, 0, NULL, NULL);

				(void) snprintf(s, sizeof(s),
				    "%s_seconds_count", lat_type[i].name);
				print_prom_u64(p, s, t, sum, NULL, NULL);
			}
		}
	}
	return (0);
}

/*
 * vdev request size stats are histograms stored as nvlist arrays of uint64.
 * Request size stats include the ZIO scheduler classes plus lower-level
 * vdev sizes. Both independent (ind) and aggregated (agg) sizes are reported.
 *
 * In many cases, the top-level "root" view obscures the underlying
 * top-level vdev operations. For example, if a pool has a log, special,
 * or cache device, then each can behave very differently. It is useful
 * to see how each is responding.
 */
int
print_vdev_size_stats(nvlist_t *nvroot, const char *pool_name,
			 const char *parent_name) {
	uint_t c, end;
	nvlist_t *nv, *nv_ex;
	uint64_t *size_array;
	uint64_t sum;
	char *p = POOL_IO_SIZE_MEASUREMENT;
	char s[2 * ZFS_MAX_DATASET_NAME_LEN];
	char t[2 * ZFS_MAX_DATASET_NAME_LEN];
	char metric_name[2 * ZFS_MAX_DATASET_NAME_LEN];
	char *vdev_desc = NULL;

	/* short_names become part of the metric name */
	struct size_lookup {
	char *name;
	char *short_name;
	};
	struct size_lookup size_type[] = {
		{ZPOOL_CONFIG_VDEV_SYNC_IND_R_HISTO,	"sync_read_ind"},
		{ZPOOL_CONFIG_VDEV_SYNC_IND_W_HISTO,	"sync_write_ind"},
		{ZPOOL_CONFIG_VDEV_ASYNC_IND_R_HISTO,	"async_read_ind"},
		{ZPOOL_CONFIG_VDEV_ASYNC_IND_W_HISTO,	"async_write_ind"},
		{ZPOOL_CONFIG_VDEV_IND_SCRUB_HISTO,	"scrub_read_ind"},
		{ZPOOL_CONFIG_VDEV_SYNC_AGG_R_HISTO,	"sync_read_agg"},
		{ZPOOL_CONFIG_VDEV_SYNC_AGG_W_HISTO,	"sync_write_agg"},
		{ZPOOL_CONFIG_VDEV_ASYNC_AGG_R_HISTO,	"async_read_agg"},
		{ZPOOL_CONFIG_VDEV_ASYNC_AGG_W_HISTO,	"async_write_agg"},
		{ZPOOL_CONFIG_VDEV_AGG_SCRUB_HISTO,	"scrub_read_agg"},
#ifdef ZPOOL_CONFIG_VDEV_IND_TRIM_HISTO
		{ZPOOL_CONFIG_VDEV_IND_TRIM_HISTO,	"trim_write_ind"},
		{ZPOOL_CONFIG_VDEV_AGG_TRIM_HISTO,	"trim_write_agg"},
#endif
		{NULL,					NULL}
	};

	if (nvlist_lookup_nvlist(nvroot,
		ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
	return (6);
	}

	vdev_desc = get_vdev_desc(nvroot, parent_name);

	for (int i = 0; size_type[i].name; i++) {
	if (nvlist_lookup_uint64_array(nv_ex,
		size_type[i].name, (uint64_t **) &size_array, &c) != 0) {
		fprintf(stderr, "error: can't get %s\n",
			size_type[i].name);
		return (3);
	}
	/* count */
	sum = 0;
	end = c - 1;
	(void) snprintf(metric_name, sizeof(metric_name),
	    "%s_%s_bytes", p, size_type[i].short_name);
	print_help_type(metric_name, "I/O request size distribution",
	    "histogram");

	for (int j = 0; j <= end; j++) {
		sum += size_array[j];
		(void) snprintf(s, sizeof(s),
			"%s_bytes_bucket", size_type[i].short_name);

		if (j >= MIN_SIZE_INDEX && j <= end) {
		(void) snprintf(t, sizeof(t),
				"name=\"%s\",%s,le=\"%d\"",
				pool_name, vdev_desc, 1 << j);
		print_prom_u64(p, s, t, sum,
				   NULL, NULL);
		}
		if (j == end) {
		(void) snprintf(t, sizeof(t),
				"name=\"%s\",%s,le=\"+Inf\"",
				pool_name, vdev_desc);
		print_prom_u64(p, s, t, sum,
				   NULL, NULL);

		/*
		 * TODO: zpool code update to include sum?
		 * Though sum is useless here and arguably redundant with other
		 * I/O size measurements. Does it makes sense to sum?
		 */
		(void) snprintf(s, sizeof(s),
				"%s_bytes_sum", size_type[i].short_name);
		(void) snprintf(t, sizeof(t),
				"name=\"%s\",%s",
				pool_name, vdev_desc);
		print_prom_u64(p, s, t, 0, NULL, NULL);

		(void) snprintf(s, sizeof(s),
				"%s_bytes_count", size_type[i].short_name);
		print_prom_u64(p, s, t, sum, NULL, NULL);
		}
	}
	}
	return (0);
}

/*
 * ZIO scheduler queue stats are stored as gauges. This is unfortunate
 * because the values can change very rapidly and any point-in-time
 * value will quickly be obsoleted. It is also not easy to downsample.
 * Thus only the top-level queue stats might be beneficial... maybe.
 */
int
print_queue_stats(nvlist_t *nvroot, const char *pool_name,
                  const char *parent_name) {
	nvlist_t *nv_ex;
	uint64_t value;
	char *p = POOL_QUEUE_MEASUREMENT;
	char s[2 * ZFS_MAX_DATASET_NAME_LEN];

	/* short_names become part of the metric name */
	struct queue_lookup {
		char *name;
		char *short_name;
	};
	struct queue_lookup queue_type[] = {
		{ZPOOL_CONFIG_VDEV_SYNC_R_ACTIVE_QUEUE,  "sync_r_active_queue"},
		{ZPOOL_CONFIG_VDEV_SYNC_W_ACTIVE_QUEUE,  "sync_w_active_queue"},
		{ZPOOL_CONFIG_VDEV_ASYNC_R_ACTIVE_QUEUE, "async_r_active_queue"},
		{ZPOOL_CONFIG_VDEV_ASYNC_W_ACTIVE_QUEUE, "async_w_active_queue"},
		{ZPOOL_CONFIG_VDEV_SCRUB_ACTIVE_QUEUE,  "async_scrub_active_queue"},
		{ZPOOL_CONFIG_VDEV_SYNC_R_PEND_QUEUE,	"sync_r_pend_queue"},
		{ZPOOL_CONFIG_VDEV_SYNC_W_PEND_QUEUE,	"sync_w_pend_queue"},
		{ZPOOL_CONFIG_VDEV_ASYNC_R_PEND_QUEUE,   "async_r_pend_queue"},
		{ZPOOL_CONFIG_VDEV_ASYNC_W_PEND_QUEUE,   "async_w_pend_queue"},
		{ZPOOL_CONFIG_VDEV_SCRUB_PEND_QUEUE,     "async_scrub_pend_queue"},
		{NULL,                                   NULL}
	};

	if (nvlist_lookup_nvlist(nvroot,
	    ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
		return (6);
	}

	(void) snprintf(s, sizeof(s), "name=\"%s\",%s",
	    pool_name, get_vdev_desc(nvroot, parent_name));

	for (int i = 0; queue_type[i].name; i++) {
		if (nvlist_lookup_uint64(nv_ex,
		    queue_type[i].name, (uint64_t *) &value) != 0) {
			fprintf(stderr, "error: can't get %s\n",
			    queue_type[i].name);
			return (3);
		}
		print_prom_u64(p, queue_type[i].short_name, s, value,
		    "queue depth", "gauge");
	}
	return (0);
}

/*
 * Summary stats for each vdev are familiar to the "zpool status"
 * and "zpool list" users.
 */
int
print_summary_stats(nvlist_t *nvroot, const char *pool_name,
                    const char *parent_name) {
	uint_t c;
	vdev_stat_t *vs;
	char *p = POOL_MEASUREMENT;
	char l[2 * ZFS_MAX_DATASET_NAME_LEN];
	char *vdev_desc = get_vdev_desc(nvroot, parent_name);

	if (nvlist_lookup_uint64_array(nvroot,
	    ZPOOL_CONFIG_VDEV_STATS, (uint64_t **) &vs, &c) != 0) {
		return (0);
	}
	/*
	 * Include the state of the vdev as a prometheus label. This allows
	 * for filtering in queries. However, these do no map directly to all
	 * of the possible human-readable names in the zpool(8) command output.
	 * For example, a healthy spare has state "AVAIL" in zpool, but "ONLINE" here.
	 */
	(void) snprintf(l, sizeof(l), "name=\"%s\",state=\"%s\",%s",
	    pool_name, zpool_state_to_name(vs->vs_state, vs->vs_aux),
            vdev_desc);

	/* Show the raw state enums. See zfs.h for the current descriptions	 */
	print_prom_u64(p, "state", l, vs->vs_state, "current state, see zfs.h",
	    "gauge");
	print_prom_u64(p, "aux_state", l, vs->vs_aux, "auxiliary state, see zfs.h",
        "gauge");

	print_prom_u64(p, "alloc_bytes", l, vs->vs_alloc,
	    "allocated size", "gauge");
	print_prom_u64(p, "free_bytes", l, vs->vs_space - vs->vs_alloc,
	    "free space", "gauge");
	print_prom_u64(p, "size_bytes", l, vs->vs_space,
	    "pool size", "gauge");

	print_prom_u64(p, "read_bytes", l, vs->vs_bytes[ZIO_TYPE_READ],
	    "read bytes", "counter");
	print_prom_u64(p, "read_errors", l, vs->vs_read_errors,
	    "read errors", "counter");
	print_prom_u64(p, "read_ops", l, vs->vs_ops[ZIO_TYPE_READ],
	    "read ops", "counter");

	print_prom_u64(p, "write_bytes", l, vs->vs_bytes[ZIO_TYPE_WRITE],
	    "write bytes", "counter");
	print_prom_u64(p, "write_errors", l, vs->vs_write_errors,
	    "write errors", "counter");
	print_prom_u64(p, "write_ops", l, vs->vs_ops[ZIO_TYPE_WRITE],
	    "write ops", "counter");

	print_prom_u64(p, "cksum_errors", l, vs->vs_checksum_errors,
	    "checksum errors", "counter");
	print_prom_u64(p, "fragmentation_ratio", l, vs->vs_fragmentation / 100,
	    "free space fragmentation metric", "gauge");

	return (0);
}

/*
 * recursive stats printer
 */
typedef int (*stat_printer_f)(nvlist_t *, const char *, const char *);

int
print_recursive_stats(stat_printer_f func, nvlist_t *nvroot,
                      const char *pool_name, const char *parent_name,
                      int descend) {
	uint_t c, children;
	nvlist_t **child;
	char vdev_name[256];
	int err;

	err = func(nvroot, pool_name, parent_name);
	if (err)
		return (err);

	if (descend && nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
	    &child, &children) == 0) {
		(void) strncpy(vdev_name, get_vdev_name(nvroot, parent_name),
		    sizeof(vdev_name));
		vdev_name[sizeof(vdev_name) - 1] = '\0';

		for (c = 0; c < children; c++) {
			print_recursive_stats(func, child[c], pool_name,
			    vdev_name, descend);
		}
	}
	return (0);
}

/*
 * call-back to print the stats from the pool config
 *
 * Note: if the pool is broken, this can hang indefinitely
 */
int
print_stats(zpool_handle_t *zhp, void *data) {
	uint_t c;
	int err = 0;
	boolean_t missing;
	nvlist_t *nv, *nv_ex, *config, *nvroot;
	vdev_stat_t *vs;
	uint64_t *lat_array;
	char *pool_name;
	pool_scan_stat_t *ps = NULL;

	/* if not this pool return quickly */
	if (data &&
	    strncmp(data, zhp->zpool_name, ZFS_MAX_DATASET_NAME_LEN) != 0)
		return (0);

	if (zpool_refresh_stats(zhp, &missing) != 0)
		return (1);

	config = zpool_get_config(zhp, NULL);

	if (nvlist_lookup_nvlist(
	    config, ZPOOL_CONFIG_VDEV_TREE, &nvroot) != 0) {
		return (2);
	}
	if (nvlist_lookup_uint64_array(nvroot,
	    ZPOOL_CONFIG_VDEV_STATS,
	    (uint64_t **) &vs, &c) != 0) {
		return (3);
	}

	pool_name = escape_string(zhp->zpool_name);
	printf("### %s stats for %s\n", COMMAND_NAME, pool_name);

	err = print_recursive_stats(print_summary_stats, nvroot,
	    pool_name, NULL, 1);
	if (err == 0)
		err = print_recursive_stats(print_vdev_latency_stats, nvroot,
		    pool_name, NULL, 1);
	if (err == 0)
		err = print_recursive_stats(print_vdev_size_stats, nvroot,
		    pool_name, NULL, 1);
	if (err == 0)
		err = print_recursive_stats(print_queue_stats, nvroot,
		    pool_name, NULL, 0);
	if (err == 0)
		err = print_scan_status(nvroot, pool_name);

	free(pool_name);
	return (0);
}


int
main(int argc, char *argv[]) {
	libzfs_handle_t *g_zfs;
	if ((g_zfs = libzfs_init()) == NULL) {
		fprintf(stderr,
		    "error: cannot initialize libzfs. "
		    "Is the zfs module loaded or zrepl running?");
		exit(1);
	}
	if (nvlist_alloc(&metric_names, NV_UNIQUE_NAME, 0) != 0) {
	    fprintf(stderr, "error: cannot allocate memory");
	    exit(1);
	}
	if (argc > 1) {
		return (zpool_iter(g_zfs, print_stats, argv[1]));
	} else {
		return (zpool_iter(g_zfs, print_stats, NULL));
	}
}
