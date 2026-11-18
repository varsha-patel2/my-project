#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------ Constants -------------------------------- */
#define NAME_LEN             50
#define DEPT_LEN             30
#define INPUT_LEN            128
#define INITIAL_CAPACITY     4

#define TARGET_HOURS         4.0    /* ideal hours per task (100% speed)      */
#define ERROR_QUALITY_LIMIT  60.0   /* quality below this counts as an error  */
#define TREND_MARGIN         2.0    /* score change needed to call a trend    */

#define PERIOD_MIN           199001 /* valid periods: YYYYMM                  */
#define PERIOD_MAX           210012
#define ALL_FROM             0      /* "no lower limit" for period filter     */
#define ALL_TO               999999 /* "no upper limit" for period filter     */

/* ------------------------------ Structures ------------------------------- */

/* One completed task entry of an employee. */
typedef struct {
    int    task_id;
    double hours;     /* hours spent on the task            */
    double quality;   /* quality score 0 - 100              */
    int    period;    /* YYYYMM, e.g. 202610 = Oct 2026     */
} TaskLog;

/* An employee with a dynamically-sized array of logs. */
typedef struct {
    int      id;
    char     name[NAME_LEN];
    char     dept[DEPT_LEN];
    TaskLog *logs;          /* dynamic array (malloc/realloc) */
    int      log_count;
    int      log_capacity;
} Employee;

/* Dynamic array of employees. */
typedef struct {
    Employee *items;
    int       count;
    int       capacity;
} EmployeeList;

/* KPIs computed for one employee (optionally for a period range). */
typedef struct {
    int    tasks;
    double total_hours;
    double avg_time;
    double avg_quality;
    double error_rate;
    double productivity;
    double reliability;
    double overall;
} KPI;

/* One row of the ranking table. */
typedef struct {
    Employee *emp;
    KPI       kpi;
} RankEntry;

/* Minimum acceptable values; anything lower is flagged. */
typedef struct {
    double min_productivity;
    double min_reliability;
    double min_overall;
} Thresholds;

typedef enum {
    TREND_NONE,       /* not enough data */
    TREND_IMPROVING,
    TREND_WORSENING,
    TREND_STABLE
} Trend;

/* ------------------------------- Globals --------------------------------- */
static EmployeeList company = { NULL, 0, 0 };
static Thresholds   limits  = { 50.0, 70.0, 60.0 };

/* =========================================================================
 *  SECTION 1 : Safe input helpers
 * ========================================================================= */

/* Remove leading and trailing whitespace in place. */
static void trim(char *s)
{
    char  *start = s;
    size_t len;

    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

/* Print prompt, read one line (without newline). Exits cleanly on EOF. */
static void read_line(const char *prompt, char *buf, size_t size)
{
    size_t len;

    printf("%s", prompt);
    fflush(stdout);

    if (fgets(buf, (int)size, stdin) == NULL) {
        printf("\nInput closed. Exiting.\n");
        exit(0);                      /* free_all() runs through atexit() */
    }

    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    } else {                          /* line was too long: drop the rest */
        int c;
        while ((c = getchar()) != '\n' && c != EOF) { }
    }
}

/* Read an integer in [min, max]; repeats until valid. */
static int read_int(const char *prompt, int min, int max)
{
    char buf[INPUT_LEN];
    char *end;
    long v;

    for (;;) {
        read_line(prompt, buf, sizeof buf);
        trim(buf);
        if (buf[0] == '\0') { puts("  Please enter a value."); continue; }
        v = strtol(buf, &end, 10);
        if (*end != '\0')   { puts("  Invalid number, try again."); continue; }
        if (v < min || v > max) {
            printf("  Value must be between %d and %d.\n", min, max);
            continue;
        }
        return (int)v;
    }
}

/* Read a real number in [min, max]; repeats until valid. */
static double read_double(const char *prompt, double min, double max)
{
    char buf[INPUT_LEN];
    char *end;
    double v;

    for (;;) {
        read_line(prompt, buf, sizeof buf);
        trim(buf);
        if (buf[0] == '\0') { puts("  Please enter a value."); continue; }
        v = strtod(buf, &end);
        if (*end != '\0')   { puts("  Invalid number, try again."); continue; }
        if (!(v >= min && v <= max)) {   /* also rejects NaN */
            printf("  Value must be between %.1f and %.1f.\n", min, max);
            continue;
        }
        return v;
    }
}

