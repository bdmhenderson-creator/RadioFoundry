// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// YSF DN on a channel, the radio core's side (radio_ysf.h)
#include "config.h"
#if CONFIG_YSF	// the whole file
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "radio/radio_ysf.h"
#include "radio/radio_bands.h"
#include "ysf/ysf_engine.h"
#include "ysf/ysf_codeplug.h"
#include "ysf/ysf_id.h"
#include "codeplug/codeplug_modes.h"
#include "dmr/ambe/ambeVoice.h"
#include "dmr/call/dmr_call.h"
#if CONFIG_YSF_VW
#include "p25/p25Voice.h"
#endif

// The voice frame YSF hands on is the one the DMR side's voice path takes and gives (ysf.h)
_Static_assert(YSF_VOICE_BYTES == DMR_AMBE_FRAME_BYTES, "one voice frame size");
_Static_assert(RADIO_YSF_RID_LEN == YSF_RID_LEN && RADIO_YSF_CALL_LEN == YSF_CS_LEN, "one field size");


static radio_ysf_settings_t set;
// Rooms: the command armed for the next transmission, and the node's last reply as published (the provider's task
// writes it, any task reads it: wx_seq odd while it writes). The room list itself stays in the engine's parser
// (ysf_wx_rx_t.out): it goes with the engine, as the page needs the YSF channel anyway
static uint8_t wx_buf[YSF_WX_CMD_MAX], wx_len, wx_cmd = YSF_WX_NONE;
static uint32_t wx_arg;
static radio_ysf_wx_t wx_state = { .asked_cmd = YSF_WX_NONE };
static volatile uint8_t wx_seq;
// This radio's RID and where it came from: worked out on the radio task only (init, and again at each begin while
// it is the fallback, which follows the codeplug); other tasks copy it under rid_seq (odd while it is written)
static char rid[RADIO_YSF_RID_LEN + 1];
static bool rid_weak;
static volatile uint8_t rid_seq;
static ysf_engine_t *eng;		// while running
static radio_ysf_stats_t stats;
static ysf_call_stats_t call_base;	// the call layer's counters already counted (they restart with the engine)
static radio_air_counts_t air_base;	// the same for the engine's and the session's

static void rid_update(void)
{
	hal_chip_t chip;
	char r[RADIO_YSF_RID_LEN + 1];
	bool have = board.system != NULL && board.system->chip != NULL && board.system->chip(&chip) &&
		    ysf_uid_usable(chip.uid);

	if (have) {
		ysf_rid_from_uid(chip.uid, r);
	} else {
		char name[CODEPLUG_NAME_LEN + 1];

		codeplug_radio_name(name);
		ysf_rid_fallback(codeplug_dmr_id(), name, r);
	}
	rid_seq++;
	memcpy(rid, r, sizeof rid);
	rid_weak = !have;
	rid_seq++;
}

void radio_ysf_init(void)
{
	memset(&set, 0, sizeof set);
	eng = NULL;
	rid_update();		// the chip's ID now; a fallback is worked out again at each begin, once the codeplug is read
}

void radio_ysf_settings(radio_ysf_settings_t *out)
{
	*out = set;
}

void radio_ysf_set_settings(const radio_ysf_settings_t *s)
{
	set = *s;
	if (set.tx_dgid > RADIO_YSF_DGID_MAX)
		set.tx_dgid = 0;
	if (set.rx_dgid > RADIO_YSF_DGID_MAX)
		set.rx_dgid = 0;
	if (set.temp_dgid > RADIO_YSF_DGID_MAX)
		set.temp_chan = 0;
}

uint32_t radio_ysf_fit(void)
{
	return board.air != NULL ? ysf_engine_fit(board.air) : 0;
}

bool radio_ysf_fits(void)
{
	return (radio_ysf_fit() & (AIR_FIT_RX | AIR_FIT_TX)) != 0;
}

