/* * Copyright (c) 2012, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file channeltls.c
 * \brief channel_t concrete subclass using or_connection_t
 **/

/*
 * Define this so channel.h gives us things only channel_t subclasses
 * should touch.
 */

#define _TOR_CHANNEL_INTERNAL

#include "or.h"
#include "channel.h"
#include "channeltls.h"
#include "config.h"
#include "connection.h"
#include "connection_or.h"
#include "control.h"
#include "relay.h"
#include "router.h"
#include "routerlist.h"

/** How many CELL_PADDING cells have we received, ever? */
uint64_t stats_n_padding_cells_processed = 0;
/** How many CELL_VERSIONS cells have we received, ever? */
uint64_t stats_n_versions_cells_processed = 0;
/** How many CELL_NETINFO cells have we received, ever? */
uint64_t stats_n_netinfo_cells_processed = 0;
/** How many CELL_VPADDING cells have we received, ever? */
uint64_t stats_n_vpadding_cells_processed = 0;
/** How many CELL_CERTS cells have we received, ever? */
uint64_t stats_n_certs_cells_processed = 0;
/** How many CELL_AUTH_CHALLENGE cells have we received, ever? */
uint64_t stats_n_auth_challenge_cells_processed = 0;
/** How many CELL_AUTHENTICATE cells have we received, ever? */
uint64_t stats_n_authenticate_cells_processed = 0;
/** How many CELL_AUTHORIZE cells have we received, ever? */
uint64_t stats_n_authorize_cells_processed = 0;

struct channel_tls_s {
  /* Base channel_t struct */
  channel_t _base;
  /* or_connection_t pointer */
  or_connection_t *conn;
};

/* channel_tls_t method declarations */

static void channel_tls_close_method(channel_t *chan);
static void channel_tls_write_cell_method(channel_t *chan,
                                          cell_t *cell);
static void channel_tls_write_var_cell_method(channel_t *chan,
                                              var_cell_t *var_cell);

/** Handle incoming cells for the handshake stuff here rather than
 * passing them on up. */

static void channel_tls_process_versions_cell(var_cell_t *cell,
                                              channel_tls_t *tlschan);
static void channel_tls_process_netinfo_cell(cell_t *cell,
                                             channel_tls_t *tlschan);
static void channel_tls_process_certs_cell(var_cell_t *cell,
                                           channel_tls_t *tlschan);
static void channel_tls_process_auth_challenge_cell(var_cell_t *cell,
                                                    channel_tls_t *tlschan);
static void channel_tls_process_authenticate_cell(var_cell_t *cell,
                                                  channel_tls_t *tlschan);
static int command_allowed_before_handshake(uint8_t command);
static int enter_v3_handshake_with_cell(var_cell_t *cell,
                                        channel_tls_t *tlschan);

/** Launch a new OR connection to <b>addr</b>:<b>port</b> and expect to
 * handshake with an OR with identity digest <b>id_digest</b>.
 *
 * If <b>id_digest</b> is me, do nothing. If we're already connected to it,
 * return that connection. If the connect() is in progress, set the
 * new conn's state to 'connecting' and return it. If connect() succeeds,
 * call connection_tls_start_handshake() on it.
 *
 * This function is called from router_retry_connections(), for
 * ORs connecting to ORs, and circuit_establish_circuit(), for
 * OPs connecting to ORs.
 *
 * Return the launched conn, or NULL if it failed.
 */

channel_t *
channel_tls_connect(const tor_addr_t *addr, uint16_t port,
                    const char *id_digest)
{
  channel_tls_t *tlschan = tor_malloc_zero(sizeof(*tlschan));
  channel_t *chan = TLS_CHAN_TO_BASE(tlschan);
  channel_init(chan);
  chan->state = CHANNEL_STATE_OPENING;
  chan->close = channel_tls_close_method;
  chan->write_cell = channel_tls_write_cell_method;
  chan->write_var_cell = channel_tls_write_var_cell_method;

  chan->active_circuit_pqueue = smartlist_new();
  chan->active_circuit_pqueue_last_recalibrated = cell_ewma_get_tick();

  /* Set up or_connection stuff */
  tlschan->conn = connection_or_connect(addr, port, id_digest, tlschan);
  if (!(tlschan->conn)) {
    channel_change_state(chan, CHANNEL_STATE_ERROR);
    goto err;
  }

  goto done;

 err:
  smartlist_free(chan->active_circuit_pqueue);
  tor_free(tlschan);
  chan = NULL;

 done:
  return chan;
}

/** Close a channel_tls_t */

static void
channel_tls_close_method(channel_t *chan)
{
  channel_tls_t *tlschan = BASE_CHAN_TO_TLS(chan);

  tor_assert(tlschan);

  /* TODO */
}

/** Given a channel_tls_t and a cell_t, transmit the cell_t */

static void
channel_tls_write_cell_method(channel_t *chan, cell_t *cell)
{
  channel_tls_t *tlschan = BASE_CHAN_TO_TLS(chan);

  tor_assert(tlschan);
  tor_assert(cell);
  tor_assert(tlschan->conn);

  connection_or_write_cell_to_buf(cell, tlschan->conn);
}

/** Given a channel_tls_t and a var_cell_t, transmit the var_cell_t */

static void
channel_tls_write_var_cell_method(channel_t *chan, var_cell_t *var_cell)
{
  channel_tls_t *tlschan = BASE_CHAN_TO_TLS(chan);

  tor_assert(tlschan);
  tor_assert(var_cell);
  tor_assert(tlschan->conn);

  connection_or_write_var_cell_to_buf(var_cell, tlschan->conn);
}

/** Handle events on an or_connection_t in these functions */

/** connection_or.c will call this when the or_connection_t associated
 * with this channel_tls_t changes state. */

