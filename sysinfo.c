#include "sysinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

/* Paths - these may need adjustment per-system */
#define BATTERY_PATH      "/sys/class/power_supply/BAT0/capacity"
#define BACKLIGHT_PATH    "/sys/class/backlight/nvidia_0"
#define WIFI_IFACE        "wlp0s20f3"

static int read_file_int(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	int val = -1;
	if (fscanf(f, "%d", &val) != 1) val = -1;
	fclose(f);
	return val;
}

static int read_file_str(const char *path, char *buf, size_t len) {
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	if (!fgets(buf, (int)len, f)) { fclose(f); return -1; }
	fclose(f);
	/* strip newline */
	size_t l = strlen(buf);
	if (l > 0 && buf[l-1] == '\n') buf[l-1] = 0;
	return 0;
}

/* Find hwmon path by sensor name (e.g., "coretemp") */
static int find_hwmon_by_name(const char *name, char *out, size_t len) {
	DIR *d = opendir("/sys/class/hwmon");
	if (!d) return -1;
	const struct dirent *ent;
	while ((ent = readdir(d))) {
		if (ent->d_name[0] == '.') continue;
		char path[278]; /* /sys/class/hwmon/ + NAME_MAX + /name + null */
		snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", ent->d_name);
		char sensor_name[64];
		if (read_file_str(path, sensor_name, sizeof(sensor_name)) == 0) {
			if (strcmp(sensor_name, name) == 0) {
				snprintf(out, len, "/sys/class/hwmon/%s", ent->d_name);
				closedir(d);
				return 0;
			}
		}
	}
	closedir(d);
	return -1;
}

static int get_battery_percent(void) {
	return read_file_int(BATTERY_PATH);
}

static int get_brightness_percent(void) {
	char path[128];
	snprintf(path, sizeof(path), "%s/brightness", BACKLIGHT_PATH);
	int cur = read_file_int(path);
	snprintf(path, sizeof(path), "%s/max_brightness", BACKLIGHT_PATH);
	int max = read_file_int(path);
	if (cur < 0 || max <= 0) return -1;
	return (cur * 100) / max;
}

static int get_cpu_temp_c(void) {
	char hwmon_path[273]; /* /sys/class/hwmon/ + NAME_MAX + null */
	if (find_hwmon_by_name("coretemp", hwmon_path, sizeof(hwmon_path)) < 0)
		return -1;
	char path[285]; /* hwmon_path + /temp1_input */
	snprintf(path, sizeof(path), "%s/temp1_input", hwmon_path);
	int millideg = read_file_int(path);
	if (millideg < 0) return -1;
	return millideg / 1000;
}

static int get_cpu_freq_mhz(void) {
	/* Read from first CPU core */
	int khz = read_file_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
	if (khz < 0) return -1;
	return khz / 1000;
}

static int get_mem_used_percent(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) return -1;

	long total = 0, available = 0;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "MemTotal:", 9) == 0)
			sscanf(line + 9, "%ld", &total);
		else if (strncmp(line, "MemAvailable:", 13) == 0)
			sscanf(line + 13, "%ld", &available);
	}
	fclose(f);

	if (total <= 0) return -1;
	return (int)(((total - available) * 100) / total);
}

static int get_wifi_signal_dbm(void) {
	FILE *f = fopen("/proc/net/wireless", "r");
	if (!f) return -1;

	char line[256];
	int signal = -1;
	/* Skip header lines */
	if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
	if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }

	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, WIFI_IFACE)) {
			/* Format: iface: status link level noise ... */
			char iface[32];
			int status, link;
			float level;
			if (sscanf(line, "%31s %d %d %f", iface, &status, &link, &level) >= 4) {
				signal = (int)level;
			}
			break;
		}
	}
	fclose(f);
	return signal;
}

static bool get_wifi_connected(void) {
	char path[128];
	snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", WIFI_IFACE);
	char state[32];
	if (read_file_str(path, state, sizeof(state)) < 0) return false;
	return strcmp(state, "up") == 0;
}

