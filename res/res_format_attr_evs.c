#include "asterisk.h"

/* based on res/res_format_attr_silk.c */

#include <ctype.h>                      /* for tolower */
#include <math.h>                       /* for log10, floor */

#include "asterisk/module.h"
#include "asterisk/format.h"
#include "asterisk/astobj2.h"           /* for ao2_bump */
#include "asterisk/format_cache.h"      /* for ast_format_evs */
#include "asterisk/logger.h"            /* for ast_debug, ast_log, etc */
#include "asterisk/strings.h"           /* for ast_str_append */
#include "asterisk/utils.h"             /* for MAX, ast_calloc, ast_free, etc */

#include "asterisk/evs.h"

/* Asterisk internal defaults; can differ from RFC defaults */
static struct evs_attr default_evs_attr = {
	.evs_mode_switch        = -1, /* primary mode                     */
	.hf_only                = -1, /* all formats                      */
	.dtx                    =  2, /* on                               */
	.dtx_send               =  1, /* do no change                     */
	.dtx_recv               =  2, /* on                               */
	.max_red                = -1, /* no redundancy limit              */
	.cmr                    =  0, /* might be in payload              */
	.cmr_included           =  0, /* CMR not in SDP                   */
	.br                     =  0, /* inclusion depends                */
	.br_send                =  0x1ffe, /* all bit-rates               */
	.br_recv                =  0x1ffe, /* all bit-rates               */
	.bw                     =  0, /* inclusion depends                */
	.bw_send                =  0x1e, /* all bandwidths                */
	.bw_recv                =  0x1e, /* all bandwidths                */
	.ch_send                =  0, /* mono                             */
	.ch_recv                =  0, /* mono                             */
	.ch_aw_send             =  7, /* MAX_RF_FEC_OFFSET; do not change */
	.ch_aw_recv             =  0, /* not at start                     */
	.mode_set               =  0, /* all modes                        */
	.mode_change_period     =  0, /* not specified                    */
	.mode_change_neighbor   =  0, /* change to any                    */
};

unsigned int evs_parse_sdp_fmtp_br_bit(float br);
unsigned int evs_parse_sdp_fmtp_br(float br1, float br2);
unsigned int evs_parse_sdp_fmtp_bw(const char *res);
float evs_generate_sdp_fmtp_br_bit(unsigned int bit);
float evs_generate_sdp_fmtp_br(unsigned int br, float *br2);
const char *evs_generate_sdp_fmtp_bw(unsigned int bw);

static void evs_destroy(struct ast_format *format)
{
	struct evs_attr *attr = ast_format_get_attribute_data(format);

	ast_free(attr);
}

static int evs_clone(const struct ast_format *src, struct ast_format *dst)
{
	struct evs_attr *original = ast_format_get_attribute_data(src);
	struct evs_attr *attr = ast_malloc(sizeof(*attr));

	if (!attr) {
		return -1;
	}

	if (original) {
		*attr = *original;
	} else {
		*attr = default_evs_attr;
	}

	ast_format_set_attribute_data(dst, attr);

	return 0;
}

unsigned int evs_parse_sdp_fmtp_br_bit(float br)
{
	if (br <= 5.9f) {
		return 1;
	} else if (br <=  7.2f) {
		return 2;
	} else if (br <=  8.0f) {
		return 3;
	} else if (br <=  9.7f) {
		return 4;
	} else if (br <= 13.2f) {
		return 5;
	} else if (br <= 16.4f) {
		return 6;
	} else if (br <= 24.4f) {
		return 7;
	} else if (br <= 32.0f) {
		return 8;
	} else if (br <= 48.0f) {
		return 9;
	} else if (br <= 64.0f) {
		return 10;
	} else if (br <= 96.0f) {
		return 11;
	} else { /* 128.0f */
		return 12;
	}
}