void
channel_tls_handle_state_change_on_orconn(channel_tls_t *chan,
                                          or_connection_t *conn,
                                          uint8_t old_state,
                                          uint8_t state)
{
  channel_t *base_chan;

  tor_assert(chan);
  tor_assert(conn);
  tor_assert(conn->chan == chan);
  tor_assert(chan->conn == conn);
  /* -Werror appeasement */
  tor_assert(old_state == old_state);

  base_chan = TLS_CHAN_TO_BASE(chan);

  /* Make sure the base connection state makes sense - shouldn't be error,
   * closed or listening. */

  tor_assert(base_chan->state == CHANNEL_STATE_OPENING ||
             base_chan->state == CHANNEL_STATE_OPEN ||
             base_chan->state == CHANNEL_STATE_MAINT ||
             base_chan->state == CHANNEL_STATE_CLOSING);

  /* Did we just go to state open? */
  if (state == OR_CONN_STATE_OPEN) {
    /*
     * We can go to CHANNEL_STATE_OPEN from CHANNEL_STATE_OPENING or
     * CHANNEL_STATE_MAINT on this.
     */
    channel_change_state(base_chan, CHANNEL_STATE_OPEN);
  } else {
    /*
     * Not open, so from CHANNEL_STATE_OPEN we go to CHANNEL_STATE_MAINT,
     * otherwise no change.
     */
    if (base_chan->state == CHANNEL_STATE_OPEN) {
      channel_change_state(base_chan, CHANNEL_STATE_MAINT);
    }
  }
}

#ifdef KEEP_TIMING_STATS
/** This is a wrapper function around the actual function that processes the
 * <b>cell</b> that just arrived on <b>conn</b>. Increment <b>*time</b>
 * by the number of microseconds used by the call to <b>*func(cell, conn)</b>.
 */

static void
channel_tls_time_process_cell(cell_t *cell, channel_tls_t *chan, int *time,
                              void (*func)(cell_t *, channel_tls_t *))
{
  struct timeval start, end;
  long time_passed;

  tor_gettimeofday(&start);

  (*func)(cell, chan);

  tor_gettimeofday(&end);
  time_passed = tv_udiff(&start, &end) ;

  if (time_passed > 10000) { /* more than 10ms */
    log_debug(LD_OR,"That call just took %ld ms.",time_passed/1000);
  }

  if (time_passed < 0) {
    log_info(LD_GENERAL,"That call took us back in time!");
    time_passed = 0;
  }

  *time += time_passed;
}
#endif

/** Handle an incoming cell, called from connection_or.c */

void
channel_tls_handle_cell(cell_t *cell, or_connection_t *conn)
{
  channel_tls_t *chan;
  int handshaking;

#ifdef KEEP_TIMING_STATS
#define PROCESS_CELL(tp, cl, cn) STMT_BEGIN {                   \
    ++num ## tp;                                                \
    channel_tls_time_process_cell(cl, cn, & tp ## time ,            \
                             channel_tls_process_ ## tp ## _cell);  \
    } STMT_END
#else
#define PROCESS_CELL(tp, cl, cn) channel_tls_process_ ## tp ## _cell(cl, cn)
#endif

  tor_assert(cell);
  tor_assert(conn);

  chan = conn->chan;

 if (!chan) {
   log_warn(LD_CHANNEL,
            "Got a cell_t on an OR connection with no channel");
   return;
  }

  handshaking = (TO_CONN(conn)->state != OR_CONN_STATE_OPEN);

  if (conn->_base.marked_for_close)
    return;

  /* Reject all but VERSIONS and NETINFO when handshaking. */
  /* (VERSIONS should actually be impossible; it's variable-length.) */
  if (handshaking && cell->command != CELL_VERSIONS &&
      cell->command != CELL_NETINFO) {
    log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
           "Received unexpected cell command %d in chan state %s / "
           "conn state %s; closing the connection.",
           (int)cell->command,
           channel_state_to_string(TLS_CHAN_TO_BASE(chan)->state),
           conn_state_to_string(CONN_TYPE_OR, TO_CONN(conn)->state));
           connection_mark_for_close(TO_CONN(conn));
    return;
  }

  if (conn->_base.state == OR_CONN_STATE_OR_HANDSHAKING_V3)
    or_handshake_state_record_cell(conn->handshake_state, cell, 1);

  switch (cell->command) {
    case CELL_PADDING:
      ++stats_n_padding_cells_processed;
      /* do nothing */
      break;
    case CELL_VERSIONS:
      tor_fragile_assert();
      break;
    case CELL_NETINFO:
      ++stats_n_netinfo_cells_processed;
      PROCESS_CELL(netinfo, cell, chan);
      break;
    case CELL_CREATE:
    case CELL_CREATE_FAST:
    case CELL_CREATED:
    case CELL_CREATED_FAST:
    case CELL_RELAY:
    case CELL_RELAY_EARLY:
    case CELL_DESTROY:
      /*
       * These are all transport independent and we pass them up through the
       * channel_t mechanism.  They are ultimately handled in cmd.c.
       */
      /* TODO pass through channel_t */
      break;
    default:
      log_fn(LOG_INFO, LD_PROTOCOL,
             "Cell of unknown type (%d) received in channeltls.c.  "
             "Dropping.",
             cell->command);
             break;
  }
}

/** Process a <b>var_cell</b> that was just received on <b>conn</b>. Keep
 * internal statistics about how many of each cell we've processed so far
 * this second, and the total number of microseconds it took to
 * process each type of cell.  All the var_cell commands are handshake-
 * related and live below the channel_t layer, so no variable-length
 * cells ever get delivered in the current implementation, but I've left
 * the mechanism in place for future use.
 */

