/*
 * Copyright 2014 Nippon Telegraph and Telephone Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/* OpenFlow channel. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <execinfo.h>

#include "event.h"
#include "openflow.h"
#include "openflow13packet.h"
#include "lagopus/dpmgr.h"
#include "lagopus/flowdb.h"
#include "lagopus/ofp_handler.h"
#include "lagopus/meter.h"
#include "lagopus/bridge.h"
#include "lagopus/session.h"
#include "ofp_apis.h"
#include "ofp_instruction.h"
#include "ofp_role.h"
#include "channel_mgr.h"
#include "channel.h"

/* Start xid. */
#define START_XID 100

/* OFP max packet size. */
#define OFP_PBUF_SIZE (64*1024) /* 64KB */

/* Channel events. */
enum channel_event {
  Channel_Start             = 0,
  Channel_Stop              = 1,
  Channel_Expired           = 2,
  TCP_Connection_Open       = 3,
  TCP_Connection_Closed     = 4,
  TCP_Connection_Failed     = 5,
  OpenFlow_Hello_Received   = 6,
  OpenFlow_Message_Received = 7,
  CHANNEL_EVENT_MAX         = 8
};

/* Channel status. */
enum channel_status {
  Idle               = 0,
  Connect            = 1,
  HelloSent          = 2,
  Established        = 3,
  Disable            = 4,
  CHANNEL_STATUS_MAX = 5
};

struct multipart {
  struct ofp_header ofp_header;
  uint16_t multipart_type;
  uint16_t used;
  uint64_t len;
  struct pbuf_tailq multipart_list;
};

/* OpenFlow channel. */
struct channel {
  /* Datapath ID */
  uint64_t dpid;

  /* Wire protocol version. */
  uint8_t version;

  /* XID. */
  uint32_t xid;

  /* Role. */
  uint32_t role;

  /* controller info */
  char *hostname;
  struct addrunion controller;
  struct addrunion local_addr;
#define OFP_CTRL_PORT_DEFAULT 6633
  uint16_t port;
  uint16_t local_port;
  enum channel_protocol protocol;
  struct session *session;

  /* Channel status and event. */
  enum channel_status status;
  enum channel_event event;

  /* Event manager and events. */
  struct event_manager *em;
  struct event *read_ev;
  struct event *write_ev;
  struct event *start_timer;
  struct event *ping_ev;
  struct event *expire_ev;

  /* Start timer interval in seconds. */
#define CHANNEL_CONNECT_INTERVAL_DEFAULT   1
#define CHANNEL_CONNECT_INTERVAL_MAX      60
  int interval;

  /* Packet buffer. */
  struct pbuf *in;
  struct pbuf_list *out;
#define CHANNEL_SIMULTANEOUS_MULTIPART_MAX 16
  struct multipart multipart[CHANNEL_SIMULTANEOUS_MULTIPART_MAX];

  uint64_t channel_id;
  uint8_t  auxiliary_id;

  /* channel lock */
  lagopus_mutex_t lock;

  /* reference counter */
  int refs;

  /* channel set with same dpid */
  LIST_ENTRY(channel) dp_entry;

  struct ofp_async_config role_mask;
};

LIST_HEAD(channel_h, channel);

struct channel_list {
  struct channel_h channel_h;
  /* channel list lock */
  lagopus_mutex_t lock;

  /* generation id */
  bool generation_is_defined;
  uint64_t generation_id;

  /* channel id max */
  uint64_t channel_id_max;
};

/* Prototypes. */
static void channel_event_nolock(struct channel *channel,
                                 enum channel_event cevent);
static void channel_read(struct event *event);
static void channel_write(struct event *event);
static bool channel_packet_ready(struct pbuf *pbuf, struct ofp_header *sneak);
static void channel_event(struct channel *channel, enum channel_event cevent);
static void channel_lock(struct channel *channel);
static void channel_unlock(struct channel *channel);
static void channel_do_expire(struct event *event);


/* Non-blocking connect result check. */
/* Assume channel locked. */
static int
channel_connect_check(struct channel *channel) {
  lagopus_result_t ret;

  /* Cancel read and write event. */
  event_cancel(&channel->read_ev);
  event_cancel(&channel->write_ev);

  if (channel->status == Disable) {
    channel_event_nolock(channel, Channel_Expired);
    return 0;
  }

  /* Check socket status. */
  ret = session_connect_check(channel->session);

  /* If getsockopt fails, it is fatal error. */
  if (ret == LAGOPUS_RESULT_OK) {
    channel_event_nolock(channel, TCP_Connection_Open);
  } else if (ret == LAGOPUS_RESULT_EINPROGRESS) {
    channel_event_nolock(channel, Channel_Start);
  } else {
    channel_event_nolock(channel, TCP_Connection_Failed);
  }

  return 0;
}

/* Channel read on. */
/* Assume channel locked. */
static void
channel_read_on(struct channel *channel) {
  if (channel->read_ev == NULL) {
    channel->read_ev = event_register_read(channel->em,
                                           session_sockfd_get(channel->session),
                                           channel_read, channel);
  }
}

/* Channel write on. */
/* Assume channel locked. */
static void
channel_write_on(struct channel *channel) {
  if (channel->write_ev == NULL) {
    channel->write_ev = event_register_write(channel->em,
                        session_sockfd_get(channel->session),
                        channel_write, channel);
  }
}

/* Assume channel locked. */
static bool
channelq_data_create(struct channelq_data **retptr,
                     struct channel *channel) {
  struct ofp_header sneak;
  struct channelq_data *cdata = NULL;

  if (channel_packet_ready(channel->in, &sneak) == true) {
    /* create channelq_data */
    cdata = (struct channelq_data *)malloc(sizeof(*cdata));
    if (cdata == NULL) {
      channel_unlock(channel);
      return false;
    }

    cdata->pbuf = channel->in;
    pbuf_plen_set(cdata->pbuf, sneak.length);
    channel->in = pbuf_alloc(OFP_PBUF_SIZE);
    if (channel->in == NULL) {
      channel->in = cdata->pbuf;
      free(cdata);
      return false;
    }

    pbuf_copy_unread_data(channel->in, cdata->pbuf,
                          pbuf_plen_get(cdata->pbuf));

    cdata->channel = channel;
    *retptr = cdata;

    return true;
  }
  return false;
}

