/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2004 Cluster File Systems, Inc.
 *   Author: Eric Barton <eric@bartonsoftware.com>
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

#include "ranal.h"

int
kranal_dist(lib_nal_t *nal, ptl_nid_t nid, unsigned long *dist)
{
        /* I would guess that if kranal_get_peer (nid) == NULL,
           and we're not routing, then 'nid' is very distant :) */
        if ( nal->libnal_ni.ni_pid.nid == nid ) {
                *dist = 0;
        } else {
                *dist = 1;
        }

        return 0;
}

void
kranal_device_callback(RAP_INT32 devid)
{
        kra_device_t *dev;
        int           i;
        
        for (i = 0; i < kranal_data.kra_ndevs; i++) {

                dev = &kranal_data.kra_devices[i];
                if (dev->rad_id != devid)
                        continue;

                spin_lock_irqsave(&dev->rad_lock, flags);

                if (!dev->rad_ready) {
                        dev->rad_ready = 1;
                        wake_up(&dev->rad_waitq);
                }

                spin_unlock_irqrestore(&dev->rad_lock, flags);
                return;
        }
        
        CWARN("callback for unknown device %d\n", devid);
}

void
kranal_schedule_conn(kra_conn_t *conn)
{
        kra_device_t    *dev = conn->rac_device;
        unsigned long    flags;
        
        spin_lock_irqsave(&dev->rad_lock, flags);
        
        if (!conn->rac_scheduled) {
                kranal_conn_addref(conn);       /* +1 ref for scheduler */
                conn->rac_scheduled = 1;
                list_add_tail(&conn->rac_schedlist, &dev->rad_connq);
                wake_up(&dev->rad_waitq);
        }

        spin_unlock_irqrestore(&dev->rad_lock, flags);
}

void
kranal_schedule_cqid (__u32 cqid)
{
        kra_conn_t         *conn;
        struct list_head   *conns;
        struct list_head   *tmp;

        conns = kranal_cqid2connlist(cqid);

        read_lock(&kranal_data.kra_global_lock);

        conn = kranal_cqid2conn_locked(cqid);
        
        if (conn == NULL)
                CWARN("no cqid %x\n", cqid);
        else
                kranal_schedule_conn(conn);
        
        read_unlock(&kranal_data.kra_global_lock);
}

void
kranal_schedule_dev(kra_device_t *dev)
{
        kra_conn_t         *conn;
        struct list_head   *conns;
        struct list_head   *tmp;
        int                 i;

        /* Don't do this in IRQ context (servers may have 1000s of clients) */
        LASSERT (!in_interrupt()); 

        CWARN("Scheduling ALL conns on device %d\n", dev->rad_id);

        for (i = 0; i < kranal_data.kra_conn_hash_size; i++) {

                /* Drop the lock on each hash bucket to ensure we don't
                 * block anyone for too long at IRQ priority on another CPU */
                
                read_lock(&kranal_data.kra_global_lock);
        
                conns = &kranal_data.kra_conns[i];

                list_for_each (tmp, conns) {
                        conn = list_entry(tmp, kra_conn_t, rac_hashlist);
                
                        if (conn->rac_device == dev)
                                kranal_schedule_conn(conn);
                }
                read_unlock(&kranal_data.kra_global_lock);
        }
}

void
kranal_tx_done (kra_tx_t *tx, int completion)
{
        ptl_err_t        ptlrc = (completion == 0) ? PTL_OK : PTL_FAIL;
        kra_device_t    *dev;
        unsigned long    flags;
        int              i;
        RAP_RETURN       rrc;

        LASSERT (!in_interrupt());

        switch (tx->tx_buftype) {
        default:
                LBUG();

        case RANAL_BUF_NONE:
        case RANAL_BUF_IMMEDIATE:
        case RANAL_BUF_PHYS_UNMAPPED:
        case RANAL_BUF_VIRT_UNMAPPED:
                break;

        case RANAL_BUF_PHYS_MAPPED:
                LASSERT (tx->tx_conn != NULL);
                dev = tx->tx_con->rac_device;
                rrc = RapkDeregisterMemory(dev->rad_handle, NULL,
                                           dev->rad_ptag, &tx->tx_map_key);
                LASSERT (rrc == RAP_SUCCESS);
                break;

        case RANAL_BUF_VIRT_MAPPED:
                LASSERT (tx->tx_conn != NULL);
                dev = tx->tx_con->rac_device;
                rrc = RapkDeregisterMemory(dev->rad_handle, tx->tx_buffer
                                           dev->rad_ptag, &tx->tx_map_key);
                LASSERT (rrc == RAP_SUCCESS);
                break;
        }

        for (i = 0; i < 2; i++) {
                /* tx may have up to 2 libmsgs to finalise */
                if (tx->tx_libmsg[i] == NULL)
                        continue;

                lib_finalize(&kranal_lib, NULL, tx->tx_libmsg[i], ptlrc);
                tx->tx_libmsg[i] = NULL;
        }

        tx->tx_buftype = RANAL_BUF_NONE;
        tx->tx_msg.ram_type = RANAL_MSG_NONE;
        tx->tx_conn = NULL;

        spin_lock_irqsave(&kranal_data.kra_tx_lock, flags);

        if (tx->tx_isnblk) {
                list_add_tail(&tx->tx_list, &kranal_data.kra_idle_nblk_txs);
        } else {
                list_add_tail(&tx->tx_list, &kranal_data.kra_idle_txs);
                wake_up(&kranal_data.kra_idle_tx_waitq);
        }

        spin_unlock_irqrestore(&kranal_data.kra_tx_lock, flags);
}

kra_tx_t *
kranal_get_idle_tx (int may_block) 
{
        unsigned long  flags;
        kra_tx_t      *tx = NULL;
        
        for (;;) {
                spin_lock_irqsave(&kranal_data.kra_tx_lock, flags);

                /* "normal" descriptor is free */
                if (!list_empty(&kranal_data.kra_idle_txs)) {
                        tx = list_entry(kranal_data.kra_idle_txs.next,
					kra_tx_t, tx_list);
                        break;
                }

                if (!may_block) {
                        /* may dip into reserve pool */
                        if (list_empty(&kranal_data.kra_idle_nblk_txs)) {
                                CERROR("reserved tx desc pool exhausted\n");
                                break;
                        }

                        tx = list_entry(kranal_data.kra_idle_nblk_txs.next,
					kra_tx_t, tx_list);
                        break;
                }

                /* block for idle tx */
                spin_unlock_irqrestore(&kranal_data.kra_tx_lock, flags);

                wait_event(kranal_data.kra_idle_tx_waitq,
			   !list_empty(&kranal_data.kra_idle_txs));
        }

        if (tx != NULL) {
                list_del(&tx->tx_list);

                /* Allocate a new completion cookie.  It might not be
                 * needed, but we've got a lock right now... */
                tx->tx_cookie = kranal_data.kra_next_tx_cookie++;

                LASSERT (tx->tx_buftype == RANAL_BUF_NONE);
                LASSERT (tx->tx_msg.ram_type == RANAL_MSG_NONE);
                LASSERT (tx->tx_conn == NULL);
                LASSERT (tx->tx_libmsg[0] == NULL);
                LASSERT (tx->tx_libmsg[1] == NULL);
        }

        spin_unlock_irqrestore(&kranal_data.kra_tx_lock, flags);
        
        return tx;
}

