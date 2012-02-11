/*
 * pcm audio input device
 *
 * Copyright (C) 2008 Google, Inc.
 * Copyright (C) 2008 HTC Corporation
 * Copyright (c) 2009-2010, Code Aurora Forum. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/dma-mapping.h>
#include <linux/msm_audio_7X30.h>
#include <linux/android_pmem.h>
#include <linux/slab.h>
#include <linux/debugfs.h>

#include <asm/atomic.h>
#include <asm/ioctls.h>

#include <mach/msm_adsp_1x.h>
#include <mach/qdsp5v2_1x/qdsp5audreccmdi.h>
#include <mach/qdsp5v2_1x/qdsp5audrecmsg.h>
#include <mach/qdsp5v2_1x/audpreproc.h>
#include <mach/qdsp5v2_1x/audio_dev_ctl.h>
#include <mach/qdsp5v2_1x/afe.h>
#include <mach/debug_audio_mm.h>

/* FRAME_NUM must be a power of two */
#define FRAME_NUM		(8)
#define FRAME_SIZE		(2052 * 2)
#define MONO_DATA_SIZE		(2048)
#define STEREO_DATA_SIZE	(MONO_DATA_SIZE * 2)
#define DMASZ 			(FRAME_SIZE * FRAME_NUM)

#define AUDIO_GET_VOICE_STATE   _IOR(AUDIO_IOCTL_MAGIC, 55, unsigned)
#define AUDIO_GET_DEV_DRV_VER   _IOR(AUDIO_IOCTL_MAGIC, 56, unsigned)
#define DEV_DRV_VER             (8255 << 16 | 1)


extern int msm_get_voice_state(void);
	
struct buffer {
	void *data;
	uint32_t size;
	uint32_t read;
	uint32_t addr;
};

struct audio_in {
	struct buffer in[FRAME_NUM];

	spinlock_t dsp_lock;

	atomic_t in_bytes;
	atomic_t in_samples;
	atomic_t rec_conf;

	struct mutex lock;
	struct mutex read_lock;

	wait_queue_head_t wait;
	wait_queue_head_t wait_enable;
	wait_queue_head_t wait_voice_incall;
	wait_queue_head_t wait_rec_cfg;

	struct msm_adsp_module *audrec;

	/* configuration to use on next enable */
	uint32_t samp_rate;
	uint32_t channel_mode;
	uint32_t buffer_size; /* 2048 for mono, 4096 for stereo */

	uint32_t dsp_cnt;
	uint32_t in_head; /* next buffer dsp will write */
	uint32_t in_tail; /* next buffer read() will read */
	uint32_t in_count; /* number of buffers available to read() */

	unsigned queue_ids;
	uint16_t enc_id; /* Session Id */

	uint16_t source; /* Encoding source bit mask */
        uint32_t in_call;
	uint32_t dev_cnt;
        int voice_state;
	spinlock_t dev_lock;

	/* data allocated for various buffers */
	char *data;
	dma_addr_t phys;

	int enabled;
	int running;
	int stopped; /* set when stopped, cleared on flush */
};

struct audio_frame {
	uint16_t frame_count_lsw;
	uint16_t frame_count_msw;
	uint16_t frame_length;
	uint16_t erased_pcm;
	unsigned char raw_bitstream[]; /* samples */
} __attribute__((packed));

/* Audrec Queue command sent macro's */
#define audrec_send_bitstreamqueue(audio, cmd, len) \
	msm_adsp_write(audio->audrec, ((audio->queue_ids & 0xFFFF0000) >> 16), cmd, len)

#define audrec_send_audrecqueue(audio, cmd, len) \
	msm_adsp_write(audio->audrec, (audio->queue_ids & 0x0000FFFF), cmd, len)

/* DSP command send functions */
static int audpcm_in_enc_config(struct audio_in *audio, int enable);
static int audpcm_in_param_config(struct audio_in *audio);
static int audpcm_in_mem_config(struct audio_in *audio);
static int audpcm_in_record_config(struct audio_in *audio, int enable);
static int audpcm_dsp_read_buffer(struct audio_in *audio, uint32_t read_cnt);

static void audpcm_in_get_dsp_frames(struct audio_in *audio);

static void audpcm_in_flush(struct audio_in *audio);

