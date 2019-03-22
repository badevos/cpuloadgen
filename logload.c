#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#define msleep(x) ( usleep ( x * 1000 ) )

static void read_stat (char * buff, int size)
{
	char * r = 0;
	FILE * fp = fopen("/proc/stat", "r");
	if (NULL == fp) {
		err(1, "fopen(/proc/stat, r)");
	}
	r = fgets(buff, size, fp);
	if (NULL == r) {
		err(1, "fgets(..., %i, /proc/stat", size);
	}
	fclose (fp);
}

static void parse_stat (char * str, long int * sum, long int * idle)
{
	int i = 0;
	char * token = NULL;
	const char d[2] = " ";

	for (*sum = 0, token=strtok(str, d); token !=NULL; token = strtok(NULL,d), ++i ) {
		*sum += atoi(token);
		if (i == 4) {
			*idle = atoi(token);
		}
	}
}

static long int prev_sum  = 0;
static long int prev_idle = 0;
static void update_stat (int verbose)
{
	char str[100] = {};
	long int sum  = 0;
	long int idle = 0;
	double idle_readable = 0.0;
	double load_readable = 0.0;

	read_stat(str, sizeof(str));
	parse_stat(str, &sum, &idle);

	idle_readable = (idle-prev_idle)*100.0/(sum-prev_sum);
	load_readable = 100.0 - idle_readable;

	if (verbose) {
		syslog(LOG_INFO, "%0.2lf%%", load_readable);
	}

	prev_sum = sum;
	prev_idle = idle;
}

int main ( int argc, char* argv[] )
{
	int delay = 0;
	int times = 0;
	int forever = 0;

	if ( argc != 3 ) {
		printf("Usage : %s iterations delay\n", argv[0]);
		printf("   iterations: number of times /proc/stat is evaluated (0=forever)\n");
		printf("   delay:      delay (in ms) between successive reads \n");
		exit(1);
	}

	times = atoi(argv[1]);
	delay = atoi(argv[2]);
	if (times == 0) {
		forever = 1;
	}

	openlog("logload", LOG_PID, LOG_USER);

	update_stat(0);
	msleep(delay);

	while (times-- || forever) {
		update_stat(1);
		msleep(delay);
	}

	return 0;
}

