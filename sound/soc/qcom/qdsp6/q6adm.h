/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __Q6_ADM_V2_H__
#define __Q6_ADM_V2_H__

#define ADM_PATH_PLAYBACK	0x1
#define ADM_PATH_LIVE_REC	0x2
#define MAX_COPPS_PER_PORT	8
#define NULL_COPP_TOPOLOGY	0x00010312

/*
 * ADM COPP topology IDs.  The processing chain each one names lives in the
 * ADSP firmware; ACDB calibration only tunes it, so selecting a topology is
 * enough to instantiate the modules with their firmware defaults.
 */
#define DEFAULT_COPP_TOPOLOGY			0x00010314
#define VPM_TX_SM_ECNS_COPP_TOPOLOGY		0x00010F71
#define VPM_TX_DM_FLUENCE_COPP_TOPOLOGY		0x00010F72
#define VPM_TX_QMIC_FLUENCE_COPP_TOPOLOGY	0x00010F75
#define VPM_TX_DM_RFECNS_COPP_TOPOLOGY		0x00010F86

/* multiple copp per stream. */
struct route_payload {
	int num_copps;
	int session_id;
	int copp_idx[MAX_COPPS_PER_PORT];
	int port_id[MAX_COPPS_PER_PORT];
};

struct q6copp;
struct q6copp *q6adm_open(struct device *dev, int port_id, int path, int rate,
			   int channel_mode, int topology, int perf_mode,
			   uint16_t bit_width, int app_type, int acdb_id);
int q6adm_close(struct device *dev, struct q6copp *copp);
int q6adm_get_copp_id(struct q6copp *copp);
int q6adm_matrix_map(struct device *dev, int path,
		     struct route_payload payload_map, int perf_mode);

#endif /* __Q6_ADM_V2_H__ */