bool radio_ysf_take(channel_t *ch)
{
	if (ch->kind != CODEPLUG_KIND_YSF || !radio_ysf_fits() || radio_listen_band(ch->rx_hz))
		return false;
	ch->mode = RADIO_MODE_YSF;
	ch->bw = RADIO_BW_12K5;
	return true;
}

// ── Who we are on air ──
// The RID: from the MCU's unique ID, the one source that differs from radio to radio by construction. Only a board
// without one falls back to the DMR ID and callsign, which a copied codeplug would share: the view says so
// (RADIO_YSF_RID_WEAK) and the screen that shows the RID marks it. Worked out at first use and, for the fallback, at
// each engine start (it follows a codeplug change); read by any task


void radio_ysf_rid(char out[RADIO_YSF_RID_LEN + 1], bool *weak)
{
	uint8_t s;
	bool w;

	do {
		s = rid_seq;
		memcpy(out, rid, sizeof rid);
		w = rid_weak;
	} while ((s & 1) || s != rid_seq);
	if (weak)
		*weak = w;
}

void radio_ysf_callsign(const char *radio_name, char out[RADIO_YSF_CALL_LEN + 1])
{
	ysf_radio_t r;

	ysf_radio_load(&r);
	if (r.callsign[0] != '\0') {
		memcpy(out, r.callsign, sizeof r.callsign);
		return;
	}
	ysf_callsign_from_name(radio_name, out);
}

// The engine's configuration for channel ch at number (0: the VFO): a live stored YSF channel's tag gives the
// DG-IDs (or the quick settings' DG-ID for it, for now), else the settings
static void params(const channel_t *ch, uint16_t number, const char *radio_name, ysf_engine_cfg_t *c)
{
	ysf_chan_t yc;
	bool weak;

	memset(c, 0, sizeof *c);
	c->silence = dmr_ambe_silence;
	if (number != set.temp_chan)
		set.temp_chan = 0;	// another channel (or the VFO): the quick DG-ID was for the one left
	if (number != 0 && number == set.temp_chan) {
		c->rx_dgid = set.temp_dgid;
		c->tx.dgid = set.temp_dgid;
	} else if (number != 0 && ysf_chan_load(number, ch, &yc)) {
		c->rx_dgid = yc.rx_dgid;
		c->tx.dgid = yc.tx_dgid;
	} else {
		c->rx_dgid = set.rx_dgid;
		c->tx.dgid = set.tx_dgid;
	}
	c->tx.mr = YSF_MR_DIRECT;
	radio_ysf_callsign(radio_name, c->tx.callsign);
	radio_ysf_rid(c->tx.radio_id, &weak);
	c->tx_inhibit = c->tx.callsign[0] == '\0';
#if CONFIG_YSF_VW
	// A stored channel set to VW (its voice byte, ysf-codeplug.md 2) sends it; the VFO and a channel in YSF for now
	// send DN
	c->tx.vw = number != 0 && ysf_chan_load(number, ch, &yc) && yc.vw;
	if (c->tx.vw)
		c->silence = imbe_silence;
#endif
}

