/*
 * NML Edge Worker — compile-time configuration
 * Override any define via -D on the compiler command line.
 */

#ifndef EDGE_CONFIG_H
#define EDGE_CONFIG_H

/* Agent identity — override with -DEDGE_AGENT_NAME='"my_edge"' */
#ifndef EDGE_AGENT_NAME
#define EDGE_AGENT_NAME "edge_1"
#endif

/* HTTP port advertised in ANNOUNCE/HEARTBEAT messages.
   The edge worker does not serve HTTP; this is informational only. */
#ifndef EDGE_HTTP_PORT
#define EDGE_HTTP_PORT 9099
#endif

/* UDP multicast — must match nml_collective.py */
#define UDP_MULTICAST_GROUP "239.78.77.76"
#define UDP_MULTICAST_PORT  7776

/* Max compact program bytes receivable in a single UDP payload.
   Python allows up to 65000 bytes (UDP_MAX_PACKET) but typical programs
   compact to 500-700 bytes. Raise if larger programs are used;
   keep under available RAM on the target MCU. */
#ifndef NML_MAX_PROGRAM_LEN
#define NML_MAX_PROGRAM_LEN 4096
#endif

/* NML execution cycle limit */
#ifndef NML_MAX_CYCLES
#define NML_MAX_CYCLES 1000000
#endif

/* Heartbeat interval (seconds) — must match HEARTBEAT_INTERVAL in nml_collective.py */
#define HEARTBEAT_INTERVAL 5

/* Result reporting: 0 = UDP multicast only, 1 = HTTP POST to sentient */
#ifndef USE_HTTP_REPORT
#define USE_HTTP_REPORT 0
#endif

/* HTTP POST URL when USE_HTTP_REPORT=1.
   Example: "http://192.168.1.10:9001/result" */
#define HTTP_REPORT_URL NULL

/* Score key priority order — first match found in named memory wins.
   Must mirror the priority in nml_collective.py:broadcast_program(). */
#define SCORE_KEYS { "fraud_score", "risk_score", "score", "result", NULL }

/* Compile-time machine UID string — last-resort fallback for identity.c.
   Used when /etc/machine-id is unavailable (Linux containers without systemd)
   or when no arm_edge_hardware_uid() BSP hook is provided.
   Override per-node with -DEDGE_MACHINE_ID_STR='"sensor_node_7"'.
   WARNING: nodes sharing the same string will collide on the Enforcer's
   one-node-per-machine rule. Each physical node must have a unique value. */
#ifndef EDGE_MACHINE_ID_STR
#define EDGE_MACHINE_ID_STR "nml_edge_default_uid"
#endif

#endif /* EDGE_CONFIG_H */
