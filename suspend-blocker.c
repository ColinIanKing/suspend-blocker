/*
 * Copyright (C) 2013-2019 Canonical
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <float.h>
#include <signal.h>
#include <sys/time.h>
#include <json.h>
#include <math.h>
#include <inttypes.h>

#define APP_NAME			"suspend-blocker"

#define STATE_UNDEFINED                 0x00000000
#define STATE_ENTER_SUSPEND             0x00000001
#define STATE_EXIT_SUSPEND              0x00000002
#define STATE_ACTIVE_WAKELOCK           0x00000004
#define STATE_SUSPEND_SUCCESS           0x00000008
#define STATE_FREEZE_ABORTED            0x00000010
#define STATE_LATE_HAS_WAKELOCK         0x00000020
#define STATE_DEEP_SUSPEND_START        0x00000040
#define STATE_DEEP_SUSPEND_END          0x00000080
#define STATE_RESUME_CAUSE              0x00000100
#define STATE_FREEZE_TASKS_REFUSE	0x00000200
#define STATE_SUSPEND_FAIL_CAUSE	0x00000400

#define OPT_WAKELOCK_BLOCKERS		0x00000001
#define OPT_VERBOSE			0x00000002
#define OPT_HISTOGRAM			0x00000004
#define OPT_RESUME_CAUSES		0x00000008
#define OPT_QUIET			0x00000010
#define OPT_PROC_WAKELOCK		0x00000020
#define OPT_HISTOGRAM_DECADES		0x00000040
#define OPT_FREQUENCY_REPORT		0x00000080

#define HASH_SIZE			(1997)
#define MAX_INTERVALS			(30)

#define WAKELOCK_START			(0)
#define WAKELOCK_END			(1)

#define WAKELOCK_NAME_SZ		(128)
#define MS				(1000.0)

#define SUSPEND_SUCCESS			(0)
#define SUSPEND_FAIL			(1)
#define SUSPEND_DURATION		(2)

#define FLOAT_TINY			(0.0000001)
#define FLOAT_CMP(a, b)			(fabs((a) - (b)) < FLOAT_TINY)

/*
 *  Calculate wakelock delta between start and end epoc
 */
#define WL_DELTA(i, f)					\
	((double)(wakelocks[i]->stats[WAKELOCK_END].f -	\
	 (double)wakelocks[i]->stats[WAKELOCK_START].f))

#define NO_NEG(v) ((v) < 0.0 ? 0.0 : (v))

#define FREQ_SIZE	(100)

typedef struct {
	unsigned int	succeed_count;
	unsigned int	failed_count;
	unsigned int	*reason_counts;
} freq_info_t;

typedef struct reason {
	char *reason;
	struct reason *next;
} reason_t;

typedef struct {
	uint64_t	active_count;	/* active count (not used in this tool) */
	uint64_t	count;		/* unlock count */
	uint64_t	expire_count;	/* expire count */
	uint64_t	wakeup_count;	/* wakeup count,
					   wakelock suspend, wait for wakelock */
	double		active_since;	/* no-op */
	double		total_time;	/* total time wakelock is active */
	double		sleep_time;	/* time preventing kernel from sleeping */
	double		max_time;	/* max time locked? */
	double		prevent_time;	/* prevent suspend time */
	double		last_change;	/* when lock was last locked/unlocked */
} wakelock_stats;

typedef struct {
	char 		*name;		/* name of wakelock */
	wakelock_stats	stats[2];	/* wakelock start + end stats */
} wakelock_info;

typedef struct {
	double	whence;
	bool	whence_valid;		/* whence time is valid or not? */
	double	pm_whence;		/* when we got a PM event */
	bool	pm_whence_valid;	/* is the above valid or not? */
	char	whence_text[32];	/* event text */
} timestamp;

typedef struct {
	char *name;			/* name of counter */
	int  count;			/* number of times detected */
} counter_info;

typedef struct time_delta_info {
	int    type;			/* info type */
	double start;			/* time it started */
	char   *reason;			/* resume reason */
	double delta;
	bool   accurate;		/* accurate or not? */
	struct time_delta_info *next;
} time_delta_info;

static int opt_flags;
static double opt_wakelock_duration;
static wakelock_info *wakelocks[HASH_SIZE];
static bool keep_running = true;
static int print(const char *format, ...) __attribute__((format(printf, 1, 2)));

/*
 *  Attempt to catch a range of signals so
 *  we can clean
 */
static const int signals[] = {
	/* POSIX.1-1990 */
#ifdef SIGHUP
	SIGHUP,
#endif
#ifdef SIGINT
	SIGINT,
#endif
#ifdef SIGQUIT
	SIGQUIT,
#endif
#ifdef SIGFPE
	SIGFPE,
#endif
#ifdef SIGTERM
	SIGTERM,
#endif
#ifdef SIGUSR1
	SIGUSR1,
#endif
#ifdef SIGUSR2
	SIGUSR2,
	/* POSIX.1-2001 */
#endif
#ifdef SIGXCPU
	SIGXCPU,
#endif
#ifdef SIGXFSZ
	SIGXFSZ,
#endif
	/* Linux various */
#ifdef SIGIOT
	SIGIOT,
#endif
#ifdef SIGSTKFLT
	SIGSTKFLT,
#endif
#ifdef SIGPWR
	SIGPWR,
#endif
#ifdef SIGINFO
	SIGINFO,
#endif
#ifdef SIGVTALRM
	SIGVTALRM,
#endif
	-1,
};

/*
 *  timeval_to_double()
 *	convert timeval to seconds as a double
 */
static double timeval_to_double(const struct timeval *tv)
{
	return (double)tv->tv_sec + ((double)tv->tv_usec / 1000000.0);
}

/*
 *  print
 *	printf that can be suppressed when OPT_QUIET is set
 */
static int print(const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = (opt_flags & OPT_QUIET) ? 0 : vprintf(format, ap);
	va_end(ap);

	return ret;
}


/*
 *  hash_djb2a()
 *	Hash a string, from Dan Bernstein comp.lang.c (xor version)
 */
static unsigned long hash_djb2a(const char *str)
{
	register unsigned long hash = 5381;
	register int c;

	while ((c = *str++)) {
		/* (hash * 33) ^ c */
		hash = ((hash << 5) + hash) ^ c;
	}
	return hash % HASH_SIZE;
}

/*
 *  wakelock_new()
 *	create a new wakelock, nstat denotes start or end wakelock event
 *	collection time
 */
static void wakelock_new(const char *name, wakelock_stats *wakelock, int nstat)
{
	unsigned long h = hash_djb2a(name);
	int i;

	for (i = 0; i < HASH_SIZE; i++) {
		if (wakelocks[h] == NULL) {
			wakelocks[h] = calloc(1, sizeof(*wakelocks[h]));
			if (!wakelocks[h]) {
				fprintf(stderr, "Out of memory\n");
				exit(EXIT_FAILURE);
			}
			wakelocks[h]->name = strdup(name);
			if (!wakelocks[h]->name) {
				free(wakelocks[h]);
				fprintf(stderr, "Out of memory\n");
				exit(EXIT_FAILURE);
			}
			memcpy(&wakelocks[h]->stats[nstat], wakelock, sizeof(*wakelock));
			return;
		}
		h = (h + 1) % HASH_SIZE;
	}
}

