/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2005 Cluster File Systems, Inc.
 *   Author: Eric Barton <eeb@bartonsoftware.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define DEBUG_SUBSYSTEM S_NAL

#include <lnet/lib-lnet.h>
#include <lnet/ptllnd_wire.h>

#include <portals/p30.h>

#define LUSTRE_PORTALS_UNLINK_SEMANTICS

#define PTLLND_MSGS_PER_BUFFER     64
#define PTLLND_MSGS_SPARE          256
#define PTLLND_PEER_HASH_SIZE      101
#define PTLLND_EQ_SIZE             1024
#define PTLLND_NTX                 256

#ifdef PTL_MD_LUSTRE_COMPLETION_SEMANTICS
# define PTLLND_MD_OPTIONS        (PTL_MD_LUSTRE_COMPLETION_SEMANTICS |\
                                   PTL_MD_EVENT_START_DISABLE)
#else
# define PTLLND_MD_OPTIONS         PTL_MD_EVENT_START_DISABLE
#endif   
                                     
typedef struct
{
        int                        plni_portal;
        ptl_pid_t                  plni_pid;
        int                        plni_peer_credits;
        int                        plni_max_msg_size;
        int                        plni_buffer_size;
        int                        plni_msgs_spare;
        int                        plni_peer_hash_size;
        int                        plni_eq_size;

        __u64                      plni_stamp;
        struct list_head           plni_active_txs;
        struct list_head           plni_zombie_txs;
        int                        plni_ntxs;
        int                        plni_nrxs;

        ptl_handle_ni_t            plni_nih;
        ptl_handle_eq_t            plni_eqh;

        struct list_head          *plni_peer_hash;
        int                        plni_npeers;

        struct list_head           plni_buffers;
        int                        plni_nbuffers;
        int                        plni_nposted_buffers;
} ptllnd_ni_t;

#define PTLLND_CREDIT_HIGHWATER(plni) ((plni)->plni_peer_credits - 1)

typedef struct
{
        struct list_head           plp_list;
        lnet_ni_t                 *plp_ni;
        lnet_nid_t                 plp_nid;
        ptl_process_id_t           plp_ptlid;
        int                        plp_credits;
        int                        plp_max_credits;
        int                        plp_outstanding_credits;
        int                        plp_max_msg_size;
        int                        plp_refcount;
        int                        plp_recvd_hello:1;
        int                        plp_closing:1;
        __u64                      plp_match;
        __u64                      plp_stamp;
        __u64                      plp_seq;
        struct list_head           plp_txq;
} ptllnd_peer_t;

typedef struct
{
        struct list_head           plb_list;
        lnet_ni_t                 *plb_ni;
        int                        plb_posted;
        int                        plb_refcount;
        ptl_handle_md_t            plb_md;
        char                      *plb_buffer;
} ptllnd_buffer_t;

typedef struct
{
        ptllnd_peer_t             *rx_peer;
        kptl_msg_t                *rx_msg;
        int                        rx_nob;
        int                        rx_replied;
        char                       rx_space[0];
} ptllnd_rx_t;

typedef struct
{
        struct list_head           tx_list;
        int                        tx_type;
        int                        tx_status;
        ptllnd_peer_t             *tx_peer;
        lnet_msg_t                *tx_lnetmsg;
        lnet_msg_t                *tx_lnetreplymsg;
        unsigned int               tx_niov;
        ptl_md_iovec_t            *tx_iov;
        ptl_handle_md_t            tx_bulkmdh;
        ptl_handle_md_t            tx_reqmdh;
        int                        tx_completing; /* someone already completing */
        int                        tx_msgsize;  /* # bytes in tx_msg */
        kptl_msg_t                 tx_msg;      /* message to send */
} ptllnd_tx_t;

#define PTLLND_RDMA_WRITE           0x100       /* pseudo message type */
#define PTLLND_RDMA_READ            0x101       /* (no msg actually sent) */

/* Hack to extract object type from event's user_ptr relies on (and checks)
 * that structs are somewhat aligned. */
#define PTLLND_EVENTARG_TYPE_TX     0x1
#define PTLLND_EVENTARG_TYPE_BUF    0x2
#define PTLLND_EVENTARG_TYPE_MASK   0x3

static inline void *
ptllnd_obj2eventarg (void *obj, int type) 
{
        unsigned long ptr = (unsigned long)obj;
        
        LASSERT ((ptr & PTLLND_EVENTARG_TYPE_MASK) == 0);
        LASSERT ((type & ~PTLLND_EVENTARG_TYPE_MASK) == 0);
        
        return (void *)(ptr | type);
}

static inline int
ptllnd_eventarg2type (void *arg) 
{
        unsigned long ptr = (unsigned long)arg;
        
        return (ptr & PTLLND_EVENTARG_TYPE_MASK);
}

static inline void *
ptllnd_eventarg2obj (void *arg) 
{
        unsigned long ptr = (unsigned long)arg;
        
        return (void *)(ptr & ~PTLLND_EVENTARG_TYPE_MASK);
}

int ptllnd_startup(lnet_ni_t *ni);
void ptllnd_shutdown(lnet_ni_t *ni);
int ptllnd_send(lnet_ni_t *ni, void *private, lnet_msg_t *msg);
int ptllnd_recv(lnet_ni_t *ni, void *private, lnet_msg_t *msg,
                int delayed, unsigned int niov, 
                struct iovec *iov, lnet_kiov_t *kiov,
                unsigned int offset, unsigned int mlen, unsigned int rlen);
int ptllnd_eager_recv(lnet_ni_t *ni, void *private, lnet_msg_t *msg,
                      void **new_privatep);
ptllnd_tx_t *ptllnd_new_tx(ptllnd_peer_t *peer, int type, int payload_nob);
void ptllnd_wait(lnet_ni_t *ni, int milliseconds);
void ptllnd_check_sends(ptllnd_peer_t *peer);
void ptllnd_destroy_peer(ptllnd_peer_t *peer);
void ptllnd_abort_txs(lnet_ni_t *ni);
void ptllnd_close_peer(ptllnd_peer_t *peer);
int ptllnd_post_buffer(ptllnd_buffer_t *buf);
int ptllnd_grow_buffers (lnet_ni_t *ni);

static inline void
ptllnd_peer_addref (ptllnd_peer_t *peer) 
{
        LASSERT (peer->plp_refcount > 0);
        peer->plp_refcount++;
}

static inline void
ptllnd_peer_decref (ptllnd_peer_t *peer)
{
        LASSERT (peer->plp_refcount > 0);
        peer->plp_refcount--;
        if (peer->plp_refcount == 0)
                ptllnd_destroy_peer(peer);
}

static inline void
ptllnd_post_tx(ptllnd_tx_t *tx)
{
        ptllnd_peer_t *peer = tx->tx_peer;

        list_add_tail(&tx->tx_list, &peer->plp_txq);
        ptllnd_check_sends(peer);
}

