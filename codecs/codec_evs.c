/*** MODULEINFO
	<depend>evs</depend>
***/

#include "asterisk.h"

#include <math.h>                       /* for log10, floor */

#include "asterisk/astobj2.h"           /* for ao2_ref */
#include "asterisk/codec.h"             /* for AST_MEDIA_TYPE_AUDIO */
#include "asterisk/frame.h"             /* for ast_frame, etc */
#include "asterisk/linkedlists.h"       /* for AST_LIST_NEXT, etc */
#include "asterisk/logger.h"            /* for ast_log, ast_debug, etc */
#include "asterisk/module.h"
#include "asterisk/translate.h"         /* for ast_trans_pvt, etc */

#include "asterisk/evs.h"               /* for evs_attr */

#undef NO_DATA                          /* already defined in <netdb.h> */
#include <3gpp-evs/cnst.h>              /* for MAX_BITS_PER_FRAME, etc */
#include <3gpp-evs/prot.h>              /* for amr_wb_enc, etc */
#include <3gpp-evs/stat_com.h>          /* for FRAMEMODE_NORMAL */
#include <3gpp-evs/typedef.h>           /* for UWord8, UWord16, Word16 */
#include <3gpp-evs/mime.h>              /* for AMRWB_IOmode2rate, etc */
/* mime.h must come last because typedef.h (Word16, Word32) missing */

#define BUFFER_SAMPLES 5760
#define BUFFER_BYTES   (MAX_BITS_PER_FRAME + 7) / 8
#define	EVS_SAMPLES    320

/* Sample frame data */
#include "asterisk/slin.h"
#include "ex_evs.h"

/*
 * Stores the function pointer 'sample_count' of the cached ast_codec
 * before this module was loaded. Allows to restore this previous
 * function pointer, when this module in unloaded.
 */
static struct ast_codec *evs_codec; /* codec of the cached format */
static int (*evs_previous_sample_counter)(struct ast_frame *frame);
static unsigned int evs_previous_maximum_ms;

struct evs_coder_pvt {
	Encoder_State *encoder;
	Decoder_State *decoder;
	short buf[BUFFER_SAMPLES];
	float con[BUFFER_BYTES];
	unsigned char fra[BUFFER_BYTES];
	Indice ind_list[MAX_NUM_INDICES];
};

static Word16 unpack_bit(UWord8 **pt, UWord8 *mask);
static Word16 rate2AMRWB_IOmode(Word32 rate);
static Word16 rate2EVSmode(Word32 rate);
static short select_mode(short Opt_AMR_WB, short Opt_RF_ON, long total_brate);
static int select_bit_rate(int bit_rate, int max_bandwidth);

/* Copy & Paste from lib_com/bitstream.c */
static Word16 unpack_bit(UWord8 **pt, UWord8 *mask)
{
	Word16 bit = ((**pt & *mask) != 0);

	*mask >>= 1;
	if (*mask == 0)
	{
		*mask = 0x80;
		(*pt)++;
	}

	return bit;
}

static Word16 rate2AMRWB_IOmode(Word32 rate)
{
	switch (rate) {
	/* EVS AMR-WB IO modes */
	case SID_1k75:
		return AMRWB_IO_SID;
	case ACELP_6k60:
		return AMRWB_IO_6600;
	case ACELP_8k85:
		return AMRWB_IO_8850;
	case ACELP_12k65:
		return AMRWB_IO_1265;
	case ACELP_14k25:
		return AMRWB_IO_1425;
	case ACELP_15k85:
		return AMRWB_IO_1585;
	case ACELP_18k25:
		return AMRWB_IO_1825;
	case ACELP_19k85:
		return AMRWB_IO_1985;
	case ACELP_23k05:
		return AMRWB_IO_2305;
	case ACELP_23k85:
		return AMRWB_IO_2385;
	default:
		return -1;
	}
}

static Word16 rate2EVSmode(Word32 rate)
{
	switch (rate) {
	/* EVS Primary modes */
	case FRAME_NO_DATA :
		return NO_DATA;
	case SID_2k40:
		return PRIMARY_SID;
	case PPP_NELP_2k80:
		return PRIMARY_2800;
	case ACELP_7k20:
		return PRIMARY_7200;
	case ACELP_8k00:
		return PRIMARY_8000;
	case ACELP_9k60:
		return PRIMARY_9600;
	case ACELP_13k20:
		return PRIMARY_13200;
	case ACELP_16k40:
		return PRIMARY_16400;
	case ACELP_24k40:
		return PRIMARY_24400;
	case ACELP_32k:
		return PRIMARY_32000;
	case ACELP_48k:
		return PRIMARY_48000;
	case ACELP_64k:
		return PRIMARY_64000;
	case HQ_96k:
		return PRIMARY_96000;
	case HQ_128k:
		return PRIMARY_128000;
	default:
		return rate2AMRWB_IOmode(rate);
	}
}