static void audpcm_in_listener(u32 evt_id, union auddev_evt_data *evt_payload,
				void *private_data)
{
	struct audio_in *audio = (struct audio_in *) private_data;
	unsigned long flags;

	MM_DBG("evt_id = %d\n", evt_id);
	switch (evt_id) {
	case AUDDEV_EVT_DEV_RDY: {
		MM_DBG("AUDDEV_EVT_DEV_RDY\n");

		spin_lock_irqsave(&audio->dev_lock, flags);
		audio->dev_cnt++;
		if (!audio->in_call) audio->source |= (0x1 << evt_payload->routing_id);
		spin_unlock_irqrestore(&audio->dev_lock, flags);

		if ((audio->running == 1) && (audio->enabled == 1)) 
			audpcm_in_record_config(audio, 1);
		break;
	}
	case AUDDEV_EVT_DEV_RLS: {
		MM_DBG("AUDDEV_EVT_DEV_RLS\n");

		spin_lock_irqsave(&audio->dev_lock, flags);
		audio->dev_cnt--;
		if (!audio->in_call) audio->source &= ~(0x1 << evt_payload->routing_id);
		spin_unlock_irqrestore(&audio->dev_lock, flags);

		if (!audio->running || !audio->enabled)
			break;

		/* Turn off as per source */
		if (audio->source && !audio->in_call)
			audpcm_in_record_config(audio, 1);
		else
			/* Turn off all */
			audpcm_in_record_config(audio, 0);

		break;
	}
        case AUDDEV_EVT_VOICE_STATE_CHG: {
                MM_DBG("AUDDEV_EVT_VOICE_STATE_CHG, state = %d\n",
                                evt_payload->voice_state);
                audio->voice_state = evt_payload->voice_state;
                if (audio->in_call) {
			if(audio->running) {
	                        if (audio->voice_state == VOICE_STATE_INCALL)
        	                        audpcm_in_record_config(audio, 1);
	                        else if (audio->voice_state == VOICE_STATE_OFFCALL) {
	                                audpcm_in_record_config(audio, 0);
	                                wake_up(&audio->wait);
	                        }
			}
			wake_up(&audio->wait_voice_incall);
                }
                break;
        }
	case AUDDEV_EVT_FREQ_CHG: {
		MM_DBG("Encoder driver: got sample rate change event\n");
		MM_DBG("sample rate %d\n", evt_payload->freq_info.sample_rate);
		MM_DBG("dev_type %d\n", evt_payload->freq_info.dev_type);
		MM_DBG("acdb_dev_id %d\n", evt_payload->freq_info.acdb_dev_id);
		if (audio->running == 1) {
			/* Stop Recording sample rate does not match
			   with device sample rate */
			if (evt_payload->freq_info.sample_rate !=
				audio->samp_rate) {
				audpcm_in_record_config(audio, 0);
				audio->stopped = 1;
				wake_up(&audio->wait);
				wake_up(&audio->wait_voice_incall);
			}
		}
		break;
	}
	default:
		MM_AUD_ERR("wrong event %d\n", evt_id);
		break;
	}
}

/* ------------------- dsp preproc event handler--------------------- */
static void audpcm_in_dsp_event(void *data, unsigned id,  void *msg)
{
	struct audio_in *audio = data;

	switch (id) {
	case AUDPREPROC_ERROR_MSG: {
		struct audpreproc_err_msg *err_msg = msg;

		MM_ERR("ERROR_MSG: stream id %d err idx %d\n",
		err_msg->stream_id, err_msg->aud_preproc_err_idx);
		/* Error case */
		wake_up(&audio->wait_enable);
		break;
	}
	case AUDPREPROC_CMD_CFG_DONE_MSG: {
		MM_DBG("CMD_CFG_DONE_MSG \n");
		break;
	}
	case AUDPREPROC_CMD_ENC_CFG_DONE_MSG: {
		struct audpreproc_cmd_enc_cfg_done_msg *enc_cfg_msg = msg;

		MM_DBG("CMD_ENC_CFG_DONE_MSG: stream id %d enc type 0x%x\n", 
			enc_cfg_msg->stream_id,	enc_cfg_msg->rec_enc_type);
		/* Encoder enable success */
		if (enc_cfg_msg->rec_enc_type & ENCODE_ENABLE) 	audpcm_in_param_config(audio);
		else { /* Encoder disable success */
			audio->running = 0;
			audpcm_in_record_config(audio, 0);
		}
		break;
	}
	case AUDPREPROC_CMD_ENC_PARAM_CFG_DONE_MSG: {
		MM_DBG("CMD_ENC_PARAM_CFG_DONE_MSG \n");
		audpcm_in_mem_config(audio);
		break;
	}
	case AUDPREPROC_AFE_CMD_AUDIO_RECORD_CFG_DONE_MSG: {
		MM_DBG("CMD_RECORD_CFG_DONE_MSG \n");
		atomic_set(&audio->rec_conf,1);
		wake_up(&audio->wait_rec_cfg);
		break;
	}
	case ADSP_MESSAGE_ID:
		pr_aud_info("audpre: enable/disable done\n");
		break;
	default:
		MM_AUD_ERR("Unknown Event id %d\n", id);
	}
}