/* Read a non-empty text value. */
static void read_text(const char *prompt, char *buf, size_t size)
{
    for (;;) {
        read_line(prompt, buf, size);
        trim(buf);
        if (buf[0] != '\0') return;
        puts("  This field cannot be empty.");
    }
}

static int read_yes_no(const char *prompt)
{
    char buf[INPUT_LEN];
    for (;;) {
        read_line(prompt, buf, sizeof buf);
        trim(buf);
        if (buf[0] == 'y' || buf[0] == 'Y') return 1;
        if (buf[0] == 'n' || buf[0] == 'N') return 0;
        puts("  Please answer y or n.");
    }
}

static int valid_period(int p)
{
    int month = p % 100;
    return p >= PERIOD_MIN && p <= PERIOD_MAX && month >= 1 && month <= 12;
}

/*
 * Read a period as YYYYMM. If allow_blank is set, an empty line returns 0
 * (meaning "not given"); otherwise it repeats. Returns 1 when *out is set.
 */
static int read_period(const char *prompt, int allow_blank, int *out)
{
    char buf[INPUT_LEN];
    char *end;
    long v;

    for (;;) {
        read_line(prompt, buf, sizeof buf);
        trim(buf);
        if (buf[0] == '\0') {
            if (allow_blank) return 0;
            puts("  Please enter a period like 202610.");
            continue;
        }
        v = strtol(buf, &end, 10);
        if (*end != '\0' || !valid_period((int)v)) {
            puts("  Invalid period. Use YYYYMM, e.g. 202610 for Oct 2026.");
            continue;
        }
        *out = (int)v;
        return 1;
    }
}

/* =========================================================================
 *  SECTION 2 : String helpers (case-insensitive compare / search)
 * ========================================================================= */

static int equals_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int contains_ci(const char *text, const char *part)
{
    size_t plen = strlen(part), i, j;
    size_t tlen = strlen(text);

    if (plen == 0) return 1;
    if (plen > tlen) return 0;
    for (i = 0; i + plen <= tlen; i++) {
        for (j = 0; j < plen; j++) {
            if (tolower((unsigned char)text[i + j]) !=
                tolower((unsigned char)part[j])) break;
        }
        if (j == plen) return 1;
    }
    return 0;
}

/* =========================================================================
 *  SECTION 3 : Employee & log management (dynamic memory)
 * ========================================================================= */

/* Search by ID. Returns a pointer to the employee, or NULL. */
static Employee *find_by_id(const EmployeeList *list, int id)
{
    Employee *p;
    for (p = list->items; p < list->items + list->count; p++)
        if (p->id == id) return p;
    return NULL;
}

/*
 * Search by (part of a) name. Returns a malloc'd array of pointers to the
 * matching employees (caller must free). *found receives the match count.
 */
static Employee **search_by_name(const EmployeeList *list, const char *part,
                                 int *found)
{
    Employee **res;
    int i, n = 0;

    *found = 0;
    res = malloc((list->count > 0 ? list->count : 1) * sizeof *res);
    if (res == NULL) return NULL;

    for (i = 0; i < list->count; i++)
        if (contains_ci(list->items[i].name, part))
            res[n++] = &list->items[i];

    *found = n;
    return res;
}

/* Add a new employee. Returns pointer to it, or NULL on error/duplicate ID. */
static Employee *add_employee(EmployeeList *list, int id,
                              const char *name, const char *dept)
{
    Employee *e;

    if (find_by_id(list, id) != NULL) return NULL;

    if (list->count == list->capacity) {
        int newcap = list->capacity ? list->capacity * 2 : INITIAL_CAPACITY;
        Employee *tmp = realloc(list->items, newcap * sizeof *tmp);
        if (tmp == NULL) return NULL;
        list->items    = tmp;
        list->capacity = newcap;
    }

    e = &list->items[list->count++];
    e->id = id;
    strncpy(e->name, name, NAME_LEN - 1); e->name[NAME_LEN - 1] = '\0';
    strncpy(e->dept, dept, DEPT_LEN - 1); e->dept[DEPT_LEN - 1] = '\0';
    e->logs         = NULL;
    e->log_count    = 0;
    e->log_capacity = 0;
    return e;
}