void
kranal_init_msg(kra_msg_t *msg, int type)
{
        msg->ram_magic = RANAL_MSG_MAGIC;
        msg->ram_version = RANAL_MSG_VERSION;
        msg->ram_type = type;
        msg->ram_srcnid = kranal_lib.libnal_ni.ni_pid.nid;
        /* ram_incarnation gets set when FMA is sent */
}

kra_tx_t
kranal_new_tx_msg (int may_block, int type)
{
        kra_tx_t *tx = kranal_get_idle_tx(may_block);

        if (tx == NULL)
                return NULL;

        kranal_init_msg(&tx->tx_msg, type);
        return tx;
}

int
kranal_setup_immediate_buffer (kra_tx_t *tx, int niov, struct iovec *iov, 
                               int offset, int nob)
                 
{
        LASSERT (nob > 0);
        LASSERT (niov > 0);
        LASSERT (tx->tx_buftype == RANAL_BUF_NONE);

        while (offset >= iov->iov_len) {
                offset -= iov->iov_len;
                niov--;
                iov++;
                LASSERT (niov > 0);
        }

        if (nob > iov->iov_len - offset) {
                CERROR("Can't handle multiple vaddr fragments\n");
                return -EMSGSIZE;
        }

        tx->tx_bufftype = RANAL_BUF_IMMEDIATE;
        tx->tx_nob = nob;
        tx->tx_buffer = (void *)(((unsigned long)iov->iov_base) + offset);
        return 0;
}

int
kranal_setup_virt_buffer (kra_tx_t *tx, int niov, struct iovec *iov, 
                          int offset, int nob)
                 
{
        LASSERT (nob > 0);
        LASSERT (niov > 0);
        LASSERT (tx->tx_buftype == RANAL_BUF_NONE);

        while (offset >= iov->iov_len) {
                offset -= iov->iov_len;
                niov--;
                iov++;
                LASSERT (niov > 0);
        }

        if (nob > iov->iov_len - offset) {
                CERROR("Can't handle multiple vaddr fragments\n");
                return -EMSGSIZE;
        }

        tx->tx_bufftype = RANAL_BUF_VIRT_UNMAPPED;
        tx->tx_nob = nob;
        tx->tx_buffer = (void *)(((unsigned long)iov->iov_base) + offset);
        return 0;
}

int
kranal_setup_phys_buffer (kra_tx_t *tx, int nkiov, ptl_kiov_t *kiov,
                          int offset, int nob)
{
        RAP_PHYS_REGION *phys = tx->tx_phys;
        int              resid;

        CDEBUG(D_NET, "niov %d offset %d nob %d\n", nkiov, offset, nob);

        LASSERT (nob > 0);
        LASSERT (nkiov > 0);
        LASSERT (tx->tx_buftype == RANAL_BUF_NONE);

        while (offset >= kiov->kiov_len) {
                offset -= kiov->kiov_len;
                nkiov--;
                kiov++;
                LASSERT (nkiov > 0);
        }

        tx->tx_bufftype = RANAL_BUF_PHYS_UNMAPPED;
        tx->tx_nob = nob;
        tx->tx_buffer = NULL;
        tx->tx_phys_offset = kiov->kiov_offset + offset;
        
        phys->Address = kranal_page2phys(kiov->kiov_page);
        phys->Length  = PAGE_SIZE;
        phys++;

        resid = nob - (kiov->kiov_len - offset);
        while (resid > 0) {
                kiov++;
                nkiov--;
                LASSERT (nkiov > 0);

                if (kiov->kiov_offset != 0 ||
                    ((resid > PAGE_SIZE) && 
                     kiov->kiov_len < PAGE_SIZE)) {
                        int i;
                        /* Can't have gaps */
                        CERROR("Can't make payload contiguous in I/O VM:"
                               "page %d, offset %d, len %d \n", nphys, 
                               kiov->kiov_offset, kiov->kiov_len);

                        for (i = -nphys; i < nkiov; i++) {
                                CERROR("kiov[%d] %p +%d for %d\n",
                                       i, kiov[i].kiov_page, 
                                       kiov[i].kiov_offset, kiov[i].kiov_len);
                        }
                        
                        return -EINVAL;
                }

                if ((phys - tx->tx_phys) == PTL_MD_MAX_IOV) {
                        CERROR ("payload too big (%d)\n", phys - tx->tx_phys);
                        return -EMSGSIZE;
                }

                phys->Address = kranal_page2phys(kiov->kiov_page);
                phys->Length  = PAGE_SIZE;
                phys++;

                resid -= PAGE_SIZE;
        }

        tx->tx_phys_npages = phys - tx->tx_phys;
        return 0;
}

static inline int
kranal_setup_buffer (kra_tx_t *tx, int niov, 
                     struct iovec *iov, ptl_kiov_t *kiov,
                     int offset, int nob)
{
        LASSERT ((iov == NULL) != (kiov == NULL));
        
        if (kiov != NULL)
                return kranal_setup_phys_buffer(tx, niov, kiov, offset, nob);
        
        return kranal_setup_virt_buffer(tx, niov, kiov, offset, nob);
}

void
kranal_map_buffer (kra_tx_t *tx)
{
        kra_conn_t     *conn = tx->tx_conn;
        kra_device_t   *dev = conn->rac_device;

        switch (tx->tx_buftype) {
        default:
                
        case RANAL_BUF_PHYS_UNMAPPED:
                rrc = RapkRegisterPhys(conn->rac_device->rad_handle,
                                       tx->tx_phys, tx->tx_phys_npages,
                                       conn->rac_device->rad_ptag,
                                       &tx->tx_map_key);
                LASSERT (rrc == RAP_SUCCESS);
                tx->tx_buftype = RANAL_BUF_PHYS_MAPPED;
                return;

        case RANAL_BUF_VIRT_UNMAPPED:
                rrc = RapkRegisterMemory(conn->rac_device->rad_handle,
                                         tx->tx_buffer, tx->tx_nob,
                                         conn->rac_device->rad_ptag,
                                         &tx->tx_map_key);
                LASSERT (rrc == RAP_SUCCESS);
                tx->tx_buftype = RANAL_BUF_VIRT_MAPPED;
                return;
        }
}

kra_conn_t *
kranal_find_conn_locked (kra_peer_t *peer)
{
        struct list_head *tmp;

        /* just return the first connection */
        list_for_each (tmp, &peer->rap_conns) {
                return list_entry(tmp, kra_conn_t, rac_list);
        }

        return NULL;
}

void
kranal_post_fma (kra_conn_t *conn, kra_tx_t *tx)
{
        unsigned long    flags;

        tx->tx_conn = conn;

        spin_lock_irqsave(&conn->rac_lock, flags);
        list_add_tail(&tx->tx_list, &conn->rac_fmaq);
        tx->tx_qtime = jiffies;
        spin_unlock_irqrestore(&conn->rac_lock, flags);

        kranal_schedule_conn(conn);
}

