/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
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
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include <obd_support.h>
#include <lustre_lite.h>
#include <lustre_dlm.h>
#include <lustre_ver.h>
//#include <lustre_mdc.h>
#include <lustre_acl.h>

#include "llite_internal.h"

#define XATTR_USER_T            (1)
#define XATTR_TRUSTED_T         (2)
#define XATTR_SECURITY_T        (3)
#define XATTR_ACL_ACCESS_T      (4)
#define XATTR_ACL_DEFAULT_T     (5)
#define XATTR_LUSTRE_T          (6)
#define XATTR_OTHER_T           (7)

static
int get_xattr_type(const char *name)
{
        if (!strcmp(name, POSIX_ACL_XATTR_ACCESS))
                return XATTR_ACL_ACCESS_T;

        if (!strcmp(name, POSIX_ACL_XATTR_DEFAULT))
                return XATTR_ACL_DEFAULT_T;

        if (!strncmp(name, XATTR_USER_PREFIX,
                     sizeof(XATTR_USER_PREFIX) - 1))
                return XATTR_USER_T;

        if (!strncmp(name, XATTR_TRUSTED_PREFIX,
                     sizeof(XATTR_TRUSTED_PREFIX) - 1))
                return XATTR_TRUSTED_T;

        if (!strncmp(name, XATTR_SECURITY_PREFIX,
                     sizeof(XATTR_SECURITY_PREFIX) - 1))
                return XATTR_SECURITY_T;

        if (!strncmp(name, XATTR_LUSTRE_PREFIX,
                     sizeof(XATTR_LUSTRE_PREFIX) - 1))
                return XATTR_LUSTRE_T;

        return XATTR_OTHER_T;
}

static
int xattr_type_filter(struct ll_sb_info *sbi, int xattr_type)
{
        if ((xattr_type == XATTR_ACL_ACCESS_T ||
             xattr_type == XATTR_ACL_DEFAULT_T) &&
           !(sbi->ll_flags & LL_SBI_ACL))
                return -EOPNOTSUPP;

        if (xattr_type == XATTR_USER_T && !(sbi->ll_flags & LL_SBI_USER_XATTR))
                return -EOPNOTSUPP;
        if (xattr_type == XATTR_TRUSTED_T && !cfs_capable(CFS_CAP_SYS_ADMIN))
                return -EPERM;
        if (xattr_type == XATTR_OTHER_T)
                return -EOPNOTSUPP;

        return 0;
}

static
int ll_setxattr_common(struct inode *inode, const char *name,
                       const void *value, size_t size,
                       int flags, __u64 valid)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ptlrpc_request *req;
        int xattr_type, rc;
        struct obd_capa *oc;
        posix_acl_xattr_header *new_value = NULL;
        struct rmtacl_ctl_entry *rce = NULL;
        ext_acl_xattr_header *acl = NULL;
        const char *pv = value;
        ENTRY;

        xattr_type = get_xattr_type(name);
        rc = xattr_type_filter(sbi, xattr_type);
        if (rc)
                RETURN(rc);

        /* b10667: ignore lustre special xattr for now */
        if ((xattr_type == XATTR_TRUSTED_T && strcmp(name, "trusted.lov") == 0) ||
            (xattr_type == XATTR_LUSTRE_T && strcmp(name, "lustre.lov") == 0))
                RETURN(0);