/* Return XID with incrementing it. */
uint32_t
channel_xid_get_nolock(struct channel *channel) {
  if (channel->xid >= UINT32_MAX) {
    channel->xid = START_XID;
  }
  return channel->xid++;
}

uint32_t
channel_xid_get(struct channel *channel) {
  uint32_t xid;

  channel_lock(channel);
  xid = channel_xid_get_nolock(channel);
  channel_unlock(channel);

  return xid;
}

/* Socket read function. */
static void
channel_read(struct event *event) {
  lagopus_result_t rc = LAGOPUS_RESULT_ANY_FAILURES;
  channelq_t *channelq;
  struct channelq_data *cdata;
  struct channel *channel;
  ssize_t nbytes;

  /* Channel read event clear. */
  channel = event_get_arg(event);
  channel_lock(channel);
  channel->read_ev = NULL;

  /* Non-blocking connect check. */
  if (channel->status == Connect || channel->status == Disable) {
    channel_connect_check(channel);
    goto done;
  }

  /* Turn on read. */
  lagopus_msg_debug(10, "read_on\n");
  channel_read_on(channel);

  /* Read packet into packet buffer. */
  nbytes = pbuf_session_read(channel->in, channel->session);
  /* Read error. */
  if (nbytes < 0) {
    /* EAGAIN is not an error.  Simply ignore it. */
    if (errno == EAGAIN) {
      goto done;
    }
    channel_event_nolock(channel, TCP_Connection_Failed);
    goto done;
  }
  /* Read socket is closed. */
  if (nbytes == 0) {
    channel_event_nolock(channel, TCP_Connection_Closed);
    goto done;
  }

  /* get channelq */
  rc = ofp_handler_get_channelq(&channelq);
  if (rc != LAGOPUS_RESULT_OK) {
    goto done;
  }

  while (channelq_data_create(&cdata, channel) == true) {
    /* put channel to ofp_handler */
    channel_unlock(channel);
    channel_refs_get(channel);
    lagopus_bbq_put(channelq, &cdata, struct channel *, -1);
    channel_lock(channel);
  }
done:
  channel_unlock(channel);
}

void
channelq_data_destroy(struct channelq_data *cdata) {
  if (cdata != NULL) {
    channel_refs_put(cdata->channel);
    pbuf_free(cdata->pbuf);
    free(cdata);
  }
}

/* Is packet ready to parse. */
static bool
channel_packet_ready(struct pbuf *pbuf, struct ofp_header *sneak) {
  lagopus_result_t ret = LAGOPUS_RESULT_ANY_FAILURES;
  /* Set readable size to pbuf. */
  pbuf_plen_set(pbuf, pbuf_readable_size(pbuf));
  /* Sneak preview of the header. */
  ret = ofp_header_decode_sneak(pbuf, sneak);
  if (ret != LAGOPUS_RESULT_OK) {
    return false;
  }
  /* Payload length check. */
  if (pbuf_plen_check(pbuf, sneak->length) !=
      LAGOPUS_RESULT_OK) {
    return false;
  }
  /* Success. */
  return true;
}


/* Socket write function. */
static void
channel_write(struct event *event) {
  struct channel *channel;
  ssize_t nbytes;

  /* Get channel. */
  channel = event_get_arg(event);

  channel_lock(channel);
  /* Clear event pointer. */
  channel->write_ev = NULL;

  /* Connect status. */
  if (channel->status == Connect || channel->status == Disable) {
    channel_connect_check(channel);
    goto done;
  }

  /* Write packet to the socket. */
  nbytes = pbuf_list_session_write(channel->out, channel->session);

  /* Write error. */
  if (nbytes < 0) {
    /* EAGAIN is not an error.  Simply ignore it. */
    if (errno == EAGAIN) {
      goto done;
    }
    lagopus_msg_warning("FAILED : channel_write TCP_Connection_Failed\n");
    channel_event_nolock(channel, TCP_Connection_Failed);
    goto done;
  }

  /* Write socket is closed. */
  if (nbytes == 0) {
    lagopus_msg_info("channel_write TCP_Connection_Closed\n");
    channel_event_nolock(channel, TCP_Connection_Closed);
    goto done;
  }

  /* If there is packet to be written, turn of write. */
  if (pbuf_list_first(channel->out) != NULL) {
    channel_write_on(channel);
  }

done:
  channel_unlock(channel);
  return;
}

void
channel_send_packet_nolock(struct channel *channel, struct pbuf *pbuf) {
  if (channel != NULL) {
    pbuf_list_add(channel->out, pbuf);
    channel_write_on(channel);
  }
}

void
channel_send_packet(struct channel *channel, struct pbuf *pbuf) {
  channel_lock(channel);
  channel_send_packet_nolock(channel, pbuf);
  channel_unlock(channel);
}


lagopus_result_t
channel_send_packet_list(struct channel *channel,
                         struct pbuf_list *pbuf_list) {
  lagopus_result_t res = LAGOPUS_RESULT_ANY_FAILURES;
  struct pbuf *pbuf = NULL;

  if (channel != NULL || pbuf_list != NULL) {
    channel_lock(channel);
    while ((pbuf = TAILQ_FIRST(&pbuf_list->tailq)) != NULL) {
      TAILQ_REMOVE(&pbuf_list->tailq, pbuf, entry);
      channel_send_packet_nolock(channel, pbuf);
    }
    channel_unlock(channel);
    res = LAGOPUS_RESULT_OK;
  } else {
    res = LAGOPUS_RESULT_INVALID_ARGS;
  }

  return res;
}

