// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm Core Voice Driver (CVD) client
 *
 * Mainline has no voice-call support for Qualcomm ADSPs: sound/soc/qcom/qdsp6/
 * implements the audio data path (ASM/AFE/ADM) only.  Voice calls - CS and
 * VoLTE alike - run on three further APR services on the same ADSP domain:
 *
 *   MVM  (0x09)  Multimode Voice Manager - owns the session, starts/stops it
 *   CVS  (0x0A)  Core Voice Stream      - vocoder encode/decode
 *   CVP  (0x0B)  Core Voice Processor   - device/topology, routes to the AFE
 *
 * For VoLTE the modem's IMS stack terminates RTP and the ADSP vocodes, so the
 * AP never touches media; it only has to build the session and route PCM.
 *
 * This client builds, starts and tears down a complete session.  Sessions are
 * named with an ASCII string rather than a numeric VSID - "11C05000" is
 * VOICEMMODE1, the multimode session stock uses for VoLTE.  There is no ALSA
 * front-end yet; the session is driven from debugfs.
 *
 * Copyright (c) 2026 Lance <Gero3977@gmail.com>
 */

#include <linux/debugfs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/soc/qcom/apr.h>
#include <linux/wait.h>

#define VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION	0x000110FF
#define VSS_IMVM_CMD_CREATE_FULL_CONTROL_SESSION	0x000110FE
#define VSS_IMVM_CMD_START_VOICE			0x00011190
#define VSS_IMVM_CMD_STOP_VOICE				0x00011192

#define VSS_ISTREAM_CMD_CREATE_PASSIVE_CONTROL_SESSION	0x00011140
#define VSS_IMVM_CMD_ATTACH_STREAM			0x0001123C
#define VSS_IMVM_CMD_DETACH_STREAM			0x0001123D
#define VSS_IMVM_CMD_ATTACH_VOCPROC			0x0001123E
#define VSS_IMVM_CMD_DETACH_VOCPROC			0x0001123F
#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2	0x000112BF
#define VSS_IVOCPROC_CMD_ENABLE				0x000100C6
#define VSS_IVOCPROC_CMD_DISABLE			0x000110E1
#define APRV2_IBASIC_CMD_DESTROY_SESSION		0x0001003C

#define VSS_IVOCPROC_DIRECTION_RX_TX			2
#define VSS_IVOCPROC_PORT_ID_NONE			0xFFFF
#define VSS_IVOCPROC_TOPOLOGY_ID_NONE			0x00010F70
#define VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS		0x00010F71
#define VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT		0x00010F77

/* AFE port IDs.  The WCD codec - and so the earpiece and the call mic - is
 * behind SLIMbus port 0 on this board.
 */
#define AFE_PORT_ID_SLIMBUS_0_RX			0x4000
#define AFE_PORT_ID_SLIMBUS_0_TX			0x4001
#define VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING		0x00010F7C
#define VSS_ICOMMON_CAL_NETWORK_ID_NONE			0x0001135E

#define SESSION_NAME_LEN		20
#define VOICEMMODE1_NAME		"11C05000"
#define Q6VOICE_TIMEOUT_MS		5000

struct vss_imvm_cmd_create_control_session {
	char name[SESSION_NAME_LEN];
} __packed;

/* Both MVM and CVS attach commands carry a bare session handle. */
struct vss_cmd_attach_handle {
	u16 handle;
} __packed;

struct vss_ivocproc_cmd_create_full_control_session_v2 {
	u16 direction;
	u16 tx_port_id;
	u32 tx_topology_id;
	u16 rx_port_id;
	u32 rx_topology_id;
	u32 profile_id;
	u32 vocproc_mode;
	u16 ec_ref_port_id;
	char name[SESSION_NAME_LEN];
} __packed;

/* One APR service endpoint (MVM, CVS or CVP). */
struct q6voice_svc {
	struct apr_device *adev;
	wait_queue_head_t wait;

	/* The command in flight, and what its response reported. */
	u32 opcode;
	u32 token;
	bool resp_received;
	u32 status;
	u16 resp_port;

	/* Handle of the session this service holds, or 0 for none. */
	u16 handle;
};

