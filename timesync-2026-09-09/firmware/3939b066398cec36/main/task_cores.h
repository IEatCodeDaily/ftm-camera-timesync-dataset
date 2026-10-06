#ifndef TASK_CORES_H
#define TASK_CORES_H

/* Deterministic ESP32-S3 application task placement.
 *
 * Core 0: Wi-Fi/FTM, UDP coordination, control, discovery, OTA, TinyUSB.
 * Core 1: camera server, synchronized capture, UVC frames, native-Y8 capture
 * and centroid detection. LIVE_TRACK drains/copies camera frames in a bounded
 * core-0 producer (below Wi-Fi system-task priority) while the exact-pixel
 * detector remains isolated on core 1. */
#define FTMCS_CONTROL_CORE 0
#define FTMCS_CAMERA_CORE  1

#endif /* TASK_CORES_H */
