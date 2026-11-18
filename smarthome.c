/*
 * ============================================================================
 *  PROJECT 31 : SMART HOME AUTOMATION CONTROLLER (RULE-BASED)
 * ============================================================================
 *  A console application that models a small smart-home controller.
 *
 *  Features
 *    - Devices live in a dynamic array of structs (malloc / realloc).
 *    - Every device owns a linked list of user schedules (time ranges); the
 *      nodes are allocated on the heap and joined with pointers.
 *    - Simulated sensors: temperature, ambient light and occupancy.
 *    - Rule engine: each rule stores condition codes and a POINTER to an
 *      action function (action_turn_on / action_turn_off).
 *    - Manual override that beats schedules and rules.
 *    - Energy report (per device and total) and a "what-if" thermostat test.
 *    - Configuration is saved to / loaded from a text file at start-up.
 *
 *  Decision order for every device in every cycle
 *    1. Manual override -> the user forced the device ON or OFF
 *    2. Schedule        -> inside a time window the device is ON
 *    3. Sensor rules    -> every matching rule fires in list order, so a
 *                          later rule overrides an earlier one. If no rule
 *                          fires the device keeps its state; it falls back
 *                          to OFF when a schedule ends or an override ends.
 *
 *  Build :  gcc smart_home.c -o smart_home
 *  Run   :  ./smart_home            (Windows: smart_home.exe)
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define NAME_LEN         32      /* device name buffer: 31 characters + '\0'  */
#define LINE_LEN         128     /* size of one line of keyboard input        */
#define MINUTES_PER_DAY  1440
#define WHATIF_STEP      5       /* minutes per cycle in the what-if run      */
#define TARIFF_PER_KWH   8.0     /* Rs per kWh - change to suit your area     */
#define CONFIG_FILE      "smarthome.cfg"

/* ============================== data types =============================== */

typedef enum { DEV_LIGHT, DEV_AC, DEV_FAN, DEV_HEATER, DEV_APPLIANCE, DEV_TYPE_COUNT } DeviceType;

static const char *TYPE_NAME[DEV_TYPE_COUNT] = { "Light", "AC", "Fan", "Heater", "Appliance" };

/* what last decided a device's state */
typedef enum { SRC_NONE, SRC_MANUAL, SRC_SCHEDULE, SRC_RULE } Source;

/* condition codes understood by the rule engine */
typedef enum {
    COND_NONE,          /* unused slot - always true            */
    COND_TEMP_GT,       /* temperature   >  threshold (C)       */
    COND_TEMP_LT,       /* temperature   <  threshold (C)       */
    COND_LIGHT_GT,      /* ambient light >  threshold (lux)     */
    COND_LIGHT_LT,      /* ambient light <  threshold (lux)     */
    COND_OCCUPIED,      /* someone is home                      */
    COND_VACANT,        /* nobody is home                       */
    COND_COUNT
} CondCode;

static const char *COND_TEXT[COND_COUNT] = {
    "(none)", "temperature >", "temperature <", "ambient light >",
    "ambient light <", "someone is home", "nobody is home"
};

/* one time window; windows of a device form a singly linked list */
typedef struct Schedule {
    int start;                  /* minutes since midnight                   */
    int end;                    /* may be < start (window crosses midnight) */
    struct Schedule *next;      /* next window, NULL at the end             */
} Schedule;

typedef struct {
    int id;
    char name[NAME_LEN];
    DeviceType type;
    int status;                 /* 1 = ON, 0 = OFF                          */
    double power_rating;        /* watts                                    */
    Schedule *schedules;        /* head of this device's schedule list      */
    int override_on;            /* 1 = manual override is active            */
    int override_state;         /* state forced while the override is on    */
    Source source;              /* what last decided the state              */
    int last_rule;              /* id of the rule that fired last           */
    double on_minutes;          /* simulated minutes spent ON               */
} Device;

typedef struct {
    double temperature;         /* degrees Celsius                          */
    int light;                  /* lux, 0..1000                             */
    int occupied;               /* 1 = someone is home                      */
} Sensors;

/* a rule action receives the device it acts on */
typedef void (*ActionFn)(Device *);

typedef struct {
    int id;
    int enabled;
    CondCode cond[2];           /* two condition slots joined with AND      */
    double threshold[2];
    int device_id;              /* target by id: the device array can move  */
    ActionFn action;            /* pointer to action_turn_on / _turn_off    */
} Rule;

typedef struct {
    Device *devices;            /* dynamic array of devices                 */
    int dev_count, dev_cap, next_dev_id;
    Rule *rules;                /* dynamic array of rules                   */
    int rule_count, rule_cap, next_rule_id;
    Sensors sensors;            /* latest sensor readings                   */
    int auto_sensors;           /* 1 = simulated day profile, 0 = manual    */
    int noise;                  /* 1 = random jitter on simulated readings  */
    int clock;                  /* simulated time, minutes since midnight   */
    long total_minutes;         /* simulated minutes since counters reset   */
} Controller;

/* ============================ small helpers ============================== */

/* "HH:MM" text; a ring of buffers lets one printf use several calls */
static const char *fmt_time(int minutes)
{
    static char ring[4][8];
    static int next = 0;
    char *buf = ring[next];

    next = (next + 1) % 4;
    snprintf(buf, 8, "%02d:%02d", (minutes / 60) % 24, minutes % 60);
    return buf;
}

