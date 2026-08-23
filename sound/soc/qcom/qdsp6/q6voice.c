// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm Core Voice Driver (CVD) client - session bring-up probe
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
 * This first step registers the three services and creates an MVM session, to
 * establish that mainline's APR bus can reach CVD at all.  Sessions are named
 * with an ASCII string rather than a numeric VSID - "11C05000" is
 * VOICEMMODE1, the multimode session stock uses for VoLTE.
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
	struct mutex lock;
	bool resp_received;
	int status;
	u16 handle;
};

static struct q6voice_svc q6voice_mvm;
static struct q6voice_svc q6voice_cvs;
static struct q6voice_svc q6voice_cvp;

static struct dentry *q6voice_debugfs;

/*
 * Which AFE ports and topologies the vocproc is built against.  Exposed as
 * parameters because the right answer depends on how the board routes voice,
 * and that is still being brought up here.
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
	init_waitqueue_head(&svc->wait);
	mutex_init(&svc->lock);
	svc->adev = adev;
	dev_set_drvdata(&adev->dev, svc);

	dev_info(&adev->dev, "q6voice: %s bound (svc 0x%02x domain 0x%02x)\n",
		 adev->name, adev->svc_id, adev->domain_id);

	return 0;
}

/*
 * A create-session command is answered by APR_BASIC_RSP_RESULT.  The handle
 * assigned to the new session arrives as the source port of that response,
 * not in the payload.
 */
static int q6voice_callback_svc(struct apr_device *adev,
				const struct apr_resp_pkt *data,
				struct q6voice_svc *svc)
{
	struct aprv2_ibasic_rsp_result_t {
		u32 opcode;
		u32 status;
	} *result;

	dev_info(&adev->dev, "q6voice: <- op 0x%08x src 0x%04x size %d\n",
		 data->hdr.opcode, data->hdr.src_port, data->payload_size);

	if (data->hdr.opcode != APR_BASIC_RSP_RESULT)
		return 0;

	if (data->payload_size < sizeof(*result))
		return 0;

	result = data->payload;

	svc->status = result->status;
	svc->handle = data->hdr.src_port;
	svc->resp_received = true;
	wake_up(&svc->wait);

	dev_info(&adev->dev,
		 "q6voice: rsp op 0x%08x status 0x%08x handle 0x%04x\n",
		 result->opcode, result->status, svc->handle);

	return 0;
}

static int q6voice_send_wait(struct q6voice_svc *svc, struct apr_pkt *pkt)
{
	int ret;

	if (!svc->adev)
		return -ENODEV;

	guard(mutex)(&svc->lock);

	svc->resp_received = false;
	svc->status = 0;

	dev_info(&svc->adev->dev, "q6voice: -> op 0x%08x dest 0x%04x len %u\n",
		 pkt->hdr.opcode, pkt->hdr.dest_port, pkt->hdr.pkt_size);

	ret = apr_send_pkt(svc->adev, pkt);
	if (ret < 0) {
		dev_err(&svc->adev->dev, "q6voice: send failed: %d\n", ret);
		return ret;
	}

	ret = wait_event_timeout(svc->wait, svc->resp_received,
				 msecs_to_jiffies(Q6VOICE_TIMEOUT_MS));
	if (!ret) {
		dev_err(&svc->adev->dev, "q6voice: timeout waiting for ADSP\n");
		return -ETIMEDOUT;
	}

	if (svc->status) {
		dev_err(&svc->adev->dev, "q6voice: ADSP error 0x%08x\n",
			svc->status);
		return -EINVAL;
	}

	return 0;
}

/*
 * Build and send one CVD command.  @dest is the handle of the session being
 * addressed, or 0 when creating a session.
 */
static int q6voice_cmd(struct q6voice_svc *svc, u32 opcode, u16 dest,
		       const void *payload, size_t payload_size)
{
	struct apr_pkt *pkt;
	int pkt_size;

	pkt_size = APR_HDR_SIZE + payload_size;

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	pkt->hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.src_port = 0;
	pkt->hdr.dest_port = dest;
	pkt->hdr.token = 0;
	pkt->hdr.opcode = opcode;

	if (payload_size)
		memcpy(p + APR_HDR_SIZE, payload, payload_size);

	return q6voice_send_wait(svc, pkt);
}

