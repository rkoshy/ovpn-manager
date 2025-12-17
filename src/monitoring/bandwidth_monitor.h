#ifndef BANDWIDTH_MONITOR_H
#define BANDWIDTH_MONITOR_H

#include <stdint.h>
#include <time.h>
#include <systemd/sd-bus.h>

/**
 * Bandwidth statistics for a single sample
 */
typedef struct {
    time_t timestamp;          /* When this sample was taken */
    uint64_t bytes_in;         /* Total bytes received */
    uint64_t bytes_out;        /* Total bytes sent */
    uint64_t packets_in;       /* Total packets received */
    uint64_t packets_out;      /* Total packets sent */
    uint64_t errors_in;        /* Total receive errors */
    uint64_t errors_out;       /* Total transmit errors */
    uint64_t dropped_in;       /* Total received packets dropped */
    uint64_t dropped_out;      /* Total transmitted packets dropped */
} BandwidthSample;

/**
 * Bandwidth rate (derived from samples)
 */
typedef struct {
    double upload_rate_bps;    /* Upload rate in bytes per second */
    double download_rate_bps;  /* Download rate in bytes per second */
    uint64_t total_uploaded;   /* Total bytes uploaded this session */
    uint64_t total_downloaded; /* Total bytes downloaded this session */
} BandwidthRate;

/**
 * Opaque structure for bandwidth monitor
 */
typedef struct BandwidthMonitor BandwidthMonitor;

/**
 * Statistics source
 */
typedef enum {
    STATS_SOURCE_DBUS,         /* Get statistics from OpenVPN3 D-Bus */
    STATS_SOURCE_SYSFS,        /* Get statistics from /sys/class/net/ */
    STATS_SOURCE_AUTO          /* Auto-detect best source */
} StatsSource;

/**
 * Create a new bandwidth monitor for a VPN session
 *
 * @param session_path D-Bus path to session (for D-Bus stats)
 * @param device_name Device name (e.g., "tun0" for sysfs stats)
 * @param source Statistics source to use
 * @param buffer_size Number of samples to keep in rolling buffer (default: 60)
 * @return New BandwidthMonitor or NULL on error
 */
BandwidthMonitor* bandwidth_monitor_create(const char *session_path,
                                           const char *device_name,
                                           StatsSource source,
                                           unsigned int buffer_size);

/**
 * Update statistics from the data source
 *
 * This should be called periodically (e.g., every 1 second) to sample
 * bandwidth statistics.
 *
 * @param monitor The bandwidth monitor
 * @param bus D-Bus connection (required if source is DBUS or AUTO)
 * @return 0 on success, negative on error
 */
int bandwidth_monitor_update(BandwidthMonitor *monitor, sd_bus *bus);

/**
 * Get the latest bandwidth rate
 *
 * @param monitor The bandwidth monitor
 * @param rate Output: Current bandwidth rate (calculated from last 2 samples)
 * @return 0 on success, negative on error
 */
int bandwidth_monitor_get_rate(BandwidthMonitor *monitor, BandwidthRate *rate);

/**
 * Get the latest sample
 *
 * @param monitor The bandwidth monitor
 * @param sample Output: Latest sample
 * @return 0 on success, negative on error
 */
int bandwidth_monitor_get_latest_sample(BandwidthMonitor *monitor,
                                        BandwidthSample *sample);

/**
 * Get historical samples from the rolling buffer
 *
 * @param monitor The bandwidth monitor
 * @param samples Output array: Array to fill with samples (newest first)
 * @param max_samples Maximum number of samples to return
 * @param count Output: Actual number of samples returned
 * @return 0 on success, negative on error
 */
int bandwidth_monitor_get_samples(BandwidthMonitor *monitor,
                                  BandwidthSample *samples,
                                  unsigned int max_samples,
                                  unsigned int *count);

/**
 * Reset the bandwidth monitor (clear all samples)
 *
 * @param monitor The bandwidth monitor
 */
void bandwidth_monitor_reset(BandwidthMonitor *monitor);

/**
 * Free bandwidth monitor and all resources
 *
 * @param monitor The bandwidth monitor
 */
void bandwidth_monitor_free(BandwidthMonitor *monitor);

/**
 * Get session start time (for calculating connection duration)
 *
 * @param monitor The bandwidth monitor
 * @return Timestamp when monitoring started (0 if no samples)
 */
time_t bandwidth_monitor_get_start_time(BandwidthMonitor *monitor);

/**
 * Get current statistics source being used
 *
 * @param monitor The bandwidth monitor
 * @return Current statistics source
 */
StatsSource bandwidth_monitor_get_source(BandwidthMonitor *monitor);

#endif /* BANDWIDTH_MONITOR_H */
