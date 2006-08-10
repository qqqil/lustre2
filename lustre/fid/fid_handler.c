/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/fid/fid_handler.c
 *  Lustre Sequence Manager
 *
 *  Copyright (c) 2006 Cluster File Systems, Inc.
 *   Author: Yury Umanets <umka@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_FID

#ifdef __KERNEL__
# include <libcfs/libcfs.h>
# include <linux/module.h>
#else /* __KERNEL__ */
# include <liblustre.h>
#endif

#include <obd.h>
#include <obd_class.h>
#include <dt_object.h>
#include <md_object.h>
#include <obd_support.h>
#include <lustre_req_layout.h>
#include <lustre_fid.h>
#include "fid_internal.h"

#ifdef __KERNEL__
/* sequence space, starts from 0x400 to have first 0x400 sequences used for
 * special purposes. */
const struct lu_range LUSTRE_SEQ_SPACE_RANGE = {
        (0x400),
        ((__u64)~0ULL)
};
EXPORT_SYMBOL(LUSTRE_SEQ_SPACE_RANGE);

/* zero range, used for init and other purposes */
const struct lu_range LUSTRE_SEQ_ZERO_RANGE = {
        0,
        0
};
EXPORT_SYMBOL(LUSTRE_SEQ_ZERO_RANGE);

/* assigns client to sequence controller node */
int seq_server_set_cli(struct lu_server_seq *seq,
                       struct lu_client_seq *cli,
                       const struct lu_context *ctx)
{
        int rc = 0;
        ENTRY;

        if (cli == NULL) {
                CDEBUG(D_INFO|D_WARNING, "%s: detached "
                       "sequence mgr client %s\n", seq->lss_name,
                       cli->lcs_exp->exp_client_uuid.uuid);
                seq->lss_cli = cli;
                RETURN(0);
        }

        if (seq->lss_cli) {
                CERROR("%s: sequence-controller is already "
                       "assigned\n", seq->lss_name);
                RETURN(-EINVAL);
        }

        CDEBUG(D_INFO|D_WARNING, "%s: attached "
               "sequence client %s\n", seq->lss_name,
               cli->lcs_exp->exp_client_uuid.uuid);

        /* asking client for new range, assign that range to ->seq_super and
         * write seq state to backing store should be atomic. */
        down(&seq->lss_sem);

        /* assign controller */
        seq->lss_cli = cli;

        /* get new range from controller only if super-sequence is not yet
         * initialized from backing store or something else. */
        if (range_is_zero(&seq->lss_super)) {
                rc = seq_client_alloc_super(cli);
                if (rc) {
                        CERROR("can't allocate super-sequence, "
                               "rc %d\n", rc);
                        GOTO(out_up, rc);
                }

                /* take super-seq from client seq mgr */
                LASSERT(range_is_sane(&cli->lcs_range));

                seq->lss_super = cli->lcs_range;

                /* save init seq to backing store. */
                rc = seq_store_write(seq, ctx);
                if (rc) {
                        CERROR("can't write sequence state, "
                               "rc = %d\n", rc);
                }
        }

        EXIT;
out_up:
        up(&seq->lss_sem);
        return rc;
}
EXPORT_SYMBOL(seq_server_set_cli);

/* on controller node, allocate new super sequence for regular sequence
 * server. */
static int __seq_server_alloc_super(struct lu_server_seq *seq,
                                    struct lu_range *range,
                                    const struct lu_context *ctx)
{
        struct lu_range *space = &seq->lss_space;
        int rc;
        ENTRY;

        LASSERT(range_is_sane(space));

        if (range_space(space) < seq->lss_super_width) {
                CWARN("sequences space is going to exhaust soon. "
                      "Can allocate only "LPU64" sequences\n",
                      range_space(space));
                *range = *space;
                space->lr_start = space->lr_end;
                rc = 0;
        } else if (range_is_exhausted(space)) {
                CERROR("sequences space is exhausted\n");
                rc = -ENOSPC;
        } else {
                range_alloc(range, space, seq->lss_super_width);
                rc = 0;
        }

        if (rc == 0) {
                rc = seq_store_write(seq, ctx);
                if (rc) {
                        CERROR("can't save state, rc = %d\n",
                               rc);
                }
        }

        if (rc == 0) {
                CDEBUG(D_INFO, "%s: allocated super-sequence "
                       DRANGE"\n", seq->lss_name, PRANGE(range));
        }

        RETURN(rc);
}

