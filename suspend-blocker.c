#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

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

#define HASH_SIZE			1997
#define MAX_INTERVALS			14

typedef struct { 
	double	whence;
} timestamp;

typedef struct {
	char *name;
	int  count;
} counter_info;

typedef struct time_delta_info {
	double delta;
	struct time_delta_info *next;
} time_delta_info;

static int opt_flags;

static counter_info wakelocks[HASH_SIZE];
static counter_info resume_causes[HASH_SIZE];

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

	qsort(counter, HASH_SIZE, sizeof(counter_info), counter_info_cmp);
	for (i = 0; i < HASH_SIZE; i++) {
		if (counter[i].name) {
			printf("  %-28.28s %8d\n", counter[i].name, counter[i].count);
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

	memset(histogram, 0, sizeof(histogram));

	for (; info; info = info->next) {
		double d = info->delta;
		
		for (i = 0; i < MAX_INTERVALS && d > 0.125; i++)
			d = d / 2.0;

		histogram[i]++;
		if (i > max)
			max = i;
		if (i < min)
			min = i;
	}

	printf("%s\n", message);
	if (max == -1) {
		printf("  No values.\n");
	} else {
		for (range1 = 0.0, range2 = 0.125, i = 0; i < MAX_INTERVALS; i++) {
			if (i >= min && i <= max) {
				if (i == MAX_INTERVALS - 1)
					printf("  %8.3f -           %d\n", range1, histogram[i]);
				else
					printf("  %8.3f - %8.3f  %d\n", range1, range2 - 0.001, histogram[i]);
			}
		
			range1 = range2;
			range2 = range2 + range2;
		}
	}
	printf("\n");
}

static void parse_timestamp(const char *line, timestamp *ts)
{
	char *ptr1, *ptr2;

	ptr1 = strstr(line, "[");
	ptr2 = strstr(line, "]");

	if (ptr1 && ptr2 && ptr2 > ptr1) {
		sscanf(ptr1 + 1, "%lf", &ts->whence);
	} else {
		ts->whence = 0.0;
	}
}

static void suspend_blocker(FILE *fp)
{
	char buf[4096];
	char wakelock[4096];
	char *resume_cause = NULL;
	int state = STATE_UNDEFINED;
	timestamp suspend_start, suspend_exit;
	timestamp last_suspend;
	int suspend_succeeded = 0;
	int suspend_failed = 0;
	int suspend_count;
	double suspend_total = 0.0;
	double suspend_duration;
	double suspend_min;
	double suspend_max;
	double interval_max = 0.0;
	time_delta_info *suspend_interval_list = NULL;
	time_delta_info *suspend_duration_list = NULL;

	last_suspend.whence = -1.0;

	if (opt_flags & OPT_VERBOSE) {
		printf("  When        Duration\n");
	}

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		char *ptr;
		size_t len = strlen(buf);

		if (buf[len - 1] == '\n')
			buf[len - 1] = '\0';

		if (strstr(buf, "suspend: enter suspend")) {
			state = STATE_ENTER_SUSPEND;
			parse_timestamp(buf, &suspend_start);
			continue;
		}
		if (strstr(buf, "PM: Entering mem sleep")) {
			state = STATE_ENTER_SUSPEND;
			parse_timestamp(buf, &suspend_start);
			continue;
		}
		if (strstr(buf, "PM: Preparing system for mem sleep")) {
			state = STATE_ENTER_SUSPEND;
			parse_timestamp(buf, &suspend_start);
			continue;
		}
		/* Nexus 7 */
		ptr = strstr(buf, "Resume caused by");
		if (ptr) {
			state |= STATE_RESUME_CAUSE;
			if (state & STATE_RESUME_CAUSE) {
				char *cause = ptr + 17;
				size_t len = strlen(cause);
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
		}

		ptr = strstr(buf, "suspend: exit suspend");
		if (ptr) {
			state &= ~STATE_ENTER_SUSPEND;
			state |= STATE_EXIT_SUSPEND;
			parse_timestamp(buf, &suspend_exit);
			suspend_duration = suspend_exit.whence - suspend_start.whence;
		}

		if (state & STATE_EXIT_SUSPEND) {
			if (opt_flags & OPT_VERBOSE) {
				printf("%12.6f: %f ", suspend_start.whence, suspend_duration);
			}
			if (state & STATE_SUSPEND_SUCCESS) {
				time_delta_info *new_sd;
				time_delta_info *new_ri;

				if (opt_flags & OPT_VERBOSE) {
					printf("Successful Suspend. ");
					if (resume_cause && (state & STATE_RESUME_CAUSE)) {
						printf("Resume cause: %s.", resume_cause);
						free(resume_cause);
						resume_cause = NULL;
					}
				}
				if (suspend_succeeded == 0)
					suspend_max = suspend_min = suspend_duration;
				else {
					if (suspend_max < suspend_duration)
						suspend_max = suspend_duration;
					if (suspend_min > suspend_duration)
						suspend_min = suspend_duration;
				}
				suspend_succeeded++;
				suspend_total += suspend_duration;

				if (last_suspend.whence > 0.0) {
					new_ri = malloc(sizeof(time_delta_info));
					if (new_ri) {
						new_ri->delta = suspend_start.whence - last_suspend.whence;
						new_ri->next = suspend_interval_list;
						suspend_interval_list = new_ri;
						if (interval_max < new_ri->delta)
							interval_max = new_ri->delta;
					}
				}

				new_sd = malloc(sizeof(time_delta_info));
				if (new_sd) {
					new_sd->delta = suspend_duration;
					new_sd->next = suspend_duration_list;
					suspend_duration_list = new_sd;
				}

				last_suspend = suspend_exit;

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
