#define _GNU_SOURCE
#define _POSIX_C_SOURCE 199309L
#include <err.h>
#include <time.h>
#include <math.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/resource.h>
//------------------------------------------------------------------------------
// pool - linked list handling
//------------------------------------------------------------------------------
typedef struct load {
    unsigned int load;
    unsigned long jiffies;
    int priority;
    int policy;
    int reschedule;
    pid_t tid;
    char * name;
    pthread_t id;
    int idles[10];
    int actives[10];
    struct load * next;
}load_t;
static int number_of_threads = 0;

static void load_debug(load_t * lt)
{
    int idle = 0;
    int active = 0;
    idle = lt->idles[0]
         + lt->idles[1]
         + lt->idles[2]
         + lt->idles[3]
         + lt->idles[4]
         + lt->idles[5]
         + lt->idles[6]
         + lt->idles[7]
         + lt->idles[8]
         + lt->idles[9];

    active = lt->actives[0]
         + lt->actives[1]
         + lt->actives[2]
         + lt->actives[3]
         + lt->actives[4]
         + lt->actives[5]
         + lt->actives[6]
         + lt->actives[7]
         + lt->actives[8]
         + lt->actives[9];

    printf("%20s @ %4u%% '%c'(%i) with %12lu jiffies, %ius idle and %ius active\n",
            lt->name,
            lt->load,
            lt->policy == SCHED_RR ? 'r' : lt->policy == SCHED_FIFO ? 'f' : 'o',
            lt->priority,
            lt->jiffies,
            idle/10,
            active/10);
}

static load_t * pool;

static load_t * pool_insert(load_t * newbie)
{
    load_t * lt = pool;

    if (NULL == lt) {
        lt = pool = newbie;
    } else {
        for ( ; lt->next; lt=lt->next);
        lt->next = newbie;
    }
    ++number_of_threads;
    return newbie;
}

static load_t * pool_remove(load_t * remove)
{
    load_t * lt = pool;

    if (lt == remove) {
        pool = lt->next;
        return remove;
    }

    for ( ; lt; lt=lt->next) {
        if (lt->next == remove) {
            load_t * temp = lt->next;
            lt->next = temp ? temp->next : NULL;
            --number_of_threads;
            if (number_of_threads < 0) {
                number_of_threads = 0;
            }
            return remove;
        }
    }

    return NULL;
}

static load_t * pool_find(const char * name)
{
    load_t * lt = pool;

    for ( ; lt; lt=lt->next) {
        if (0 == strcmp(lt->name, name)) {
            return lt;
        }
    }

    return NULL;
}