static int seq_server_alloc_super(struct lu_server_seq *seq,
                                  struct lu_range *range,
                                  const struct lu_context *ctx)
{
        int rc;
        ENTRY;

        down(&seq->lss_sem);
        rc = __seq_server_alloc_super(seq, range, ctx);
        up(&seq->lss_sem);

        RETURN(rc);
}

static int __seq_server_alloc_meta(struct lu_server_seq *seq,
                                   struct lu_range *range,
                                   const struct lu_context *ctx)
{
        struct lu_range *super = &seq->lss_super;
        int rc = 0;
        ENTRY;

        LASSERT(range_is_sane(super));

        /* XXX: here we should avoid cascading RPCs using kind of async
         * preallocation when meta-sequence is close to exhausting. */
        if (range_is_exhausted(super)) {
                if (!seq->lss_cli) {
                        CERROR("no seq-controller client is setup\n");
                        RETURN(-EOPNOTSUPP);
                }

                rc = seq_client_alloc_super(seq->lss_cli);
                if (rc) {
                        CERROR("can't allocate new super-sequence, "
                               "rc %d\n", rc);
                        RETURN(rc);
                }

                /* saving new range into allocation space. */
                *super = seq->lss_cli->lcs_range;
                LASSERT(range_is_sane(super));
        }
        range_alloc(range, super, seq->lss_meta_width);

        rc = seq_store_write(seq, ctx);
        if (rc) {
                CERROR("can't save state, rc = %d\n",
		       rc);
        }

        if (rc == 0) {
                CDEBUG(D_INFO, "%s: allocated meta-sequence "
                       DRANGE"\n", seq->lss_name, PRANGE(range));
        }

        RETURN(rc);
}

static int seq_server_alloc_meta(struct lu_server_seq *seq,
                                 struct lu_range *range,
                                 const struct lu_context *ctx)
{
        int rc;
        ENTRY;

        down(&seq->lss_sem);
        rc = __seq_server_alloc_meta(seq, range, ctx);
        up(&seq->lss_sem);

        RETURN(rc);
}

static int seq_req_handle0(const struct lu_context *ctx,
                           struct ptlrpc_request *req,
                           struct seq_thread_info *info)
{
        struct lu_site *site;
        struct lu_range *out;
        int rc = -EPROTO;
        __u32 *opc;
        ENTRY;

        site = req->rq_export->exp_obd->obd_lu_dev->ld_site;
        LASSERT(site != NULL);
			
        req_capsule_pack(&info->sti_pill);

        opc = req_capsule_client_get(&info->sti_pill,
                                     &RMF_SEQ_OPC);
        if (opc != NULL) {
                out = req_capsule_server_get(&info->sti_pill,
                                             &RMF_SEQ_RANGE);
                if (out == NULL)
                        RETURN(-EPROTO);

                switch (*opc) {
                case SEQ_ALLOC_META:
                        if (!site->ls_server_seq) {
                                CERROR("sequence-server is not "
                                       "initialized\n");
                                RETURN(-EINVAL);
                        }
                        rc = seq_server_alloc_meta(site->ls_server_seq,
                                                   out, ctx);
                        break;
                case SEQ_ALLOC_SUPER:
                        if (!site->ls_control_seq) {
                                CERROR("sequence-controller is not "
                                       "initialized\n");
                                RETURN(-EINVAL);
                        }
                        rc = seq_server_alloc_super(site->ls_control_seq,
                                                    out, ctx);
                        break;
                default:
                        CERROR("wrong opc %#x\n", *opc);
                        break;
                }
        }

        RETURN(rc);
}

static void *seq_thread_init(const struct lu_context *ctx,
                             struct lu_context_key *key)
{
        struct seq_thread_info *info;

        /*
         * check that no high order allocations are incurred.
         */
        CLASSERT(CFS_PAGE_SIZE >= sizeof *info);
        OBD_ALLOC_PTR(info);
        if (info == NULL)
                info = ERR_PTR(-ENOMEM);
        return info;
}

static void seq_thread_fini(const struct lu_context *ctx,
                            struct lu_context_key *key, void *data)
{
        struct seq_thread_info *info = data;
        OBD_FREE_PTR(info);
}

struct lu_context_key seq_thread_key = {
        .lct_tags = LCT_MD_THREAD,
        .lct_init = seq_thread_init,
        .lct_fini = seq_thread_fini
};

