#include "globals.h"

#ifdef WITH_CARDREADER

#include "module-gbox.h"
#include "module-led.h"
#include "oscam-chk.h"
#include "oscam-client.h"
#include "oscam-ecm.h"
#include "oscam-emm.h"
#include "oscam-net.h"
#include "oscam-time.h"
#include "oscam-work.h"
#include "oscam-reader.h"
#include "reader-common.h"
//#include "csctapi/atr.h"
#include "csctapi/icc_async.h"
#include "readers.h" // required by the EMU reader

extern const struct s_cardsystem *cardsystems[];
extern char *RDR_CD_TXT[];

int32_t check_sct_len(const uint8_t *data, int32_t off)
{
	int32_t len = SCT_LEN(data);
	if(len + off > MAX_LEN)
	{
		cs_log_dbg(D_TRACE | D_READER, "check_sct_len(): smartcard section too long %d > %d", len, MAX_LEN - off);
		len = -1;
	}
	return len;
}

static void reader_nullcard(struct s_reader *reader)
{
	reader->csystem_active = false;
	reader->csystem = NULL;
	memset(reader->hexserial, 0, sizeof(reader->hexserial));
	memset(reader->prid, 0xFF, sizeof(reader->prid));
	memset(reader->sa, 0, sizeof(reader->sa));
#if defined(READER_NAGRA) || defined(READER_NAGRA_MERLIN)
	memset(reader->emm82u, 0, sizeof(reader->emm82u));
	memset(reader->emm84, 0, sizeof(reader->emm84));
	memset(reader->emm84s, 0, sizeof(reader->emm84s));
	memset(reader->emm83s, 0, sizeof(reader->emm83s));
	memset(reader->emm83u, 0, sizeof(reader->emm83u));
	memset(reader->emm87, 0, sizeof(reader->emm87));
#endif
	reader->caid = 0;
	reader->nprov = 0;
	cs_clear_entitlement(reader);
}

int32_t reader_cmd2icc(struct s_reader *reader, const uint8_t *buf, const int32_t l, uint8_t *cta_res, uint16_t *p_cta_lr)
{
	int32_t rc;
	*p_cta_lr = CTA_RES_LEN - 1; // FIXME not sure whether this one is necessary
	rdr_log_dump_dbg(reader, D_READER, buf, l, "write to cardreader");
	rc = ICC_Async_CardWrite(reader, (uint8_t *)buf, (uint16_t)l, cta_res, p_cta_lr);
	return rc;
}

#define CMD_LEN 5

int32_t card_write(struct s_reader *reader, const uint8_t *cmd, const uint8_t *data, uint8_t *response, uint16_t *response_length)
{
	int32_t datalen = MAX_ECM_SIZE; // default datalen is max ecm size defined
	uint8_t buf[MAX_ECM_SIZE + CMD_LEN];
	// always copy to be able to be able to use const buffer without changing all code
	memcpy(buf, cmd, CMD_LEN); // copy command

	if(data)
	{
		if(cmd[4])
		{
			datalen = cmd[4];
		}
		memcpy(buf + CMD_LEN, data, datalen);
		return (reader_cmd2icc(reader, buf, CMD_LEN + datalen, response, response_length));
	}
	else
		{ return (reader_cmd2icc(reader, buf, CMD_LEN, response, response_length)); }
}

static inline int reader_use_gpio(struct s_reader *reader)
{
	return reader->use_gpio && reader->detect > 4;
}

static int32_t reader_card_inserted(struct s_reader *reader)
{
	if(!reader_use_gpio(reader) && (reader->detect & 0x7f) > 3)
		{ return 1; }

	int32_t card;
	if(ICC_Async_GetStatus(reader, &card))
	{
		rdr_log(reader, "Error getting card status.");
		return 0; // corresponds with no card inside!!
	}
	return (card);
}

