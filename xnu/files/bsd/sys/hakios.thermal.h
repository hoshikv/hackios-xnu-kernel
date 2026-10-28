/*
 * The thermal governor of the POCO F6 / Redmi Turbo 3 (peridot): which of the SoC's temperature
 * sensors there are, where each sits, how warm it may get, and the decision made every tick -- how
 * fast each group of processor cores may run, and how hot iOS should be told the phone is.
 *
 * Only the decision is here, as inline functions with no kernel in them: bsd/dev/hakios.thermal.c
 * reads the sensors and carries the decision out, and tools/thermal-test compiles this same header
 * on the computer and drives it through heat-ups, cool-downs, failing sensors and a hot battery.
 *
 * The sensors: Peridot has three TSENS blocks (dt: tsens@c228000 with its readings at 0xc222000, 14
 * sensors; tsens@c229000 at 0xc223000, 16 sensors; tsens@c22a000 at 0xc224000, 5 sensors). Which
 * sensor is where, and the limits, come from the phone's own device tree (its thermal-zones: the
 * "-step" zones are where Xiaomi's kernel starts to slow the cores, the "-usr" zones where it shuts
 * the phone down):
 *
 *   tsens0  0 aoss-0      1, 2 cpuss-0, cpuss-1 (shared)
 *           3..10 cpu-1-0-0..cpu-1-3-1 (the four big cores, two sensors each)
 *           11..13 cpu-2-0-0..cpu-2-0-2 (the prime core)
 *   tsens1  0 aoss-1      1..3 cpu-0-0-0..cpu-0-2-0 (the three small cores)
 *           4..7 nsphvx-0/1, nsphmx-0/1     8..11 gpuss-0..3
 *           12 video     13 ddr     14, 15 camera-0, camera-1
 *   tsens2  0 aoss-2      1..4 mdmss-0..3
 *
 * The phone's device tree also names 19 PMIC/ADC temperature zones (10 PMIC die temperatures and 9
 * board thermistors) and 27 non-temperature zones (16 modem QMI, 3 PMXR2230 BCL, 7 PM7550BA
 * BCL/current and 1 SOCD). They are all in the table so every thermal zone is accounted for; only
 * TSENS has a reader in this kernel so far, and the non-temperature zones never enter the governor.
 *
 * The CPU zones follow the hardware's frequency groups: cpu-0-* is the small group (domain 0),
 * cpu-1-* the big group (domain 1), cpu-2-* the prime core (domain 2); cpuss-* are shared by all.
 *
 * Xiaomi's limits are 108..112 C to start slowing the cores and 115 C to shut down. Ours are lower on
 * purpose: this kernel runs every core it uses at the top of its frequency table all the time
 * (cpufreq=1), with no other governor -- so slowing starts at 95 C, well before the hardware's own
 * protection would have to act, and 115 C is the emergency stop as in Xiaomi's.
 *
 * Temperatures are kept in tenths of a degree Celsius, as TSENS reports them.
 */
#ifndef _SYS_HAKIOS_THERMAL_H_
#define _SYS_HAKIOS_THERMAL_H_

#include <stdbool.h>
#include <stdint.h>

#define THERMAL_DOMAINS         3               /* the cores' frequency groups: small, big, prime (hakios.cpufreq.c) */
#define THERMAL_ALL_CPUS        (-2)            /* a zone shared by every group of cores */
#define THERMAL_NO_CPU          (-1)            /* a zone of something else: the GPU, the DSPs, the memory */
#define THERMAL_INVALID         (-32768)        /* no reading from that sensor this tick */

/* iOS's own levels (OSThermalPressureLevel, <libkern/OSThermalNotification.h>): what thermald posts */
#define THERMAL_NOMINAL         0               /* nothing to worry about */
#define THERMAL_MODERATE        1               /* within 10 C of slowing down */
#define THERMAL_HEAVY           2               /* the cores are being slowed */
#define THERMAL_TRAPPING        3               /* within 5 C of the emergency stop */
#define THERMAL_SLEEPING        4               /* past it: everything at the lowest speed */