/*
 *  wakelock_update()
 *	update wakelock stats
 */
static void wakelock_update(const char *name, wakelock_stats *wakelock, int nstat)
{
	unsigned long h = hash_djb2a(name);
	int i;

	for (i = 0; i < HASH_SIZE; i++) {
		if (wakelocks[h] &&
		    !strcmp(name, wakelocks[h]->name)) {
			memcpy(&wakelocks[h]->stats[nstat], wakelock, sizeof(*wakelock));
			return;
		}
		h = (h + 1) % HASH_SIZE;
	}

	wakelock_new(name, wakelock, nstat);
}

/*
 *  wakelock_free()
 *	free up wakelock hash table
 */
static void wakelock_free(void)
{
	int i;

	for (i = 0; i < HASH_SIZE; i++) {
		if (wakelocks[i]) {
			free(wakelocks[i]);
			wakelocks[i] = NULL;
		}
	}
}

/*
 *  wakelock_read_sys()
 *	read wakelock status, nstat indicates start or end epoc, from
 *	/sys interface
 */
static int wakelock_read_sys(const int nstat)
{
	FILE *fp;
	char buf[4096];
	int line;

	if ((fp = fopen("/sys/kernel/debug/wakeup_sources", "r")) == NULL)
		return 1;

	for (line = 0; fgets(buf, sizeof(buf), fp) != NULL; line++) {
		wakelock_stats wakelock;
		char name[WAKELOCK_NAME_SZ];

		if (!line)
			continue;	/* skip header */

		memset(&wakelock, 0, sizeof(wakelock));
		if (sscanf(buf, "%127s"
		    " %" SCNu64 " %" SCNu64 " %" SCNu64
		    " %" SCNu64 " %lg %lg %lg %lg %lg"
		    ,
		    name,
		    &wakelock.active_count,
		    &wakelock.count,			/* aka event_count */
		    &wakelock.wakeup_count,
		    &wakelock.expire_count,

		    &wakelock.active_since,
		    &wakelock.total_time,
		    &wakelock.max_time,
		    &wakelock.last_change,
		    &wakelock.prevent_time		/* aka prevent_suspend_time */
		    ) == 10)
			wakelock_update(name, &wakelock, nstat);
	}
	(void)fclose(fp);

	return 0;
}

/*
 *  wakelock_read_proc()
 *	read wakelock status, nstat indicates start or end epoc, from
 *	/proc interface
 */
static int wakelock_read_proc(const int nstat)
{
	FILE *fp;
	char buf[4096];
	int line;

	if ((fp = fopen("/proc/wakelocks", "r")) == NULL)
		return 1;

	for (line = 0; fgets(buf, sizeof(buf), fp) != NULL; line++) {
		wakelock_stats wakelock;
		char name[WAKELOCK_NAME_SZ];

		if (!line)
			continue;	/* skip header */

		memset(&wakelock, 0, sizeof(wakelock));
		if (sscanf(buf, "\"%127[^\"]\" %" SCNu64 " %" SCNu64 " %" SCNu64
		    " %lg %lg %lg %lg %lg",
		    name,
		    &wakelock.count, &wakelock.expire_count,
		    &wakelock.wakeup_count, &wakelock.active_since,
		    &wakelock.total_time, &wakelock.sleep_time,
		    &wakelock.max_time, &wakelock.last_change) == 9) {
			wakelock.total_time /= 1000000.0;
			wakelock.sleep_time /= 1000000.0;
			wakelock.max_time /= 1000000.0;
			wakelock.last_change /= 1000000.0;
			wakelock_update(name, &wakelock, nstat);
		}
	}
	(void)fclose(fp);

	return 0;
}

/*
 *  wakelock_read()
 *	read wakelock status, nstat indicates start or end epoc
 */
static int wakelock_read(const int nstat)
{
	int ret;

	ret = wakelock_read_proc(nstat);
	if (ret)
		ret = wakelock_read_sys(nstat);
	return ret;
}

/*
 *  wakelock_sort()
 *	qsort comparitor to sort wakelock hack by wakelock name
 */
static int wakelock_sort(const void *p1, const void *p2)
{
	wakelock_info *const *w1 = (wakelock_info *const *)p1;
	wakelock_info *const *w2 = (wakelock_info *const *)p2;

	if (!*w1 && !*w2)
		return 0;
	if (!*w2)
		return -1;
	if (!*w1)
		return 1;

	return strcmp((*w1)->name, (*w2)->name);
}

/*
 *  json_null
 *	report error if json object is null
 */
static void json_null(json_object *obj, char *name)
{
	if (obj == NULL)
		fprintf(stderr, "Cannot allocate json %s.\n", name);
}

/*
 *  json_array()
 *	create new json array
 */
static json_object *json_array(void)
{
	json_object *obj = json_object_new_array();

	json_null(obj, "array");
	return obj;
}

/*
 *  json_int()
 *	create new json object from an integer
 */
static json_object *json_int(const int i)
{
	json_object *obj = json_object_new_int(i);

	json_null(obj, "integer");
	return obj;
}

/*
 *  json_double()
 *	create new json object from a double
 */
static json_object *json_double(const double d)
{
	json_object *obj = json_object_new_double(d);

	json_null(obj, "double");
	return obj;
}

/*
 *  json_str()
 *	create new json object from a C string
 */
static json_object *json_str(const char *str)
{
	json_object *obj = json_object_new_string(str);

	json_null(obj, "string");
	return obj;
}

/*
 *  json_obj()
 *	create new json
 */
static json_object *json_obj(void)
{
	json_object *obj = json_object_new_object();

	json_null(obj, "object");
	return obj;
}

/*
 *  wakelock_check()
 *	check wakelock activity
 */