static void seq_thread_info_init(struct ptlrpc_request *req,
                                 struct seq_thread_info *info)
{
        int i;

        /* mark rep buffer as req-layout stuff expects */
        for (i = 0; i < ARRAY_SIZE(info->sti_rep_buf_size); i++)
                info->sti_rep_buf_size[i] = -1;

        /* init request capsule */
        req_capsule_init(&info->sti_pill, req, RCL_SERVER,
                         info->sti_rep_buf_size);

        req_capsule_set(&info->sti_pill, &RQF_SEQ_QUERY);
}

static void seq_thread_info_fini(struct seq_thread_info *info)
{
        req_capsule_fini(&info->sti_pill);
}

static int seq_req_handle(struct ptlrpc_request *req)
{
        const struct lu_context *ctx;
        struct seq_thread_info *info;
        int rc = 0;
        ENTRY;

        OBD_FAIL_RETURN(OBD_FAIL_SEQ_ALL_REPLY_NET | OBD_FAIL_ONCE, 0);

        ctx = req->rq_svc_thread->t_ctx;
        LASSERT(ctx != NULL);
        LASSERT(ctx->lc_thread == req->rq_svc_thread);

        info = lu_context_key_get(ctx, &seq_thread_key);
        LASSERT(info != NULL);

        seq_thread_info_init(req, info);

        if (req->rq_reqmsg->opc == SEQ_QUERY) {
                if (req->rq_export != NULL) {
                        /* 
                         * no need to return error here and overwrite @rc, this
                         * function should return 0 even if seq_req_handle0()
                         * returns some error code.
                         */
                        seq_req_handle0(ctx, req, info);
                } else {
                        CERROR("Unconnected request\n");
                        req->rq_status = -ENOTCONN;
                }
        } else {
                CERROR("Wrong opcode: %d\n", req->rq_reqmsg->opc);
                req->rq_status = -ENOTSUPP;
                rc = ptlrpc_error(req);
                GOTO(out_info, rc);
        }

        target_send_reply(req, rc, OBD_FAIL_SEQ_ALL_REPLY_NET);
        EXIT;
out_info:
        seq_thread_info_fini(info);
        return rc;
}

static void seq_server_proc_fini(struct lu_server_seq *seq);

#ifdef LPROCFS
static int seq_server_proc_init(struct lu_server_seq *seq)
{
        int rc;
        ENTRY;

        seq->lss_proc_dir = lprocfs_register(seq->lss_name,
                                             proc_lustre_root,
                                             NULL, NULL);
        if (IS_ERR(seq->lss_proc_dir)) {
                CERROR("LProcFS failed in seq-init\n");
                rc = PTR_ERR(seq->lss_proc_dir);
                RETURN(rc);
        }

        seq->lss_proc_entry = lprocfs_register("services",
                                               seq->lss_proc_dir,
                                               NULL, NULL);
        if (IS_ERR(seq->lss_proc_entry)) {
                CERROR("LProcFS failed in seq-init\n");
                rc = PTR_ERR(seq->lss_proc_entry);
                GOTO(out_cleanup, rc);
        }

        rc = lprocfs_add_vars(seq->lss_proc_dir,
                              seq_server_proc_list, seq);
        if (rc) {
                CERROR("can't init sequence manager "
                       "proc, rc %d\n", rc);
                GOTO(out_cleanup, rc);
        }

        RETURN(0);

out_cleanup:
        seq_server_proc_fini(seq);
        return rc;
}

static void seq_server_proc_fini(struct lu_server_seq *seq)
{
        ENTRY;
        if (seq->lss_proc_entry != NULL) {
                if (!IS_ERR(seq->lss_proc_entry))
                        lprocfs_remove(seq->lss_proc_entry);
                seq->lss_proc_entry = NULL;
        }

        if (seq->lss_proc_dir != NULL) {
                if (!IS_ERR(seq->lss_proc_dir))
                        lprocfs_remove(seq->lss_proc_dir);
                seq->lss_proc_dir = NULL;
        }
        EXIT;
}
#else
static int seq_server_proc_init(struct lu_server_seq *seq)
{
        return 0;
}

static void seq_server_proc_fini(struct lu_server_seq *seq)
{
        return;
}
#endif