static void read_line(const char *prompt, char *buf, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, (int)size, stdin) == NULL) {      /* end of input: quit */
        printf("\n");
        exit(0);
    }
    buf[strcspn(buf, "\r\n")] = '\0';
}

static int read_int(const char *prompt, int lo, int hi)
{
    char line[LINE_LEN], *end;
    long v;
    int parsed;

    for (;;) {
        read_line(prompt, line, sizeof line);
        v = strtol(line, &end, 10);
        parsed = (end != line);
        while (isspace((unsigned char)*end)) end++;
        if (parsed && *end == '\0' && v >= lo && v <= hi)
            return (int)v;
        printf("  Please enter a whole number from %d to %d.\n", lo, hi);
    }
}

static double read_double(const char *prompt, double lo, double hi)
{
    char line[LINE_LEN], *end;
    double v;
    int parsed;

    for (;;) {
        read_line(prompt, line, sizeof line);
        v = strtod(line, &end);
        parsed = (end != line);
        while (isspace((unsigned char)*end)) end++;
        if (parsed && *end == '\0' && v >= lo && v <= hi)
            return v;
        printf("  Please enter a number from %g to %g.\n", lo, hi);
    }
}

/* reads HH:MM; pressing Enter returns def (pass -1 when a value is required) */
static int read_time(const char *prompt, int def)
{
    char line[LINE_LEN], extra;
    int h, m;

    for (;;) {
        read_line(prompt, line, sizeof line);
        if (line[0] == '\0' && def >= 0)
            return def;
        if (sscanf(line, "%d:%d %c", &h, &m, &extra) == 2 &&
            h >= 0 && h < 24 && m >= 0 && m < 60)
            return h * 60 + m;
        printf("  Use 24-hour HH:MM, for example 06:30 or 22:00.\n");
    }
}

static int ask_yes_no(const char *prompt)
{
    char line[LINE_LEN];

    read_line(prompt, line, sizeof line);
    return line[0] == 'y' || line[0] == 'Y';
}

/* ============================ rule actions =============================== */

static void action_turn_on(Device *d)  { d->status = 1; }
static void action_turn_off(Device *d) { d->status = 0; }

/* actions are stored in the config file as small numbers */
static int action_code(ActionFn f)           { return f == action_turn_on ? 1 : 2; }
static ActionFn action_from_code(int code)   { return code == 1 ? action_turn_on : action_turn_off; }

/* ======================= devices and their schedules ===================== */

static Device *find_device(const Controller *c, int id)
{
    for (int i = 0; i < c->dev_count; i++)
        if (c->devices[i].id == id)
            return &c->devices[i];
    return NULL;
}

/* Appends a device (id 0 = give it the next free id). The returned pointer
 * is only valid until the next add_device(), because realloc() may move
 * the whole array - that is why rules refer to devices by id. */
static Device *add_device(Controller *c, int id, const char *name,
                          DeviceType type, double power)
{
    Device *d;

    if (c->dev_count == c->dev_cap) {                /* full: double the array */
        int new_cap = c->dev_cap ? c->dev_cap * 2 : 4;
        Device *bigger = realloc(c->devices, (size_t)new_cap * sizeof(Device));
        if (bigger == NULL) {
            printf("  Out of memory - device not added.\n");
            return NULL;
        }
        c->devices = bigger;
        c->dev_cap = new_cap;
    }
    d = &c->devices[c->dev_count++];
    memset(d, 0, sizeof *d);
    d->id = (id > 0) ? id : c->next_dev_id;
    if (d->id >= c->next_dev_id)
        c->next_dev_id = d->id + 1;
    strncpy(d->name, name, NAME_LEN - 1);
    d->type = type;
    d->power_rating = power;
    return d;
}

static void free_schedules(Device *d)
{
    Schedule *s = d->schedules;

    while (s != NULL) {
        Schedule *next = s->next;
        free(s);
        s = next;
    }
    d->schedules = NULL;
}

/* appends a window at the tail of the device's linked list */
static int add_schedule(Device *d, int start, int end)
{
    Schedule *node = malloc(sizeof *node);
    Schedule **pp;

    if (node == NULL) {
        printf("  Out of memory - schedule not added.\n");
        return 0;
    }
    node->start = start;
    node->end = end;
    node->next = NULL;
    pp = &d->schedules;
    while (*pp != NULL)
        pp = &(*pp)->next;
    *pp = node;
    return 1;
}

/* is 'minute' inside any of the device's windows? */
static int in_schedule(const Device *d, int minute)
{
    for (const Schedule *s = d->schedules; s != NULL; s = s->next) {
        if (s->start < s->end) {
            if (minute >= s->start && minute < s->end)
                return 1;
        } else if (minute >= s->start || minute < s->end) {   /* crosses midnight */
            return 1;
        }
    }
    return 0;
}