static int32_t reader_activate_card(struct s_reader *reader, ATR *atr, uint16_t deprecated)
{
	int32_t i, ret;

	if(reader->card_status != CARD_NEED_INIT)
		{ return 0; }

	/* Activate card */
	for(i = 0; i < 3; i++)
	{
		ret = ICC_Async_Activate(reader, atr, deprecated);
		if(!ret)
			{ break; }
		rdr_log(reader, "Error activating card.");
		led_status_card_activation_error();
		cs_sleepms(500);
	}
	if(ret) { return (0); }

	//rdr_log("ATR: %s", cs_hexdump(1, atr, atr_size, tmp, sizeof(tmp))); // FIXME
	cs_sleepms(300); // was 1000: the card is already answering (ATR read); fast reset runs without this delay and works
	return (1);
}

void cardreader_get_card_info(struct s_reader *reader)
{
	if(reader->disable_card_info)
		{ return; } // card info completely disabled for this reader
	if((reader->card_status == CARD_NEED_INIT) || (reader->card_status == CARD_INSERTED))
	{
		struct s_client *cl = reader->client;
		if(cl)
			{ cl->last = time((time_t *)0); }

		if(reader->csystem_active && reader->csystem && reader->csystem->card_info)
		{
			reader->csystem->card_info(reader);
		}
	}
}

void cardreader_poll_status(struct s_reader *reader)
{
	if (reader && reader->card_status == CARD_INSERTED)
	{
		if (reader->csystem_active && reader->csystem && reader->csystem->poll_status)
			{ reader->csystem->poll_status(reader); }
	}
}

static int32_t reader_get_cardsystem(struct s_reader *reader, ATR *atr)
{
	int32_t i;

#ifdef WITH_EMU
	if(reader->typ == R_EMU)
	{
		NULLFREE(reader->csystem_data);
		rdr_log(reader, "found card system %s", reader_emu.desc);
		reader->csystem = &reader_emu;
		reader->csystem_active = true;
		led_status_found_cardsystem();
		return (reader->csystem_active);
	}
#endif

	for(i = 0; cardsystems[i]; i++)
	{
		NULLFREE(reader->csystem_data);
		const struct s_cardsystem *csystem = cardsystems[i];
		if(csystem->card_init(reader, atr))
		{
			rdr_log(reader, "found card system %s", csystem->desc);
			reader->csystem = csystem;
			reader->csystem_active = true;
			led_status_found_cardsystem();
			break;
		}
		else
		{
			// On error free allocated card system data if any
			if(csystem->card_done)
				csystem->card_done(reader);
			NULLFREE(reader->csystem_data);
		}
	}

	if(!reader->csystem_active)
	{
		rdr_log(reader, "card system not supported");
		led_status_unsupported_card_system();
	}

	return (reader->csystem_active);
}

static inline int reader_is_conax(struct s_reader *reader)
{
	return reader->csystem && reader->csystem->desc && !strcmp(reader->csystem->desc, "conax");
}