static void pool_debug(void)
{
    load_t * lt = pool;
    int i = 0;

    if (!lt) {
        printf("no threads active\n");
        return;
    }

    if (!lt->name) {
        printf("no threads active\n");
        return;
    }

    for ( ; lt; lt=lt->next, ++i) {
        load_debug(lt);
    }

    printf("\n");
}
//------------------------------------------------------------------------------
// time handling
//------------------------------------------------------------------------------
static int64_t timeref_us (void)
{
    int r = 0;
    struct timespec ts;
    if ((r=clock_gettime(CLOCK_MONOTONIC, &ts)) < 0) {
        err(1, "clock_gettime(CLOCK_MONOTONIC, ...)");
    }
    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t elapsed_us (uint64_t ref)
{
    uint64_t now = timeref_us();
    if (now > ref) {
        return now - ref;
    } else {
        return 0xffffffffffffffffL - ref + now;
    }
}

__attribute__((unused))
static void jiffie_loop(unsigned long jiffies)
{
    while(jiffies--);
}
//------------------------------------------------------------------------------
// load functions
//------------------------------------------------------------------------------
static void load_scheduling(load_t * lt)
{
    if (lt->reschedule) {
        struct sched_param sp;
        sp.sched_priority = lt->priority;
        if (lt->policy == SCHED_RR || lt->policy == SCHED_FIFO) {
            if (0 != sched_setscheduler(lt->tid, lt->policy, &sp)) {
                warn("sched_setscheduler");
            }
        } else {
            if (0 != setpriority(PRIO_PROCESS, lt->tid, lt->priority)) {
                warn("setpriority");
            }
        }
        lt->reschedule = 0;
    }
}

//#ifndef gettid
//#define gettid syscall(SYS_gettid)
//#endif
static void * load_function (void * userdata)
{
    int i = 0;
    uint64_t total;
    uint64_t ref, active, idle;
    load_t * lt = (load_t *)userdata;

    lt->tid = syscall(SYS_gettid);

    while (1) {
        /* evaluate rescheduling, causes possible syscalls, outside measurement */
        load_scheduling(lt);
        /* start 'active' measurement */
        ref = timeref_us();
        /* keep stats */
        lt->idles[i] = idle;
        lt->actives[i] = active;
        if (++i == 10) {
            i = 0;
        }
        /* busy loop */
        jiffie_loop(lt->jiffies);
        /* measure 'active' part */
        active = elapsed_us(ref);
        /* calculate sleep part */
        total = (active * 100.0) / (lt->load);
        idle = total - active;
        /* sleep */
        usleep(idle);
    }

    /* the code below would be executed if there would be a condition to stop,
     * for example run for a specified duration */
    printf("%s %i%% done\n", lt->name, lt->load);

    pool_remove(lt);
    memset(lt, 0x00, sizeof(load_t));
    free(lt);
    pthread_exit(NULL);

    return NULL;
}

static load_t * load_create(const char * name, int load, int jiffies, int policy, int priority)
{
    load_t * lt = calloc(1, sizeof(load_t));
    if (NULL == lt) {
        err(1, "load_create(%s, %i, %i): calloc(1, %i)",
            name, load, jiffies, sizeof(load_t));
    }

    lt->load = load;
    lt->name = strdup(name);
    lt->jiffies = jiffies ?: 100000;
    lt->policy = policy;
    lt->priority = priority;
    lt->reschedule = 1;

    if (0 != pthread_create(&lt->id, NULL, load_function, lt)) {
        warn("pthread_create");
        goto err1;
    }

    if (0 != pthread_setname_np(lt->id, name)) {
        warn("pthread_setname_np");
        goto err1;
    }

    pool_insert(lt);
    pool_debug();

#if DEBUG_GDB
    if (0 != pthread_join(lt->id, NULL)) {
        warn("pthread_join");
        goto err2;
    }
#endif

    return lt;

#if DEBUG_GDB
err2:
    pool_remove(lt);
#endif

err1:
    free(lt->name);
    memset(lt, 0x00, sizeof(load_t));
    free(lt);
    err(1, "pthread_join(...)");

    return NULL;
}

static void * load_delete(load_t * lt)
{
    if (0 != pthread_cancel(lt->id)) {
        err(1, "pthread_cancel('%s')", lt->name);
    }

    memset(lt->name, 0x00, strlen(lt->name));
    free(lt->name);
    memset(lt, 0x00, sizeof(load_t));
    free(lt);

    return lt;
}

static load_t * load_modify(const char * name, int load, int jiffies, int policy, int priority)
{
    load_t * lt = pool_find(name);
    if (lt) {
        if (load) {
            lt->load = load;
        }
        if (jiffies) {
            lt->jiffies = jiffies;
        }
        if (lt->policy != policy) {
            lt->policy = policy;
            lt->reschedule = 1;
        }
        if (lt->priority != priority) {
            lt->priority = priority;
            lt->reschedule = 1;
        }
    }

    return lt;
}

//------------------------------------------------------------------------------
// console handling
//------------------------------------------------------------------------------
static void help(void)
{
    printf("usage: command name [options]\n");
    printf("commands:\n");
    printf("  create name load [jiffies policy priority]\n");
    printf("  modify name load [jiffies policy priority]\n");
    printf("  delete name\n");
    printf("  active\n");
    printf("  help command\n");
    printf("\n");
    printf("arguments:\n");
    printf("    name:     unique name for the thread\n");
    printf("    load:     percent load for the thread to generate (1~99)\n");
    printf("    jiffies:  tweak the length of the actual busy loop. ( while(jiffies--); )\n");
    printf("    policy:   adjust thread scheduling policy [f|r|o]\n");
    printf("    priority: adjust priority for SCHED_RR and SCHED_FIFO, or nice level for SCHED_OTHER (1~99)\n");
}

static struct {
    char * command;
    char * name;
    unsigned long load;
    unsigned long jiffies;
    unsigned long policy;
    unsigned long priority;
    load_t * lt;
}console;

__attribute__((unused))
static void debug_str (char * str)
{
    for( ; *str; ++str) {
        printf("%02X ", *str);
    }
    printf("\n");
}

static void strip_newline(char * input)
{
    char * mod = 0;
    if (input) {
        mod = &input[strlen(input)-1];
        if ( (*mod == '\r') || (*mod=='\n') ) {
            *mod = 0;
        }
    }
}

static int console_parse_number (unsigned long * value, char * input)
{
    char * endptr = 0;

    strip_newline(input);

    *value = strtoul(input, &endptr, 0);

    return (*endptr) ? 1 : 0;
}

static void console_init(char * input)
{
    char * load     = 0;
    char * jiffies  = 0;
    char * policy   = 0;
    char * priority = 0;

    if ( NULL == (console.command=strtok(input, " ")) )
        return;
    strip_newline(console.command);

    console.name=strtok(NULL, " ");
    strip_newline(console.name);

    if ( NULL != (load=strtok(NULL, " ")) ) {
        if (0 != console_parse_number(&console.load, load)) {
            printf("invalid value for 'load'\n");
            return;
        }
    }

    if ( NULL != (jiffies=strtok(NULL, " ")) ) {
        if (0 != console_parse_number(&console.jiffies, jiffies)) {
            printf("invalid value for 'jiffies'\n");
            return;
        }
    }

    if ( NULL != (policy=strtok(NULL, " ")) ) {
        switch (policy[0]) {
            case 'o' : console.policy = SCHED_OTHER;
                       break;
            case 'r' : console.policy = SCHED_RR;
                       break;
            case 'f' : console.policy = SCHED_FIFO;
                       break;
            default  : printf("invalid value for 'policy': choose 'o', 'r', or 'f'\n");
                       return;
        }
    }

    if ( NULL != (priority=strtok(NULL, " ")) ) {
        if (0 != console_parse_number(&console.priority, priority)) {
            printf("invalid value for 'jiffies'\n");
            return;
        }
    }
}

static void console_create(char * input)
{
    console_init(input);
    load_t * lt = pool_find(console.name);

    if (lt) {
        printf("error: thread with name '%s' already exists\n", console.name);
        return;
    }

    lt = load_create(console.name, console.load, console.jiffies, console.policy, console.priority);
    if (lt) {
        printf("done\n");
    } else {
        printf("error\n");
    }
}

static void console_delete(char * input)
{
    console_init(input);
    load_t * lt = pool_find(console.name);
    if (NULL == lt) {
        printf("error: thread with name '%s' not found\n", console.name);
        return;
    }

    pool_remove(lt);
    load_delete(lt);

    printf("done\n");
}

static void console_modify(char * input)
{
    console_init(input);
    load_t * lt = 0;

    lt = load_modify(console.name, console.load, console.jiffies, console.policy, console.priority);
    if (lt) {
        printf("done\n");
    } else {
        printf("error\n");
    }
}

static void console_jiffies(char * input)
{
    console_init(input);

    load_t * lt = pool;
    for ( ;lt; lt=lt->next) {
        lt->jiffies = console.jiffies;
    }
}

static void on_stdin (void)
{
    char buff[1024];

    if (0 == fgets(buff, sizeof(buff), stdin)) {
        warn("fgets");
        return;
    }

    if (0 == strncmp(buff, "create", 6)) {
        console_create(buff);
    } else if (0 == strncmp(buff, "delete", 6)) {
        console_delete(buff);
    } else if (0 == strncmp(buff, "modify", 6)) {
        console_modify(buff);
    } else if (0 == strncmp(buff, "active", 6)) {
        pool_debug();
    } else if (0 == strncmp(buff, "jiffie", 6)) {
        console_jiffies(buff);
    } else {
        help();
    }
}
//------------------------------------------------------------------------------
// main
//------------------------------------------------------------------------------
int main (int argc, char * argv[])
{
    int r = 0;
    struct pollfd pfd;
    memset(&pfd, 0x00, sizeof(struct pollfd));
    pfd.events = POLLIN;

    while(1) {
        r = poll(&pfd, 1, -1);
        if (r) {
            on_stdin();
        }
    }

    return 0;
}