/* Copy & Paste from lib_enc/io_enc.c:io_ini_enc */
static short select_mode(short Opt_AMR_WB, short Opt_RF_ON, long total_brate)
{
	if (Opt_AMR_WB) {
		return MODE1;
	}

	switch (total_brate) {
	case 2800:
		return MODE1;
	case 7200:
		return MODE1;
	case 8000:
		return MODE1;
	case 9600:
		return MODE2;
	case 13200:
		if (Opt_RF_ON) {
			return MODE2;
		} else {
			return MODE1;
		}
	case 16400:
		return MODE2;
	case 24400:
		return MODE2;
	case 32000:
		return MODE1;
	case 48000:
		return MODE2;
	case 64000:
		return MODE1;
	case 96000:
		return MODE2;
	case 128000:
		return MODE2;
	}

	ast_log(LOG_ERROR, "unexpected bit-rate %ld\n", total_brate);
	return 0;
}

static int select_bit_rate(int bit_rate, int max_bandwidth)
{
	switch (max_bandwidth) {
	case NB:
		return MIN(bit_rate, PRIMARY_24400);
	default:
		return bit_rate;
	}
}

static int lintoevs_new(struct ast_trans_pvt *pvt)
{
	struct evs_coder_pvt *apvt = pvt->pvt;
	const unsigned int sample_rate = pvt->t->src_codec.sample_rate;

	struct evs_attr *attr = pvt->explicit_dst ?
		ast_format_get_attribute_data(pvt->explicit_dst) : NULL;
	const int channel_aware = attr ? MIN(attr->ch_aw_send, attr->ch_aw_recv) : -2;
	const unsigned int dtx_on = attr ? MIN(attr->dtx, attr->dtx_send) : 0;
	const int amr_wb = attr ? attr->evs_mode_switch : -1;
	const int max_bandwidth = attr ? /* WB matches all bit-rates */
		floor(log10(attr->bw_send) / log10(2)) - 1 : WB;
	int bit_rate_evs = attr ? /* 16.4 is available in all bandwidths */
		floor(log10(attr->br_send) / log10(2)) - 1 : PRIMARY_16400;
	int bit_rate_amr;

	apvt->encoder = ast_malloc(sizeof(*apvt->encoder));
	if (NULL == apvt->encoder) {
		ast_log(LOG_ERROR, "Error creating the 3GPP EVS encoder\n");
		return -1;
	}

	if (attr && 0 < attr->mode_set) {
		bit_rate_amr = floor(log10(attr->mode_set) / log10(2));
	} else {
		bit_rate_amr = AMRWB_IO_2385;
	}

	apvt->encoder->ind_list = apvt->ind_list;
	apvt->encoder->input_Fs = sample_rate;
	/* Value range:  0..2, see res/res_format_attr_evs.c */
	apvt->encoder->Opt_DTX_ON = (0 < dtx_on);
	apvt->encoder->var_SID_rate_flag = 1; /* Automatic interval */
	/* Value range: -1..1, see res/res_format_attr_evs.c */
	apvt->encoder->Opt_AMR_WB = (0 < amr_wb);
	/* Value range: -2..7, see res/res_format_attr_evs.c */
	apvt->encoder->Opt_RF_ON = (0 < channel_aware);
	if (apvt->encoder->Opt_RF_ON) {
		/* EVS library crashed with higher values */
		apvt->encoder->rf_fec_offset = MIN(channel_aware, MAX_RF_FEC_OFFSET);
	} else {
		/* Must be set although it should follow Opt_RF_ON */
		apvt->encoder->rf_fec_offset = 0;
	}
	apvt->encoder->rf_fec_indicator = 1; /* Frame-erasure-rate indicator = HI */

	/* Variable bit-rate (SC-VBR) requires DTX according to the 3GPP EVS
	 * library "lib_enc/io_enc.c:io_ini_enc" cases:
	 * 1) st->Opt_SC_VBR && !st->Opt_DTX_ON
	 * 2) st->total_brate == ACELP_5k90 */
	apvt->encoder->Opt_SC_VBR = (0 == bit_rate_evs);
	if (sample_rate <= 8000 || max_bandwidth == NB) {
		apvt->encoder->max_bwidth = NB;
	} else if (sample_rate <= 16000 || max_bandwidth == WB || apvt->encoder->Opt_SC_VBR) {
		apvt->encoder->max_bwidth = WB;
	} else if (sample_rate <= 32000 || max_bandwidth == SWB) {
		apvt->encoder->max_bwidth = SWB;
	} else {
		apvt->encoder->max_bwidth = FB;
	}
	if (apvt->encoder->Opt_AMR_WB) {
		apvt->encoder->total_brate = AMRWB_IOmode2rate[bit_rate_amr];
	} else if (apvt->encoder->Opt_SC_VBR) {
		apvt->encoder->total_brate = PRIMARYmode2rate[PRIMARY_7200];
	} else {
		bit_rate_evs = select_bit_rate(bit_rate_evs, apvt->encoder->max_bwidth);
		apvt->encoder->total_brate = PRIMARYmode2rate[bit_rate_evs];
	}
	apvt->encoder->codec_mode = select_mode(apvt->encoder->Opt_AMR_WB,
		apvt->encoder->Opt_RF_ON, apvt->encoder->total_brate);
	apvt->encoder->last_codec_mode = apvt->encoder->codec_mode;

	/* After setting the above parameters (some set other parameters) */
	init_encoder(apvt->encoder);

	if (attr) {
		if (apvt->encoder->Opt_AMR_WB) {
			attr->mode_current = 0x10 + bit_rate_amr;
		} else if (apvt->encoder->max_bwidth ==  NB) {
			attr->mode_current = 0x00 + bit_rate_evs;
		} else if (apvt->encoder->max_bwidth ==  WB) {
			attr->mode_current = 0x20 + bit_rate_evs;
		} else if (apvt->encoder->max_bwidth == SWB) {
			attr->mode_current = 0x30 + bit_rate_evs;
		} else { /* FB */
			attr->mode_current = 0x40 + bit_rate_evs;
		}
	}

	ast_debug(3, "Created encoder (3GPP EVS) with sample rate %d\n", sample_rate);
	return 0;
}