/* removes a device, frees its schedules and drops the rules that used it */
static void remove_device(Controller *c, int id)
{
    int idx = -1, kept = 0;

    for (int i = 0; i < c->dev_count; i++)
        if (c->devices[i].id == id)
            idx = i;
    if (idx < 0)
        return;

    free_schedules(&c->devices[idx]);
    memmove(&c->devices[idx], &c->devices[idx + 1],
            (size_t)(c->dev_count - idx - 1) * sizeof(Device));
    c->dev_count--;

    for (int i = 0; i < c->rule_count; i++)
        if (c->rules[i].device_id != id)
            c->rules[kept++] = c->rules[i];
    c->rule_count = kept;
}

/* =============================== rule engine ============================= */

static Rule *add_rule(Controller *c, int id, CondCode cond1, double thr1,
                      CondCode cond2, double thr2, int device_id, ActionFn action)
{
    Rule *r;

    if (c->rule_count == c->rule_cap) {
        int new_cap = c->rule_cap ? c->rule_cap * 2 : 8;
        Rule *bigger = realloc(c->rules, (size_t)new_cap * sizeof(Rule));
        if (bigger == NULL) {
            printf("  Out of memory - rule not added.\n");
            return NULL;
        }
        c->rules = bigger;
        c->rule_cap = new_cap;
    }
    r = &c->rules[c->rule_count++];
    r->id = (id > 0) ? id : c->next_rule_id;
    if (r->id >= c->next_rule_id)
        c->next_rule_id = r->id + 1;
    r->enabled = 1;
    r->cond[0] = cond1;  r->threshold[0] = thr1;
    r->cond[1] = cond2;  r->threshold[1] = thr2;
    r->device_id = device_id;
    r->action = action;
    return r;
}

/* decision construct: evaluates ONE condition code against the sensors */
static int check_condition(CondCode code, double value, const Sensors *s)
{
    switch (code) {
        case COND_NONE:     return 1;
        case COND_TEMP_GT:  return s->temperature > value;
        case COND_TEMP_LT:  return s->temperature < value;
        case COND_LIGHT_GT: return s->light > value;
        case COND_LIGHT_LT: return s->light < value;
        case COND_OCCUPIED: return s->occupied;
        case COND_VACANT:   return !s->occupied;
        default:            return 0;
    }
}

static int cond_needs_value(CondCode code)
{
    return code >= COND_TEMP_GT && code <= COND_LIGHT_LT;
}

static int rule_matches(const Rule *r, const Sensors *s)
{
    return r->enabled &&
           check_condition(r->cond[0], r->threshold[0], s) &&
           check_condition(r->cond[1], r->threshold[1], s);
}

/* ========================== simulated sensor input ======================= */

/* A smooth day profile; noise = 1 adds a little random jitter. */
static Sensors simulate_sensors(int minute, int noise)
{
    Sensors s;
    double h = minute / 60.0;

    /* temperature: 21 C at 05:00, rising to 33 C at 15:00, cooling overnight */
    if (h >= 5.0 && h < 15.0)
        s.temperature = 21.0 + 12.0 * (h - 5.0) / 10.0;
    else
        s.temperature = 33.0 - 12.0 * (h >= 15.0 ? h - 15.0 : h + 9.0) / 14.0;

    /* daylight: dark before 06:00 and after 18:00, 900 lux peak at noon */
    if (h >= 6.0 && h <= 18.0)
        s.light = (int)(900.0 * (h < 12.0 ? h - 6.0 : 18.0 - h) / 6.0);
    else
        s.light = 0;

    /* someone is home (and awake) in the morning and in the evening */
    s.occupied = (h >= 6.0 && h < 9.0) || (h >= 17.0 && h < 23.0);

    if (noise) {
        s.temperature += (rand() % 21 - 10) / 10.0;          /* +/- 1.0 C  */
        if (s.light > 0) {
            s.light += rand() % 101 - 50;                    /* +/- 50 lux */
            if (s.light < 0)
                s.light = 0;
        }
    }
    return s;
}

/* ============================ text and listings ========================== */

static void schedule_text(const Device *d, char *out, size_t size)
{
    size_t used = 0;

    out[0] = '\0';
    for (const Schedule *s = d->schedules; s != NULL && used < size; s = s->next) {
        int n = snprintf(out + used, size - used, "%s%s-%s",
                         used ? ", " : "", fmt_time(s->start), fmt_time(s->end));
        if (n < 0)
            break;
        used += (size_t)n;
    }
    if (used == 0)
        snprintf(out, size, "none");
}

static void source_text(const Device *d, char *out, size_t size)
{
    switch (d->source) {
        case SRC_MANUAL:   snprintf(out, size, "manual override"); break;
        case SRC_SCHEDULE: snprintf(out, size, "schedule");        break;
        case SRC_RULE:     snprintf(out, size, "rule #%d", d->last_rule); break;
        default:           snprintf(out, size, "-");               break;
    }
}

static void print_devices(const Controller *c)
{
    char sched[96], ctrl[24];

    if (c->dev_count == 0) {
        printf("\n  (no devices yet)\n");
        return;
    }
    printf("\n ID  %-20s %-10s %8s  %-5s %-16s %s\n",
           "Device", "Type", "Power", "State", "Controlled by", "Schedules");
    for (int i = 0; i < c->dev_count; i++) {
        const Device *d = &c->devices[i];
        schedule_text(d, sched, sizeof sched);
        source_text(d, ctrl, sizeof ctrl);
        printf("%3d  %-20s %-10s %6g W  %-5s %-16s %s\n",
               d->id, d->name, TYPE_NAME[d->type], d->power_rating,
               d->status ? "ON" : "OFF", ctrl, sched);
    }
}