static int q6voice_create_session(struct q6voice_svc *svc, u32 opcode,
				  const char *name)
{
	struct vss_imvm_cmd_create_control_session session = {};
	int ret;

	strscpy(session.name, name, sizeof(session.name));

	ret = q6voice_cmd(svc, opcode, 0, &session, sizeof(session));
	if (ret)
		return ret;

	dev_info(&svc->adev->dev, "q6voice: session \"%s\" created, handle 0x%04x\n",
		 name, svc->handle);

	return 0;
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
	int ret;

	strscpy(cvp.name, name, sizeof(cvp.name));

	ret = q6voice_cmd(&q6voice_cvp,
			  VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2, 0,
			  &cvp, sizeof(cvp));
	if (ret)
		return ret;

	dev_info(&q6voice_cvp.adev->dev,
		 "q6voice: vocproc created, handle 0x%04x\n", q6voice_cvp.handle);

	return 0;
}

/*
 * Bring up a voice session end to end.  The order matters: the modem owns the
 * state machine for a passive session, so every object has to exist and be
 * attached before START_VOICE.
 */
static int q6voice_start_voice(const char *name)
{
	int ret;

	if (!q6voice_mvm.adev || !q6voice_cvs.adev || !q6voice_cvp.adev)
		return -ENODEV;

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

	dev_info(&q6voice_mvm.adev->dev, "q6voice: voice session started\n");

	return 0;
}

static int q6voice_stop_voice(void)
{
	if (!q6voice_mvm.adev || !q6voice_mvm.handle)
		return -ENODEV;

	return q6voice_cmd(&q6voice_mvm, VSS_IMVM_CMD_STOP_VOICE,
			   q6voice_mvm.handle, NULL, 0);
}

static int q6voice_create_set(void *data, u64 val)
{
	if (!val)
		return 0;

	if (!q6voice_mvm.adev)
		return -ENODEV;

	return q6voice_create_session(&q6voice_mvm,
				      VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION,
				      VOICEMMODE1_NAME);
}
DEFINE_DEBUGFS_ATTRIBUTE(q6voice_create_fops, NULL, q6voice_create_set, "%llu\n");

static int q6voice_start_set(void *data, u64 val)
{
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

static int q6voice_bound_get(void *data, u64 *val)
{
	*val = (q6voice_mvm.adev ? BIT(0) : 0) |
	       (q6voice_cvs.adev ? BIT(1) : 0) |
	       (q6voice_cvp.adev ? BIT(2) : 0);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(q6voice_bound_fops, q6voice_bound_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(q6voice_handle_fops, q6voice_handle_get, NULL, "%llu\n");

static void q6voice_debugfs_init(void)
{
	if (q6voice_debugfs)
		return;

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

#define Q6VOICE_SVC_DRIVER(_lname, _uname, _compat)			\
static int q6##_lname##_probe(struct apr_device *adev)			\
{									\
	return q6voice_probe_svc(adev, &q6voice_##_lname);		\
}									\
static int q6##_lname##_callback(struct apr_device *adev,		\
				 const struct apr_resp_pkt *data)	\
{									\
	return q6voice_callback_svc(adev, data, &q6voice_##_lname);	\
}									\
static const struct of_device_id q6##_lname##_device_id[] = {		\
	{ .compatible = _compat },					\
	{}								\
};									\
MODULE_DEVICE_TABLE(of, q6##_lname##_device_id);			\
static struct apr_driver q6##_lname##_driver = {			\
	.probe = q6##_lname##_probe,					\
	.callback = q6##_lname##_callback,				\
	.driver = {							\
		.name = "qcom-q6" #_lname,				\
		.of_match_table = of_match_ptr(q6##_lname##_device_id),	\
	},								\
}

Q6VOICE_SVC_DRIVER(mvm, MVM, "qcom,q6mvm");
Q6VOICE_SVC_DRIVER(cvs, CVS, "qcom,q6cvs");
Q6VOICE_SVC_DRIVER(cvp, CVP, "qcom,q6cvp");

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

	for (i = ARRAY_SIZE(q6voice_drivers); i--; )
		apr_driver_unregister(q6voice_drivers[i]);

	debugfs_remove_recursive(q6voice_debugfs);
	q6voice_debugfs = NULL;
}
module_exit(q6voice_exit);

MODULE_DESCRIPTION("Q6 Core Voice Driver client");
MODULE_LICENSE("GPL v2");