static int evstolin_new(struct ast_trans_pvt *pvt)
{
	struct evs_coder_pvt *apvt = pvt->pvt;
	const unsigned int sample_rate = pvt->t->dst_codec.sample_rate;

	apvt->decoder = ast_malloc(sizeof(*apvt->decoder));
	if (NULL == apvt->decoder) {
		ast_log(LOG_ERROR, "Error creating the 3GPP EVS decoder\n");
		return -1;
	}

	apvt->decoder->output_Fs = sample_rate;
	init_decoder(apvt->decoder);

	ast_debug(3, "Created decoder (3GPP EVS) with sample rate %d\n", sample_rate);
	return 0;
}

static int lintoevs_framein(struct ast_trans_pvt *pvt, struct ast_frame *f)
{
	struct evs_coder_pvt *apvt = pvt->pvt;

	/* XXX We should look at how old the rest of our stream is, and if it
	 is too old, then we should overwrite it entirely, otherwise we can
	 get artifacts of earlier talk that do not belong */
	memcpy(apvt->buf + pvt->samples, f->data.ptr, f->datalen);
	pvt->samples += f->samples;

	return 0;
}

static struct ast_frame *lintoevs_frameout(struct ast_trans_pvt *pvt)
{
	struct evs_coder_pvt *apvt = pvt->pvt;
	const unsigned int sample_rate = pvt->t->src_codec.sample_rate;
	const unsigned int max_bandwidth = ((sample_rate / 8000) >> 1);
	const short n_samples = sample_rate / 50;
	struct ast_frame *result = NULL;
	struct ast_frame *last = NULL;
	int samples = 0; /* Output samples */

	struct evs_attr *attr = ast_format_get_attribute_data(pvt->f.subclass.format);
	const int mode = attr ? attr->mode_current :
		((0x20 >> apvt->encoder->Opt_AMR_WB) + PRIMARY_16400); /* 0x20 is WB */
	const int cmr = attr ? attr->cmr : 0;
	const int bandwidth = (mode & 0x70);
	unsigned int bit_rate = (mode & 0x0f);

