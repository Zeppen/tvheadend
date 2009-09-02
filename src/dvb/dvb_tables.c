/*
 *  DVB Table support
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

#include "tvhead.h"
#include "dvb.h"
#include "dvb_support.h"
#include "epg.h"
#include "transports.h"
#include "channels.h"
#include "psi.h"
#include "notify.h"

#define TDT_CRC           0x1
#define TDT_QUICKREQ      0x2
#define TDT_INC_TABLE_HDR 0x4

static int tdt_id_tally;

/**
 *
 */
typedef struct th_dvb_table {
  /**
   * Flags, must never be changed after creation.
   * We inspect it without holding global_lock
   */
  int tdt_flags;

  /**
   * Cycle queue
   * Tables that did not get a fd or filter in hardware will end up here
   * waiting for any other table to be received so it can reuse that fd.
   * Only linked if fd == -1
   */
  TAILQ_ENTRY(th_dvb_table) tdt_pending_link;

  /**
   * File descriptor for filter
   */
  int tdt_fd;

  LIST_ENTRY(th_dvb_table) tdt_link;

  char *tdt_name;

  void *tdt_opaque;
  int (*tdt_callback)(th_dvb_mux_instance_t *tdmi, uint8_t *buf, int len,
		      uint8_t tableid, void *opaque);


  int tdt_count;
  int tdt_pid;

  struct dmx_sct_filter_params *tdt_fparams;

  int tdt_id;

} th_dvb_table_t;




/**
 * Helper for preparing a section filter parameter struct
 */
static struct dmx_sct_filter_params *
dvb_fparams_alloc(void)
{
  return calloc(1, sizeof(struct dmx_sct_filter_params));
}


/**
 *
 */
static void
dvb_table_fastswitch(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_table_t *tdt;
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  char buf[100];

  if(!tdmi->tdmi_table_initial)
    return;

  LIST_FOREACH(tdt, &tdmi->tdmi_tables, tdt_link)
    if((tdt->tdt_flags & TDT_QUICKREQ) && tdt->tdt_count == 0)
      return;

  tdmi->tdmi_table_initial = 0;
  tda->tda_initial_num_mux--;


  if(tda->tda_logging) {
    dvb_mux_nicename(buf, sizeof(buf), tdmi);
    tvhlog(LOG_INFO, "dvb", "\"%s\" initial scan completed for \"%s\"",
	     tda->tda_rootpath, buf);
  }
  dvb_adapter_mux_scanner(tda);
}


/**
 *
 */
static void
tdt_open_fd(th_dvb_mux_instance_t *tdmi, th_dvb_table_t *tdt)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  struct epoll_event e;
  
  assert(tdt->tdt_fd == -1);
  TAILQ_REMOVE(&tdmi->tdmi_table_queue, tdt, tdt_pending_link);

  tdt->tdt_fd = open(tda->tda_demux_path, O_RDWR);

  if(tdt->tdt_fd != -1) {

    tdt->tdt_id = ++tdt_id_tally;

    e.events = EPOLLIN;
    e.data.u64 = ((uint64_t)tdt->tdt_fd << 32) | tdt->tdt_id;

    if(epoll_ctl(tda->tda_table_epollfd, EPOLL_CTL_ADD, tdt->tdt_fd, &e)) {
      close(tdt->tdt_fd);
      tdt->tdt_fd = -1;
    } else {
      if(ioctl(tdt->tdt_fd, DMX_SET_FILTER, tdt->tdt_fparams)) {
	close(tdt->tdt_fd);
	tdt->tdt_fd = -1;
      }
    }
  }

  if(tdt->tdt_fd == -1)
    TAILQ_INSERT_TAIL(&tdmi->tdmi_table_queue, tdt, tdt_pending_link);
}


/**
 * Close FD for the given table and put table on the pending list
 */
static void
tdt_close_fd(th_dvb_mux_instance_t *tdmi, th_dvb_table_t *tdt)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;

  assert(tdt->tdt_fd != -1);

  epoll_ctl(tda->tda_table_epollfd, EPOLL_CTL_DEL, tdt->tdt_fd, NULL);
  close(tdt->tdt_fd);

  tdt->tdt_fd = -1;
  TAILQ_INSERT_TAIL(&tdmi->tdmi_table_queue, tdt, tdt_pending_link);
}


