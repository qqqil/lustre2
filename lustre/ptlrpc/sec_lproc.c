/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2014, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ptlrpc/sec_lproc.c
 *
 * Author: Eric Mei <ericm@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_SEC

#include <libcfs/libcfs.h>
#include <linux/crypto.h>

#include <obd.h>
#include <obd_class.h>
#include <obd_support.h>
#include <lustre_net.h>
#include <lustre_import.h>
#include <lustre_dlm.h>
#include <lustre_sec.h>

#include "ptlrpc_internal.h"

static char *sec_flags2str(unsigned long flags, char *buf, int bufsize)
{
	buf[0] = '\0';

	if (flags & PTLRPC_SEC_FL_REVERSE)
		strlcat(buf, "reverse,", bufsize);
	if (flags & PTLRPC_SEC_FL_ROOTONLY)
		strlcat(buf, "rootonly,", bufsize);
	if (flags & PTLRPC_SEC_FL_UDESC)
		strlcat(buf, "udesc,", bufsize);
	if (flags & PTLRPC_SEC_FL_BULK)
		strlcat(buf, "bulk,", bufsize);
	if (buf[0] == '\0')
		strlcat(buf, "-,", bufsize);

	return buf;
}

static int sptlrpc_info_lprocfs_seq_show(struct seq_file *seq, void *v)
{
        struct obd_device *dev = seq->private;
        struct client_obd *cli = &dev->u.cli;
        struct ptlrpc_sec *sec = NULL;
        char               str[32];

	LASSERT(strcmp(dev->obd_type->typ_name, LUSTRE_OSC_NAME) == 0 ||
		strcmp(dev->obd_type->typ_name, LUSTRE_MDC_NAME) == 0 ||
		strcmp(dev->obd_type->typ_name, LUSTRE_MGC_NAME) == 0 ||
		strcmp(dev->obd_type->typ_name, LUSTRE_LWP_NAME) == 0 ||
		strcmp(dev->obd_type->typ_name, LUSTRE_OSP_NAME) == 0);

        if (cli->cl_import)
                sec = sptlrpc_import_sec_ref(cli->cl_import);
        if (sec == NULL)
                goto out;

        sec_flags2str(sec->ps_flvr.sf_flags, str, sizeof(str));

	seq_printf(seq, "rpc flavor:	%s\n",
		   sptlrpc_flavor2name_base(sec->ps_flvr.sf_rpc));
	seq_printf(seq, "bulk flavor:	%s\n",
		   sptlrpc_flavor2name_bulk(&sec->ps_flvr, str, sizeof(str)));
	seq_printf(seq, "flags:		%s\n",
		   sec_flags2str(sec->ps_flvr.sf_flags, str, sizeof(str)));
	seq_printf(seq, "id:		%d\n", sec->ps_id);
	seq_printf(seq, "refcount:	%d\n",
		   atomic_read(&sec->ps_refcount));
	seq_printf(seq, "nctx:	%d\n", atomic_read(&sec->ps_nctx));
	seq_printf(seq, "gc internal	%lld\n", sec->ps_gc_interval);
	seq_printf(seq, "gc next	%lld\n",
		   sec->ps_gc_interval ?
		   (s64)(sec->ps_gc_next - ktime_get_real_seconds()) : 0ll);

	sptlrpc_sec_put(sec);
out:
        return 0;
}

LPROC_SEQ_FOPS_RO(sptlrpc_info_lprocfs);

static int sptlrpc_ctxs_lprocfs_seq_show(struct seq_file *seq, void *v)
{
        struct obd_device *dev = seq->private;
        struct client_obd *cli = &dev->u.cli;
        struct ptlrpc_sec *sec = NULL;

	LASSERT(strcmp(dev->obd_type->typ_name, LUSTRE_OSC_NAME) == 0 ||
		strcmp(dev->obd_type->typ_name, LUSTRE_MDC_NAME) == 0 ||
		strcmp(dev->obd_type->typ_name, LUSTRE_MGC_NAME) == 0 ||
		strcmp(dev->obd_type->typ_name, LUSTRE_LWP_NAME) == 0 ||
		strcmp(dev->obd_type->typ_name, LUSTRE_OSP_NAME) == 0);

        if (cli->cl_import)
                sec = sptlrpc_import_sec_ref(cli->cl_import);
        if (sec == NULL)
                goto out;

        if (sec->ps_policy->sp_cops->display)
                sec->ps_policy->sp_cops->display(sec, seq);

        sptlrpc_sec_put(sec);
out:
        return 0;
}

LPROC_SEQ_FOPS_RO(sptlrpc_ctxs_lprocfs);