	if (0x10 == bandwidth) {
		apvt->encoder->Opt_AMR_WB = 1;
		apvt->encoder->total_brate = AMRWB_IOmode2rate[bit_rate];
	} else if (mode <= 0x7f) { /* 0xff = NO_REQ */
		apvt->encoder->Opt_AMR_WB = 0;
		apvt->encoder->total_brate = PRIMARYmode2rate[bit_rate];
		apvt->encoder->Opt_SC_VBR = 0;
		apvt->encoder->Opt_RF_ON = 0;
		if (0x00 == bandwidth) {
			apvt->encoder->max_bwidth = MIN(max_bandwidth,  NB);
			if (0 == bit_rate) {
				apvt->encoder->Opt_SC_VBR = 1;
				apvt->encoder->total_brate = PRIMARYmode2rate[PRIMARY_7200];
			}
		} else if (0x20 == bandwidth) {
			apvt->encoder->max_bwidth = MIN(max_bandwidth,  WB);
			if (0 == bit_rate) {
				apvt->encoder->Opt_SC_VBR = 1;
				apvt->encoder->total_brate = PRIMARYmode2rate[PRIMARY_7200];
			}
		} else if (0x30 == bandwidth) {
			apvt->encoder->max_bwidth = MIN(max_bandwidth, SWB);
		} else if (0x40 == bandwidth) {
			apvt->encoder->max_bwidth = MIN(max_bandwidth,  FB);
		} else if (0x50 == bandwidth) {
			apvt->encoder->Opt_RF_ON = 1;
			apvt->encoder->total_brate = PRIMARYmode2rate[PRIMARY_13200];
			apvt->encoder->max_bwidth = MIN(max_bandwidth,  WB);
		} else if (0x60 == bandwidth) {
			apvt->encoder->Opt_RF_ON = 1;
			apvt->encoder->total_brate = PRIMARYmode2rate[PRIMARY_13200];
			apvt->encoder->max_bwidth = MIN(max_bandwidth, SWB);
		} /* else (0x70) is reserved; do nothing */
	}
	apvt->encoder->codec_mode = select_mode(apvt->encoder->Opt_AMR_WB,
		apvt->encoder->Opt_RF_ON, apvt->encoder->total_brate);

	while (pvt->samples >= n_samples) {
		struct ast_frame *current;
		unsigned char *out = pvt->outbuf.uc;
		const short *in = apvt->buf + samples;
		int datalen = 0;

		if (apvt->encoder->Opt_AMR_WB) {
			amr_wb_enc(apvt->encoder, in, n_samples);
		} else {
			evs_enc(apvt->encoder, in, n_samples);
		}

		samples += n_samples;
		pvt->samples -= n_samples;

		bit_rate = rate2EVSmode(apvt->encoder->nb_bits_tot * 50);
		if (bit_rate == NO_DATA) {
			continue; /* happens in case of DTX */
		} else if (bit_rate < 0) {
			ast_log(LOG_ERROR, "Error encoding the 3GPP EVS frame (code: %d)\n", apvt->encoder->nb_bits_tot);
			continue;
		}

		/* Change Mode Request (CMR) */
		if (apvt->encoder->Opt_AMR_WB || 1 == cmr) {
			out[0] = 0x7f; /* NO_REQ = no change in mode requested */
			out[0] = out[0] | 0x80; /* Header Type identification bit */
			datalen = datalen + 1;
			out++;
		}

		/* Table of Content (ToC), see lib_com/bitstream.c:write_indices */
		out[0] = 0x00; /* Header Type identification and Followed bit */
		out[0] |= (apvt->encoder->Opt_AMR_WB << 5); /* EVS mode bit */
		out[0] |= (apvt->encoder->Opt_AMR_WB << 4); /* Quality bit */
		out[0] |= bit_rate;
		datalen = datalen + 1;
		out++;

		/* Payload: fill rest of buffer, which is going to be send via RTP */
		indices_to_serial(apvt->encoder, out, &apvt->encoder->nb_bits_tot);

		/* Convert bits into bytes, +7 is for rounding-up */
		datalen = datalen + ((apvt->encoder->nb_bits_tot + 7) / 8);
		/* out was and is still part of pvt->outbuf.uc */
		current = ast_trans_frameout(pvt, datalen, EVS_SAMPLES);

		/* Everything used, therefore reset hidden index pointers */
		reset_indices_enc(apvt->encoder);

		if (!current) {
			continue;
		} else if (last) {
			AST_LIST_NEXT(last, frame_list) = current;
		} else {
			result = current;
		}
		last = current;
	}