/**
 *
 */
static void
dvb_proc_table(th_dvb_mux_instance_t *tdmi, th_dvb_table_t *tdt, uint8_t *sec,
	       int r)
{
  int chkcrc = tdt->tdt_flags & TDT_CRC;
  int tableid, len;
  uint8_t *ptr;
  int ret;

  /* It seems some hardware (or is it the dvb API?) does not
     honour the DMX_CHECK_CRC flag, so we check it again */
  if(chkcrc && psi_crc32(sec, r))
    return;
      
  r -= 3;
  tableid = sec[0];
  len = ((sec[1] & 0x0f) << 8) | sec[2];
  
  if(len < r)
    return;

  ptr = &sec[3];
  if(chkcrc) len -= 4;   /* Strip trailing CRC */

  if(tdt->tdt_flags & TDT_INC_TABLE_HDR)
    ret = tdt->tdt_callback(tdmi, sec, len + 3, tableid, tdt->tdt_opaque);
  else
    ret = tdt->tdt_callback(tdmi, ptr, len, tableid, tdt->tdt_opaque);
  
  if(ret == 0)
    tdt->tdt_count++;

  if(tdt->tdt_flags & TDT_QUICKREQ)
    dvb_table_fastswitch(tdmi);
}

/**
 *
 */
static void *
dvb_table_input(void *aux)
{
  th_dvb_adapter_t *tda = aux;
  int r, i, tid, fd, x;
  struct epoll_event ev[1];
  uint8_t sec[4096];
  th_dvb_mux_instance_t *tdmi;
  th_dvb_table_t *tdt;
  int64_t t;

  while(1) {
    x = epoll_wait(tda->tda_table_epollfd, ev, sizeof(ev) / sizeof(ev[0]), -1);

    for(i = 0; i < x; i++) {
    
      tid = ev[i].data.u64 & 0xffffffff;
      fd  = ev[i].data.u64 >> 32; 

      if(!(ev[i].events & EPOLLIN))
	continue;

      if((r = read(fd, sec, sizeof(sec))) < 3)
	continue;

      pthread_mutex_lock(&global_lock);
      if((tdmi = tda->tda_mux_current) != NULL) {
	t = getclock_hires();
	/*
	 * Supress first 250ms of table info. It seems that sometimes
	 * the tuners not have actually tuned once they have returned
	 * from the ioctl(). So we will wait some time before we start
	 * accepting tables. 
	 * Not a perfect tix...
	 */
	if(t - tdmi->tdmi_table_start >= 250000) {
      
	  LIST_FOREACH(tdt, &tdmi->tdmi_tables, tdt_link)
	    if(tdt->tdt_id == tid)
	      break;

	  if(tdt != NULL) {
	    dvb_proc_table(tdmi, tdt, sec, r);

	    /* Any tables pending (that wants a filter/fd) */
	    if(TAILQ_FIRST(&tdmi->tdmi_table_queue) != NULL) {
	      tdt_close_fd(tdmi, tdt);

	      tdt = TAILQ_FIRST(&tdmi->tdmi_table_queue);
	      assert(tdt != NULL);

	      tdt_open_fd(tdmi, tdt);
	    }
	  }
	}
      }
      pthread_mutex_unlock(&global_lock);
    }
  }
  return NULL;
}



/**
 *
 */
void
dvb_table_init(th_dvb_adapter_t *tda)
{
  pthread_t ptid;
  tda->tda_table_epollfd = epoll_create(50);
  pthread_create(&ptid, NULL, dvb_table_input, tda);
}


/**
 *
 */
static void
dvb_tdt_destroy(th_dvb_adapter_t *tda, th_dvb_mux_instance_t *tdmi,
		th_dvb_table_t *tdt)
{
  LIST_REMOVE(tdt, tdt_link);

  if(tdt->tdt_fd == -1) {
    TAILQ_REMOVE(&tdmi->tdmi_table_queue, tdt, tdt_pending_link);
  } else {
    epoll_ctl(tda->tda_table_epollfd, EPOLL_CTL_DEL, tdt->tdt_fd, NULL);
    close(tdt->tdt_fd);
  }

  free(tdt->tdt_name);
  free(tdt->tdt_fparams);
  free(tdt);
}