void cardreader_do_reset(struct s_reader *reader)
{
	reader_nullcard(reader);
	ATR atr;
	int32_t ret = 0;
	int16_t i = 0;
	int16_t j = 0;

	if (reader->typ == R_SMART && reader->smartdev_found >= 4) j = 1; else j = 1; // back to a single start

	for (i= 0; i < j; i++)
	{
		ret = ICC_Async_Reset(reader, &atr, reader_activate_card, reader_get_cardsystem);

		if(ret == -1)
			{ return; }

		if(ret == 0)
		{
			uint16_t y;
			uint16_t deprecated;

			reader->resetalways = 0;
			if (reader->typ == R_SMART && reader->smartdev_found >= 4) y = 2; else y = 2;
			//rdr_log(reader, "the restart atempts in deprecated is %u", y);

			for(deprecated = reader->deprecated; deprecated < y; deprecated++)
			{
				if(!reader_activate_card(reader, &atr, deprecated)) { break; }

				ret = reader_get_cardsystem(reader, &atr);
				if(ret)
					{ break; }

				if(!deprecated)
					{ rdr_log(reader, "Normal mode failed, reverting to Deprecated Mode"); }
			}
			//try reset reader before each command
			if (!ret)
			{
				rdr_log(reader, "Try reset reader before each command");
				reader->resetalways = 1;
				if(!reader_activate_card(reader, &atr, reader->deprecated)) { break; }
				ret = reader_get_cardsystem(reader, &atr);
			}
		}

		if (ret)
		{
			rdr_log(reader,"THIS WAS A SUCCESSFUL START ATTEMPT No  %u out of max allotted of %u", (i + 1), j);
			break;
		}
		else
		{
			rdr_log(reader, "THIS WAS A FAILED START ATTEMPT No %u out of max allotted of %u", (i + 1), j);
		}
	}

	if(!ret)
	{
		reader->card_status = CARD_FAILURE;
		rdr_log(reader, "card initializing error");
		ICC_Async_DisplayMsg(reader, "AER");
		led_status_card_activation_error();
	}
	else
	{
		cardreader_get_card_info(reader);
		reader->card_status = CARD_INSERTED;
		reader->fast_reset_paused = 0;
		reader->fast_reset_idle_base = time(NULL); // idle countdown starts here, even if no ECM ever arrives
		do_emm_from_file(reader);
		// Start the interval after initialization; do not reset the card a second time immediately.
		reader->fast_reset_last = time(NULL);
		ICC_Async_DisplayMsg(reader, "AOK");
#ifdef MODULE_GBOX
		gbx_local_card_stat(LOCALCARDUP, reader->caid); // local card up
#endif
	}

	return;
}

/*
 * Fast Reset
 * Periodic lean card reset that keeps the card responsive.  It is deliberately
 * minimal: it reuses the current reader settings and skips card-system init by
 * default. It is scheduled only when no ECM is waiting and never takes priority
 * over queued ECM work. On failure it falls back to a full reset.
 */
static int32_t reader_activate_card_fast(struct s_reader *reader, ATR *atr, uint16_t deprecated)
{
	// card_status is deliberately NOT required to be CARD_NEED_INIT here: during a fast reset the
	// reader stays CARD_INSERTED so ECMs keep matching it and simply wait in its queue
	return !ICC_Async_Activate(reader, atr, deprecated);
}

static int32_t reader_fast_cardsystem(struct s_reader *reader, ATR *atr)
{
	const struct s_cardsystem *csystem = reader->csystem;
	if(!csystem || !csystem->card_init)
		{ return 0; }
	if(reader->fast_reset_nocardinit)
		{ return 1; }
	NULLFREE(reader->csystem_data);
	return csystem->card_init(reader, atr) ? 1 : 0;
}

