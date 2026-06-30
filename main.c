#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/statvfs.h>
#include <sys/sysinfo.h>

#define MAX_LINE_LENGTH     1024
#define MAX_CORES           64
#define MAX_NIC				4
#define TARGET_DISK         "sda"
#define SAMPLE_MS           1000
#define MS_PER_SEC          1000.0
#define DISKSTATS_PATH      "/proc/diskstats"
#define THERMAL_PATH        "/sys/class/thermal/thermal_zone1/temp"
#define CPUINFO_PATH        "/proc/cpuinfo"
#define MEMINFO_PATH		"/proc/meminfo"
#define BAT_PATH			"/sys/class/power_supply/BAT0/capacity"
#define NETDEV_PATH			"/proc/net/dev"
#define TEMP_DIVISOR        1000

typedef struct {
    double cores_mhz[MAX_CORES];
    double cpu_mhz;
    int cpu_temp;

    unsigned long io_ms;
    unsigned long ios_active;
    int root_space;
    int home_space;
    
    /* Memory measured in Kilobytes */
    int mem_free;
    int mem_total;
    int mem_available;
    
    int batt_perc;
    
    /* Net Stats */
    char nic_names[MAX_NIC][32];
    unsigned long net_rx[MAX_NIC];
    unsigned long net_tx[MAX_NIC];
    int nic_count;
    
    long uptime_d; /* Days */
    long uptime_h; /* Hours */
    long uptime_m; /* Minutes */
    long uptime_s; /* Seconds */
} stats;

int get_cpu_mhz(stats *s) {
    FILE *fptr = NULL;
    char line[MAX_LINE_LENGTH] = {0};
    char search_term[] = "cpu MHz";
    double core_total = 0.0;
    int core = 0;

    fptr = fopen(CPUINFO_PATH, "r");
    if (fptr == NULL) {
        fprintf(stderr, "get_cpu_mhz: failed to open %s\n", CPUINFO_PATH);
        return -1;
    }

    while (fgets(line, sizeof(line), fptr) != NULL) {
        char *colon = NULL;

        if (strstr(line, search_term) == NULL) {
            continue;
        }

        colon = strchr(line, ':');
        if (colon == NULL) {
            continue;
        }

        if (core < MAX_CORES) {
            s->cores_mhz[core] = atof(colon + 1);
            core_total += s->cores_mhz[core];
            core++;
        }
    }

    if (core > 0) {
        s->cpu_mhz = core_total / core;
    }

    fclose(fptr);
    return 0;
}

int get_cpu_temp(stats *s) {
    FILE *fptr = NULL;
    char line[MAX_LINE_LENGTH] = {0};
    char *result = NULL;

    fptr = fopen(THERMAL_PATH, "r");
    if (fptr == NULL) {
        fprintf(stderr, "get_cpu_temp: failed to open %s\n", THERMAL_PATH);
        return -1;
    }

    result = fgets(line, sizeof(line), fptr);
    if (result == NULL) {
        fprintf(stderr, "get_cpu_temp: failed to read temperature\n");
        fclose(fptr);
        return -1;
    }

    /* Kernel reports millidegrees C, divide by 1000 for degrees C */
    s->cpu_temp = (int)(atof(line) / TEMP_DIVISOR);

    fclose(fptr);
    return 0;
}

