#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <float.h>

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

#define OPT_WAKELOCK_BLOCKERS		0x00000001
#define OPT_VERBOSE			0x00000002
#define OPT_HISTOGRAM			0x00000004
#define OPT_RESUME_CAUSES		0x00000008

#define HASH_SIZE			(1997)
#define MAX_INTERVALS			(14)

typedef struct {
	double	whence;
	bool	whence_valid;
	double	pm_whence;
	bool	pm_whence_valid;
	char	whence_text[32];
} timestamp;

typedef struct {
	char *name;
	int  count;
} counter_info;

typedef struct time_delta_info {
	double delta;
	bool   accurate;
	struct time_delta_info *next;
} time_delta_info;

static int opt_flags;

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
	counter_info *w1 = (counter_info *)p1;
	counter_info *w2 = (counter_info *)p2;
	int diff = w2->count - w1->count;

	if ((diff == 0) && (w1->count | w2->count))
		return strcmp(w1->name, w1->name);
	else
		return diff;
}

/*
 *  hash_pjw()
 *	Hash a string, from Aho, Sethi, Ullman, Compiling Techniques.
 */
static unsigned long hash_pjw(const char *str)
{
  	unsigned long h = 0, g;

	while (*str) {
		h = (h << 4) + (*str);
		if (0 != (g = h & 0xf0000000)) {
			h = h ^ (g >> 24);
			h = h ^ g;
		}
		str++;
	}

  	return h % HASH_SIZE;
}