static ssize_t
lprocfs_sptlrpc_sepol_seq_write(struct file *file, const char __user *buffer,
				size_t count, void *data)
{
	struct seq_file	*seq = file->private_data;
	struct obd_device *dev = seq->private;
	struct client_obd *cli = &dev->u.cli;
	struct obd_import *imp = cli->cl_import;
	struct sepol_downcall_data *param;
	int size = sizeof(*param);
	int rc = 0;

	if (count < size) {
		CERROR("%s: invalid data count = %lu, size = %d\n",
		       dev->obd_name, (unsigned long) count, size);
		return -EINVAL;
	}

	OBD_ALLOC(param, size);
	if (param == NULL)
		return -ENOMEM;

	if (copy_from_user(param, buffer, size)) {
		CERROR("%s: bad sepol data\n", dev->obd_name);
		GOTO(out, rc = -EFAULT);
	}

	if (param->sdd_magic != SEPOL_DOWNCALL_MAGIC) {
		CERROR("%s: sepol downcall bad params\n",
		       dev->obd_name);
		GOTO(out, rc = -EINVAL);
	}

	if (param->sdd_sepol_len == 0 ||
	    param->sdd_sepol_len >= sizeof(imp->imp_sec->ps_sepol)) {
		CERROR("%s: invalid sepol data returned\n",
		       dev->obd_name);
		GOTO(out, rc = -EINVAL);
	}
	rc = param->sdd_sepol_len; /* save sdd_sepol_len */
	OBD_FREE(param, size);
	size = offsetof(struct sepol_downcall_data,
			sdd_sepol[rc]);

	/* alloc again with real size */
	rc = 0;
	OBD_ALLOC(param, size);
	if (param == NULL)
		return -ENOMEM;

	if (copy_from_user(param, buffer, size)) {
		CERROR("%s: bad sepol data\n", dev->obd_name);
		GOTO(out, rc = -EFAULT);
	}

	spin_lock(&imp->imp_sec->ps_lock);
	snprintf(imp->imp_sec->ps_sepol, param->sdd_sepol_len + 1, "%s",
		 param->sdd_sepol);
	imp->imp_sec->ps_sepol_mtime = param->sdd_sepol_mtime;
	spin_unlock(&imp->imp_sec->ps_lock);

out:
	if (param != NULL)
		OBD_FREE(param, size);

	return rc ? rc : count;
}
LPROC_SEQ_FOPS_WR_ONLY(srpc, sptlrpc_sepol);

int sptlrpc_lprocfs_cliobd_attach(struct obd_device *dev)
{
	int     rc;

	if (strcmp(dev->obd_type->typ_name, LUSTRE_OSC_NAME) != 0 &&
	    strcmp(dev->obd_type->typ_name, LUSTRE_MDC_NAME) != 0 &&
	    strcmp(dev->obd_type->typ_name, LUSTRE_MGC_NAME) != 0 &&
	    strcmp(dev->obd_type->typ_name, LUSTRE_LWP_NAME) != 0 &&
	    strcmp(dev->obd_type->typ_name, LUSTRE_OSP_NAME) != 0) {
		CERROR("can't register lproc for obd type %s\n",
		       dev->obd_type->typ_name);
		return -EINVAL;
	}

	rc = ldebugfs_seq_create(dev->obd_debugfs_entry, "srpc_info", 0444,
				 &sptlrpc_info_lprocfs_fops, dev);
	if (rc) {
		CERROR("create proc entry srpc_info for %s: %d\n",
		       dev->obd_name, rc);
		return rc;
	}

	rc = ldebugfs_seq_create(dev->obd_debugfs_entry, "srpc_contexts",
				 0444, &sptlrpc_ctxs_lprocfs_fops, dev);
	if (rc) {
		CERROR("create proc entry srpc_contexts for %s: %d\n",
		       dev->obd_name, rc);
		return rc;
	}

	rc = ldebugfs_seq_create(dev->obd_debugfs_entry, "srpc_sepol",
				 0200, &srpc_sptlrpc_sepol_fops, dev);
	if (rc) {
		CERROR("create proc entry srpc_sepol for %s: %d\n",
		       dev->obd_name, rc);
		return rc;
	}

	return 0;
}
EXPORT_SYMBOL(sptlrpc_lprocfs_cliobd_attach);

LDEBUGFS_SEQ_FOPS_RO(sptlrpc_proc_enc_pool);

static struct lprocfs_vars sptlrpc_lprocfs_vars[] = {
	{ .name	=	"encrypt_page_pools",
	  .fops	=	&sptlrpc_proc_enc_pool_fops	},
	{ NULL }
};

struct dentry *sptlrpc_debugfs_dir;
EXPORT_SYMBOL(sptlrpc_debugfs_dir);

int sptlrpc_lproc_init(void)
{
	int rc;

	LASSERT(sptlrpc_debugfs_dir == NULL);

	sptlrpc_debugfs_dir = ldebugfs_register("sptlrpc", debugfs_lustre_root,
						sptlrpc_lprocfs_vars, NULL);
	if (IS_ERR_OR_NULL(sptlrpc_debugfs_dir)) {
		rc = sptlrpc_debugfs_dir ? PTR_ERR(sptlrpc_debugfs_dir)
					 : -ENOMEM;
		sptlrpc_debugfs_dir = NULL;
		return rc;
	}
	return 0;
}

void sptlrpc_lproc_fini(void)
{
	if (!IS_ERR_OR_NULL(sptlrpc_debugfs_dir))
		ldebugfs_remove(&sptlrpc_debugfs_dir);
}