/**
 * Add a new DVB table
 */
static void
tdt_add(th_dvb_mux_instance_t *tdmi, struct dmx_sct_filter_params *fparams,
	int (*callback)(th_dvb_mux_instance_t *tdmi, uint8_t *buf, int len,
			 uint8_t tableid, void *opaque), void *opaque,
	const char *name, int flags, int pid, th_dvb_table_t *tdt)
{
  th_dvb_table_t *t;

  LIST_FOREACH(t, &tdmi->tdmi_tables, tdt_link) {
    if(pid == t->tdt_pid) {
      free(tdt);
      free(fparams);
      return;
    }
  }

  if(fparams == NULL)
    fparams = dvb_fparams_alloc();

  if(flags & TDT_CRC) fparams->flags |= DMX_CHECK_CRC;
  fparams->flags |= DMX_IMMEDIATE_START;
  fparams->pid = pid;


  if(tdt == NULL)
    tdt = calloc(1, sizeof(th_dvb_table_t));

  tdt->tdt_name = strdup(name);
  tdt->tdt_callback = callback;
  tdt->tdt_opaque = opaque;
  tdt->tdt_pid = pid;
  tdt->tdt_flags = flags;
  tdt->tdt_fparams = fparams;
  LIST_INSERT_HEAD(&tdmi->tdmi_tables, tdt, tdt_link);
  tdt->tdt_fd = -1;
  TAILQ_INSERT_TAIL(&tdmi->tdmi_table_queue, tdt, tdt_pending_link);

  tdt_open_fd(tdmi, tdt);
}


/**
 * DVB Descriptor; Short Event
 */
static int
dvb_desc_short_event(uint8_t *ptr, int len, 
		     char *title, size_t titlelen,
		     char *desc,  size_t desclen)
{
  int r;

  if(len < 4)
    return -1;
  ptr += 3; len -= 3;

  if((r = dvb_get_string_with_len(title, titlelen, ptr, len)) < 0)
    return -1;
  ptr += r; len -= r;

  if((r = dvb_get_string_with_len(desc, desclen, ptr, len)) < 0)
    return -1;

  return 0;
}


/**
 * DVB Descriptor; Service
 */
static int
dvb_desc_service(uint8_t *ptr, int len, uint8_t *typep, 
		 char *provider, size_t providerlen,
		 char *name, size_t namelen)
{
  int r;

  if(len < 2)
    return -1;

  *typep = ptr[0];

  ptr++;
  len--;

  if((r = dvb_get_string_with_len(provider, providerlen, ptr, len)) < 0)
    return -1;
  ptr += r; len -= r;

  if((r = dvb_get_string_with_len(name, namelen, ptr, len)) < 0)
    return -1;
  ptr += r; len -= r;
  return 0;
}


/**
 * DVB EIT (Event Information Table)
 */