static void counter_increment(const char *name, counter_info counter[])
{
	unsigned long i = hash_pjw(name);
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

static void counter_dump(counter_info counter[])
{
	int i;
	int total = 0;

	for (i = 0; i < HASH_SIZE; i++) {
		if (counter[i].name)
			total += counter[i].count;
	}

	qsort(counter, HASH_SIZE, sizeof(counter_info), counter_info_cmp);
	for (i = 0; i < HASH_SIZE; i++) {
		if (counter[i].name) {
			printf("  %-28.28s %8d  %5.2f%%\n", counter[i].name, counter[i].count,
				100.0 * (double)counter[i].count / (double)total);
			free(counter[i].name);
		}
	}
	printf("\n");
}

static void histogram_dump(time_delta_info *info, const char *message)
{
	int histogram[MAX_INTERVALS];
	int i;
	double range1, range2;
	int max = -1;
	int min = MAX_INTERVALS;
	time_delta_info *tdi = info, *next;
	int total = 0;
	int accurate = 0;

	memset(histogram, 0, sizeof(histogram));

	for (tdi = info; tdi; tdi = tdi->next) {
		total++;
		if (tdi->accurate)
			accurate++;
	}

	for (tdi = info; tdi; ) {
		double d = tdi->delta;
		next = tdi->next;

		for (i = 0; i < MAX_INTERVALS && d > 0.125; i++)
			d = d / 2.0;

		histogram[i]++;
		if (i > max)
			max = i;
		if (i < min)
			min = i;

		free(tdi);
		tdi = next;
	}

	printf("%s\n", message);
	if (max == -1) {
		printf("  No values.\n");
	} else {
		for (range1 = 0.0, range2 = 0.125, i = 0; i < MAX_INTERVALS; i++) {
			if (i >= min && i <= max) {
				double pc = 100.0 * (double) histogram[i] / (double)total;
				if (i == MAX_INTERVALS - 1)
					printf("  %8.3f -          seconds    %6d  %5.2f%%\n", range1, histogram[i], pc);
				else
					printf("  %8.3f - %8.3f seconds    %6d  %5.2f%%\n", range1, range2 - 0.001, histogram[i], pc);
			}
			range1 = range2;
			range2 = range2 + range2;
		}
		if (accurate != total) {
			printf("NOTE: %5.2f%% of the samples were inaccurate estimates.\n",
				100.0 * (double)(total-accurate) / (double)total);
		}
	}
	printf("\n");
}

/*
 *  Parse PM time stamps of the form:
 *  	PM: suspend entry 2013-06-20 14:16:08.865677626 UTC
 */
static void parse_pm_timestamp(const char *ptr, timestamp *ts)
{
	struct tm tm;
	double sec;

	memset(&tm, 0, sizeof(tm));

	sscanf(ptr, "%d-%d-%d %d:%d:%lf",
		&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		&tm.tm_hour, &tm.tm_min, &sec);

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

	if (ptr1 && ptr2 && ptr2 > ptr1) {
		sscanf(ptr1 + 1, "%lf", &ts->whence);
		ts->whence_valid = true;
	} else {
		ts->whence = 0.0;
		ts->whence_valid = false;
	}

	sprintf(ts->whence_text, "%12.6f  ", ts->whence);
}

static void suspend_blocker(FILE *fp)
{
	char buf[4096];
	char wakelock[4096];
	char *resume_cause = NULL;
	int state = STATE_UNDEFINED;
	timestamp suspend_start, suspend_exit;
	double last_suspend;
	int suspend_succeeded = 0;
	int suspend_failed = 0;
	int suspend_count;
	double suspend_total = 0.0;
	double suspend_duration_parsed = -1.0;
	double suspend_min = DBL_MAX;
	double suspend_max = DBL_MIN;
	double interval_max = 0.0;
	time_delta_info *suspend_interval_list = NULL;
	time_delta_info *suspend_duration_list = NULL;
	bool needs_config_suspend_time = true;

	counter_info wakelocks[HASH_SIZE];
	counter_info resume_causes[HASH_SIZE];

	memset(wakelocks, 0, sizeof(wakelocks));
	memset(resume_causes, 0, sizeof(resume_causes));

	last_suspend = -1.0;

	if (opt_flags & OPT_VERBOSE)
		printf("       When         Duration\n");

	timestamp_init(&suspend_start);
	timestamp_init(&suspend_exit);

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		char *ptr;
		size_t len = strlen(buf);

		if (buf[len - 1] == '\n')
			buf[len - 1] = '\0';

		ptr = strstr(buf, "PM: suspend entry");
		if (ptr) {
			parse_pm_timestamp(ptr + 18, &suspend_start);
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

		ptr = strstr(buf, "Resume caused by");
		if (ptr) {
			char *cause = ptr + 17;
			size_t len = strlen(cause);

			state |= STATE_RESUME_CAUSE;
			if (resume_cause) {
				resume_cause = realloc(resume_cause, strlen(resume_cause) + 3 + len);
				if (resume_cause)
					strcat(resume_cause, "; ");
					strcat(resume_cause, cause);
			} else {
				resume_cause = malloc(len + 1);
				strcpy(resume_cause, cause);
			}
			if (opt_flags & OPT_RESUME_CAUSES)
				counter_increment(cause, resume_causes);
		}

		/* In this form, we have a pretty good idea what the suspend duration is */
		ptr = strstr(buf, "Suspended for");
		if (ptr) {
			suspend_duration_parsed = atof(ptr + 14);
			needs_config_suspend_time = false;
		}

		ptr = strstr(buf, "suspend: exit suspend");
		if (ptr) {
			state &= ~STATE_ENTER_SUSPEND;
			state |= STATE_EXIT_SUSPEND;
			parse_timestamp(buf, &suspend_exit);
		}

		if (state & STATE_EXIT_SUSPEND) {
			double s_start = 0.0, s_exit = 0.0, s_duration = 0.0;
			bool s_duration_accurate = false;

			/*  1st, check least inaccurate way of measuring suspend */
			if (suspend_start.whence_valid && suspend_exit.whence_valid) {
				s_start    = suspend_start.whence;
				s_exit     = suspend_exit.whence;
				s_duration = s_exit - s_start;
			}
			/*  2nd, if we have suspend_duration_parsed, then use this */
			if (suspend_duration_parsed > 0.0) {
				s_duration = suspend_duration_parsed;
				suspend_duration_parsed = -1.0;
				s_duration_accurate = true;
			}
			/*  3rd, most accurate estimate should always be considered */
			if (suspend_start.pm_whence_valid && suspend_exit.pm_whence_valid) {
				s_start = suspend_start.pm_whence;
				s_exit  = suspend_exit.pm_whence;
				s_duration = s_exit - s_start;
				s_duration_accurate = true;
			}

			timestamp_init(&suspend_start);
			timestamp_init(&suspend_exit);

			if (opt_flags & OPT_VERBOSE)
				printf("  %s %11.5f ", suspend_start.whence_text, s_duration);

			if (state & STATE_SUSPEND_SUCCESS) {
				time_delta_info *new_info;

				if (opt_flags & OPT_VERBOSE) {
					printf("Successful Suspend. ");
					if (resume_cause && (state & STATE_RESUME_CAUSE)) {
						printf("Resume cause: %s.", resume_cause);
						free(resume_cause);
						resume_cause = NULL;
					}
				}

				if (suspend_max < s_duration)
					suspend_max = s_duration;
				if (suspend_min > s_duration)
					suspend_min = s_duration;

				suspend_succeeded++;
				suspend_total += s_duration;

				if (opt_flags & OPT_HISTOGRAM) {
					if (last_suspend > 0.0) {
						new_info = malloc(sizeof(time_delta_info));
						if (new_info == NULL) {
							fprintf(stderr, "Out of memory!\n");
							exit(EXIT_FAILURE);
						}
						new_info->delta = s_start - last_suspend;
						new_info->accurate = true;
						new_info->next = suspend_interval_list;
						suspend_interval_list = new_info;
						if (interval_max < new_info->delta)
							interval_max = new_info->delta;
					}

					new_info = malloc(sizeof(time_delta_info));
					if (new_info == NULL) {
						fprintf(stderr, "Out of memory!\n");
						exit(EXIT_FAILURE);
					}
					new_info->delta = s_duration;
					new_info->accurate = s_duration_accurate;
					new_info->next = suspend_duration_list;
					suspend_duration_list = new_info;
				}

				last_suspend = s_start;
			} else
				suspend_failed++;

			if (opt_flags & OPT_VERBOSE) {
				if (state & STATE_ACTIVE_WAKELOCK)
					printf("Failed on wakelock %s ", wakelock);
				if (state & STATE_FREEZE_ABORTED)
					printf("(Aborted in Freezer).");
				if (state & STATE_LATE_HAS_WAKELOCK)
					printf("(Wakelock during power_suspend_late)");

				printf("\n");
			}
			state = STATE_UNDEFINED;
			continue;
		}

		ptr = strstr(buf, "active wake lock");
		if (ptr && (state & STATE_ENTER_SUSPEND)) {
			sscanf(ptr + 17, "%[^,^\n]", wakelock);
			if (opt_flags & OPT_WAKELOCK_BLOCKERS)
				counter_increment(wakelock, wakelocks);
			state |= STATE_ACTIVE_WAKELOCK;
			continue;
		}

		ptr = strstr(buf, "Disabling non-boot CPUs");
		if (ptr && (state & STATE_ENTER_SUSPEND)) {
			state |= STATE_SUSPEND_SUCCESS;
			continue;
		}

		if (strstr(buf, "Freezing of user space  aborted")) {
			state |= STATE_FREEZE_ABORTED;
			continue;
		}

		if (strstr(buf, "Freezing of tasks  aborted")) {
			state |= STATE_FREEZE_ABORTED;
			continue;
		}

		if (strstr(buf, "power_suspend_late return -11")) {
			/* See power_suspend_late, has_wake_lock() true, so return -EAGAIN */
			state |= STATE_LATE_HAS_WAKELOCK;
			continue;
		}
	}

	if (opt_flags & OPT_VERBOSE)
		putchar('\n');

	suspend_count = suspend_failed + suspend_succeeded;

	if (opt_flags & OPT_WAKELOCK_BLOCKERS) {
		printf("Suspend blocking wakelocks:\n");
		counter_dump(wakelocks);
	}

	if (opt_flags & OPT_RESUME_CAUSES) {
		printf("Resume wakeup causes:\n");
		counter_dump(resume_causes);
	}

	if (opt_flags & OPT_HISTOGRAM) {
		histogram_dump(suspend_interval_list, "Period of time between each successful suspend:");
		histogram_dump(suspend_duration_list, "Duration of successful suspends:");
	}

	printf("Stats:\n");

	printf("  %d suspends aborted (%.2f%%).\n",
		suspend_failed,
		suspend_count == 0 ? 0.0 : 100.0 * (double)suspend_failed / (double)suspend_count);
	printf("  %d suspends succeeded (%.2f%%).\n",
		suspend_succeeded,
		suspend_count == 0 ? 0.0 : 100.0 * (double)suspend_succeeded / (double)suspend_count);
	printf("  %f seconds average suspend duration (min %f, max %f).\n",
		suspend_succeeded == 0 ? 0.0 : suspend_total / (double)suspend_succeeded,
		suspend_min, suspend_max);

	if ((suspend_count > 0) && needs_config_suspend_time) {
		printf("\nNOTE: suspend times are very dubious, enable kernel config setting\n");
		printf("      CONFIG_SUSPEND_TIME=y for accurate suspend times.\n");
	}

	free(resume_cause);
}

void show_help(char * const argv[])
{
	printf("%s, version %s\n\n", APP_NAME, VERSION);
	printf("usage: %s [options] [kernel_log]\n", argv[0]);
	printf("\t-b list blocking wakelock names and count.\n");
	printf("\t-h this help.\n");
	printf("\t-H histogram of times between suspend and suspend duration.\n");
	printf("\t-r list causes of resume.\n");
	printf("\t-v verbose information.\n");
}


int main(int argc, char **argv)
{
	for (;;) {
		int c = getopt(argc, argv, "bhHrv");
		if (c == -1)
			break;
		switch (c) {
		case 'b':
			opt_flags |= OPT_WAKELOCK_BLOCKERS;
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
		}
	}

	if (optind == argc) {
		printf("stdin:\n");
		suspend_blocker(stdin);
	}
	while (optind < argc) {
		FILE *fp;

		printf("%s:\n", argv[optind]);
		if ((fp = fopen(argv[optind], "r")) == NULL) {
			fprintf(stderr, "Cannot open %s.\n", argv[optind]);
			exit(EXIT_FAILURE);
		}
		suspend_blocker(fp);
		fclose(fp);
		optind++;
	}
	exit(EXIT_SUCCESS);
}
