#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define APP_NAME			"suspend-blocker"

#define STATE_UNDEFINED			0x00000000
#define	STATE_ENTER_SUSPEND		0x00000001
#define STATE_SUSPEND_TIME		0x00000002
#define STATE_ACTIVE_WAKELOCK		0x00000004
#define STATE_SUSPEND_SUCCESS		0x00000008
#define STATE_FREEZE_ABORTED		0x00000010
#define STATE_LATE_HAS_WAKELOCK		0x00000020
#define STATE_DEEP_SUSPEND_START	0x00000040
#define STATE_DEEP_SUSPEND_END		0x00000080

#define OPT_WAKELOCK_BLOCKERS		0x00000001
#define OPT_VERBOSE			0x00000002

#define HASH_SIZE			1997

typedef struct { 
	double		seconds_since_1900;
	int		day;
	int		month;
	int		year;
	int		hour;
	int 		minute;
	double		second;
} timestamp;

typedef struct {
	char *name;
	int  count;
} wakelock_info;

static int opt_flags;

static wakelock_info wakelocks[HASH_SIZE];

static int wakelock_cmp(const void *p1, const void *p2)
{
	wakelock_info *w1 = (wakelock_info *)p1;
	wakelock_info *w2 = (wakelock_info *)p2;
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

static void wakelock_increment(const char *name)
{
	unsigned long i = hash_pjw(name);
	unsigned long j = 0;

	for (j = 0; j < HASH_SIZE; j++) {
		if (wakelocks[i].name == NULL) {
			wakelocks[i].name = strdup(name);
			if (wakelocks[i].name == NULL) {
				fprintf(stderr, "Out of memory!\n");
				exit(EXIT_FAILURE);
			}
			wakelocks[i].count++;
			return;
		}
		if (strcmp(wakelocks[i].name, name) == 0) {
			wakelocks[i].count++;
			return;
		}
		i = (i + 1) % HASH_SIZE;
	}

	fprintf(stderr, "Wakelock hash table full!\n");
	exit(EXIT_FAILURE);
}


static void parse_timestamp(const char *ptr, timestamp *ts)
{
	struct tm tm;

	sscanf(ptr, "%d-%d-%d %d:%d:%lf",
		&ts->year, &ts->month, &ts->day,
		&ts->hour, &ts->minute, &ts->second);

	tm.tm_year = ts->year - 1900;
	tm.tm_mon  = ts->month;
	tm.tm_mday = ts->day;
	tm.tm_hour = ts->hour;
	tm.tm_min  = ts->minute;
	tm.tm_sec  = (int)ts->second;

	ts->seconds_since_1900 = mktime(&tm) + (ts->second - tm.tm_sec);
}

static void suspend_blocker(FILE *fp)
{
	char buf[4096];
	char wakelock[4096];
	int state = STATE_UNDEFINED;
	timestamp suspend_start, suspend_exit;
	int suspend_succeeded = 0;
	int suspend_failed = 0;
	int suspend_count;
	double suspend_total = 0.0;
	int i;

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		char *ptr;

		if (strstr(buf, "suspend: enter suspend")) {
			state = STATE_ENTER_SUSPEND;
			continue;
		}

		ptr = strstr(buf, "PM: suspend entry");
		if (ptr) {
			parse_timestamp(ptr + 18, &suspend_start);
			state |= STATE_SUSPEND_TIME;
			continue;
		}

		ptr = strstr(buf, "PM: suspend exit");
		if (ptr) {
			parse_timestamp(ptr + 17, &suspend_exit);

			if (opt_flags & OPT_VERBOSE) {
				printf("%4.4d/%2.2d/%2.2d %2.2d:%2.2d:%08.5f: %f ", 
					suspend_start.year, suspend_start.month, suspend_start.day,
					suspend_start.hour, suspend_start.minute, suspend_start.second,
					suspend_exit.seconds_since_1900 - suspend_start.seconds_since_1900);
			}
			if (state & STATE_SUSPEND_SUCCESS) {
				if (opt_flags & OPT_VERBOSE) {
					printf("Successful Suspend. ");
				}
				suspend_succeeded++;
				suspend_total += (suspend_exit.seconds_since_1900 - suspend_start.seconds_since_1900);
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
			if (opt_flags & OPT_WAKELOCK_BLOCKERS) {
				wakelock_increment(wakelock);
			}
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
		qsort(wakelocks, HASH_SIZE, sizeof(wakelock_info), wakelock_cmp);
		for (i = 0; i < HASH_SIZE; i++) {
			if (wakelocks[i].name) {
				printf("%-28.28s %8d\n", wakelocks[i].name, wakelocks[i].count);
				free(wakelocks[i].name);
			}
		}
		printf("\n");
	}

	printf("%d suspends aborted (%.2f%%).\n",
		suspend_failed,
		suspend_count == 0 ? 0.0 : 100.0 * (double)suspend_failed / (double)suspend_count);
	printf("%d suspends succeeded (%.2f%%).\n",
		suspend_succeeded,
		suspend_count == 0 ? 0.0 : 100.0 * (double)suspend_succeeded / (double)suspend_count);
	printf("%f seconds average suspend duration.\n",
		suspend_succeeded == 0 ? 0.0 : suspend_total / (double)suspend_succeeded);
}

void show_help(char * const argv[])
{
	printf("%s, version %s\n\n", APP_NAME, VERSION);
	printf("usage: %s [options] [kernel_log]\n", argv[0]);
	printf("\t-b list blocking wakelock names and count.\n");
	printf("\t-h this help.\n");
	printf("\t-v verbose information.\n");
}

int main(int argc, char **argv)
{
	for (;;) {
		int c = getopt(argc, argv, "bhv");	
		if (c == -1)
			break;
		switch (c) {
		case 'b':
			opt_flags |= OPT_WAKELOCK_BLOCKERS;
			break;
		case 'v':
			opt_flags |= OPT_VERBOSE;
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
