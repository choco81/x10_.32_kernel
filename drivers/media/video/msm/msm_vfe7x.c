/* Copyright (c) 2009, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/msm_adsp.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/android_pmem.h>
#include <mach/msm_adsp.h>
#include <mach/clk.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/clk.h>
#include <mach/msm_adsp.h>
#include "msm_vfe7x.h"
#include <linux/pm_qos_params.h>

#define QDSP_CMDQUEUE 25

#define VFE_RESET_CMD 0
#define VFE_START_CMD 1
#define VFE_STOP_CMD  2
#define VFE_FRAME_ACK 20
#define STATS_AF_ACK  21
#define STATS_WE_ACK  22

#define MSG_STOP_ACK  1
#define MSG_SNAPSHOT  2
#define MSG_OUTPUT1   6
#define MSG_OUTPUT2   7
#define MSG_STATS_AF  8
#define MSG_STATS_WE  9

#define VFE_ADSP_EVENT 0xFFFF
#define SNAPSHOT_MASK_MODE 0x00000002
#define MSM_AXI_QOS_PREVIEW		192000
#define MSM_AXI_QOS_SNAPSHOT	192000


static struct msm_adsp_module *qcam_mod;
static struct msm_adsp_module *vfe_mod;
static struct msm_vfe_callback *resp;
static void *extdata;
static uint32_t extlen;

struct mutex vfe_lock;
static void     *vfe_syncdata;
static uint8_t vfestopped;

static struct stop_event stopevent;

static struct clk *ebi1_clk;
static const char *const ebi1_clk_name = "ebi1_clk";

static void vfe_7x_convert(struct msm_vfe_phy_info *pinfo,
		enum vfe_resp_msg type,
		void *data, void **ext, int32_t *elen)
{
	switch (type) {
	case VFE_MSG_OUTPUT_P: {
		pinfo->y_phy = ((struct vfe_endframe *)data)->y_address;
		pinfo->cbcr_phy =
			((struct vfe_endframe *)data)->cbcr_address;

		pinfo->output_id = OUTPUT_TYPE_P;

		CDBG("vfe_7x_convert, y_phy = 0x%x, cbcr_phy = 0x%x\n",
				 pinfo->y_phy, pinfo->cbcr_phy);

		((struct vfe_frame_extra *)extdata)->bl_evencol =
		((struct vfe_endframe *)data)->blacklevelevencolumn;

		((struct vfe_frame_extra *)extdata)->bl_oddcol =
		((struct vfe_endframe *)data)->blackleveloddcolumn;

		((struct vfe_frame_extra *)extdata)->g_def_p_cnt =
		((struct vfe_endframe *)data)->greendefectpixelcount;

		((struct vfe_frame_extra *)extdata)->r_b_def_p_cnt =
		((struct vfe_endframe *)data)->redbluedefectpixelcount;

		*ext  = extdata;
		*elen = extlen;
	}
		break;

	case VFE_MSG_STATS_AF:
	case VFE_MSG_STATS_WE:
		pinfo->sbuf_phy = *(uint32_t *)data;
		break;

	default:
		break;
	} /* switch */
}