/*
 * Add a performance log to an employee (array grows automatically).
 * Returns 1 on success, 0 if the task ID already exists or memory fails.
 */
static int add_log(Employee *e, int task_id, double hours, double quality,
                   int period)
{
    TaskLog *p;

    for (p = e->logs; p < e->logs + e->log_count; p++)
        if (p->task_id == task_id) return 0;          /* duplicate task id */

    if (e->log_count == e->log_capacity) {
        int newcap = e->log_capacity ? e->log_capacity * 2 : INITIAL_CAPACITY;
        TaskLog *tmp = realloc(e->logs, newcap * sizeof *tmp);
        if (tmp == NULL) return 0;
        e->logs         = tmp;
        e->log_capacity = newcap;
    }

    p = &e->logs[e->log_count++];
    p->task_id = task_id;
    p->hours   = hours;
    p->quality = quality;
    p->period  = period;
    return 1;
}

/* Release every block of dynamic memory (registered with atexit). */
static void free_all(void)
{
    int i;
    for (i = 0; i < company.count; i++) free(company.items[i].logs);
    free(company.items);
    company.items = NULL;
    company.count = company.capacity = 0;
}

/* =========================================================================
 *  SECTION 4 : KPI computation
 * ========================================================================= */

/*
 * Compute KPIs of one employee using only logs with from <= period <= to.
 * Returns the number of logs used (0 means no data; KPI is all zeros).
 */
static int compute_kpi(const Employee *e, int from, int to, KPI *k)
{
    const TaskLog *p;
    int    errors = 0;
    double quality_sum = 0.0, speed;

    memset(k, 0, sizeof *k);

    for (p = e->logs; p < e->logs + e->log_count; p++) {
        if (p->period < from || p->period > to) continue;
        k->tasks++;
        k->total_hours += p->hours;
        quality_sum    += p->quality;
        if (p->quality < ERROR_QUALITY_LIMIT) errors++;
    }
    if (k->tasks == 0) return 0;

    k->avg_time    = k->total_hours / k->tasks;
    k->avg_quality = quality_sum / k->tasks;
    k->error_rate  = 100.0 * errors / k->tasks;

    speed = TARGET_HOURS / k->avg_time * 100.0;
    k->productivity = speed > 100.0 ? 100.0 : speed;
    k->reliability  = 0.5 * (100.0 - k->error_rate) + 0.5 * k->avg_quality;
    k->overall      = 0.5 * k->productivity + 0.5 * k->reliability;
    return k->tasks;
}

/* Score of a single task (same weights as the overall KPI). */
static double log_score(const TaskLog *t)
{
    double speed = TARGET_HOURS / t->hours * 100.0;
    if (speed > 100.0) speed = 100.0;
    return 0.5 * speed + 0.5 * t->quality;
}

/*
 * Build a flag description such as "LOW-PROD LOW-REL".
 * Returns the number of thresholds that were violated (0 = employee is OK).
 */
static int describe_flags(const KPI *k, char *buf, size_t size)
{
    int n = 0;
    buf[0] = '\0';
    if (k->productivity < limits.min_productivity) {
        strncat(buf, "LOW-PROD ", size - strlen(buf) - 1); n++;
    }
    if (k->reliability < limits.min_reliability) {
        strncat(buf, "LOW-REL ", size - strlen(buf) - 1);  n++;
    }
    if (k->overall < limits.min_overall) {
        strncat(buf, "LOW-OVERALL ", size - strlen(buf) - 1); n++;
    }
    if (n == 0) strncpy(buf, "OK", size - 1), buf[size - 1] = '\0';
    return n;
}

/* =========================================================================
 *  SECTION 5 : Sorting (ranking) and statistics
 * ========================================================================= */