static void describe_condition(CondCode code, double v, char *out, size_t size)
{
    switch (code) {
        case COND_TEMP_GT:  snprintf(out, size, "temp > %.1f C", v);    break;
        case COND_TEMP_LT:  snprintf(out, size, "temp < %.1f C", v);    break;
        case COND_LIGHT_GT: snprintf(out, size, "light > %.0f lux", v); break;
        case COND_LIGHT_LT: snprintf(out, size, "light < %.0f lux", v); break;
        case COND_OCCUPIED: snprintf(out, size, "someone is home");     break;
        case COND_VACANT:   snprintf(out, size, "nobody is home");      break;
        default:            out[0] = '\0';                              break;
    }
}

static void print_rules(const Controller *c)
{
    if (c->rule_count == 0) {
        printf("\n  (no rules defined)\n");
        return;
    }
    printf("\n ID  On   Rule (later rules override earlier ones)\n");
    for (int i = 0; i < c->rule_count; i++) {
        const Rule *r = &c->rules[i];
        const Device *d = find_device(c, r->device_id);
        char a[48], b[48];

        describe_condition(r->cond[0], r->threshold[0], a, sizeof a);
        describe_condition(r->cond[1], r->threshold[1], b, sizeof b);
        printf("%3d  %-3s  IF %s%s%s THEN turn %s %s\n",
               r->id, r->enabled ? "yes" : "no", a, b[0] ? " AND " : "", b,
               r->action == action_turn_on ? "ON" : "OFF",
               d ? d->name : "(missing device)");
    }
}

static void print_change(const Controller *c, const Device *d)
{
    printf("   [%s] %-20s -> %-3s  (", fmt_time(c->clock), d->name, d->status ? "ON" : "OFF");
    if (d->source == SRC_MANUAL)        printf("manual override");
    else if (d->source == SRC_SCHEDULE) printf("schedule");
    else if (d->source == SRC_RULE)     printf("rule #%d", d->last_rule);
    else                                printf("no active schedule or rule");
    printf(")\n");
}

/* ========================= one control cycle ============================= */

/* Decides the state of one device: override > schedule > rules. */
static void evaluate_device(Controller *c, Device *d, int verbose)
{
    int before = d->status;

    if (d->override_on) {                                /* 1. manual override */
        d->status = d->override_state;
        d->source = SRC_MANUAL;
    } else if (in_schedule(d, c->clock)) {               /* 2. user schedule   */
        d->status = 1;
        d->source = SRC_SCHEDULE;
    } else {                                             /* 3. sensor rules    */
        int fired = 0;

        for (int i = 0; i < c->rule_count; i++) {
            const Rule *r = &c->rules[i];
            if (r->device_id == d->id && rule_matches(r, &c->sensors)) {
                r->action(d);                            /* call via the pointer */
                d->source = SRC_RULE;
                d->last_rule = r->id;
                fired = 1;
            }
        }
        if (!fired && (d->source == SRC_SCHEDULE || d->source == SRC_MANUAL)) {
            d->status = 0;                /* schedule ended / override released */
            d->source = SRC_NONE;
        }
    }
    if (verbose && d->status != before)
        print_change(c, d);
}

/* One periodic evaluation: read sensors, update every device, count energy
 * for 'step' minutes, then advance the clock. */
static void run_cycle(Controller *c, int step, int verbose)
{
    if (c->auto_sensors)
        c->sensors = simulate_sensors(c->clock, c->noise);

    if (verbose)
        printf("%s  %5.1f C  %4d lux  %s\n", fmt_time(c->clock),
               c->sensors.temperature, c->sensors.light,
               c->sensors.occupied ? "occupied" : "vacant");

    for (int i = 0; i < c->dev_count; i++) {
        Device *d = &c->devices[i];
        evaluate_device(c, d, verbose);
        if (d->status)
            d->on_minutes += step;
    }
    c->clock = (c->clock + step) % MINUTES_PER_DAY;
    c->total_minutes += step;
}

/* ======================= energy report and what-if ======================= */

static void reset_energy(Controller *c)
{
    for (int i = 0; i < c->dev_count; i++)
        c->devices[i].on_minutes = 0.0;
    c->total_minutes = 0;
}

static void energy_report(const Controller *c)
{
    double total = 0.0;

    if (c->total_minutes == 0) {
        printf("\n  Nothing simulated yet - run a simulation first (option 11).\n");
        return;
    }
    printf("\n=== ENERGY USAGE REPORT (simulated %.1f hours) ===\n", c->total_minutes / 60.0);
    printf(" ID  %-20s %8s %9s %11s\n", "Device", "Power", "ON hours", "Energy kWh");
    for (int i = 0; i < c->dev_count; i++) {
        const Device *d = &c->devices[i];
        double hours = d->on_minutes / 60.0;
        double kwh = d->power_rating * hours / 1000.0;          /* W * h / 1000 */

        total += kwh;
        printf("%3d  %-20s %6g W %9.2f %11.3f\n", d->id, d->name, d->power_rating, hours, kwh);
    }
    printf("\n  TOTAL ENERGY   : %.3f kWh\n", total);
    printf("  ESTIMATED COST : Rs %.2f  (at Rs %.2f per kWh)\n", total * TARIFF_PER_KWH, TARIFF_PER_KWH);
}