static struct q6voice_svc q6voice_mvm;
static struct q6voice_svc q6voice_cvs;
static struct q6voice_svc q6voice_cvp;

/*
 * Serialises every command sequence against the others and against unbind:
 * building or tearing down a session takes several commands across all three
 * services, and none of them may be interleaved with another sequence.
 */
static DEFINE_MUTEX(q6voice_lock);

static struct dentry *q6voice_debugfs;

/* Whether a session has been started, so teardown can be idempotent. */
static bool q6voice_session_up;

/*
 * Which AFE ports and topologies the vocproc is built against.  Exposed as
 * parameters because the right answer depends on how the board routes voice.
 */
static u16 rx_port = AFE_PORT_ID_SLIMBUS_0_RX;
module_param(rx_port, ushort, 0644);
MODULE_PARM_DESC(rx_port, "AFE Rx port ID for the vocproc");

static u16 tx_port = AFE_PORT_ID_SLIMBUS_0_TX;
module_param(tx_port, ushort, 0644);
MODULE_PARM_DESC(tx_port, "AFE Tx port ID for the vocproc");

static uint rx_topology = VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT;
module_param(rx_topology, uint, 0644);
MODULE_PARM_DESC(rx_topology, "Rx vocproc topology ID");

static uint tx_topology = VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS;
module_param(tx_topology, uint, 0644);
MODULE_PARM_DESC(tx_topology, "Tx vocproc topology ID");

static int q6voice_probe_svc(struct apr_device *adev, struct q6voice_svc *svc)
{
	guard(mutex)(&q6voice_lock);

	init_waitqueue_head(&svc->wait);
	svc->handle = 0;
	svc->adev = adev;
	dev_set_drvdata(&adev->dev, svc);

	return 0;
}

/*
 * Drop the binding before the bus tears the APR device down.  Taking the lock
 * waits out any command sequence still using the endpoint, and every later one
 * sees ->adev cleared and fails with -ENODEV.  Whatever session the ADSP still
 * holds for this service is lost with the binding.
 */
static void q6voice_remove_svc(struct q6voice_svc *svc)
{
	guard(mutex)(&q6voice_lock);

	svc->adev = NULL;
	svc->handle = 0;
	if (svc == &q6voice_mvm)
		q6voice_session_up = false;
}

/*
 * Every command is answered by APR_BASIC_RSP_RESULT, which echoes the command
 * opcode and the token it was sent with.  Anything else, or a result for some
 * other command, is not the answer being waited for.  For a create command,
 * the handle of the new session arrives as the source port of the response.
 */
static int q6voice_callback_svc(struct apr_device *adev,
				const struct apr_resp_pkt *data,
				struct q6voice_svc *svc)
{
	const struct aprv2_ibasic_rsp_result_t *result;

	if (data->hdr.opcode != APR_BASIC_RSP_RESULT ||
	    data->payload_size < sizeof(*result)) {
		dev_dbg(&adev->dev, "unexpected opcode %#x, size %d\n",
			data->hdr.opcode, data->payload_size);
		return 0;
	}

	result = data->payload;
	if (result->opcode != svc->opcode || data->hdr.token != svc->token) {
		dev_dbg(&adev->dev, "stale result for %#x (token %#x)\n",
			result->opcode, data->hdr.token);
		return 0;
	}

	svc->status = result->status;
	svc->resp_port = data->hdr.src_port;
	svc->resp_received = true;
	wake_up(&svc->wait);

	return 0;
}

/*
 * Build and send one CVD command, and wait for its result.  @dest is the
 * handle of the session being addressed, or 0 when creating a session.
 * Called with q6voice_lock held.
 */