static void
channel_timer(struct event *event) {
  struct channel *channel = event_get_arg(event);

  channel_lock(channel);
  event_cancel(&channel->start_timer);
  channel_event_nolock(channel, Channel_Start);
  channel_unlock(channel);
}

/* Update channel timer according to channel status. */
static void
channel_update(struct channel *channel, enum channel_status next) {
  if (channel->status != next) {
    channel->status = next;
  }

  switch (channel->status) {
    case Idle:
      if (channel->start_timer == NULL)
        channel->start_timer = event_register_timer(channel->em,
                               channel_timer,
                               channel, channel->interval);
      break;
    case Connect:
      break;
    case HelloSent:
      event_cancel(&channel->start_timer);
      break;
    case Established:
      event_cancel(&channel->start_timer);
      break;
    default:
      break;
  }
}

/* for inet_aton() will be removed in near future. */
#include <arpa/inet.h>

/* Initiate connection to OpenFlow controller. */
static int
channel_start(struct channel *channel) {
  lagopus_result_t ret;

  if (channel->session == NULL) {
    lagopus_msg_warning("session is not set, channel[%p]\n", channel);
    return -1;
  }

  /* Socket must be closed. */
  if (session_is_alive(channel->session)) {
    return -1;
  }

  /* Logging. */
  lagopus_msg_info("Connecting to OpenFlow controller %s:%d\n",
                   channel->hostname, channel->port);

  ret = session_connect(channel->session, &channel->controller, channel->port,
                        &channel->local_addr, channel->local_port);
  if (ret < 0 && errno != EINPROGRESS) {
    lagopus_msg_warning("connect error %s\n", strerror(errno));
    return -1;
  }

  /* Increment channel reference count. */
  channel->refs++;

  /* Register read and write event. */
  channel_read_on(channel);
  channel_write_on(channel);

  /* Generate xid. */
  channel->xid = START_XID;

  /* Success. */
  return 0;
}

static int
channel_stop(struct channel *channel) {
  lagopus_msg_info("channel_stop() is called\n");

  if (session_is_alive(channel->session)) {
    lagopus_msg_info("closing the socket %d\n",
                     session_sockfd_get(channel->session));
    session_close(channel->session);
  }

  if (channel_mgr_has_alive_channel_by_dpid(channel->dpid)
      == false) {
    lagopus_result_t ret;
    enum fail_mode sm;

    ret = dpmgr_bridge_fail_mode_get(channel->dpid, &sm);
    if (ret == LAGOPUS_RESULT_OK) {
      if (sm == FAIL_SECURE_MODE) {
        ret = dpmgr_switch_mode_set(channel->dpid, SWITCH_MODE_SECURE);
        lagopus_msg_info("switch to SWITCH_MODE_SECURE\n");
      } else {
        ret = dpmgr_switch_mode_set(channel->dpid, SWITCH_MODE_STANDALONE);
        lagopus_msg_info("switch to SWITCH_MODE_STANDALONE\n");
      }
      if (ret != LAGOPUS_RESULT_OK) {
        lagopus_perror(ret);
      }
    } else {
      lagopus_perror(ret);
    }
    /* TODO: close all auxiliary connections if existed */
  }

  event_cancel(&channel->read_ev);
  event_cancel(&channel->write_ev);
  event_cancel(&channel->start_timer);
  event_cancel(&channel->ping_ev);
  event_cancel(&channel->expire_ev);

  return 0;
}

static void
channel_do_expire(struct event *event) {
  struct channel *channel;
  channel = event_get_arg(event);

  channel_lock(channel);
  channel->expire_ev = NULL;
  channel_event_nolock(channel, Channel_Expired);
  channel_unlock(channel);
}

static int
channel_expire(struct channel *channel) {
  if (channel->refs == 1 || channel->refs == 0) {
    channel_stop(channel);
    channel->refs = 0;
  } else {
    /* fake channel event for rescheduling do_expire. */
    channel->event = Channel_Start;
  }

  return 0;
}

/* No operation. */
static int
channel_ignore(struct channel *channel) {
  /* Nothing to do. */
  lagopus_msg_info("channel_ignore is called %p\n", channel);
  return 0;
}

static int
channel_connect_success(struct channel *channel) {
  int rv = 0;
  /* Reset connect interval. */

  channel->interval = CHANNEL_CONNECT_INTERVAL_DEFAULT;

  /* Turn on read. */
  lagopus_msg_debug(10, "read_on\n");
  channel_read_on(channel);

  /* Send Hello. */
  ofp_hello_send(channel);

  /* Success. */
  return rv;
}

static int
channel_connect_fail(struct channel *channel) {
  lagopus_msg_info("connect fail\n");
  /* Double connect interval. */
  channel->interval *= 2;
  if (channel->interval >= CHANNEL_CONNECT_INTERVAL_MAX) {
    channel->interval = CHANNEL_CONNECT_INTERVAL_MAX;
  }
  channel_stop(channel);

  return 0;
}

/* Debug ping timer. */
static void
ping_timer(struct event *event) {
  struct channel *channel;

  channel = event_get_arg(event);
  channel_lock(channel);
  channel->ping_ev = NULL;
  channel_unlock(channel);

  ofp_echo_request_send(channel);

  channel_lock(channel);
  channel->ping_ev = event_register_timer(channel->em, ping_timer,
                                          channel, 1);
  channel_unlock(channel);
}