static void wakelock_check(double request_duration, double duration, json_object *json_results)
{
	int i;
	json_object *results, *obj, *array = NULL, *wl_item;

	if (json_results) {
		if ((results = json_obj()) == NULL)
			goto out;
		json_object_object_add(json_results, "wakelock-data", results);

		if ((obj = json_double(duration)) == NULL)
			goto out;
		json_object_object_add(results, "duration-seconds", obj);
	}

	qsort(wakelocks, HASH_SIZE, sizeof(wakelock_info *), wakelock_sort);
	/* since we're sorted, if entry 0 is empty then we have no wakelocks */
	if (!wakelocks[0]) {
		print("No wakelock data.\n");
		return;
	}

	if (json_results) {
		if ((array = json_array()) == NULL)
			goto out;
		json_object_object_add(results, "wakelocks", array);
	}

	print("%-32s %-8s %-8s %-8s %-8s %-8s %-8s %-8s\n",
		"Wakelock", "Active", "Count", "Expire", "Wakeup", "Total", "Sleep", "Prevent");
	print("%-32s %-8s %-8s %-8s %-8s %-8s %-8s %-8s\n",
		"Name", "count", "", "count", "count", "time %", "time %", "time %");
	for (i = 0; i < HASH_SIZE; i++) {
		if (wakelocks[i]) {
			double	d_count = WL_DELTA(i, count),
				d_active_count = WL_DELTA(i, active_count),
				d_expire_count = WL_DELTA(i, expire_count),
				d_wakeup_count = WL_DELTA(i, wakeup_count),
				d_total_time = (100.0 * WL_DELTA(i, total_time) / MS) / duration,
				d_sleep_time = (100.0 * WL_DELTA(i, sleep_time) / MS) / duration,
				d_prevent_time = (100.0 * WL_DELTA(i, prevent_time) / MS) / duration;

			d_total_time = NO_NEG(d_total_time);
			d_sleep_time = NO_NEG(d_sleep_time);
			d_prevent_time = NO_NEG(d_prevent_time);

			/* dump out stats if non-zero */
			if (d_active_count + d_count + d_expire_count + d_wakeup_count + d_total_time + d_sleep_time + d_prevent_time > 0.0) {
				print("%-32.32s %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f\n",
					wakelocks[i]->name,
					d_active_count,
					d_count, d_expire_count, d_wakeup_count,
					d_total_time, d_sleep_time, d_prevent_time);

				if (json_results) {
					if ((wl_item = json_obj()) == NULL)
						goto out;
					json_object_array_add(array, wl_item);
					if ((obj = json_str(wakelocks[i]->name)) == NULL)
						goto out;
					json_object_object_add(wl_item, "wakelock", obj);
					if ((obj = json_double(d_active_count / duration)) == NULL)
						goto out;
					json_object_object_add(wl_item, "active_count_per_second", obj);
					if ((obj = json_double(d_count / duration)) == NULL)
						goto out;
					json_object_object_add(wl_item, "count_per_second", obj);
					if ((obj = json_double(d_expire_count / duration)) == NULL)
						goto out;
					json_object_object_add(wl_item, "expire_count_per_second", obj);
					if ((obj = json_double(d_wakeup_count / duration)) == NULL)
						goto out;
					json_object_object_add(wl_item, "wakeup_count_per_second", obj);
					if ((obj = json_double(d_total_time)) == NULL)
						goto out;
					json_object_object_add(wl_item, "total_time_percent", obj);
					if ((obj = json_double(d_sleep_time)) == NULL)
						goto out;
					json_object_object_add(wl_item, "sleep_time_percent", obj);
					if ((obj = json_double(d_prevent_time)) == NULL)
						goto out;
					json_object_object_add(wl_item, "prevent_time_percent", obj);
				}
			}
		}
	}
	printf("Requested test duration: %.2f seconds, actual duration: %.2f seconds\n",
		request_duration, duration);
out:
	return;
}


/*
 *  timestamp_init()
 *	initialize a timestamp
 */
static inline void timestamp_init(timestamp *ts)
{
	ts->whence = -1.0;
	ts->whence_valid = false;
	ts->pm_whence = -1.0;
	ts->pm_whence_valid = false;
	ts->whence_text[0] = '\0';
}

static int counter_info_cmp(const void *p1, const void *p2)
{
	counter_info const *w1 = (counter_info const *)p1;
	counter_info const *w2 = (counter_info const *)p2;
	int diff = w2->count - w1->count;

	if ((diff == 0) && (w1->count | w2->count))
		return strcmp(w1->name, w2->name);
	else
		return diff;
}

/*
 *  counter_free()
 *	free counter hash table
 */
static void counter_free(counter_info counter[])
{
	unsigned long i;

	for (i = 0; i < HASH_SIZE; i++) {
		if (counter[i].name)
			free(counter[i].name);

		memset(&counter[i], 0, sizeof(counter_info));
	}
}

/*
 *  counter_increment()
 *	increment a hashed counter
 */
static void counter_increment(const char *name, counter_info counter[])
{
	unsigned long i = hash_djb2a(name);
	unsigned long j = 0;

	for (j = 0; j < HASH_SIZE; j++) {
		if (counter[i].name == NULL) {
			counter[i].name = strdup(name);
			if (counter[i].name == NULL) {
				fprintf(stderr, "Out of memory!\n");
				exit(EXIT_FAILURE);
			}
			counter[i].count++;
			return;
		}
		if (strcmp(counter[i].name, name) == 0) {
			counter[i].count++;
			return;
		}
		i = (i + 1) % HASH_SIZE;
	}

	fprintf(stderr, "Hash table full!\n");
	exit(EXIT_FAILURE);
}

/*
 *  counter_dump()
 *	output counters
 */
static void counter_dump(counter_info counter[], const char *label, json_object *json_results)
{
	int i;
	int total = 0;
	bool none = true;

	for (i = 0; i < HASH_SIZE; i++)
		if (counter[i].name)
			total += counter[i].count;

	qsort(counter, HASH_SIZE, sizeof(counter_info), counter_info_cmp);

	for (i = 0; i < HASH_SIZE; i++) {
		if (counter[i].name) {
			print("  %-28.28s %8d  %5.2f%%\n", counter[i].name, counter[i].count,
				100.0 * (double)counter[i].count / (double)total);
			none = false;
		}
	}
	if (none)
		printf("  None\n");
	print("\n");

	if (json_results) {
		json_object *array;

		if ((array = json_array()) == NULL)
			return;

		for (i = 0; i < HASH_SIZE; i++) {
			if (counter[i].name) {
				json_object *result, *obj;

				if ((result = json_obj()) == NULL) {
					break;
				}
				json_object_array_add(array, result);

				if ((obj = json_str(counter[i].name)) == NULL)
					break;
				json_object_object_add(result, "name", obj);

				if ((obj = json_int(counter[i].count)) == NULL)
					break;
				json_object_object_add(result, "count", obj);

				if ((obj = json_double((double)counter[i].count / (double)total)) == NULL)
					break;
				json_object_object_add(result, "percent", obj);
			}
		}

		json_object_object_add(json_results, label, array);
	}
}

static int double_cmp(const void *v1, const void *v2)
{
	double const *i1 = (double const *)v1;
	double const *i2 = (double const *)v2;
	double d = *i1 - *i2;


	if (d < 0.0)
		return -1;
	if (d > 0.0)
		return 1;
	return 0;
}

/*
 *  time_calc_stats()
 *
 */