static int q6voice_cmd(struct q6voice_svc *svc, u32 opcode, u16 dest,
		       const void *payload, size_t payload_size)
{
	size_t pkt_size = APR_HDR_SIZE + payload_size;
	struct apr_pkt *pkt;
	int ret;

	lockdep_assert_held(&q6voice_lock);

	if (!svc->adev)
		return -ENODEV;

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	svc->opcode = opcode;
	svc->token++;
	svc->resp_received = false;

	pkt = p;
	pkt->hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.src_port = 0;
	pkt->hdr.dest_port = dest;
	pkt->hdr.token = svc->token;
	pkt->hdr.opcode = opcode;

	if (payload_size)
		memcpy(p + APR_HDR_SIZE, payload, payload_size);

	ret = apr_send_pkt(svc->adev, pkt);
	if (ret < 0) {
		dev_err(&svc->adev->dev, "failed to send %#x: %d\n", opcode, ret);
		return ret;
	}

	ret = wait_event_timeout(svc->wait, svc->resp_received,
				 msecs_to_jiffies(Q6VOICE_TIMEOUT_MS));
	if (!ret) {
		dev_err(&svc->adev->dev, "timeout waiting for %#x\n", opcode);
		return -ETIMEDOUT;
	}

	if (svc->status) {
		dev_err(&svc->adev->dev, "%#x failed: ADSP error %#x\n",
			opcode, svc->status);
		return -EINVAL;
	}

	return 0;
}

static int q6voice_create(struct q6voice_svc *svc, u32 opcode,
			  const void *payload, size_t payload_size)
{
	int ret;

	ret = q6voice_cmd(svc, opcode, 0, payload, payload_size);
	if (ret)
		return ret;

	svc->handle = svc->resp_port;
	dev_dbg(&svc->adev->dev, "session created, handle %#06x\n", svc->handle);

	return 0;
}

static int q6voice_create_session(struct q6voice_svc *svc, u32 opcode,
				  const char *name)
{
	struct vss_imvm_cmd_create_control_session session = {};

	strscpy(session.name, name, sizeof(session.name));

	return q6voice_create(svc, opcode, &session, sizeof(session));
}

static int q6voice_attach(struct q6voice_svc *svc, u32 opcode, u16 dest,
			  u16 handle)
{
	struct vss_cmd_attach_handle attach = { .handle = handle };

	return q6voice_cmd(svc, opcode, dest, &attach, sizeof(attach));
}

/*
 * Build the vocproc against the AFE ports the board routes voice over.  The
 * ADSP will happily create a vocproc with no ports but refuses to enable one,
 * so the ports have to be real even before any audio flows through them.
 */
static int q6voice_create_vocproc(const char *name)
{
	struct vss_ivocproc_cmd_create_full_control_session_v2 cvp = {
		.direction	= VSS_IVOCPROC_DIRECTION_RX_TX,
		.tx_port_id	= tx_port,
		.tx_topology_id	= tx_topology,
		.rx_port_id	= rx_port,
		.rx_topology_id	= rx_topology,
		.profile_id	= VSS_ICOMMON_CAL_NETWORK_ID_NONE,
		.vocproc_mode	= VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING,
		.ec_ref_port_id	= VSS_IVOCPROC_PORT_ID_NONE,
	};

	strscpy(cvp.name, name, sizeof(cvp.name));

	return q6voice_create(&q6voice_cvp,
			      VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2,
			      &cvp, sizeof(cvp));
}

/*
 * Bring up a voice session end to end.  The order matters: the modem owns the
 * state machine for a passive session, so every object has to exist and be
 * attached before START_VOICE.
 */
static int q6voice_build_session(const char *name)
{
	int ret;

	ret = q6voice_create_session(&q6voice_mvm,
				     VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION,
				     name);
	if (ret)
		return ret;

	ret = q6voice_create_session(&q6voice_cvs,
				     VSS_ISTREAM_CMD_CREATE_PASSIVE_CONTROL_SESSION,
				     name);
	if (ret)
		return ret;

	ret = q6voice_attach(&q6voice_mvm, VSS_IMVM_CMD_ATTACH_STREAM,
			     q6voice_mvm.handle, q6voice_cvs.handle);
	if (ret)
		return ret;

	ret = q6voice_create_vocproc(name);
	if (ret)
		return ret;

	ret = q6voice_cmd(&q6voice_cvp, VSS_IVOCPROC_CMD_ENABLE,
			  q6voice_cvp.handle, NULL, 0);
	if (ret)
		return ret;

	ret = q6voice_attach(&q6voice_mvm, VSS_IMVM_CMD_ATTACH_VOCPROC,
			     q6voice_mvm.handle, q6voice_cvp.handle);
	if (ret)
		return ret;

	ret = q6voice_cmd(&q6voice_mvm, VSS_IMVM_CMD_START_VOICE,
			  q6voice_mvm.handle, NULL, 0);
	if (ret)
		return ret;

	q6voice_session_up = true;

	dev_info(&q6voice_mvm.adev->dev, "voice session started\n");

	return 0;
}