static int
channel_hello_confirm(struct channel *channel) {
  lagopus_result_t ret;
  lagopus_msg_debug(10, "read_on\n");
  channel_read_on(channel);

  /* Debug ping event. */
  lagopus_msg_info("channel_hello_confirm in\n");
  ret = dpmgr_switch_mode_set(channel->dpid, SWITCH_MODE_OPENFLOW);
  if (ret != LAGOPUS_RESULT_OK) {
    lagopus_perror(ret);
  }
  channel->ping_ev = event_register_timer(channel->em, ping_timer, channel, 1);
  return 0;
}

static int
channel_process(struct channel *channel) {
  lagopus_msg_info("channel_process() is called %p\n", channel);
  return 0;
}

/* Finite State Machine. */
struct {
  int (*func)(struct channel *);
  int next;
} FSM[CHANNEL_STATUS_MAX][CHANNEL_EVENT_MAX] = {
  {
    /* Idle state */
    {channel_start,           Connect},      /* Channel_Start */
    {channel_stop,            Idle},         /* Channel_Stop */
    {channel_expire,          Disable},      /* Channel_Expired */
    {channel_stop,            Idle},         /* TCP_Connection_Open */
    {channel_stop,            Idle},         /* TCP_Connection_Closed */
    {channel_stop,            Idle},         /* TCP_Connection_Failed */
    {channel_stop,            Idle},         /* OpenFlow_Hello_Received */
    {channel_stop,            Idle},         /* OpenFlow_Message_Received */
  },
  {
    /* Connect */
    {channel_connect_check,   Connect},	     /* Channel_Start */
    {channel_stop,            Idle},	       /* Channel_Stop */
    {channel_expire,          Disable},      /* Channel_Expired */
    {channel_connect_success, HelloSent},    /* TCP_Connection_Open */
    {channel_stop,            Idle},         /* TCP_Connection_Closed */
    {channel_connect_fail,    Idle},         /* TCP_Connection_Failed */
    {channel_stop,            Idle},         /* OpenFlow_Hello_Received */
    {channel_stop,            Idle},         /* OpenFlow_Message_Received */
  },
  {
    /* HelloSent */
    {channel_ignore,          HelloSent},    /* Channel_Start */
    {channel_stop,            Idle},         /* Channel_Stop */
    {channel_expire,          Disable},      /* Channel_Expire */
    {channel_stop,            Idle},         /* TCP_Connection_Open */
    {channel_stop,            Idle},         /* TCP_Connection_Closed */
    {channel_stop,            Idle},         /* TCP_Connection_Failed */
    {channel_hello_confirm,   Established},  /* OpenFlow_Hello_Received */
    {channel_stop,            Idle},         /* OpenFlow_Message_Received */
  },
  {
    /* Established */
    {channel_ignore,          Established},  /* Channel_Start */
    {channel_stop,            Idle},         /* Channel_Stop */
    {channel_expire,          Disable},      /* Channel_Expired */
    {channel_stop,            Idle},         /* TCP_Connection_Open */
    {channel_stop,            Idle},         /* TCP_Connection_Closed */
    {channel_stop,            Idle},         /* TCP_Connection_Failed */
    {channel_process,         Established},  /* OpenFlow_Hello_Received */
    {channel_process,         Established},  /* OpenFlow_Message_Received */
  },
  {
    /* Disable */
    {channel_expire,          Disable},      /* Channel_Start */
    {channel_expire,          Disable},      /* Channel_Stop */
    {channel_expire,          Disable},      /* Channel_Expired */
    {channel_expire,          Disable},      /* TCP_Connection_Open */
    {channel_expire,          Disable},      /* TCP_Connection_Closed */
    {channel_expire,          Disable},      /* TCP_Connection_Failed */
    {channel_expire,          Disable},      /* OpenFlow_Hello_Received */
    {channel_expire,          Disable},      /* OpenFlow_Message_Received */
  },
};

/* Execute FSM event of the channel. */
void
channel_event_nolock(struct channel *channel, enum channel_event cevent) {
  int ret;
  enum channel_status next;

  /* Valication. */
  if (channel->status >= CHANNEL_STATUS_MAX) {
    lagopus_msg_fatal("Invalid channel status\n");
    exit(1);
  }
  if (cevent >= CHANNEL_EVENT_MAX) {
    lagopus_msg_fatal("Invalid channel event\n");
    exit(1);
  }

  channel->event = cevent;

  /* Next status. */
  next = FSM[channel->status][cevent].next;

  /* Execute function. */
  ret = (*(FSM[channel->status][cevent].func))(channel);
  if (ret < 0) {
    return;
  }

  /* Status and timer update. */
  channel_update(channel, next);
}

static void
channel_event(struct channel *channel, enum channel_event cevent) {
  channel_lock(channel);
  channel_event_nolock(channel, cevent);
  channel_unlock(channel);
}

/* Assume channel locked. */
static lagopus_result_t
session_set(struct channel *channel) {
  if (session_is_alive(channel->session) == true) {
    lagopus_msg_info("session is alive.\n");
    return LAGOPUS_RESULT_ALREADY_EXISTS;
  } else if (channel->session != NULL) {
    session_destroy(channel->session);
  }

  switch (channel->protocol) {
    case PROTOCOL_TCP:
      channel->session = session_create(SESSION_TCP);
      break;
    case PROTOCOL_TLS:
      channel->session = session_create(SESSION_TLS);
      break;
    case PROTOCOL_TCP6:
      channel->session = session_create(SESSION_TCP6);
      break;
    case PROTOCOL_TLS6:
      channel->session = session_create(SESSION_TLS6);
      break;
    default:
      lagopus_msg_warning("unknown protocol %d\n", channel->protocol);
      break;
  }
  if (channel->session == NULL) {
    return LAGOPUS_RESULT_ANY_FAILURES;
  } else {
    return LAGOPUS_RESULT_OK;
  }
}

/* Multipart object init. */
static void
multipart_init(struct multipart *m) {
  TAILQ_INIT(&m->multipart_list);
}

