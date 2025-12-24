#include "bandwidth_monitor.h"
#include "../utils/logger.h"
#include "../dbus/session_client.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/**
 * Internal structure for bandwidth monitor
 */
struct BandwidthMonitor {
    char *session_path;         /* D-Bus session path */
    char *device_name;          /* Network device name (e.g., "tun0") */
    StatsSource source;         /* Statistics source */
    BandwidthSample *samples;   /* Rolling buffer of samples */
    unsigned int buffer_size;   /* Size of rolling buffer */
    unsigned int sample_count;  /* Number of samples in buffer */
    unsigned int write_index;   /* Next index to write in circular buffer */
    time_t start_time;          /* When monitoring started */
    BandwidthSample baseline;   /* Initial sample (for calculating totals) */
    int has_baseline;           /* Whether we have a baseline */
};

/**
 * Read statistics from sysfs
 */
static int read_sysfs_stats(const char *device_name, BandwidthSample *sample) {
    char path[256];
    FILE *fp;
    uint64_t value;

    if (!device_name || !sample) {
        return -EINVAL;
    }

    /* Initialize sample */
    memset(sample, 0, sizeof(*sample));
    sample->timestamp = time(NULL);

    /* Read RX bytes */
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", device_name);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &value) == 1) {
            sample->bytes_in = value;
        }
        fclose(fp);
    } else {
        return -errno;
    }

    /* Read TX bytes */
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes", device_name);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &value) == 1) {
            sample->bytes_out = value;
        }
        fclose(fp);
    }

    /* Read RX packets */
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_packets", device_name);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &value) == 1) {
            sample->packets_in = value;
        }
        fclose(fp);
    }

    /* Read TX packets */
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_packets", device_name);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &value) == 1) {
            sample->packets_out = value;
        }
        fclose(fp);
    }

    /* Read RX errors */
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_errors", device_name);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &value) == 1) {
            sample->errors_in = value;
        }
        fclose(fp);
    }

    /* Read TX errors */
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_errors", device_name);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &value) == 1) {
            sample->errors_out = value;
        }
        fclose(fp);
    }

    /* Read RX dropped */
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_dropped", device_name);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &value) == 1) {
            sample->dropped_in = value;
        }
        fclose(fp);
    }

    /* Read TX dropped */
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_dropped", device_name);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &value) == 1) {
            sample->dropped_out = value;
        }
        fclose(fp);
    }

    return 0;
}

/**
 * Read statistics from D-Bus (OpenVPN3 session statistics)
 */
static int read_dbus_stats(sd_bus *bus, const char *session_path, BandwidthSample *sample) {
    uint64_t bytes_in, bytes_out, packets_in, packets_out;
    int r;

    if (!bus || !session_path || !sample) {
        return -EINVAL;
    }

    /* Initialize sample */
    memset(sample, 0, sizeof(*sample));
    sample->timestamp = time(NULL);

    /* Get statistics from D-Bus */
    r = session_get_statistics(bus, session_path,
                               &bytes_in, &bytes_out,
                               &packets_in, &packets_out);
    if (r < 0) {
        return r;
    }

    /* Fill in sample data */
    sample->bytes_in = bytes_in;
    sample->bytes_out = bytes_out;
    sample->packets_in = packets_in;
    sample->packets_out = packets_out;

    /* Note: D-Bus statistics don't provide error/dropped counts,
     * those will remain 0 */

    return 0;
}

/**
 * Create a new bandwidth monitor
 */