#ifdef CONFIG_FS_POSIX_ACL
        if (sbi->ll_flags & LL_SBI_RMT_CLIENT &&
            (xattr_type == XATTR_ACL_ACCESS_T ||
            xattr_type == XATTR_ACL_DEFAULT_T)) {
                rce = rct_search(&sbi->ll_rct, cfs_curproc_pid());
                if (rce == NULL ||
                    (rce->rce_ops != RMT_LSETFACL &&
                    rce->rce_ops != RMT_RSETFACL))
                        RETURN(-EOPNOTSUPP);

                if (rce->rce_ops == RMT_LSETFACL) {
                        struct eacl_entry *ee;

                        ee = et_search_del(&sbi->ll_et, cfs_curproc_pid(),
                                           ll_inode2fid(inode), xattr_type);
                        LASSERT(ee != NULL);
                        if (valid & OBD_MD_FLXATTR) {
                                acl = lustre_acl_xattr_merge2ext(
                                                (posix_acl_xattr_header *)value,
                                                size, ee->ee_acl);
                                if (IS_ERR(acl)) {
                                        ee_free(ee);
                                        RETURN(PTR_ERR(acl));
                                }
                                size =  CFS_ACL_XATTR_SIZE(\
                                                le32_to_cpu(acl->a_count), \
                                                ext_acl_xattr);
                                pv = (const char *)acl;
                        }
                        ee_free(ee);
                } else if (rce->rce_ops == RMT_RSETFACL) {
                        size = lustre_posix_acl_xattr_filter(
                                                (posix_acl_xattr_header *)value,
                                                size, &new_value);
                        if (unlikely(size < 0))
                                RETURN(size);

                        pv = (const char *)new_value;
                } else
                        RETURN(-EOPNOTSUPP);

                valid |= rce_ops2valid(rce->rce_ops);
        }
#endif
        oc = ll_mdscapa_get(inode);
        rc = md_setxattr(sbi->ll_md_exp, ll_inode2fid(inode), oc,
                         valid, name, pv, size, 0, flags, ll_i2suppgid(inode),
                         &req);
        capa_put(oc);
#ifdef CONFIG_FS_POSIX_ACL
        if (new_value != NULL)
                lustre_posix_acl_xattr_free(new_value, size);
        if (acl != NULL)
                lustre_ext_acl_xattr_free(acl);
#endif
        if (rc) {
                if (rc == -EOPNOTSUPP && xattr_type == XATTR_USER_T) {
                        LCONSOLE_INFO("Disabling user_xattr feature because "
                                      "it is not supported on the server\n");
                        sbi->ll_flags &= ~LL_SBI_USER_XATTR;
                }
                RETURN(rc);
        }

        ptlrpc_req_finished(req);
        RETURN(0);
}

int ll_setxattr(struct dentry *dentry, const char *name,
                const void *value, size_t size, int flags)
{
        struct inode *inode = dentry->d_inode;

        LASSERT(inode);
        LASSERT(name);

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p), xattr %s\n",
               inode->i_ino, inode->i_generation, inode, name);

        ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_SETXATTR, 1);

        if ((strncmp(name, XATTR_TRUSTED_PREFIX,
                     sizeof(XATTR_TRUSTED_PREFIX) - 1) == 0 &&
             strcmp(name + sizeof(XATTR_TRUSTED_PREFIX) - 1, "lov") == 0) ||
            (strncmp(name, XATTR_LUSTRE_PREFIX,
                     sizeof(XATTR_LUSTRE_PREFIX) - 1) == 0 &&
             strcmp(name + sizeof(XATTR_LUSTRE_PREFIX) - 1, "lov") == 0)) {
                struct lov_user_md *lump = (struct lov_user_md *)value;
                int rc = 0;

                if (S_ISREG(inode->i_mode)) {
                        struct file f;
                        int flags = FMODE_WRITE;

                        f.f_dentry = dentry;
                        rc = ll_lov_setstripe_ea_info(inode, &f, flags,
                                                      lump, sizeof(*lump));
                        /* b10667: rc always be 0 here for now */
                        rc = 0;
                } else if (S_ISDIR(inode->i_mode)) {
                        rc = ll_dir_setstripe(inode, lump, 0);
                }

                return rc;

        } else if (strcmp(name, XATTR_NAME_LMA) == 0 ||
                   strcmp(name, XATTR_NAME_LINK) == 0)
                return 0;

        return ll_setxattr_common(inode, name, value, size, flags,
                                  OBD_MD_FLXATTR);
}

int ll_removexattr(struct dentry *dentry, const char *name)
{
        struct inode *inode = dentry->d_inode;

        LASSERT(inode);
        LASSERT(name);

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p), xattr %s\n",
               inode->i_ino, inode->i_generation, inode, name);

        ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_REMOVEXATTR, 1);
        return ll_setxattr_common(inode, name, NULL, 0, 0,
                                  OBD_MD_FLXATTRRM);
}

