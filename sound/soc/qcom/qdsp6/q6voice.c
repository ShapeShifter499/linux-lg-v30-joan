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

#define SESSION_NAME_LEN		20
#define VOICEMMODE1_NAME		"11C05000"
#define Q6VOICE_TIMEOUT_MS		1000

struct vss_imvm_cmd_create_control_session {
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

static int q6voice_create_mvm_session(const char *name)
{
	struct vss_imvm_cmd_create_control_session *session;
	struct apr_pkt *pkt;
	int pkt_size, ret;

	pkt_size = APR_HDR_SIZE + sizeof(*session);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	session = p + APR_HDR_SIZE;

	pkt->hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.src_port = 0;
	pkt->hdr.dest_port = 0;
	pkt->hdr.token = 0;
	pkt->hdr.opcode = VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION;

	strscpy(session->name, name, sizeof(session->name));

	ret = q6voice_send_wait(&q6voice_mvm, pkt);
	if (ret)
		return ret;

	dev_info(&q6voice_mvm.adev->dev,
		 "q6voice: MVM session \"%s\" created, handle 0x%04x\n",
		 name, q6voice_mvm.handle);

	return 0;
}

static int q6voice_start_set(void *data, u64 val)
{
	if (!val)
		return 0;

	return q6voice_create_mvm_session(VOICEMMODE1_NAME);
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