void
channel_tls_handle_var_cell(var_cell_t *var_cell, or_connection_t *conn)
{
  channel_tls_t *chan;
  int handshaking;

#ifdef KEEP_TIMING_STATS
  /* how many of each cell have we seen so far this second? needs better
   * name. */
  static int num_versions = 0, num_certs = 0;
  static time_t current_second = 0; /* from previous calls to time */
  time_t now = time(NULL);

  if (current_second == 0) current_second = now;
  if (now > current_second) { /* the second has rolled over */
    /* print stats */
    log_info(LD_OR,
             "At end of second: %d versions (%d ms), %d certs (%d ms)",
             num_versions, versions_time / ((now - current_second) * 1000),
             num_certs, certs_time / ((now - current_second) * 1000));

    num_versions = num_certs = 0;
    versions_time = certs_time = 0;

    /* remember which second it is, for next time */
    current_second = now;
  }
#endif

  tor_assert(var_cell);
  tor_assert(conn);

  chan = conn->chan;

  if (!chan) {
    log_warn(LD_CHANNEL,
             "Got a var_cell_t on an OR connection with no channel");
    return;
  }

  handshaking = (TO_CONN(conn)->state != OR_CONN_STATE_OPEN);

  if (TO_CONN(conn)->marked_for_close)
    return;

  switch (TO_CONN(conn)->state) {
    case OR_CONN_STATE_OR_HANDSHAKING_V2:
      if (var_cell->command != CELL_VERSIONS) {
        log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
               "Received a cell with command %d in unexpected "
               "orconn state \"%s\" [%d], channel state \"%s\" [%d]; "
               "closing the connection.",
               (int)(var_cell->command),
               conn_state_to_string(CONN_TYPE_OR, TO_CONN(conn)->state),
               TO_CONN(conn)->state,
               channel_state_to_string(TLS_CHAN_TO_BASE(chan)->state),
               (int)(TLS_CHAN_TO_BASE(chan)->state));
        /* TODO channel to error? */
        connection_mark_for_close(TO_CONN(conn));
        return;
      }
      break;
    case OR_CONN_STATE_TLS_HANDSHAKING:
      /* If we're using bufferevents, it's entirely possible for us to
       * notice "hey, data arrived!" before we notice "hey, the handshake
       * finished!" And we need to be accepting both at once to handle both
       * the v2 and v3 handshakes. */

      /* fall through */
    case OR_CONN_STATE_TLS_SERVER_RENEGOTIATING:
      if (!(command_allowed_before_handshake(var_cell->command))) {
        log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
               "Received a cell with command %d in unexpected "
               "orconn state \"%s\" [%d], channel state \"%s\" [%d]; "
               "closing the connection.",
               (int)(var_cell->command),
               conn_state_to_string(CONN_TYPE_OR, TO_CONN(conn)->state),
               (int)(TO_CONN(conn)->state),
               channel_state_to_string(TLS_CHAN_TO_BASE(chan)->state),
               (int)(TLS_CHAN_TO_BASE(chan)->state));
        /* TODO channel to error? */
        connection_mark_for_close(TO_CONN(conn));
        return;
      } else {
        if (enter_v3_handshake_with_cell(var_cell, chan) < 0)
          return;
      }
      break;
    case OR_CONN_STATE_OR_HANDSHAKING_V3:
      if (var_cell->command != CELL_AUTHENTICATE)
        or_handshake_state_record_var_cell(conn->handshake_state, var_cell, 1);
      break; /* Everything is allowed */
    case OR_CONN_STATE_OPEN:
      if (conn->link_proto < 3) {
        log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
               "Received a variable-length cell with command %d in orconn "
               "state %s [%d], channel state %s [%d] with link protocol %d; "
               "ignoring it.",
               (int)(var_cell->command),
               conn_state_to_string(CONN_TYPE_OR, TO_CONN(conn)->state),
               (int)(TO_CONN(conn)->state),
               channel_state_to_string(TLS_CHAN_TO_BASE(chan)->state),
               (int)(TLS_CHAN_TO_BASE(chan)->state),
               (int)(conn->link_proto));
        return;
      }
      break;
    default:
      log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
             "Received var-length cell with command %d in unexpected "
             "orconn state \"%s\" [%d], channel state \"%s\" [%d]; "
             "ignoring it.",
             (int)(var_cell->command),
             conn_state_to_string(CONN_TYPE_OR, TO_CONN(conn)->state),
             (int)(TO_CONN(conn)->state),
             channel_state_to_string(TLS_CHAN_TO_BASE(chan)->state),
             (int)(TLS_CHAN_TO_BASE(chan)->state));
      return;
  }

  /* Now handle the cell */

  switch (var_cell->command) {
    case CELL_VERSIONS:
      ++stats_n_versions_cells_processed;
      PROCESS_CELL(versions, var_cell, chan);
      break;
    case CELL_VPADDING:
      ++stats_n_vpadding_cells_processed;
      /* Do nothing */
      break;
    case CELL_CERTS:
      ++stats_n_certs_cells_processed;
      PROCESS_CELL(certs, var_cell, chan);
      break;
    case CELL_AUTH_CHALLENGE:
      ++stats_n_auth_challenge_cells_processed;
      PROCESS_CELL(auth_challenge, var_cell, chan);
      break;
    case CELL_AUTHENTICATE:
      ++stats_n_authenticate_cells_processed;
      PROCESS_CELL(authenticate, var_cell, chan);
      break;
    case CELL_AUTHORIZE:
      ++stats_n_authorize_cells_processed;
      /* Ignored so far. */
      break;
    default:
      log_fn(LOG_INFO, LD_PROTOCOL,
             "Variable-length cell of unknown type (%d) received.",
             (int)(var_cell->command));
      break;
  }
}

/** Return true if <b>command</b> is a cell command that's allowed to start a
 * V3 handshake. */
static int
command_allowed_before_handshake(uint8_t command)
{
  switch (command) {
    case CELL_VERSIONS:
    case CELL_VPADDING:
    case CELL_AUTHORIZE:
      return 1;
    default:
      return 0;
  }
}

/** Called when we as a server receive an appropriate cell while waiting
 * either for a cell or a TLS handshake.  Set the connection's state to
 * "handshaking_v3', initializes the or_handshake_state field as needed,
 * and add the cell to the hash of incoming cells.)
 *
 * Return 0 on success; return -1 and mark the connection on failure.
 */
static int
enter_v3_handshake_with_cell(var_cell_t *cell, channel_tls_t *chan)
{
  int started_here = 0;

  tor_assert(cell);
  tor_assert(chan);
  tor_assert(chan->conn);

  started_here = connection_or_nonopen_was_started_here(chan->conn);

  tor_assert(TO_CONN(chan->conn)->state == OR_CONN_STATE_TLS_HANDSHAKING ||
             TO_CONN(chan->conn)->state ==
               OR_CONN_STATE_TLS_SERVER_RENEGOTIATING);

  if (started_here) {
    log_fn(LOG_PROTOCOL_WARN, LD_OR,
           "Received a cell while TLS-handshaking, not in "
           "OR_HANDSHAKING_V3, on a connection we originated.");
  }
  chan->conn->_base.state = OR_CONN_STATE_OR_HANDSHAKING_V3;
  if (connection_init_or_handshake_state(chan->conn, started_here) < 0) {
    connection_mark_for_close(TO_CONN(chan->conn));
    channel_change_state(TLS_CHAN_TO_BASE(chan),
                         CHANNEL_STATE_ERROR);
    return -1;
  }
  or_handshake_state_record_var_cell(chan->conn->handshake_state, cell, 1);
  return 0;
}