void cardreader_do_fast_reset(struct s_reader *reader)
{
	if(!reader)
		{ return; }

	if(reader->fast_reset <= 0 || reader->card_status != CARD_INSERTED || !reader->csystem_active || !reader_is_conax(reader))
	{
		reader->fast_reset_queued = 0;
		return;
	}

	ATR atr;
	int32_t ret;
	// Set running before clearing queued so the periodic checker cannot queue a duplicate reset.
	reader->fr_ms_atr = reader->fr_ms_flush = reader->fr_ms_pps = reader->fr_ms_set = 0;
	reader->fast_reset_running = 1;
	reader->fast_reset_queued = 0;
	// Keep CARD_INSERTED so matching_reader() and dvbapi do not temporarily drop this reader.
	// Pending ECM jobs are allowed to finish before this maintenance job is scheduled.
	struct timeb fr_start, fr_end;
	cs_ftime(&fr_start);
	int8_t lean = 0;
	const struct s_cardreader *crdr_ops = reader->crdr;
	// TCMG-style lean reset: only when the card does not need a PPS after every reset
	if(crdr_ops && crdr_ops->lean_reset && reader->pps_state != 1 && !reader->fast_reset_lean_bad && reader->fast_reset_nocardinit)
	{
		if(crdr_ops->lean_reset(reader) == 0)
		{
			lean = 1; ret = 1;
			// A PPS-switched card must prove it still answers at the working speed: Conax record read
			// (same command as the card init). If it does not, drop to the normal reset and stop using lean.
			if(reader->pps_state == 1 && reader_is_conax(reader))
			{
				static const uint8_t probe[] = { 0xDD, 0x26, 0x00, 0x00, 0x03, 0x10, 0x01, 0x40 };
				uint8_t rsp[CTA_RES_LEN]; uint16_t lr = 0;
				if(ICC_Async_CardWrite(reader, (uint8_t *)probe, sizeof(probe), rsp, &lr) != OK || lr < 1 || rsp[0] != 0x98)
				{
					rdr_log(reader, "[lean probe failed -> normal reset from now on]");
					reader->fast_reset_lean_bad = 1;
					lean = 0; ret = 0;
				}
			}
		}
	}
	if(!lean)
	{
		ret = ICC_Async_Reset(reader, &atr, reader_activate_card_fast, reader_fast_cardsystem);
		if(ret == 0)
		{
			if(reader_activate_card_fast(reader, &atr, reader->deprecated))
				{ ret = !reader->fast_reset_nocardinit ? reader_fast_cardsystem(reader, &atr) : 1; }
		}
	}
	cs_ftime(&fr_end);
	reader->fast_reset_last = time(NULL);
	reader->fast_reset_running = 0;
	{
		int64_t fr_ms = comp_timeb(&fr_end, &fr_start);
		char fr_line[160];
		if(lean)
			{ snprintf(fr_line, sizeof(fr_line), "[lean: reset+ATR %d, settings %d, total %"PRId64"]", reader->fr_ms_atr, reader->fr_ms_set, fr_ms); }
		else
			{ snprintf(fr_line, sizeof(fr_line), "[reset+ATR %d, flush %d, PPS %d, settings %d, total %"PRId64"]",
					   reader->fr_ms_atr, reader->fr_ms_flush, reader->fr_ms_pps, reader->fr_ms_set, fr_ms); }
		rdr_log_dbg(reader, D_READER, "fast reset detail: path=%s pps_state=%d nocardinit=%d lean_bad=%d ret=%d",
					lean ? "lean" : "normal", reader->pps_state, reader->fast_reset_nocardinit, reader->fast_reset_lean_bad, ret);
		if(ret > 0)
		{
			if(fr_ms > 1500)
				{ rdr_log(reader, "%s SLOW", fr_line); }
			else
				{ rdr_log(reader, "%s", fr_line); }
		}
		else if(ret == 0)
			{ rdr_log(reader, "%s FAILED -> full reset", fr_line); }
		else
			{ rdr_log(reader, "%s FAILED", fr_line); }
	}

	if(ret > 0)
	{
		reader->card_status = CARD_INSERTED;
		if(!reader->fast_reset_nocardinfo)
			{ cardreader_get_card_info(reader); }
		rdr_log_dbg(reader, D_READER, "fast reset ok");
	}
	else if(ret < 0)
	{
		reader->card_status = CARD_INSERTED;
	}
	else
	{
		reader->card_status = CARD_NEED_INIT;
		cardreader_do_reset(reader);
	}
}