// ── Voice: DN through the DMR side's voice path (ambeVoice.h), as radio_nxdn.c uses it ──
#if !CONFIG_YSF_VW
static void v_start(const ysf_call_t *c, void *ctx) { (void)c; (void)ctx; radio_air_voice_rx_start(); }
static void v_voice(const uint8_t v[YSF_VOICE_BYTES], void *ctx) { (void)ctx; radio_air_voice_rx_frame(v); }
static void v_end(void *ctx) { (void)ctx; radio_air_voice_rx_end(); }
static bool v_tx(uint8_t *v, void *ctx) { (void)ctx; return radio_air_voice_tx_next(v); }
#define v_update	NULL
#define v_voice_fr	NULL
#else
// ── ... and VW through P25's full-rate voice path (p25Voice.h, core/mbe), ysf.md 10.3 ──
// One over is DN or VW. DN plays on the DMR vocoder, so the full-rate codec's memory can be that vocoder's lent
// block (hal_vocoder_t.lend) for as long as a VW over plays or is sent, and VW costs no RAM of its own: the radio
// task stops the DMR voice path, takes the block and starts P25's on it when a VW over comes, and gives it back once
// the over has played out, before the DMR path (and with it the vocoder, which rebuilds its state after a lend) runs
// again. Only one path owns the audio at a time: `fr_on` says which.
//
// The engine's hooks run in the provider's task, the switch in the radio task. Neither waits for the other: each
// says it is in and looks for the other (as radio_p25.c's follower does). The provider's hook that finds the radio
// task switching drops its frame (the decoder conceals one); the radio task that finds a hook running tries at its
// next tick. A frame for the path that is off is dropped too: the switch follows within a tick, and a VW over's
// header comes 100 ms before its first voice
static volatile uint8_t fr_on;			// the full-rate path owns the audio: radio task writes
static volatile uint8_t in_hook, in_switch;
static bool rx_fr;				// the call the hooks have is VW (provider's task)
static bool tx_fr;				// a VW transmission is on (radio task)
#ifndef HAL_VOCODER_LEND_BYTES
// A vocoder that lends nothing (no platform that runs YSF today): memory of its own
static p25_voice_mem_t fr_own;
#endif

static bool enter(volatile uint8_t *mine, const volatile uint8_t *other)
{
	*mine = 1;
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	if (*other) {
		*mine = 0;
		return false;
	}
	return true;
}

static void leave(volatile uint8_t *mine)
{
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	*mine = 0;
}

// The DN hooks reach the DMR path, the VW ones P25's, each only while its path owns the audio (a DN start on the
// DMR path would start its stream from this task)
static bool hook_in(bool fr)
{
	if (!enter(&in_hook, &in_switch))
		return false;
	if ((fr_on != 0) != fr) {
		leave(&in_hook);
		return false;
	}
	return true;
}

static void call_end(bool fr)
{
	if (!hook_in(fr))
		return;
	if (fr)
		p25VoiceHooks()->rx_end(NULL);
	else
		radio_air_voice_rx_end();
	leave(&in_hook);
}

static void call_start(const ysf_call_t *c)
{
	rx_fr = c->dt == YSF_DT_VW;
	if (!hook_in(rx_fr))
		return;
	if (rx_fr)
		p25VoiceHooks()->rx_start(NULL, NULL);
	else
		radio_air_voice_rx_start();
	leave(&in_hook);
}

static void v_start(const ysf_call_t *c, void *ctx) { (void)ctx; call_start(c); }

// A call that turns out DN or VW after it started (a late entry's first frames lost): the other path from here
static void v_update(const ysf_call_t *c, void *ctx)
{
	(void)ctx;
	if ((c->dt == YSF_DT_VW) != rx_fr && (c->dt == YSF_DT_VW || c->dt == YSF_DT_VD2)) {
		call_end(rx_fr);
		call_start(c);
	}
}

static void v_voice(const uint8_t v[YSF_VOICE_BYTES], void *ctx)
{
	(void)ctx;
	if (hook_in(false)) {
		radio_air_voice_rx_frame(v);
		leave(&in_hook);
	}
}

static void v_voice_fr(const uint8_t fr[YSF_VW_BYTES], uint8_t errs, void *ctx)
{
	(void)ctx;
	if (!hook_in(true)) {
		stats.vw_dropped++;
		return;
	}
	p25VoiceHooks()->rx_voice(fr, errs, NULL);
	leave(&in_hook);
}

static void v_end(void *ctx) { (void)ctx; call_end(rx_fr); }

// The encoder's next frame, from the path of the transmission (the engine sends silence while it has none)
static bool v_tx(uint8_t *v, void *ctx)
{
	bool fr = eng->tx.id.vw, got = false;

	(void)ctx;
	if (hook_in(fr)) {
		got = fr ? p25VoiceHooks()->tx_voice(v, NULL) : radio_air_voice_tx_next(v);
		leave(&in_hook);
	}
	return got;
}