static int
dvb_eit_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  th_transport_t *t;
  channel_t *ch;
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;

  uint16_t serviceid;
  int version;
  uint8_t section_number;
  uint8_t last_section_number;
  uint16_t transport_stream_id;
  uint16_t original_network_id;
  uint8_t segment_last_section_number;
  uint8_t last_table_id;

  uint16_t event_id;
  time_t start_time, stop_time;

  int duration;
  int dllen;
  uint8_t dtag, dlen;

  char title[256];
  char desc[5000];
  epg_content_type_t *ect;

  event_t *e;

  lock_assert(&global_lock);

  //  printf("EIT!, tid = %x\n", tableid);

  if(tableid < 0x4e || tableid > 0x6f || len < 11)
    return -1;

  serviceid                   = ptr[0] << 8 | ptr[1];
  version                     = ptr[2] >> 1 & 0x1f;
  section_number              = ptr[3];
  last_section_number         = ptr[4];
  transport_stream_id         = ptr[5] << 8 | ptr[6];
  original_network_id         = ptr[7] << 8 | ptr[8];
  segment_last_section_number = ptr[9];
  last_table_id               = ptr[10];

  if((ptr[2] & 1) == 0) {
    /* current_next_indicator == next, skip this */
    return -1;
  }

  len -= 11;
  ptr += 11;

  /* Search all muxes on adapter */
  LIST_FOREACH(tdmi, &tda->tda_muxes, tdmi_adapter_link)
    if(tdmi->tdmi_transport_stream_id == transport_stream_id)
      break;
  
  if(tdmi == NULL)
    return -1;

  t = dvb_transport_find(tdmi, serviceid, 0, NULL);
  if(t == NULL)
    return 0;

  ch = t->tht_ch;
  if(ch == NULL)
    return 0;

  while(len >= 12) {
    event_id                  = ptr[0] << 8 | ptr[1];
    start_time                = dvb_convert_date(&ptr[2]);
    duration                  = bcdtoint(ptr[7] & 0xff) * 3600 +
                                bcdtoint(ptr[8] & 0xff) * 60 +
                                bcdtoint(ptr[9] & 0xff);
    dllen                     = ((ptr[10] & 0x0f) << 8) | ptr[11];

    len -= 12;
    ptr += 12;

    if(dllen > len)
      break;
    stop_time = start_time + duration;

    if(stop_time < dispatch_clock) {
      /* Already come to pass, skip over it */
      len -= dllen;
      ptr += dllen;
      continue;
    }

    if((e = epg_event_create(ch, start_time, start_time + duration,
			     event_id)) == NULL) {
      len -= dllen;
      ptr += dllen;
      continue;
    }

    ect = NULL;
    *title = 0;
    *desc = 0;
    while(dllen > 0) {
      dtag = ptr[0];
      dlen = ptr[1];

      len -= 2; ptr += 2; dllen -= 2; 

      if(dlen > len)
	break;

      switch(dtag) {
      case DVB_DESC_SHORT_EVENT:
	if(!dvb_desc_short_event(ptr, dlen,
				 title, sizeof(title),
				 desc,  sizeof(desc))) {
	  epg_event_set_title(e, title);
	  epg_event_set_desc(e, desc);
	}
	break;

      case DVB_DESC_CONTENT:
	if(dlen >= 2) {
	  /* We only support one content type per event atm. */
	  ect = epg_content_type_find_by_dvbcode(*ptr);
	  epg_event_set_content_type(e, ect);
	}
	break;
      }

      len -= dlen; ptr += dlen; dllen -= dlen;
    }
  }
  return 0;
}


/**
 * DVB SDT (Service Description Table)
 */
static int
dvb_sdt_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  th_transport_t *t;
  int version;
  uint8_t section_number;
  uint8_t last_section_number;
  uint16_t service_id;
  uint16_t transport_stream_id;
  uint16_t original_network_id;

  int reserved;
  int running_status, free_ca_mode;
  int dllen;
  uint8_t dtag, dlen;

  char provider[256];
  char chname0[256], *chname;
  uint8_t stype;
  int l;

  if(len < 8)
    return -1;

  transport_stream_id         = ptr[0] << 8 | ptr[1];
  version                     = ptr[2] >> 1 & 0x1f;
  section_number              = ptr[3];
  last_section_number         = ptr[4];
  original_network_id         = ptr[5] << 8 | ptr[6];
  reserved                    = ptr[7];

  if((ptr[2] & 1) == 0) {
    /* current_next_indicator == next, skip this */
    return -1;
  }

  len -= 8;
  ptr += 8;


  while(len >= 5) {
    service_id                = ptr[0] << 8 | ptr[1];
    reserved                  = ptr[2];
    running_status            = (ptr[3] >> 5) & 0x7;
    free_ca_mode              = (ptr[3] >> 4) & 0x1;
    dllen                     = ((ptr[3] & 0x0f) << 8) | ptr[4];

    len -= 5;
    ptr += 5;

    if(dllen > len)
      break;

    stype = 0;

    chname = NULL;

    while(dllen > 2) {
      dtag = ptr[0];
      dlen = ptr[1];

      len -= 2; ptr += 2; dllen -= 2; 

      if(dlen > len)
	break;

      switch(dtag) {
      case DVB_DESC_SERVICE:
	if(dvb_desc_service(ptr, dlen, &stype,
			    provider, sizeof(provider),
			    chname0, sizeof(chname0)) == 0) {
	  chname = chname0;
	  /* Some providers insert spaces.
	     Clean up that (both heading and trailing) */
	  while(*chname <= 32 && *chname != 0)
	    chname++;

	  l = strlen(chname);
	  while(l > 1 && chname[l - 1] <= 32) {
	    chname[l - 1] = 0;
	    l--;
	  }

	  if(l == 0) {
	    chname = chname0;
	    snprintf(chname0, sizeof(chname0), "noname-sid-0x%x", service_id);
	  }

	  t = dvb_transport_find(tdmi, service_id, 0, NULL);
	  if(t == NULL)
	    break;

	  if(t->tht_servicetype != stype ||
	     t->tht_scrambled != free_ca_mode ||
	     strcmp(t->tht_provider    ?: "", provider) ||
	     strcmp(t->tht_svcname     ?: "", chname  )) {
	    
	    t->tht_servicetype = stype;
	    t->tht_scrambled = free_ca_mode;
	    
	    free((void *)t->tht_provider);
	    t->tht_provider = strdup(provider);
	    
	    free((void *)t->tht_svcname);
	    t->tht_svcname = strdup(chname);
	    
	    t->tht_config_save(t);
	  }
	}
	break;
      }

      len -= dlen; ptr += dlen; dllen -= dlen;
    }
  }
  return 0;
}