/* ------------------- dsp audrec event handler--------------------- */
static void audrec_dsp_event(void *data, unsigned id, size_t len,
			    void (*getevent)(void *ptr, size_t len))
{
	struct audio_in *audio = data;

	switch (id) {
	case AUDREC_CMD_MEM_CFG_DONE_MSG: {
		MM_DBG("CMD_MEM_CFG_DONE\n");
		audio->running = 1;
		if (!audio->in_call && (audio->dev_cnt > 0)) audpcm_in_record_config(audio, 1);
		wake_up(&audio->wait_enable);
		break;
	}
	case AUDREC_FATAL_ERR_MSG: {
		struct audrec_fatal_err_msg fatal_err_msg;

		getevent(&fatal_err_msg, AUDREC_FATAL_ERR_MSG_LEN);
		MM_AUD_ERR("FATAL_ERR_MSG: err id %d\n",
				fatal_err_msg.audrec_err_id);
		/* Error stop the encoder */
		audio->stopped = 1;
		wake_up(&audio->wait_voice_incall);
		wake_up(&audio->wait);
		break;
	}
	case AUDREC_UP_PACKET_READY_MSG: {
		struct audrec_up_pkt_ready_msg pkt_ready_msg;

		getevent(&pkt_ready_msg, AUDREC_UP_PACKET_READY_MSG_LEN);
		MM_DBG("UP_PACKET_READY_MSG: write lsw %d write msw %d read lsw %d read msw %d\n",
		pkt_ready_msg.audrec_packet_write_cnt_lsw,
		pkt_ready_msg.audrec_packet_write_cnt_msw,
		pkt_ready_msg.audrec_up_prev_read_cnt_lsw,
		pkt_ready_msg.audrec_up_prev_read_cnt_msw);

		audpcm_in_get_dsp_frames(audio);
		break;
	}
	case ADSP_MESSAGE_ID:
		MM_DBG("audrec: enable/disable done\n");
		break;
	default:
		MM_AUD_ERR("Unknown Event id %d\n", id);
	}
}

static void audpcm_in_get_dsp_frames(struct audio_in *audio)
{
	struct audio_frame *frame;
	uint32_t index;
	unsigned long flags;

	index = audio->in_head;

	frame = (void *) (((char *)audio->in[index].data) - \
			 sizeof(*frame));

	spin_lock_irqsave(&audio->dsp_lock, flags);
	audio->in[index].size = frame->frame_length;

	/* statistics of read */
	atomic_add(audio->in[index].size, &audio->in_bytes);
	atomic_add(1, &audio->in_samples);

	audio->in_head = (audio->in_head + 1) & (FRAME_NUM - 1);

	/* If overflow, move the tail index foward. */
	if (audio->in_head == audio->in_tail)
		audio->in_tail = (audio->in_tail + 1) & (FRAME_NUM - 1);
	else
		audio->in_count++;

	audpcm_dsp_read_buffer(audio, audio->dsp_cnt++);
	spin_unlock_irqrestore(&audio->dsp_lock, flags);

	wake_up(&audio->wait);
}

struct msm_adsp_ops audrec_adsp_ops = {
	.event = audrec_dsp_event,
};

static int audpcm_in_enc_config(struct audio_in *audio, int enable)
{
	struct audpreproc_audrec_cmd_enc_cfg cmd;

	MM_DBG("enable=%d\n",enable);
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDPREPROC_AUDREC_CMD_ENC_CFG;
	cmd.stream_id = audio->enc_id;

	if (enable)
		cmd.audrec_enc_type = ENC_TYPE_WAV | MSM_AUD_ENC_MODE_TUNNEL | ENCODE_ENABLE;
	else
		cmd.audrec_enc_type &= ~(ENCODE_ENABLE);

	return audpreproc_send_audreccmdqueue(&cmd, sizeof(cmd));
}