/* Runs one full simulated day on a PRIVATE COPY of the devices and rules,
 * so the real controller is untouched. Temperature rules of AC and heater
 * devices are moved by thermostat_shift degrees. kwh_out[i] receives the
 * energy of device i. Returns 1 on success. */
static int simulate_day(const Controller *c, double thermostat_shift, double *kwh_out)
{
    Controller sim = *c;                      /* shallow copy of the controller */

    sim.devices = malloc((size_t)(c->dev_count > 0 ? c->dev_count : 1) * sizeof(Device));
    sim.rules   = malloc((size_t)(c->rule_count > 0 ? c->rule_count : 1) * sizeof(Rule));
    if (sim.devices == NULL || sim.rules == NULL) {
        free(sim.devices);
        free(sim.rules);
        return 0;
    }
    memcpy(sim.devices, c->devices, (size_t)c->dev_count * sizeof(Device));
    if (c->rule_count > 0)
        memcpy(sim.rules, c->rules, (size_t)c->rule_count * sizeof(Rule));

    for (int i = 0; i < sim.dev_count; i++) {             /* clean start of day */
        Device *d = &sim.devices[i];
        d->status = 0;
        d->override_on = 0;
        d->source = SRC_NONE;
        d->last_rule = 0;
        d->on_minutes = 0.0;
    }
    for (int i = 0; i < sim.rule_count; i++) {            /* move the thermostat */
        Rule *r = &sim.rules[i];
        const Device *target = find_device(&sim, r->device_id);

        if (target == NULL || (target->type != DEV_AC && target->type != DEV_HEATER))
            continue;
        for (int k = 0; k < 2; k++)
            if (r->cond[k] == COND_TEMP_GT || r->cond[k] == COND_TEMP_LT)
                r->threshold[k] += thermostat_shift;
    }

    sim.auto_sensors = 1;
    sim.noise = 0;                           /* same weather for every run */
    sim.clock = 0;
    sim.total_minutes = 0;
    for (int t = 0; t < MINUTES_PER_DAY; t += WHATIF_STEP)
        run_cycle(&sim, WHATIF_STEP, 0);

    for (int i = 0; i < sim.dev_count; i++)
        kwh_out[i] = sim.devices[i].power_rating * sim.devices[i].on_minutes / 60000.0;

    free(sim.devices);
    free(sim.rules);
    return 1;
}

static void menu_what_if(const Controller *c)
{
    double shift, tot_base = 0.0, tot_alt = 0.0;
    double *base, *alt;

    if (c->dev_count == 0) {
        printf("  Add some devices first.\n");
        return;
    }
    shift = read_double("Thermostat change in C (for example 2 or -2): ", -10.0, 10.0);

    base = calloc((size_t)c->dev_count, sizeof(double));
    alt  = calloc((size_t)c->dev_count, sizeof(double));
    if (base == NULL || alt == NULL ||
        !simulate_day(c, 0.0, base) || !simulate_day(c, shift, alt)) {
        printf("  Could not run the scenario (out of memory).\n");
        free(base);
        free(alt);
        return;
    }

    printf("\n=== WHAT-IF: thermostat %+.1f C (one simulated day) ===\n", shift);
    printf("Moves the temperature rules of AC and heater devices. Manual\n");
    printf("overrides are ignored; weather and occupancy follow the day profile.\n\n");
    printf("%-22s %10s %10s %10s\n", "Device", "Now kWh", "What-if", "Change");
    for (int i = 0; i < c->dev_count; i++) {
        printf("%-22s %10.3f %10.3f %+10.3f\n", c->devices[i].name, base[i], alt[i], alt[i] - base[i]);
        tot_base += base[i];
        tot_alt += alt[i];
    }
    printf("%-22s %10.3f %10.3f %+10.3f\n", "TOTAL", tot_base, tot_alt, tot_alt - tot_base);
    if (tot_base > 0.0)
        printf("\nChange in daily energy : %+.1f%%\n", 100.0 * (tot_alt - tot_base) / tot_base);
    printf("Estimated cost, 30 days: Rs %.0f -> Rs %.0f\n",
           tot_base * 30.0 * TARIFF_PER_KWH, tot_alt * 30.0 * TARIFF_PER_KWH);

    free(base);
    free(alt);
}

/* ============================ configuration file ========================= */
/*
 *  smarthome.cfg is plain text, one record per line:
 *    DEVICE <id> <type 0-4> <watts> <name...>
 *    SCHED  <device id> <start minute> <end minute>
 *    RULE   <id> <enabled> <cond1> <thr1> <cond2> <thr2> <device id> <action 1=ON 2=OFF>
 */

static void init_controller(Controller *c)
{
    memset(c, 0, sizeof *c);
    c->next_dev_id = 1;
    c->next_rule_id = 1;
    c->auto_sensors = 1;
    c->clock = 6 * 60;                       /* simulation starts at 06:00 */
}