static int time_calc_stats(
	time_delta_info *info,
	double *mode,
	double *median,
	double *mean,
	double *min,
	double *max,
	double *sum)
{
	time_delta_info *tdi;
	int total;
	double *deltas;
	int i;
	int count = 0, max_count = 0, delta = -1;

	*mode = 0.0;
	*median = 0.0;
	*mean = 0.0;
	*min = 0.0;
	*max = 0.0;
	*sum = 0.0;

	for (total = 0, tdi = info; tdi; tdi = tdi->next) {
		if (tdi->type == SUSPEND_FAIL)
			continue;
		if (total == 0) {
			*min = tdi->delta;
			*max = tdi->delta;
		} else {
			if (*min > tdi->delta)
				*min = tdi->delta;
			if (*max < tdi->delta)
				*max = tdi->delta;
		}
		*sum += tdi->delta;
		total++;
	}

	if (total == 0)
		return 0;

	*mean = *sum / (double)total;

	deltas = calloc(total, sizeof(double));
	if (deltas == NULL) {
		fprintf(stderr, "Cannot allocate array for mode calculation.\n");
		return 1;
	}

	for (i = 0, tdi = info; tdi; tdi = tdi->next) {
		if (tdi->type == SUSPEND_FAIL)
			continue;
		deltas[i++] = tdi->delta;
	}

	qsort(deltas, total, sizeof(double), double_cmp);

	/* Calculate mode to nearest 1/2 second */
	for (i = 0; i < total; i++) {
		if (delta != (int)rint(2.0 * deltas[i])) {
			delta = (int)rint(2.0 * deltas[i]);
			count = 1;
		} else {
			count++;
		}

		if (count >= max_count) {
			max_count = count;
			*mode = delta / 2.0;
		}

	}

	/* Calculate median */
	if (total % 2 == 1) {
		*median = deltas[total / 2];
	} else {
		*median = (deltas[total / 2] +
			  deltas[(total / 2) - 1]) / 2.0;
	}

	free(deltas);

	return 0;
}

/*
 *  histogram_dump()
 *	dump out a histogram of durations
 */
static void histogram_dump(time_delta_info *info, const char *message)
{
	int histogram[MAX_INTERVALS];
	double sum[MAX_INTERVALS];
	int i;
	double delta_sum = 0.0;
	int max = -1;
	int min = MAX_INTERVALS;
	time_delta_info *tdi = info;
	int total = 0;
	int accurate = 0;

	memset(histogram, 0, sizeof(histogram));
	for (i = 0; i < MAX_INTERVALS; i++)
		sum[i] = 0.0;

	for (tdi = info; tdi; tdi = tdi->next) {
		if (tdi->type == SUSPEND_FAIL)
			continue;
		total++;
		if (tdi->accurate)
			accurate++;
	}

	for (tdi = info; tdi; tdi = tdi->next) {
		double d = tdi->delta;

		if (tdi->type == SUSPEND_FAIL)
			continue;

		for (i = 0; (i < MAX_INTERVALS - 1) && (d > 0.125); i++) {
			if (opt_flags & OPT_HISTOGRAM_DECADES)
				d = d - 10.0;
			else
				d = d / 2.0;
		}

		histogram[i]++;
		sum[i] += tdi->delta;
		delta_sum += tdi->delta;
		if (i > max)
			max = i;
		if (i < min)
			min = i;
	}

	print("%s\n", message);
	if (max == -1) {
		print("  No values.\n");
	} else {
		double range1 = 0.0, range2;

		if (opt_flags & OPT_HISTOGRAM_DECADES)
			range2 = 10.0;
		else
			range2 = 0.125;

		printf("   Interval (seconds)          Frequency    Cumulative Time (Seconds)\n");
		for (range1 = 0.0, i = 0; i < MAX_INTERVALS; i++) {
			if (i >= min && i <= max) {
				double pc = 100.0 * (double) histogram[i] / (double)total;
				if (i == MAX_INTERVALS - 1)
					print("  %8.3f -              %6d  %5.2f%%  %9.2f  %5.2f%%\n", range1, histogram[i], pc, sum[i], 100.0 * sum[i] / delta_sum);
				else
					print("  %8.3f - %8.3f     %6d  %5.2f%%  %9.2f  %5.2f%%\n", range1, range2 - 0.001, histogram[i], pc, sum[i], 100.0 * sum[i] / delta_sum);
			}
			range1 = range2;
			if (opt_flags & OPT_HISTOGRAM_DECADES)
				range2 += 10.0;
			else
				range2 = range2 + range2;
		}
		if (accurate != total) {
			print("NOTE: %5.2f%% of the samples were inaccurate estimates.\n",
				100.0 * (double)(total-accurate) / (double)total);
		}
	}
	print("\n");
}


/*
 *  frequency_dump()
 * 	for importing into a spreadsheet
 */
static void frequency_dump(
	time_delta_info *suspend_list,
	const int opt_freq_min)
{
	time_delta_info *tdi;
	double t_start = 1.0e50, t_end = 0.0;
	int hours, i;
	freq_info_t *freq;
	reason_t *reason_list = NULL, *r;
	int reasons = 0;
	int secs = opt_freq_min * 60;

	for (tdi = suspend_list; tdi; tdi = tdi->next) {
		if (t_end < tdi->start)
			t_end = tdi->start;
		if (t_start > tdi->start)
			t_start = tdi->start;
		if (tdi->reason) {
			bool found = false;

			for (r = reason_list; r; r = r->next) {
				if (!strcmp(r->reason, tdi->reason)) {
					found = true;
					break;
				}
			}
			if (!found) {
				r = malloc(sizeof(reason_t));
				if (!r) {
					fprintf(stderr, "Cannot allocate memory\n");
					exit(EXIT_FAILURE);
				}
				r->reason = tdi->reason;
				r->next = reason_list;
				reason_list = r;
				reasons++;
			}
		}
	}

	hours = (int)(((t_end - t_start) / secs) + 0.9999);
	if ((reasons < 1) || (hours < 1)) {
		printf("\nNot enough data for frequency data\n");
		goto free_list;
	}
	freq = alloca(sizeof(freq_info_t) * hours);

	memset(freq, 0, sizeof(freq_info_t) * hours);
	for (i = 0; i < hours; i++)
		freq[i].reason_counts = calloc(reasons, sizeof(unsigned int));

	for (tdi = suspend_list; tdi; tdi = tdi->next) {
		int whence = (int)((tdi->start - t_start) / secs);
		if (tdi->type == SUSPEND_FAIL)
			freq[whence].failed_count++;
		else
			freq[whence].succeed_count++;
		if (tdi->reason) {
			for (i = 0, r = reason_list; r; r = r->next, i++) {
				if (!strcmp(r->reason, tdi->reason))
					freq[whence].reason_counts[i]++;
			}
		}
	}

	printf("\n%s\t%s\t%s\t%s", "Time", "Hour", "Good", "Failed");
	for (r = reason_list; r; r = r->next)
		printf("\t%s", r->reason);
	printf("\n");

	for (i = 0; i < hours; i++) {
		int j;
		time_t t = (time_t)(t_start + (3600.0 * (double)i));
		struct tm *tm;

		tm = localtime(&t);
		printf("%2.2d:%2.2d\t%d\t%u\t%u",
			tm->tm_hour, tm->tm_min, i,
			freq[i].succeed_count,
			freq[i].failed_count);
		for (j = 0; j < reasons; j++)
			printf("\t%u", freq[i].reason_counts[j]);
		printf("\n");
	}
	printf("\nPrefixes:\n");
	printf(" 'A:' - Aborted suspend\n");
	printf(" 'R:' - Resumed\n");

free_list:
	for (r = reason_list; r;) {
		reason_t *next = r->next;

		free(r);
		r = next;
	}
}


/*
 *  Parse PM time stamps of the form:
 *  	PM: suspend entry 2013-06-20 14:16:08.865677626 UTC
 */