/* Channel allocation. */
static struct channel *
channel_alloc_internal(struct addrunion *controller,
                       struct event_manager *em,
                       uint64_t dpid) {
  int i;
  lagopus_result_t ret;
  struct channel *channel;

  /* Allocate a new channel. */
  channel = (struct channel *)calloc(1, sizeof(struct channel));
  if (channel == NULL) {
    return NULL;
  }

  /* Set dpid. */
  channel->dpid = dpid;

  /* Initial socket. */
  channel->controller = *controller;
  channel->port = OFP_CTRL_PORT_DEFAULT;
  channel->local_port = 0;

  switch (addrunion_af_get(controller)) {
    case AF_INET:
      addrunion_ipv4_set(&channel->local_addr, "0.0.0.0");
      channel->protocol = PROTOCOL_TCP;
      //  channel->protocol = PROTOCOL_TLS;
      break;
    case AF_INET6:
      addrunion_ipv6_set(&channel->local_addr, "::0");
      channel->protocol = PROTOCOL_TCP6;
      break;
    default:
      lagopus_msg_warning("unknown address family %d\n",
                          addrunion_af_get(controller));
      free(channel);
      return NULL;
  }

  ret = session_set(channel);
  if (ret != LAGOPUS_RESULT_OK) {
    lagopus_msg_warning("session is no created\n");
    free(channel);
    return NULL;
  }

  /* Wire protocol version. */
  channel->version = 0x04;

  /* Default role is equal. */
  channel->role = OFPCR_ROLE_EQUAL;

  /* Input and output buffer. */
  channel->in = pbuf_alloc(OFP_PBUF_SIZE);
  if (channel->in == NULL) {
    free(channel);
    return NULL;
  }
  channel->out = pbuf_list_alloc();
  if (channel->out == NULL) {
    pbuf_free(channel->in);
    free(channel);
    return NULL;
  }

  /* Set channel status. */
  channel->em = em;
  channel->status = Disable;
  channel->interval = CHANNEL_CONNECT_INTERVAL_DEFAULT;
  /* Init multipart objects. */
  for (i = 0; i < CHANNEL_SIMULTANEOUS_MULTIPART_MAX; i++) {
    multipart_init(&channel->multipart[i]);
  }

  ret = lagopus_mutex_create(&channel->lock);
  if (ret != LAGOPUS_RESULT_OK) {
    lagopus_perror(ret);
    channel_free(channel);
    return NULL;
  }

  channel->refs = 0;
  channel->dp_entry.le_prev = NULL;

  ofp_role_channel_mask_init(&channel->role_mask);

  /* Initiate FSM. */
  channel_update(channel, Disable);

  return channel;
}

struct channel *
channel_alloc(struct addrunion *controller, struct event_manager *em,
              uint64_t dpid) {
  return channel_alloc_internal(controller, em, dpid);
}

struct channel *
channel_alloc_ip4addr(const char *ipaddr, const char *port,
                      struct event_manager *em, uint64_t dpid) {
  struct addrunion addr;
  struct channel *channel;

  addrunion_ipv4_set(&addr, ipaddr);

  channel = channel_alloc_internal(&addr, em, dpid);
  if (channel != NULL) {
    channel->port = (uint16_t) atoi(port);
  }

  return channel;
}

/* Multipart object init. */
static void
multipart_free(struct multipart *m) {
  struct pbuf *p;

  if (m->used == 1) {
    lagopus_msg_warning("unfinished multiparts existed."
                        " xid %d, multipart_type %d\n",
                        m->ofp_header.xid, m->multipart_type);
    while ((p = TAILQ_FIRST(&m->multipart_list)) != NULL) {
      TAILQ_REMOVE(&m->multipart_list, p, entry);
      pbuf_reset(p);
      pbuf_free(p);
    }
  }
}

void
channel_refs_get(struct channel *channel) {
  channel_lock(channel);
  channel->refs++;
  channel_unlock(channel);
}

void
channel_refs_put(struct channel *channel) {
  channel_lock(channel);
  assert(channel->refs > 0);
  channel->refs--;
  channel_unlock(channel);
}

/* Free channel. */
lagopus_result_t
channel_free(struct channel *channel) {
  if (channel != NULL) {
    int i;
    lagopus_result_t ret = LAGOPUS_RESULT_OK;

    if (channel->lock != NULL) {
      lagopus_mutex_lock(&channel->lock);
    }

    if (channel->refs > 0) {
      ret = LAGOPUS_RESULT_BUSY;
      channel_unlock(channel);
      goto done;
    }

    assert(channel->refs == 0);
    event_cancel(&channel->read_ev);
    event_cancel(&channel->write_ev);
    event_cancel(&channel->start_timer);
    event_cancel(&channel->ping_ev);
    event_cancel(&channel->expire_ev);

    session_destroy(channel->session);
    channel->session = NULL;
    pbuf_free(channel->in);
    pbuf_list_free(channel->out);

    /* Free multipart objects. */
    for (i = 0; i < CHANNEL_SIMULTANEOUS_MULTIPART_MAX; i++) {
      multipart_free(&channel->multipart[i]);
    }

    if (channel->dp_entry.le_prev != NULL) {
      LIST_REMOVE(channel, dp_entry);
    }
    channel_unlock(channel);

    lagopus_mutex_destroy(&channel->lock);
    free(channel);

  done:
    return ret;
  } else {
    return LAGOPUS_RESULT_INVALID_ARGS;
  }
}

/* Channel getter/seter. */
uint8_t
channel_version_get_nolock(struct channel *channel) {
  uint8_t ver;
  ver = channel->version;

  return ver;
}

uint8_t
channel_version_get(struct channel *channel) {
  uint8_t ver;
  channel_lock(channel);
  ver = channel_version_get_nolock(channel);
  channel_unlock(channel);

  return ver;
}

void
channel_version_set_nolock(struct channel *channel, uint8_t version) {
  channel->version = version;
}