BandwidthMonitor* bandwidth_monitor_create(const char *session_path,
                                           const char *device_name,
                                           StatsSource source,
                                           unsigned int buffer_size) {
    BandwidthMonitor *monitor;

    /* Allocate monitor structure */
    monitor = calloc(1, sizeof(BandwidthMonitor));
    if (!monitor) {
        return NULL;
    }

    /* Set default buffer size if not specified */
    if (buffer_size == 0) {
        buffer_size = 60;  /* Default: 60 seconds */
    }

    /* Copy session path if provided */
    if (session_path) {
        monitor->session_path = strdup(session_path);
        if (!monitor->session_path) {
            free(monitor);
            return NULL;
        }
    }

    /* Copy device name if provided */
    if (device_name) {
        monitor->device_name = strdup(device_name);
        if (!monitor->device_name) {
            free(monitor->session_path);
            free(monitor);
            return NULL;
        }
    }

    /* Allocate sample buffer */
    monitor->samples = calloc(buffer_size, sizeof(BandwidthSample));
    if (!monitor->samples) {
        free(monitor->device_name);
        free(monitor->session_path);
        free(monitor);
        return NULL;
    }

    monitor->buffer_size = buffer_size;
    monitor->source = source;
    monitor->sample_count = 0;
    monitor->write_index = 0;
    monitor->has_baseline = 0;

    return monitor;
}

/**
 * Add a sample to the rolling buffer
 */
static void add_sample(BandwidthMonitor *monitor, const BandwidthSample *sample) {
    /* Store sample in circular buffer */
    monitor->samples[monitor->write_index] = *sample;

    /* Update write index (circular) */
    monitor->write_index = (monitor->write_index + 1) % monitor->buffer_size;

    /* Update sample count (max is buffer_size) */
    if (monitor->sample_count < monitor->buffer_size) {
        monitor->sample_count++;
    }

    /* Set start time from first sample */
    if (monitor->sample_count == 1) {
        monitor->start_time = sample->timestamp;
    }

    /* Store baseline from first sample */
    if (!monitor->has_baseline) {
        monitor->baseline = *sample;
        monitor->has_baseline = 1;
    }
}

/**
 * Update statistics from the data source
 */
int bandwidth_monitor_update(BandwidthMonitor *monitor, sd_bus *bus) {
    BandwidthSample sample;
    int r;

    if (!monitor) {
        return -EINVAL;
    }

    /* Try sources based on configuration */
    if (monitor->source == STATS_SOURCE_DBUS || monitor->source == STATS_SOURCE_AUTO) {
        if (bus && monitor->session_path) {
            r = read_dbus_stats(bus, monitor->session_path, &sample);
            if (r >= 0) {
                logger_debug("BandwidthMonitor: Using D-Bus statistics (bytes_in=%lu, bytes_out=%lu)",
                       sample.bytes_in, sample.bytes_out);
                add_sample(monitor, &sample);
                return 0;
            }
            /* If D-Bus fails and source is AUTO, try sysfs */
            if (monitor->source == STATS_SOURCE_DBUS) {
                logger_debug("BandwidthMonitor: D-Bus statistics failed (%d), no fallback available", r);
                return r;
            }
            logger_debug("BandwidthMonitor: D-Bus statistics failed (%d), falling back to sysfs", r);
        }
    }

    /* Try sysfs */
    if (monitor->source == STATS_SOURCE_SYSFS || monitor->source == STATS_SOURCE_AUTO) {
        if (monitor->device_name) {
            r = read_sysfs_stats(monitor->device_name, &sample);
            if (r >= 0) {
                logger_debug("BandwidthMonitor: Using sysfs statistics (bytes_in=%lu, bytes_out=%lu)",
                       sample.bytes_in, sample.bytes_out);
                add_sample(monitor, &sample);
                return 0;
            }
            return r;
        }
    }

    return -ENOTSUP;
}

/**
 * Get the latest bandwidth rate
 */