static bool get_bluetooth_on(void) {
	/* Check if any bluetooth rfkill is unblocked */
	DIR *d = opendir("/sys/class/rfkill");
	if (!d) return false;

	const struct dirent *ent;
	bool on = false;
	while ((ent = readdir(d))) {
		if (ent->d_name[0] == '.') continue;
		char path[279]; /* /sys/class/rfkill/ + NAME_MAX + /type + null */
		snprintf(path, sizeof(path), "/sys/class/rfkill/%s/type", ent->d_name);
		char type[32];
		if (read_file_str(path, type, sizeof(type)) == 0 && strcmp(type, "bluetooth") == 0) {
			snprintf(path, sizeof(path), "/sys/class/rfkill/%s/soft", ent->d_name);
			int soft = read_file_int(path);
			snprintf(path, sizeof(path), "/sys/class/rfkill/%s/hard", ent->d_name);
			int hard = read_file_int(path);
			if (soft == 0 && hard == 0) {
				on = true;
				break;
			}
		}
	}
	closedir(d);
	return on;
}

static bool get_caps_lock(void) {
	/* Check first available capslock LED */
	int brightness = read_file_int("/sys/class/leds/input0::capslock/brightness");
	return brightness > 0;
}

void sysinfo_update(struct sysinfo *info) {
	info->battery_percent = get_battery_percent();
	info->brightness_percent = get_brightness_percent();
	info->cpu_temp_c = get_cpu_temp_c();
	info->cpu_freq_mhz = get_cpu_freq_mhz();
	info->mem_used_percent = get_mem_used_percent();
	info->wifi_signal_dbm = get_wifi_signal_dbm();
	info->wifi_connected = get_wifi_connected();
	info->bluetooth_on = get_bluetooth_on();
	info->caps_lock = get_caps_lock();
}

void sysinfo_adjust_brightness(int delta) {
	char path[128];
	snprintf(path, sizeof(path), "%s/brightness", BACKLIGHT_PATH);
	int cur = read_file_int(path);
	snprintf(path, sizeof(path), "%s/max_brightness", BACKLIGHT_PATH);
	int max = read_file_int(path);
	if (cur < 0 || max <= 0) return;

	int step = max / 20; /* 5% steps */
	if (step < 1) step = 1;
	int newval = cur + delta * step;
	if (newval < 1) newval = 1;
	if (newval > max) newval = max;

	snprintf(path, sizeof(path), "%s/brightness", BACKLIGHT_PATH);
	FILE *f = fopen(path, "w");
	if (f) { fprintf(f, "%d", newval); fclose(f); }
}

const char *sysinfo_format_status(const struct sysinfo *info) {
	static char buf[256];
	char *p = buf;
	const char *end = buf + sizeof(buf);

	/* Battery */
	if (info->battery_percent >= 0)
		p += snprintf(p, (size_t)(end - p), "BAT %d%%", info->battery_percent);

	/* Brightness */
	if (info->brightness_percent >= 0)
		p += snprintf(p, (size_t)(end - p), "%sBRI %d%%", p > buf ? "  " : "", info->brightness_percent);

	/* CPU temp */
	if (info->cpu_temp_c >= 0)
		p += snprintf(p, (size_t)(end - p), "%s%d°C", p > buf ? "  " : "", info->cpu_temp_c);

	/* CPU freq */
	if (info->cpu_freq_mhz >= 0) {
		if (info->cpu_freq_mhz >= 1000)
			p += snprintf(p, (size_t)(end - p), "%s%.1fGHz", p > buf ? "  " : "", info->cpu_freq_mhz / 1000.0);
		else
			p += snprintf(p, (size_t)(end - p), "%s%dMHz", p > buf ? "  " : "", info->cpu_freq_mhz);
	}

	/* Memory */
	if (info->mem_used_percent >= 0)
		p += snprintf(p, (size_t)(end - p), "%sMEM %d%%", p > buf ? "  " : "", info->mem_used_percent);

	/* WiFi */
	if (info->wifi_connected) {
		if (info->wifi_signal_dbm != -1)
			p += snprintf(p, (size_t)(end - p), "%sWiFi %ddBm", p > buf ? "  " : "", info->wifi_signal_dbm);
		else
			p += snprintf(p, (size_t)(end - p), "%sWiFi", p > buf ? "  " : "");
	}

	/* Bluetooth */
	if (info->bluetooth_on)
		p += snprintf(p, (size_t)(end - p), "%sBT", p > buf ? "  " : "");

	/* Caps Lock */
	if (info->caps_lock)
		p += snprintf(p, (size_t)(end - p), "%sCAPS", p > buf ? "  " : "");

	/* Time (24-hour format) */
	time_t now = time(NULL);
	const struct tm *tm = localtime(&now);
	if (tm)
		(void)snprintf(p, (size_t)(end - p), "%s%02d:%02d", p > buf ? "  " : "",
			tm->tm_hour, tm->tm_min);

	return buf;
}