static void parse_pm_timestamp(const char *ptr, timestamp *ts)
{
	struct tm tm;
	double sec;
	int n;

	memset(&tm, 0, sizeof(tm));

	n = sscanf(ptr, "%4d-%2d-%2d %2d:%2d:%20lf",
		&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		&tm.tm_hour, &tm.tm_min, &sec);
	if (n != 6) {
		ts->pm_whence = -1.0;
		ts->pm_whence_valid = false;
		*ts->whence_text = '\0';
		return;
	}

	sprintf(ts->whence_text, "%2.2d:%2.2d:%08.5f",
		tm.tm_hour, tm.tm_min, sec);

	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	tm.tm_sec = (int)sec;

	ts->pm_whence = sec - (double)tm.tm_sec + (double)mktime(&tm);
	ts->pm_whence_valid = true;
}

/*
 *  Parse kernel timestamp, this is not accurate at all, but
 *  it is better than nothing (marginally better). It is of the
 *  form:
 *	[ 2476.670867] suspend: enter suspend
 */
static void parse_timestamp(const char *line, timestamp *ts)
{
	char *ptr1, *ptr2;

	ptr1 = strstr(line, "[");
	ptr2 = strstr(line, "]");

	ts->whence_valid = false;
	ts->whence = -1.0;

	if (ptr1 && ptr2 && ptr2 > ptr1) {
		int n = sscanf(ptr1 + 1, "%20lf", &ts->whence);
		if (n == 1) {
			ts->whence_valid = true;
			sprintf(ts->whence_text, "%12.6f  ", ts->whence);
		} else {
			ts->whence = -1.0;
			*ts->whence_text = '\0';
		}
	}

}

/*
 *  free_time_delta_info_list()
 *	free list
 */
static void free_time_delta_info_list(time_delta_info *list)
{
	while (list) {
		time_delta_info *next = list->next;
		if (list->reason)
			free(list->reason);
		free(list);
		list = next;
	}
}

static int str_cmp(const void *p1, const void *p2)
{
	return strcmp(* (char * const *) p1, * (char * const *) p2);
}

static char *str_sort_add(char *resume_cause, const char *cause)
{
	char *str, *token;
	char **ptrs;
	size_t n, i;

	/* Don't duplicate */
	if (resume_cause == NULL) {
		if ((str = strdup(cause)) == NULL)
			goto err;
		return str;
	}
	if (strstr(resume_cause, cause))
		return resume_cause;

	for (n = 2, str = resume_cause; *str; str++)
		if (*str == '+')
			n++;

	ptrs = alloca(sizeof(char *) * n);
	for (i = 0, str = resume_cause; (token = strtok(str, "+")) != NULL; str = NULL) {
		if ((ptrs[i++] = strdup(token)) == NULL)
			goto err;
	}
	if ((ptrs[i] = strdup(cause)) == NULL)
		goto err;

	qsort(ptrs, n, sizeof(char *), str_cmp);
	str = ptrs[0];
	for (i = 1; i < n; i++) {
		str = realloc(str, strlen(str) + strlen(ptrs[i]) + 3);
		if (!str)
			goto err;
		strcat(str, "+");
		strcat(str, ptrs[i]);
		free(ptrs[i]);
	}
	return str;
err:
	fprintf(stderr, "Out of memory allocating string\n");
	exit(EXIT_FAILURE);
}

/*
 *  suspend_blocker()
 *	parse a kernel log looking for suspend/resume and wakelocks
 */