// Called every second from the client check loop.
static void cardreader_fast_reset_check(struct s_client *cl, struct s_reader *rdr)
{
	if(!cl || !rdr || rdr->fast_reset <= 0 || rdr->fast_reset_queued || rdr->fast_reset_running ||
			rdr->card_status != CARD_INSERTED || !rdr->csystem_active || !reader_is_conax(rdr))
		{ return; }

	time_t now = time(NULL);
	time_t activity = cl->lastecm;

	if(rdr->fast_reset_idle > 0)
	{
		if(rdr->fast_reset_paused)
		{
			if(activity > rdr->fast_reset_last) // activity again: resume with a fresh interval
			{
				rdr->fast_reset_paused = 0;
				rdr->fast_reset_last = now;
			}
			return;
		}
		// Idle is measured from the last ECM, or - if there was none since the card was initialised -
		// from the card init / enable time. Without that baseline the pause could only ever start after
		// the first ECM and fast reset kept running forever on a reader nobody uses.
		if(!rdr->fast_reset_idle_base)
			{ rdr->fast_reset_idle_base = now; }
		time_t idle_since = (activity > rdr->fast_reset_idle_base) ? activity : rdr->fast_reset_idle_base;
		if(now - idle_since >= rdr->fast_reset_idle)
		{
			rdr->fast_reset_paused = 1;
			return;
		}
	}

	if(!rdr->fast_reset_last)
		{ rdr->fast_reset_last = now; return; }
	if(now - rdr->fast_reset_last < rdr->fast_reset)
		{ return; }

	// Never put a maintenance reset ahead of requests already waiting for a CW.
	if(job_ecm_waiting(cl))
		{ return; }

	rdr->fast_reset_queued = 1;
	if(!add_job(cl, ACTION_READER_FAST_RESET, NULL, 0))
		{ rdr->fast_reset_queued = 0; }
}

static int32_t cardreader_device_init(struct s_reader *reader)
{
	int32_t rc = -1; // FIXME
	if(ICC_Async_Device_Init(reader))
		{ rdr_log(reader, "Cannot open device: %s", reader->device); }
	else
		{ rc = OK; }
	return ((rc != OK) ? 2 : 0); // exit code 2 means keep retrying, exit code 0 means all OK
}

int32_t cardreader_do_checkhealth(struct s_reader *reader)
{
	struct s_client *cl = reader->client;
	if(reader_card_inserted(reader))
	{
		if(reader->card_status == NO_CARD || reader->card_status == UNKNOWN)
		{
			rdr_log(reader, "card detected");
			led_status_card_detected();
			reader->card_status = CARD_NEED_INIT;
			add_job(cl, ACTION_READER_RESET, NULL, 0);
		}
	}
	else
	{
		rdr_log_dbg(reader, D_READER, "%s: !reader_card_inserted", __func__);
		if(reader->card_status == CARD_INSERTED || reader->card_status == CARD_NEED_INIT)
		{
			rdr_log(reader, "card ejected");
			reader_nullcard(reader);
			if(reader->csystem && reader->csystem->card_done)
				reader->csystem->card_done(reader);
			NULLFREE(reader->csystem_data);

			if(cl)
			{
				cl->lastemm = 0;
				cl->lastecm = 0;
			}
			led_status_card_ejected();
#ifdef MODULE_GBOX
			reader->card_status = NO_CARD;
			gbx_local_card_stat(LOCALCARDEJECTED, reader->caid);
#endif
		}
		reader->card_status = NO_CARD;
	}
	rdr_log_dbg(reader, D_READER, "%s: reader->card_status = %d, ret = %d", __func__,
				reader->card_status, reader->card_status == CARD_INSERTED);

	return reader->card_status == CARD_INSERTED;
}

// Called from the reader thread when it has just run out of jobs: a moment where no ECM is waiting,
// so an overdue fast reset can go in right away instead of waiting for the next 1 s health tick.
void cardreader_fast_reset_idle_check(struct s_client *cl, struct s_reader *rdr)
{
	if(!rdr || !rdr->enable || !rdr->active)
		{ return; }
	cardreader_fast_reset_check(cl, rdr);
}

// Check for card inserted or card removed on pysical reader
void cardreader_checkhealth(struct s_client *cl, struct s_reader *rdr)
{
	if(!rdr || !rdr->enable || !rdr->active)
		{ return; }
	cardreader_fast_reset_check(cl, rdr);
	add_job(cl, ACTION_READER_CHECK_HEALTH, NULL, 0);
}

void cardreader_reset(struct s_client *cl)
{
	add_job(cl, ACTION_READER_RESET, NULL, 0);
}