static void vfe_7x_ops(void *driver_data, unsigned id, size_t len,
		void (*getevent)(void *ptr, size_t len))
{
	uint32_t evt_buf[3];
	struct msm_vfe_resp *rp;
	void *data;
	CDBG("%s:id=%d\n", __func__, id);

	len = (id == VFE_ADSP_EVENT) ? 0 : len;
	data = resp->vfe_alloc(sizeof(struct msm_vfe_resp) + len,
		vfe_syncdata,  GFP_ATOMIC);

	if (!data) {
		pr_err("%s: rp: cannot allocate buffer\n", __func__);
		return;
	}
	rp = (struct msm_vfe_resp *)data;
	rp->evt_msg.len = len;

	if (id == VFE_ADSP_EVENT) {
		/* event */
		rp->type           = VFE_EVENT;
		rp->evt_msg.type   = MSM_CAMERA_EVT;
		getevent(evt_buf, sizeof(evt_buf));
		rp->evt_msg.msg_id = evt_buf[0];
	CDBG("%s:event:msg_id=%d\n", __func__, rp->evt_msg.msg_id);
		resp->vfe_resp(rp, MSM_CAM_Q_VFE_EVT, vfe_syncdata,
		GFP_ATOMIC);
	} else {
		/* messages */
		rp->evt_msg.type   = MSM_CAMERA_MSG;
		rp->evt_msg.msg_id = id;
		rp->evt_msg.data = rp + 1;
		getevent(rp->evt_msg.data, len);
	CDBG("%s:messages:msg_id=%d\n", __func__, rp->evt_msg.msg_id);

		switch (rp->evt_msg.msg_id) {
		case MSG_SNAPSHOT:
			update_axi_qos(MSM_AXI_QOS_PREVIEW);
			rp->type = VFE_MSG_SNAPSHOT;
			break;

		case MSG_OUTPUT1:
		case MSG_OUTPUT2:
			rp->type = VFE_MSG_OUTPUT_P;
			vfe_7x_convert(&(rp->phy), VFE_MSG_OUTPUT_P,
				rp->evt_msg.data, &(rp->extdata),
				&(rp->extlen));
			break;

		case MSG_STATS_AF:
			rp->type = VFE_MSG_STATS_AF;
			vfe_7x_convert(&(rp->phy), VFE_MSG_STATS_AF,
					rp->evt_msg.data, NULL, NULL);
			break;

		case MSG_STATS_WE:
			rp->type = VFE_MSG_STATS_WE;
			vfe_7x_convert(&(rp->phy), VFE_MSG_STATS_WE,
					rp->evt_msg.data, NULL, NULL);

			CDBG("MSG_STATS_WE: phy = 0x%x\n", rp->phy.sbuf_phy);
			break;

		case MSG_STOP_ACK:
			rp->type = VFE_MSG_GENERAL;
			stopevent.state = 1;
			wake_up(&stopevent.wait);
			break;


		default:
			rp->type = VFE_MSG_GENERAL;
			break;
		}
		resp->vfe_resp(rp, MSM_CAM_Q_VFE_MSG, vfe_syncdata, GFP_ATOMIC);
	}
}

static struct msm_adsp_ops vfe_7x_sync = {
	.event = vfe_7x_ops,
};

static int vfe_7x_enable(struct camera_enable_cmd *enable)
{
	int rc = -EFAULT;

	if (!strcmp(enable->name, "QCAMTASK"))
		rc = msm_adsp_enable(qcam_mod);
	else if (!strcmp(enable->name, "VFETASK"))
		rc = msm_adsp_enable(vfe_mod);

	return rc;
}

static int vfe_7x_disable(struct camera_enable_cmd *enable,
		struct platform_device *dev __attribute__((unused)))
{
	int rc = -EFAULT;

	if (!strcmp(enable->name, "QCAMTASK"))
		rc = msm_adsp_disable(qcam_mod);
	else if (!strcmp(enable->name, "VFETASK"))
		rc = msm_adsp_disable(vfe_mod);

	return rc;
}

static int vfe_7x_stop(void)
{
	int rc = 0;
	uint32_t stopcmd = VFE_STOP_CMD;
	rc = msm_adsp_write(vfe_mod, QDSP_CMDQUEUE,
				&stopcmd, sizeof(uint32_t));
	if (rc < 0) {
		CDBG("%s:%d: failed rc = %d \n", __func__, __LINE__, rc);
		return rc;
	}

	stopevent.state = 0;
	rc = wait_event_timeout(stopevent.wait,
		stopevent.state != 0,
		msecs_to_jiffies(stopevent.timeout));

	return rc;
}

static void vfe_7x_release(struct platform_device *pdev)
{
	mutex_lock(&vfe_lock);
	vfe_syncdata = NULL;
	mutex_unlock(&vfe_lock);

	if (!vfestopped) {
		CDBG("%s:%d:Calling vfe_7x_stop()\n", __func__, __LINE__);
		vfe_7x_stop();
	} else
		vfestopped = 0;

	msm_adsp_disable(qcam_mod);
	msm_adsp_disable(vfe_mod);

	msm_adsp_put(qcam_mod);
	msm_adsp_put(vfe_mod);

	msm_camio_disable(pdev);

	if (ebi1_clk) {
		clk_set_rate(ebi1_clk, 0);
		clk_put(ebi1_clk);
		ebi1_clk = 0;
	}

	kfree(extdata);
	extlen = 0;

	/* set back the AXI frequency to default */
	update_axi_qos(PM_QOS_DEFAULT_VALUE);
}

static int vfe_7x_init(struct msm_vfe_callback *presp,
	struct platform_device *dev)
{
	int rc = 0;

	init_waitqueue_head(&stopevent.wait);
	stopevent.timeout = 200;
	stopevent.state = 0;