/*
 * Tear the session down in the exact reverse of the order it was built.  The
 * ADSP keeps whatever it was given: a session that is not destroyed survives
 * module unload, and a later create for the same VSID then goes unanswered
 * rather than being refused - which presents as a timeout on a command that
 * worked a moment earlier.
 *
 * Every step is attempted even if an earlier one fails; leaving an object
 * behind is worse than a failed command, because it is what poisons the next
 * create.
 */
static int q6voice_stop_voice(void)
{
	int ret, err = 0;

	lockdep_assert_held(&q6voice_lock);

	if (!q6voice_mvm.adev)
		return -ENODEV;

	if (!q6voice_session_up && !q6voice_mvm.handle &&
	    !q6voice_cvs.handle && !q6voice_cvp.handle)
		return 0;

	if (q6voice_session_up && q6voice_mvm.handle) {
		ret = q6voice_cmd(&q6voice_mvm, VSS_IMVM_CMD_STOP_VOICE,
				  q6voice_mvm.handle, NULL, 0);
		if (ret)
			err = ret;
	}

	if (q6voice_mvm.handle && q6voice_cvp.handle) {
		ret = q6voice_attach(&q6voice_mvm, VSS_IMVM_CMD_DETACH_VOCPROC,
				     q6voice_mvm.handle, q6voice_cvp.handle);
		if (ret)
			err = ret;
	}

	if (q6voice_cvp.handle) {
		ret = q6voice_cmd(&q6voice_cvp, VSS_IVOCPROC_CMD_DISABLE,
				  q6voice_cvp.handle, NULL, 0);
		if (ret)
			err = ret;

		ret = q6voice_cmd(&q6voice_cvp, APRV2_IBASIC_CMD_DESTROY_SESSION,
				  q6voice_cvp.handle, NULL, 0);
		if (ret)
			err = ret;
		else
			q6voice_cvp.handle = 0;
	}

	if (q6voice_mvm.handle && q6voice_cvs.handle) {
		ret = q6voice_attach(&q6voice_mvm, VSS_IMVM_CMD_DETACH_STREAM,
				     q6voice_mvm.handle, q6voice_cvs.handle);
		if (ret)
			err = ret;
	}

	if (q6voice_cvs.handle) {
		ret = q6voice_cmd(&q6voice_cvs, APRV2_IBASIC_CMD_DESTROY_SESSION,
				  q6voice_cvs.handle, NULL, 0);
		if (ret)
			err = ret;
		else
			q6voice_cvs.handle = 0;
	}

	if (q6voice_mvm.handle) {
		ret = q6voice_cmd(&q6voice_mvm, APRV2_IBASIC_CMD_DESTROY_SESSION,
				  q6voice_mvm.handle, NULL, 0);
		if (ret)
			err = ret;
		else
			q6voice_mvm.handle = 0;
	}

	q6voice_session_up = false;

	dev_info(&q6voice_mvm.adev->dev, "voice session torn down%s\n",
		 err ? " (with errors)" : "");

	return err;
}

static int q6voice_start_voice(const char *name)
{
	int ret;

	lockdep_assert_held(&q6voice_lock);

	if (!q6voice_mvm.adev || !q6voice_cvs.adev || !q6voice_cvp.adev)
		return -ENODEV;

	if (q6voice_session_up)
		return -EBUSY;

	ret = q6voice_build_session(name);
	if (ret) {
		/*
		 * Whatever was created before the failure still exists on the
		 * ADSP and would poison the next attempt, so unwind it here.
		 */
		q6voice_stop_voice();
		return ret;
	}

	return 0;
}