void
kranal_launch_tx (kra_tx_t *tx, ptl_nid_t nid)
{
        unsigned long    flags;
        kra_peer_t      *peer;
        kra_conn_t      *conn;
        unsigned long    now;
        rwlock_t        *g_lock = &kranal_data.kra_global_lock;

        /* If I get here, I've committed to send, so I complete the tx with
         * failure on any problems */
        
        LASSERT (tx->tx_conn == NULL);          /* only set when assigned a conn */

        read_lock(g_lock);
        
        peer = kranal_find_peer_locked(nid);
        if (peer == NULL) {
                read_unlock(g_lock);
                kranal_tx_done(tx, -EHOSTUNREACH);
                return;
        }

        conn = kranal_find_conn_locked(peer);
        if (conn != NULL) {
                kranal_post_fma(conn, tx);
                read_unlock(g_lock);
                return;
        }
        
        /* Making one or more connections; I'll need a write lock... */
        read_unlock(g_lock);
        write_lock_irqsave(g_lock, flags);

        peer = kranal_find_peer_locked(nid);
        if (peer == NULL) {
                write_unlock_irqrestore(g_lock, flags);
                kranal_tx_done(tx -EHOSTUNREACH);
                return;
        }

        conn = kranal_find_conn_locked(peer);
        if (conn != NULL) {
                /* Connection exists; queue message on it */
                kranal_post_fma(conn, tx);
                write_unlock_irqrestore(g_lock, flags);
                return;
        }

        LASSERT (peer->rap_persistence > 0);

        if (!peer->rap_connecting) {
                now = CURRENT_TIME;
                if (now < peer->rap_reconnect_time) {
                        write_unlock_irqrestore(g_lock, flags);
                        kranal_tx_done(tx, -EHOSTUNREACH);
                        return;
                }
        
                peer->rap_connecting = 1;
                kranal_peer_addref(peer); /* extra ref for connd */
        
                spin_lock(&kranal_data.kra_connd_lock);
        
                list_add_tail(&peer->rap_connd_list,
			      &kranal_data.kra_connd_peers);
                wake_up(&kranal_data.kra_connd_waitq);
        
                spin_unlock(&kranal_data.kra_connd_lock);
        }
        
        /* A connection is being established; queue the message... */
        list_add_tail(&tx->tx_list, &peer->rap_tx_queue);

        write_unlock_irqrestore(g_lock, flags);
}

static void
kranal_rdma(kra_tx_t *tx, int type, 
            kra_rdma_desc_t *rard, int nob, __u64 cookie)
{
        kra_conn_t *conn = tx->tx_conn;
        RAP_RETURN  rrc;

        /* prep final completion message */
        kranal_init_msg(&tx->tx_msg, type);
        tx->tx_msg.ram_u.completion.racm_cookie = cookie;
        
        LASSERT (tx->tx_buftype == RANAL_BUF_PHYS_MAPPED ||
                 tx->tx_buftype == RANAL_BUF_VIRT_MAPPED);
        LASSERT (nob <= rard->rard_nob);

        memset(&tx->tx_rdma_desc, 0, sizeof(tx->tx_rdma_desc));
        tx->tx_rdma_desc.SrcPtr = tx->tx_buffer;
        tx->tx_rdma_desc.SrcKey = tx->tx_map_key;
        tx->tx_rdma_desc.DstPtr = rard->rard_addr;
        tx->tx_rdma_desc.DstKey = rard->rard_key;
        tx->tx_rdma_desc.Length = nob;
        tx->tx_rdma_desc.AppPtr = tx;

        if (nob == 0) { /* Immediate completion */
                kranal_post_fma(conn, tx);
                return;
        }
        
        rrc = RapkPostRdma(conn->rac_rihandle, &tx->tx_rdma_desc);
        LASSERT (rrc == RAP_SUCCESS);

        spin_lock_irqsave(&conn->rac_lock, flags);
        list_add_tail(&tx->tx_list, &conn->rac_rdmaq);
        tx->tx_qtime = jiffies;
        spin_unlock_irqrestore(&conn->rac_lock, flags);
}

int
kranal_consume_rxmsg (kra_conn_t *conn, void *buffer, int nob)
{
        __u32      nob_received = nob;
        RAP_RETURN rrc;

        LASSERT (conn->rac_rxmsg != NULL);

        rrc = RapkFmaCopyToUser(conn->rac_rihandle, buffer,
                                &nob_received, sizeof(kra_msg_t));
        LASSERT (rrc == RAP_SUCCESS);

        conn->rac_rxmsg = NULL;

        if (nob_received != nob) {
                CWARN("Expected %d immediate bytes but got %d\n",
                      nob, nob_received);
                return -EPROTO;
        }
        
        return 0;
}

ptl_err_t
kranal_do_send (lib_nal_t    *nal, 
                void         *private,
                lib_msg_t    *libmsg,
                ptl_hdr_t    *hdr, 
                int           type, 
                ptl_nid_t     nid, 
                ptl_pid_t     pid,
                unsigned int  niov, 
                struct iovec *iov, 
                ptl_kiov_t   *kiov,
                size_t        offset,
                size_t        nob)
{
        kra_conn_t *conn;
        kra_tx_t   *tx;

        /* NB 'private' is different depending on what we're sending.... */

        CDEBUG(D_NET, "sending "LPSZ" bytes in %d frags to nid:"LPX64
               " pid %d\n", nob, niov, nid , pid);

        LASSERT (nob == 0 || niov > 0);
        LASSERT (niov <= PTL_MD_MAX_IOV);

        LASSERT (!in_interrupt());
        /* payload is either all vaddrs or all pages */
        LASSERT (!(kiov != NULL && iov != NULL));

        switch(type) {
        default:
                LBUG();
                
        case PTL_MSG_REPLY: {
                /* reply's 'private' is the conn that received the GET_REQ */
                conn = private;
                LASSERT (conn->rac_rxmsg != NULL);

                if (conn->rac_rxmsg->ram_type == RANAL_MSG_IMMEDIATE) {
                        if (nob > RANAL_MAX_IMMEDIATE) {
                                CERROR("Can't REPLY IMMEDIATE %d to "LPX64"\n",
                                       nob, nid);
                                return PTL_FAIL;
                        }
                        break;                  /* RDMA not expected */
                }
                
                /* Incoming message consistent with immediate reply? */
                if (conn->rac_rxmsg->ram_type != RANAL_MSG_GET_REQ) {
                        CERROR("REPLY to "LPX64" bad msg type %x!!!\n",
			       nid, conn->rac_rxmsg->ram_type);
                        return PTL_FAIL;
                }

                tx = kranal_get_idle_tx(0);
                if (tx == NULL)
                        return PTL_FAIL;

                rc = kranal_setup_buffer(tx, niov, iov, kiov, offset, nob);
                if (rc != 0) {
                        kranal_tx_done(tx, rc);
                        return PTL_FAIL;
                }

                tx->tx_conn = conn;
                tx->tx_libmsg[0] = libmsg;

                kranal_map_buffer(tx);
                kranal_rdma(tx, RANAL_MSG_GET_DONE,
                            &conn->rac_rxmsg->ram_u.getreq.ragm_desc, nob,
                            &conn->rac_rxmsg->ram_u.getreq.ragm_cookie);
                return PTL_OK;
        }

        case PTL_MSG_GET:
                if (kiov == NULL &&             /* not paged */
                    nob <= RANAL_MAX_IMMEDIATE && /* small enough */
                    nob <= kranal_tunables.kra_max_immediate)
                        break;                  /* send IMMEDIATE */

                tx = kranal_new_tx_msg(0, RANAL_MSG_GET_REQ);
                if (tx == NULL)
                        return PTL_NO_SPACE;

                rc = kranal_setup_buffer(tx, niov, iov, kiov, offset, nob);
                if (rc != 0) {
                        kranal_tx_done(tx, rc);
                        return PTL_FAIL;
                }

                tx->tx_libmsg[1] = lib_create_reply_msg(&kranal_lib, nid, libmsg);
                if (tx->tx_libmsg[1] == NULL) {
                        CERROR("Can't create reply for GET to "LPX64"\n", nid);
                        kranal_tx_done(tx, rc);
                        return PTL_FAIL;
                }

                tx->tx_libmsg[0] = libmsg;
                tx->tx_msg.ram_u.get.ragm_hdr = *hdr;
                /* rest of tx_msg is setup just before it is sent */
                kranal_launch_tx(tx, nid);
                return PTL_OK

        case PTL_MSG_ACK:
                LASSERT (nob == 0);
                break;

        case PTL_MSG_PUT:
                if (kiov == NULL &&             /* not paged */
                    nob <= RANAL_MAX_IMMEDIATE && /* small enough */
                    nob <= kranal_tunables.kra_max_immediate)
                        break;                  /* send IMMEDIATE */
                
                tx = kranal_new_tx_msg(!in_interrupt(), RANA_MSG_PUT_REQ);
                if (tx == NULL)
                        return PTL_NO_SPACE;

                rc = kranal_setup_buffer(tx, niov, iov, kiov, offset, nob);
                if (rc != 0) {
                        kranal_tx_done(tx, rc);
                        return PTL_FAIL;
                }

                tx->tx_libmsg[0] = libmsg;
                tx->tx_msg.ram_u.putreq.raprm_hdr = *hdr;
                /* rest of tx_msg is setup just before it is sent */
                kranal_launch_tx(tx, nid);
                return PTL_OK;
        }

        LASSERT (kiov == NULL);
        LASSERT (nob <= RANAL_MAX_IMMEDIATE);

        tx = kranal_new_tx_msg(!(type == PTL_MSG_ACK ||
                                 type == PTL_MSG_REPLY ||
                                 in_interrupt()), 
                               RANAL_MSG_IMMEDIATE);
        if (tx == NULL)
                return PTL_NO_SPACE;

        rc = kranal_setup_immediate_buffer(tx, niov, iov, offset, nob);
        if (rc != 0) {
                kranal_tx_done(tx, rc);
                return PTL_FAIL;
        }
                
        tx->tx_msg.ram_u.immediate.raim_hdr = *hdr;
        tx->tx_libmsg[0] = libmsg;
        kranal_launch_tx(tx, nid);
        return PTL_OK;
}