static void suspend_blocker(
	FILE *fp,
	const char *filename,
	json_object *json_results,
	const int opt_freq_min)
{
	char buf[4096];
	char wakelock[4096];
	char *resume_cause = NULL;
	char *suspend_fail_cause = NULL;
	int state = STATE_UNDEFINED;
	timestamp suspend_start, suspend_exit;
	double last_exit;
	int suspend_succeeded = 0;
	int suspend_failed = 0;
	int suspend_count;
	double interval_mode, interval_median, suspend_mode, suspend_median;
	double interval_mean, interval_min, interval_max = 0.0, interval_sum, interval_percent;
	double suspend_mean, suspend_min, suspend_max, suspend_sum, suspend_percent;
	double total_percent;
	double percent_succeeded, percent_failed;
	double suspend_duration_parsed = -1.0;
	time_delta_info *suspend_list = NULL;
	time_delta_info *suspend_duration_list = NULL;
	bool needs_config_suspend_time = true;
	json_object *result = NULL, *obj;

	counter_info wakelocks_count[HASH_SIZE];
	counter_info resume_causes[HASH_SIZE];
	counter_info suspend_fail_causes[HASH_SIZE];
	counter_info wakeup_sources[HASH_SIZE];

	if (json_results) {
		if ((result = json_obj()) == NULL)
			goto out;
		json_object_array_add(json_results, result);

		if ((obj = json_str(filename)) == NULL)
			goto out;
		json_object_object_add(result, "kernel-log", obj);
	}

	memset(wakelocks_count, 0, sizeof(wakelocks_count));
	memset(resume_causes, 0, sizeof(resume_causes));
	memset(suspend_fail_causes, 0, sizeof(suspend_fail_causes));
	memset(wakeup_sources, 0, sizeof(wakeup_sources));

	last_exit = -1.0;

	if (opt_flags & OPT_VERBOSE)
		print("       When         Duration (Seconds)\n");

	timestamp_init(&suspend_start);
	timestamp_init(&suspend_exit);

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		char *ptr, *cause;
		size_t len = strlen(buf);

		if (buf[len - 1] == '\n')
			buf[len - 1] = '\0';

		ptr = strstr(buf, "PM: suspend entry");
		if (ptr) {
			state = STATE_ENTER_SUSPEND;
			parse_pm_timestamp(ptr + 18, &suspend_start);
			suspend_duration_parsed = -1.0;
			continue;
		}
		ptr = strstr(buf, "PM: suspend exit");
		if (ptr) {
			parse_pm_timestamp(ptr + 17, &suspend_exit);
		}

		if (strstr(buf, "suspend: enter suspend")) {
			state = STATE_ENTER_SUSPEND;
			parse_timestamp(buf, &suspend_start);
			suspend_duration_parsed = -1.0;
			continue;
		}
		if (strstr(buf, "PM: Entering mem sleep")) {
			state = STATE_ENTER_SUSPEND;
			parse_timestamp(buf, &suspend_start);
			suspend_duration_parsed = -1.0;
			continue;
		}
		if (strstr(buf, "PM: Preparing system for mem sleep")) {
			state = STATE_ENTER_SUSPEND;
			parse_timestamp(buf, &suspend_start);
			suspend_duration_parsed = -1.0;
			continue;
		}
		if (strstr(buf, "PM: Some devices failed to suspend")) {
			state |= STATE_SUSPEND_FAIL_CAUSE;
			/* Pick first failure cause up, ignore rest */
			if (!suspend_fail_cause) {
				suspend_fail_cause = strdup("device suspend failure");
				counter_increment(suspend_fail_cause, suspend_fail_causes);
			}
			continue;
		}

		ptr = strstr(buf, "active wakeup source: ");
		if (ptr) {
			ptr += 22;
			if (*ptr)
				counter_increment(ptr, wakeup_sources);
		}

		ptr = strstr(buf, "Resume caused by");
		if (ptr)
			cause = ptr + 17;
		else {
			ptr = strstr(buf, "[SPM] wake up by");
			if (ptr) {
				char *ws;

				cause = ptr + 17;
				ws = strchr(cause, ' ');
				if (ws)
					*ws = '\0';
				ws = strchr(cause, ',');
				if (ws)
					*ws = '\0';
			}
		}
		if (ptr) {
			state |= STATE_RESUME_CAUSE;
			resume_cause = str_sort_add(resume_cause, cause);
			if (opt_flags & OPT_RESUME_CAUSES)
				counter_increment(cause, resume_causes);
		}

		/* In this form, we have a pretty good idea what the suspend duration is */
		ptr = strstr(buf, "Suspended for");
		if (ptr) {
			suspend_duration_parsed = atof(ptr + 14);
			needs_config_suspend_time = false;
		}

		if (strstr(buf, "suspend: exit suspend") ||
                    strstr(buf, "PM: suspend exit")) {
			if (state & STATE_ENTER_SUSPEND) {
				state &= ~STATE_ENTER_SUSPEND;
				state |= STATE_EXIT_SUSPEND;
				parse_timestamp(buf, &suspend_exit);
			}
		}

		if (state & STATE_EXIT_SUSPEND) {
			double s_start = 0.0, s_exit = 0.0, s_duration = 0.0;
			bool s_duration_accurate = false;
			bool valid = false;

			/*  1st, check least inaccurate way of measuring suspend */
			if (suspend_start.whence_valid && suspend_exit.whence_valid) {
				s_start    = suspend_start.whence;
				s_exit     = suspend_exit.whence;
				s_duration = s_exit - s_start;
				valid = true;
			}
			/*  2nd, if we have suspend_duration_parsed, then use this */
			if (suspend_duration_parsed > 0.0) {
				s_duration = suspend_duration_parsed;
				suspend_duration_parsed = -1.0;
				s_duration_accurate = true;
				valid = true;
			}
			/*  3rd, most accurate estimate should always be considered */
			if (suspend_start.pm_whence_valid && suspend_exit.pm_whence_valid) {
				s_start = suspend_start.pm_whence;
				s_exit  = suspend_exit.pm_whence;
				s_duration = s_exit - s_start;
				s_duration_accurate = true;
				valid = true;
			}

			if (opt_flags & OPT_VERBOSE)
				print("%-15s %11.5f ",
					*suspend_start.whence_text ? suspend_start.whence_text : "<unknown>",
					s_duration);

			timestamp_init(&suspend_start);
			timestamp_init(&suspend_exit);

			if (state & STATE_SUSPEND_SUCCESS) {
				time_delta_info *new_info;

				if (opt_flags & OPT_VERBOSE) {
					print("Successful suspend");
					if (resume_cause && (state & STATE_RESUME_CAUSE)) {
						print(", resume cause: %s", resume_cause);
					}
				}

				if (valid && last_exit > 0.0) {
					char buffer[1024];
					double delta = s_start - last_exit;

					if (delta > 0.0) {
						new_info = malloc(sizeof(time_delta_info));
						if (!new_info) {
							fprintf(stderr, "Out of memory!\n");
							exit(EXIT_FAILURE);
						}
						new_info->type = SUSPEND_SUCCESS;
						snprintf(buffer, sizeof(buffer), "R:%s", resume_cause);
						new_info->reason = strdup(buffer);
						new_info->start = s_start;
						new_info->delta = s_start - last_exit;
						new_info->accurate = true;
						new_info->next = suspend_list;
						suspend_list = new_info;
						if (interval_max < new_info->delta)
							interval_max = new_info->delta;
					}
				}
				if (s_duration > 0.0) {
					char buffer[1024];

					new_info = malloc(sizeof(time_delta_info));
					if (!new_info) {
						fprintf(stderr, "Out of memory!\n");
						exit(EXIT_FAILURE);
					}
					snprintf(buffer, sizeof(buffer), "R:%s", resume_cause);
					new_info->reason = strdup(buffer);
					new_info->type = SUSPEND_DURATION;
					new_info->start = s_start;
					new_info->delta = s_duration;
					new_info->accurate = s_duration_accurate;
					new_info->next = suspend_duration_list;
					suspend_duration_list = new_info;
				}
				free(resume_cause);
				resume_cause = NULL;
				suspend_succeeded++;

				last_exit = s_exit;
			} else {
				time_delta_info *new_info;

				new_info = malloc(sizeof(time_delta_info));
				if (!new_info) {
					fprintf(stderr, "Out of memory!\n");
					exit(EXIT_FAILURE);
				}
				new_info->type = SUSPEND_FAIL;
				new_info->reason = NULL;
				new_info->start = s_start;
				new_info->delta = 0;
				new_info->accurate = false;
				new_info->next = suspend_list;
				suspend_list = new_info;

				suspend_failed++;
				if (resume_cause && (state & STATE_RESUME_CAUSE)) {
					char buffer[1024];

					if (opt_flags & OPT_VERBOSE)
						print("Suspend aborted, resume cause: %s\n", resume_cause);

					snprintf(buffer, sizeof(buffer), "A:%s", resume_cause);
					new_info->reason = strdup(buffer);
					free(resume_cause);
					resume_cause = NULL;
					state = STATE_UNDEFINED;
					continue;
				}
				if (suspend_fail_cause && (state & STATE_SUSPEND_FAIL_CAUSE)) {
					char buffer[1024];

					if (opt_flags & OPT_VERBOSE)
						print("Suspend aborted, %s\n", suspend_fail_cause);
					snprintf(buffer, sizeof(buffer), "A:%s", suspend_fail_cause);

					new_info->reason = strdup(buffer);
					free(suspend_fail_cause);
					suspend_fail_cause = NULL;
					state = STATE_UNDEFINED;
				}
				if (state & STATE_ACTIVE_WAKELOCK) {
					char buffer[1024];

					if (opt_flags & OPT_VERBOSE)
						print("Failed on wakelock %s, ", wakelock);

					snprintf(buffer, sizeof(buffer), "F:%s", wakelock);
					new_info->reason = strdup(buffer);
				}
				if (state & STATE_FREEZE_ABORTED) {
					new_info->reason = strdup("freezer abort");
					if (opt_flags & OPT_VERBOSE) {
						if (state & STATE_FREEZE_TASKS_REFUSE)
							print("Suspend aborted in freezer, tasks refused to freeze");
						else
							print("Suspend aborted in freezer");
					}
					new_info->reason = strdup("A:freezer");
				}
				if (state & STATE_LATE_HAS_WAKELOCK) {
					if (opt_flags & OPT_VERBOSE)
						print("Wakelock during power_suspend_late");
					new_info->reason = strdup("A:wakelock");
				}

			}
			if (opt_flags & OPT_VERBOSE)
				print("\n");
			state = STATE_UNDEFINED;
			continue;
		}

		ptr = strstr(buf, "active wake lock");
		if (ptr && (state & STATE_ENTER_SUSPEND)) {
			if ((sscanf(ptr + 17, "%[^,^\n]", wakelock) == 1) &&
			    (opt_flags & OPT_WAKELOCK_BLOCKERS))
				counter_increment(wakelock, wakelocks_count);
			state |= STATE_ACTIVE_WAKELOCK;
			continue;
		}

		ptr = strstr(buf, "Disabling non-boot CPUs");
		if (ptr && (state & STATE_ENTER_SUSPEND)) {
			state |= STATE_SUSPEND_SUCCESS;
			continue;
		}

		if (strstr(buf, "Freezing of user space  aborted") ||
		    strstr(buf, "Freezing of user space aborted")) {
			state |= STATE_FREEZE_ABORTED;
			counter_increment("user space freezer abort", suspend_fail_causes);
			continue;
		}

		if (strstr(buf, "Freezing of tasks  aborted") ||
                    strstr(buf, "Freezing of tasks aborted")) {
			state |= STATE_FREEZE_ABORTED;
			counter_increment("tasks freezer abort", suspend_fail_causes);
			if (strstr(buf, "tasks refusing to freeze"))
				state |= STATE_FREEZE_TASKS_REFUSE;
			continue;
		}

		if (strstr(buf, "power_suspend_late return -11")) {
			/* See power_suspend_late, has_wake_lock() true, so return -EAGAIN */
			counter_increment("late suspend wakelock", suspend_fail_causes);
			state |= STATE_LATE_HAS_WAKELOCK;
			continue;
		}
	}

	if (opt_flags & OPT_VERBOSE)
		putchar('\n');

	suspend_count = suspend_failed + suspend_succeeded;

	if (opt_flags & OPT_WAKELOCK_BLOCKERS) {
		print("Suspend blocking wakelocks:\n");
		counter_dump(wakelocks_count, "suspend-blocking-wakelocks", result);
	}

	if (opt_flags & OPT_RESUME_CAUSES) {
		print("Resume wakeup causes:\n");
		counter_dump(resume_causes, "resume-wakeups", result);
		print("Suspend failure causes:\n");
		counter_dump(suspend_fail_causes, "suspend-failures", result);
		printf("Active wakeup sources:\n");
		counter_dump(wakeup_sources, "wakeup-sources", result);
	}

	time_calc_stats(suspend_list, &interval_mode, &interval_median,
		&interval_mean, &interval_min, &interval_max, &interval_sum);
	time_calc_stats(suspend_duration_list, &suspend_mode, &suspend_median,
		&suspend_mean, &suspend_min, &suspend_max, &suspend_sum);

	if (opt_flags & OPT_HISTOGRAM) {
		histogram_dump(suspend_list, "Time between successful suspends:");
		histogram_dump(suspend_duration_list, "Duration of successful suspends:");
	}

	print("Suspends:\n");
	percent_failed = (suspend_count == 0) ?
		0.0 : 100.0 * (double)suspend_failed / suspend_count;
	percent_succeeded = (suspend_count == 0) ?
		0.0 : 100.0 * (double)suspend_succeeded / suspend_count;
	total_percent = interval_sum + suspend_sum;
	suspend_percent = FLOAT_CMP(total_percent, 0.0) ?
		0.0 : 100.0 * suspend_sum / total_percent;
	interval_percent = FLOAT_CMP(total_percent, 0.0) ?
		0.0 : 100.0 * interval_sum / total_percent;

	print("  %d suspends aborted (%.2f%%).\n", suspend_failed, percent_failed);
	print("  %d suspends succeeded (%.2f%%).\n", suspend_succeeded, percent_succeeded);
	print("  total time: %f seconds (%.2f%%).\n", suspend_sum, suspend_percent);
	print("  minimum: %f seconds.\n", suspend_min);
	print("  maximum: %f seconds.\n", suspend_max);
	print("  mean: %f seconds.\n", suspend_mean);
	print("  mode: %f seconds.\n", suspend_mode);
	print("  median: %f seconds.\n", suspend_median);

	print("\nTime between successful suspends:\n");
	print("  total time: %f seconds (%.2f%%).\n", interval_sum, interval_percent);
	print("  minimum: %f seconds.\n", interval_min);
	print("  maximum: %f seconds.\n", interval_max);
	print("  mean: %f seconds.\n", interval_mean);
	print("  mode: %f seconds.\n", interval_mode);
	print("  median: %f seconds.\n", interval_median);

	if ((suspend_count > 0) && needs_config_suspend_time) {
		print("\nNOTE: suspend times are very dubious, enable kernel config setting\n");
		print("      CONFIG_SUSPEND_TIME=y for accurate suspend times.\n");
	}

	if (opt_flags & OPT_FREQUENCY_REPORT) {
		frequency_dump(suspend_list, opt_freq_min);
	}

	if (json_results) {
		/* suspend stats */
		if ((obj = json_int(suspend_count)) == NULL)
			goto out;
		json_object_object_add(result, "suspends-attempted", obj);
		if ((obj = json_int(suspend_failed)) == NULL)
			goto out;
		json_object_object_add(result, "suspends-aborted", obj);
		if ((obj = json_int(suspend_succeeded)) == NULL)
			goto out;
		json_object_object_add(result, "suspends-succeeded", obj);

		if ((obj = json_double(percent_failed)) == NULL)
			goto out;
		json_object_object_add(result, "suspends-aborted-percent", obj);
		if ((obj = json_double(percent_succeeded)) == NULL)
			goto out;
		json_object_object_add(result, "suspends-succeeded-percent", obj);

		if ((obj = json_double(suspend_sum)) == NULL)
			goto out;
		json_object_object_add(result, "suspends-total-time-seconds", obj);
		if ((obj = json_double(suspend_percent)) == NULL)
			goto out;
		json_object_object_add(result, "suspends-total-time-percent", obj);

		if ((obj = json_double(suspend_min)) == NULL)
			goto out;
		json_object_object_add(result, "suspend-minimum-duration-seconds", obj);
		if ((obj = json_double(suspend_max)) == NULL)
			goto out;
		json_object_object_add(result, "suspend-maximum-duration-seconds", obj);
		if ((obj = json_double(suspend_mean)) == NULL)
			goto out;
		json_object_object_add(result, "suspend-mean-duration-seconds", obj);
		if ((obj = json_double(suspend_mode)) == NULL)
			goto out;
		json_object_object_add(result, "suspend-mode-duration-seconds", obj);
		if ((obj = json_double(suspend_median)) == NULL)
			goto out;
		json_object_object_add(result, "suspend-median-duration-seconds", obj);

		/* Awake (between suspend) stats */
		if ((obj = json_double(interval_sum)) == NULL)
			goto out;
		json_object_object_add(result, "awake-total-time-seconds", obj);
		if ((obj = json_double(interval_percent)) == NULL)
			goto out;
		json_object_object_add(result, "awake-total-time-percent", obj);

		if ((obj = json_double(interval_min)) == NULL)
			goto out;
		json_object_object_add(result, "awake-minimum-duration-seconds", obj);
		if ((obj = json_double(interval_max)) == NULL)
			goto out;
		json_object_object_add(result, "awake-maximum-duration-seconds", obj);
		if ((obj = json_double(interval_mean)) == NULL)
			goto out;
		json_object_object_add(result, "awake-mean-duration-seconds", obj);
		if ((obj = json_double(interval_mode)) == NULL)
			goto out;
		json_object_object_add(result, "awake-mode-duration-seconds", obj);
		if ((obj = json_double(interval_median)) == NULL)
			goto out;
		json_object_object_add(result, "awake-median-duration-seconds", obj);
	}