static int q6voice_create_set(void *data, u64 val)
{
	if (!val)
		return 0;

	guard(mutex)(&q6voice_lock);

	if (q6voice_mvm.handle)
		return -EBUSY;

	return q6voice_create_session(&q6voice_mvm,
				      VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION,
				      VOICEMMODE1_NAME);
}
DEFINE_DEBUGFS_ATTRIBUTE(q6voice_create_fops, NULL, q6voice_create_set, "%llu\n");

static int q6voice_start_set(void *data, u64 val)
{
	guard(mutex)(&q6voice_lock);

	if (val)
		return q6voice_start_voice(VOICEMMODE1_NAME);

	return q6voice_stop_voice();
}
DEFINE_DEBUGFS_ATTRIBUTE(q6voice_start_fops, NULL, q6voice_start_set, "%llu\n");

static int q6voice_handle_get(void *data, u64 *val)
{
	*val = q6voice_mvm.handle;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(q6voice_handle_fops, q6voice_handle_get, NULL, "%llu\n");

static int q6voice_bound_get(void *data, u64 *val)
{
	*val = (q6voice_mvm.adev ? BIT(0) : 0) |
	       (q6voice_cvs.adev ? BIT(1) : 0) |
	       (q6voice_cvp.adev ? BIT(2) : 0);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(q6voice_bound_fops, q6voice_bound_get, NULL, "%llu\n");

static void q6voice_debugfs_init(void)
{
	q6voice_debugfs = debugfs_create_dir("q6voice", NULL);
	debugfs_create_file("create_session", 0200, q6voice_debugfs, NULL,
			    &q6voice_create_fops);
	debugfs_create_file("start_voice", 0200, q6voice_debugfs, NULL,
			    &q6voice_start_fops);
	debugfs_create_file("mvm_handle", 0400, q6voice_debugfs, NULL,
			    &q6voice_handle_fops);
	debugfs_create_file("bound", 0400, q6voice_debugfs, NULL,
			    &q6voice_bound_fops);
}

#define Q6VOICE_SVC_DRIVER(_name, _compat)				\
static int q6##_name##_probe(struct apr_device *adev)			\
{									\
	return q6voice_probe_svc(adev, &q6voice_##_name);		\
}									\
static void q6##_name##_remove(struct apr_device *adev)		\
{									\
	q6voice_remove_svc(&q6voice_##_name);				\
}									\
static int q6##_name##_callback(struct apr_device *adev,		\
				const struct apr_resp_pkt *data)	\
{									\
	return q6voice_callback_svc(adev, data, &q6voice_##_name);	\
}									\
static const struct of_device_id q6##_name##_device_id[] = {		\
	{ .compatible = _compat },					\
	{}								\
};									\
MODULE_DEVICE_TABLE(of, q6##_name##_device_id);			\
static struct apr_driver q6##_name##_driver = {			\
	.probe = q6##_name##_probe,					\
	.remove = q6##_name##_remove,					\
	.callback = q6##_name##_callback,				\
	.driver = {							\
		.name = "qcom-q6" #_name,				\
		.of_match_table = q6##_name##_device_id,		\
	},								\
}

Q6VOICE_SVC_DRIVER(mvm, "qcom,q6mvm");
Q6VOICE_SVC_DRIVER(cvs, "qcom,q6cvs");
Q6VOICE_SVC_DRIVER(cvp, "qcom,q6cvp");

static struct apr_driver * const q6voice_drivers[] = {
	&q6mvm_driver,
	&q6cvs_driver,
	&q6cvp_driver,
};

static int __init q6voice_init(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(q6voice_drivers); i++) {
		ret = apr_driver_register(q6voice_drivers[i]);
		if (ret)
			goto err;
	}

	q6voice_debugfs_init();

	return 0;

err:
	while (i--)
		apr_driver_unregister(q6voice_drivers[i]);

	return ret;
}
module_init(q6voice_init);

static void __exit q6voice_exit(void)
{
	int i;

	debugfs_remove_recursive(q6voice_debugfs);

	scoped_guard(mutex, &q6voice_lock)
		q6voice_stop_voice();

	for (i = ARRAY_SIZE(q6voice_drivers); i--; )
		apr_driver_unregister(q6voice_drivers[i]);
}
module_exit(q6voice_exit);

MODULE_DESCRIPTION("Q6 Core Voice Driver client");
MODULE_LICENSE("GPL");