void cardreader_init_locks(void)
{
	ICC_Async_Init_Locks();
}

bool cardreader_init(struct s_reader *reader)
{
	struct s_client *client = reader->client;
	client->typ = 'r';
	int8_t i = 0;
	set_localhost_ip(&client->ip);

	while((cardreader_device_init(reader) == 2) && i < 10)
	{
		cs_sleepms(2000);
		if(!ll_contains(configured_readers, reader) || !is_valid_client(client) || reader->enable != 1)
			{ return false; }
		i++;
	}

	if (i >= 10)
	{
		reader->card_status = READER_DEVICE_ERROR;
		cardreader_close(reader);
		reader->enable = 0;
		return false;
	}
	else
	{
		if(reader->typ == R_INTERNAL)
		{
			if(boxtype_is("dm500") || boxtype_is("dm600pvr"))
				{reader->cardmhz = 3150;}

			if(boxtype_is("dm7025"))
				{reader->cardmhz = 8300;}

			if((!strncmp(boxtype_get(), "vu", 2 ))||(boxtype_is("ini-8000am")))
				{reader->cardmhz = 2700; reader->mhz = 450;} // only one speed for VU+ and Atemio Nemesis due to usage of TDA8024
		}

		if(
		reader->typ == R_INTERNAL && (
		(strncmp(boxtype_get(), "dm500hdv2", 9) == 0) ||
		(strncmp(boxtype_get(), "dm800sev2", 9) == 0) ||
		(strncmp(boxtype_get(), "dm7020hd",  8) == 0) ||
		(strncmp(boxtype_get(), "dm500hd",   7) == 0) ||
		(strncmp(boxtype_get(), "dm800se",   7) == 0) ||
		(strncmp(boxtype_get(), "dm7080",    6) == 0) ||
		(strncmp(boxtype_get(), "dm8000",    6) == 0) ||
		(strncmp(boxtype_get(), "dm520",     5) == 0) ||
		(strncmp(boxtype_get(), "dm525",     5) == 0) ||
		(strncmp(boxtype_get(), "dm800",     5) == 0) ||
		(strncmp(boxtype_get(), "dm820",     5) == 0) ||
		(strncmp(boxtype_get(), "dm900",     5) == 0) ||
		(strncmp(boxtype_get(), "dm920",     5) == 0) ||
		(strncmp(boxtype_get(), "one",       3) == 0) ||
		(strncmp(boxtype_get(), "two",       3) == 0)) )
		{
			rdr_log(reader, "Dreambox %s found! set Internal Card-MHz = 2700", boxtype_get() );
			reader->cardmhz = 2700;
			return true;
		}

		if((reader->cardmhz > 2000) && (reader->typ != R_SMART))
		{
			rdr_log(reader, "Reader initialized (device=%s, detect=%s%s, pll max=%.2f MHz, wanted mhz=%.2f MHz)",
					reader->device,
					reader->detect & 0x80 ? "!" : "",
					RDR_CD_TXT[reader->detect & 0x7f],
					(float)reader->cardmhz / 100,
					(float)reader->mhz / 100);
			rdr_log(reader,"Reader sci internal, detected box type: %s", boxtype_get());
		}
		else
		{
			if (reader->typ == R_SMART || is_smargo_reader(reader))
			{
				rdr_log_dbg(reader, D_IFD, "clocking for smartreader with smartreader or smargo protocol");
				if (reader->cardmhz >= 2000) reader->cardmhz =  369; else
				if (reader->cardmhz >= 1600) reader->cardmhz = 1600; else
				if (reader->cardmhz >= 1200) reader->cardmhz = 1200; else
				if (reader->cardmhz >= 961)  reader->cardmhz =  961; else
				if (reader->cardmhz >= 800)  reader->cardmhz =  800; else
				if (reader->cardmhz >= 686)  reader->cardmhz =  686; else
				if (reader->cardmhz >= 600)  reader->cardmhz =  600; else
				if (reader->cardmhz >= 534)  reader->cardmhz =  534; else
				if (reader->cardmhz >= 480)  reader->cardmhz =  480; else
				if (reader->cardmhz >= 436)  reader->cardmhz =  436; else
				if (reader->cardmhz >= 400)  reader->cardmhz =  400; else
				if (reader->cardmhz >= 369)  reader->cardmhz =  369; else
				if (reader->cardmhz == 357)  reader->cardmhz =  369; else // 357 not a default smartreader setting
				if (reader->cardmhz >= 343)  reader->cardmhz =  343; else
											 reader->cardmhz =  320;

				if (reader->mhz >= 1600) reader->mhz = 1600; else
				if (reader->mhz >= 1200) reader->mhz = 1200; else
				if (reader->mhz >= 961)  reader->mhz =  961; else
				if (reader->mhz >= 900)  reader->mhz =  900; else
				if (reader->mhz >= 800)  reader->mhz =  800; else
				if (reader->mhz >= 686)  reader->mhz =  686; else
				if (reader->mhz >= 600)  reader->mhz =  600; else
				if (reader->mhz >= 534)  reader->mhz =  534; else
				if (reader->mhz >= 480)  reader->mhz =  480; else
				if (reader->mhz >= 436)  reader->mhz =  436; else
				if (reader->mhz >= 400)  reader->mhz =  369; else
				if (reader->mhz >= 369)  reader->mhz =  369; else
				if (reader->mhz == 357)  reader->mhz =  369; else // 357 not a default smartreader setting
				if (reader->mhz >= 343)  reader->mhz =  343; else
										 reader->mhz =  320;
			}

			if ((reader->typ == R_SMART || is_smargo_reader(reader)) && reader->autospeed == 1)
			{
				rdr_log(reader, "Reader initialized (device=%s, detect=%s%s, mhz= AUTO, cardmhz=%d)",
						reader->device,
						reader->detect & 0x80 ? "!" : "",
						RDR_CD_TXT[reader->detect & 0x7f],
						reader->cardmhz);
			}
			else
			{
				rdr_log(reader, "Reader initialized (device=%s, detect=%s%s, mhz=%d, cardmhz=%d)",
						reader->device,
						reader->detect & 0x80 ? "!" : "",
						RDR_CD_TXT[reader->detect & 0x7f],
						reader->mhz,
						reader->cardmhz);

				if (reader->typ == R_INTERNAL && !(reader->cardmhz > 2000))
					rdr_log(reader,"Reader sci internal, detected box type: %s", boxtype_get());
			}
		}
		return true;
	}
}