#define THERMAL_TICK_MS         250             /* how often the sensors are read */
#define THERMAL_HOT_STEP        50              /* 5 C past the limit: slow down three rows a tick, not one */
#define THERMAL_COOL_MARGIN     50              /* 5 C under the limit before speeding up again */
#define THERMAL_COOL_TICKS      4               /* ... and for this many ticks in a row (one second) */
#define THERMAL_EMERGENCY_TICKS 8               /* two seconds past the stop: the run ends (a panic) */
#define THERMAL_BLIND_TICKS     20              /* five seconds with no CPU reading: slow to half and say so */

#define THERMAL_BATTERY_WARM    450             /* the battery's temperature: 45 C, at least MODERATE */
#define THERMAL_BATTERY_HOT     500             /* 50 C: the cores held at most at half speed, HEAVY */
#define THERMAL_BATTERY_DANGER  580             /* 58 C: TRAPPING, the cores at the lowest speed */

enum thermal_source {
	THERMAL_SOURCE_TSENS = 0,
	THERMAL_SOURCE_ADC,
	THERMAL_SOURCE_PMIC,
	THERMAL_SOURCE_QMI,
	THERMAL_SOURCE_BCL,
	THERMAL_SOURCE_SOC,
};

struct thermal_zone {
	const char *name;                       /* as the device tree calls it */
	uint8_t block;                          /* TSENS block 0..2 */
	uint16_t sensor;                        /* TSENS sensor, or the provider's channel */
	int8_t domain;                          /* 0..2, THERMAL_ALL_CPUS or THERMAL_NO_CPU */
	int16_t passive;                        /* start slowing at this, tenths of C */
	int16_t critical;                       /* the emergency stop */
	uint8_t source;                         /* enum thermal_source; absent means TSENS */
};

static inline bool
thermal_source_is_temperature(uint8_t source)
{
	return source == THERMAL_SOURCE_TSENS || source == THERMAL_SOURCE_ADC || source == THERMAL_SOURCE_PMIC;
}