ptl_err_t
kranal_send (lib_nal_t *nal, void *private, lib_msg_t *cookie,
	     ptl_hdr_t *hdr, int type, ptl_nid_t nid, ptl_pid_t pid,
	     unsigned int niov, struct iovec *iov,
	     size_t offset, size_t len)
{
        return kranal_do_send(nal, private, cookie,
			      hdr, type, nid, pid,
			      niov, iov, NULL,
			      offset, len);
}

ptl_err_t
kranal_send_pages (lib_nal_t *nal, void *private, lib_msg_t *cookie, 
		   ptl_hdr_t *hdr, int type, ptl_nid_t nid, ptl_pid_t pid,
		   unsigned int niov, ptl_kiov_t *kiov, 
		   size_t offset, size_t len)
{
        return kranal_do_send(nal, private, cookie,
			      hdr, type, nid, pid,
			      niov, NULL, kiov,
			      offset, len);
}

ptl_err_t
kranal_recvmsg (lib_nal_t *nal, void *private, lib_msg_t *libmsg,
		unsigned int niov, struct iovec *iov, ptl_kiov_t *kiov,
		size_t offset, size_t mlen, size_t rlen)
{
        kra_conn_t  *conn = private;
        kra_msg_t   *rxmsg = conn->rac_rxmsg;
        void        *buffer;
        int          rc;
        
        LASSERT (mlen <= rlen);
        LASSERT (!in_interrupt());
        /* Either all pages or all vaddrs */
        LASSERT (!(kiov != NULL && iov != NULL));

        switch(rxmsg->ram_type) {
        default:
                LBUG();
                return PTL_FAIL;
                
        case RANAL_MSG_IMMEDIATE:
                if (mlen == 0) {
                        buffer = NULL;
                } else if (kiov != NULL) {
                        CERROR("Can't recv immediate into paged buffer\n");
                        return PTL_FAIL;
                } else {
                        LASSERT (niov > 0);
                        while (offset >= iov->iov_len) {
                                offset -= iov->iov_len;
                                iov++;
                                niov--;
                                LASSERT (niov > 0);
                        }
                        if (mlen > iov->iov_len - offset) {
                                CERROR("Can't handle immediate frags\n");
                                return PTL_FAIL;
                        }
                        buffer = ((char *)iov->iov_base) + offset;
                }
                rc = kranal_consume_rxmsg(conn, buffer, mlen);
                lib_finalize(nal, NULL, libmsg, (rc == 0) ? PTL_OK : PTL_FAIL);
                return PTL_OK;

        case RANAL_MSG_GET_REQ:
                /* If the GET matched, we've already handled it in
                 * kranal_do_send which is called to send the REPLY.  We're
                 * only called here to complete the GET receive (if we needed
                 * it which we don't, but I digress...) */
                LASSERT (libmsg == NULL);
                lib_finalize(nal, NULL, libmsg, PTL_OK);
                return PTL_OK;

        case RANAL_MSG_PUT_REQ:
                if (libmsg == NULL) {           /* PUT didn't match... */
                        lib_finalize(null, NULL, libmsg, PTL_OK);
                        return PTL_OK;
                }
                
                tx = kranal_new_tx_msg(0, RANAL_MSG_PUT_ACK);
                if (tx == NULL)
                        return PTL_NO_SPACE;

                rc = kranal_setup_buffer(tx, niov, iov, kiov, offset, mlen);
                if (rc != 0) {
                        kranal_tx_done(tx, rc);
                        return PTL_FAIL;
                }

                kranal_map_buffer(tx);
                
                tx->tx_msg.ram_u.putack.rapam_src_cookie = 
                        conn->rac_rxmsg->ram_u.putreq.raprm_cookie;
                tx->tx_msg.ram_u.putack.rapam_dst_cookie = tx->tx_cookie;
                tx->tx_msg.ram_u.putack.rapam_dst.desc.rard_key = tx->tx_map_key;
                tx->tx_msg.ram_u.putack.rapam_dst.desc.rard_addr = tx->tx_buffer;
                tx->tx_msg.ram_u.putack.rapam_dst.desc.rard_nob = mlen;

                tx->tx_libmsg[0] = libmsg; /* finalize this on RDMA_DONE */

                kranal_post_fma(conn, tx);
                
                /* flag matched by consuming rx message */
                kranal_consume_rxmsg(conn, NULL, 0);
                return PTL_OK;
        }
}

ptl_err_t
kranal_recv (lib_nal_t *nal, void *private, lib_msg_t *msg,
	     unsigned int niov, struct iovec *iov, 
	     size_t offset, size_t mlen, size_t rlen)
{
        return kranal_recvmsg(nal, private, msg, niov, iov, NULL,
			      offset, mlen, rlen);
}

ptl_err_t
kranal_recv_pages (lib_nal_t *nal, void *private, lib_msg_t *msg,
		   unsigned int niov, ptl_kiov_t *kiov, 
		   size_t offset, size_t mlen, size_t rlen)
{
        return kranal_recvmsg(nal, private, msg, niov, NULL, kiov,
			      offset, mlen, rlen);
}

int
kranal_thread_start (int(*fn)(void *arg), void *arg)
{
        long    pid = kernel_thread(fn, arg, 0);

        if (pid < 0)
                return(int)pid;

        atomic_inc(&kranal_data.kra_nthreads);
        return 0;
}