void cardreader_close(struct s_reader *reader)
{
	ICC_Async_Close(reader);
}

void reader_post_process(struct s_reader *reader)
{
	// some systems eg. nagra2/3 needs post process after receiving cw from card
	// To save ECM/CW time we added this function after writing ecm answer
	if(reader->csystem_active && reader->csystem && reader->csystem->post_process)
	{
		reader->csystem->post_process(reader);
	}
}

int32_t cardreader_do_ecm(struct s_reader *reader, ECM_REQUEST *er, struct s_ecm_answer *ea)
{
	int32_t rc = -1;
	if((rc = cardreader_do_checkhealth(reader)))
	{
		rdr_log_dbg(reader, D_READER, "%s: cardreader_do_checkhealth returned rc=%d", __func__, rc);
		struct s_client *cl = reader->client;
		if(cl)
		{
			cl->last_srvid = er->srvid;
			cl->last_caid = er->caid;
			cl->last_provid = er->prid;
			cl->last = time((time_t *)0);
		}

		if(reader->csystem_active && reader->csystem && reader->csystem->do_ecm)
		{
			rc = reader->csystem->do_ecm(reader, er, ea);
			rdr_log_dbg(reader, D_READER, "%s: after csystem->do_ecm rc=%d", __func__, rc);
		}
		else
			{ rc = 0; }
	}
	rdr_log_dbg(reader, D_READER, "%s: ret rc=%d", __func__, rc);
	return (rc);
}