/** Process a 'versions' cell.  The current link protocol version must be 0
 * to indicate that no version has yet been negotiated.  We compare the
 * versions in the cell to the list of versions we support, pick the
 * highest version we have in common, and continue the negotiation from
 * there.
 */
static void
channel_tls_process_versions_cell(var_cell_t *cell, channel_tls_t *chan)
{
  int highest_supported_version = 0;
  const uint8_t *cp, *end;
  int started_here = 0;

  tor_assert(cell);
  tor_assert(chan);
  tor_assert(chan->conn);

  started_here = connection_or_nonopen_was_started_here(chan->conn);

  if (chan->conn->link_proto != 0 ||
      (chan->conn->handshake_state &&
       chan->conn->handshake_state->received_versions)) {
    log_fn(LOG_PROTOCOL_WARN, LD_OR,
           "Received a VERSIONS cell on a connection with its version "
           "already set to %d; dropping",
           (int)(chan->conn->link_proto));
    return;
  }
  switch (chan->conn->_base.state)
    {
    case OR_CONN_STATE_OR_HANDSHAKING_V2:
    case OR_CONN_STATE_OR_HANDSHAKING_V3:
      break;
    case OR_CONN_STATE_TLS_HANDSHAKING:
    case OR_CONN_STATE_TLS_SERVER_RENEGOTIATING:
    default:
      log_fn(LOG_PROTOCOL_WARN, LD_OR,
             "VERSIONS cell while in unexpected state");
      return;
  }

  tor_assert(chan->conn->handshake_state);
  end = cell->payload + cell->payload_len;
  for (cp = cell->payload; cp+1 < end; ++cp) {
    uint16_t v = ntohs(get_uint16(cp));
    if (is_or_protocol_version_known(v) && v > highest_supported_version)
      highest_supported_version = v;
  }
  if (!highest_supported_version) {
    log_fn(LOG_PROTOCOL_WARN, LD_OR,
           "Couldn't find a version in common between my version list and the "
           "list in the VERSIONS cell; closing connection.");
    connection_mark_for_close(TO_CONN(chan->conn));
    channel_change_state(TLS_CHAN_TO_BASE(chan),
                         CHANNEL_STATE_ERROR);
    return;
  } else if (highest_supported_version == 1) {
    /* Negotiating version 1 makes no sense, since version 1 has no VERSIONS
     * cells. */
    log_fn(LOG_PROTOCOL_WARN, LD_OR,
           "Used version negotiation protocol to negotiate a v1 connection. "
           "That's crazily non-compliant. Closing connection.");
    connection_mark_for_close(TO_CONN(chan->conn));
    channel_change_state(TLS_CHAN_TO_BASE(chan),
                         CHANNEL_STATE_ERROR);
    return;
  } else if (highest_supported_version < 3 &&
             chan->conn->_base.state ==  OR_CONN_STATE_OR_HANDSHAKING_V3) {
    log_fn(LOG_PROTOCOL_WARN, LD_OR,
           "Negotiated link protocol 2 or lower after doing a v3 TLS "
           "handshake. Closing connection.");
    connection_mark_for_close(TO_CONN(chan->conn));
    channel_change_state(TLS_CHAN_TO_BASE(chan),
                         CHANNEL_STATE_ERROR);
    return;
  }

  chan->conn->link_proto = highest_supported_version;
  chan->conn->handshake_state->received_versions = 1;

  if (chan->conn->link_proto == 2) {
    log_info(LD_OR,
             "Negotiated version %d with %s:%d; sending NETINFO.",
             highest_supported_version,
             safe_str_client(chan->conn->_base.address),
             chan->conn->_base.port);

    if (connection_or_send_netinfo(chan->conn) < 0) {
      connection_mark_for_close(TO_CONN(chan->conn));
      channel_change_state(TLS_CHAN_TO_BASE(chan),
                           CHANNEL_STATE_ERROR);
      return;
    }
  } else {
    const int send_versions = !started_here;
    /* If we want to authenticate, send a CERTS cell */
    const int send_certs = !started_here || public_server_mode(get_options());
    /* If we're a relay that got a connection, ask for authentication. */
    const int send_chall = !started_here && public_server_mode(get_options());
    /* If our certs cell will authenticate us, we can send a netinfo cell
     * right now. */
    const int send_netinfo = !started_here;
    const int send_any =
      send_versions || send_certs || send_chall || send_netinfo;
    tor_assert(chan->conn->link_proto >= 3);

    log_info(LD_OR,
             "Negotiated version %d with %s:%d; %s%s%s%s%s",
             highest_supported_version,
             safe_str_client(chan->conn->_base.address),
             chan->conn->_base.port,
             send_any ? "Sending cells:" : "Waiting for CERTS cell",
             send_versions ? " VERSIONS" : "",
             send_certs ? " CERTS" : "",
             send_chall ? " AUTH_CHALLENGE" : "",
             send_netinfo ? " NETINFO" : "");

#ifdef DISABLE_V3_LINKPROTO_SERVERSIDE
    if (1) {
      connection_mark_for_close(TO_CONN(chan->conn));
      channel_change_state(TLS_CHAN_TO_BASE(chan),
                           CHANNEL_STATE_CLOSING);
      return;
    }
#endif

    if (send_versions) {
      if (connection_or_send_versions(chan->conn, 1) < 0) {
        log_warn(LD_OR, "Couldn't send versions cell");
        connection_mark_for_close(TO_CONN(chan->conn));
        channel_change_state(TLS_CHAN_TO_BASE(chan),
                             CHANNEL_STATE_ERROR);
        return;
      }
    }
    if (send_certs) {
      if (connection_or_send_certs_cell(chan->conn) < 0) {
        log_warn(LD_OR, "Couldn't send certs cell");
        connection_mark_for_close(TO_CONN(chan->conn));
        channel_change_state(TLS_CHAN_TO_BASE(chan),
                             CHANNEL_STATE_ERROR);
        return;
      }
    }
    if (send_chall) {
      if (connection_or_send_auth_challenge_cell(chan->conn) < 0) {
        log_warn(LD_OR, "Couldn't send auth_challenge cell");
        connection_mark_for_close(TO_CONN(chan->conn));
        channel_change_state(TLS_CHAN_TO_BASE(chan),
                             CHANNEL_STATE_ERROR);
        return;
      }
    }
    if (send_netinfo) {
      if (connection_or_send_netinfo(chan->conn) < 0) {
        log_warn(LD_OR, "Couldn't send netinfo cell");
        connection_mark_for_close(TO_CONN(chan->conn));
        channel_change_state(TLS_CHAN_TO_BASE(chan),
                             CHANNEL_STATE_ERROR);
        return;
      }
    }
  }
}