unsigned int evs_parse_sdp_fmtp_br(float br1, float br2)
{
	unsigned int i, end;
	unsigned int res = 0;
	unsigned int start = evs_parse_sdp_fmtp_br_bit(br1);

	if (br2) {
		end = evs_parse_sdp_fmtp_br_bit(br2);
	} else {
		end = start;
	}
	for (i = start; i <= end; i = i + 1) {
		res |= (1 << i);
	}

	return res;
}

unsigned int evs_parse_sdp_fmtp_bw(const char *res)
{
	if (0 == strcmp("nb-swb", res)) {
		return 0x0e;
	} else if (0 == strcmp("nb-wb", res)) {
		return 0x06;
	} else if (0 == strcmp("fb", res)) {
		return 0x10;
	} else if (0 == strcmp("swb", res)) {
		return 0x08;
	} else if (0 == strcmp("wb", res)) {
		return 0x04;
	} else if (0 == strcmp("nb", res)) {
		return 0x02;
	} else { /* nb-fb */
		return 0x1e;
	}
}

static struct ast_format *evs_parse_sdp_fmtp(const struct ast_format *format, const char *attrib)
{
	struct ast_format *cloned;
	struct evs_attr *attr;
	unsigned int val;
	char *attributes;
	char *tmp;
	char res[7];
	float br1 = 0;
	float br2 = 0;
	const unsigned int size = 9; /* same as bit-field definition of mode_set */
	int v[size];
	/* init each slot as 'not specified' */
	for (val = 0; val < size; val = val + 1) {
		v[val] = -1;
	}

	cloned = ast_format_clone(format);
	if (!cloned) {
		return NULL;
	}
	attr = ast_format_get_attribute_data(cloned);

	/* lower-case everything, so we are case-insensitive */
	/* no implementation is known which is affected by this */
	attributes = ast_strdupa(attrib);
	for (tmp = attributes; *tmp; ++tmp) {
		*tmp = tolower(*tmp);
	} /* based on channels/chan_sip.c:process_a_sdp_image() */

	attr->evs_mode_switch = -1;
	tmp = strstr(attributes, "evs-mode-switch=");
	if (tmp) {
		if (sscanf(tmp, "evs-mode-switch=%30u", &val) == 1) {
			attr->evs_mode_switch = val;
		}
	}

	attr->hf_only = -1;
	tmp = strstr(attributes, "hf-only=");
	if (tmp) {
		if (sscanf(tmp, "hf-only=%30u", &val) == 1) {
			attr->hf_only = val;
		}
	}

	attr->dtx = 2;
	tmp = strstr(attributes, "dtx=");
	if (tmp) {
		if (sscanf(tmp, "dtx=%30u", &val) == 1) {
			attr->dtx = val;
		}
	}

	attr->dtx_send = 1;
	attr->dtx_recv = 2;
	tmp = strstr(attributes, "dtx-recv=");
	if (tmp) {
		if (sscanf(tmp, "dtx-recv=%30u", &val) == 1) {
			attr->dtx_send = val;
		}
	}

	attr->max_red = -1;
	tmp = strstr(attributes, "max-red=");
	if (tmp) {
		if (sscanf(tmp, "max-red=%30u", &val) == 1) {
			attr->max_red = val;
		}
	}

	attr->cmr = 0;
	tmp = strstr(attributes, "cmr=");
	if (tmp) {
		if (sscanf(tmp, "cmr=%30d", &val) == 1) {
			attr->cmr = val;
			attr->cmr_included = 1;
		}
	}

	attr->ch_recv = 0;
	tmp = strstr(attributes, "ch-send=");
	if (tmp) {
		if (sscanf(tmp, "ch-send=%30d", &val) == 1) {
			attr->ch_recv = val;
		}
	}

	attr->ch_send = 0;
	tmp = strstr(attributes, "ch-recv=");
	if (tmp) {
		if (sscanf(tmp, "ch-recv=%30d", &val) == 1) {
			attr->ch_send = val;
		}
	}

	attr->ch_aw_send =  0;
	attr->ch_aw_recv = -2;
	tmp = strstr(attributes, "ch-aw-recv=");
	if (tmp) {
		if (sscanf(tmp, "ch-aw-recv=%30d", &val) == 1) {
			attr->ch_aw_send = val;
		}
	}