int32_t cardreader_do_emm(struct s_reader *reader, EMM_PACKET *ep)
{
	int32_t rc;

	// check health does not work with new card status check but is actually not needed for emm.
	if(reader->typ == R_SMART)
	{
		rc = 1;
	}
	else
	{
		rc = cardreader_do_checkhealth(reader);
	}

	if(rc)
	{
		if((1 << (ep->emm[0] % 0x80)) & reader->b_nano)
			{ return 3; }

		if(reader->csystem_active && reader->csystem && reader->csystem->do_emm)
			{ rc = reader->csystem->do_emm(reader, ep); }
		else
			{ rc = 0; }
	}

	if(rc > 0) { cs_ftime(&reader->emm_last); } // last time emm written is now!
	return (rc);
}

int32_t cardreader_do_rawcmd(struct s_reader *reader, CMD_PACKET *cp)
{
	int32_t rc;
	rc = -9;  // means no dedicated support by csystem
	if(reader->csystem_active && reader->csystem && reader->csystem->do_rawcmd)
	{
		rc = reader->csystem->do_rawcmd(reader, cp);
	}
	return (rc);
}

void cardreader_process_ecm(struct s_reader *reader, struct s_client *cl, ECM_REQUEST *er)
{
	struct timeb tps, tpe;
	struct s_ecm_answer ea;
	memset(&ea, 0, sizeof(struct s_ecm_answer));

#ifdef WITH_EXTENDED_CW
	// Correct CSA mode is CBC - default to that instead
	ea.cw_ex.algo_mode = CW_ALGO_MODE_CBC;
#endif

	cs_ftime(&tps);
	int32_t rc = cardreader_do_ecm(reader, er, &ea);
	cs_ftime(&tpe);

	rdr_log_dbg(reader, D_READER, "%s: cardreader_do_ecm returned rc=%d (ERROR=%d)", __func__, rc, ERROR);

	ea.rc = E_FOUND; // default assume found
	ea.rcEx = 0; // no special flag

	if(rc == ERROR)
	{
		char buf[CS_SERVICENAME_SIZE];
		rdr_log_dbg(reader, D_READER, "Error processing ecm for caid %04X, provid %06X, srvid %04X, servicename: %s",
						er->caid, er->prid, er->srvid, get_servicename(cl, er->srvid, er->prid, er->caid, buf, sizeof(buf)));
		ea.rc = E_NOTFOUND;
		ea.rcEx = 0;
		ICC_Async_DisplayMsg(reader, "Eer");
	}

	if(rc == E_CORRUPT)
	{
		char buf[CS_SERVICENAME_SIZE];
		rdr_log_dbg(reader, D_READER, "Error processing ecm for caid %04X, provid %06X, srvid %04X, servicename: %s",
						er->caid, er->prid, er->srvid, get_servicename(cl, er->srvid, er->prid, er->caid, buf, sizeof(buf)));
		ea.rc = E_NOTFOUND;
		ea.rcEx = E2_WRONG_CHKSUM; // flag it as wrong checksum
		memcpy(ea.msglog, "Invalid ecm type for card", 25);
	}
#ifdef CS_CACHEEX_AIO
	er->localgenerated = 1;
#endif
	write_ecm_answer(reader, er, ea.rc, ea.rcEx, ea.cw, ea.msglog, ea.tier, &ea.cw_ex);

	cl->lastecm = time((time_t *)0);
#ifdef WITH_DEBUG
	if(cs_dblevel & D_READER)
	{
		char ecmd5[17 * 3];
		cs_hexdump(0, er->ecmd5, 16, ecmd5, sizeof(ecmd5));

		rdr_log_dbg(reader, D_READER, "ecm hash: %s real time: %"PRId64" ms", ecmd5, comp_timeb(&tpe, &tps));
	}
#endif
	reader_post_process(reader);
}

#endif