out:
	free(resume_cause);
	free(suspend_fail_cause);
	free_time_delta_info_list(suspend_list);
	free_time_delta_info_list(suspend_duration_list);
	counter_free(wakelocks_count);
	counter_free(resume_causes);
	counter_free(suspend_fail_causes);
	counter_free(wakeup_sources);
}


/*
 *  json_write()
 *	dump out collected JSON data
 */
static int json_write(json_object *obj, const char *filename)
{
	const char *str;
	FILE *fp;

	if (obj == NULL) {
		fprintf(stderr, "Cannot create JSON log, no JSON data.\n");
		return -1;
	}

#ifdef JSON_C_TO_STRING_PRETTY
	str = json_object_to_json_string_ext(
		obj, JSON_C_TO_STRING_PRETTY);
#else
	str = json_object_to_json_string(obj);
#endif
	if (str == NULL) {
		fprintf(stderr, "Cannot turn JSON object to text for JSON output.\n");
		return -1;
	}
	if ((fp = fopen(filename, "w")) == NULL) {
		fprintf(stderr, "Cannot create JSON log file %s.\n", filename);
		return -1;
	}

	fprintf(fp, "%s", str);
	(void)fclose(fp);
	json_object_put(obj);

	return 0;
}

static void show_help(char * const argv[])
{
	printf("%s, version %s\n\n", APP_NAME, VERSION);
	printf("usage: %s [options] [kernel_log]\n", argv[0]);
	printf("\t-b       list blocking wakelock names and count.\n");
	printf("\t-d       bucket histogram into 10s of seconds rather than powers of 2.\n");
	printf("\t-f mins  dump suspend frequency stats for importing into spreadsheet.\n");
	printf("\t-h       this help.\n");
	printf("\t-H       histogram of times between suspend and suspend duration.\n");
	printf("\t-o       output results in json format to an named files.\n");
	printf("\t-r       list causes of resume.\n");
	printf("\t-v       verbose information.\n");
	printf("\t-w       profile wakelocks.\n");
}