static int audpcm_in_param_config(struct audio_in *audio)
{
	struct audpreproc_audrec_cmd_parm_cfg_wav cmd;

	MM_DBG("enc_id=%d, rate=%d, chan_mode=%d\n",audio->enc_id, audio->samp_rate, audio->channel_mode);
	memset(&cmd, 0, sizeof(cmd));
	cmd.common.cmd_id = AUDPREPROC_AUDREC_CMD_PARAM_CFG;
	cmd.common.stream_id = audio->enc_id;
	cmd.aud_rec_samplerate_idx = audio->samp_rate;
	cmd.aud_rec_stereo_mode = audio->channel_mode;

	return audpreproc_send_audreccmdqueue(&cmd, sizeof(cmd));
}


/* To Do: msm_snddev_route_enc(audio->enc_id); */
static int audpcm_in_record_config(struct audio_in *audio, int enable)
{
	struct audpreproc_afe_cmd_audio_record_cfg cmd;

	MM_DBG("enable=%d\n",enable);
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDPREPROC_AFE_CMD_AUDIO_RECORD_CFG;
	cmd.stream_id = audio->enc_id;
	if (enable)
		cmd.destination_activity = AUDIO_RECORDING_TURN_ON;
	else
		cmd.destination_activity = AUDIO_RECORDING_TURN_OFF;

	cmd.source_mix_mask = audio->source;
	if (audio->enc_id == 2) {
                if ((cmd.source_mix_mask &
                                INTERNAL_CODEC_TX_SOURCE_MIX_MASK) ||
                        (cmd.source_mix_mask & AUX_CODEC_TX_SOURCE_MIX_MASK) ||
                        (cmd.source_mix_mask & VOICE_UL_SOURCE_MIX_MASK) ||
                        (cmd.source_mix_mask & VOICE_DL_SOURCE_MIX_MASK)) {
                        cmd.pipe_id = SOURCE_PIPE_1;
                }
                if (cmd.source_mix_mask &
                                AUDPP_A2DP_PIPE_SOURCE_MIX_MASK)
                        cmd.pipe_id |= SOURCE_PIPE_0;
        }

	return audpreproc_send_audreccmdqueue(&cmd, sizeof(cmd));
}

static int audpcm_in_mem_config(struct audio_in *audio)
{
	struct audrec_cmd_arecmem_cfg cmd;
	uint16_t *data = (void *) audio->data;
	int n;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDREC_CMD_MEM_CFG_CMD;
	cmd.audrec_up_pkt_intm_count = 1;
	cmd.audrec_ext_pkt_start_addr_msw = audio->phys >> 16;
	cmd.audrec_ext_pkt_start_addr_lsw = audio->phys;
	cmd.audrec_ext_pkt_buf_number = FRAME_NUM;

	/* prepare buffer pointers:
	 * Mono: 1024 samples + 4 halfword header
	 * Stereo: 2048 samples + 4 halfword header
	 */
	for (n = 0; n < FRAME_NUM; n++) {
		audio->in[n].data = data + 4;
		data += (4 + (audio->channel_mode ? 2048 : 1024));
		MM_DBG("0x%8x\n", (int)(audio->in[n].data - 8));
	}

	return audrec_send_audrecqueue(audio, &cmd, sizeof(cmd));
}

static int audpcm_dsp_read_buffer(struct audio_in *audio, uint32_t read_cnt)
{
	struct up_audrec_packet_ext_ptr cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = UP_AUDREC_PACKET_EXT_PTR;
	cmd.audrec_up_curr_read_count_msw = read_cnt >> 16;
	cmd.audrec_up_curr_read_count_lsw = read_cnt;

	return audrec_send_bitstreamqueue(audio, &cmd, sizeof(cmd));
}

/* must be called with audio->lock held */
static int audpcm_in_enable(struct audio_in *audio)
{
	if (audio->enabled)
		return 0;

	if (audpreproc_enable(audio->enc_id, &audpcm_in_dsp_event, audio)) {
		MM_AUD_ERR("msm_adsp_enable(audpreproc) failed\n");
		return -ENODEV;
	}

	if (msm_adsp_enable(audio->audrec)) {
		MM_AUD_ERR("msm_adsp_enable(audrec) failed\n");
		audpreproc_disable(audio->enc_id, audio);
		return -ENODEV;
	}
	audio->enabled = 1;
	audpcm_in_enc_config(audio, 1);

	return 0;
}