	/* Move the data at the end of the buffer to the front */
	if (samples) {
		memmove(apvt->buf, apvt->buf + samples, pvt->samples * 2);
	}

	return result;
}

static int evstolin_framein(struct ast_trans_pvt *pvt, struct ast_frame *f)
{
	/* ToDo: 1) Packet-Loss Concealment (PLC)
	 *       2) several frames; currently just one frame
	 *       3) Compact format; currently only Header-Full format */
	struct evs_coder_pvt *apvt = pvt->pvt;
	const short n_samples = pvt->t->dst_codec.sample_rate / 50;

	struct evs_attr *attr = ast_format_get_attribute_data(f->subclass.format);
	unsigned char *in = f->data.ptr;
	unsigned char *payload = in;
	const frameMode bad_frame = FRAMEMODE_NORMAL;
	unsigned int toc_byte = in[0];
	UWord16 core_mode;
	unsigned int num_bits;

	if (toc_byte & 0x80) { /* Header Type identification bit */
		/* not Table of Content (ToC) but Change-Mode Request (CMR) */
		if (attr && toc_byte <= 0x7f) { /* 0xff = NO_REQ */
			attr->mode_current = toc_byte;
		}
		/* next byte is ToC */
		in++; payload++;
		toc_byte = in[0];
	}

	if (toc_byte & 0x80) { /* Header Type identification bit */
		ast_log(LOG_ERROR, "2nd CMR; bitstream is corrupted\n");
	}

	if (toc_byte & 0x40) { /* Followed bit */
		ast_log(LOG_ERROR, "2nd frame; bitstream is corrupted\n"); /* ToDo */
	}

	core_mode = toc_byte & 0x0f;
	if (toc_byte & 0x20) { /* EVS mode bit */
		apvt->decoder->Opt_AMR_WB = 1;
		apvt->decoder->bfi = !(toc_byte & 0x10); /* Quality bit */
		apvt->decoder->total_brate = AMRWB_IOmode2rate[core_mode];
	} else {
		apvt->decoder->Opt_AMR_WB = 0;
		apvt->decoder->bfi = 0; /* Bad frame indicator; ignored for EVS */
		apvt->decoder->total_brate = PRIMARYmode2rate[core_mode];
	}
	/* Next byte is Payload */
	payload++;

	num_bits = apvt->decoder->total_brate / 50;
	if (MAX_BITS_PER_FRAME < num_bits) {
		ast_log(LOG_ERROR, "more than %d bits; bitstream is corrupted\n",
			MAX_BITS_PER_FRAME);
	}
	/* AMR payload is reordered on the wire, see lib_com/mime.h
	 * and lib_com/bitsream.c:read_indices_mime */
	if (apvt->decoder->Opt_AMR_WB) {
		UWord8 mask = 0x80;
		int i;

		/* Clear all bytes of the buffer */
		for (i = 0; i < ((num_bits + 7) / 8); i = i + 1) {
			apvt->fra[i] = 0x00;
		}
		for (i = 0; i < num_bits; i = i + 1) {
			/* unpack_bit increases the payload pointer by one after 8 bits
			 * unpack_bit shifts the mask by one after each bit */
			int bit_value = unpack_bit(&payload, &mask);
			/* Returns the bit position for the current bit in
			 * the current AMR-WB mode */
			int position = sort_ptr[core_mode][i];
			/* Writes (|=) at its new byte (/8) and bit (%8) position from
			 * left to right (<<7-) */
			apvt->fra[position / 8] |= (bit_value << (7 - (position % 8)));
		}
		/* Unpack auxiliary bits of Silence Insertion Description (SID) frame */
		if (apvt->decoder->total_brate == SID_1k75)
		{
			Word16 sti = unpack_bit(&payload, &mask);
			Word16 cmi = unpack_bit(&payload, &mask) << 3;
			cmi |= unpack_bit(&payload, &mask) << 2;
			cmi |= unpack_bit(&payload, &mask) << 1;
			cmi |= unpack_bit(&payload, &mask) << 0;
			if (sti == 0) { /* SID_FIRST; otherwise SID_UPDATE */
				apvt->decoder->total_brate = 0;
			}
		}
		payload = apvt->fra;
		/* read_indices_from_djb could be avoided for AMR-WB, which would
		 * avoid one round of bit-shuffling. However, many flags of the
		 * decoder state are set by this function. Please, report this as
		 * issue, if you are affected by this additional bit-shuffling. */
	}
	read_indices_from_djb(apvt->decoder, payload, num_bits, 0, 0);

	if (apvt->decoder->Opt_AMR_WB) {
		amr_wb_dec(apvt->decoder, apvt->con);
	} else {
		evs_dec(apvt->decoder, apvt->con, bad_frame);
	}
	syn_output(apvt->con, n_samples, pvt->outbuf.i16 + pvt->samples);

	if (apvt->decoder->ini_frame < MAX_FRAME_COUNTER) {
		apvt->decoder->ini_frame = apvt->decoder->ini_frame + 1;
	}

	pvt->samples += n_samples;
	pvt->datalen += n_samples * 2;

	return 0;
}