static const struct thermal_zone hakios_thermal_zones[] = {
	/* Peridot TSENS0: CPU complex and always-on sensors. */
	{ "aoss-0", 0, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_TSENS },
	{ "cpuss-0", 0, 1, THERMAL_ALL_CPUS, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpuss-1", 0, 2, THERMAL_ALL_CPUS, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-1-0-0", 0, 3, 1, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-1-0-1", 0, 4, 1, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-1-1-0", 0, 5, 1, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-1-1-1", 0, 6, 1, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-1-2-0", 0, 7, 1, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-1-2-1", 0, 8, 1, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-1-3-0", 0, 9, 1, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-1-3-1", 0, 10, 1, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-2-0-0", 0, 11, 2, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-2-0-1", 0, 12, 2, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-2-0-2", 0, 13, 2, 950, 1150, THERMAL_SOURCE_TSENS },
	/* Peridot TSENS1: CPU, neural, GPU, video, memory and camera zones. */
	{ "aoss-1", 1, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-0-0-0", 1, 1, 0, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-0-1-0", 1, 2, 0, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "cpu-0-2-0", 1, 3, 0, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "nsphvx-0", 1, 4, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_TSENS },
	{ "nsphvx-1", 1, 5, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_TSENS },
	{ "nsphmx-0", 1, 6, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_TSENS },
	{ "nsphmx-1", 1, 7, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_TSENS },
	{ "gpuss-0", 1, 8, THERMAL_NO_CPU, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "gpuss-1", 1, 9, THERMAL_NO_CPU, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "gpuss-2", 1, 10, THERMAL_NO_CPU, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "gpuss-3", 1, 11, THERMAL_NO_CPU, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "video", 1, 12, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_TSENS },
	{ "ddr", 1, 13, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_TSENS },
	{ "camera-0", 1, 14, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_TSENS },
	{ "camera-1", 1, 15, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_TSENS },
	/* Peridot TSENS2: modem subsystem. */
	{ "aoss-2", 2, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_TSENS },
	{ "mdmss-0", 2, 1, THERMAL_NO_CPU, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "mdmss-1", 2, 2, THERMAL_NO_CPU, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "mdmss-2", 2, 3, THERMAL_NO_CPU, 950, 1150, THERMAL_SOURCE_TSENS },
	{ "mdmss-3", 2, 4, THERMAL_NO_CPU, 950, 1150, THERMAL_SOURCE_TSENS },
	/* Peridot PMIC die temperatures (dt: pmic temp-alarm zones); readers not wired yet. */
	{ "pmxr2230_tz", 0, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_PMIC },
	{ "pm8550vs_c_tz", 0, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_PMIC },
	{ "pm8550vs_d_tz", 0, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_PMIC },
	{ "pm8550vs_e_tz", 0, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_PMIC },
	{ "pm8550vs_g_tz", 0, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_PMIC },
	{ "pm8550ve_tz", 0, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_PMIC },
	{ "pmr735a_tz", 0, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_PMIC },
	{ "pm8010m_tz", 0, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_PMIC },
	{ "pm8010n_tz", 0, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_PMIC },
	{ "pm7550ba_tz", 0, 0, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_PMIC },
	/* Peridot board thermistors (dt: ADC5 gen3, PMK8550); readers not wired yet. */
	{ "xo-therm", 0, 0x44, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_ADC },
	{ "cpu_therm", 0, 0x145, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_ADC },
	{ "quiet_therm", 0, 0x147, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_ADC },
	{ "pa_therm1", 0, 0x146, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_ADC },
	{ "pa_therm0", 0, 0x144, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_ADC },
	{ "flash_therm", 0, 0x148, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_ADC },
	{ "wifi_therm", 0, 0x74c, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_ADC },
	{ "charger_therm0", 0, 0x749, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_ADC },
	{ "conn_therm", 0, 0x747, THERMAL_NO_CPU, 1000, 1150, THERMAL_SOURCE_ADC },
	/* Peridot modem QMI zones (dt: qmi-ts-sensors); no temperature reader here. */
	{ "sdr0_pa", 0, 0x48, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "sdr0", 0, 0x26, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "mmw0", 0, 0x2e, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "mmw1", 0, 0x2f, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "mmw2", 0, 0x30, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "mmw3", 0, 0x31, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "mmw_pa1", 0, 0x1a, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "mmw_pa2", 0, 0x1b, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "mmw_pa3", 0, 0x1c, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "mmw_ific0", 0, 0x32, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "sub1_modem_cfg", 0, 0x33, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "sub1_lte_cc", 0, 0x34, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "sub1_mcg_fr1_cc", 0, 0x35, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "sub1_mcg_fr2_cc", 0, 0x36, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "sub1_scg_fr1_cc", 0, 0x37, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	{ "sub1_scg_fr2_cc", 0, 0x38, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_QMI },
	/* Peridot PMXR2230 BCL zones (dt: pmxr2230-bcl); not temperatures. */
	{ "pmxr2230-bcl-lvl0", 0, 0x05, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_BCL },
	{ "pmxr2230-bcl-lvl1", 0, 0x06, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_BCL },
	{ "pmxr2230-bcl-lvl2", 0, 0x07, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_BCL },
	/* Peridot PM7550BA BCL/current zones (dt: pm7550ba-bcl, pm7550ba-ibat); not temperatures. */
	{ "pm7550ba-ibat-lvl0", 0, 0x00, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_BCL },
	{ "pm7550ba-ibat-lvl1", 0, 0x01, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_BCL },
	{ "pm7550ba-bcl-lvl0", 0, 0x05, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_BCL },
	{ "pm7550ba-bcl-lvl1", 0, 0x06, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_BCL },
	{ "pm7550ba-bcl-lvl2", 0, 0x07, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_BCL },
	{ "pm7550ba-2s-ibat-0", 0, 0x08, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_BCL },
	{ "pm7550ba-2s-ibat-1", 0, 0x09, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_BCL },
	/* Peridot SOCD zone (dt: socd); not a temperature. */
	{ "socd", 0, 0, THERMAL_NO_CPU, 0, 0, THERMAL_SOURCE_SOC },
};
#define THERMAL_ZONES   ((int)(sizeof(hakios_thermal_zones) / sizeof(hakios_thermal_zones[0])))

/* What the governor keeps from one tick to the next. */
struct thermal_gov {
	int top[THERMAL_DOMAINS];               /* the highest row of each group's frequency table; -1: not ours */
	int cap[THERMAL_DOMAINS];               /* the row each group may run at now */
	int cool[THERMAL_DOMAINS];              /* ticks in a row it has been cool enough to speed up */
	int level;                              /* THERMAL_NOMINAL..THERMAL_SLEEPING */
	int hottest;                            /* the zone furthest past (or nearest to) its limit; -1 none */
	int16_t hottest_temp;
	int hot_ticks;                          /* ticks in a row past a critical limit */
	int blind_ticks;                        /* ticks in a row with no reading of any CPU zone */
	uint32_t throttles;                     /* times a group was slowed, over the run */
};

/* What one tick decided, besides the caps. */
#define THERMAL_CAPS_CHANGED    (1u << 0)       /* write the caps to the hardware */
#define THERMAL_LEVEL_CHANGED   (1u << 1)       /* tell iOS */
#define THERMAL_EMERGENCY       (1u << 2)       /* past the stop for too long: end the run now */
#define THERMAL_BLIND           (1u << 3)       /* no CPU sensor reads: held at half speed */

static inline void
thermal_gov_init(struct thermal_gov *g, const int top[THERMAL_DOMAINS])
{
	*g = (struct thermal_gov){ .level = THERMAL_NOMINAL, .hottest = -1 };
	for (int d = 0; d < THERMAL_DOMAINS; d++) {
		g->top[d] = top[d];
		g->cap[d] = top[d];
	}
}

static inline int
thermal_clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/*
 * One tick: `tenths` holds a reading per zone of hakios_thermal_zones (THERMAL_INVALID where there was
 * none), `battery` the battery's temperature in tenths (below -2000: not known). Changes the caps and
 * the level in `g`; returns THERMAL_* flags.
 */
static inline unsigned
thermal_gov_step(struct thermal_gov *g, const int16_t *tenths, int battery)
{
	int excess[THERMAL_DOMAINS], package = -10000, worst = -10000, worst_zone = -1, any_cpu = 0, critical = 0;
	int old_cap[THERMAL_DOMAINS], old_level = g->level, level;
	unsigned flags = 0;

	for (int d = 0; d < THERMAL_DOMAINS; d++) {
		excess[d] = -10000;
		old_cap[d] = g->cap[d];
	}
	/* how far past its limit each zone is (negative: under it), and the worst of them */
	for (int z = 0; z < THERMAL_ZONES; z++) {
		const struct thermal_zone *zone = &hakios_thermal_zones[z];
		int t = tenths[z], over;

		if (!thermal_source_is_temperature(zone->source) || t == THERMAL_INVALID) {
			continue;
		}
		over = t - zone->passive;
		if (zone->domain >= 0) {
			excess[zone->domain] = over > excess[zone->domain] ? over : excess[zone->domain];
			any_cpu = 1;
		} else {
			/* the shared CPU zones and everything else heat the whole package: they count for every group */
			package = over > package ? over : package;
			any_cpu |= zone->domain == THERMAL_ALL_CPUS;
		}
		if (t >= zone->critical) {
			critical = 1;
		}
		if (over > worst) {
			worst = over;
			worst_zone = z;
		}
	}
	g->hottest = worst_zone;
	g->hottest_temp = worst_zone >= 0 ? tenths[worst_zone] : THERMAL_INVALID;

	/* no reading of any CPU zone for a while: a sensor block that has stopped is no reason to run hot */
	g->blind_ticks = any_cpu ? 0 : g->blind_ticks + 1;

	for (int d = 0; d < THERMAL_DOMAINS; d++) {
		int e = excess[d] > package ? excess[d] : package;

		if (g->top[d] < 0) {
			continue;                       /* a group with none of our cores in it */
		}
		if (e >= THERMAL_HOT_STEP) {
			g->cap[d] -= 3;
			g->cool[d] = 0;
		} else if (e >= 0) {
			g->cap[d] -= 1;
			g->cool[d] = 0;
		} else if (e < -THERMAL_COOL_MARGIN) {
			if (++g->cool[d] >= THERMAL_COOL_TICKS && g->cap[d] < g->top[d]) {
				g->cap[d] += 1;         /* one row a second back up: no see-saw */
				g->cool[d] = 0;
			}
		} else {
			g->cool[d] = 0;                 /* between the two: hold where it is */
		}
		if (battery >= THERMAL_BATTERY_DANGER) {
			g->cap[d] = 0;
		} else if (battery >= THERMAL_BATTERY_HOT && g->cap[d] > g->top[d] / 2) {
			g->cap[d] = g->top[d] / 2;
		}
		if (g->blind_ticks >= THERMAL_BLIND_TICKS && g->cap[d] > g->top[d] / 2) {
			g->cap[d] = g->top[d] / 2;
		}
		if (critical) {
			g->cap[d] = 0;
		}
		g->cap[d] = thermal_clamp(g->cap[d], 0, g->top[d]);
		if (g->cap[d] < old_cap[d]) {
			g->throttles++;
		}
		if (g->cap[d] != old_cap[d]) {
			flags |= THERMAL_CAPS_CHANGED;
		}
	}

	/* the level iOS is told */
	if (critical) {
		level = THERMAL_SLEEPING;
	} else if (worst_zone >= 0 && g->hottest_temp >= hakios_thermal_zones[worst_zone].critical - 50) {
		level = THERMAL_TRAPPING;
	} else {
		int slowed = 0;
		for (int d = 0; d < THERMAL_DOMAINS; d++) {
			slowed |= g->top[d] >= 0 && g->cap[d] < g->top[d];
		}
		level = slowed ? THERMAL_HEAVY : worst >= -100 ? THERMAL_MODERATE : THERMAL_NOMINAL;
	}
	if (battery >= THERMAL_BATTERY_DANGER && level < THERMAL_TRAPPING) {
		level = THERMAL_TRAPPING;
	} else if (battery >= THERMAL_BATTERY_HOT && level < THERMAL_HEAVY) {
		level = THERMAL_HEAVY;
	} else if (battery >= THERMAL_BATTERY_WARM && level < THERMAL_MODERATE) {
		level = THERMAL_MODERATE;
	}
	g->level = level;
	if (level != old_level) {
		flags |= THERMAL_LEVEL_CHANGED;
	}

	g->hot_ticks = critical ? g->hot_ticks + 1 : 0;
	if (g->hot_ticks >= THERMAL_EMERGENCY_TICKS) {
		flags |= THERMAL_EMERGENCY;
	}
	if (g->blind_ticks >= THERMAL_BLIND_TICKS) {
		flags |= THERMAL_BLIND;
	}
	return flags;
}

static inline const char *
thermal_level_name(int level)
{
	static const char *const names[] = { "nominal", "moderate", "heavy", "trapping", "sleeping" };
	return level >= 0 && level <= THERMAL_SLEEPING ? names[level] : "?";
}

#endif /* _SYS_HAKIOS_THERMAL_H_ */