/* Insertion sort, highest overall score first (ties: lower ID first). */
static void sort_ranking(RankEntry *a, int n)
{
    int i, j;
    RankEntry key;

    for (i = 1; i < n; i++) {
        key = a[i];
        j = i - 1;
        while (j >= 0 &&
               (a[j].kpi.overall < key.kpi.overall ||
                (a[j].kpi.overall == key.kpi.overall &&
                 a[j].emp->id > key.emp->id))) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

/* Insertion sort of doubles, ascending. */
static void sort_doubles(double *a, int n)
{
    int i, j;
    double key;
    for (i = 1; i < n; i++) {
        key = a[i];
        for (j = i - 1; j >= 0 && a[j] > key; j--) a[j + 1] = a[j];
        a[j + 1] = key;
    }
}

static double mean(const double *a, int n)
{
    double s = 0.0;
    int i;
    if (n <= 0) return 0.0;
    for (i = 0; i < n; i++) s += a[i];
    return s / n;
}

/* Median on a sorted COPY, so the caller's array is not disturbed. */
static double median(const double *a, int n)
{
    double *copy, m;
    if (n <= 0) return 0.0;
    copy = malloc(n * sizeof *copy);
    if (copy == NULL) return 0.0;
    memcpy(copy, a, n * sizeof *copy);
    sort_doubles(copy, n);
    m = (n % 2) ? copy[n / 2] : (copy[n / 2 - 1] + copy[n / 2]) / 2.0;
    free(copy);
    return m;
}

static double min_of(const double *a, int n)
{
    double m;
    int i;
    if (n <= 0) return 0.0;
    m = a[0];
    for (i = 1; i < n; i++) if (a[i] < m) m = a[i];
    return m;
}

static double max_of(const double *a, int n)
{
    double m;
    int i;
    if (n <= 0) return 0.0;
    m = a[0];
    for (i = 1; i < n; i++) if (a[i] > m) m = a[i];
    return m;
}

/*
 * Filter employees by department ("" = all) and period range, compute their
 * KPIs, sort them and return a malloc'd array of RankEntry (caller frees).
 * Employees with no logs in the range are left out.
 */
static RankEntry *build_ranking(const EmployeeList *list, const char *dept,
                                int from, int to, int *out_count)
{
    RankEntry *r;
    KPI k;
    int i, n = 0;

    *out_count = 0;
    r = malloc((list->count > 0 ? list->count : 1) * sizeof *r);
    if (r == NULL) return NULL;

    for (i = 0; i < list->count; i++) {
        Employee *e = &list->items[i];
        if (dept[0] != '\0' && !equals_ci(e->dept, dept)) continue;
        if (compute_kpi(e, from, to, &k) == 0) continue;
        r[n].emp = e;
        r[n].kpi = k;
        n++;
    }
    sort_ranking(r, n);
    *out_count = n;
    return r;
}

/* =========================================================================
 *  SECTION 6 : Trend detection
 * ========================================================================= */

/*
 * Look at the last n logs (in the order they were added). The older half is
 * compared with the newer half using the per-task score.
 *   newer - older >  TREND_MARGIN  -> improving
 *   newer - older < -TREND_MARGIN  -> worsening
 *   otherwise                      -> stable
 * Needs at least 2 logs; otherwise TREND_NONE is returned.
 */
static Trend detect_trend(const Employee *e, int n, double *older_avg,
                          double *newer_avg, int *used)
{
    const TaskLog *start, *p;
    int half, i;
    double s1 = 0.0, s2 = 0.0, diff;

    *older_avg = *newer_avg = 0.0;
    if (n > e->log_count) n = e->log_count;
    *used = n;
    if (n < 2) return TREND_NONE;

    start = e->logs + (e->log_count - n);
    half  = n / 2;

    for (i = 0, p = start; i < n; i++, p++) {
        if (i < half) s1 += log_score(p);
        else          s2 += log_score(p);
    }
    *older_avg = s1 / half;
    *newer_avg = s2 / (n - half);
    diff = *newer_avg - *older_avg;

    if (diff >  TREND_MARGIN) return TREND_IMPROVING;
    if (diff < -TREND_MARGIN) return TREND_WORSENING;
    return TREND_STABLE;
}

static const char *trend_name(Trend t)
{
    switch (t) {
        case TREND_IMPROVING: return "IMPROVING";
        case TREND_WORSENING: return "WORSENING";
        case TREND_STABLE:    return "STABLE";
        default:              return "NOT ENOUGH DATA";
    }
}

/* =========================================================================
 *  SECTION 7 : Printing helpers
 * ========================================================================= */

static void line(int n, char c)
{
    while (n-- > 0) putchar(c);
    putchar('\n');
}

static void print_period(int p)
{
    printf("%d-%02d", p / 100, p % 100);
}

static void print_filter(const char *dept, int from, int to)
{
    printf("Filter -> Department: %s | Period: ",
           dept[0] ? dept : "All");
    if (from == ALL_FROM && to == ALL_TO) {
        printf("All time");
    } else {
        if (from == ALL_FROM) printf("start"); else print_period(from);
        printf(" to ");
        if (to == ALL_TO) printf("latest"); else print_period(to);
    }
    putchar('\n');
}

static void print_kpi_block(const KPI *k)
{
    char flags[64];
    describe_flags(k, flags, sizeof flags);
    printf("  Tasks completed   : %d\n",    k->tasks);
    printf("  Total hours       : %.1f\n",  k->total_hours);
    printf("  Avg completion    : %.2f hrs/task\n", k->avg_time);
    printf("  Avg quality       : %.2f\n",  k->avg_quality);
    printf("  Error rate        : %.1f %%\n", k->error_rate);
    printf("  Productivity score: %.2f\n",  k->productivity);
    printf("  Reliability index : %.2f\n",  k->reliability);
    printf("  Overall score     : %.2f\n",  k->overall);
    printf("  Status            : %s\n",    flags);
}

static void print_employee_detail(const Employee *e)
{
    KPI k;
    const TaskLog *p;

    printf("\nEmployee ID : %d\nName        : %s\nDepartment  : %s\n",
           e->id, e->name, e->dept);
    printf("Logs stored : %d\n", e->log_count);

    if (compute_kpi(e, ALL_FROM, ALL_TO, &k) == 0) {
        puts("  (no performance logs yet)");
        return;
    }
    puts("KPIs (all time):");
    print_kpi_block(&k);

    puts("Task logs:");
    printf("  %-8s %-8s %-8s %-8s %s\n", "Task", "Period", "Hours", "Quality",
           "Error?");
    for (p = e->logs; p < e->logs + e->log_count; p++) {
        printf("  %-8d ", p->task_id);
        print_period(p->period);
        printf("  %-8.1f %-8.1f %s\n", p->hours, p->quality,
               p->quality < ERROR_QUALITY_LIMIT ? "yes" : "no");
    }
}

static void print_trend_line(const Employee *e, int n)
{
    double older, newer;
    int used;
    Trend t = detect_trend(e, n, &older, &newer, &used);

    printf("  %-6d %-20s %-16s ", e->id, e->name, trend_name(t));
    if (t == TREND_NONE)
        printf("(only %d log(s))\n", used);
    else
        printf("last %d logs: older avg %.1f -> newer avg %.1f (%+.1f)\n",
               used, older, newer, newer - older);
}

/* =========================================================================
 *  SECTION 8 : Menu actions
 * ========================================================================= */

static void ask_filter(char *dept, size_t size, int *from, int *to)
{
    int v;
    read_line("Department filter (blank = all): ", dept, size);
    trim(dept);
    *from = ALL_FROM;
    *to   = ALL_TO;
    if (read_period("From period YYYYMM (blank = no limit): ", 1, &v)) *from = v;
    if (read_period("To period   YYYYMM (blank = no limit): ", 1, &v)) *to   = v;
    if (*from > *to) {
        int t = *from; *from = *to; *to = t;
        puts("  (From was later than To - the two were swapped.)");
    }
}

static void menu_add_employee(void)
{
    char name[NAME_LEN], dept[DEPT_LEN];
    int id;

    puts("\n--- Add Employee ---");
    id = read_int("Employee ID (1-999999): ", 1, 999999);
    if (find_by_id(&company, id) != NULL) {
        printf("An employee with ID %d already exists.\n", id);
        return;
    }
    read_text("Name       : ", name, sizeof name);
    read_text("Department : ", dept, sizeof dept);

    if (add_employee(&company, id, name, dept) != NULL)
        printf("Employee %d (%s) added.\n", id, name);
    else
        puts("Could not add employee (out of memory).");
}

static void menu_add_log(void)
{
    Employee *e;
    int id, task_id, period;
    double hours, quality;

    puts("\n--- Add Performance Log ---");
    if (company.count == 0) { puts("No employees yet. Add one first."); return; }

    id = read_int("Employee ID: ", 1, 999999);
    e = find_by_id(&company, id);
    if (e == NULL) { printf("No employee with ID %d.\n", id); return; }
    printf("Adding logs for %s (%s)\n", e->name, e->dept);

    do {
        task_id = read_int("Task ID (1-999999999): ", 1, 999999999);
        hours   = read_double("Hours spent (0.1 - 500): ", 0.1, 500.0);
        quality = read_double("Quality score (0 - 100): ", 0.0, 100.0);
        read_period("Period YYYYMM (e.g. 202610): ", 0, &period);

        if (add_log(e, task_id, hours, quality, period))
            puts("Log added.");
        else
            printf("Task ID %d already exists for this employee "
                   "(or memory error). Log NOT added.\n", task_id);
    } while (read_yes_no("Add another log for this employee? (y/n): "));
}

static void menu_view_employees(void)
{
    int i;
    puts("\n--- All Employees ---");
    if (company.count == 0) { puts("No employees stored."); return; }

    printf("%-8s %-22s %-16s %s\n", "ID", "Name", "Department", "Logs");
    line(56, '-');
    for (i = 0; i < company.count; i++) {
        const Employee *e = &company.items[i];
        printf("%-8d %-22s %-16s %d\n", e->id, e->name, e->dept, e->log_count);
    }
}

static void menu_ranking(void)
{
    char dept[DEPT_LEN];
    char flags[64];
    int from, to, n, i, flagged = 0;
    RankEntry *r;

    puts("\n--- Ranking & Flags ---");
    ask_filter(dept, sizeof dept, &from, &to);
    r = build_ranking(&company, dept, from, to, &n);
    if (r == NULL) { puts("Out of memory."); return; }

    putchar('\n');
    print_filter(dept, from, to);
    if (n == 0) { puts("No employees have logs for this filter."); free(r); return; }

    line(104, '=');
    printf("%-5s %-6s %-18s %-12s %-6s %-8s %-7s %-7s %-7s %-7s %s\n",
           "Rank", "ID", "Name", "Dept", "Tasks", "AvgTime", "Err%",
           "Prod", "Reliab", "Score", "Status");
    line(104, '-');
    for (i = 0; i < n; i++) {
        const KPI *k = &r[i].kpi;
        if (describe_flags(k, flags, sizeof flags) > 0) flagged++;
        printf("%-5d %-6d %-18.18s %-12.12s %-6d %-8.2f %-7.1f %-7.1f %-7.1f %-7.1f %s\n",
               i + 1, r[i].emp->id, r[i].emp->name, r[i].emp->dept,
               k->tasks, k->avg_time, k->error_rate, k->productivity,
               k->reliability, k->overall, flags);
    }
    line(104, '=');
    printf("Thresholds: productivity >= %.1f, reliability >= %.1f, overall >= %.1f\n",
           limits.min_productivity, limits.min_reliability, limits.min_overall);
    printf("%d of %d employee(s) flagged below threshold.\n", flagged, n);
    free(r);
}

/* Print one statistic line: average, median, min, max. */
static void print_stat(const char *label, const double *a, int n)
{
    printf("  %-26s avg %8.2f | median %8.2f | min %8.2f | max %8.2f\n",
           label, mean(a, n), median(a, n), min_of(a, n), max_of(a, n));
}

static void menu_summary(void)
{
    char dept[DEPT_LEN];
    int from, to, n, i, j, total_logs = 0, flagged = 0, m = 0;
    char flags[64];
    RankEntry *r;
    double *prod, *rel, *score, *avgt, *err, *tasks, *hours, *qual;

    puts("\n--- Summary Report ---");
    ask_filter(dept, sizeof dept, &from, &to);
    r = build_ranking(&company, dept, from, to, &n);
    if (r == NULL) { puts("Out of memory."); return; }

    putchar('\n');
    print_filter(dept, from, to);
    if (n == 0) { puts("No data for this filter."); free(r); return; }

    for (i = 0; i < n; i++) total_logs += r[i].kpi.tasks;

    prod  = malloc(n * sizeof *prod);
    rel   = malloc(n * sizeof *rel);
    score = malloc(n * sizeof *score);
    avgt  = malloc(n * sizeof *avgt);
    err   = malloc(n * sizeof *err);
    tasks = malloc(n * sizeof *tasks);
    hours = malloc(total_logs * sizeof *hours);
    qual  = malloc(total_logs * sizeof *qual);

    if (!prod || !rel || !score || !avgt || !err || !tasks || !hours || !qual) {
        puts("Out of memory.");
        goto cleanup;
    }

    for (i = 0; i < n; i++) {
        const Employee *e = r[i].emp;
        const KPI *k = &r[i].kpi;
        const TaskLog *p;

        prod[i] = k->productivity;  rel[i]   = k->reliability;
        score[i] = k->overall;      avgt[i]  = k->avg_time;
        err[i]  = k->error_rate;    tasks[i] = k->tasks;
        if (describe_flags(k, flags, sizeof flags) > 0) flagged++;

        for (j = 0, p = e->logs; j < e->log_count; j++, p++) {
            if (p->period < from || p->period > to) continue;
            hours[m] = p->hours;
            qual[m]  = p->quality;
            m++;
        }
    }

    line(100, '=');
    printf("Employees included : %d\n", n);
    printf("Task logs included : %d\n", total_logs);
    printf("Employees flagged  : %d\n", flagged);
    printf("Top performer      : %s (score %.2f)\n",
           r[0].emp->name, r[0].kpi.overall);
    printf("Lowest performer   : %s (score %.2f)\n",
           r[n - 1].emp->name, r[n - 1].kpi.overall);

    puts("\nPer-employee KPIs:");
    print_stat("Tasks completed",       tasks, n);
    print_stat("Avg completion (hrs)",  avgt,  n);
    print_stat("Error rate (%)",        err,   n);
    print_stat("Productivity score",    prod,  n);
    print_stat("Reliability index",     rel,   n);
    print_stat("Overall score",         score, n);

    puts("\nPer-task logs:");
    print_stat("Hours per task",        hours, m);
    print_stat("Quality score",         qual,  m);
    line(100, '=');

cleanup:
    free(prod); free(rel); free(score); free(avgt);
    free(err);  free(tasks); free(hours); free(qual);
    free(r);
}

static void menu_search(void)
{
    int choice;
    puts("\n--- Search Employee ---");
    puts("1. By ID");
    puts("2. By name (partial, not case-sensitive)");
    choice = read_int("Choice: ", 1, 2);

    if (choice == 1) {
        int id = read_int("Employee ID: ", 1, 999999);
        Employee *e = find_by_id(&company, id);
        if (e == NULL) printf("No employee with ID %d.\n", id);
        else           print_employee_detail(e);
    } else {
        char part[NAME_LEN];
        Employee **res;
        int found, i;

        read_text("Name contains: ", part, sizeof part);
        res = search_by_name(&company, part, &found);
        if (res == NULL) { puts("Out of memory."); return; }
        if (found == 0) printf("No employee matches \"%s\".\n", part);
        else {
            printf("%d match(es) found.\n", found);
            for (i = 0; i < found; i++) print_employee_detail(res[i]);
        }
        free(res);
    }
}

static void menu_trend(void)
{
    int id, n, i;

    puts("\n--- Trend Detection ---");
    n  = read_int("Number of most recent records N (2-1000): ", 2, 1000);
    id = read_int("Employee ID (0 = all employees): ", 0, 999999);

    puts("(Logs are read in the order they were added; the older half of the");
    puts(" last N logs is compared with the newer half.)");
    printf("  %-6s %-20s %-16s %s\n", "ID", "Name", "Trend", "Details");
    line(90, '-');

    if (id == 0) {
        if (company.count == 0) { puts("No employees stored."); return; }
        for (i = 0; i < company.count; i++) print_trend_line(&company.items[i], n);
    } else {
        Employee *e = find_by_id(&company, id);
        if (e == NULL) printf("No employee with ID %d.\n", id);
        else           print_trend_line(e, n);
    }
}

static void menu_thresholds(void)
{
    puts("\n--- Flag Thresholds ---");
    printf("Current: productivity >= %.1f, reliability >= %.1f, overall >= %.1f\n",
           limits.min_productivity, limits.min_reliability, limits.min_overall);
    if (!read_yes_no("Change them? (y/n): ")) return;

    limits.min_productivity = read_double("Min productivity (0-100): ", 0, 100);
    limits.min_reliability  = read_double("Min reliability  (0-100): ", 0, 100);
    limits.min_overall      = read_double("Min overall      (0-100): ", 0, 100);
    puts("Thresholds updated.");
}

/*
 * Demo data: 5 employees, 8 logs each, with a deliberate pattern
 * (improving, worsening, steady-good, poor, average).
 */
static void load_sample_data(void)
{
    static const struct {
        int id; const char *name; const char *dept;
        double base_h, slope_h, base_q, slope_q;
    } demo[] = {
        { 101, "Aarav Sharma", "Engineering", 6.0, -0.5, 55.0,  4.0 },
        { 102, "Priya Verma",  "Engineering", 2.5,  0.6, 90.0, -4.0 },
        { 103, "Rohan Gupta",  "Sales",       3.0,  0.0, 88.0,  0.0 },
        { 104, "Neha Singh",   "Sales",       8.0,  0.1, 45.0,  1.0 },
        { 105, "Karan Mehta",  "HR",          4.5,  0.0, 72.0,  0.5 }
    };
    int d, i, added = 0, skipped = 0;
    int count = (int)(sizeof demo / sizeof demo[0]);

    for (d = 0; d < count; d++) {
        Employee *e = add_employee(&company, demo[d].id, demo[d].name,
                                   demo[d].dept);
        if (e == NULL) { skipped++; continue; }   /* ID already exists */
        for (i = 0; i < 8; i++) {
            double h = demo[d].base_h + demo[d].slope_h * i;
            double q = demo[d].base_q + demo[d].slope_q * i;
            int period = 202607 + i / 2;          /* 2 logs per month */
            if (h < 0.5)   h = 0.5;
            if (q < 0.0)   q = 0.0;
            if (q > 100.0) q = 100.0;
            add_log(e, demo[d].id * 100 + i + 1, h, q, period);
        }
        added++;
    }
    printf("Sample data loaded: %d employee(s) added, %d skipped (ID exists).\n",
           added, skipped);
    puts("Periods used: 2026-07 to 2026-10, departments: Engineering, Sales, HR.");
}

static void print_menu(void)
{
    putchar('\n');
    line(52, '=');
    puts("  EMPLOYEE PERFORMANCE & KPI DASHBOARD");
    line(52, '=');
    puts("  1. Add employee");
    puts("  2. Add performance log");
    puts("  3. View all employees");
    puts("  4. Rank employees & show flags (filterable)");
    puts("  5. Summary report - average / median (filterable)");
    puts("  6. Search employee (by ID or name)");
    puts("  7. Trend detection (last N records)");
    puts("  8. Change flag thresholds");
    puts("  9. Load sample data");
    puts("  0. Exit");
    line(52, '-');
}

/* =========================================================================
 *  SECTION 9 : main
 * ========================================================================= */
int main(void)
{
    int choice;

    atexit(free_all);   /* guarantees all dynamic memory is released */

    puts("Welcome! Tip: choose option 9 first to load demo data.");

    for (;;) {
        print_menu();
        choice = read_int("Enter choice: ", 0, 9);

        switch (choice) {
            case 1: menu_add_employee();  break;
            case 2: menu_add_log();       break;
            case 3: menu_view_employees();break;
            case 4: menu_ranking();       break;
            case 5: menu_summary();       break;
            case 6: menu_search();        break;
            case 7: menu_trend();         break;
            case 8: menu_thresholds();    break;
            case 9: load_sample_data();   break;
            case 0: puts("Goodbye!");     return 0;
        }
    }
}