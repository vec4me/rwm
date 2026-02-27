#ifndef SYSINFO_H
#define SYSINFO_H

#include <stdbool.h>

/* System information structure - all values are -1 if unavailable */
struct sysinfo {
	int battery_percent;      /* 0-100 */
	int brightness_percent;   /* 0-100 */
	int cpu_temp_c;           /* degrees celsius */
	int cpu_freq_mhz;         /* MHz */
	int mem_used_percent;     /* 0-100 */
	int wifi_signal_dbm;      /* dBm (negative, e.g. -50) */
	bool wifi_connected;
	bool bluetooth_on;
	bool caps_lock;
};

/* Profiling data for each metric (microseconds) */
struct sysinfo_profile {
	double battery_us;
	double brightness_us;
	double cpu_temp_us;
	double cpu_freq_us;
	double mem_us;
	double wifi_signal_us;
	double wifi_state_us;
	double bluetooth_us;
	double capslock_us;
};

/* Start background thread for gathering system info */
void sysinfo_start(void);

/* Stop background thread */
void sysinfo_stop(void);

/* Get current cached sysinfo (non-blocking) */
void sysinfo_get(struct sysinfo *info);

/* Get profiling data (last update times in microseconds) */
void sysinfo_get_profile(struct sysinfo_profile *p);

/* Fill sysinfo struct with current values (legacy, calls sysinfo_get) */
void sysinfo_update(struct sysinfo *info);

/* Adjust screen brightness by delta steps (positive = brighter) */
void sysinfo_adjust_brightness(int delta);

/* Format status string for display (returns static buffer) */
const char *sysinfo_format_status(const struct sysinfo *info);

#endif