void
kranal_thread_fini (void)
{
        atomic_dec(&kranal_data.kra_nthreads);
}

int
kranal_check_conn (kra_conn_t *conn)
{
        kra_tx_t          *tx;
        struct list_head  *ttmp;
        unsigned long      flags;
        long               timeout;
        unsigned long      now = jiffies;

        if (!conn->rac_closing &&
            time_after_eq(now, conn->rac_last_sent + conn->rac_keepalive * HZ)) {
                /* not sent in a while; schedule conn so scheduler sends a keepalive */
                kranal_schedule_conn(conn);
        }

        /* wait twice as long for CLOSE to be sure peer is dead */
        timeout = (conn->rac_closing ? 1 : 2) * conn->rac_timeout * HZ;

        if (!conn->rac_close_recvd &&
            time_after_eq(now, conn->rac_last_rx + timeout)) {
                CERROR("Nothing received from "LPX64" within %d seconds\n",
                       conn->rac_peer->rap_nid, (now - conn->rac_last_rx)/HZ);
                return -ETIMEDOUT;
        }

        if (conn->rac_closing)
                return 0;
        
        /* Check the conn's queues are moving.  These are "belt+braces" checks,
         * in case of hardware/software errors that make this conn seem
         * responsive even though it isn't progressing its message queues. */

        spin_lock_irqsave(&conn->rac_lock, flags);

        list_for_each (ttmp, &conn->rac_fmaq) {
                tx = list_entry(ttmp, kra_tx_t, tx_list);
                
                if (time_after_eq(now, tx->tx_qtime + timeout)) {
                        spin_unlock_irqrestore(&conn->rac_lock, flags);
                        CERROR("tx on fmaq for "LPX64" blocked %d seconds\n",
                               conn->rac_perr->rap_nid, (now - tx->tx_qtime)/HZ);
                        return -ETIMEDOUT;
                }
        }
        
        list_for_each (ttmp, &conn->rac_rdmaq) {
                tx = list_entry(ttmp, kra_tx_t, tx_list);
                
                if (time_after_eq(now, tx->tx_qtime + timeout)) {
                        spin_unlock_irqrestore(&conn->rac_lock, flags);
                        CERROR("tx on rdmaq for "LPX64" blocked %d seconds\n",
                               conn->rac_perr->rap_nid, (now - tx->tx_qtime)/HZ);
                        return -ETIMEDOUT;
                }
        }
        
        list_for_each (ttmp, &conn->rac_replyq) {
                tx = list_entry(ttmp, kra_tx_t, tx_list);
                
                if (time_after_eq(now, tx->tx_qtime + timeout)) {
                        spin_unlock_irqrestore(&conn->rac_lock, flags);
                        CERROR("tx on replyq for "LPX64" blocked %d seconds\n",
                               conn->rac_perr->rap_nid, (now - tx->tx_qtime)/HZ);
                        return -ETIMEDOUT;
                }
        }
        
        spin_unlock_irqrestore(&conn->rac_lock, flags);
        return 0;
}

void
kranal_check_conns (int idx, unsigned long *min_timeoutp)
{
        struct list_head  *conns = &kranal_data.kra_conns[idx];
        struct list_head  *ctmp;
        kra_conn_t        *conn;

 again:
        /* NB. We expect to check all the conns and not find any problems, so
         * we just use a shared lock while we take a look... */
        read_lock(&kranal_data.kra_global_lock);

        list_for_each (ctmp, conns) {
                conn = list_entry(ptmp, kra_conn_t, rac_hashlist);

                if (conn->rac_timeout < *min_timeoutp )
                        *min_timeoutp = conn->rac_timeout;
                if (conn->rac_keepalive < *min_timeoutp )
                        *min_timeoutp = conn->rac_keepalive;

                rc = kranal_check_conn(conn);
                if (rc == 0)
                        continue;

                kranal_conn_addref(conn);
                read_unlock(&kranal_data.kra_global_lock);

                CERROR("Check on conn to "LPX64"failed: %d\n",
                       conn->rac_peer->rap_nid, rc);

                write_lock_irqsave(&kranal_data.kra_global_lock);

                if (!conn->rac_closing)
                        kranal_close_conn_locked(conn, -ETIMEDOUT);
                else
                        kranal_terminate_conn_locked(conn);
                        
                kranal_conn_decref(conn);

                /* start again now I've dropped the lock */
                goto again;
        }

        read_unlock(&kranal_data.kra_global_lock);
}

int
kranal_connd (void *arg)
{
	char               name[16];
        wait_queue_t       wait;
        unsigned long      flags;
        kra_peer_t        *peer;
        int                i;

	snprintf(name, sizeof(name), "kranal_connd_%02ld", (long)arg);
        kportal_daemonize(name);
        kportal_blockallsigs();

        init_waitqueue_entry(&wait, current);

        spin_lock_irqsave(&kranal_data.kra_connd_lock, flags);

        while (!kranal_data.kra_shutdown) {
                /* Safe: kra_shutdown only set when quiescent */

                if (!list_empty(&kranal_data.kra_connd_peers)) {
                        peer = list_entry(kranal_data.kra_connd_peers.next,
					  kra_peer_t, rap_connd_list);
                        
                        list_del_init(&peer->rap_connd_list);
                        spin_unlock_irqrestore(&kranal_data.kra_connd_lock, flags);

                        kranal_connect(peer);
                        kranal_put_peer(peer);

                        spin_lock_irqsave(&kranal_data.kra_connd_lock, flags);
			continue;
                }

                set_current_state(TASK_INTERRUPTIBLE);
                add_wait_queue(&kranal_data.kra_connd_waitq, &wait);
                
                spin_unlock_irqrestore(&kranal_data.kra_connd_lock, flags);

                schedule ();
                
                set_current_state(TASK_RUNNING);
                remove_wait_queue(&kranal_data.kra_connd_waitq, &wait);

                spin_lock_irqsave(&kranal_data.kra_connd_lock, flags);
        }

        spin_unlock_irqrestore(&kranal_data.kra_connd_lock, flags);

        kranal_thread_fini();
        return 0;
}

void
kranal_update_reaper_timeout(long timeout) 
{
        unsigned long   flags;

        LASSERT (timeout > 0);
        
        spin_lock_irqsave(&kranal_data.kra_reaper_lock, flags);
        
        if (timeout < kranal_data.kra_new_min_timeout)
                kranal_data.kra_new_min_timeout = timeout;

        spin_unlock_irqrestore(&kranal_data.kra_reaper_lock, flags);
}