	if (presp && presp->vfe_resp)
		resp = presp;
	else
		return -EFAULT;

	ebi1_clk = clk_get(NULL, ebi1_clk_name);
	if (!ebi1_clk) {
		pr_err("%s: could not get %s\n", __func__, ebi1_clk_name);
		return -EIO;
	}

	rc = clk_set_rate(ebi1_clk, 128000000);
	if (rc < 0) {
		pr_err("%s: clk_set_rate(%s) failed: %d\n", __func__,
			ebi1_clk_name, rc);
		return rc;
	}

	/* Bring up all the required GPIOs and Clocks */
	rc = msm_camio_enable(dev);
	if (rc < 0)
		return rc;
	msm_camio_camif_pad_reg_reset();

	extlen = sizeof(struct vfe_frame_extra);

	extdata =
		kmalloc(extlen, GFP_ATOMIC);
	if (!extdata) {
		rc = -ENOMEM;
		goto init_fail;
	}

	rc = msm_adsp_get("QCAMTASK", &qcam_mod, &vfe_7x_sync, NULL);
	if (rc) {
		rc = -EBUSY;
		goto get_qcam_fail;
	}

	rc = msm_adsp_get("VFETASK", &vfe_mod, &vfe_7x_sync, NULL);
	if (rc) {
		rc = -EBUSY;
		goto get_vfe_fail;
	}

	return 0;

get_vfe_fail:
	msm_adsp_put(qcam_mod);
get_qcam_fail:
	kfree(extdata);
init_fail:
	extlen = 0;
	return rc;
}

static int vfe_7x_config_axi(int mode,
	struct axidata *ad, struct axiout *ao)
{
	struct msm_pmem_region *regptr;
	unsigned long *bptr;
	int    cnt;

	int rc = 0;

	if (mode == OUTPUT_1 || mode == OUTPUT_1_AND_2) {
		regptr = ad->region;

		CDBG("bufnum1 = %d\n", ad->bufnum1);
		CDBG("config_axi1: O1, phy = 0x%lx, y_off = %d, cbcr_off =%d\n",
			regptr->paddr, regptr->info.y_off,
			regptr->info.cbcr_off);

		bptr = &ao->output1buffer1_y_phy;
		for (cnt = 0; cnt < ad->bufnum1; cnt++) {
			*bptr = regptr->paddr + regptr->info.y_off;
			bptr++;
			*bptr = regptr->paddr + regptr->info.cbcr_off;

			bptr++;
			regptr++;
		}

		regptr--;
		for (cnt = 0; cnt < (8 - ad->bufnum1); cnt++) {
			*bptr = regptr->paddr + regptr->info.y_off;
			bptr++;
			*bptr = regptr->paddr + regptr->info.cbcr_off;
			bptr++;
		}
	} /* if OUTPUT1 or Both */

	if (mode == OUTPUT_2 || mode == OUTPUT_1_AND_2) {
		regptr = &(ad->region[ad->bufnum1]);

		CDBG("bufnum2 = %d\n", ad->bufnum2);
		CDBG("config_axi2: O2, phy = 0x%lx, y_off = %d, cbcr_off =%d\n",
		     regptr->paddr, regptr->info.y_off, regptr->info.cbcr_off);

		bptr = &ao->output2buffer1_y_phy;
		for (cnt = 0; cnt < ad->bufnum2; cnt++) {
			*bptr = regptr->paddr + regptr->info.y_off;
			bptr++;
			*bptr = regptr->paddr + regptr->info.cbcr_off;

			bptr++;
			regptr++;
		}

		regptr--;
		for (cnt = 0; cnt < (8 - ad->bufnum2); cnt++) {
			*bptr = regptr->paddr + regptr->info.y_off;
			bptr++;
			*bptr = regptr->paddr + regptr->info.cbcr_off;
			bptr++;
		}
	}

	return rc;
}