void
channel_version_set(struct channel *channel, uint8_t version) {
  channel_lock(channel);
  channel_version_set_nolock(channel, version);
  channel_unlock(channel);
}

struct session *
channel_session_get(struct channel *channel) {
  struct session *s;

  channel_lock(channel);
  s = channel->session;
  channel_unlock(channel);

  return s;
}

void
channel_session_set(struct channel *channel, struct session *session) {
  channel->session = session;
}

void
channel_xid_set(struct channel *channel, uint32_t xid) {
  channel_lock(channel);
  channel->xid = xid;
  channel_unlock(channel);
}

uint32_t
channel_role_get(struct channel *channel) {
  uint32_t ret;

  channel_lock(channel);
  ret = channel->role;
  channel_unlock(channel);

  return ret;
}

void
channel_role_set(struct channel *channel, uint32_t role) {
  channel->role = role;
}

uint64_t
channel_dpid_get_nolock(struct channel *channel) {
  return channel->dpid;
}

uint64_t
channel_dpid_get(struct channel *channel) {
  uint64_t dpid;

  channel_lock(channel);
  dpid = channel_dpid_get_nolock(channel);
  channel_unlock(channel);

  return dpid;
}

uint64_t
channel_id_get(struct channel *channel) {
  uint64_t ret;

  channel_lock(channel);
  ret = channel->channel_id;
  channel_unlock(channel);

  return ret;
}

void
channel_id_set(struct channel *channel, uint64_t channel_id) {
  channel_lock(channel);
  channel->channel_id = channel_id;
  channel_unlock(channel);
}

void
channel_lock(struct channel *channel) {
  lagopus_mutex_lock(&channel->lock);
}

void
channel_unlock(struct channel *channel) {
  lagopus_mutex_unlock(&channel->lock);
}

struct pbuf *
channel_pbuf_list_get_nolock(struct channel *channel, size_t size) {
  struct pbuf *pbuf;
  pbuf = pbuf_list_get(channel->out, size);

  return pbuf;
}

struct pbuf *
channel_pbuf_list_get(struct channel *channel, size_t size) {
  struct pbuf *pbuf;

  channel_lock(channel);
  pbuf = channel_pbuf_list_get_nolock(channel, size);
  channel_unlock(channel);

  return pbuf;
}

void
channel_pbuf_list_unget_nolock(struct channel *channel, struct pbuf *pbuf) {
  pbuf_list_unget(channel->out, pbuf);
}

void
channel_pbuf_list_unget(struct channel *channel, struct pbuf *pbuf) {
  lagopus_mutex_lock(&channel->lock);
  channel_pbuf_list_unget_nolock(channel, pbuf);
  lagopus_mutex_unlock(&channel->lock);
}

lagopus_result_t
channel_addr_get(struct channel *channel, struct addrunion *addrunion) {
  channel_lock(channel);
  *addrunion = channel->controller;
  channel_unlock(channel);
  return LAGOPUS_RESULT_OK;
}

lagopus_result_t
channel_protocol_set(struct channel *channel,
                     enum channel_protocol protocol) {
  lagopus_result_t ret = LAGOPUS_RESULT_BUSY;

  channel_lock(channel);
  if (session_is_alive(channel->session) == false) {
    if (channel->protocol != protocol) {
      channel->protocol = protocol;
      ret = session_set(channel);
    } else {
      ret = LAGOPUS_RESULT_OK;
    }
  }
  channel_unlock(channel);
  return ret;
}

lagopus_result_t
channel_protocol_unset(struct channel *channel) {
  lagopus_result_t ret = LAGOPUS_RESULT_BUSY;

  channel_lock(channel);
  if (session_is_alive(channel->session) == false) {
    if (channel->protocol != PROTOCOL_TCP) {
      channel->protocol = PROTOCOL_TCP;
      session_set(channel);
    }
    ret = LAGOPUS_RESULT_OK;
  }
  channel_unlock(channel);
  return ret;
}

lagopus_result_t
channel_protocol_get(struct channel *channel,
                     enum channel_protocol *protocol) {
  channel_lock(channel);
  *protocol = channel->protocol;
  channel_unlock(channel);
  return LAGOPUS_RESULT_OK;
}

lagopus_result_t
channel_port_set(struct channel *channel, uint16_t port) {
  lagopus_result_t ret = LAGOPUS_RESULT_BUSY;

  channel_lock(channel);
  if (session_is_alive(channel->session) == false) {
    channel->port = port;
    ret = LAGOPUS_RESULT_OK;
  }
  channel_unlock(channel);
  return ret;
}

lagopus_result_t
channel_port_unset(struct channel *channel) {
  lagopus_result_t ret = LAGOPUS_RESULT_BUSY;

  channel_lock(channel);
  if (session_is_alive(channel->session) == false) {
    channel->port = OFP_CTRL_PORT_DEFAULT;
    ret = LAGOPUS_RESULT_OK;
  }
  channel_unlock(channel);
  return ret;
}

lagopus_result_t
channel_port_get(struct channel *channel, uint16_t *port) {
  channel_lock(channel);
  *port= channel->port;
  channel_unlock(channel);
  return LAGOPUS_RESULT_OK;
}

lagopus_result_t
channel_local_port_set(struct channel *channel, uint16_t port) {
  lagopus_result_t ret = LAGOPUS_RESULT_BUSY;

  channel_lock(channel);
  if (session_is_alive(channel->session) == false) {
    channel->local_port = port;
    ret = LAGOPUS_RESULT_OK;
  }
  channel_unlock(channel);
  return ret;
}

lagopus_result_t
channel_local_port_unset(struct channel *channel) {
  lagopus_result_t ret = LAGOPUS_RESULT_BUSY;

  channel_lock(channel);
  if (session_is_alive(channel->session) == false) {
    channel->local_port = 0;
    ret = LAGOPUS_RESULT_OK;
  }
  channel_unlock(channel);
  return ret;
}