/* frees every schedule list and both dynamic arrays */
static void clear_config(Controller *c)
{
    for (int i = 0; i < c->dev_count; i++)
        free_schedules(&c->devices[i]);
    free(c->devices);
    free(c->rules);
    c->devices = NULL;
    c->rules = NULL;
    c->dev_count = c->dev_cap = 0;
    c->rule_count = c->rule_cap = 0;
    c->next_dev_id = c->next_rule_id = 1;
}

static int save_config(const Controller *c, const char *path)
{
    FILE *fp = fopen(path, "w");

    if (fp == NULL)
        return 0;
    fprintf(fp, "# Smart Home Controller configuration\n");
    for (int i = 0; i < c->dev_count; i++) {
        const Device *d = &c->devices[i];
        fprintf(fp, "DEVICE %d %d %g %s\n", d->id, (int)d->type, d->power_rating, d->name);
        for (const Schedule *s = d->schedules; s != NULL; s = s->next)
            fprintf(fp, "SCHED %d %d %d\n", d->id, s->start, s->end);
    }
    for (int i = 0; i < c->rule_count; i++) {
        const Rule *r = &c->rules[i];
        fprintf(fp, "RULE %d %d %d %g %d %g %d %d\n", r->id, r->enabled,
                (int)r->cond[0], r->threshold[0], (int)r->cond[1], r->threshold[1],
                r->device_id, action_code(r->action));
    }
    fclose(fp);
    return 1;
}

/* returns 0 when the file does not exist (the controller is left untouched) */
static int load_config(Controller *c, const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_LEN * 2];

    if (fp == NULL)
        return 0;
    clear_config(c);

    while (fgets(line, sizeof line, fp) != NULL) {
        int id, type, start, end, enabled, cond1, cond2, dev, act;
        double power, thr1, thr2;
        char name[NAME_LEN];

        if (sscanf(line, "DEVICE %d %d %lf %31[^\n]", &id, &type, &power, name) == 4) {
            if (id > 0 && type >= 0 && type < DEV_TYPE_COUNT && find_device(c, id) == NULL)
                add_device(c, id, name, (DeviceType)type, power);
        } else if (sscanf(line, "SCHED %d %d %d", &id, &start, &end) == 3) {
            Device *d = find_device(c, id);
            if (d != NULL && start >= 0 && start < MINUTES_PER_DAY &&
                end >= 0 && end < MINUTES_PER_DAY && start != end)
                add_schedule(d, start, end);
        } else if (sscanf(line, "RULE %d %d %d %lf %d %lf %d %d", &id, &enabled,
                          &cond1, &thr1, &cond2, &thr2, &dev, &act) == 8) {
            if (cond1 > COND_NONE && cond1 < COND_COUNT &&
                cond2 >= COND_NONE && cond2 < COND_COUNT && find_device(c, dev) != NULL) {
                Rule *r = add_rule(c, id, (CondCode)cond1, thr1, (CondCode)cond2, thr2,
                                   dev, action_from_code(act));
                if (r != NULL)
                    r->enabled = enabled;
            }
        }
    }
    fclose(fp);
    return 1;
}

/* a ready-made home used the first time the program runs */
static void load_demo(Controller *c)
{
    Device *d;                               /* only used right after each add */

    add_device(c, 0, "Living Room Light", DEV_LIGHT, 12);        /* id 1 */
    add_device(c, 0, "Living Room AC",    DEV_AC,    1500);      /* id 2 */
    add_device(c, 0, "Ceiling Fan",       DEV_FAN,   75);        /* id 3 */
    d = add_device(c, 0, "Geyser",        DEV_HEATER, 2000);     /* id 4 */
    if (d) add_schedule(d, 6 * 60, 7 * 60);                      /* 06:00-07:00 */
    d = add_device(c, 0, "Porch Light",   DEV_LIGHT, 9);         /* id 5 */
    if (d) add_schedule(d, 18 * 60 + 30, 6 * 60);                /* 18:30-06:00 */

    /* AC: hot and someone home -> ON; cool or nobody home -> OFF */
    add_rule(c, 0, COND_TEMP_GT,  28,  COND_OCCUPIED, 0, 2, action_turn_on);
    add_rule(c, 0, COND_TEMP_LT,  25,  COND_NONE,     0, 2, action_turn_off);
    add_rule(c, 0, COND_VACANT,   0,   COND_NONE,     0, 2, action_turn_off);
    /* Light: dark and someone home -> ON; bright or nobody home -> OFF */
    add_rule(c, 0, COND_LIGHT_LT, 200, COND_OCCUPIED, 0, 1, action_turn_on);
    add_rule(c, 0, COND_LIGHT_GT, 350, COND_NONE,     0, 1, action_turn_off);
    add_rule(c, 0, COND_VACANT,   0,   COND_NONE,     0, 1, action_turn_off);
    /* Fan: warm and someone home -> ON; cool or nobody home -> OFF */
    add_rule(c, 0, COND_TEMP_GT,  26,  COND_OCCUPIED, 0, 3, action_turn_on);
    add_rule(c, 0, COND_TEMP_LT,  24,  COND_NONE,     0, 3, action_turn_off);
    add_rule(c, 0, COND_VACANT,   0,   COND_NONE,     0, 3, action_turn_off);
}

/* ================================= menus ================================= */