static int vfe_7x_config(struct msm_vfe_cfg_cmd *cmd, void *data)
{
	struct msm_pmem_region *regptr;
	unsigned char buf[256];

	struct vfe_stats_ack sack;
	struct axidata *axid;
	uint32_t i, op_mode;
	uint32_t *_mode;

	struct vfe_stats_we_cfg *scfg = NULL;
	struct vfe_stats_af_cfg *sfcfg = NULL;

	struct axiout *axio = NULL;
	void   *cmd_data = NULL;
	void   *cmd_data_alloc = NULL;
	long rc = 0;
	struct msm_vfe_command_7k *vfecmd;

	vfecmd =
			kmalloc(sizeof(struct msm_vfe_command_7k),
				GFP_ATOMIC);
	if (!vfecmd) {
		pr_err("vfecmd alloc failed!\n");
		return -ENOMEM;
	}

	if (cmd->cmd_type != CMD_FRAME_BUF_RELEASE &&
	    cmd->cmd_type != CMD_STATS_BUF_RELEASE &&
	    cmd->cmd_type != CMD_STATS_AF_BUF_RELEASE) {
		if (copy_from_user(vfecmd,
				(void __user *)(cmd->value),
				sizeof(struct msm_vfe_command_7k))) {
			rc = -EFAULT;
			goto config_failure;
		}
	}

	switch (cmd->cmd_type) {
	case CMD_STATS_AEC_AWB_ENABLE:
	case CMD_STATS_AXI_CFG: {
		axid = data;
		if (!axid) {
			rc = -EFAULT;
			goto config_failure;
		}

		scfg =
			kmalloc(sizeof(struct vfe_stats_we_cfg),
				GFP_ATOMIC);
		if (!scfg) {
			rc = -ENOMEM;
			goto config_failure;
		}

		if (copy_from_user(scfg,
					(void __user *)(vfecmd->value),
					vfecmd->length)) {

			rc = -EFAULT;
			goto config_done;
		}

		CDBG("STATS_ENABLE: bufnum = %d, enabling = %d\n",
			axid->bufnum1, scfg->wb_expstatsenable);

		if (axid->bufnum1 > 0) {
			regptr = axid->region;

			for (i = 0; i < axid->bufnum1; i++) {

				CDBG("STATS_ENABLE, phy = 0x%lx\n",
					regptr->paddr);

				scfg->wb_expstatoutputbuffer[i] =
					(void *)regptr->paddr;
				regptr++;
			}

			cmd_data = scfg;

		} else {
			rc = -EINVAL;
			goto config_done;
		}
	}
		break;

	case CMD_STATS_AF_ENABLE:
	case CMD_STATS_AF_AXI_CFG: {
		axid = data;
		if (!axid) {
			rc = -EFAULT;
			goto config_failure;
		}

		sfcfg =
			kmalloc(sizeof(struct vfe_stats_af_cfg),
				GFP_ATOMIC);

		if (!sfcfg) {
			rc = -ENOMEM;
			goto config_failure;
		}

		if (copy_from_user(sfcfg,
					(void __user *)(vfecmd->value),
					vfecmd->length)) {

			rc = -EFAULT;
			goto config_done;
		}

		CDBG("AF_ENABLE: bufnum = %d, enabling = %d\n",
			axid->bufnum1, sfcfg->af_enable);

		if (axid->bufnum1 > 0) {
			regptr = &axid->region[0];

			for (i = 0; i < axid->bufnum1; i++) {

				CDBG("STATS_ENABLE, phy = 0x%lx\n",
					regptr->paddr);

				sfcfg->af_outbuf[i] =
					(void *)regptr->paddr;

				regptr++;
			}

			cmd_data = sfcfg;

		} else {
			rc = -EINVAL;
			goto config_done;
		}
	}
		break;

	case CMD_FRAME_BUF_RELEASE: {
		struct msm_frame *b;
		unsigned long p;
		struct vfe_outputack fack;
		if (!data)  {
			rc = -EFAULT;
			goto config_failure;
		}

		b = (struct msm_frame *)(cmd->value);
		p = *(unsigned long *)data;

		fack.header = VFE_FRAME_ACK;

		fack.output2newybufferaddress =
			(void *)(p + b->y_off);

		fack.output2newcbcrbufferaddress =
			(void *)(p + b->cbcr_off);

		vfecmd->queue = QDSP_CMDQUEUE;
		vfecmd->length = sizeof(struct vfe_outputack);
		cmd_data = &fack;
	}
		break;

	case CMD_SNAP_BUF_RELEASE:
		break;

	case CMD_STATS_BUF_RELEASE: {
		CDBG("vfe_7x_config: CMD_STATS_BUF_RELEASE\n");
		if (!data) {
			rc = -EFAULT;
			goto config_failure;
		}

		sack.header = STATS_WE_ACK;
		sack.bufaddr = (void *)*(uint32_t *)data;

		vfecmd->queue  = QDSP_CMDQUEUE;
		vfecmd->length = sizeof(struct vfe_stats_ack);
		cmd_data = &sack;
	}
		break;

	case CMD_STATS_AF_BUF_RELEASE: {
		CDBG("vfe_7x_config: CMD_STATS_AF_BUF_RELEASE\n");
		if (!data) {
			rc = -EFAULT;
			goto config_failure;
		}

		sack.header = STATS_AF_ACK;
		sack.bufaddr = (void *)*(uint32_t *)data;

		vfecmd->queue  = QDSP_CMDQUEUE;
		vfecmd->length = sizeof(struct vfe_stats_ack);
		cmd_data = &sack;
	}
		break;

	case CMD_GENERAL:
	case CMD_STATS_DISABLE: {
		if (vfecmd->length > 256) {
			cmd_data_alloc =
			cmd_data = kmalloc(vfecmd->length, GFP_ATOMIC);
			if (!cmd_data) {
				rc = -ENOMEM;
				goto config_failure;
			}
		} else
			cmd_data = buf;

		if (copy_from_user(cmd_data,
					(void __user *)(vfecmd->value),
					vfecmd->length)) {

			rc = -EFAULT;
			goto config_done;
		}

		if (vfecmd->queue == QDSP_CMDQUEUE) {
			switch (*(uint32_t *)cmd_data) {
			case VFE_RESET_CMD:
				msm_camio_vfe_blk_reset();
				vfestopped = 0;
				break;

			case VFE_START_CMD:
				_mode = (uint32_t *)cmd_data;
				op_mode = *(++_mode);
				if (op_mode & SNAPSHOT_MASK_MODE) {
					/* request AXI bus for snapshot */
					if (update_axi_qos(MSM_AXI_QOS_SNAPSHOT)
						< 0) {
						rc = -EFAULT;
						goto config_failure;
					}
				} else {
					/* request AXI bus for snapshot */
					if (update_axi_qos(MSM_AXI_QOS_PREVIEW)
						< 0) {
						rc = -EFAULT;
						goto config_failure;
					}
				}
				msm_camio_camif_pad_reg_reset_2();
				vfestopped = 0;
				break;

			case VFE_STOP_CMD:
				vfestopped = 1;
				goto config_send;

			default:
				break;
			}
		} /* QDSP_CMDQUEUE */
	}
		break;
	case CMD_AXI_CFG_PREVIEW:
	case CMD_RAW_PICT_AXI_CFG: {
		axid = data;
		if (!axid) {
			rc = -EFAULT;
			goto config_failure;
		}

		axio = kmalloc(sizeof(struct axiout), GFP_ATOMIC);
		if (!axio) {
			rc = -ENOMEM;
			goto config_failure;
		}

		if (copy_from_user(axio, (void __user *)(vfecmd->value),
					sizeof(struct axiout))) {
			rc = -EFAULT;
			goto config_done;
		}

		vfe_7x_config_axi(OUTPUT_2, axid, axio);
		cmd_data = axio;
	}
		break;

	case CMD_AXI_CFG_SNAP: {
		axid = data;
		if (!axid) {
			rc = -EFAULT;
			goto config_failure;
		}

		axio = kmalloc(sizeof(struct axiout), GFP_ATOMIC);
		if (!axio) {
			rc = -ENOMEM;
			goto config_failure;
		}

		if (copy_from_user(axio, (void __user *)(vfecmd->value),
					sizeof(struct axiout))) {
			rc = -EFAULT;
			goto config_done;
		}

		vfe_7x_config_axi(OUTPUT_1_AND_2, axid, axio);

		cmd_data = axio;
	}
		break;

	default:
		break;
	} /* switch */

	if (vfestopped)
		goto config_done;

config_send:
	CDBG("send adsp command = %d\n", *(uint32_t *)cmd_data);
	rc = msm_adsp_write(vfe_mod, vfecmd->queue,
				cmd_data, vfecmd->length);

config_done:
	if (cmd_data_alloc != NULL)
		kfree(cmd_data_alloc);

config_failure:
	kfree(scfg);
	kfree(axio);
	kfree(vfecmd);
	return rc;
}

void msm_camvfe_fn_init(struct msm_camvfe_fn *fptr, void *data)
{
	mutex_init(&vfe_lock);
	fptr->vfe_init    = vfe_7x_init;
	fptr->vfe_enable  = vfe_7x_enable;
	fptr->vfe_config  = vfe_7x_config;
	fptr->vfe_disable = vfe_7x_disable;
	fptr->vfe_release = vfe_7x_release;
	vfe_syncdata = data;
}