int seq_server_init(struct lu_server_seq *seq,
                    struct dt_device *dev,
                    const char *uuid,
                    enum lu_mgr_type type,
                    const struct lu_context *ctx)
{
        int is_srv = type == LUSTRE_SEQ_SERVER;
        
        int rc, portal = is_srv ?
                SEQ_SERVER_PORTAL : SEQ_CONTROLLER_PORTAL;

        struct ptlrpc_service_conf seq_conf = {
                .psc_nbufs = MDS_NBUFS,
                .psc_bufsize = MDS_BUFSIZE,
                .psc_max_req_size = SEQ_MAXREQSIZE,
                .psc_max_reply_size = SEQ_MAXREPSIZE,
                .psc_req_portal = portal,
                .psc_rep_portal = MDC_REPLY_PORTAL,
                .psc_watchdog_timeout = SEQ_SERVICE_WATCHDOG_TIMEOUT,
                .psc_num_threads = SEQ_NUM_THREADS,
                .psc_ctx_tags = LCT_MD_THREAD|LCT_DT_THREAD
        };
        ENTRY;

	LASSERT(dev != NULL);
        LASSERT(uuid != NULL);

        seq->lss_dev = dev;
        seq->lss_cli = NULL;
        seq->lss_type = type;
        sema_init(&seq->lss_sem, 1);

        seq->lss_super_width = LUSTRE_SEQ_SUPER_WIDTH;
        seq->lss_meta_width = LUSTRE_SEQ_META_WIDTH;

        snprintf(seq->lss_name, sizeof(seq->lss_name), "%s-%s-%s",
                 LUSTRE_SEQ_NAME, (is_srv ? "srv" : "ctl"),
                 uuid);

        seq->lss_space = LUSTRE_SEQ_SPACE_RANGE;
        seq->lss_super = LUSTRE_SEQ_ZERO_RANGE;

        lu_device_get(&seq->lss_dev->dd_lu_dev);

        rc = seq_store_init(seq, ctx);
        if (rc)
                GOTO(out, rc);

        /* request backing store for saved sequence info */
        rc = seq_store_read(seq, ctx);
        if (rc == -ENODATA) {
                CDEBUG(D_INFO|D_WARNING, "%s: no data on "
                       "storage was found, %s\n", seq->lss_name,
                       is_srv ? "wait for controller attach" :
                       "this is first controller run");
        } else if (rc) {
		CERROR("can't read sequence state, rc = %d\n",
		       rc);
		GOTO(out, rc);
	}

        rc  = seq_server_proc_init(seq);
        if (rc)
		GOTO(out, rc);

        seq->lss_service =  ptlrpc_init_svc_conf(&seq_conf,
						 seq_req_handle,
                                                 LUSTRE_SEQ_NAME,
						 seq->lss_proc_entry,
						 NULL);
	if (seq->lss_service != NULL)
		rc = ptlrpc_start_threads(NULL, seq->lss_service,
                                          LUSTRE_SEQ_NAME);
	else
		rc = -ENOMEM;

	EXIT;

out:
	if (rc) {
		seq_server_fini(seq, ctx);
        } else {
                CDEBUG(D_INFO|D_WARNING, "%s Sequence Manager\n",
                       (is_srv ? "Server" : "Controller"));
        }
	return rc;
}
EXPORT_SYMBOL(seq_server_init);

void seq_server_fini(struct lu_server_seq *seq,
                     const struct lu_context *ctx)
{
        ENTRY;

        if (seq->lss_service != NULL) {
                ptlrpc_unregister_service(seq->lss_service);
                seq->lss_service = NULL;
        }

        seq_server_proc_fini(seq);
        seq_store_fini(seq, ctx);

        if (seq->lss_dev != NULL) {
                lu_device_put(&seq->lss_dev->dd_lu_dev);
                seq->lss_dev = NULL;
        }

        EXIT;
}
EXPORT_SYMBOL(seq_server_fini);

static int fid_init(void)
{
	ENTRY;
	RETURN(0);
}

static int fid_fini(void)
{
	ENTRY;
	RETURN(0);
}

static int __init fid_mod_init(void)
{
        /* init caches if any */
        fid_init();
        return 0;
}

static void __exit fid_mod_exit(void)
{
        /* free caches if any */
        fid_fini();
        return;
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre FID Module");
MODULE_LICENSE("GPL");

cfs_module(fid, "0.1.0", fid_mod_init, fid_mod_exit);
#endif