int read_disk_sample(stats *s) {
    FILE *fp = NULL;
    char line[MAX_LINE_LENGTH] = {0};
    char dev[64] = {0};
    unsigned long f[14] = {0};
    stats s2 = {0};
    unsigned long delta_io_ms = 0;
    int found = 0;
    int n = 0;

    /* First sample */
    fp = fopen(DISKSTATS_PATH, "r");
    if (fp == NULL) {
        fprintf(stderr, "read_disk_sample: failed to open %s\n", DISKSTATS_PATH);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        n = sscanf(line,
            "%*u %*u %63s "
            "%lu %lu %lu %lu "
            "%lu %lu %lu %lu "
            "%lu %lu %lu %lu %lu",
            dev,
            &f[0], &f[1], &f[2], &f[3],
            &f[4], &f[5], &f[6], &f[7],
            &f[8], &f[9], &f[10], &f[11], &f[12]);

        if (n < 11 || strcmp(dev, TARGET_DISK) != 0) {
            continue;
        }

        s->ios_active = f[8];
        s->io_ms      = f[9];
        found = 1;
        break;
    }

    fclose(fp);

    if (!found) {
        fprintf(stderr, "read_disk_sample: device '%s' not found\n", TARGET_DISK);
        return -1;
    }

    usleep(SAMPLE_MS * 1000);

    /* Second sample */
    found = 0;
    fp = fopen(DISKSTATS_PATH, "r");
    if (fp == NULL) {
        fprintf(stderr, "read_disk_sample: failed to open %s (second sample)\n", DISKSTATS_PATH);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        n = sscanf(line,
            "%*u %*u %63s "
            "%lu %lu %lu %lu "
            "%lu %lu %lu %lu "
            "%lu %lu %lu %lu %lu",
            dev,
            &f[0], &f[1], &f[2], &f[3],
            &f[4], &f[5], &f[6], &f[7],
            &f[8], &f[9], &f[10], &f[11], &f[12]);

        if (n < 11 || strcmp(dev, TARGET_DISK) != 0) {
            continue;
        }

        s2.ios_active = f[8];
        s2.io_ms      = f[9];
        found = 1;
        break;
    }

    fclose(fp);

    if (!found) {
        fprintf(stderr, "read_disk_sample: device '%s' not found (second sample)\n", TARGET_DISK);
        return -1;
    }

    delta_io_ms = s2.io_ms - s->io_ms;
    s->ios_active = s2.ios_active;

    /* Clamp to 100 — can exceed on SSDs with parallel queues */
    s->io_ms = (delta_io_ms > SAMPLE_MS) ? SAMPLE_MS : delta_io_ms;

    return 0;
}

int get_fs_space(stats *s) {
    struct statvfs root_vfs = {0};
    struct statvfs home_vfs = {0};
    unsigned long block_size = 0;
    unsigned long available_blocks = 0;

    if (statvfs("/", &root_vfs) != 0) {
        fprintf(stderr, "get_fs_space: statvfs failed for /\n");
        return -1;
    }

    if (statvfs("/home", &home_vfs) != 0) {
        fprintf(stderr, "get_fs_space: statvfs failed for /home\n");
        return -1;
    }

    /* f_frsize is the fundamental block size; f_bavail is blocks available
       to unprivileged users (excludes root-reserved blocks) */
    block_size = root_vfs.f_frsize;
    available_blocks = root_vfs.f_bavail;
    s->root_space = (int)((block_size * available_blocks) / (1024 * 1024));

    block_size = home_vfs.f_frsize;
    available_blocks = home_vfs.f_bavail;
    s->home_space = (int)((block_size * available_blocks) / (1024 * 1024));

    return 0;
}

int get_mem_info(stats *s) {
    FILE *fptr = NULL;
    char line[MAX_LINE_LENGTH] = {0};

    fptr = fopen(MEMINFO_PATH, "r");
    if (fptr == NULL) {
        fprintf(stderr, "get_mem_info: failed to open %s\n", MEMINFO_PATH);
        return -1;
    }

    while (fgets(line, sizeof(line), fptr) != NULL) {
        char *colon = strchr(line, ':');
        if (colon == NULL) {
            continue;
        }

        if (strncmp(line, "MemTotal", 8) == 0) {
            s->mem_total = atoi(colon + 1);
        } else if (strncmp(line, "MemFree", 7) == 0) {
            s->mem_free = atoi(colon + 1);
        } else if (strncmp(line, "MemAvailable", 12) == 0) {
            s->mem_available = atoi(colon + 1);
        }
    }
        
    fclose(fptr);
    return 0;
}

int get_bat_perc(stats *s) {
	FILE *fptr = NULL;
	char line[MAX_LINE_LENGTH] = {0};
    char *result = NULL;

    fptr = fopen(BAT_PATH, "r");
    if (fptr == NULL) {
        fprintf(stderr, "get_bat_perc: failed to open %s\n", BAT_PATH);
        return -1;
    }

    result = fgets(line, sizeof(line), fptr);
    if (result == NULL) {
        fprintf(stderr, "get_bat_perc: failed to read temperature\n");
        fclose(fptr);
        return -1;
    }

    s->batt_perc = (int)(atof(line));

    fclose(fptr);
    return 0;
}