	attr->br = 0;
	attr->br_recv = 0x1ffe; /* all bit-rates */
	attr->br_send = 0x1ffe; /* all bit-rates */
	tmp = strstr(attributes, "br=");
	if (tmp) {
		if (sscanf(tmp, "br=%4f-%4f", &br1, &br2) > 0) {
			attr->br_recv = evs_parse_sdp_fmtp_br(br1, br2);
			attr->br_send = evs_parse_sdp_fmtp_br(br1, br2);
			attr->br = 1; /* was included */
		}
		br2 = 0;
	}
	tmp = strstr(attributes, "br-send=");
	if (tmp) {
		if (sscanf(tmp, "br-send=%4f-%4f", &br1, &br2) > 0) {
			attr->br_recv = evs_parse_sdp_fmtp_br(br1, br2);
			attr->br_recv |= 0x0001; /* was included */
		}
		br2 = 0;
	}
	tmp = strstr(attributes, "br-recv=");
	if (tmp) {
		if (sscanf(tmp, "br-recv=%4f-%4f", &br1, &br2) > 0) {
			attr->br_send = evs_parse_sdp_fmtp_br(br1, br2);
			attr->br_send |= 0x0001; /* was included */
		}
		br2 = 0;
	}

	attr->bw = 0;
	attr->bw_recv = 0x1e; /* all bandwidths (nb-fb) */
	attr->bw_send = 0x1e; /* all bandwidths (nb-fb) */
	tmp = strstr(attributes, "bw=");
	if (tmp) {
		if (sscanf(tmp, "bw=%6[^ ;]", res) == 1) {
			attr->bw_recv = evs_parse_sdp_fmtp_bw(res);
			attr->bw_send = evs_parse_sdp_fmtp_bw(res);
			attr->bw = 1; /* was included in SDP */
		}
	}
	tmp = strstr(attributes, "bw-send=");
	if (tmp) {
		if (sscanf(tmp, "bw-send=%6[^ ;]", res) == 1) {
			attr->bw_recv = evs_parse_sdp_fmtp_bw(res);
			attr->bw_recv |= 0x01; /* was included in SDP */
		}
	}
	tmp = strstr(attributes, "bw-recv=");
	if (tmp) {
		if (sscanf(tmp, "bw-recv=%6[^ ;]", res) == 1) {
			attr->bw_send = evs_parse_sdp_fmtp_bw(res);
			attr->bw_send |= 0x01; /* was included in SDP */
		}
	}

	attr->mode_set = 0;
	tmp = strstr(attributes, "mode-set=");
	if (tmp) {
		if (sscanf(tmp, "mode-set=%30u,%30u,%30u,%30u,%30u,%30u,%30u,%30u,%30u",
				&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]) > 0) {
			for (val = 0; val < size; val = val + 1) {
				if (0 <= v[val] && v[val] < size) {
					attr->mode_set = (attr->mode_set | (1 << v[val]));
					attr->mode_current = v[val];
				}
			}
		}
	}

	attr->mode_change_period = 0;
	tmp = strstr(attributes, "mode-change-period=");
	if (tmp) {
		if (sscanf(tmp, "mode-change-period=%30u", &val) == 1) {
			attr->mode_change_period = val;
		}
	}

	attr->mode_change_neighbor = 0;
	tmp = strstr(attributes, "mode-change-neighbor=");
	if (tmp) {
		if (sscanf(tmp, "mode-change-neighbor=%30u", &val) == 1) {
			attr->mode_change_neighbor = val;
		}
	}

	return cloned;
}

