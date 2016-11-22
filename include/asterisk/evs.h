#ifndef _AST_FORMAT_EVS_H_
#define _AST_FORMAT_EVS_H_

struct evs_attr {
	/* EVS modes
	 * -1 primary mode; not included in SDP because default
	 *  0 primary mode; always included in SDP
	 *  1 AMR-WB IO mode */
	int evs_mode_switch;
	/* Formats
	 * -1 Header-Full and Compact; not included in SDP because default
	 *  0 Header-Full and Compact; always included in SDP
	 *  1 Header-Full only */
	int hf_only;
	/* Discontinuous Transmission
	 *  0 disabled
	 *  1 might happen; always included in SDP
	 *  2 might happen; not included in SDP because default */
	unsigned int dtx;
	/*  Asterisk as DTX sender
	 *  0 receiver told sender (Asterisk) not to do DTX
	 *  1 receiver allows DTX */
	unsigned int dtx_send;
	/*  Asterisk as DTX receiver
	 *  0 not able to handle DTX
	 *  1 wants to do DTX
	 *  2 wants to do DTX; not included in SDP because default */
	unsigned int dtx_recv;
	int max_red;
	/* Change-Mode Request
	 * -1 limited to AMR-WB
	 *  0 unlimited, might be present; always included in SDP
	 *  1 unlimited, present always */
	int cmr;
	unsigned int cmr_included;
	/* Bit-rates and Bandwitdhs
	 * the general flag "br" stores, whether "br" was sent in SDP
	 * same for send and recv; bit 0 stores, whether it was in SDP
	 * the remaining bit indicate the acceptable bit-rates/bandwidths */
	unsigned int br;
	unsigned int br_send:13;
	unsigned int br_recv:13;
	unsigned int bw;
	unsigned int bw_send:5;
	unsigned int bw_recv:5;
	/* Channels
	 * 0; mono channel; not included in SDP because default
	 * 1; mono channel; always included in SDP
	 * n(umber) of channels, like stereo and so on */
	unsigned int ch_send;
	unsigned int ch_recv;
	/* Aware of underlying RTP channel characteristics (RTCP)
	 * -2 not at start; not included in SDP because default
	 * -1 not aware, no link to RTCP
	 *  0 not at start
	 *  n partial redundancy */
	int ch_aw_send;
	int ch_aw_recv;
	unsigned int mode_set:9;
	unsigned int mode_change_period;
	unsigned int mode_change_neighbor;
	/* internal variables for transcoding module */
	unsigned char mode_current; /* see evs_clone for default */
};

#endif /* _AST_FORMAT_EVS_H */