int get_uptime(stats *s) {
	struct sysinfo info;
	
	if (sysinfo(&info) == 0) {
		s->uptime_s = info.uptime;
		s->uptime_d = s->uptime_s / 86400;
		s->uptime_h = (s->uptime_s % 86400) / 3600;
		s->uptime_m = (s->uptime_s % 3600) / 60;
	} else { 
		fprintf(stderr, "get_uptime: sysinfo failed\n");
	}
	return 0;
}

int get_nic_stats(stats *s) {
    FILE *fp = fopen(NETDEV_PATH, "r");
    char line[MAX_LINE_LENGTH];
    int count = 0;

    if (fp == NULL) {
        fprintf(stderr, "get_nic_stats: failed to open %s\n", NETDEV_PATH);
        return 1;
    }

    /* Skip the first two header lines of /proc/net/dev */
    if (!fgets(line, sizeof(line), fp) || !fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 1;
    }

    while (fgets(line, sizeof(line), fp) != NULL && count < MAX_NIC) {
        char name[32];
        unsigned long rx_bytes = 0, tx_bytes = 0;
        
        /* 
         * /proc/net/dev format: 
         * face | bytes packets errs drop ... | bytes packets errs drop ...
         * We match the interface name and pull field 1 (RX) and field 9 (TX)
         */
        int parsed = sscanf(line, " %31[^:]: %lu %*u %*u %*u %*u %*u %*u %*u %lu", 
                            name, &rx_bytes, &tx_bytes);

        
        if (parsed == 3) {
            /* Strip any trailing whitespace from interface name */
            char *end = name + strlen(name) - 1;
            while (end > name && (*end == ' ' || *end == '\t')) {
                *end = '\0';
                end--;
            }
            
            /* Filter out loopback adapter if desired, or keep it */
            if (strcmp(name, "lo") == 0) continue;

            strncpy(s->nic_names[count], name, sizeof(s->nic_names[count]) - 1);
            s->net_rx[count] = rx_bytes;
            s->net_tx[count] = tx_bytes;
            count++;
        }
    }

    s->nic_count = count;
    fclose(fp);
    return 0;
}

int main(void) {
    stats s = {0};
    int ret = 0;

    ret = get_cpu_mhz(&s);
    if (ret != 0) fprintf(stderr, "main: get_cpu_mhz failed\n");

    ret = get_cpu_temp(&s);
    if (ret != 0) fprintf(stderr, "main: get_cpu_temp failed\n");

    ret = read_disk_sample(&s);
    if (ret != 0) fprintf(stderr, "main: read_disk_sample failed\n");

    ret = get_fs_space(&s);
    if (ret != 0) fprintf(stderr, "main: get_fs_space failed\n");

    ret = get_mem_info(&s);
    if (ret != 0) fprintf(stderr, "main: get_mem_info failed\n");

    ret = get_bat_perc(&s);
    if (ret != 0) fprintf(stderr, "main: get_bat_perc failed\n");

    ret = get_uptime(&s);
    if (ret != 0) fprintf(stderr, "main: get_uptime failed\n");

    ret = get_nic_stats(&s);
    if (ret != 0) fprintf(stderr, "main: get_nic_stats failed\n");

    printf("Uptime: %ld days, %ld hours, %ld minutes\n", s.uptime_d, s.uptime_h, s.uptime_m);
    printf("CPU: %.2f MHz avg, %d C\n", s.cpu_mhz, s.cpu_temp);
    printf("Disk %s: %lu IOs in flight, %.1f%% utilization\n", TARGET_DISK, s.ios_active, ((double)s.io_ms / SAMPLE_MS) * 100.0);
    printf("Root: %d MB free, Home: %d MB free\n", s.root_space, s.home_space);
    printf("Memory: %d total, %d free, %d available\n", s.mem_total, s.mem_free, s.mem_available);
    printf("Battery percentage: %d Percent\n", s.batt_perc);
    for (int i = 0; i < s.nic_count; i++) {
        printf("[%s] RX: %lu bytes | TX: %lu bytes\n", s.nic_names[i], s.net_rx[i], s.net_tx[i]);
    }

    return 0;
}