float evs_generate_sdp_fmtp_br_bit(unsigned int bit)
{
	if (1 == bit) {
		return 5.9f;
	} else if (2 == bit) {
		return 7.2f;
	} else if (3 == bit) {
		return 8.0f;
	} else if (4 == bit) {
		return 9.6f;
	} else if (5 == bit) {
		return 13.2f;
	} else if (6 == bit) {
		return 16.4f;
	} else if (7 == bit) {
		return 24.4f;
	} else if (8 == bit) {
		return 32.0f;
	} else if (9 == bit) {
		return 48.0f;
	} else if (10 == bit) {
		return 64.0f;
	} else if (11 == bit) {
		return 96.0f;
	} else if (12 == bit) {
		return 128.0f;
	} else {
		return 13.2f;
		ast_log(LOG_ERROR, "bit %u is out of range\n", bit);
	}
}

float evs_generate_sdp_fmtp_br(unsigned int br, float *br2)
{
	unsigned int start = 1;
	unsigned int end = 12;
	unsigned int i;

	br = br & 0x1ffe; /* removes the included-in-SDP flag */

	for (i = 1; i <= 12; i = i + 1) {
		if (br & (1 << i)) {
			start = i;
			break;
		}
	}

	for (i = 12; 1 <= i; i = i - 1) {
		if (br & (1 << i)) {
			end = i;
			break;
		}
	}

	*br2 = evs_generate_sdp_fmtp_br_bit(end);
	return evs_generate_sdp_fmtp_br_bit(start);
}

const char *evs_generate_sdp_fmtp_bw(unsigned int bw)
{
	bw = bw & 0x1e; /* removes the included-in-SDP flag */

	if (bw == 0x0e) {
		return "nb-swb";
	} else if (bw == 0x06) {
		return "nb-wb";
	} else if (bw == 0x10) {
		return "fb";
	} else if (bw == 0x08) {
		return "swb";
	} else if (bw == 0x04) {
		return "wb";
	} else if (bw == 0x02) {
		return "nb";
	} else {
		return "nb-fb";
	}
}