lagopus_result_t
channel_local_port_get(struct channel *channel, uint16_t *port) {
  channel_lock(channel);
  *port= channel->local_port;
  channel_unlock(channel);
  return LAGOPUS_RESULT_OK;
}

lagopus_result_t
channel_local_addr_set(struct channel *channel, struct addrunion *addrunion) {
  lagopus_result_t ret = LAGOPUS_RESULT_BUSY;

  channel_lock(channel);
  if (session_is_alive(channel->session) == false) {
    channel->local_addr = *addrunion;
    ret = LAGOPUS_RESULT_OK;
  }
  channel_unlock(channel);
  return ret;
}

lagopus_result_t
channel_local_addr_unset(struct channel *channel) {
  lagopus_result_t ret = LAGOPUS_RESULT_BUSY;

  channel_lock(channel);
  if (session_is_alive(channel->session) == false) {
    if (channel->protocol == PROTOCOL_TCP || channel->protocol == PROTOCOL_TLS) {
      addrunion_ipv4_set(&channel->local_addr, "0.0.0.0");
      ret = LAGOPUS_RESULT_OK;
    } else if (channel->protocol == PROTOCOL_TCP6 ||
               channel->protocol == PROTOCOL_TLS6) {
      addrunion_ipv6_set(&channel->local_addr, "::0");
      ret = LAGOPUS_RESULT_OK;
    } else {
      ret = LAGOPUS_RESULT_ANY_FAILURES;
    }
  }
  channel_unlock(channel);
  return ret;
}

lagopus_result_t
channel_local_addr_get(struct channel *channel, struct addrunion *addrunion) {
  channel_lock(channel);
  *addrunion = channel->local_addr;
  channel_unlock(channel);
  return LAGOPUS_RESULT_OK;
}

lagopus_result_t
channel_features_get(struct channel *channel,
                     struct ofp_switch_features *features) {
  return dpmgr_bridge_ofp_features_get(channel->dpid, features);
}

int
channel_multipart_used_count_get(struct channel *channel) {
  int i, cnt = 0;
  channel_lock(channel);
  for (i = 0; i < CHANNEL_SIMULTANEOUS_MULTIPART_MAX; i++) {
    if (channel->multipart[i].used == 1) {
      cnt++;
    }
  }
  channel_unlock(channel);

  return cnt;
}

lagopus_result_t
channel_multipart_put(struct channel *channel, struct pbuf *pbuf,
                      struct ofp_header *xid_header, uint16_t mtype) {
  int i;
  struct multipart *m = NULL;
  lagopus_result_t ret;

  channel_lock(channel);
  for (i = 0; i < CHANNEL_SIMULTANEOUS_MULTIPART_MAX; i++) {
    if (channel->multipart[i].used == 0) {
      m = &channel->multipart[i];
    } else if (channel->multipart[i].ofp_header.xid
               == xid_header->xid) {
      if (channel->multipart[i].multipart_type != mtype) {
        /* but multipart type is not matched. */
        ret = LAGOPUS_RESULT_OFP_ERROR;
        break;
      }
      /* found */
      channel->multipart[i].len += pbuf_plen_get(pbuf);
      pbuf_get(pbuf);
      TAILQ_INSERT_TAIL(&channel->multipart[i].multipart_list, pbuf, entry);
      ret = LAGOPUS_RESULT_OK;
      break;
    }
  }

  if (m == NULL && i == CHANNEL_SIMULTANEOUS_MULTIPART_MAX) {
    ret = LAGOPUS_RESULT_NO_MEMORY;
  } else if (m != NULL && i == CHANNEL_SIMULTANEOUS_MULTIPART_MAX) {
    m->ofp_header = *xid_header;
    m->multipart_type = mtype;
    m->len = pbuf_plen_get(pbuf);
    m->used = 1;
    pbuf_get(pbuf);
    TAILQ_INSERT_TAIL(&m->multipart_list, pbuf, entry);
    ret = LAGOPUS_RESULT_OK;
  }

  channel_unlock(channel);
  return ret;
}

static void
reset(struct multipart *m) {
  m->used = 0;
}


lagopus_result_t
channel_multipart_get(struct channel *channel, struct pbuf **pbuf,
                      struct ofp_header *xid_header, uint16_t mtype) {
  int i;
  struct pbuf *p;
  lagopus_result_t ret = LAGOPUS_RESULT_ANY_FAILURES;

  channel_lock(channel);
  for (i = 0; i < CHANNEL_SIMULTANEOUS_MULTIPART_MAX; i++) {
    if (channel->multipart[i].ofp_header.xid == xid_header->xid
        && channel->multipart[i].multipart_type == mtype) {
      /* found */
      ret = LAGOPUS_RESULT_OK;
      break;
    }
  }
  if (i == CHANNEL_SIMULTANEOUS_MULTIPART_MAX) {
    ret = LAGOPUS_RESULT_NOT_FOUND;
  } else {
    *pbuf = pbuf_alloc(channel->multipart[i].len);
    if (*pbuf == NULL) {
      ret = LAGOPUS_RESULT_NO_MEMORY;
    } else {
      /* set return val. */
      ret = LAGOPUS_RESULT_OK;
      while ((p = TAILQ_FIRST(&channel->multipart[i].multipart_list))
             != NULL) {
        TAILQ_REMOVE(&channel->multipart[i].multipart_list, p, entry);
        ret = pbuf_copy(*pbuf, p);
        pbuf_reset(p);
        pbuf_free(p);

        /* check return val from pbuf_copy(). */
        if (ret != LAGOPUS_RESULT_OK) {
          if (ret == LAGOPUS_RESULT_OUT_OF_RANGE) {
            /* overwrite return val. */
            ret = LAGOPUS_RESULT_NO_MEMORY;
          }
          break;
        }
      }

      if (ret == LAGOPUS_RESULT_OK) {
        reset(&channel->multipart[i]);
      } else {
        multipart_free(&channel->multipart[i]);
      }
    }
  }
  channel_unlock(channel);
  return ret;
}