/* shows the devices and asks for one id; NULL if there is no such device */
static Device *ask_device(Controller *c, const char *prompt)
{
    int id;
    Device *d;

    if (c->dev_count == 0) {
        printf("  There are no devices yet.\n");
        return NULL;
    }
    print_devices(c);
    id = read_int(prompt, 1, 1000000);
    d = find_device(c, id);
    if (d == NULL)
        printf("  No device with id %d.\n", id);
    return d;
}

static void menu_add_device(Controller *c)
{
    char name[LINE_LEN];
    int type;
    double power;
    Device *d;

    read_line("Device name: ", name, sizeof name);
    if (name[0] == '\0') {
        printf("  The name cannot be empty.\n");
        return;
    }
    for (int t = 0; t < DEV_TYPE_COUNT; t++)
        printf("  %d = %s\n", t + 1, TYPE_NAME[t]);
    type = read_int("Device type: ", 1, DEV_TYPE_COUNT) - 1;
    power = read_double("Power rating in watts: ", 0.1, 100000.0);

    d = add_device(c, 0, name, (DeviceType)type, power);
    if (d != NULL)
        printf("  Added device #%d '%s'.\n", d->id, d->name);
}

static void menu_remove_device(Controller *c)
{
    Device *d = ask_device(c, "Device id to remove: ");
    char prompt[LINE_LEN];
    int id;

    if (d == NULL)
        return;
    id = d->id;
    snprintf(prompt, sizeof prompt, "Remove '%s' and the rules that use it? (y/n): ", d->name);
    if (ask_yes_no(prompt)) {
        remove_device(c, id);
        printf("  Device removed.\n");
    }
}

static void menu_add_schedule(Controller *c)
{
    Device *d = ask_device(c, "Device id: ");
    int start, end;

    if (d == NULL)
        return;
    start = read_time("Start time (HH:MM): ", -1);
    end   = read_time("End time   (HH:MM): ", -1);
    if (start == end) {
        printf("  Start and end must be different.\n");
        return;
    }
    if (add_schedule(d, start, end))
        printf("  Schedule %s-%s added to '%s'%s.\n", fmt_time(start), fmt_time(end),
               d->name, start > end ? " (runs past midnight)" : "");
}

static void menu_clear_schedules(Controller *c)
{
    Device *d = ask_device(c, "Device id: ");

    if (d == NULL)
        return;
    free_schedules(d);
    printf("  All schedules of '%s' removed.\n", d->name);
}

static void menu_override(Controller *c)
{
    Device *d = ask_device(c, "Device id: ");
    int choice;

    if (d == NULL)
        return;
    choice = read_int("1 = force ON, 2 = force OFF, 3 = release override: ", 1, 3);
    if (choice == 3) {
        d->override_on = 0;
        printf("  Override released - '%s' is under automatic control again.\n", d->name);
        evaluate_device(c, d, 1);
    } else {
        d->override_on = 1;
        d->override_state = (choice == 1);
        d->status = d->override_state;
        d->source = SRC_MANUAL;
        printf("  '%s' is forced %s until you release the override.\n",
               d->name, d->status ? "ON" : "OFF");
    }
}

static void menu_sensors(Controller *c)
{
    printf("\nCurrent readings: %.1f C, %d lux, %s  (source: %s)\n",
           c->sensors.temperature, c->sensors.light,
           c->sensors.occupied ? "someone home" : "nobody home",
           c->auto_sensors ? "simulated day profile" : "manual readings");

    if (read_int("1 = type in readings, 2 = simulated day profile: ", 1, 2) == 1) {
        c->sensors.temperature = read_double("Temperature in C (-20 to 60): ", -20.0, 60.0);
        c->sensors.light = read_int("Ambient light in lux (0 to 1000): ", 0, 1000);
        c->sensors.occupied = read_int("Occupancy (1 = someone home, 0 = nobody): ", 0, 1);
        c->auto_sensors = 0;
        printf("  Manual readings saved; they stay fixed while the simulation runs.\n");
    } else {
        c->auto_sensors = 1;
        c->noise = ask_yes_no("Add small random noise to the readings? (y/n): ");
        printf("  Sensors now follow the simulated day profile.\n");
    }
}

static void menu_add_rule(Controller *c)
{
    CondCode code[2] = { COND_NONE, COND_NONE };
    double value[2] = { 0.0, 0.0 };
    Device *d;
    Rule *r;
    int act;

    if (c->dev_count == 0) {
        printf("  Add a device first.\n");
        return;
    }
    printf("\nConditions:\n");
    for (int k = 1; k < COND_COUNT; k++)
        printf("  %d = %s\n", k, COND_TEXT[k]);

    for (int slot = 0; slot < 2; slot++) {
        char prompt[64];

        snprintf(prompt, sizeof prompt, "Condition %d (%s): ", slot + 1,
                 slot == 0 ? "required" : "0 = none");
        code[slot] = (CondCode)read_int(prompt, slot == 0 ? 1 : 0, COND_COUNT - 1);
        if (code[slot] == COND_NONE)
            break;
        if (cond_needs_value(code[slot]))
            value[slot] = read_double("  Threshold value: ", -1000.0, 100000.0);
    }

    d = ask_device(c, "Target device id: ");
    if (d == NULL)
        return;
    act = read_int("Action (1 = turn ON, 2 = turn OFF): ", 1, 2);
    r = add_rule(c, 0, code[0], value[0], code[1], value[1], d->id,
                 act == 1 ? action_turn_on : action_turn_off);
    if (r != NULL)
        printf("  Rule #%d added.\n", r->id);
}