/**
 * PAT - Program Allocation table
 */
static int
dvb_pat_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  uint16_t service, pmt, tsid;
  th_transport_t *t;

  if(len < 5)
    return -1;

  if((ptr[2] & 1) == 0) {
    /* current_next_indicator == next, skip this */
    return -1;
  }

  tsid = (ptr[0] << 8) | ptr[1];

  if(tdmi->tdmi_transport_stream_id != tsid)
    dvb_mux_set_tsid(tdmi, tsid);

  ptr += 5;
  len -= 5;

  while(len >= 4) {
    service =  ptr[0]         << 8 | ptr[1];
    pmt     = (ptr[2] & 0x1f) << 8 | ptr[3];

    if(service != 0) {
      t = dvb_transport_find(tdmi, service, pmt, NULL);
      dvb_table_add_transport(tdmi, t, pmt);
    }
    ptr += 4;
    len -= 4;
  }
  return 0;
}


typedef struct ca_stream {
  th_dvb_table_t tdt;
  int cs_caid;
} ca_stream_t;



/**
 * CA - Conditional Access
 */
static int
dvb_ca_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		uint8_t tableid, void *opaque)
{
  return 0;
}

/**
 * CAT - Conditional Access Table
 */
static int
dvb_cat_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  int tag, tlen;
  uint16_t caid;
  uint16_t pid;
  ca_stream_t *cs;

  if((ptr[2] & 1) == 0) {
    /* current_next_indicator == next, skip this */
    return -1;
  }

  ptr += 5;
  len -= 5;

  while(len > 2) {
    tag = *ptr++;
    tlen = *ptr++;
    len -= 2;
    switch(tag) {
    case DVB_DESC_CA:
      caid =  (ptr[0]        << 8)  | ptr[1];
      pid  = ((ptr[2] & 0x1f << 8)) | ptr[3];

      if(pid == 0)
	break;

      cs = calloc(1, sizeof(ca_stream_t));
      cs->cs_caid = caid;
      tdt_add(tdmi, NULL, dvb_ca_callback, cs, "CA", 
	      TDT_INC_TABLE_HDR, pid, &cs->tdt);
      break;

    default:
      break;
    }

    ptr += tlen;
    len -= tlen;
  }
  return 0;
}


/**
 * Tables for delivery descriptor parsing
 */
static const fe_code_rate_t fec_tab [16] = {
  FEC_AUTO, FEC_1_2, FEC_2_3, FEC_3_4,
  FEC_5_6, FEC_7_8, FEC_8_9, 
#if DVB_API_VERSION >= 5
  FEC_3_5,
#else
  FEC_NONE,
#endif
  FEC_4_5, 
#if DVB_API_VERSION >= 5
  FEC_9_10,
#else
  FEC_NONE,
#endif
  FEC_NONE, FEC_NONE,
  FEC_NONE, FEC_NONE, FEC_NONE, FEC_NONE
};


static const fe_modulation_t qam_tab [6] = {
	 QAM_AUTO, QAM_16, QAM_32, QAM_64, QAM_128, QAM_256
};

/**
 * Cable delivery descriptor
 */