static void *fr_mem(void)
{
#ifdef HAL_VOCODER_LEND_BYTES
	_Static_assert(sizeof(p25_voice_mem_t) <= HAL_VOCODER_LEND_BYTES, "VW's codec does not fit the vocoder's block");
	size_t len = 0;
	void *m = board.vocoder != NULL && board.vocoder->lend != NULL ? board.vocoder->lend(&len) : NULL;

	if (m == NULL || len < sizeof(p25_voice_mem_t) || ((uintptr_t)m & 7u) != 0)
		return NULL;
	return m;
#else
	return &fr_own;
#endif
}

// Hand the audio to the full-rate path (on) or back to the DMR one. Radio task. False: not now (a hook is running,
// or no memory), the caller tries again
static bool fr_switch(bool on)
{
	void *m;

	if ((fr_on != 0) == on)
		return true;
	if (!enter(&in_switch, &in_hook))
		return false;
	if (on) {
		ambeVoiceStop();		// the vocoder's last call: from here its block is free
		m = fr_mem();
		if (m == NULL) {
			ambeVoiceStart();
			stats.vw_no_mem++;
			leave(&in_switch);
			return false;
		}
		p25VoiceBegin(m);
		fr_on = 1;
		stats.vw_overs++;
	} else {
		fr_on = 0;
		p25VoiceEnd();			// the stream stopped: the block is the vocoder's again
		ambeVoiceStart();
	}
	leave(&in_switch);
	return true;
}

// Every tick while the engine runs: the full-rate path while a VW over is played or sent, and until it played out
static void fr_poll(const ysf_engine_view_t *ev)
{
	bool want;

	if (tx_fr)
		want = true;
	else if (ev->rx_active && ev->rx_played)
		want = ev->rx.dt == YSF_DT_VW;
	else
		want = fr_on && p25VoicePlaying();
	fr_switch(want);
	if (fr_on)
		p25VoicePoll();
}
#endif

// A node's reply: published for the screens. A list keeps the page asked for as its first entry
static void v_wx(const ysf_wx_t *w, void *ctx)
{
	(void)ctx;
	wx_seq++;
	if (w->kind == YSF_WX_ALL) {
		wx_state.search = w->search;
		wx_state.rooms = w->rooms;
		wx_state.rooms_total = w->rooms_total;
	}
	// The parser keeps the node and room of the last replies, and drops them when another node answers
	wx_state.connected = w->connected;
	memcpy(wx_state.node_name, w->node_name, sizeof wx_state.node_name);
	memcpy(wx_state.room_id, w->room_id, sizeof wx_state.room_id);
	memcpy(wx_state.room_name, w->room_name, sizeof wx_state.room_name);
	memcpy(wx_state.node_call, w->node_call, sizeof wx_state.node_call);
	wx_state.kind = w->kind;
	wx_state.serial++;
	wx_seq++;
}

static const ysf_engine_hooks_t voice_hooks = { v_start, v_update, v_voice, v_end, v_tx, v_wx, v_voice_fr };

bool radio_ysf_wx_arm(uint8_t cmd, uint32_t arg, const char *text)
{
	wx_len = ysf_wx_command(cmd, arg, text, wx_buf);
	wx_cmd = wx_len ? cmd : YSF_WX_NONE;
	wx_arg = arg;
	return wx_len != 0;
}

void radio_ysf_wx_disarm(void)
{
	wx_cmd = YSF_WX_NONE;
}

void radio_ysf_wx(radio_ysf_wx_t *out)
{
	uint8_t s;

	do {
		s = wx_seq;
		*out = wx_state;
	} while ((s & 1) || s != wx_seq);
}

bool radio_ysf_wx_room(uint8_t i, ysf_wx_room_t *out)
{
	uint8_t s;
	bool ok;

	do {
		const ysf_engine_t *e = eng;

		s = wx_seq;
		ok = e != NULL && i < wx_state.rooms;
		if (ok)
			*out = e->rx.wx.out.room[i];
	} while ((s & 1) || s != wx_seq);
	return ok;
}