int
kranal_reaper (void *arg)
{
        wait_queue_t       wait;
        unsigned long      flags;
        kra_conn_t        *conn;
        kra_peer_t        *peer;
        unsigned long      flags;
        long               timeout;
        int                i;
        int                conn_entries = kranal_data.kra_conn_hash_size;
        int                conn_index = 0;
        int                base_index = conn_entries - 1;
        unsigned long      next_check_time = jiffies;
        long               next_min_timeout = MAX_SCHEDULE_TIMEOUT;
        long               current_min_timeout = 1;
        
        kportal_daemonize("kranal_reaper");
        kportal_blockallsigs();

        init_waitqueue_entry(&wait, current);

        spin_lock_irqsave(&kranal_data.kra_reaper_lock, flags);
        kranal_data.kra_new_min_timeout = 1;

        while (!kranal_data.kra_shutdown) {

                /* careful with the jiffy wrap... */
                timeout = (long)(next_check_time - jiffies);
                if (timeout <= 0) {
                
                        /* I wake up every 'p' seconds to check for
                         * timeouts on some more peers.  I try to check
                         * every connection 'n' times within the global
                         * minimum of all keepalive and timeout intervals,
                         * to ensure I attend to every connection within
                         * (n+1)/n times its timeout intervals. */
                
                        const int     p = 1;
                        const int     n = 3;
                        unsigned long min_timeout;
                        int           chunk;

                        if (kranal_data.kra_new_min_timeout != MAX_SCHEDULE_TIMEOUT) {
                                /* new min timeout set: restart min timeout scan */
                                next_min_timeout = MAX_SCHEDULE_TIMEOUT;
                                base_index = conn_index - 1;
                                if (base_index < 0)
                                        base_index = conn_entries - 1;

                                if (kranal_data.kra_new_min_timeout < current_min_timeout) {
                                        current_min_timeout = kranal_data.kra_new_min_timeout;
                                        CWARN("Set new min timeout %ld\n",
                                              current_min_timeout);
                                }

                                kranal_data.kra_new_min_timeout = MAX_SCHEDULE_TIMEOUT;
                        }
                        min_timeout = current_min_timeout;

                        spin_unlock_irqrestore(&kranal_data.kra_reaper_lock,
                                               flags);

                        LASSERT (min_timeout > 0);

                        /* Compute how many table entries to check now so I
                         * get round the whole table fast enough (NB I do
                         * this at fixed intervals of 'p' seconds) */
			chunk = conn_entries;
                        if (min_timeout > n * p)
                                chunk = (chunk * n * p) / min_timeout;
                        if (chunk == 0)
                                chunk = 1;

                        for (i = 0; i < chunk; i++) {
                                kranal_check_conns(conn_index, 
                                                   &next_min_timeout);
                                conn_index = (conn_index + 1) % conn_entries;
                        }

                        next_check_time += p * HZ;

                        spin_lock_irqsave(&kranal_data.kra_reaper_lock, flags);

                        if (((conn_index - chunk <= base_index &&
                              base_index < conn_index) ||
                             (conn_index - conn_entries - chunk <= base_index &&
                              base_index < conn_index - conn_entries))) {

                                /* Scanned all conns: set current_min_timeout... */
                                if (current_min_timeout != next_min_timeout) {
                                        current_min_timeout = next_min_timeout;                                        
                                        CWARN("Set new min timeout %ld\n",
                                              current_min_timeout);
                                }

                                /* ...and restart min timeout scan */
                                next_min_timeout = MAX_SCHEDULE_TIMEOUT;
                                base_index = conn_index - 1;
                                if (base_index < 0)
                                        base_index = conn_entries - 1;
                        }
                }

                set_current_state(TASK_INTERRUPTIBLE);
                add_wait_queue(&kranal_data.kra_reaper_waitq, &wait);

                spin_unlock_irqrestore(&kranal_data.kra_reaper_lock, flags);

                busy_loops = 0;
                schedule_timeout(timeout);

                spin_lock_irqsave(&kranal_data.kra_reaper_lock, flags);

                set_current_state(TASK_RUNNING);
                remove_wait_queue(&kranal_data.kra_reaper_waitq, &wait);
        }

        kranal_thread_fini();
        return 0;
}

void
kranal_process_rdmaq (__u32 cqid)
{
        kra_conn_t          *conn;
        kra_tx_t            *tx;
        RAP_RETURN           rrc;
        unsigned long        flags;
        RAP_RDMA_DESCRIPTOR *desc;
        
        read_lock(&kranal_data.kra_global_lock);

        conn = kranal_cqid2conn_locked(cqid);
        LASSERT (conn != NULL);

        rrc = RapkRdmaDone(conn->rac_rihandle, &desc);
        LASSERT (rrc == RAP_SUCCESS);

        spin_lock_irqsave(&conn->rac_lock, flags);

        LASSERT (!list_empty(&conn->rac_rdmaq));
        tx = list_entry(con->rac_rdmaq.next, kra_tx_t, tx_list);
        list_del(&tx->tx_list);

        LASSERT(desc->AppPtr == (void *)tx);
        LASSERT(desc->tx_msg.ram_type == RANAL_MSG_PUT_DONE ||
                desc->tx_msg.ram_type == RANAL_MSG_GET_DONE);

        list_add_tail(&tx->tx_list, &conn->rac_fmaq);
        tx->tx_qtime = jiffies;
        
        spin_unlock_irqrestore(&conn->rac_lock, flags);

        /* Get conn's fmaq processed, now I've just put something there */
        kranal_schedule_conn(conn);

        read_unlock(&kranal_data.kra_global_lock);
}

int
kranal_sendmsg(kra_conn_t *conn, kra_msg_t *msg,
               void *immediate, int immediatenob)
{
        int   sync = (msg->ram_type & RANAL_MSG_FENCE) != 0;

        LASSERT (sizeof(*msg) <= RANAL_FMA_PREFIX_LEN);
        LASSERT ((msg->ram_type == RANAL_MSG_IMMEDIATE) ?
                 immediatenob <= RANAL_FMA_MAX_DATA_LEN :
                 immediatenob == 0);

        msg->ram_incarnation = conn->rac_incarnation;
        msg->ram_seq = conn->rac_tx_seq;

        if (sync)
                rrc = RapkFmaSyncSend(conn->rac_device.rad_handle,
                                      immediate, immediatenob,
                                      msg, sizeof(*msg));
        else
                rrc = RapkFmaSend(conn->rac_device.rad_handle,
                                  immediate, immediatenob,
                                  msg, sizeof(*msg));

        switch (rrc) {
        case RAP_SUCCESS:
                conn->rac_last_tx = jiffies;
                conn->rac_tx_seq++;
                return 0;
                
        case RAP_NOT_DONE:
                return -EAGAIN;

        default:
                LBUG();
        }
}