/* must be called with audio->lock held */
static int audpcm_in_disable(struct audio_in *audio)
{
	if (audio->enabled) {
		audio->enabled = 0;
		audpcm_in_enc_config(audio, 0);
		wake_up(&audio->wait);
		wake_up(&audio->wait_voice_incall);
		wait_event_interruptible_timeout(audio->wait_enable, audio->running == 0, 1*HZ);
		msm_adsp_disable(audio->audrec);
		audpreproc_disable(audio->enc_id, audio);
	}
	return 0;
}

static void audpcm_in_flush(struct audio_in *audio)
{
	int i;

	audio->dsp_cnt = 0;
	audio->in_head = 0;
	audio->in_tail = 0;
	audio->in_count = 0;
	audio->stopped = 0;
	for (i = 0; i < FRAME_NUM; i++) {
		audio->in[i].size = 0;
		audio->in[i].read = 0;
	}
	MM_DBG("in_bytes %d\n", atomic_read(&audio->in_bytes));
	MM_DBG("in_samples %d\n", atomic_read(&audio->in_samples));
	atomic_set(&audio->in_bytes, 0);
	atomic_set(&audio->in_samples, 0);
}

/* ------------------- device --------------------- */
static long audpcm_in_ioctl(struct file *file,
				unsigned int cmd, unsigned long arg)
{
	struct audio_in *audio = file->private_data;
	int rc = 0;

	if (cmd == AUDIO_STOP) {
		audio->stopped = 1;
		wake_up(&audio->wait_voice_incall);
	}

	mutex_lock(&audio->lock);

	switch (cmd) {

	case AUDIO_GET_STATS: {
		struct msm_audio_stats stats;
		stats.byte_count = atomic_read(&audio->in_bytes);
		stats.sample_count = atomic_read(&audio->in_samples);
		if (copy_to_user((void *) arg, &stats, sizeof(stats)))
			rc = -EFAULT;
		break;
        }
	case AUDIO_GET_VOICE_STATE: {
		int vstate = audio->voice_state;
		if (copy_to_user((void *) arg, &vstate, sizeof(vstate)))
			rc = -EFAULT;
		break;
        }
        case AUDIO_GET_DEV_DRV_VER: {
                unsigned int vers = DEV_DRV_VER;
                if (copy_to_user((void *) arg, &vers, sizeof(vers)))
                        rc = -EFAULT;
                break;
        }
	case AUDIO_START: {

		uint32_t freq;
		struct timespec ts;	
		/* Poll at 48KHz always */
		freq = 48000;
		MM_DBG("AUDIO_START\n");

		if (audio->in_call && (audio->voice_state != VOICE_STATE_INCALL)) {
			ts = CURRENT_TIME;
			rc = wait_event_interruptible(audio->wait_voice_incall,
				audio->voice_state == VOICE_STATE_INCALL || audio->stopped);
			if(rc < 0 || audio->voice_state != VOICE_STATE_INCALL || audio->stopped) {
				MM_DBG("valid incall state unreacheable\n");
				return -EPERM;
			}
			ts = timespec_sub(CURRENT_TIME,ts);
			MM_DBG("waited %ld.%03ld sec for voice incall state\n", ts.tv_sec, ts.tv_nsec/NSEC_PER_MSEC);
		}

		rc = msm_snddev_request_freq(&freq, audio->enc_id, SNDDEV_CAP_TX, AUDDEV_CLNT_ENC);
		if (rc < 0) {
			MM_DBG("sample rate can not be set, return code %d\n",	rc);
			msm_snddev_withdraw_freq(audio->enc_id,
						SNDDEV_CAP_TX, AUDDEV_CLNT_ENC);
			MM_DBG("msm_snddev_withdraw_freq\n");
			break;
		}
		MM_DBG("freq configured for %d\n", freq);

		rc = audpcm_in_enable(audio);
		if (rc == 0) {
			ts = CURRENT_TIME;
			rc = wait_event_interruptible_timeout(audio->wait_enable, audio->running != 0, 3*HZ);
			ts = timespec_sub(CURRENT_TIME,ts);
			MM_DBG("state = %d, rc = %d after %ld.%03ld sec\n", audio->running, rc, 
							ts.tv_sec, ts.tv_nsec/NSEC_PER_MSEC);
			rc = (audio->running == 0) ? -ENODEV : 0;
		} else MM_DBG("failed to enable audpcm\n");
		
		if (rc == 0 && audio->in_call) {
			rc = audpcm_in_record_config(audio, 1);
			if (rc != 0) MM_ERR("failed to send record config cmd\n");
			else  {
				ts = CURRENT_TIME;
				atomic_set(&audio->rec_conf,0);
				wait_event_interruptible_timeout(audio->wait_rec_cfg,
					atomic_read(&audio->rec_conf) != 0, 3*HZ);
				if (atomic_read(&audio->rec_conf) == 0) {
					MM_DBG("failed to config recording\n");
					rc = -EFAULT;
				} else {
					ts = timespec_sub(CURRENT_TIME,ts);
					MM_DBG("record config success after %ld.%03ld sec\n",
							ts.tv_sec, ts.tv_nsec/NSEC_PER_MSEC);
				}
			}
		}
		audio->stopped = 0;
		break;
	}
	case AUDIO_STOP: {
		MM_DBG("AUDIO_STOP\n");
		rc = audpcm_in_disable(audio);
		if (rc != 0) MM_DBG("failed to stop audio\n");
		rc = msm_snddev_withdraw_freq(audio->enc_id,SNDDEV_CAP_TX, AUDDEV_CLNT_ENC);
		break;
	}
	case AUDIO_FLUSH: {
		MM_DBG("AUDIO_FLUSH\n");
		if (audio->stopped) {
			/* Make sure we're stopped and we wake any threads
			 * that might be blocked holding the read_lock.
			 * While audio->stopped read threads will always
			 * exit immediately.
			 */
			wake_up(&audio->wait);
			mutex_lock(&audio->read_lock);
			audpcm_in_flush(audio);
			mutex_unlock(&audio->read_lock);
		}
		break;
	}
	case AUDIO_SET_CONFIG: {
		struct msm_audio_config cfg;
		if (copy_from_user(&cfg, (void *) arg, sizeof(cfg))) {
			rc = -EFAULT;
			break;
		}
		if (cfg.channel_count == 1) {
			cfg.channel_count = AUDREC_CMD_MODE_MONO;
                        audio->buffer_size = MONO_DATA_SIZE;
		} else if (cfg.channel_count == 2) {
			cfg.channel_count = AUDREC_CMD_MODE_STEREO;
			audio->buffer_size = STEREO_DATA_SIZE;
		} else {
			rc = -EINVAL;
			break;
		}
		audio->samp_rate = cfg.sample_rate;
		audio->channel_mode = cfg.channel_count;
		break;
	}
	case AUDIO_GET_CONFIG: {
		struct msm_audio_config cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.buffer_size = audio->buffer_size;
		cfg.buffer_count = FRAME_NUM;
		cfg.sample_rate = audio->samp_rate;
		if (audio->channel_mode == AUDREC_CMD_MODE_MONO)
			cfg.channel_count = 1;
		else
			cfg.channel_count = 2;
		if (copy_to_user((void *) arg, &cfg, sizeof(cfg)))
			rc = -EFAULT;
		break;
	}
        case AUDIO_SET_INCALL: {
                struct msm_voicerec_mode cfg;
                unsigned long flags;
                MM_DBG("AUDIO_SET_INCALL\n");
                if (copy_from_user(&cfg, (void *) arg, sizeof(cfg))) {
                        rc = -EFAULT;
                        break;
                }
                if (cfg.rec_mode != VOC_REC_BOTH &&
                        cfg.rec_mode != VOC_REC_UPLINK &&
                        cfg.rec_mode != VOC_REC_DOWNLINK) {
                        MM_ERR("invalid rec_mode\n");
                        rc = -EINVAL;
                        break;
                } else {
                        spin_lock_irqsave(&audio->dev_lock, flags);
                        if (cfg.rec_mode == VOC_REC_UPLINK)
                                audio->source = VOICE_UL_SOURCE_MIX_MASK;
                        else if (cfg.rec_mode == VOC_REC_DOWNLINK)
                                audio->source = VOICE_DL_SOURCE_MIX_MASK;
                        else
                                audio->source = VOICE_DL_SOURCE_MIX_MASK |
                                                VOICE_UL_SOURCE_MIX_MASK ;
                        audio->in_call = 1;
                        spin_unlock_irqrestore(&audio->dev_lock, flags);
                }
                break;
        }
	case AUDIO_GET_SESSION_ID: {
		if (copy_to_user((void *) arg, &audio->enc_id,
			sizeof(unsigned short))) {
			rc = -EFAULT;
		}
		break;
	}
	default:
		rc = -EINVAL;
	}
	if(rc != 0) MM_DBG("returning error %d\n",rc);
	mutex_unlock(&audio->lock);
	return rc;
}