static
int ll_getxattr_common(struct inode *inode, const char *name,
                       void *buffer, size_t size, __u64 valid)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ptlrpc_request *req = NULL;
        struct mdt_body *body;
        int xattr_type, rc;
        void *xdata;
        struct obd_capa *oc;
        struct rmtacl_ctl_entry *rce = NULL;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p)\n",
               inode->i_ino, inode->i_generation, inode);

        /* listxattr have slightly different behavior from of ext3:
         * without 'user_xattr' ext3 will list all xattr names but
         * filtered out "^user..*"; we list them all for simplicity.
         */
        if (!name) {
                xattr_type = XATTR_OTHER_T;
                goto do_getxattr;
        }

        xattr_type = get_xattr_type(name);
        rc = xattr_type_filter(sbi, xattr_type);
        if (rc)
                RETURN(rc);

#ifdef CONFIG_FS_POSIX_ACL
        if (sbi->ll_flags & LL_SBI_RMT_CLIENT &&
            (xattr_type == XATTR_ACL_ACCESS_T ||
            xattr_type == XATTR_ACL_DEFAULT_T)) {
                rce = rct_search(&sbi->ll_rct, cfs_curproc_pid());
                if (rce == NULL ||
                    (rce->rce_ops != RMT_LSETFACL &&
                    rce->rce_ops != RMT_LGETFACL &&
                    rce->rce_ops != RMT_RSETFACL &&
                    rce->rce_ops != RMT_RGETFACL))
                        RETURN(-EOPNOTSUPP);
        }
#endif

        /* posix acl is under protection of LOOKUP lock. when calling to this,
         * we just have path resolution to the target inode, so we have great
         * chance that cached ACL is uptodate.
         */
#ifdef CONFIG_FS_POSIX_ACL
        if (xattr_type == XATTR_ACL_ACCESS_T &&
            !(sbi->ll_flags & LL_SBI_RMT_CLIENT)) {
                struct ll_inode_info *lli = ll_i2info(inode);
                struct posix_acl *acl;

                spin_lock(&lli->lli_lock);
                acl = posix_acl_dup(lli->lli_posix_acl);
                spin_unlock(&lli->lli_lock);

                if (!acl)
                        RETURN(-ENODATA);

                rc = posix_acl_to_xattr(acl, buffer, size);
                posix_acl_release(acl);
                RETURN(rc);
        }
        if (xattr_type == XATTR_ACL_DEFAULT_T && !S_ISDIR(inode->i_mode))
                RETURN(-ENODATA);
#endif

do_getxattr:
        oc = ll_mdscapa_get(inode);
        rc = md_getxattr(sbi->ll_md_exp, ll_inode2fid(inode), oc,
                         valid | (rce ? rce_ops2valid(rce->rce_ops) : 0),
                         name, NULL, 0, size, 0, &req);
        capa_put(oc);
        if (rc) {
                if (rc == -EOPNOTSUPP && xattr_type == XATTR_USER_T) {
                        LCONSOLE_INFO("Disabling user_xattr feature because "
                                      "it is not supported on the server\n");
                        sbi->ll_flags &= ~LL_SBI_USER_XATTR;
                }
                RETURN(rc);
        }

        body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);
        LASSERT(body);

        /* only detect the xattr size */
        if (size == 0)
                GOTO(out, rc = body->eadatasize);

        if (size < body->eadatasize) {
                CERROR("server bug: replied size %u > %u\n",
                       body->eadatasize, (int)size);
                GOTO(out, rc = -ERANGE);
        }

        /* do not need swab xattr data */
        xdata = req_capsule_server_sized_get(&req->rq_pill, &RMF_EADATA,
                                             body->eadatasize);
        if (!xdata)
                GOTO(out, rc = -EFAULT);

#ifdef CONFIG_FS_POSIX_ACL
        if (body->eadatasize >= 0 && rce && rce->rce_ops == RMT_LSETFACL) {
                ext_acl_xattr_header *acl;

                acl = lustre_posix_acl_xattr_2ext((posix_acl_xattr_header *)xdata,
                                                  body->eadatasize);
                if (IS_ERR(acl))
                        GOTO(out, rc = PTR_ERR(acl));

                rc = ee_add(&sbi->ll_et, cfs_curproc_pid(), ll_inode2fid(inode),
                            xattr_type, acl);
                if (unlikely(rc < 0)) {
                        lustre_ext_acl_xattr_free(acl);
                        GOTO(out, rc);
                }
        }

        if (xattr_type == XATTR_ACL_ACCESS_T && !body->eadatasize)
                GOTO(out, rc = -ENODATA);
