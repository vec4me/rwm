#define _POSIX_C_SOURCE 200809L
#include "sysinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <fcntl.h>

/* Paths - these may need adjustment per-system */
#define BATTERY_PATH      "/sys/class/power_supply/BAT0/capacity"
#define BACKLIGHT_PATH    "/sys/class/backlight/nvidia_0"
#define WIFI_IFACE        "wlp0s20f3"

/* Update intervals in seconds */
#define INTERVAL_BATTERY    30
#define INTERVAL_BRIGHTNESS  2
#define INTERVAL_CPU         1
#define INTERVAL_MEM         2
#define INTERVAL_WIFI        5
#define INTERVAL_BLUETOOTH  10
#define INTERVAL_CAPS        1

/* Background thread state */
static pthread_t sysinfo_thread;
static atomic_bool sysinfo_running;
static struct sysinfo shared_info;
static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Cached paths (discovered once at startup) */
static char cached_hwmon_path[280];
static char cached_bt_rfkill[280];

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

/* Find bluetooth rfkill device path */
static int find_bt_rfkill(char *out, size_t len) {
	DIR *d = opendir("/sys/class/rfkill");
	if (!d) return -1;
	const struct dirent *ent;
	while ((ent = readdir(d))) {
		if (ent->d_name[0] == '.') continue;
		char path[279];
		snprintf(path, sizeof(path), "/sys/class/rfkill/%s/type", ent->d_name);
		char type[32];
		if (read_file_str(path, type, sizeof(type)) == 0 && strcmp(type, "bluetooth") == 0) {
			snprintf(out, len, "/sys/class/rfkill/%s", ent->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

/* Persistent file descriptors */
static int fd_battery = -1;
static int fd_brightness = -1;
static int fd_max_brightness = -1;
static int fd_cpu_temp = -1;
static int fd_cpu_freq = -1;
static int fd_meminfo = -1;
static int fd_wireless = -1;
static int fd_wifi_state = -1;
static int fd_bt_soft = -1;
static int fd_bt_hard = -1;
static int fd_capslock = -1;

/* Cached max brightness value (doesn't change) */
static int cached_max_brightness = -1;

/* Read int from open fd (seeks to start first) */
static int read_fd_int(int fd) {
	if (fd < 0) return -1;
	if (lseek(fd, 0, SEEK_SET) < 0) return -1;
	char buf[32];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0) return -1;
	buf[n] = '\0';
	return atoi(buf);
}

/* Read string from open fd (seeks to start first) */
static int read_fd_str(int fd, char *buf, size_t len) {
	if (fd < 0) return -1;
	if (lseek(fd, 0, SEEK_SET) < 0) return -1;
	ssize_t n = read(fd, buf, len - 1);
	if (n <= 0) return -1;
	buf[n] = '\0';
	/* strip newline */
	if (buf[n-1] == '\n') buf[n-1] = '\0';
	return 0;
}

static int get_battery_percent(void) {
	return read_fd_int(fd_battery);
}

static int get_brightness_percent(void) {
	int cur = read_fd_int(fd_brightness);
	if (cur < 0 || cached_max_brightness <= 0) return -1;
	return (cur * 100) / cached_max_brightness;
}

static int get_cpu_temp_c(void) {
	int millideg = read_fd_int(fd_cpu_temp);
	if (millideg < 0) return -1;
	return millideg / 1000;
}

static int get_cpu_freq_mhz(void) {
	int khz = read_fd_int(fd_cpu_freq);
	if (khz < 0) return -1;
	return khz / 1000;
}

static int get_mem_used_percent(void) {
	if (fd_meminfo < 0) return -1;
	if (lseek(fd_meminfo, 0, SEEK_SET) < 0) return -1;

	char content[1024];
	ssize_t n = read(fd_meminfo, content, sizeof(content) - 1);
	if (n <= 0) return -1;
	content[n] = '\0';

	long total = 0, available = 0;
	const char *line = content;
	while (line && *line) {
		if (strncmp(line, "MemTotal:", 9) == 0)
			sscanf(line + 9, "%ld", &total);
		else if (strncmp(line, "MemAvailable:", 13) == 0)
			sscanf(line + 13, "%ld", &available);
		line = strchr(line, '\n');
		if (line) line++;
	}

	if (total <= 0) return -1;
	return (int)(((total - available) * 100) / total);
}

static int get_wifi_signal_dbm(void) {
	if (fd_wireless < 0) return -1;
	if (lseek(fd_wireless, 0, SEEK_SET) < 0) return -1;

	char content[512];
	ssize_t n = read(fd_wireless, content, sizeof(content) - 1);
	if (n <= 0) return -1;
	content[n] = '\0';

	int signal = -1;
	char *line = content;
	/* Skip two header lines */
	for (int i = 0; i < 2 && line; i++) {
		line = strchr(line, '\n');
		if (line) line++;
	}

	while (line && *line) {
		if (strstr(line, WIFI_IFACE)) {
			char iface[32];
			int status, link;
			float level;
			if (sscanf(line, "%31s %d %d %f", iface, &status, &link, &level) >= 4)
				signal = (int)level;
			break;
		}
		line = strchr(line, '\n');
		if (line) line++;
	}
	return signal;
}

static bool get_wifi_connected(void) {
	char state[32];
	if (read_fd_str(fd_wifi_state, state, sizeof(state)) < 0) return false;
	return strcmp(state, "up") == 0;
}

static bool get_bluetooth_on(void) {
	int soft = read_fd_int(fd_bt_soft);
	int hard = read_fd_int(fd_bt_hard);
	return (soft == 0 && hard == 0);
}

static bool get_caps_lock(void) {
	int brightness = read_fd_int(fd_capslock);
	return brightness > 0;
}

/* Open all file descriptors */
static void open_fds(void) {
	char path[300];

	fd_battery = open(BATTERY_PATH, O_RDONLY);

	snprintf(path, sizeof(path), "%s/brightness", BACKLIGHT_PATH);
	fd_brightness = open(path, O_RDONLY);
	snprintf(path, sizeof(path), "%s/max_brightness", BACKLIGHT_PATH);
	fd_max_brightness = open(path, O_RDONLY);
	cached_max_brightness = read_fd_int(fd_max_brightness);

	if (cached_hwmon_path[0]) {
		snprintf(path, sizeof(path), "%s/temp1_input", cached_hwmon_path);
		fd_cpu_temp = open(path, O_RDONLY);
	}

	fd_cpu_freq = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", O_RDONLY);
	fd_meminfo = open("/proc/meminfo", O_RDONLY);
	fd_wireless = open("/proc/net/wireless", O_RDONLY);

	snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", WIFI_IFACE);
	fd_wifi_state = open(path, O_RDONLY);

	if (cached_bt_rfkill[0]) {
		snprintf(path, sizeof(path), "%s/soft", cached_bt_rfkill);
		fd_bt_soft = open(path, O_RDONLY);
		snprintf(path, sizeof(path), "%s/hard", cached_bt_rfkill);
		fd_bt_hard = open(path, O_RDONLY);
	}

	fd_capslock = open("/sys/class/leds/input0::capslock/brightness", O_RDONLY);
}

/* Profiling data */
static struct {
	double battery_us;
	double brightness_us;
	double cpu_temp_us;
	double cpu_freq_us;
	double mem_us;
	double wifi_signal_us;
	double wifi_state_us;
	double bluetooth_us;
	double capslock_us;
} profile_times;

static double time_diff_us(const struct timespec *start, const struct timespec *end) {
	return (double)(end->tv_sec - start->tv_sec) * 1000000.0 +
	       (double)(end->tv_nsec - start->tv_nsec) / 1000.0;
}

#define PROFILE(field, call) do { \
	struct timespec _start, _end; \
	clock_gettime(CLOCK_MONOTONIC, &_start); \
	(call); \
	clock_gettime(CLOCK_MONOTONIC, &_end); \
	profile_times.field = time_diff_us(&_start, &_end); \
} while(0)

/* Background thread function */
static void *sysinfo_thread_fn(void *arg) {
	(void)arg;

	time_t last_battery = 0, last_brightness = 0, last_cpu = 0;
	time_t last_mem = 0, last_wifi = 0, last_bt = 0, last_caps = 0;

	while (atomic_load(&sysinfo_running)) {
		time_t now = time(NULL);
		struct sysinfo local;

		/* Copy current values */
		pthread_mutex_lock(&info_mutex);
		local = shared_info;
		pthread_mutex_unlock(&info_mutex);

		/* Update metrics based on their intervals */
		if (now - last_battery >= INTERVAL_BATTERY) {
			PROFILE(battery_us, local.battery_percent = get_battery_percent());
			last_battery = now;
		}
		if (now - last_brightness >= INTERVAL_BRIGHTNESS) {
			PROFILE(brightness_us, local.brightness_percent = get_brightness_percent());
			last_brightness = now;
		}
		if (now - last_cpu >= INTERVAL_CPU) {
			PROFILE(cpu_temp_us, local.cpu_temp_c = get_cpu_temp_c());
			PROFILE(cpu_freq_us, local.cpu_freq_mhz = get_cpu_freq_mhz());
			last_cpu = now;
		}
		if (now - last_mem >= INTERVAL_MEM) {
			PROFILE(mem_us, local.mem_used_percent = get_mem_used_percent());
			last_mem = now;
		}
		if (now - last_wifi >= INTERVAL_WIFI) {
			PROFILE(wifi_signal_us, local.wifi_signal_dbm = get_wifi_signal_dbm());
			PROFILE(wifi_state_us, local.wifi_connected = get_wifi_connected());
			last_wifi = now;
		}
		if (now - last_bt >= INTERVAL_BLUETOOTH) {
			PROFILE(bluetooth_us, local.bluetooth_on = get_bluetooth_on());
			last_bt = now;
		}
		if (now - last_caps >= INTERVAL_CAPS) {
			PROFILE(capslock_us, local.caps_lock = get_caps_lock());
			last_caps = now;
		}

		/* Update shared state */
		pthread_mutex_lock(&info_mutex);
		shared_info = local;
		pthread_mutex_unlock(&info_mutex);

		struct timespec ts = {0, 100000000}; /* 100ms */
		nanosleep(&ts, NULL);
	}
	return NULL;
}

void sysinfo_start(void) {
	/* Discover paths once */
	find_hwmon_by_name("coretemp", cached_hwmon_path, sizeof(cached_hwmon_path));
	find_bt_rfkill(cached_bt_rfkill, sizeof(cached_bt_rfkill));

	/* Open all file descriptors */
	open_fds();

	/* Initialize shared info */
	shared_info = (struct sysinfo){-1, -1, -1, -1, -1, -1, false, false, false};

	/* Start background thread */
	atomic_store(&sysinfo_running, true);
	pthread_create(&sysinfo_thread, NULL, sysinfo_thread_fn, NULL);
}

void sysinfo_stop(void) {
	atomic_store(&sysinfo_running, false);
	pthread_join(sysinfo_thread, NULL);
}

void sysinfo_get(struct sysinfo *info) {
	pthread_mutex_lock(&info_mutex);
	*info = shared_info;
	pthread_mutex_unlock(&info_mutex);
}

void sysinfo_get_profile(struct sysinfo_profile *p) {
	p->battery_us = profile_times.battery_us;
	p->brightness_us = profile_times.brightness_us;
	p->cpu_temp_us = profile_times.cpu_temp_us;
	p->cpu_freq_us = profile_times.cpu_freq_us;
	p->mem_us = profile_times.mem_us;
	p->wifi_signal_us = profile_times.wifi_signal_us;
	p->wifi_state_us = profile_times.wifi_state_us;
	p->bluetooth_us = profile_times.bluetooth_us;
	p->capslock_us = profile_times.capslock_us;
}

/* Legacy synchronous update (deprecated, but kept for compatibility) */
void sysinfo_update(struct sysinfo *info) {
	sysinfo_get(info);
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