static void handle_sig(int dummy)
{
	(void)dummy;
	keep_running = false;
}

int main(int argc, char **argv)
{
	char *opt_json_file = NULL;
	json_object *json_results = NULL;
	int opt_freq_min = 60;

	for (;;) {
		int c = getopt(argc, argv, "bhHrvo:qw:df:");
		if (c == -1)
			break;
		switch (c) {
		case 'b':
			opt_flags |= OPT_WAKELOCK_BLOCKERS;
			break;
		case 'd':
			opt_flags |= OPT_HISTOGRAM_DECADES;
			break;
		case 'f':
			opt_flags |= OPT_FREQUENCY_REPORT;
			opt_freq_min = atoi(optarg);	/* Minutes */
			if (opt_freq_min < 1) {
				fprintf(stderr, "-f option must be 1 or more minutes\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'r':
			opt_flags |= OPT_RESUME_CAUSES;
			break;
		case 'v':
			opt_flags |= OPT_VERBOSE;
			break;
		case 'H':
			opt_flags |= OPT_HISTOGRAM;
			break;
		case 'h':
			show_help(argv);
			exit(EXIT_SUCCESS);
		case 'o':
			opt_json_file = optarg;
			break;
		case 'q':
			opt_flags |= OPT_QUIET;
			opt_flags &= ~OPT_VERBOSE;
			break;
		case 'w':
			opt_flags |= OPT_PROC_WAKELOCK;
			opt_wakelock_duration = atof(optarg);
			break;
		}
	}

	if (opt_json_file) {
		if ((json_results = json_obj()) == NULL)
			exit(EXIT_FAILURE);
	}

	if (opt_flags & OPT_PROC_WAKELOCK) {
		struct sigaction new_action;
		struct timeval tv, tv_start, tv_now;
		int i;
		double duration;

		memset(&new_action, 0, sizeof(new_action));
		for (i = 0; signals[i] != -1; i++) {
			new_action.sa_handler = handle_sig;
			sigemptyset(&new_action.sa_mask);
			new_action.sa_flags = 0;

			if (sigaction(signals[i], &new_action, NULL) < 0) {
				fprintf(stderr, "sigaction failed: errno=%d (%s)\n",
					errno, strerror(errno));
				exit(EXIT_FAILURE);
			}
		}

		if (gettimeofday(&tv_start, NULL) < 0) {
			fprintf(stderr, "gettimeofday failed: errno=%d (%s)\n",
				errno, strerror(errno));
			exit(EXIT_FAILURE);
		}

		do {
			int ret;

			wakelock_read(WAKELOCK_START);
			tv.tv_sec = (long)opt_wakelock_duration;
			tv.tv_usec = (long)((opt_wakelock_duration - tv.tv_sec) * 1000000.0);
			ret = select(0, NULL, NULL, NULL, &tv);
			if (ret < 0) {
				if (errno == EINTR) {
					fprintf(stderr, "Interrupted by a signal\n");
					if (gettimeofday(&tv_now, NULL) < 0) {
						fprintf(stderr, "gettimeofday failed: errno=%d (%s)\n",
							errno, strerror(errno));
						exit(EXIT_FAILURE);
					}
					duration = timeval_to_double(&tv_now) - timeval_to_double(&tv_start);
					break;
				}
				if (errno) {
					fprintf(stderr, "Select failed: %s\n", strerror(errno));
					exit(EXIT_FAILURE);
				}
			}
			if (gettimeofday(&tv_now, NULL) < 0) {
				fprintf(stderr, "gettimeofday failed: errno=%d (%s)\n",
					errno, strerror(errno));
				exit(EXIT_FAILURE);
			}
			duration = timeval_to_double(&tv_now) - timeval_to_double(&tv_start);
		} while (keep_running && (duration < opt_wakelock_duration));

		wakelock_read(WAKELOCK_END);
		wakelock_check(opt_wakelock_duration, duration, json_results);
		wakelock_free();
	} else {
		json_object *obj = NULL;


		if (json_results) {
			if ((obj = json_array()) == NULL)
				exit(EXIT_FAILURE);
			json_object_object_add(json_results, "wakelock-stats-from-klog", obj);
		}

		if (optind == argc) {
			print("stdin:\n");
			suspend_blocker(stdin, "stdin", obj, opt_freq_min);
		}

		while (optind < argc) {
			FILE *fp;

			print("%s:\n", argv[optind]);
			if ((fp = fopen(argv[optind], "r")) == NULL) {
				fprintf(stderr, "Cannot open %s.\n", argv[optind]);
				exit(EXIT_FAILURE);
			}
			suspend_blocker(fp, argv[optind], obj, opt_freq_min);
			(void)fclose(fp);
			optind++;
		}
	}

	if (opt_json_file)
		json_write(json_results, opt_json_file);

	exit(EXIT_SUCCESS);
}