#endif
        LASSERT(buffer);
        memcpy(buffer, xdata, body->eadatasize);
        rc = body->eadatasize;
        EXIT;
out:
        ptlrpc_req_finished(req);
        return rc;
}

ssize_t ll_getxattr(struct dentry *dentry, const char *name,
                    void *buffer, size_t size)
{
        struct inode *inode = dentry->d_inode;

        LASSERT(inode);
        LASSERT(name);

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p), xattr %s\n",
               inode->i_ino, inode->i_generation, inode, name);

        ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_GETXATTR, 1);

        if ((strncmp(name, XATTR_TRUSTED_PREFIX,
                     sizeof(XATTR_TRUSTED_PREFIX) - 1) == 0 &&
             strcmp(name + sizeof(XATTR_TRUSTED_PREFIX) - 1, "lov") == 0) ||
            (strncmp(name, XATTR_LUSTRE_PREFIX,
                     sizeof(XATTR_LUSTRE_PREFIX) - 1) == 0 &&
             strcmp(name + sizeof(XATTR_LUSTRE_PREFIX) - 1, "lov") == 0)) {
                struct lov_user_md *lump;
                struct lov_mds_md *lmm = NULL;
                struct ptlrpc_request *request = NULL;
                int rc = 0, lmmsize = 0;

                if (S_ISREG(inode->i_mode)) {
                        rc = ll_lov_getstripe_ea_info(dentry->d_parent->d_inode,
                                                      dentry->d_name.name, &lmm,
                                                      &lmmsize, &request);
                } else if (S_ISDIR(inode->i_mode)) {
                        rc = ll_dir_getstripe(inode, &lmm, &lmmsize, &request);
                } else {
                        rc = -ENODATA;
                }

                if (rc < 0)
                       GOTO(out, rc);
                if (size == 0)
                       GOTO(out, rc = lmmsize);

                if (size < lmmsize) {
                        CERROR("server bug: replied size %d > %d for %s (%s)\n",
                               lmmsize, (int)size, dentry->d_name.name, name);
                        GOTO(out, rc = -ERANGE);
                }

                lump = (struct lov_user_md *)buffer;
                memcpy(lump, lmm, lmmsize);

                rc = lmmsize;
out:
                ptlrpc_req_finished(request);
                return(rc);
        }

        return ll_getxattr_common(inode, name, buffer, size, OBD_MD_FLXATTR);
}

ssize_t ll_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
        struct inode *inode = dentry->d_inode;
        int rc = 0, rc2 = 0;
        struct lov_mds_md *lmm = NULL;
        struct ptlrpc_request *request = NULL;
        int lmmsize;

        LASSERT(inode);

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p)\n",
               inode->i_ino, inode->i_generation, inode);

        ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_LISTXATTR, 1);

        rc = ll_getxattr_common(inode, NULL, buffer, size, OBD_MD_FLXATTRLS);

        if (S_ISREG(inode->i_mode)) {
                struct ll_inode_info *lli = ll_i2info(inode);
                struct lov_stripe_md *lsm = NULL;
                lsm = lli->lli_smd;
                if (lsm == NULL)
                        rc2 = -1;
        } else if (S_ISDIR(inode->i_mode)) {
                rc2 = ll_dir_getstripe(inode, &lmm, &lmmsize, &request);
        }

        if (rc2 < 0) {
                GOTO(out, rc2 = 0);
        } else {
                const int prefix_len = sizeof(XATTR_LUSTRE_PREFIX) - 1;
                const size_t name_len   = sizeof("lov") - 1;
                const size_t total_len  = prefix_len + name_len + 1;

                if (buffer && (rc + total_len) <= size) {
                        buffer += rc;
                        memcpy(buffer,XATTR_LUSTRE_PREFIX, prefix_len);
                        memcpy(buffer+prefix_len, "lov", name_len);
                        buffer[prefix_len + name_len] = '\0';
                }
                rc2 = total_len;
        }
out:
        ptlrpc_req_finished(request);
        rc = rc + rc2;

        return rc;
}
