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

/* Fill sysinfo struct with current values */
void sysinfo_update(struct sysinfo *info);

/* Adjust screen brightness by delta steps (positive = brighter) */
void sysinfo_adjust_brightness(int delta);

/* Format status string for display (returns static buffer) */
const char *sysinfo_format_status(const struct sysinfo *info);

#endif