/** Process a 'netinfo' cell: read and act on its contents, and set the
 * connection state to "open". */
static void
channel_tls_process_netinfo_cell(cell_t *cell, channel_tls_t *chan)
{
  time_t timestamp;
  uint8_t my_addr_type;
  uint8_t my_addr_len;
  const uint8_t *my_addr_ptr;
  const uint8_t *cp, *end;
  uint8_t n_other_addrs;
  time_t now = time(NULL);

  long apparent_skew = 0;
  tor_addr_t my_apparent_addr = TOR_ADDR_NULL;

  tor_assert(cell);
  tor_assert(chan);
  tor_assert(chan->conn);

  if (chan->conn->link_proto < 2) {
    log_fn(LOG_PROTOCOL_WARN, LD_OR,
           "Received a NETINFO cell on %s connection; dropping.",
           chan->conn->link_proto == 0 ? "non-versioned" : "a v1");
    return;
  }
  if (chan->conn->_base.state != OR_CONN_STATE_OR_HANDSHAKING_V2 &&
      chan->conn->_base.state != OR_CONN_STATE_OR_HANDSHAKING_V3) {
    log_fn(LOG_PROTOCOL_WARN, LD_OR,
           "Received a NETINFO cell on non-handshaking connection; dropping.");
    return;
  }
  tor_assert(chan->conn->handshake_state &&
             chan->conn->handshake_state->received_versions);

  if (chan->conn->_base.state == OR_CONN_STATE_OR_HANDSHAKING_V3) {
    tor_assert(chan->conn->link_proto >= 3);
    if (chan->conn->handshake_state->started_here) {
      if (!(chan->conn->handshake_state->authenticated)) {
        log_fn(LOG_PROTOCOL_WARN, LD_OR,
               "Got a NETINFO cell from server, "
               "but no authentication.  Closing the connection.");
        connection_mark_for_close(TO_CONN(chan->conn));
        channel_change_state(TLS_CHAN_TO_BASE(chan),
                             CHANNEL_STATE_ERROR);
        return;
      }
    } else {
      /* we're the server.  If the client never authenticated, we have
         some housekeeping to do.*/
      if (!(chan->conn->handshake_state->authenticated)) {
        tor_assert(tor_digest_is_zero(
                  (const char*)(chan->conn->handshake_state->
                      authenticated_peer_id)));
        connection_or_set_circid_type(chan->conn, NULL);

        connection_or_init_conn_from_address(chan->conn,
                  &(chan->conn->_base.addr),
                  chan->conn->_base.port,
                  (const char*)(chan->conn->handshake_state->
                   authenticated_peer_id),
                  0);
      }
    }
  }

  /* Decode the cell. */
  timestamp = ntohl(get_uint32(cell->payload));
  if (labs(now - chan->conn->handshake_state->sent_versions_at) < 180) {
    apparent_skew = now - timestamp;
  }

  my_addr_type = (uint8_t) cell->payload[4];
  my_addr_len = (uint8_t) cell->payload[5];
  my_addr_ptr = (uint8_t*) cell->payload + 6;
  end = cell->payload + CELL_PAYLOAD_SIZE;
  cp = cell->payload + 6 + my_addr_len;
  if (cp >= end) {
    log_fn(LOG_PROTOCOL_WARN, LD_OR,
           "Addresses too long in netinfo cell; closing connection.");
    connection_mark_for_close(TO_CONN(chan->conn));
    channel_change_state(TLS_CHAN_TO_BASE(chan),
                         CHANNEL_STATE_ERROR);
    return;
  } else if (my_addr_type == RESOLVED_TYPE_IPV4 && my_addr_len == 4) {
    tor_addr_from_ipv4n(&my_apparent_addr, get_uint32(my_addr_ptr));
  } else if (my_addr_type == RESOLVED_TYPE_IPV6 && my_addr_len == 16) {
    tor_addr_from_ipv6_bytes(&my_apparent_addr, (const char *) my_addr_ptr);
  }

  n_other_addrs = (uint8_t) *cp++;
  while (n_other_addrs && cp < end-2) {
    /* Consider all the other addresses; if any matches, this connection is
     * "canonical." */
    tor_addr_t addr;
    const uint8_t *next =
      decode_address_from_payload(&addr, cp, (int)(end-cp));
    if (next == NULL) {
      log_fn(LOG_PROTOCOL_WARN,  LD_OR,
             "Bad address in netinfo cell; closing connection.");
      connection_mark_for_close(TO_CONN(chan->conn));
      channel_change_state(TLS_CHAN_TO_BASE(chan),
                           CHANNEL_STATE_ERROR);
      return;
    }
    if (tor_addr_eq(&addr, &(chan->conn->real_addr))) {
      chan->conn->is_canonical = 1;
      break;
    }
    cp = next;
    --n_other_addrs;
  }

  /* Act on apparent skew. */
  /** Warn when we get a netinfo skew with at least this value. */
#define NETINFO_NOTICE_SKEW 3600
  if (labs(apparent_skew) > NETINFO_NOTICE_SKEW &&
      router_get_by_id_digest(chan->conn->identity_digest)) {
    char dbuf[64];
    int severity;
    /*XXXX be smarter about when everybody says we are skewed. */
    if (router_digest_is_trusted_dir(chan->conn->identity_digest))
      severity = LOG_WARN;
    else
      severity = LOG_INFO;
    format_time_interval(dbuf, sizeof(dbuf), apparent_skew);
    log_fn(severity, LD_GENERAL,
           "Received NETINFO cell with skewed time from "
           "server at %s:%d.  It seems that our clock is %s by %s, or "
           "that theirs is %s. Tor requires an accurate clock to work: "
           "please check your time and date settings.",
           chan->conn->_base.address,
           (int)(chan->conn->_base.port),
           apparent_skew > 0 ? "ahead" : "behind",
           dbuf,
           apparent_skew > 0 ? "behind" : "ahead");
    if (severity == LOG_WARN) /* only tell the controller if an authority */
      control_event_general_status(LOG_WARN,
                          "CLOCK_SKEW SKEW=%ld SOURCE=OR:%s:%d",
                          apparent_skew,
                          chan->conn->_base.address,
                          chan->conn->_base.port);
  }

  /* XXX maybe act on my_apparent_addr, if the source is sufficiently
   * trustworthy. */

  if (connection_or_set_state_open(chan->conn) < 0) {
    log_fn(LOG_PROTOCOL_WARN, LD_OR,
           "Got good NETINFO cell from %s:%d; but "
           "was unable to make the OR connection become open.",
           safe_str_client(chan->conn->_base.address),
           chan->conn->_base.port);
    connection_mark_for_close(TO_CONN(chan->conn));
    channel_change_state(TLS_CHAN_TO_BASE(chan),
                         CHANNEL_STATE_ERROR);
  } else {
    log_info(LD_OR,
             "Got good NETINFO cell from %s:%d; OR connection is now "
             "open, using protocol version %d. Its ID digest is %s. "
             "Our address is apparently %s.",
             safe_str_client(chan->conn->_base.address),
             chan->conn->_base.port,
             (int)(chan->conn->link_proto),
             hex_str(TLS_CHAN_TO_BASE(chan)->identity_digest, DIGEST_LEN),
             tor_addr_is_null(&my_apparent_addr) ?
             "<none>" : fmt_and_decorate_addr(&my_apparent_addr));
  }
  assert_connection_ok(TO_CONN(chan->conn),time(NULL));
}