int
kranal_process_fmaq (kra_conn_t *conn) 
{
        unsigned long flags;
        int           more_to_do;
        kra_tx_t     *tx;
        int           rc;
        int           expect_reply;

        /* NB I will be rescheduled some via a rad_fma_cq event if my FMA is
         * out of credits when I try to send right now... */

        if (conn->rac_closing) {

                if (!list_empty(&conn->rac_rdmaq)) {
                        /* Can't send CLOSE yet; I'm still waiting for RDMAs I
                         * posted to finish */
                        LASSERT (!conn->rac_close_sent);
                        kranal_init_msg(&conn->rac_msg, RANAL_MSG_NOOP);
                        kranal_sendmsg(conn, &conn->rac_msg, NULL, 0);
                        return 0;
                }

                if (conn->rac_close_sent)
                        return 0;
                
                kranal_init_msg(&conn->rac_msg, RANAL_MSG_CLOSE);
                rc = kranal_sendmsg(conn, &conn->rac_msg, NULL, 0);
                conn->rac_close_sent = (rc == 0);
                return 0;
        }

        spin_lock_irqsave(&conn->rac_lock, flags);

        if (list_empty(&conn->rac_fmaq)) {

                spin_unlock_irqrestore(&conn->rac_lock, flags);

                if (time_after_eq(conn->rac_last_tx + conn->rac_keepalive)) {
                        kranal_init_msg(&conn->rac_msg, RANAL_MSG_NOOP);
                        kranal_sendmsg(conn, &conn->rac_msg, NULL, 0);
                }
                return 0;
        }
        
        tx = list_entry(conn->rac_fmaq.next, kra_tx_t, tx_list);
        list_del(&tx->tx_list);
        more_to_do = !list_empty(&conn->rac_fmaq);

        spin_unlock_irqrestore(&conn->rac_lock, flags);

        expect_reply = 0;
        switch (tx->tx_msg.ram_type) {
        default:
                LBUG();
                
        case RANAL_MSG_IMMEDIATE:
        case RANAL_MSG_PUT_NAK:
        case RANAL_MSG_PUT_DONE:
        case RANAL_MSG_GET_NAK:
        case RANAL_MSG_GET_DONE:
                rc = kranal_sendmsg(conn, &tx->tx_msg,
                                    tx->tx_buffer, tx->tx_nob);
                expect_reply = 0;
                break;
                
        case RANAL_MSG_PUT_REQ:
                tx->tx_msg.ram_u.putreq.raprm_cookie = tx->tx_cookie;
                rc = kranal_sendmsg(conn, &tx->tx_msg, NULL, 0);
                kranal_map_buffer(tx);
                expect_reply = 1;
                break;

        case RANAL_MSG_PUT_ACK:
                rc = kranal_sendmsg(conn, &tx->tx_msg, NULL, 0);
                expect_reply = 1;
                break;

        case RANAL_MSG_GET_REQ:
                kranal_map_buffer(tx);
                tx->tx_msg.ram_u.get.ragm_cookie = tx->tx_cookie;
                tx->tx_msg.ram_u.get.ragm_desc.rard_key = tx->tx_map_key;
                tx->tx_msg.ram_u.get.ragm_desc.rard_addr = tx->tx_buffer;
                tx->tx_msg.ram_u.get.ragm_desc.rard_nob = tx->tx_nob;
                rc = kranal_sendmsg(conn, &tx->tx_msg, NULL, 0);
                expect_reply = 1;
                break;
        }

        if (rc == -EAGAIN) {
                /* replace at the head of the list for later */
                spin_lock_irqsave(&conn->rac_lock, flags);
                list_add(&tx->tx_list, &conn->rac_fmaq);
                spin_unlock_irqrestore(&conn->rac_lock, flags);

                return 0;
        }

        LASSERT (rc == 0);
        
        if (!expect_reply) {
                kranal_tx_done(tx, 0);
        } else {
                spin_lock_irqsave(&conn->rac_lock, flags);
                list_add_tail(&tx->tx_list, &conn->rac_replyq);
                tx->tx_qtime = jiffies;
                spin_unlock_irqrestore(&conn->rac_lock, flags);
        }

        return more_to_do;
}

static inline void
kranal_swab_rdma_desc (kra_rdma_desc_t *d)
{
        __swab64s(&d->rard_key.Key);
        __swab16s(&d->rard_key.Cookie);
        __swab16s(&d->rard_key.MdHandle);
        __swab32s(&d->rard_key.Flags);
        __swab64s(&d->rard_addr);
        __swab32s(&d->rard_nob);
}

kra_tx_t *
kranal_match_reply(kra_conn_t *conn, int type, __u64 cookie)
{
        unsigned long     flags;
        struct list_head *ttmp;
        kra_tx_t         *tx;
        
        list_for_each(ttmp, &conn->rac_replyq) {
                tx = list_entry(ttmp, kra_tx_t, tx_list);
                
                if (tx->tx_cookie != cookie)
                        continue;
                
                if (tx->tx_msg.ram_type != type) {
                        CWARN("Unexpected type %x (%x expected) "
                              "matched reply from "LPX64"\n",
                              tx->tx_msg.ram_type, type,
                              conn->rac_peer->rap_nid);
                        return NULL;
                }
        }
        
        CWARN("Unmatched reply from "LPX64"\n", conn->rac_peer->rap_nid);
        return NULL;
}

int
kranal_process_receives(kra_conn_t *conn)
{
        unsigned long flags;
        __u32         seq;
        __u32         nob;
        kra_msg_t    *msg;
        RAP_RETURN    rrc = RapkFmaGetPrefix(conn->rac_rihandle, &msg);
        kra_peer_t   *peer = conn->rac_peer;

        if (rrc == RAP_NOT_DONE)
                return 0;
        
        LASSERT (rrc == RAP_SUCCESS);
        conn->rac_last_rx = jiffies;
        seq = conn->rac_seq++;

        if (msg->ram_magic != RANAL_MSG_MAGIC) {
                if (__swab32(msg->ram_magic) != RANAL_MSG_MAGIC) {
                        CERROR("Unexpected magic %08x from "LPX64"\n",
                               msg->ram_magic, peer->rap_nid);
                        goto out;
                }

                __swab32s(&msg->ram_magic);
                __swab16s(&msg->ram_version);
                __swab16s(&msg->ram_type);
                __swab64s(&msg->ram_srcnid);
                __swab64s(&msg->ram_incarnation);
                __swab32s(&msg->ram_seq);

                /* NB message type checked below; NOT here... */
                switch (msg->ram_type) {
                case RANAL_MSG_PUT_ACK:
                        kranal_swab_rdma_desc(&msg->ram_u.putack.rapam_desc);
                        break;

                case RANAL_MSG_GET_REQ:
                        kranal_swab_rdma_desc(&msg->ram_u.get.ragm_desc);
                        break;
                        
                default:
                        break;
                }
        }

        if (msg->ram_version != RANAL_MSG_VERSION) {
                CERROR("Unexpected protocol version %d from "LPX64"\n",
                       msg->ram_version, peer->rap_nid);
                goto out;
        }

        if (msg->ram_srcnid != peer->rap_nid) {
                CERROR("Unexpected peer "LPX64" from "LPX64"\n",
                       msg->ram_srcnid, peer->rap_nid);
                goto out;
        }
        
        if (msg->ram_incarnation != conn->rac_incarnation) {
                CERROR("Unexpected incarnation "LPX64"("LPX64
                       " expected) from "LPX64"\n",
                       msg->ram_incarnation, conn->rac_incarnation,
                       peer->rap_nid);
                goto out;
        }
        
        if (msg->ram_seq != seq) {
                CERROR("Unexpected sequence number %d(%d expected) from "
                       LPX64"\n", msg->ram_seq, seq, peer->rap_nid);
                goto out;
        }

        if ((msg->ram_type & RANAL_MSG_FENCE) != 0) {
                /* This message signals RDMA completion: wait now... */
                rrc = RapkFmaSyncWait(conn->rac_rihandle);
                LASSERT (rrc == RAP_SUCCESS);
        }
 
        if (msg->ram_type == RANAL_MSG_CLOSE) {
                conn->rac_close_recvd = 1;
                write_lock_irqsave(&kranal_data.kra_global_lock);

                if (!conn->rac_closing)
                        kranal_close_conn_locked(conn, -ETIMEDOUT);
                else if (conn->rac_close_sent)
                        kranal_terminate_conn_locked(conn);
                
                goto out;
        }

        if (conn->rac_closing)
                goto out;
        
        conn->rac_rxmsg = msg;                  /* stash message for portals callbacks */
                                                /* they'll NULL rac_rxmsg if they consume it */
        switch (msg->ram_type) {
        case RANAL_MSG_NOOP:
                /* Nothing to do; just a keepalive */
                break;
                
        case RANAL_MSG_IMMEDIATE:
                lib_parse(&kranal_lib, &msg->ram_u.immediate.raim_hdr, conn);
                break;
                
        case RANAL_MSG_PUT_REQ:
                lib_parse(&kranal_lib, &msg->ram_u.putreq.raprm_hdr, conn);

                if (conn->rac_rxmsg == NULL)    /* lib_parse matched something */
                        break;

                tx = kranal_new_tx_msg(0, RANAL_MSG_PUT_NAK);
                if (tx == NULL)
                        break;
                
                tx->tx_msg.ram_u.racm_cookie = msg->msg_u.putreq.raprm_cookie;
                kranal_post_fma(conn, tx);
                break;

        case RANAL_MSG_PUT_NAK:
                tx = kranal_match_reply(conn, RANAL_MSG_PUT_REQ,
                                        msg->ram_u.completion.racm_cookie);
                if (tx == NULL)
                        break;
                
                LASSERT (tx->tx_buftype == RANAL_BUF_PHYS_MAPPED ||
                         tx->tx_buftype == RANAL_BUF_VIRT_MAPPED);
                kranal_tx_done(tx, -ENOENT);    /* no match */
                break;
                
        case RANAL_MSG_PUT_ACK:
                tx = kranal_match_reply(conn, RANAL_MSG_PUT_REQ,
                                        msg->ram_u.putack.rapam_src_cookie);
                if (tx == NULL)
                        break;

                kranal_rdma(tx, RANAL_MSG_PUT_DONE,
                            &msg->ram_u.putack.rapam_desc, 
                            msg->msg_u.putack.rapam_desc.rard_nob,
                            msg->ram_u.putack.rapam_dst_cookie);
                break;

        case RANAL_MSG_PUT_DONE:
                tx = kranal_match_reply(conn, RANAL_MSG_PUT_ACK,
                                        msg->ram_u.completion.racm_cookie);
                if (tx == NULL)
                        break;

                LASSERT (tx->tx_buftype == RANAL_BUF_PHYS_MAPPED ||
                         tx->tx_buftype == RANAL_BUF_VIRT_MAPPED);
                kranal_tx_done(tx, 0);
                break;

        case RANAL_MSG_GET_REQ:
                lib_parse(&kranal_lib, &msg->ram_u.getreq.ragm_hdr, conn);
                
                if (conn->rac_rxmsg == NULL)    /* lib_parse matched something */
                        break;

                tx = kranal_new_tx_msg(0, RANAL_MSG_GET_NAK);
                if (tx == NULL)
                        break;

                tx->tx_msg.ram_u.racm_cookie = msg->msg_u.getreq.ragm_cookie;
                kranal_post_fma(conn, tx);
                break;
                
        case RANAL_MSG_GET_NAK:
                tx = kranal_match_reply(conn, RANAL_MSG_GET_REQ,
                                        msg->ram_u.completion.racm_cookie);
                if (tx == NULL)
                        break;
                
                LASSERT (tx->tx_buftype == RANAL_BUF_PHYS_MAPPED ||
                         tx->tx_buftype == RANAL_BUF_VIRT_MAPPED);
                kranal_tx_done(tx, -ENOENT);    /* no match */
                break;
                
        case RANAL_MSG_GET_DONE:
                tx = kranal_match_reply(conn, RANAL_MSG_GET_REQ,
                                        msg->ram_u.completion.racm_cookie);
                if (tx == NULL)
                        break;
                
                LASSERT (tx->tx_buftype == RANAL_BUF_PHYS_MAPPED ||
                         tx->tx_buftype == RANAL_BUF_VIRT_MAPPED);
                kranal_tx_done(tx, 0);
                break;
        }

 out:
        if (conn->rac_msg != NULL)
                kranal_consume_rxmsg(conn, NULL, 0);

        return 1;
}