static int
dvb_table_cable_delivery(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
			 uint16_t tsid)
{
  struct dvb_mux_conf dmc;
  int freq, symrate;

  if(!tdmi->tdmi_adapter->tda_autodiscovery)
    return -1;

  if(len < 11) {
    printf("Invalid CABLE DESCRIPTOR\n");
    return -1;
  }
  memset(&dmc, 0, sizeof(dmc));
  dmc.dmc_fe_params.inversion = INVERSION_AUTO;

  freq =
    bcdtoint(ptr[0]) * 1000000 + bcdtoint(ptr[1]) * 10000 + 
    bcdtoint(ptr[2]) * 100     + bcdtoint(ptr[3]);

  dmc.dmc_fe_params.frequency = freq * 100;

  symrate =
    bcdtoint(ptr[7]) * 100000 + bcdtoint(ptr[8]) * 1000 + 
    bcdtoint(ptr[9]) * 10     + (ptr[10] >> 4);

  dmc.dmc_fe_params.u.qam.symbol_rate = symrate * 100;


  if((ptr[6] & 0x0f) > 5)
    dmc.dmc_fe_params.u.qam.modulation = QAM_AUTO;
  else
    dmc.dmc_fe_params.u.qam.modulation = qam_tab[ptr[6] & 0x0f];

  dmc.dmc_fe_params.u.qam.fec_inner = fec_tab[ptr[10] & 0x07];

  dvb_mux_create(tdmi->tdmi_adapter, &dmc, tsid, NULL,
		 "automatic mux discovery", 1, NULL);
  return 0;
}

/**
 * Satellite delivery descriptor
 */
static int
dvb_table_sat_delivery(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		       uint16_t tsid)
{
  int freq, symrate;
  struct dvb_mux_conf dmc;

  if(!tdmi->tdmi_adapter->tda_autodiscovery)
    return -1;

  if(len < 11)
    return -1;

  memset(&dmc, 0, sizeof(dmc));
  dmc.dmc_fe_params.inversion = INVERSION_AUTO;

  freq =
    bcdtoint(ptr[0]) * 1000000 + bcdtoint(ptr[1]) * 10000 + 
    bcdtoint(ptr[2]) * 100     + bcdtoint(ptr[3]);
  dmc.dmc_fe_params.frequency = freq * 10;

  symrate =
    bcdtoint(ptr[7]) * 100000 + bcdtoint(ptr[8]) * 1000 + 
    bcdtoint(ptr[9]) * 10     + (ptr[10] >> 4);
  dmc.dmc_fe_params.u.qam.symbol_rate = symrate * 100;

  dmc.dmc_fe_params.u.qam.fec_inner = fec_tab[ptr[10] & 0x0f];

  dmc.dmc_polarisation = (ptr[6] >> 5) & 0x03;
   // Same satconf (lnb, switch, etc)
  dmc.dmc_satconf = tdmi->tdmi_conf.dmc_satconf;

#if DVB_API_VERSION >= 5
  int modulation = (ptr[6] & 0x03);

  if (modulation == 0x01)
    dmc.dmc_fe_modulation = QPSK;
  else if (modulation == 0x02)
    dmc.dmc_fe_modulation = PSK_8;
  else if (modulation == 0x03)
    dmc.dmc_fe_modulation = QAM_16;
  else 
    dmc.dmc_fe_modulation = 0;

  if (ptr[6] & 0x04) { 
    dmc.dmc_fe_delsys = SYS_DVBS2;

    switch ((ptr[6] >> 3) & 0x03) {
      case 0x00:
        dmc.dmc_fe_rolloff = ROLLOFF_35;
        break;
      case 0x01:
        dmc.dmc_fe_rolloff = ROLLOFF_25;
        break;
      case 0x02:
        dmc.dmc_fe_rolloff = ROLLOFF_20;
        break;
      default:
      case 0x03:
        dmc.dmc_fe_rolloff = ROLLOFF_AUTO;
        break;
    }
  }
  else {
    dmc.dmc_fe_delsys = SYS_DVBS;
    dmc.dmc_fe_rolloff = ROLLOFF_35;
  }
#endif
  dvb_mux_create(tdmi->tdmi_adapter, &dmc, tsid, NULL,
		 "automatic mux discovery", 1, NULL);
  
  return 0;
}



/**
 * NIT - Network Information Table
 */