/** Process a CERTS cell from a channel.
 *
 * If the other side should not have sent us a CERTS cell, or the cell is
 * malformed, or it is supposed to authenticate the TLS key but it doesn't,
 * then mark the connection.
 *
 * If the cell has a good cert chain and we're doing a v3 handshake, then
 * store the certificates in or_handshake_state.  If this is the client side
 * of the connection, we then authenticate the server or mark the connection.
 * If it's the server side, wait for an AUTHENTICATE cell.
 */
static void
channel_tls_process_certs_cell(var_cell_t *cell, channel_tls_t *chan)
{
  tor_cert_t *link_cert = NULL;
  tor_cert_t *id_cert = NULL;
  tor_cert_t *auth_cert = NULL;
  uint8_t *ptr;
  int n_certs, i;
  int send_netinfo = 0;

  tor_assert(cell);
  tor_assert(chan);
  tor_assert(chan->conn);

#define ERR(s)                                                  \
  do {                                                          \
    log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,                      \
           "Received a bad CERTS cell from %s:%d: %s",          \
           safe_str(chan->conn->_base.address),                 \
           chan->conn->_base.port, (s));                        \
    connection_mark_for_close(TO_CONN(chan->conn));             \
    channel_change_state(TLS_CHAN_TO_BASE(chan),                \
                         CHANNEL_STATE_ERROR);                  \
    return;                                                     \
  } while (0)

  if (chan->conn->_base.state != OR_CONN_STATE_OR_HANDSHAKING_V3)
    ERR("We're not doing a v3 handshake!");
  if (chan->conn->link_proto < 3)
    ERR("We're not using link protocol >= 3");
  if (chan->conn->handshake_state->received_certs_cell)
    ERR("We already got one");
  if (chan->conn->handshake_state->authenticated) {
    /* Should be unreachable, but let's make sure. */
    ERR("We're already authenticated!");
  }
  if (cell->payload_len < 1)
    ERR("It had no body");
  if (cell->circ_id)
    ERR("It had a nonzero circuit ID");

  n_certs = cell->payload[0];
  ptr = cell->payload + 1;
  for (i = 0; i < n_certs; ++i) {
    uint8_t cert_type;
    uint16_t cert_len;
    if (ptr + 3 > cell->payload + cell->payload_len) {
      goto truncated;
    }
    cert_type = *ptr;
    cert_len = ntohs(get_uint16(ptr+1));
    if (ptr + 3 + cert_len > cell->payload + cell->payload_len) {
      goto truncated;
    }
    if (cert_type == OR_CERT_TYPE_TLS_LINK ||
        cert_type == OR_CERT_TYPE_ID_1024 ||
        cert_type == OR_CERT_TYPE_AUTH_1024) {
      tor_cert_t *cert = tor_cert_decode(ptr + 3, cert_len);
      if (!cert) {
        log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
               "Received undecodable certificate in CERTS cell from %s:%d",
               safe_str(chan->conn->_base.address),
               chan->conn->_base.port);
      } else {
        if (cert_type == OR_CERT_TYPE_TLS_LINK) {
          if (link_cert) {
            tor_cert_free(cert);
            ERR("Too many TLS_LINK certificates");
          }
          link_cert = cert;
        } else if (cert_type == OR_CERT_TYPE_ID_1024) {
          if (id_cert) {
            tor_cert_free(cert);
            ERR("Too many ID_1024 certificates");
          }
          id_cert = cert;
        } else if (cert_type == OR_CERT_TYPE_AUTH_1024) {
          if (auth_cert) {
            tor_cert_free(cert);
            ERR("Too many AUTH_1024 certificates");
          }
          auth_cert = cert;
        } else {
          tor_cert_free(cert);
        }
      }
    }
    ptr += 3 + cert_len;
    continue;

  truncated:
    ERR("It ends in the middle of a certificate");
  }

  if (chan->conn->handshake_state->started_here) {
    int severity;
    if (! (id_cert && link_cert))
      ERR("The certs we wanted were missing");
    /* Okay. We should be able to check the certificates now. */
    if (! tor_tls_cert_matches_key(chan->conn->tls, link_cert)) {
      ERR("The link certificate didn't match the TLS public key");
    }
    /* Note that this warns more loudly about time and validity if we were
    * _trying_ to connect to an authority, not necessarily if we _did_ connect
    * to one. */
    if (router_digest_is_trusted_dir(
          TLS_CHAN_TO_BASE(chan)->identity_digest))
      severity = LOG_WARN;
    else
      severity = LOG_PROTOCOL_WARN;

    if (! tor_tls_cert_is_valid(severity, link_cert, id_cert, 0))
      ERR("The link certificate was not valid");
    if (! tor_tls_cert_is_valid(severity, id_cert, id_cert, 1))
      ERR("The ID certificate was not valid");

    chan->conn->handshake_state->authenticated = 1;
    {
      const digests_t *id_digests = tor_cert_get_id_digests(id_cert);
      crypto_pk_t *identity_rcvd;
      if (!id_digests)
        ERR("Couldn't compute digests for key in ID cert");

      identity_rcvd = tor_tls_cert_get_key(id_cert);
      if (!identity_rcvd)
        ERR("Internal error: Couldn't get RSA key from ID cert.");
      memcpy(chan->conn->handshake_state->authenticated_peer_id,
             id_digests->d[DIGEST_SHA1], DIGEST_LEN);
      connection_or_set_circid_type(chan->conn, identity_rcvd);
      crypto_pk_free(identity_rcvd);
    }

    if (connection_or_client_learned_peer_id(chan->conn,
            chan->conn->handshake_state->authenticated_peer_id) < 0)
      ERR("Problem setting or checking peer id");

    log_info(LD_OR,
             "Got some good certificates from %s:%d: Authenticated it.",
             safe_str(chan->conn->_base.address), chan->conn->_base.port);

    chan->conn->handshake_state->id_cert = id_cert;
    id_cert = NULL;

    if (!public_server_mode(get_options())) {
      /* If we initiated the connection and we are not a public server, we
       * aren't planning to authenticate at all.  At this point we know who we
       * are talking to, so we can just send a netinfo now. */
      send_netinfo = 1;
    }
  } else {
    if (! (id_cert && auth_cert))
      ERR("The certs we wanted were missing");

    /* Remember these certificates so we can check an AUTHENTICATE cell */
    if (! tor_tls_cert_is_valid(LOG_PROTOCOL_WARN, auth_cert, id_cert, 1))
      ERR("The authentication certificate was not valid");
    if (! tor_tls_cert_is_valid(LOG_PROTOCOL_WARN, id_cert, id_cert, 1))
      ERR("The ID certificate was not valid");

    log_info(LD_OR,
             "Got some good certificates from %s:%d: "
             "Waiting for AUTHENTICATE.",
             safe_str(chan->conn->_base.address),
             chan->conn->_base.port);
    /* XXXX check more stuff? */

    chan->conn->handshake_state->id_cert = id_cert;
    chan->conn->handshake_state->auth_cert = auth_cert;
    id_cert = auth_cert = NULL;
  }

  chan->conn->handshake_state->received_certs_cell = 1;

  if (send_netinfo) {
    if (connection_or_send_netinfo(chan->conn) < 0) {
      log_warn(LD_OR, "Couldn't send netinfo cell");
      connection_mark_for_close(TO_CONN(chan->conn));
      channel_change_state(TLS_CHAN_TO_BASE(chan),
                           CHANNEL_STATE_ERROR);
      goto err;
    }
  }

 err:
  tor_cert_free(id_cert);
  tor_cert_free(link_cert);
  tor_cert_free(auth_cert);