static int reconfig_record(struct audio_in *audio) {

	MM_DBG("\n");
	mutex_lock(&audio->lock);
	atomic_set(&audio->rec_conf,0);
	if (audpcm_in_record_config(audio, 0) != 0) {
		mutex_unlock(&audio->lock);
		return -EFAULT;
	}
	wait_event_interruptible_timeout(audio->wait_rec_cfg, 
			atomic_read(&audio->rec_conf) == 1, 1*HZ);
	if (atomic_read(&audio->rec_conf) != 1) {
		mutex_unlock(&audio->lock);
		return -ENODEV;
	}					
	atomic_set(&audio->rec_conf,0);
	if (audpcm_in_record_config(audio, 1) != 0) {
		mutex_unlock(&audio->lock);
		return -EFAULT;
	}
	wait_event_interruptible_timeout(audio->wait_rec_cfg, 
			atomic_read(&audio->rec_conf) == 1, 1*HZ);
	if (atomic_read(&audio->rec_conf) != 1) {
		mutex_unlock(&audio->lock);
		return -ENODEV;
	}
	mutex_unlock(&audio->lock);
	return 0;
}

static ssize_t audpcm_in_read(struct file *file,
				char __user *buf,
				size_t count, loff_t *pos)
{
	struct audio_in *audio = file->private_data;
	unsigned long flags;
	const char __user *start = buf;
	void *data;
	uint32_t index;
	uint32_t size;
	int rc = 0;

	mutex_lock(&audio->read_lock);
	while (count > 0) {

                rc = wait_event_interruptible_timeout(
                        audio->wait, (audio->in_count > 0) || audio->stopped ||
                        (audio->in_call && audio->running &&
                                (audio->voice_state == VOICE_STATE_OFFCALL)), 1*HZ);

		if (rc == 0 && !audio->in_count 
			&& !audio->stopped && audio->voice_state == VOICE_STATE_INCALL) {
			rc = reconfig_record(audio);
			if (rc == 0) {
				MM_DBG("Recording reconfigured\n");
				continue;
			}
			MM_AUD_ERR("Failed to reconfigure recording\n");
                        break;
		} else rc = 0;

                if (!audio->in_count) {
                        if (audio->stopped) {
                                MM_AUD_ERR("No more data, driver stopped\n");
				rc = 0;/* End of File */
				break;
                        } else if (audio->in_call && audio->running &&
                                (audio->voice_state == VOICE_STATE_OFFCALL)) {
                                MM_AUD_ERR("No more data, already in off-call state\n");
                                rc = -EPERM; /* Voice Call stopped */
                                break;
                        }
		}

		index = audio->in_tail;
		data = (uint8_t *) audio->in[index].data;
		size = audio->in[index].size;
		if (count >= size) {
			if (copy_to_user(buf, data, size)) {
				rc = -EFAULT;
				break;
			}
			spin_lock_irqsave(&audio->dsp_lock, flags);
			if (index != audio->in_tail) {
				/* overrun -- data are invalid and we need to retry */
				spin_unlock_irqrestore(&audio->dsp_lock, flags);
				continue;
			}
			audio->in[index].size = 0;
			audio->in_tail = (audio->in_tail + 1) & (FRAME_NUM - 1);
			audio->in_count--;
			spin_unlock_irqrestore(&audio->dsp_lock, flags);
			count -= size;
			buf += size;
		} else {
			MM_AUD_ERR("short read\n");
			break;
		}
	}
	mutex_unlock(&audio->read_lock);

	if (buf > start)
		return buf - start;

	return rc;
}