bool radio_ysf_begin(const channel_t *ch, uint16_t number, const char *radio_name)
{
	ysf_engine_cfg_t cfg;

	radio_ysf_end();
	stats.begins++;
	if (rid_weak)
		rid_update();	// the fallback follows the codeplug
	params(ch, number, radio_name, &cfg);
	eng = radio_air_engine_get(sizeof(ysf_engine_t));
	if (eng == NULL || board.air == NULL || ysf_engine_fit(board.air) == 0) {
		eng = NULL;
		stats.begin_fails++;
		return false;
	}
	ambeVoiceStart();
	if (!ysf_engine_begin(eng, board.air, &cfg, &voice_hooks, NULL)) {
		ambeVoiceStop();
		eng = NULL;
		stats.begin_fails++;
		return false;
	}
	memset(&call_base, 0, sizeof call_base);
	memset(&air_base, 0, sizeof air_base);
	return true;
}

void radio_ysf_end(void)
{
	if (eng == NULL)
		return;
	wx_seq++;
	wx_state.rooms = 0;	// the list lived in the engine
	wx_seq++;
	ysf_engine_end(eng);		// no hook runs after it
#if CONFIG_YSF_VW
	if (fr_on) {
		fr_on = 0;
		p25VoiceEnd();
	}
	tx_fr = rx_fr = false;
#endif
	ambeVoiceStop();
	eng = NULL;
}

bool radio_ysf_running(void)
{
	return eng != NULL;
}

void radio_ysf_retuned(void)
{
	if (eng != NULL)
		ysf_engine_retuned(eng);
}

void radio_ysf_apply(const channel_t *ch, uint16_t number, const char *radio_name)
{
	ysf_engine_cfg_t cfg;

	if (eng == NULL || ysf_engine_tx_state(eng) != AIR_TX_IDLE)
		return;
	params(ch, number, radio_name, &cfg);
	ysf_engine_set(eng, &cfg);
}

static void count(const ysf_engine_view_t *ev)
{
	const ysf_call_stats_t *c = &eng->rx.stats;

	radio_air_count(&stats.air, &air_base, ev->rx_calls, ev->tx_calls, ev->tx_filler, &eng->av.session.stats);
	stats.frames += c->frames - call_base.frames;
	stats.fich_fail += c->fich_fail - call_base.fich_fail;
	stats.csd_fail += c->csd_fail - call_base.csd_fail;
	stats.dch_fail += c->dch_fail - call_base.dch_fail;
	stats.voice += c->voice - call_base.voice;
	stats.voice_held += c->voice_held - call_base.voice_held;
	stats.fec_bits += c->fec_bits - call_base.fec_bits;
	stats.vch_fixed += c->vch_fixed - call_base.vch_fixed;
	call_base = *c;
}

bool radio_ysf_poll(radio_ysf_view_t *v)
{
	ysf_engine_view_t ev;
	radio_ysf_view_t n;
	bool weak;
	char me[RADIO_YSF_RID_LEN + 1];

	if (eng == NULL) {
		n = *v;		// the last call heard stays
		n.flags = 0;
		n.tx = AIR_TX_IDLE;
		n.rx_dgid = set.rx_dgid;
		n.tx_dgid = set.tx_dgid;
	} else {
		ysf_engine_view(eng, &ev);
		count(&ev);
#if CONFIG_YSF_VW
		fr_poll(&ev);
#endif
		memset(&n, 0, sizeof n);
		n.flags = RADIO_YSF_ON;
		if (ev.rx_active) {
			n.flags |= RADIO_YSF_RX;
			if (ev.rx_played)
				n.flags |= RADIO_YSF_PLAYED;
			if (ev.rx.late)
				n.flags |= RADIO_YSF_LATE;
			if (ev.rx.dt != YSF_DT_VD2 && !(CONFIG_YSF_VW && ev.rx.dt == YSF_DT_VW))
				n.flags |= RADIO_YSF_NOT_DN;
			if (ev.rx.dt == YSF_DT_DATA)
				n.flags |= RADIO_YSF_DATA;
		}
		if (eng->cfg.tx_inhibit)
			n.flags |= RADIO_YSF_NO_CALL;
		n.tx = ev.tx;
		n.dgid = ev.rx.dgid;
		n.rx_dgid = eng->cfg.rx_dgid;
		n.tx_dgid = eng->cfg.tx.dgid;
		memcpy(n.src, ev.rx.src, sizeof n.src);
		memcpy(n.src_rid, ev.rx.src_id, sizeof n.src_rid);
	}
	radio_ysf_rid(me, &weak);
	if (weak)
		n.flags |= RADIO_YSF_RID_WEAK;
	n.fit = (uint16_t)radio_ysf_fit();
	bool changed = memcmp(&n, v, sizeof n) != 0;
	*v = n;
	return changed;
}