#undef ERR
}

/** Process an AUTH_CHALLENGE cell from an OR connection.
 *
 * If we weren't supposed to get one (for example, because we're not the
 * originator of the connection), or it's ill-formed, or we aren't doing a v3
 * handshake, mark the connection.  If the cell is well-formed but we don't
 * want to authenticate, just drop it.  If the cell is well-formed *and* we
 * want to authenticate, send an AUTHENTICATE cell and then a NETINFO cell. */
static void
channel_tls_process_auth_challenge_cell(var_cell_t *cell, channel_tls_t *chan)
{
  int n_types, i, use_type = -1;
  uint8_t *cp;

  tor_assert(cell);
  tor_assert(chan);
  tor_assert(chan->conn);

#define ERR(s)                                                  \
  do {                                                          \
    log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,                      \
           "Received a bad AUTH_CHALLENGE cell from %s:%d: %s", \
           safe_str(chan->conn->_base.address),                 \
           chan->conn->_base.port, (s));                        \
    connection_mark_for_close(TO_CONN(chan->conn));             \
    channel_change_state(TLS_CHAN_TO_BASE(chan),                \
                         CHANNEL_STATE_ERROR);                  \
    return;                                                     \
  } while (0)

  if (chan->conn->_base.state != OR_CONN_STATE_OR_HANDSHAKING_V3)
    ERR("We're not currently doing a v3 handshake");
  if (chan->conn->link_proto < 3)
    ERR("We're not using link protocol >= 3");
  if (!(chan->conn->handshake_state->started_here))
    ERR("We didn't originate this connection");
  if (chan->conn->handshake_state->received_auth_challenge)
    ERR("We already received one");
  if (!(chan->conn->handshake_state->received_certs_cell))
    ERR("We haven't gotten a CERTS cell yet");
  if (cell->payload_len < OR_AUTH_CHALLENGE_LEN + 2)
    ERR("It was too short");
  if (cell->circ_id)
    ERR("It had a nonzero circuit ID");

  n_types = ntohs(get_uint16(cell->payload + OR_AUTH_CHALLENGE_LEN));
  if (cell->payload_len < OR_AUTH_CHALLENGE_LEN + 2 + 2*n_types)
    ERR("It looks truncated");

  /* Now see if there is an authentication type we can use */
  cp = cell->payload+OR_AUTH_CHALLENGE_LEN + 2;
  for (i = 0; i < n_types; ++i, cp += 2) {
    uint16_t authtype = ntohs(get_uint16(cp));
    if (authtype == AUTHTYPE_RSA_SHA256_TLSSECRET)
      use_type = authtype;
  }

  chan->conn->handshake_state->received_auth_challenge = 1;

  if (! public_server_mode(get_options())) {
    /* If we're not a public server then we don't want to authenticate on a
       connection we originated, and we already sent a NETINFO cell when we
       got the CERTS cell. We have nothing more to do. */
    return;
  }

  if (use_type >= 0) {
    log_info(LD_OR,
             "Got an AUTH_CHALLENGE cell from %s:%d: Sending "
             "authentication",
             safe_str(chan->conn->_base.address),
             chan->conn->_base.port);

    if (connection_or_send_authenticate_cell(chan->conn, use_type) < 0) {
      log_warn(LD_OR,
               "Couldn't send authenticate cell");
      connection_mark_for_close(TO_CONN(chan->conn));
      channel_change_state(TLS_CHAN_TO_BASE(chan),
                           CHANNEL_STATE_ERROR);
      return;
    }
  } else {
    log_info(LD_OR,
             "Got an AUTH_CHALLENGE cell from %s:%d, but we don't "
             "know any of its authentication types. Not authenticating.",
             safe_str(chan->conn->_base.address),
             chan->conn->_base.port);
  }

  if (connection_or_send_netinfo(chan->conn) < 0) {
    log_warn(LD_OR, "Couldn't send netinfo cell");
    connection_mark_for_close(TO_CONN(chan->conn));
    channel_change_state(TLS_CHAN_TO_BASE(chan),
                         CHANNEL_STATE_ERROR);

    return;
  }