int
kranal_scheduler (void *arg)
{
        kra_device_t   *dev = (kra_device_t *)arg;
        wait_queue_t    wait;
        char            name[16];
        kra_conn_t     *conn;
        unsigned long   flags;
        int             rc;
        int             i;
        __u32           cqid;
        int             did_something;
        int             busy_loops = 0;

        snprintf(name, sizeof(name), "kranal_sd_%02ld", dev->rad_idx);
        kportal_daemonize(name);
        kportal_blockallsigs();

        init_waitqueue_entry(&wait, current);

        spin_lock_irqsave(&dev->rad_lock, flags);

        while (!kranal_data.kra_shutdown) {
                /* Safe: kra_shutdown only set when quiescent */
                
		if (busy_loops++ >= RANAL_RESCHED) {
                        spin_unlock_irqrestore(&dev->rad_lock, flags);

                        our_cond_resched();
			busy_loops = 0;

                        spin_lock_irqsave(&dev->rad_lock, flags);
		}

                did_something = 0;

                if (dev->rad_ready) {
                        dev->rad_ready = 0;
                        spin_unlock_irqrestore(&dev->rad_lock, flags);

                        rrc = RapkCQDone(dev->rad_rdma_cq, &cqid, &event_type);

                        LASSERT (rrc == RAP_SUCCESS || rrc == RAP_NOT_DONE);
                        LASSERT ((event_type & RAPK_CQ_EVENT_OVERRUN) == 0);
                        
                        if (rrc == RAP_SUCCESS) {
                                kranal_process_rdmaq(cqid);
                                did_something = 1;
                        }
                        
                        rrc = RapkCQDone(dev->rad_fma_cq, &cqid, &event_type);
                        LASSERT (rrc == RAP_SUCCESS || rrc == RAP_NOT_DONE);
                        
                        if (rrc == RAP_SUCCESS) {
                                if ((event_type & RAPK_CQ_EVENT_OVERRUN) != 0)
                                        kranal_schedule_dev(dev);
                                else
                                        kranal_schedule_cqid(cqid);
                                did_something = 1;
                        }
                        
                        spin_lock_irqsave(&dev->rad_lock, flags);

                        /* If there were no completions to handle, I leave
                         * rad_ready clear.  NB I cleared it BEFORE I checked
                         * the completion queues since I'm racing with the
                         * device callback. */

                        if (did_something)
                                dev->rad_ready = 1;
                }
		
                if (!list_empty(&dev->rad_connq)) {
                        conn = list_entry(dev->rad_connq.next,
                                          kra_conn_t, rac_schedlist);
                        list_del(&conn->rac_schedlist);
                        spin_unlock_irqrestore(&dev->rad_lock, flags);

                        LASSERT (conn->rac_scheduled);

                        resched  = kranal_process_fmaq(conn);
                        resched |= kranal_process_receives(conn);
                        did_something = 1;

                        spin_lock_irqsave(&dev->rad_lock, flags);
                        if (resched)
                                list_add_tail(&conn->rac_schedlist,
                                              &dev->rad_connq);
                }

                if (did_something)
                        continue;

                add_wait_queue(&dev->rad_waitq, &wait);
                set_current_state(TASK_INTERRUPTIBLE);

                spin_unlock_irqrestore(&dev->rad_lock, flags);

                busy_loops = 0;
                schedule();

                set_current_state(TASK_RUNNING);
                remove_wait_queue(&dev->rad_waitq, &wait);

                spin_lock_irqsave(&dev->rad_lock, flags);
        }

        spin_unlock_irqrestore(&dev->rad_lock, flags);

        kranal_thread_fini();
        return 0;
}


lib_nal_t kranal_lib = {
        libnal_data:        &kranal_data,      /* NAL private data */
        libnal_send:         kranal_send,
        libnal_send_pages:   kranal_send_pages,
        libnal_recv:         kranal_recv,
        libnal_recv_pages:   kranal_recv_pages,
        libnal_dist:         kranal_dist
};