static bool ysf_no_tx(void)
{
	return eng != NULL && (eng->av.session.fit & AIR_FIT_TX) == 0;
}

static bool ysf_tx_prepare(void)
{
	if (eng == NULL)
		return false;
	if (wx_cmd != YSF_WX_NONE) {
		// A room command armed: this transmission is it, and ends by itself
		uint8_t cmd = wx_cmd;

		wx_cmd = YSF_WX_NONE;
		if (!ysf_engine_tx_prepare_data(eng, wx_buf, wx_len))
			return false;
		wx_seq++;
		wx_state.asked++;
		wx_state.asked_cmd = cmd;
		if (cmd == YSF_WX_ALL || cmd == YSF_WX_SEARCH)
			wx_state.rooms_first = (uint16_t)(cmd == YSF_WX_ALL && wx_arg >= 1 ? wx_arg : 1);
		wx_seq++;
		return true;
	}
#if CONFIG_YSF_VW
	// The transmission's voice path owns the audio before the engine asks for its first frame
	tx_fr = eng->cfg.tx.vw;
	if (!fr_switch(tx_fr)) {
		tx_fr = false;
		return false;
	}
	if (!ysf_engine_tx_prepare(eng)) {
		tx_fr = false;
		return false;
	}
	if (tx_fr)
		p25VoiceTx(true);
	else
		radio_air_voice_tx(true);
	return true;
#else
	if (!ysf_engine_tx_prepare(eng))
		return false;
	radio_air_voice_tx(true);
	return true;
#endif
}

// The encoder stops: the operator let go, or the transmission ended
static void voice_tx_off(void)
{
#if CONFIG_YSF_VW
	if (tx_fr) {
		p25VoiceTx(false);
		return;
	}
#endif
	radio_air_voice_tx(false);
}

static bool ysf_tx_go(void)
{
	return eng != NULL && ysf_engine_tx_go(eng);
}

static void ysf_tx_stop(void)
{
	if (eng != NULL) {
		voice_tx_off();
		ysf_engine_tx_stop(eng);
	}
}

static void ysf_tx_abort(void)
{
	if (eng != NULL) {
		voice_tx_off();
		ysf_engine_tx_abort(eng);
#if CONFIG_YSF_VW
		tx_fr = false;		// fr_poll gives the audio back
#endif
	}
}

static uint8_t ysf_tx_state(void)
{
	return eng != NULL ? (uint8_t)ysf_engine_tx_state(eng) : (uint8_t)AIR_TX_IDLE;
}

static void ysf_tx_clear(void)
{
	if (eng != NULL) {
		voice_tx_off();
		ysf_engine_tx_clear(eng);
#if CONFIG_YSF_VW
		tx_fr = false;
#endif
	}
}

const radio_air_ops_t radio_ysf_air = {
	radio_ysf_running, ysf_no_tx, ysf_tx_prepare, ysf_tx_go, ysf_tx_stop, ysf_tx_abort, ysf_tx_state, ysf_tx_clear,
};

void radio_ysf_stats(radio_ysf_stats_t *out)
{
	*out = stats;
}

void radio_ysf_reset_stats(void)
{
	memset(&stats, 0, sizeof stats);
}

#endif	// CONFIG_YSF