uint8_t
channel_auxiliary_id_get(struct channel *channel) {
  return channel->auxiliary_id;
}

struct channel_list *
channel_list_alloc(void) {
  lagopus_result_t ret;
  struct channel_list *channel_list;

  channel_list = malloc(sizeof(struct channel_list));
  if (channel_list == NULL) {
    return channel_list;
  }

  ret = lagopus_mutex_create(&channel_list->lock);
  if (ret != LAGOPUS_RESULT_OK) {
    lagopus_perror(ret);
    free(channel_list);
    return NULL;
  }

  channel_list->generation_is_defined = false;
  channel_list->generation_id = 0;

  channel_list->channel_id_max = 0;

  LIST_INIT(&channel_list->channel_h);

  return channel_list;
}

/* Assume all channels was freed. */
void
channel_list_free(struct channel_list *channel_list) {
  if (LIST_EMPTY(&channel_list->channel_h) == false) {
    lagopus_msg_fatal("channel exists.\n");
  }

  lagopus_mutex_destroy(&channel_list->lock);
  free(channel_list);

  return;
}

lagopus_result_t
channel_list_insert(struct channel_list *channel_list,
                    struct channel *channel) {
  if (channel_list == NULL || channel == NULL) {
    return LAGOPUS_RESULT_INVALID_ARGS;
  }

  lagopus_mutex_lock(&channel_list->lock);
  LIST_INSERT_HEAD(&channel_list->channel_h, channel, dp_entry);
  lagopus_mutex_unlock(&channel_list->lock);

  return LAGOPUS_RESULT_OK;
}

lagopus_result_t
channel_list_iterate(struct channel_list *channel_list,
                     lagopus_result_t (*func)(struct channel *channel, void *val), void *val) {
  lagopus_result_t ret;
  struct channel *channel;

  if (channel_list == NULL || func == NULL) {
    return LAGOPUS_RESULT_INVALID_ARGS;
  }

  lagopus_mutex_lock(&channel_list->lock);
  LIST_FOREACH(channel, &channel_list->channel_h, dp_entry) {
    ret = func(channel, val);
    if (ret != LAGOPUS_RESULT_OK) {
      break;
    }
  }
  lagopus_mutex_unlock(&channel_list->lock);

  return ret;
}

uint64_t
channel_list_channel_id_get(struct channel_list *channel_list) {
  uint64_t ret;

  channel_list_lock(channel_list);
  ret = channel_list->channel_id_max++;
  channel_list_unlock(channel_list);

  return ret;
}

bool
channel_is_alive(struct channel *channel) {
  if (channel == NULL) {
    return false;
  }

  if (channel->status != Established && channel->status != HelloSent) {
    return false;
  }

  if (session_is_alive(channel->session) == false) {
    return false;
  }

  return true;
}

void
channel_role_mask_get(struct channel *channel,
                      struct ofp_async_config *role_mask) {
  channel_lock(channel);
  *role_mask = channel->role_mask;
  channel_unlock(channel);
}

void
channel_role_mask_set(struct channel *channel,
                      struct ofp_async_config *role_mask) {
  channel_lock(channel);
  channel->role_mask = *role_mask;
  channel_unlock(channel);
}

static lagopus_result_t
count_alive_num(struct channel *channel, void *val) {
  if (channel_is_alive(channel) == true) {
    (*(int *) val)++;
  }

  return LAGOPUS_RESULT_OK;
}

int
channel_list_get_alive_num(struct channel_list *channel_list) {
  int cnt = 0;
  lagopus_result_t ret;

  ret = channel_list_iterate(channel_list, count_alive_num, (void *) &cnt);
  if (ret != LAGOPUS_RESULT_OK) {
    return 0;
  }

  return cnt;
}

void
channel_list_lock(struct channel_list *channel_list) {
  lagopus_mutex_lock(&channel_list->lock);
}

void
channel_list_unlock(struct channel_list *channel_list) {
  lagopus_mutex_unlock(&channel_list->lock);
}

bool
channel_role_channel_check_mask(struct channel *channel,
                                uint8_t type, uint8_t reason) {
  bool ret;
  channel_lock(channel);
  ret = ofp_role_channel_check_mask(&channel->role_mask, type,
                                    reason, channel->role);
  channel_unlock(channel);

  return ret;
}

bool
channel_list_generation_id_is_defined(struct channel_list *channel_list) {
  bool ret;
  channel_list_lock(channel_list);
  ret = channel_list->generation_is_defined;
  channel_list_unlock(channel_list);

  return ret;
}

uint64_t
channel_list_generation_id_get(struct channel_list *channel_list) {
  uint64_t gen;

  channel_list_lock(channel_list);
  gen = channel_list->generation_id;
  channel_list_unlock(channel_list);

  return gen;
}

void
channel_list_generation_id_set(struct channel_list *channel_list,
                               uint64_t gen) {
  channel_list_lock(channel_list);
  channel_list->generation_is_defined = true;
  channel_list->generation_id = gen;
  channel_list_unlock(channel_list);

  return;
}

void
channel_enable(struct channel *channel) {
  channel_lock(channel);
  channel_update(channel, Idle);
  channel_unlock(channel);
}

void
channel_hello_received_set(struct channel *channel) {
  channel_event(channel, OpenFlow_Hello_Received);
}

void
channel_disable(struct channel *channel) {
  channel_lock(channel);
  if (channel->expire_ev == NULL
      && channel->event != Channel_Expired) {
    channel_update(channel, Disable);
    channel->expire_ev =
      event_register_event(channel->em, channel_do_expire, channel);
  }
  channel_unlock(channel);
}