#undef ERR
}

/** Process an AUTHENTICATE cell from a channel.
 *
 * If it's ill-formed or we weren't supposed to get one or we're not doing a
 * v3 handshake, then mark the connection.  If it does not authenticate the
 * other side of the connection successfully (because it isn't signed right,
 * we didn't get a CERTS cell, etc) mark the connection.  Otherwise, accept
 * the identity of the router on the other side of the connection.
 */
static void
channel_tls_process_authenticate_cell(var_cell_t *cell, channel_tls_t *chan)
{
  uint8_t expected[V3_AUTH_FIXED_PART_LEN];
  const uint8_t *auth;
  int authlen;

  tor_assert(cell);
  tor_assert(chan);
  tor_assert(chan->conn);

#define ERR(s)                                                  \
  do {                                                          \
    log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,                      \
           "Received a bad AUTHENTICATE cell from %s:%d: %s",   \
           safe_str(chan->conn->_base.address),                 \
           chan->conn->_base.port, (s));                        \
    connection_mark_for_close(TO_CONN(chan->conn));             \
    channel_change_state(TLS_CHAN_TO_BASE(chan),                \
                         CHANNEL_STATE_ERROR);                  \
    return;                                                     \
  } while (0)

  if (chan->conn->_base.state != OR_CONN_STATE_OR_HANDSHAKING_V3)
    ERR("We're not doing a v3 handshake");
  if (chan->conn->link_proto < 3)
    ERR("We're not using link protocol >= 3");
  if (chan->conn->handshake_state->started_here)
    ERR("We originated this connection");
  if (chan->conn->handshake_state->received_authenticate)
    ERR("We already got one!");
  if (chan->conn->handshake_state->authenticated) {
    /* Should be impossible given other checks */
    ERR("The peer is already authenticated");
  }
  if (!(chan->conn->handshake_state->received_certs_cell))
    ERR("We never got a certs cell");
  if (chan->conn->handshake_state->auth_cert == NULL)
    ERR("We never got an authentication certificate");
  if (chan->conn->handshake_state->id_cert == NULL)
    ERR("We never got an identity certificate");
  if (cell->payload_len < 4)
    ERR("Cell was way too short");

  auth = cell->payload;
  {
    uint16_t type = ntohs(get_uint16(auth));
    uint16_t len = ntohs(get_uint16(auth+2));
    if (4 + len > cell->payload_len)
      ERR("Authenticator was truncated");

    if (type != AUTHTYPE_RSA_SHA256_TLSSECRET)
      ERR("Authenticator type was not recognized");

    auth += 4;
    authlen = len;
  }

  if (authlen < V3_AUTH_BODY_LEN + 1)
    ERR("Authenticator was too short");

  if (connection_or_compute_authenticate_cell_body(
                        chan->conn, expected, sizeof(expected), NULL, 1) < 0)
    ERR("Couldn't compute expected AUTHENTICATE cell body");

  if (tor_memneq(expected, auth, sizeof(expected)))
    ERR("Some field in the AUTHENTICATE cell body was not as expected");

  {
    crypto_pk_t *pk = tor_tls_cert_get_key(
                                   chan->conn->handshake_state->auth_cert);
    char d[DIGEST256_LEN];
    char *signed_data;
    size_t keysize;
    int signed_len;

    if (!pk)
      ERR("Internal error: couldn't get RSA key from AUTH cert.");
    crypto_digest256(d, (char*)auth, V3_AUTH_BODY_LEN, DIGEST_SHA256);

    keysize = crypto_pk_keysize(pk);
    signed_data = tor_malloc(keysize);
    signed_len = crypto_pk_public_checksig(pk, signed_data, keysize,
                                           (char*)auth + V3_AUTH_BODY_LEN,
                                           authlen - V3_AUTH_BODY_LEN);
    crypto_pk_free(pk);
    if (signed_len < 0) {
      tor_free(signed_data);
      ERR("Signature wasn't valid");
    }
    if (signed_len < DIGEST256_LEN) {
      tor_free(signed_data);
      ERR("Not enough data was signed");
    }
    /* Note that we deliberately allow *more* than DIGEST256_LEN bytes here,
     * in case they're later used to hold a SHA3 digest or something. */
    if (tor_memneq(signed_data, d, DIGEST256_LEN)) {
      tor_free(signed_data);
      ERR("Signature did not match data to be signed.");
    }
    tor_free(signed_data);
  }

  /* Okay, we are authenticated. */
  chan->conn->handshake_state->received_authenticate = 1;
  chan->conn->handshake_state->authenticated = 1;
  chan->conn->handshake_state->digest_received_data = 0;
  {
    crypto_pk_t *identity_rcvd =
      tor_tls_cert_get_key(chan->conn->handshake_state->id_cert);
    const digests_t *id_digests =
      tor_cert_get_id_digests(chan->conn->handshake_state->id_cert);

    /* This must exist; we checked key type when reading the cert. */
    tor_assert(id_digests);

    memcpy(chan->conn->handshake_state->authenticated_peer_id,
           id_digests->d[DIGEST_SHA1], DIGEST_LEN);

    connection_or_set_circid_type(chan->conn, identity_rcvd);
    crypto_pk_free(identity_rcvd);

    connection_or_init_conn_from_address(chan->conn,
                  &(chan->conn->_base.addr),
                  chan->conn->_base.port,
                  (const char*)(chan->conn->handshake_state->
                    authenticated_peer_id),
                  0);

    log_info(LD_OR,
             "Got an AUTHENTICATE cell from %s:%d: Looks good.",
             safe_str(chan->conn->_base.address),
             chan->conn->_base.port);
  }

#undef ERR
}