static void lintoevs_destroy(struct ast_trans_pvt *pvt)
{
	struct evs_coder_pvt *apvt = pvt->pvt;

	if (NULL == apvt || NULL == apvt->encoder) {
		return;
	}

	destroy_encoder(apvt->encoder);
	ast_free(apvt->encoder);

	ast_debug(3, "Destroyed encoder (3GPP EVS)\n");
}

static void evstolin_destroy(struct ast_trans_pvt *pvt)
{
	struct evs_coder_pvt *apvt = pvt->pvt;

	if (NULL == apvt || NULL == apvt->decoder) {
		return;
	}

	destroy_decoder(apvt->decoder);
	ast_free(apvt->decoder);

	ast_debug(3, "Destroyed decoder (3GPP EVS)\n");
}

static struct ast_translator evstolin = {
	.table_cost = AST_TRANS_COST_LY_LL_ORIGSAMP,
	.name = "evstolin",
	.src_codec = {
		.name = "evs",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.dst_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 8000,
	},
	.format = "slin",
	.newpvt = evstolin_new,
	.framein = evstolin_framein,
	.destroy = evstolin_destroy,
	.sample = evs_sample,
	.desc_size = sizeof(struct evs_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES / 2,
	.buf_size = BUFFER_SAMPLES,
};

static struct ast_translator lintoevs = {
	.table_cost = AST_TRANS_COST_LL_LY_ORIGSAMP,
	.name = "lintoevs",
	.src_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 8000,
	},
	.dst_codec = {
		.name = "evs",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.format = "evs",
	.newpvt = lintoevs_new,
	.framein = lintoevs_framein,
	.frameout = lintoevs_frameout,
	.destroy = lintoevs_destroy,
	.sample = slin8_sample,
	.desc_size = sizeof(struct evs_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES / 2,
	.buf_size = BUFFER_SAMPLES,
};

static struct ast_translator evstolin16 = {
	.table_cost = AST_TRANS_COST_LY_LL_ORIGSAMP - 1,
	.name = "evstolin16",
	.src_codec = {
		.name = "evs",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.dst_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.format = "slin16",
	.newpvt = evstolin_new,
	.framein = evstolin_framein,
	.destroy = evstolin_destroy,
	.sample = evs_sample,
	.desc_size = sizeof(struct evs_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES / 2,
	.buf_size = BUFFER_SAMPLES,
};

static struct ast_translator lin16toevs = {
	.table_cost = AST_TRANS_COST_LL_LY_ORIGSAMP - 1,
	.name = "lin16toevs",
	.src_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.dst_codec = {
		.name = "evs",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.format = "evs",
	.newpvt = lintoevs_new,
	.framein = lintoevs_framein,
	.frameout = lintoevs_frameout,
	.destroy = lintoevs_destroy,
	.sample = slin16_sample,
	.desc_size = sizeof(struct evs_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES / 2,
	.buf_size = BUFFER_SAMPLES,
};

static struct ast_translator evstolin32 = {
	.table_cost = AST_TRANS_COST_LY_LL_ORIGSAMP - 2,
	.name = "evstolin32",
	.src_codec = {
		.name = "evs",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.dst_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 32000,
	},
	.format = "slin32",
	.newpvt = evstolin_new,
	.framein = evstolin_framein,
	.destroy = evstolin_destroy,
	.sample = evs_sample,
	.desc_size = sizeof(struct evs_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES / 2,
	.buf_size = BUFFER_SAMPLES,
};

static struct ast_translator lin32toevs = {
	.table_cost = AST_TRANS_COST_LL_LY_ORIGSAMP - 2,
	.name = "lin32toevs",
	.src_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 32000,
	},
	.dst_codec = {
		.name = "evs",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.format = "evs",
	.newpvt = lintoevs_new,
	.framein = lintoevs_framein,
	.frameout = lintoevs_frameout,
	.destroy = lintoevs_destroy,
	.desc_size = sizeof(struct evs_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES / 2,
	.buf_size = BUFFER_SAMPLES,
};

static struct ast_translator evstolin48 = {
	.table_cost = AST_TRANS_COST_LY_LL_ORIGSAMP - 4,
	.name = "evstolin48",
	.src_codec = {
		.name = "evs",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.dst_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.format = "slin48",
	.newpvt = evstolin_new,
	.framein = evstolin_framein,
	.destroy = evstolin_destroy,
	.sample = evs_sample,
	.desc_size = sizeof(struct evs_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES / 2,
	.buf_size = BUFFER_SAMPLES,
};

static struct ast_translator lin48toevs = {
	.table_cost = AST_TRANS_COST_LL_LY_ORIGSAMP - 4,
	.name = "lin48toevs",
	.src_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.dst_codec = {
		.name = "evs",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.format = "evs",
	.newpvt = lintoevs_new,
	.framein = lintoevs_framein,
	.frameout = lintoevs_frameout,
	.destroy = lintoevs_destroy,
	.desc_size = sizeof(struct evs_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES / 2,
	.buf_size = BUFFER_SAMPLES,
};

static int evs_sample_counter(struct ast_frame *frame)
{
	return EVS_SAMPLES; /* ToDo: several frames per RTP payload (ToC) */
	/* is this required? would limit Asterisk to Header-Full
	 * format, because here the result of the SDP negotiation
	 * is unknown. In Header-Full-only mode, the payloads are
	 * not padded to identify the Compact format, see
	 * 3GPP TS 26.445 A.2.3.2. Mhm, has anyone an idea? */
}

static int unload_module(void)
{
	int res;

	if (evs_codec) {
		evs_codec->samples_count = evs_previous_sample_counter;
		evs_codec->maximum_ms = evs_previous_maximum_ms;
		ao2_ref(evs_codec, -1);
	}

	res = ast_unregister_translator(&evstolin);
	res |= ast_unregister_translator(&lintoevs);
	res |= ast_unregister_translator(&evstolin16);
	res |= ast_unregister_translator(&lin16toevs);
	res |= ast_unregister_translator(&evstolin32);
	res |= ast_unregister_translator(&lin32toevs);
	res |= ast_unregister_translator(&evstolin48);
	res |= ast_unregister_translator(&lin48toevs);

	return res;
}

static int load_module(void)
{
	int res;

	evs_codec = ast_codec_get("evs", AST_MEDIA_TYPE_AUDIO, 16000);
	if (NULL == evs_codec) {
		ast_log(LOG_ERROR, "Please, apply the file 'codec_evs.patch'!\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	evs_previous_sample_counter = evs_codec->samples_count;
	evs_codec->samples_count = evs_sample_counter;
	evs_previous_maximum_ms = evs_codec->maximum_ms;
	evs_codec->maximum_ms = 20; /* ToDo: several frames per RTP payload */
	/* A smoothable codec allows Asterisk to put several frame blocks
	 * into one RTP packet, for example when the negotiated paketization
	 * time (ptime) is 60ms. Frames of codecs like 3GPP EVS cannot be put
	 * like this into a RTP packet, because each frame block might have a
	 * different length. Therefore, 3GPP EVS works with a Table of Contents.
	 * Or stated differently: Smoothable works only with codecs which have
	 * known fixed size, the same for each frame block. Commented because it
	 * is set already, being non-smoothable is the default. */
	/* evs_codec->smooth = 0; */

	res = ast_register_translator(&evstolin);
	res |= ast_register_translator(&lintoevs);
	res |= ast_register_translator(&evstolin16);
	res |= ast_register_translator(&lin16toevs);
	res |= ast_register_translator(&evstolin32);
	res |= ast_register_translator(&lin32toevs);
	res |= ast_register_translator(&evstolin48);
	res |= ast_register_translator(&lin48toevs);

	if (res) {
		unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "3GPP EVS Coder/Decoder");