int bandwidth_monitor_get_rate(BandwidthMonitor *monitor, BandwidthRate *rate) {
    BandwidthSample *latest, *previous;
    unsigned int latest_idx, previous_idx;
    time_t time_diff;
    uint64_t bytes_in_diff, bytes_out_diff;

    if (!monitor || !rate) {
        return -EINVAL;
    }

    /* Need at least 2 samples to calculate rate */
    if (monitor->sample_count < 2) {
        memset(rate, 0, sizeof(*rate));
        return -EAGAIN;
    }

    /* Get latest sample (most recent) */
    if (monitor->write_index == 0) {
        latest_idx = monitor->buffer_size - 1;
    } else {
        latest_idx = monitor->write_index - 1;
    }
    latest = &monitor->samples[latest_idx];

    /* Get previous sample */
    if (latest_idx == 0) {
        previous_idx = monitor->buffer_size - 1;
    } else {
        previous_idx = latest_idx - 1;
    }
    previous = &monitor->samples[previous_idx];

    /* Calculate time difference */
    time_diff = latest->timestamp - previous->timestamp;
    if (time_diff <= 0) {
        time_diff = 1;  /* Avoid division by zero */
    }

    /* Calculate byte differences */
    bytes_in_diff = latest->bytes_in - previous->bytes_in;
    bytes_out_diff = latest->bytes_out - previous->bytes_out;

    /* Calculate rates (bytes per second) */
    rate->download_rate_bps = (double)bytes_in_diff / time_diff;
    rate->upload_rate_bps = (double)bytes_out_diff / time_diff;

    /* Calculate totals from baseline */
    if (monitor->has_baseline) {
        rate->total_downloaded = latest->bytes_in - monitor->baseline.bytes_in;
        rate->total_uploaded = latest->bytes_out - monitor->baseline.bytes_out;
    } else {
        rate->total_downloaded = latest->bytes_in;
        rate->total_uploaded = latest->bytes_out;
    }

    return 0;
}

/**
 * Get the latest sample
 */
int bandwidth_monitor_get_latest_sample(BandwidthMonitor *monitor,
                                        BandwidthSample *sample) {
    unsigned int latest_idx;

    if (!monitor || !sample) {
        return -EINVAL;
    }

    if (monitor->sample_count == 0) {
        return -EAGAIN;
    }

    /* Get latest sample */
    if (monitor->write_index == 0) {
        latest_idx = monitor->buffer_size - 1;
    } else {
        latest_idx = monitor->write_index - 1;
    }

    *sample = monitor->samples[latest_idx];
    return 0;
}

/**
 * Get historical samples (newest first)
 */
int bandwidth_monitor_get_samples(BandwidthMonitor *monitor,
                                  BandwidthSample *samples,
                                  unsigned int max_samples,
                                  unsigned int *count) {
    unsigned int idx, i;

    if (!monitor || !samples || !count) {
        return -EINVAL;
    }

    /* Limit to actual sample count */
    *count = (max_samples < monitor->sample_count) ? max_samples : monitor->sample_count;

    /* Copy samples in reverse order (newest first) */
    for (i = 0; i < *count; i++) {
        /* Calculate index (walk backwards from write_index) */
        if (i < monitor->write_index) {
            idx = monitor->write_index - 1 - i;
        } else {
            idx = monitor->buffer_size - (i - monitor->write_index) - 1;
        }

        samples[i] = monitor->samples[idx];
    }

    return 0;
}

/**
 * Reset the bandwidth monitor
 */
void bandwidth_monitor_reset(BandwidthMonitor *monitor) {
    if (!monitor) {
        return;
    }

    monitor->sample_count = 0;
    monitor->write_index = 0;
    monitor->has_baseline = 0;
    memset(&monitor->baseline, 0, sizeof(monitor->baseline));
    memset(monitor->samples, 0, monitor->buffer_size * sizeof(BandwidthSample));
}

/**
 * Free bandwidth monitor
 */
void bandwidth_monitor_free(BandwidthMonitor *monitor) {
    if (!monitor) {
        return;
    }

    free(monitor->session_path);
    free(monitor->device_name);
    free(monitor->samples);
    free(monitor);
}

/**
 * Get session start time
 */
time_t bandwidth_monitor_get_start_time(BandwidthMonitor *monitor) {
    if (!monitor) {
        return 0;
    }

    return monitor->start_time;
}

/**
 * Get current statistics source
 */
StatsSource bandwidth_monitor_get_source(BandwidthMonitor *monitor) {
    if (!monitor) {
        return STATS_SOURCE_AUTO;
    }

    return monitor->source;
}