static void menu_edit_rule(Controller *c)
{
    int id, idx = -1, act;

    if (c->rule_count == 0) {
        printf("  There are no rules yet.\n");
        return;
    }
    print_rules(c);
    id = read_int("Rule id: ", 1, 1000000);
    for (int i = 0; i < c->rule_count; i++)
        if (c->rules[i].id == id)
            idx = i;
    if (idx < 0) {
        printf("  No rule with id %d.\n", id);
        return;
    }
    act = read_int("1 = enable / disable, 2 = delete: ", 1, 2);
    if (act == 1) {
        c->rules[idx].enabled = !c->rules[idx].enabled;
        printf("  Rule #%d is now %s.\n", id, c->rules[idx].enabled ? "enabled" : "disabled");
    } else {
        memmove(&c->rules[idx], &c->rules[idx + 1],
                (size_t)(c->rule_count - idx - 1) * sizeof(Rule));
        c->rule_count--;
        printf("  Rule #%d deleted.\n", id);
    }
}

static void menu_run(Controller *c)
{
    char prompt[LINE_LEN];
    int cycles, step;

    if (c->dev_count == 0) {
        printf("  Add some devices first.\n");
        return;
    }
    snprintf(prompt, sizeof prompt, "Start time HH:MM (Enter = %s): ", fmt_time(c->clock));
    c->clock = read_time(prompt, c->clock);
    cycles = read_int("Number of cycles (1-1000, 96 cycles of 15 min = one day): ", 1, 1000);
    step = read_int("Minutes per cycle (1-240): ", 1, 240);
    if (ask_yes_no("Reset the energy counters first? (y/n): "))
        reset_energy(c);

    printf("\n--- simulation start ---\n");
    for (int i = 0; i < cycles; i++)                 /* periodic evaluation loop */
        run_cycle(c, step, 1);
    printf("--- simulation end (clock is now %s) ---\n", fmt_time(c->clock));
}

static void print_menu(const Controller *c)
{
    printf("\n==================== SMART HOME CONTROLLER ====================\n");
    printf(" %s | %.1f C | %d lux | %s | sensors: %s\n", fmt_time(c->clock),
           c->sensors.temperature, c->sensors.light,
           c->sensors.occupied ? "occupied" : "vacant",
           c->auto_sensors ? "simulated" : "manual");
    printf("---------------------------------------------------------------\n");
    printf("  %-26s%s\n", " 1. Add device",          " 8. Add rule");
    printf("  %-26s%s\n", " 2. Remove device",       " 9. Show rules");
    printf("  %-26s%s\n", " 3. Show devices",        "10. Enable/disable/delete rule");
    printf("  %-26s%s\n", " 4. Add schedule",        "11. Run simulation");
    printf("  %-26s%s\n", " 5. Clear schedules",     "12. Energy usage report");
    printf("  %-26s%s\n", " 6. Manual override",     "13. What-if: move thermostat");
    printf("  %-26s%s\n", " 7. Sensors (set / mode)", "14. Save configuration");
    printf("  %-26s\n",   " 0. Exit");
}

/* ================================= main ================================== */

int main(void)
{
    Controller c;
    int choice;

    init_controller(&c);
    srand((unsigned)time(NULL));

    if (load_config(&c, CONFIG_FILE)) {
        printf("Loaded saved configuration from %s.\n", CONFIG_FILE);
    } else {
        load_demo(&c);
        printf("No saved configuration found - loaded a demo home (%d devices, %d rules).\n",
               c.dev_count, c.rule_count);
    }
    c.sensors = simulate_sensors(c.clock, 0);

    do {
        print_menu(&c);
        choice = read_int("Choose an option: ", 0, 14);
        switch (choice) {
            case 1:  menu_add_device(&c);      break;
            case 2:  menu_remove_device(&c);   break;
            case 3:  print_devices(&c);        break;
            case 4:  menu_add_schedule(&c);    break;
            case 5:  menu_clear_schedules(&c); break;
            case 6:  menu_override(&c);        break;
            case 7:  menu_sensors(&c);         break;
            case 8:  menu_add_rule(&c);        break;
            case 9:  print_rules(&c);          break;
            case 10: menu_edit_rule(&c);       break;
            case 11: menu_run(&c);             break;
            case 12: energy_report(&c);        break;
            case 13: menu_what_if(&c);         break;
            case 14:
                if (save_config(&c, CONFIG_FILE))
                    printf("  Saved to %s.\n", CONFIG_FILE);
                else
                    printf("  Could not write %s.\n", CONFIG_FILE);
                break;
            default: break;
        }
    } while (choice != 0);

    if (ask_yes_no("Save the configuration before leaving? (y/n): ")) {
        if (save_config(&c, CONFIG_FILE))
            printf("  Saved to %s.\n", CONFIG_FILE);
        else
            printf("  Could not write %s.\n", CONFIG_FILE);
    }
    clear_config(&c);                        /* give all heap memory back */
    printf("Goodbye!\n");
    return 0;
}