static int
dvb_nit_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  uint8_t tag, tlen;
  int ntl;
  char networkname[256];
  uint16_t tsid;

  if(tableid != 0x40)
    return -1;

  if((ptr[2] & 1) == 0) {
    /* current_next_indicator == next, skip this */
    return -1;
  }

  ptr += 5;
  len -= 5;

  ntl = ((ptr[0] & 0xf) << 8) | ptr[1];
  ptr += 2;
  len -= 2;
  if(ntl > len)
    return -1;

  while(ntl > 2) {
    tag = *ptr++;
    tlen = *ptr++;
    len -= 2;
    ntl -= 2;

    switch(tag) {
    case DVB_DESC_NETWORK_NAME:
      if(dvb_get_string(networkname, sizeof(networkname), ptr, tlen))
	return -1;

      if(strcmp(tdmi->tdmi_network ?: "", networkname))
	dvb_mux_set_networkname(tdmi, networkname);
      break;
    }

    ptr += tlen;
    len -= tlen;
    ntl -= tlen;
  }

  if(len < 2)
    return -1;

  ntl =  ((ptr[0] & 0xf) << 8) | ptr[1];
  ptr += 2;
  len -= 2;

  if(len < ntl)
    return -1;

  while(len >= 6) {
    tsid = ( ptr[0]        << 8) | ptr[1];
    ntl =  ((ptr[4] & 0xf) << 8) | ptr[5];

    ptr += 6;
    len -= 6;
    if(ntl > len)
      break;

    while(ntl > 2) {
      tag = *ptr++;
      tlen = *ptr++;
      len -= 2;
      ntl -= 2;

      switch(tag) {
      case DVB_DESC_SAT:
	dvb_table_sat_delivery(tdmi, ptr, tlen, tsid);
	break;
      case DVB_DESC_CABLE:
	dvb_table_cable_delivery(tdmi, ptr, tlen, tsid);
	break;
      }

      ptr += tlen;
      len -= tlen;
      ntl -= tlen;
    }
  }
  return 0;
}


/**
 * VCT - ATSC Virtual Channel Table
 */
static int
atsc_vct_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		  uint8_t tableid, void *opaque)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  th_transport_t *t;
  int numch;
  char chname[256];
  uint8_t atsc_stype;
  uint8_t stype;
  uint16_t service_id;
  uint16_t transport_stream_id;
  int dlen, dl;
  uint8_t *dptr;

  if(tableid != 0xc8 && tableid != 0xc9)
    return -1; // Fail

  ptr += 5; // Skip generic header 
  len -= 5; 

  if(len < 2)
    return -1;

  numch = ptr[1];
  ptr += 2;
  len -= 2;

  for(; numch > 0 && len >= 32; ptr += 32 + dlen, len -= 32 + dlen, numch--) {
    
    dl = dlen = ((ptr[30] & 3) << 8) | ptr[31];

    if(dlen + 32 > len)
      return -1; // Corrupt table

    transport_stream_id = (ptr[22] << 8) | ptr[23];
    
    /* Search all muxes on adapter */
    LIST_FOREACH(tdmi, &tda->tda_muxes, tdmi_adapter_link)
      if(tdmi->tdmi_transport_stream_id == transport_stream_id)
	break;
    
    if(tdmi == NULL)
      continue;

    service_id = (ptr[24] << 8) | ptr[25];
    if((t = dvb_transport_find(tdmi, service_id, 0, NULL)) == NULL)
      continue;

    atsc_stype = ptr[27] & 0x3f;
    if(atsc_stype != 0x02) {
      /* Not ATSC TV */
      continue;
    }

    stype = ST_SDTV;
    atsc_utf16_to_utf8(ptr, 7, chname, sizeof(chname));

    dptr = ptr + 32;
    while(dl > 1) {
      //      int desclen = dptr[1];
      dl   -= dptr[1] + 2;
      dptr += dptr[1] + 2;
    }

    if(t->tht_servicetype != stype ||
       strcmp(t->tht_svcname ?: "", chname)) {

      t->tht_servicetype = stype;
      tvh_str_set(&t->tht_svcname, chname);
      
      t->tht_config_save(t);
    }
  }
  return 0;
}




/**
 * PMT - Program Mapping Table
 */