static void evs_generate_sdp_fmtp(const struct ast_format *format, unsigned int payload, struct ast_str **str)
{
	int appended = 0;
	int listed = 0;
	struct evs_attr *attr = ast_format_get_attribute_data(format);

	if (!attr) {
		attr = &default_evs_attr;
	}

	if (-1 != attr->evs_mode_switch) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "evs-mode-switch=%d", attr->evs_mode_switch);
		appended = appended + 1;
	}
	if (-1 != attr->hf_only) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "hf-only=%d", attr->hf_only);
		appended = appended + 1;
	}
	if (2 != attr->dtx) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "dtx=%d", attr->dtx);
		appended = appended + 1;
	}
	if (2 != attr->dtx_recv) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "dtx-recv=%d", attr->dtx_recv);
		appended = appended + 1;
	}
	if (-1 != attr->max_red) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "max-red=%d", attr->max_red);
		appended = appended + 1;
	}
	if (attr->cmr || attr->cmr_included) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "cmr=%d", attr->cmr);
		appended = appended + 1;
	}

	if ((attr->br_send & 0x1ffe) == (attr->br_recv & 0x1ffe)) {
		if ((0 != attr->br) || (attr->br_send != 0x1ffe)) {
			float br1, br2;

			if (0 == appended) {
				ast_str_append(str, 0, "a=fmtp:%d ", payload);
			} else {
				ast_str_append(str, 0, ";");
			}
			br1 = evs_generate_sdp_fmtp_br(attr->br_send, &br2);
			if (br1 == br2) {
				ast_str_append(str, 0, "br=%g", br1);
			} else {
				ast_str_append(str, 0, "br=%g-%g", br1, br2);
			}
			appended = appended + 1;
		}
	}

	if ((0x01 & attr->br_send) ||
		(((attr->br_send & 0x1ffe) != (attr->br_recv & 0x1ffe)) && (attr->br_send != 0x1ffe))) {
		float br1, br2;

		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		br1 = evs_generate_sdp_fmtp_br(attr->br_send, &br2);
		if (br1 == br2) {
			ast_str_append(str, 0, "br-send=%g", br1);
		} else {
			ast_str_append(str, 0, "br-send=%g-%g", br1, br2);
		}
		appended = appended + 1;
	}

	if ((0x01 & attr->br_recv) ||
		(((attr->br_recv & 0x1ffe) != (attr->br_send & 0x1ffe)) && (attr->br_recv != 0x1ffe))) {
		float br1, br2;

		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		br1 = evs_generate_sdp_fmtp_br(attr->br_recv, &br2);
		if (br1 == br2) {
			ast_str_append(str, 0, "br-recv=%g", br1);
		} else {
			ast_str_append(str, 0, "br-recv=%g-%g", br1, br2);
		}
		appended = appended + 1;
	}

	if ((attr->bw_send & 0x1e) == (attr->bw_recv & 0x1e)) {
		if ((0 != attr->bw) || (attr->bw_send != 0x1e)) {
			if (0 == appended) {
				ast_str_append(str, 0, "a=fmtp:%d ", payload);
			} else {
				ast_str_append(str, 0, ";");
			}
			ast_str_append(str, 0, "bw=%s", evs_generate_sdp_fmtp_bw(attr->bw_send));
			appended = appended + 1;
		}
	}

	if ((0x01 & attr->bw_send) ||
		(((attr->bw_send & 0x1e) != (attr->bw_recv & 0x1e)) && (attr->bw_send != 0x1e))) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "bw-send=%s", evs_generate_sdp_fmtp_bw(attr->bw_send));
		appended = appended + 1;
	}

	if ((0x01 & attr->bw_recv) ||
		(((attr->bw_send & 0x1e) != (attr->bw_recv & 0x1e)) && (attr->bw_recv != 0x1e))) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "bw-recv=%s", evs_generate_sdp_fmtp_bw(attr->bw_recv));
		appended = appended + 1;
	}

	if (0 != attr->ch_send) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "ch-send=%d", attr->ch_send);
		appended = appended + 1;
	}
	if (0 != attr->ch_recv) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "ch-recv=%d", attr->ch_recv);
		appended = appended + 1;
	}
	if (0 != attr->ch_aw_recv && -2 != attr->ch_aw_recv) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "ch-aw-recv=%d", attr->ch_aw_recv);
		appended = appended + 1;
	}

	if (0 != attr->mode_set)
	{
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "mode-set=");
		if (attr->mode_set & 0x01) {
			if (0 == listed) {
				ast_str_append(str, 0, "0");
			} else {
				ast_str_append(str, 0, ",0");
			}
			listed = listed + 1;
		}
		if (attr->mode_set & 0x02) {
			if (0 == listed) {
				ast_str_append(str, 0, "1");
			} else {
				ast_str_append(str, 0, ",1");
			}
			listed = listed + 1;
		}
		if (attr->mode_set & 0x04) {
			if (0 == listed) {
				ast_str_append(str, 0, "2");
			} else {
				ast_str_append(str, 0, ",2");
			}
			listed = listed + 1;
		}
		if (attr->mode_set & 0x08) {
			if (0 == listed) {
				ast_str_append(str, 0, "3");
			} else {
				ast_str_append(str, 0, ",3");
			}
			listed = listed + 1;
		}
		if (attr->mode_set & 0x10) {
			if (0 == listed) {
				ast_str_append(str, 0, "4");
			} else {
				ast_str_append(str, 0, ",4");
			}
			listed = listed + 1;
		}
		if (attr->mode_set & 0x20) {
			if (0 == listed) {
				ast_str_append(str, 0, "5");
			} else {
				ast_str_append(str, 0, ",5");
			}
			listed = listed + 1;
		}
		if (attr->mode_set & 0x40) {
			if (0 == listed) {
				ast_str_append(str, 0, "6");
			} else {
				ast_str_append(str, 0, ",6");
			}
			listed = listed + 1;
		}
		if (attr->mode_set & 0x80) {
			if (0 == listed) {
				ast_str_append(str, 0, "7");
			} else {
				ast_str_append(str, 0, ",7");
			}
			listed = listed + 1;
		}
		if (attr->mode_set & 0x100) {
			if (0 == listed) {
				ast_str_append(str, 0, "8");
			} else {
				ast_str_append(str, 0, ",8");
			}
			listed = listed + 1;
		}
		appended = appended + 1;
	}
	if (0 != attr->mode_change_period) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "mode-change-period=%d", attr->mode_change_period);
		appended = appended + 1;
	}
	if (0 != attr->mode_change_neighbor) {
		if (0 == appended) {
			ast_str_append(str, 0, "a=fmtp:%d ", payload);
		} else {
			ast_str_append(str, 0, ";");
		}
		ast_str_append(str, 0, "mode-change-neighbor=%d", attr->mode_change_neighbor);
		appended = appended + 1;
	}
	if (0 != appended) {
		ast_str_append(str, 0, "\r\n");
	}
}