static ssize_t audpcm_in_write(struct file *file,
				const char __user *buf,
				size_t count, loff_t *pos)
{
	return -EINVAL;
}

static int audpcm_in_release(struct inode *inode, struct file *file)
{
	struct audio_in *audio = file->private_data;

	mutex_lock(&audio->lock);
	audio->in_call = 0;

	msm_snddev_withdraw_freq(audio->enc_id, SNDDEV_CAP_TX, AUDDEV_CLNT_ENC);
	auddev_unregister_evt_listner(AUDDEV_CLNT_ENC, audio->enc_id);
	audpcm_in_disable(audio);
	audpcm_in_flush(audio);
	msm_adsp_put(audio->audrec);
	audpreproc_aenc_free(audio->enc_id);

	iounmap(audio->data);
	pmem_kfree(audio->phys);

	mutex_unlock(&audio->lock);

	kfree(audio);

	MM_DBG("closed.\n");

	return 0;
}

static int audpcm_in_open(struct inode *inode, struct file *file)
{
	struct audio_in *audio;
	const char *modname;	
	int rc;


	audio = kzalloc(sizeof(struct audio_in), GFP_KERNEL);

	if (!audio) return -ENOMEM;

        audio->phys = pmem_kalloc(DMASZ, PMEM_MEMTYPE_EBI1 | PMEM_ALIGNMENT_4K);
        if (!IS_ERR((void *) audio->phys)) {
		audio->data = ioremap(audio->phys, DMASZ);
                if (!audio->data) {
       	                MM_AUD_ERR("Could not remap DMA buffers\n");
               	        pmem_kfree(audio->phys);
			kfree(audio);
                       	return -ENOMEM;
                }
        } else {
       	        MM_AUD_ERR("Could not allocate DMA buffers\n");
		kfree(audio);
               	return -ENOMEM;
        }

	audio->enc_id = audpreproc_aenc_alloc(ENC_TYPE_WAV | MSM_AUD_ENC_MODE_TUNNEL, 
					&modname, &audio->queue_ids);
	if (audio->enc_id < 0) {
		MM_AUD_ERR("No free encoder available\n");
		rc = -ENODEV;
		goto no_aenc;
	}

	MM_DBG("allocated encoder %d, module %s\n", audio->enc_id, modname);

	rc = auddev_register_evt_listner(AUDDEV_EVT_DEV_RDY | AUDDEV_EVT_DEV_RLS |
					AUDDEV_EVT_FREQ_CHG | AUDDEV_EVT_VOICE_STATE_CHG,
					AUDDEV_CLNT_ENC, audio->enc_id,
					audpcm_in_listener, audio);
	if (rc) {
		MM_AUD_ERR("failed to register device event listener\n");
		goto evt_error;
	}

	rc = msm_adsp_get(modname, &audio->audrec, &audrec_adsp_ops, audio);

	if (rc) {
		MM_AUD_ERR("Failed to get AUDREC task\n");
		goto no_audrec;
	}

	if(!audio->audrec) {
		MM_AUD_ERR("Null AUDREC task returned by ADSP\n");
		rc = -EFAULT;
		goto no_audrec;
	}

	audio->source  = INTERNAL_CODEC_TX_SOURCE_MIX_MASK;
	audio->channel_mode = AUDREC_CMD_MODE_MONO;
	audio->buffer_size = MONO_DATA_SIZE;
	audio->samp_rate = 8000;
	audio->stopped = 0;
	audio->running = 0;
	audio->enabled = 0;
	audpcm_in_flush(audio);

	mutex_init(&audio->lock);
	mutex_init(&audio->read_lock);
	spin_lock_init(&audio->dsp_lock);
	spin_lock_init(&audio->dev_lock);
	init_waitqueue_head(&audio->wait);
	init_waitqueue_head(&audio->wait_enable);
	init_waitqueue_head(&audio->wait_voice_incall);
	init_waitqueue_head(&audio->wait_rec_cfg);

        audio->voice_state = msm_get_voice_state();

	file->private_data = audio;

	return rc;

no_audrec:
	auddev_unregister_evt_listner(AUDDEV_CLNT_ENC, audio->enc_id);
evt_error:
	audpreproc_aenc_free(audio->enc_id);

no_aenc:
        iounmap(audio->data);
	pmem_kfree(audio->phys);
	kfree(audio);	

	return rc;
}

static const struct file_operations audio_in_fops = {
	.owner		= THIS_MODULE,
	.open		= audpcm_in_open,
	.release	= audpcm_in_release,
	.read		= audpcm_in_read,
	.write		= audpcm_in_write,
	.unlocked_ioctl	= audpcm_in_ioctl,
};

struct miscdevice audio_in_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "msm_pcm_in",
	.fops	= &audio_in_fops,
};

static int __init audpcm_in_init(void)
{
	return misc_register(&audio_in_misc);
}

device_initcall(audpcm_in_init);