static int
dvb_pmt_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  th_transport_t *t = opaque;
  
  pthread_mutex_lock(&t->tht_stream_mutex);
  psi_parse_pmt(t, ptr, len, 1, 1);
  pthread_mutex_unlock(&t->tht_stream_mutex);
  return 0;
}


/**
 * Demux for default DVB tables that we want
 */
static void
dvb_table_add_default_dvb(th_dvb_mux_instance_t *tdmi)
{
  struct dmx_sct_filter_params *fp;

  /* Network Information Table */

  fp = dvb_fparams_alloc();
  fp->filter.filter[0] = 0x40;
  fp->filter.mask[0] = 0xff;
  tdt_add(tdmi, fp, dvb_nit_callback, NULL, "nit", 
	  TDT_QUICKREQ | TDT_CRC, 0x10, NULL);

  /* Service Descriptor Table */

  fp = dvb_fparams_alloc();
  fp->filter.filter[0] = 0x42;
  fp->filter.mask[0] = 0xff;
  tdt_add(tdmi, fp, dvb_sdt_callback, NULL, "sdt", 
	  TDT_QUICKREQ | TDT_CRC, 0x11, NULL);

  /* Event Information table */

  fp = dvb_fparams_alloc();
  tdt_add(tdmi, fp, dvb_eit_callback, NULL, "eit", 
	  TDT_CRC, 0x12, NULL);
}


/**
 * Demux for default ATSC tables that we want
 */
static void
dvb_table_add_default_atsc(th_dvb_mux_instance_t *tdmi)
{
  struct dmx_sct_filter_params *fp;
  int tableid;

  if(tdmi->tdmi_conf.dmc_fe_params.u.vsb.modulation == VSB_8) {
    tableid = 0xc8; // Terrestrial
  } else {
    tableid = 0xc9; // Cable
  }

  /* Virtual Channel Table */
  fp = dvb_fparams_alloc();
  fp->filter.filter[0] = tableid;
  fp->filter.mask[0] = 0xff;
  tdt_add(tdmi, fp, atsc_vct_callback, NULL, "vct",
	  TDT_QUICKREQ | TDT_CRC, 0x1ffb, NULL);
}




/**
 * Setup FD + demux for default tables that we want
 */
void
dvb_table_add_default(th_dvb_mux_instance_t *tdmi)
{
  struct dmx_sct_filter_params *fp;

  tdmi->tdmi_table_start = getclock_hires();

  /* Program Allocation Table */

  fp = dvb_fparams_alloc();
  fp->filter.filter[0] = 0x00;
  fp->filter.mask[0] = 0xff;
  tdt_add(tdmi, fp, dvb_pat_callback, NULL, "pat", 
	  TDT_QUICKREQ | TDT_CRC, 0, NULL);

  /* Conditional Access Table */

  fp = dvb_fparams_alloc();
  fp->filter.filter[0] = 0x1;
  fp->filter.mask[0] = 0xff;
  tdt_add(tdmi, fp, dvb_cat_callback, NULL, "cat", 
	  TDT_CRC, 1, NULL);


  switch(tdmi->tdmi_adapter->tda_type) {
  case FE_QPSK:
  case FE_OFDM:
  case FE_QAM:
    dvb_table_add_default_dvb(tdmi);
    break;

  case FE_ATSC:
    dvb_table_add_default_atsc(tdmi);


  }

}


/**
 * Setup FD + demux for a services PMT
 */
void
dvb_table_add_transport(th_dvb_mux_instance_t *tdmi, th_transport_t *t,
			int pmt_pid)
{
  struct dmx_sct_filter_params *fp;
  char pmtname[100];

  snprintf(pmtname, sizeof(pmtname), "PMT(%d), service:%d", 
	   pmt_pid, t->tht_dvb_service_id);
  fp = dvb_fparams_alloc();
  fp->filter.filter[0] = 0x02;
  fp->filter.mask[0] = 0xff;
  tdt_add(tdmi, fp, dvb_pmt_callback, t, pmtname, 
	  TDT_CRC | TDT_QUICKREQ, pmt_pid, NULL);
}


/**
 *
 */
void
dvb_table_flush_all(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  th_dvb_table_t *tdt;

  while((tdt = LIST_FIRST(&tdmi->tdmi_tables)) != NULL)
    dvb_tdt_destroy(tda, tdmi, tdt);
  
}