static enum ast_format_cmp_res evs_cmp(const struct ast_format *format1, const struct ast_format *format2)
{
	struct evs_attr *attr1 = ast_format_get_attribute_data(format1);
	struct evs_attr *attr2 = ast_format_get_attribute_data(format2);

	if (!attr1) {
		attr1 = &default_evs_attr;
	}

	if (!attr2) {
		attr2 = &default_evs_attr;
	}

	if ((1 < attr1->ch_recv || 1 < attr2->ch_recv) && (attr1->ch_recv != attr2->ch_recv)) {
		return AST_FORMAT_CMP_NOT_EQUAL;
	}

	if ((1 < attr1->ch_send || 1 < attr2->ch_send) && (attr1->ch_send != attr2->ch_send)) {
		return AST_FORMAT_CMP_NOT_EQUAL;
	}

	return AST_FORMAT_CMP_EQUAL;
}

static struct ast_format *evs_getjoint(const struct ast_format *format1, const struct ast_format *format2)
{
	struct evs_attr *attr1 = ast_format_get_attribute_data(format1);
	struct evs_attr *attr2 = ast_format_get_attribute_data(format2);
	struct evs_attr *attr_res;
	struct ast_format *jointformat = NULL;

	if (!attr1) {
		attr1 = &default_evs_attr;
	}

	if (!attr2) {
		attr2 = &default_evs_attr;
	}

	if (format1 == ast_format_evs) {
		jointformat = (struct ast_format *) format2;
	}
	if (format2 == ast_format_evs) {
		jointformat = (struct ast_format *) format1;
	}
	if (format1 == format2) {
		if (!jointformat) {
			ast_debug(3, "Both formats were not cached but the same.\n");
			jointformat = (struct ast_format *) format1;
		} else {
			ast_debug(3, "Both formats were cached.\n");
			jointformat = NULL;
		}
	}
	if (!jointformat) {
		ast_debug(3, "Which pointer shall be returned? Let us create a new one!\n");
		jointformat = ast_format_clone(format1);
	} else {
		ao2_bump(jointformat);
	}
	if (!jointformat) {
		return NULL;
	}
	attr_res = ast_format_get_attribute_data(jointformat);

	if (0 == attr1->mode_set && 0 == attr2->mode_set) {
		attr_res->mode_set = 0; /* both allowed all = 0 */
	} else if (0 != attr1->mode_set && 0 == attr2->mode_set) {
		attr_res->mode_set = attr1->mode_set; /* attr2 allowed all */
	} else if (0 == attr1->mode_set && 0 != attr2->mode_set) {
		attr_res->mode_set = attr2->mode_set; /* attr1 allowed all */
	} else { /* both parties restrict, let us check if they match */
		attr_res->mode_set = (attr1->mode_set & attr2->mode_set);
		if (0 == attr_res->mode_set) {
			/* not expected because everyone supports 0,1,2 */
			ast_log(LOG_WARNING, "no AMR-WB mode in common\n");
			return NULL;
		}
	}

	attr_res->br = MAX(attr1->br, attr2->br);
	/* build subset, copy the SDP bit */
	attr_res->br_send = (attr1->br_send & attr2->br_send) |
	                    (attr1->br_send & 0x01) |
	                    (attr2->br_send & 0x01);
	attr_res->br_recv = (attr2->br_recv & attr1->br_recv) |
	                    (attr1->br_recv & 0x01) |
	                    (attr2->br_recv & 0x01);
	if (0 == (attr_res->br_recv & 0x1ffe) || 0 == (attr_res->br_send & 0x1ffe)) {
		ast_log(LOG_WARNING, "no bitrate in common\n");
		return NULL;
	}

	attr_res->bw = MAX(attr1->bw, attr2->bw);
	/* build subset, copy the SDP bit */
	attr_res->bw_send = (attr1->bw_send & attr2->bw_send) |
	                    (attr1->bw_send & 0x01) |
	                    (attr2->bw_send & 0x01);
	attr_res->bw_recv = (attr2->bw_recv & attr1->bw_recv) |
	                    (attr1->bw_recv & 0x01) |
	                    (attr2->bw_recv & 0x01);
	if (0 == (attr_res->bw_recv & 0x1e) || 0 == (attr_res->bw_send & 0x1e)) {
		ast_log(LOG_WARNING, "no bandwidth in common\n");
		return NULL;
	}

	attr_res->evs_mode_switch = MAX(attr1->evs_mode_switch, attr2->evs_mode_switch);
	attr_res->hf_only = MAX(attr1->hf_only, attr2->hf_only);
	attr_res->dtx = MIN(attr1->dtx, attr2->dtx);
	attr_res->dtx_send = MIN(attr1->dtx_send, attr2->dtx_send);
	attr_res->dtx_recv = MIN(attr1->dtx_recv, attr2->dtx_recv);
	attr_res->max_red = MAX(attr1->max_red, attr2->max_red);

	if ((attr1->cmr && attr2->cmr) && (attr1->cmr != attr2->cmr)) {
		ast_log(LOG_WARNING, "please, revise your choice in struct default_evs_attr\n");
		return NULL;
	} else if (attr2->cmr) {
		attr_res->cmr = attr2->cmr;
	} else if (attr1->cmr) {
		attr_res->cmr = attr1->cmr;
	} else {
		attr_res->cmr = 0;
	}
	attr_res->cmr_included = MAX(attr1->cmr_included, attr2->cmr_included);

	if ((1 < attr1->ch_recv || 1 < attr2->ch_recv) && (attr1->ch_recv != attr2->ch_recv)) {
		return NULL;
	} else {
		attr_res->ch_recv = MAX(attr1->ch_recv, attr2->ch_recv);
	}
	if ((1 < attr1->ch_send || 1 < attr2->ch_send) && (attr1->ch_send != attr2->ch_send)) {
		return NULL;
	} else {
		attr_res->ch_send = MAX(attr1->ch_send, attr2->ch_send);
	}

	attr_res->ch_aw_send = MIN(attr1->ch_aw_send, attr2->ch_aw_send);
	attr_res->ch_aw_recv = MAX(attr1->ch_aw_recv, attr2->ch_aw_recv);

	attr_res->mode_change_period = MAX(attr1->mode_change_period, attr2->mode_change_period);
	attr_res->mode_change_neighbor = MAX(attr1->mode_change_neighbor, attr2->mode_change_neighbor);

	/* internal variables for transcoding module */
	/* starting point; later, changes with a change-mode request (CMR) */
	if (0 < attr_res->mode_set) {
		attr_res->mode_current = floor(log10(attr_res->mode_set) / log10(2));
	}

	return jointformat;
}

static struct ast_format_interface evs_interface = {
	.format_destroy = evs_destroy,
	.format_clone = evs_clone,
	.format_cmp = evs_cmp,
	.format_get_joint = evs_getjoint,
	.format_attribute_set = NULL,
	.format_parse_sdp_fmtp = evs_parse_sdp_fmtp,
	.format_generate_sdp_fmtp = evs_generate_sdp_fmtp,
};

static int load_module(void)
{
	if (ast_format_interface_register("evs", &evs_interface)) {
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"EVS Format Attribute Module",
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
);
