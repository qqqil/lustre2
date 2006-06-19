/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/mgs/mgs_llog.c
 *  Lustre Management Server (mgs) config llog creation
 *
 *  Copyright (C) 2006 Cluster File Systems, Inc.
 *   Author: Nathan Rutman <nathan@clusterfs.com>
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
 */

#ifndef EXPORT_SYMTAB
#define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MGS
#define D_MGS D_CONFIG /*|D_WARNING*/

#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#endif

#include <obd.h>
#include <obd_class.h>
#include <lustre_log.h>
#include <obd_ost.h>
#include <libcfs/list.h>
#include <linux/lvfs.h>
#include <lustre_fsfilt.h>
#include <lustre_disk.h>
#include <lustre_param.h>
#include "mgs_internal.h"

/********************** Class fns ********************/

static int class_dentry_readdir(struct obd_device *obd, struct dentry *dir,
                                struct vfsmount *inmnt, 
                                struct list_head *dentry_list){
        /* see mds_cleanup_pending */
        struct lvfs_run_ctxt saved;
        struct file *file;
        struct dentry *dentry;
        struct vfsmount *mnt;
        int rc = 0;
        ENTRY;
                                                                                
        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        dentry = dget(dir);
        if (IS_ERR(dentry))
                GOTO(out_pop, rc = PTR_ERR(dentry));
        mnt = mntget(inmnt);
        if (IS_ERR(mnt)) {
                l_dput(dentry);
                GOTO(out_pop, rc = PTR_ERR(mnt));
        }

        file = dentry_open(dentry, mnt, O_RDONLY);
        if (IS_ERR(file))
                /* dentry_open_it() drops the dentry, mnt refs */
                GOTO(out_pop, rc = PTR_ERR(file));
                                                                                
        INIT_LIST_HEAD(dentry_list);
        rc = l_readdir(file, dentry_list);
        filp_close(file, 0);
        /*  filp_close->fput() drops the dentry, mnt refs */
                                                                                
out_pop:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        RETURN(rc);
}

/******************** DB functions *********************/

static inline int name_create(char **newname, char *prefix, char *suffix)
{
        LASSERT(newname);
        OBD_ALLOC(*newname, strlen(prefix) + strlen(suffix) + 1);
        if (!*newname) 
                return -ENOMEM;
        sprintf(*newname, "%s%s", prefix, suffix);
        return 0;
}

static inline void name_destroy(char *name)
{        
        if (name)
                OBD_FREE(name, strlen(name) + 1);
}

/* from the (client) config log, figure out:
        1. which ost's/mdt's are configured (by index)
        2. what the last config step is
        3. COMPAT_146 lov name
        4. COMPAT_146 mdt lov name
        5. COMPAT_146 mdc name 
*/
/* It might be better to have a separate db file, instead of parsing the info
   out of the client log.  This is slow and potentially error-prone. */
static int mgs_fsdb_handler(struct llog_handle *llh, struct llog_rec_hdr *rec, 
                            void *data)
{
        struct fs_db *fsdb = (struct fs_db *)data;
        int cfg_len = rec->lrh_len;
        char *cfg_buf = (char*) (rec + 1);
        struct lustre_cfg *lcfg;
        __u32 index;
        int rc = 0;
        ENTRY;

        if (rec->lrh_type != OBD_CFG_REC) {
                CERROR("unhandled lrh_type: %#x\n", rec->lrh_type);
                RETURN(-EINVAL);
        }

        rc = lustre_cfg_sanity_check(cfg_buf, cfg_len);
        if (rc) {
                CERROR("Insane cfg\n");
                RETURN(rc);
        }

        lcfg = (struct lustre_cfg *)cfg_buf;

        CDEBUG(D_INFO, "cmd %x %s %s\n", lcfg->lcfg_command, 
               lustre_cfg_string(lcfg, 0), lustre_cfg_string(lcfg, 1));

        /* Figure out ost indicies */ 
        /* lov_modify_tgts add 0:lov1  1:ost1_UUID  2(index):0  3(gen):1 */
        if (lcfg->lcfg_command == LCFG_LOV_ADD_OBD ||
            lcfg->lcfg_command == LCFG_LOV_DEL_OBD) {
                index = simple_strtoul(lustre_cfg_string(lcfg, 2),
                                       NULL, 10);
                CDEBUG(D_MGS, "OST index for %s is %u (%s)\n",
                       lustre_cfg_string(lcfg, 1), index, 
                       lustre_cfg_string(lcfg, 2));
                set_bit(index, fsdb->fsdb_ost_index_map);
        }
        
        /* Figure out mdt indicies */
        /* attach   0:MDC_uml1_mdsA_MNT_client  1:mdc  2:1d834_MNT_client_03f */
        if ((lcfg->lcfg_command == LCFG_ATTACH) &&
            (strcmp(lustre_cfg_string(lcfg, 1), LUSTRE_MDC_NAME) == 0)) {
                rc = server_name2index(lustre_cfg_string(lcfg, 0),
                                       &index, NULL);
                if (rc != LDD_F_SV_TYPE_MDT) {
                        CWARN("Unparsable MDC name %s, assuming index 0\n",
                              lustre_cfg_string(lcfg, 0));
                        index = 0;
                }
                rc = 0;
                CDEBUG(D_MGS, "MDT index is %u\n", index);
                set_bit(index, fsdb->fsdb_mdt_index_map);
        }

        /* COMPAT_146 */
        /* figure out the old LOV name. fsdb_gen = 0 means old log */
        if ((fsdb->fsdb_gen == 0) && (lcfg->lcfg_command == LCFG_ATTACH) &&
            (strcmp(lustre_cfg_string(lcfg, 1), LUSTRE_LOV_NAME) == 0)) {
                fsdb->fsdb_flags |= FSDB_OLDLOG14;
                name_destroy(fsdb->fsdb_clilov);
                rc = name_create(&fsdb->fsdb_clilov, 
                                 lustre_cfg_string(lcfg, 0), "");
                if (rc) 
                        RETURN(rc);
                CDEBUG(D_MGS, "client lov name is %s\n", fsdb->fsdb_clilov);
        }

        /* figure out the old MDT lov name from the MDT uuid */
        if ((fsdb->fsdb_gen == 0) && (lcfg->lcfg_command == LCFG_SETUP) &&
            (strncmp(lustre_cfg_string(lcfg, 0), "MDC_", 4) == 0)) {
                char *ptr;
                fsdb->fsdb_flags |= FSDB_OLDLOG14;
                ptr = strstr(lustre_cfg_string(lcfg, 1), "_UUID");
                if (!ptr) {
                        CERROR("Can't parse MDT uuid %s\n", 
                               lustre_cfg_string(lcfg, 1));
                        RETURN(-EINVAL);
                }
                *ptr = '\0';
                name_destroy(fsdb->fsdb_mdtlov);
                rc = name_create(&fsdb->fsdb_mdtlov, 
                                 "lov_", lustre_cfg_string(lcfg, 1));
                if (rc) 
                        RETURN(rc);
                rc = name_create(&fsdb->fsdb_mdc, 
                                 lustre_cfg_string(lcfg, 0), "");
                if (rc) 
                        RETURN(rc);
                CDEBUG(D_MGS, "MDT lov name is %s\n", fsdb->fsdb_mdtlov);
        }
        /* end COMPAT_146 */

        /* Keep track of the latest marker step */
        if (lcfg->lcfg_command == LCFG_MARKER) {
                struct cfg_marker *marker;
                marker = lustre_cfg_buf(lcfg, 1);
                fsdb->fsdb_gen = max(fsdb->fsdb_gen, marker->cm_step);
        }

        RETURN(rc);
}

static int mgs_get_fsdb_from_llog(struct obd_device *obd, char *logname,
                                struct fs_db *fsdb)
{
        struct llog_handle *loghandle;
        struct lvfs_run_ctxt saved;
        int rc, rc2;
        ENTRY;

        down(&fsdb->fsdb_sem);
        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        
        rc = llog_create(llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT),
                         &loghandle, NULL, logname);
        if (rc)
                GOTO(out_pop, rc);

        rc = llog_init_handle(loghandle, LLOG_F_IS_PLAIN, NULL);
        if (rc)
                GOTO(out_close, rc);

        if (llog_get_size(loghandle) <= 1)
                fsdb->fsdb_flags |= FSDB_EMPTY;

        rc = llog_process(loghandle, mgs_fsdb_handler, (void *)fsdb, NULL);
        CDEBUG(D_INFO, "get_db = %d\n", rc);
out_close:
        rc2 = llog_close(loghandle);
        if (!rc)
                rc = rc2;

out_pop:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        up(&fsdb->fsdb_sem);

        RETURN(rc);
}

static int next_index(void *index_map, int map_len)
{
        int i;
        for (i = 0; i < map_len * 8; i++)
                 if (!test_bit(i, index_map)) {
                         return i;
                 }
        CERROR("max index %d exceeded.\n", i);
        return -1;
}

#if 0
static int count_osts(void *index_map, int map_len)
{
       int i, num;
       for (i = 0, num = 0; i < map_len * 8; i++)
               if (test_bit(i, index_map))
                        num++;
       return num;
}
#endif

static struct fs_db *mgs_find_fsdb(struct obd_device *obd, char *fsname)
{
        struct mgs_obd *mgs = &obd->u.mgs;
        struct fs_db *fsdb;
        struct list_head *tmp;

        list_for_each(tmp, &mgs->mgs_fs_db_list) {
                fsdb = list_entry(tmp, struct fs_db, fsdb_list);
                if (strcmp(fsdb->fsdb_name, fsname) == 0)
                        return fsdb;
        }
        return NULL;
}

#define INDEX_MAP_SIZE 4096

/* caller must hold the mgs->mgs_fs_db_lock */
static struct fs_db *mgs_new_fsdb(struct obd_device *obd, char *fsname)
{
        struct mgs_obd *mgs = &obd->u.mgs;
        struct fs_db *fsdb;
        int rc;
        ENTRY;
        
        OBD_ALLOC_PTR(fsdb);
        if (!fsdb) 
                RETURN(NULL);

        OBD_ALLOC(fsdb->fsdb_ost_index_map, INDEX_MAP_SIZE);
        OBD_ALLOC(fsdb->fsdb_mdt_index_map, INDEX_MAP_SIZE);
        if (!fsdb->fsdb_ost_index_map || !fsdb->fsdb_mdt_index_map) {
                CERROR("No memory for index maps\n");
                GOTO(err, 0);
        }
        
        strncpy(fsdb->fsdb_name, fsname, sizeof(fsdb->fsdb_name));
        rc = name_create(&fsdb->fsdb_mdtlov, fsname, "-mdtlov");
        if (rc) 
                GOTO(err, rc);
        rc = name_create(&fsdb->fsdb_clilov, fsname, "-clilov");
        if (rc) 
                GOTO(err, rc);

        sema_init(&fsdb->fsdb_sem, 1);
        list_add(&fsdb->fsdb_list, &mgs->mgs_fs_db_list);

        RETURN(fsdb);
err:
        if (fsdb->fsdb_ost_index_map) 
                OBD_FREE(fsdb->fsdb_ost_index_map, INDEX_MAP_SIZE);
        if (fsdb->fsdb_mdt_index_map) 
                OBD_FREE(fsdb->fsdb_mdt_index_map, INDEX_MAP_SIZE);
        name_destroy(fsdb->fsdb_clilov); 
        name_destroy(fsdb->fsdb_mdtlov); 
        OBD_FREE_PTR(fsdb);
        RETURN(NULL);
}

static void mgs_free_fsdb(struct fs_db *fsdb)
{
        /* wait for anyone with the sem */
        down(&fsdb->fsdb_sem);
        list_del(&fsdb->fsdb_list);
        OBD_FREE(fsdb->fsdb_ost_index_map, INDEX_MAP_SIZE);
        OBD_FREE(fsdb->fsdb_mdt_index_map, INDEX_MAP_SIZE);
        name_destroy(fsdb->fsdb_clilov); 
        name_destroy(fsdb->fsdb_mdtlov); 
        name_destroy(fsdb->fsdb_mdc); 
        OBD_FREE_PTR(fsdb);
}

int mgs_init_fsdb_list(struct obd_device *obd)
{
        struct mgs_obd *mgs = &obd->u.mgs;
        INIT_LIST_HEAD(&mgs->mgs_fs_db_list);
        return 0;
}

int mgs_cleanup_fsdb_list(struct obd_device *obd)
{
        struct mgs_obd *mgs = &obd->u.mgs;
        struct fs_db *fsdb;
        struct list_head *tmp, *tmp2;
        down(&mgs->mgs_sem);
        list_for_each_safe(tmp, tmp2, &mgs->mgs_fs_db_list) {
                fsdb = list_entry(tmp, struct fs_db, fsdb_list);
                mgs_free_fsdb(fsdb);
        }
        up(&mgs->mgs_sem);
        return 0;
}

static int mgs_find_or_make_fsdb(struct obd_device *obd, char *name, 
                               struct fs_db **dbh)
{
        struct mgs_obd *mgs = &obd->u.mgs;
        struct fs_db *fsdb;
        char *cliname;
        int rc = 0;

        down(&mgs->mgs_sem);
        fsdb = mgs_find_fsdb(obd, name);
        if (fsdb) {
                up(&mgs->mgs_sem);
                *dbh = fsdb;
                return 0;
        }

        CDEBUG(D_MGS, "Creating new db\n");
        fsdb = mgs_new_fsdb(obd, name);
        up(&mgs->mgs_sem);
        if (!fsdb) 
                return -ENOMEM;

        /* populate the db from the client llog */
        name_create(&cliname, name, "-client");
        rc = mgs_get_fsdb_from_llog(obd, cliname, fsdb);
        name_destroy(cliname);
        if (rc) {
                CERROR("Can't get db from llog %d\n", rc);
                mgs_free_fsdb(fsdb);
                return rc;
        }

        *dbh = fsdb;
        
        return 0;
}

/* 1 = index in use
   0 = index unused 
   -1= empty client log */
int mgs_check_index(struct obd_device *obd, struct mgs_target_info *mti)
{
        struct fs_db *fsdb;
        void *imap;
        int rc = 0;
        ENTRY;

        LASSERT(!(mti->mti_flags & LDD_F_NEED_INDEX));

        rc = mgs_find_or_make_fsdb(obd, mti->mti_fsname, &fsdb); 
        if (rc) {
                CERROR("Can't get db for %s\n", mti->mti_fsname);
                RETURN(rc);
        }

        if (fsdb->fsdb_flags & FSDB_EMPTY) 
                RETURN(-1);

        if (mti->mti_flags & LDD_F_SV_TYPE_OST) 
                imap = fsdb->fsdb_ost_index_map;
        else if (mti->mti_flags & LDD_F_SV_TYPE_MDT) 
                imap = fsdb->fsdb_mdt_index_map;
        else
                RETURN(-EINVAL);

        if (test_bit(mti->mti_stripe_index, imap)) 
                RETURN(1);
        RETURN(0);
}


int mgs_set_index(struct obd_device *obd, struct mgs_target_info *mti)
{
        struct fs_db *fsdb;
        void *imap;
        int rc = 0;
        ENTRY;

        rc = mgs_find_or_make_fsdb(obd, mti->mti_fsname, &fsdb); 
        if (rc) {
                CERROR("Can't get db for %s\n", mti->mti_fsname);
                RETURN(rc);
        }

        if (mti->mti_flags & LDD_F_SV_TYPE_OST) 
                imap = fsdb->fsdb_ost_index_map;
        else if (mti->mti_flags & LDD_F_SV_TYPE_MDT) 
                imap = fsdb->fsdb_mdt_index_map;
        else
                RETURN(-EINVAL);

        if (mti->mti_flags & LDD_F_NEED_INDEX) {
                rc = next_index(imap, INDEX_MAP_SIZE);
                if (rc == -1)
                        RETURN(-ERANGE);
                mti->mti_stripe_index = rc;
        }

        /* Remove after CMD */
        if ((mti->mti_flags & LDD_F_SV_TYPE_MDT) && 
            (mti->mti_stripe_index > 0)) {
                LCONSOLE_ERROR("MDT index must = 0 (until Clustered MetaData "
                               "feature is ready.)\n");
                mti->mti_stripe_index = 0;
        }

        if (mti->mti_stripe_index >= INDEX_MAP_SIZE * 8) {
                LCONSOLE_ERROR("Server %s requested index %d, but the"
                               "max index is %d.\n", 
                               mti->mti_svname, mti->mti_stripe_index,
                               INDEX_MAP_SIZE * 8);
                RETURN(-ERANGE);
        }
         
        if (test_bit(mti->mti_stripe_index, imap)) {
                if (mti->mti_flags & LDD_F_VIRGIN) {
                        LCONSOLE_ERROR("Server %s requested index %d, but that "
                                       "index is already in use\n",
                                       mti->mti_svname, mti->mti_stripe_index);
                        RETURN(-EADDRINUSE);
                } else {
                        CDEBUG(D_MGS, "Server %s updating index %d\n",
                               mti->mti_svname, mti->mti_stripe_index);
                        RETURN(EALREADY);
                }
        }

        set_bit(mti->mti_stripe_index, imap);
        fsdb->fsdb_flags &= ~FSDB_EMPTY;
        server_make_name(mti->mti_flags, mti->mti_stripe_index,
                         mti->mti_fsname, mti->mti_svname);

        CDEBUG(D_MGS, "Set index for %s to %d\n", mti->mti_svname, 
               mti->mti_stripe_index);

        RETURN(0);
}
                           
/******************** config log recording functions *********************/

static int record_lcfg(struct obd_device *obd, struct llog_handle *llh,
                         struct lustre_cfg *lcfg)
{
        struct lvfs_run_ctxt   saved;
        struct llog_rec_hdr    rec;
        int buflen, rc;

        LASSERT(llh);
        LASSERT(llh->lgh_ctxt);        

        buflen = lustre_cfg_len(lcfg->lcfg_bufcount,
                                lcfg->lcfg_buflens);
        rec.lrh_len = llog_data_len(buflen);
        rec.lrh_type = OBD_CFG_REC;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        /* idx = -1 means append */
        rc = llog_write_rec(llh, &rec, NULL, 0, (void *)lcfg, -1);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        if (rc) {
                CERROR("failed %d\n", rc);
        }
        LASSERT(!rc);
        return rc;
}

static int record_base(struct obd_device *obd, struct llog_handle *llh,
                     char *cfgname, lnet_nid_t nid, int cmd,
                     char *s1, char *s2, char *s3, char *s4)
{
        struct lustre_cfg_bufs bufs;
        struct lustre_cfg     *lcfg;
        int rc;
               
        CDEBUG(D_MGS, "lcfg %s %#x %s %s %s %s\n", cfgname,
               cmd, s1, s2, s3, s4); 

        lustre_cfg_bufs_reset(&bufs, cfgname);
        if (s1) 
                lustre_cfg_bufs_set_string(&bufs, 1, s1);
        if (s2) 
                lustre_cfg_bufs_set_string(&bufs, 2, s2);
        if (s3) 
                lustre_cfg_bufs_set_string(&bufs, 3, s3);
        if (s4) 
                lustre_cfg_bufs_set_string(&bufs, 4, s4);

        lcfg = lustre_cfg_new(cmd, &bufs);
        lcfg->lcfg_nid = nid;

        rc = record_lcfg(obd, llh, lcfg);
        
        lustre_cfg_free(lcfg);
        
        if (rc) {
                CERROR("error %d: lcfg %s %#x %s %s %s %s\n", rc, cfgname,
                       cmd, s1, s2, s3, s4); 
        }
        return(rc);
}


static inline int record_add_uuid(struct obd_device *obd, 
                                  struct llog_handle *llh, 
                                  uint64_t nid, char *uuid)
{
        return record_base(obd,llh,NULL,nid,LCFG_ADD_UUID,uuid,0,0,0);

}

static inline int record_add_conn(struct obd_device *obd, 
                                  struct llog_handle *llh,
                                  char *devname,
                                  char *uuid)
{
        return record_base(obd,llh,devname,0,LCFG_ADD_CONN,uuid,0,0,0);
}

static inline int record_attach(struct obd_device *obd, struct llog_handle *llh,
                                char *devname, char *type, char *uuid)
{
        return record_base(obd,llh,devname,0,LCFG_ATTACH,type,uuid,0,0);
}

static inline int record_setup(struct obd_device *obd, struct llog_handle *llh,
                               char *devname, 
                               char *s1, char *s2, char *s3, char *s4)
{
        return record_base(obd,llh,devname,0,LCFG_SETUP,s1,s2,s3,s4);
}

static int record_lov_setup(struct obd_device *obd, struct llog_handle *llh,
                            char *devname, struct lov_desc *desc)
{
        struct lustre_cfg_bufs bufs;
        struct lustre_cfg *lcfg;
        int rc;

        lustre_cfg_bufs_reset(&bufs, devname);
        lustre_cfg_bufs_set(&bufs, 1, desc, sizeof(*desc));
        lcfg = lustre_cfg_new(LCFG_SETUP, &bufs);

        rc = record_lcfg(obd, llh, lcfg);

        lustre_cfg_free(lcfg);
        return rc;
}

static inline int record_lov_add(struct obd_device *obd,
                                 struct llog_handle *llh,
                                 char *lov_name, char *ost_uuid,
                                 char *index, char *gen)
{
        return record_base(obd,llh,lov_name,0,LCFG_LOV_ADD_OBD,
                           ost_uuid,index,gen,0);
}                                  

static inline int record_mount_opt(struct obd_device *obd, 
                                   struct llog_handle *llh,
                                   char *profile, char *lov_name,
                                   char *mdc_name)
{
        return record_base(obd,llh,NULL,0,LCFG_MOUNTOPT,
                           profile,lov_name,mdc_name,0);
}                

static int record_marker(struct obd_device *obd, struct llog_handle *llh,
                         struct fs_db *fsdb, __u32 flags,
                         char *svname, char *comment)
{
        struct cfg_marker marker;
        struct timeval tv;
        struct lustre_cfg_bufs bufs;
        struct lustre_cfg *lcfg;
        int rc;

        if (flags & CM_START) 
                fsdb->fsdb_gen++;
        marker.cm_step = fsdb->fsdb_gen;
        marker.cm_flags = flags;
        strncpy(marker.cm_svname, svname, sizeof(marker.cm_svname)); 
        strncpy(marker.cm_comment, comment, sizeof(marker.cm_comment)); 
        do_gettimeofday(&tv);
        marker.cm_createtime = tv.tv_sec;
        marker.cm_canceltime = 0;
        lustre_cfg_bufs_reset(&bufs, NULL);
        lustre_cfg_bufs_set(&bufs, 1, &marker, sizeof(marker));
        lcfg = lustre_cfg_new(LCFG_MARKER, &bufs);

        rc = record_lcfg(obd, llh, lcfg);

        lustre_cfg_free(lcfg);
        return rc;
}

static int record_start_log(struct obd_device *obd, 
                            struct llog_handle **llh, char *name)
{
        static struct obd_uuid cfg_uuid = { .uuid = "config_uuid" };
        struct lvfs_run_ctxt saved;
        int rc = 0;
        
        if (*llh) {
                GOTO(out, rc = -EBUSY);
        }

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        rc = llog_create(llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT),
                         llh, NULL, name);
        if (rc == 0)
                llog_init_handle(*llh, LLOG_F_IS_PLAIN, &cfg_uuid);
        else
                *llh = NULL;

        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

out:
        if (rc) {
                CERROR("Can't start log %s: %d\n", name, rc);
        }
        RETURN(rc);
}

static int record_end_log(struct obd_device *obd, struct llog_handle **llh)
{
        struct lvfs_run_ctxt saved;
        int rc = 0;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        
        rc = llog_close(*llh);
        *llh = NULL;
        
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        RETURN(rc);
}

static int mgs_log_is_empty(struct obd_device *obd, char *name)
{
        struct lvfs_run_ctxt saved;
        struct llog_handle *llh;
        int rc = 0;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        rc = llog_create(llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT),
                         &llh, NULL, name);
        if (rc == 0) {
                llog_init_handle(llh, LLOG_F_IS_PLAIN, NULL);
                rc = llog_get_size(llh);
                llog_close(llh);
        }
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        /* header is record 1 */
        return(rc <= 1);
}

/******************** config "macros" *********************/

/* write an lcfg directly into a log (with markers) */
static int mgs_write_log_direct(struct obd_device *obd, struct fs_db *fsdb,
                                char *logname, char *obdname,  
                                struct lustre_cfg *lcfg)
{
        struct llog_handle *llh = NULL;
        int rc;
        ENTRY;

        rc = record_start_log(obd, &llh, logname);
        rc = record_marker(obd, llh, fsdb, CM_START, obdname, "param"); 
        
        rc = record_lcfg(obd, llh, lcfg);

        rc = record_marker(obd, llh, fsdb, CM_END, obdname, "param"); 
        rc = record_end_log(obd, &llh);
        
        RETURN(rc);
}

/* write the lcfg in all logs for the given fs */
int mgs_write_log_direct_all(struct obd_device *obd, struct fs_db *fsdb,
                           char *fsname, struct lustre_cfg *lcfg)
{
        struct mgs_obd *mgs = &obd->u.mgs;
        struct list_head dentry_list;
        struct l_linux_dirent *dirent, *n;
        char *logname;
        int rc, len = strlen(fsname);
        ENTRY;
        
        /* We need to set params for any future logs 
           as well. FIXME Append this file to every new log. 
           Actually, we should store as params (text), not llogs.  Or
           in a database. */
        name_create(&logname, fsname, "-params");
        if (mgs_log_is_empty(obd, logname)) {
                struct llog_handle *llh = NULL;
                rc = record_start_log(obd, &llh, logname);
                rc = record_end_log(obd, &llh);
        }
        name_destroy(logname);

        /* Find all the logs in the CONFIGS directory */
        rc = class_dentry_readdir(obd, mgs->mgs_configs_dir,
                                  mgs->mgs_vfsmnt, &dentry_list);
        if (rc) {
                CERROR("Can't read %s dir\n", MOUNT_CONFIGS_DIR);
                RETURN(rc);
        }
                                                                                
        /* Could use fsdb index maps instead of directory listing */
        list_for_each_entry_safe(dirent, n, &dentry_list, lld_list) {
                list_del(&dirent->lld_list);
                if (strncmp(fsname, dirent->lld_name, len) == 0) {
                        CDEBUG(D_MGS, "Changing log %s\n", dirent->lld_name);
                        rc = mgs_write_log_direct(obd, fsdb, dirent->lld_name,
                                                  dirent->lld_name, lcfg);
                }
                OBD_FREE(dirent, sizeof(*dirent));
        }
        
        RETURN(rc);
}

/* lov is the first thing in the mdt and client logs */
static int mgs_write_log_lov(struct obd_device *obd, struct fs_db *fsdb, 
                             struct mgs_target_info *mti,
                             char *logname, char *lovname)
{
        struct llog_handle *llh = NULL;
        struct lov_desc *lovdesc;
        char *uuid;
        int rc = 0;
        ENTRY;

        CDEBUG(D_MGS, "Writing log %s\n", logname);

        /*
        #01 L attach   0:lov_mdsA  1:lov  2:71ccb_lov_mdsA_19f961a9e1
        #02 L lov_setup 0:lov_mdsA 1:(struct lov_desc)
              uuid=lov1_UUID, stripe count=1, size=1048576, offset=0, pattern=0
        */

        /* FIXME just make lov_setup accept empty desc (put uuid in buf 2) */
        OBD_ALLOC(lovdesc, sizeof(*lovdesc));
        if (lovdesc == NULL)
                RETURN(-ENOMEM);
        lovdesc->ld_magic = LOV_DESC_MAGIC;
        lovdesc->ld_tgt_count = 0;
        /* Defaults.  Can be changed later by lcfg config_param */ 
        lovdesc->ld_default_stripe_count = 1;
        lovdesc->ld_pattern = LOV_PATTERN_RAID0;
        lovdesc->ld_default_stripe_size = 1024 * 1024;
        lovdesc->ld_default_stripe_offset = 0;
        sprintf((char*)lovdesc->ld_uuid.uuid, "%s_UUID", lovname);
        /* can these be the same? */
        uuid = (char *)lovdesc->ld_uuid.uuid;

        /* This should always be the first entry in a log.
        rc = mgs_clear_log(obd, logname); */
        rc = record_start_log(obd, &llh, logname);
        rc = record_marker(obd, llh, fsdb, CM_START, lovname, "lov setup"); 
        rc = record_attach(obd, llh, lovname, "lov", uuid);
        rc = record_lov_setup(obd, llh, lovname, lovdesc);
        rc = record_marker(obd, llh, fsdb, CM_END, lovname, "lov setup"); 
        rc = record_end_log(obd, &llh);
        
        OBD_FREE(lovdesc, sizeof(*lovdesc));
        RETURN(rc);
}

/* add failnids to open log */
static int mgs_write_log_failnids(struct obd_device *obd,
                                  struct mgs_target_info *mti,
                                  struct llog_handle *llh,
                                  char *cliname)
{
        char *failnodeuuid = NULL;
        char *ptr = mti->mti_params;
        lnet_nid_t nid;
        int rc = 0;

        /*
        #03 L add_uuid  nid=uml1@tcp(0x20000c0a80201) nal=90 0:  1:uml1_UUID
        #04 L add_uuid  nid=1@elan(0x1000000000001)   nal=90 0:  1:uml1_UUID
        #05 L setup    0:OSC_uml1_ost1_mdsA  1:ost1_UUID  2:uml1_UUID
        #06 L add_uuid  nid=uml2@tcp(0x20000c0a80202) nal=90 0:  1:uml2_UUID
        #0x L add_uuid  nid=2@elan(0x1000000000002)   nal=90 0:  1:uml2_UUID
        #07 L add_conn 0:OSC_uml1_ost1_mdsA  1:uml2_UUID
        */

        /* Pull failnid info out of params string */
        while (class_find_param(ptr, PARAM_FAILNODE, &ptr) == 0) {
                while (class_parse_nid(ptr, &nid, &ptr) == 0) {
                        if (failnodeuuid == NULL) {
                                /* We don't know the failover node name, 
                                   so just use the first nid as the uuid */
                                rc = name_create(&failnodeuuid,
                                                 libcfs_nid2str(nid), "");
                                if (rc) 
                                        return rc;
                        }
                        CDEBUG(D_MGS, "add nid %s for failover uuid %s, "
                               "client %s\n", libcfs_nid2str(nid),
                               failnodeuuid, cliname);
                        rc = record_add_uuid(obd, llh, nid, failnodeuuid);
                }
                if (failnodeuuid) {
                        rc = record_add_conn(obd, llh, cliname, failnodeuuid);
                        name_destroy(failnodeuuid);
                        failnodeuuid = NULL;
                }
        }

        return rc;
}

static int mgs_write_log_mdt(struct obd_device *obd, struct fs_db *fsdb,
                             struct mgs_target_info *mti)
{
        struct llog_handle *llh = NULL;
        char *cliname, *mdcname, *nodeuuid, *mdcuuid;
        int rc, i, first_log = 0;
        ENTRY;

        CDEBUG(D_MGS, "writing new mdt %s\n", mti->mti_svname);

        /* COMPAT_146 */
        if (mti->mti_flags & LDD_F_UPGRADE14) {
                /* We're starting with an old uuid.  Assume old name for lov
                   as well since the lov entry already exists in the log. */
                CDEBUG(D_MGS, "old mds uuid %s\n", mti->mti_uuid);
                if (strncmp(mti->mti_uuid, fsdb->fsdb_mdtlov + 4, 
                            strlen(fsdb->fsdb_mdtlov) - 4) != 0) {
                        CERROR("old mds uuid %s doesn't match log %s (%s)\n",
                               mti->mti_uuid, fsdb->fsdb_mdtlov, 
                               fsdb->fsdb_mdtlov + 4);
                        RETURN(-EINVAL);
                }
        }
        /* end COMPAT_146 */
        
        if (mti->mti_uuid[0] == '\0') {
                /* Make up our own uuid */
                snprintf(mti->mti_uuid, sizeof(mti->mti_uuid),
                         "%s_UUID", mti->mti_svname);
        }

        /* Append mdt info to mdt log */
        if (mgs_log_is_empty(obd, mti->mti_svname)) {
                /* This is the first time for all logs for this fs, 
                   since any ost should have already started the mdt log. */
                first_log++;
                rc = mgs_write_log_lov(obd, fsdb, mti, mti->mti_svname,
                                       fsdb->fsdb_mdtlov);
        } 
        /* else there's already some ost entries in the mdt log. */

        /* We added the lov, maybe some osc's, now for the mdt.
           We might add more ost's after this. Note that during the parsing
           of this log, this is when the mdt will start. (This was not 
           formerly part of the old mds log, it was directly executed by
           lconf.) */ 
        /*
        mount_option 0:  1:mdsA  2:lov_mdsA
        attach mds mdsA mdsA_UUID
        setup /dev/loop2 ldiskfs mdsA errors=remount-ro,user_xattr
        */
        rc = record_start_log(obd, &llh, mti->mti_svname);
        rc = record_marker(obd, llh, fsdb, CM_START, mti->mti_svname,"add mdt"); 
        rc = record_mount_opt(obd, llh, mti->mti_svname, fsdb->fsdb_mdtlov, 0);
        rc = record_attach(obd, llh, mti->mti_svname, LUSTRE_MDS_NAME, 
                           mti->mti_uuid);
        rc = record_setup(obd, llh, mti->mti_svname,
                          "dev"/*ignored*/, "type"/*ignored*/,
                          mti->mti_svname, 0/*options*/);
        rc = record_marker(obd, llh, fsdb, CM_END, mti->mti_svname, "add mdt"); 
        rc = record_end_log(obd, &llh);

        /* Append the mdt info to the client log */
        name_create(&cliname, mti->mti_fsname, "-client");
        if (first_log) { 
                /* Start client log */
                rc = mgs_write_log_lov(obd, fsdb, mti, cliname,
                                       fsdb->fsdb_clilov);
        }

        name_create(&nodeuuid, libcfs_nid2str(mti->mti_nids[0]),/*"_UUID"*/"");
        name_create(&mdcname, mti->mti_svname, "-mdc");
        name_create(&mdcuuid, mdcname, "_UUID");
        /* 
        #09 L add_uuid nid=uml1@tcp(0x20000c0a80201) 0:  1:uml1_UUID
        #10 L attach   0:MDC_uml1_mdsA_MNT_client  1:mdc  2:1d834_MNT_client_03f
        #11 L setup    0:MDC_uml1_mdsA_MNT_client  1:mdsA_UUID  2:uml1_UUID
        #12 L add_uuid nid=uml2@tcp(0x20000c0a80202) 0:  1:uml2_UUID
        #13 L add_conn 0:MDC_uml1_mdsA_MNT_client  1:uml2_UUID
        #14 L mount_option 0:  1:client  2:lov1  3:MDC_uml1_mdsA_MNT_client
        */
        rc = record_start_log(obd, &llh, cliname);
        rc = record_marker(obd, llh, fsdb, CM_START, mti->mti_svname,"add mdc");
        /* COMPAT_146 */
        if (mti->mti_flags & LDD_F_UPGRADE14) { 
                /* Old client log already has MDC entry, but needs mount opt 
                   for new client name (lustre-client) */
                /* FIXME Old MDT log already has an old mount opt 
                   which we should remove (currently handled by
                   class_del_profiles()) */
                rc = record_mount_opt(obd, llh, cliname, fsdb->fsdb_clilov,
                                      fsdb->fsdb_mdc);
                /* end COMPAT_146 */
        } else {
                for (i = 0; i < mti->mti_nid_count; i++) {
                        CDEBUG(D_MGS, "add nid %s\n",
                               libcfs_nid2str(mti->mti_nids[i]));
                        rc = record_add_uuid(obd, llh, mti->mti_nids[i],
                                             nodeuuid);
                }
                rc = record_attach(obd, llh, mdcname, LUSTRE_MDC_NAME, mdcuuid);
                rc = record_setup(obd, llh, mdcname, mti->mti_uuid,nodeuuid,
                                  0, 0);
                rc = mgs_write_log_failnids(obd, mti, llh, mdcname);
                rc = record_mount_opt(obd, llh, cliname, fsdb->fsdb_clilov,
                                      mdcname);
        }
        rc = record_marker(obd, llh, fsdb, CM_END, mti->mti_svname, "add mdc"); 
        rc = record_end_log(obd, &llh);

        name_destroy(mdcuuid);
        name_destroy(mdcname);
        name_destroy(nodeuuid);
        name_destroy(cliname);
        RETURN(rc);
}

/* Add the ost info to the client/mdt lov */
static int mgs_write_log_osc(struct obd_device *obd, struct fs_db *fsdb,
                             struct mgs_target_info *mti,
                             char *logname, char *lovname, int flags)
{
        struct llog_handle *llh = NULL;
        char *nodeuuid, *oscname, *oscuuid, *lovuuid;
        char index[5];
        int i, rc;

        if (mgs_log_is_empty(obd, logname)) {
                /* The first item in the log must be the lov, so we have
                   somewhere to add our osc. */
                rc = mgs_write_log_lov(obd, fsdb, mti, logname, lovname);
        }
  
        CDEBUG(D_MGS, "adding osc for %s to log %s\n",
               mti->mti_svname, logname);

        name_create(&nodeuuid, libcfs_nid2str(mti->mti_nids[0]), "");
        name_create(&oscname, mti->mti_svname, "-osc");
        name_create(&oscuuid, oscname, "_UUID");
        name_create(&lovuuid, lovname, "_UUID");

        /*
        #03 L add_uuid nid=uml1@tcp(0x20000c0a80201) 0:  1:uml1_UUID
        multihomed (#4)
        #04 L add_uuid  nid=1@elan(0x1000000000001)  nal=90 0:  1:uml1_UUID
        #04 L attach   0:OSC_uml1_ost1_MNT_client  1:osc  2:89070_lov1_a41dff51a
        #05 L setup    0:OSC_uml1_ost1_MNT_client  1:ost1_UUID  2:uml1_UUID
        failover (#6,7)
        #06 L add_uuid nid=uml2@tcp(0x20000c0a80202) 0:  1:uml2_UUID
        #07 L add_conn 0:OSC_uml1_ost1_MNT_client  1:uml2_UUID
        #08 L lov_modify_tgts add 0:lov1  1:ost1_UUID  2(index):0  3(gen):1
        */
        rc = record_start_log(obd, &llh, logname);
        rc = record_marker(obd, llh, fsdb, CM_START | flags, mti->mti_svname,
                           "add osc"); 
        for (i = 0; i < mti->mti_nid_count; i++) {
                CDEBUG(D_MGS, "add nid %s\n", libcfs_nid2str(mti->mti_nids[i]));
                rc = record_add_uuid(obd, llh, mti->mti_nids[i], nodeuuid);
        }
        rc = record_attach(obd, llh, oscname, LUSTRE_OSC_NAME, lovuuid);
        rc = record_setup(obd, llh, oscname, mti->mti_uuid, nodeuuid, 0, 0);
        rc = mgs_write_log_failnids(obd, mti, llh, oscname);
        snprintf(index, sizeof(index), "%d", mti->mti_stripe_index);
        rc = record_lov_add(obd, llh, lovname, mti->mti_uuid, index, "1");
        rc = record_marker(obd, llh, fsdb, CM_END | flags, mti->mti_svname,
                           "add osc"); 
        rc = record_end_log(obd, &llh);
        
        name_destroy(lovuuid);
        name_destroy(oscuuid);
        name_destroy(oscname);
        name_destroy(nodeuuid);
        return rc;
}

static int mgs_write_log_ost(struct obd_device *obd, struct fs_db *fsdb,
                             struct mgs_target_info *mti)
{
        struct llog_handle *llh = NULL;
        char *logname;
        char *ptr = mti->mti_params;
        int rc, flags = 0, failout = 0;
        ENTRY;
        
        CDEBUG(D_MGS, "writing new ost %s\n", mti->mti_svname);

        /* The ost startup log */

        /* If the ost log already exists, that means that someone reformatted
           the ost and it called target_add again. */
        if (!mgs_log_is_empty(obd, mti->mti_svname)) {
                LCONSOLE_ERROR("The config log for %s already exists, yet the "
                               "server claims it never registered.  It may have"
                               " been reformatted, or the index changed. Use "
                               " tunefs.lustre --writeconf to regenerate "
                               " all logs.\n", mti->mti_svname);
                RETURN(-EALREADY);
        }
        /*
        attach obdfilter ost1 ost1_UUID
        setup /dev/loop2 ldiskfs f|n errors=remount-ro,user_xattr
        */
        if (class_find_param(ptr, PARAM_FAILMODE, &ptr) == 0) 
                failout = (strncmp(ptr, "failout", 7) == 0);
        rc = record_start_log(obd, &llh, mti->mti_svname);
        rc = record_marker(obd, llh, fsdb, CM_START, mti->mti_svname,"add ost"); 
        if (*mti->mti_uuid == '\0') 
                snprintf(mti->mti_uuid, sizeof(mti->mti_uuid),
                         "%s_UUID", mti->mti_svname);
        rc = record_attach(obd, llh, mti->mti_svname,
                           "obdfilter"/*LUSTRE_OST_NAME*/, mti->mti_uuid);
        rc = record_setup(obd, llh, mti->mti_svname,
                          "dev"/*ignored*/, "type"/*ignored*/,
                          failout ? "n" : "f", 0/*options*/);
        rc = record_marker(obd, llh, fsdb, CM_END, mti->mti_svname, "add ost"); 
        rc = record_end_log(obd, &llh);

        /* We also have to update the other logs where this osc is part of 
           the lov */

        /* Append ost info to mdt log */
        if (mti->mti_flags & LDD_F_UPGRADE14) 
                /* If we're upgrading, the old mdt log already has our
                   entry. Let's do a fake one for fun. */
                flags = CM_SKIP | CM_UPGRADE146;
        /* FIXME add to all MDT logs for CMD */
        /* FIXME need real MDT name, but MDT may not have registered yet! */
        name_create(&logname, mti->mti_fsname, "-MDT0000");
        mgs_write_log_osc(obd, fsdb, mti, logname, fsdb->fsdb_mdtlov, flags);
        name_destroy(logname);
        
        /* Append ost info to the client log */
        name_create(&logname, mti->mti_fsname, "-client");
        mgs_write_log_osc(obd, fsdb, mti, logname, fsdb->fsdb_clilov, flags);
        name_destroy(logname);
        
        RETURN(rc);
}

/* Add additional failnids to an existing log.  
   The mdc/osc must have been added to logs first */
/* tcp nids must be in dotted-quad ascii -
   we can't resolve hostnames from the kernel. */
static int mgs_write_log_add_failnid(struct obd_device *obd, struct fs_db *fsdb,
                                     struct mgs_target_info *mti)
{
        char *logname, *cliname;
        struct llog_handle *llh = NULL;
        int rc;
        ENTRY;

        /* Verify that we know about this target */
        if (mgs_log_is_empty(obd, mti->mti_svname)) {
                LCONSOLE_ERROR("The target %s has not registered yet. "
                               "It must be started before failnids can "
                               "be added.\n", mti->mti_svname);
                RETURN(-ENOENT);
        }

        /* Create mdc/osc client name (e.g. lustre-OST0001-osc) */
        if (mti->mti_flags & LDD_F_SV_TYPE_MDT) {
                name_create(&cliname, mti->mti_svname, "-mdc");
        } else if (mti->mti_flags & LDD_F_SV_TYPE_OST) {
                name_create(&cliname, mti->mti_svname, "-osc");
        } else {
                RETURN(-EINVAL);
        }
        
        /* Add failover nids to client log */
        name_create(&logname, mti->mti_fsname, "-client");
        rc = record_start_log(obd, &llh, logname);
        rc = record_marker(obd, llh, fsdb, CM_START, mti->mti_svname,
                           "add failnid");
        rc = mgs_write_log_failnids(obd, mti, llh, cliname);
        rc = record_marker(obd, llh, fsdb, CM_END, mti->mti_svname,
                           "add failnid"); 
        rc = record_end_log(obd, &llh);
        name_destroy(logname);

        if (mti->mti_flags & LDD_F_SV_TYPE_OST) {
                /* Add OST failover nids to the MDT log as well */
                name_create(&logname, mti->mti_fsname, "-MDT0000");
                rc = record_start_log(obd, &llh, logname);
                rc = record_marker(obd, llh, fsdb, CM_START, mti->mti_svname,
                                   "add failnid");
                rc = mgs_write_log_failnids(obd, mti, llh, cliname);
                rc = record_marker(obd, llh, fsdb, CM_END, mti->mti_svname,
                                   "add failnid"); 
                rc = record_end_log(obd, &llh);
                name_destroy(logname);
        }

        name_destroy(cliname);
        RETURN(rc);
}

static int mgs_write_log_params(struct obd_device *obd, struct fs_db *fsdb,
                                struct mgs_target_info *mti)
{
        struct lustre_cfg_bufs bufs;
        struct lustre_cfg *lcfg;
        char *ptr = mti->mti_params;
        char *endptr;
        char *end = mti->mti_params + sizeof(mti->mti_params);
        int rc = 0, len;
        ENTRY;

        if (!mti->mti_params) 
                RETURN(0);

        while (ptr < end) {
                while (*ptr == ' ') 
                        ptr++;
                if (*ptr == '\0' || (ptr >= end))
                        break;
                endptr = strchr(ptr, ' ');
                if (endptr)
                        len = endptr - ptr;
                else
                        len = strlen(ptr);
                CDEBUG(D_MGS, "next param '%.*s'\n", len, ptr);

                /* The params are stored in MOUNT_DATA_FILE and modified 
                   via tunefs.lustre */

                /* Processed in lustre_start_mgc */
                if (class_match_param(ptr, PARAM_MGSNODE, &endptr) == 0) 
                        GOTO(end_while, rc);

                /* Processed in mgs_write_log_ost */
                if (class_match_param(ptr, PARAM_FAILMODE, &endptr) == 0) 
                        GOTO(end_while, rc);

                if (class_match_param(ptr, PARAM_FAILNODE, &endptr) == 0) {
                        /* Add a failover nidlist */
                        rc = 0;
                        /* We already processed failovers params for new
                           targets in mgs_write_log_target */
                        if (mti->mti_flags & MTI_F_IOCTL) {
                                CDEBUG(D_MGS, "Adding failnode\n");
                                rc = mgs_write_log_add_failnid(obd, fsdb, mti);
                        }
                        GOTO(end_while, rc);
                }

                if (class_match_param(ptr, PARAM_SYS_TIMEOUT, &endptr) == 0) {
                        /* Change obd timeout */
                        int timeout;
                        timeout = simple_strtoul(endptr, &endptr, 0);

                        CDEBUG(D_MGS, "obd timeout %d\n", timeout);
                        lustre_cfg_bufs_reset(&bufs, NULL);
                        lcfg = lustre_cfg_new(LCFG_SET_TIMEOUT, &bufs);
                        lcfg->lcfg_num = timeout;
                        /* modify all servers and clients */
                        rc = mgs_write_log_direct_all(obd, fsdb, mti->mti_fsname,
                                                   lcfg); 
                        lustre_cfg_free(lcfg);
                        GOTO(end_while, rc);
                }

                if (class_match_param(ptr, PARAM_LOV_STRIPE, &endptr) == 0){
                        /* Change lov default stripe params */
                        char *logname;
                        CDEBUG(D_MGS, "lov param %s\n", ptr);
                        if (!(mti->mti_flags & LDD_F_SV_TYPE_MDT)) {
                                LCONSOLE_ERROR("Default stripe params must be "
                                               "set on the MDT, not %s. "
                                               "Ignoring.\n",
                                               mti->mti_svname);
                                GOTO(end_while, rc = 0);
                        }

                        /* Modify mdtlov */
                        if (mgs_log_is_empty(obd, mti->mti_svname))
                                GOTO(end_while, rc = -ENODEV);
                        lustre_cfg_bufs_reset(&bufs, fsdb->fsdb_mdtlov);
                        lustre_cfg_bufs_set(&bufs, 1, ptr, len);
                        lcfg = lustre_cfg_new(LCFG_PARAM, &bufs);
                        rc = mgs_write_log_direct(obd, fsdb, mti->mti_svname, 
                                                  fsdb->fsdb_mdtlov, lcfg);
                        lustre_cfg_free(lcfg);
                        if (rc) 
                                GOTO(end_while, rc);

                        /* Modify clilov */
                        name_create(&logname, mti->mti_fsname, "-client");
                        lustre_cfg_bufs_reset(&bufs, fsdb->fsdb_clilov);
                        lustre_cfg_bufs_set(&bufs, 1, ptr, len);
                        lcfg = lustre_cfg_new(LCFG_PARAM, &bufs);
                        rc = mgs_write_log_direct(obd, fsdb, logname,
                                                  fsdb->fsdb_clilov, lcfg);
                        lustre_cfg_free(lcfg);
                        name_destroy(logname);
                        GOTO(end_while, rc);
                }

                LCONSOLE_WARN("Ignoring unrecognized param '%.*s'\n", len, ptr);

end_while:
                if (rc) {
                        CERROR("err %d on param '%.*s\n", rc, len, ptr);
                        break;
                }
                ptr += len;
        }

        RETURN(rc);
}

int mgs_check_failnid(struct obd_device *obd, struct mgs_target_info *mti)
{
        /* Not implementing automatic failover nid addition at this time. */
        return 0;
#if 0
        struct fs_db *fsdb;
        int rc;
        ENTRY;

        rc = mgs_find_or_make_fsdb(obd, fsname, &fsdb); 
        if (rc) 
                RETURN(rc);

        if (mgs_log_is_empty(obd, mti->mti_svname)) 
                /* should never happen */
                RETURN(-ENOENT);

        CDEBUG(D_MGS, "Checking for new failnids for %s\n", mti->mti_svname);

        /* FIXME We can just check mti->params to see if we're already in
           the failover list.  Modify mti->params for rewriting back at 
           server_register_target(). */
        
        down(&fsdb->fsdb_sem);
        rc = mgs_write_log_add_failnid(obd, fsdb, mti);
        up(&fsdb->fsdb_sem);

        RETURN(rc);
#endif
}

int mgs_write_log_target(struct obd_device *obd,
                         struct mgs_target_info *mti)
{
        struct fs_db *fsdb;
        int rc = -EINVAL;
        ENTRY;

        /* set/check the new target index */
        rc = mgs_set_index(obd, mti);
        if (rc < 0) {
                CERROR("Can't get index (%d)\n", rc);
                RETURN(rc);
        }

        /* COMPAT_146 */
        if (mti->mti_flags & LDD_F_UPGRADE14) {
                if (rc == EALREADY) {
                        CDEBUG(D_MGS, "Found index for %s old log, upgrading\n",
                               mti->mti_svname);
                } else {
                        LCONSOLE_ERROR("Failed to find %s in the old client "
                                       "log\n", mti->mti_svname);
                        /* RETURN(-EINVAL); */
                }
                /* end COMPAT_146 */
        } else {
                if (rc == EALREADY) {
                        /* Update a target entry in the logs */
                        CERROR("updates not yet implemented\n");
                        /* FIXME mark old log sections as invalid, 
                           inc config ver #, add new log sections.
                           Make sure to update client and mds logs too
                           if needed */
                        RETURN(-ENXIO);
                }
        }

        rc = mgs_find_or_make_fsdb(obd, mti->mti_fsname, &fsdb); 
        if (rc) {
                CERROR("Can't get db for %s\n", mti->mti_fsname);
                RETURN(rc);
        }

        down(&fsdb->fsdb_sem);

        if (mti->mti_flags & LDD_F_SV_TYPE_MDT) {
                rc = mgs_write_log_mdt(obd, fsdb, mti);
        } else if (mti->mti_flags & LDD_F_SV_TYPE_OST) {
                rc = mgs_write_log_ost(obd, fsdb, mti);
        } else {
                CERROR("Unknown target type %#x, can't create log for %s\n",
                       mti->mti_flags, mti->mti_svname);
        }
        if (rc) {
                CERROR("Can't write logs for %s (%d)\n", mti->mti_svname, rc);
                GOTO(out_up, rc);
        }

        rc = mgs_write_log_params(obd, fsdb, mti);

out_up:
        up(&fsdb->fsdb_sem);
        RETURN(rc);
}

/* COMPAT_146 */
/* upgrade pre-mountconf logs to mountconf at first connect */ 
int mgs_upgrade_sv_14(struct obd_device *obd, struct mgs_target_info *mti)
{
        struct fs_db *fsdb;
        int rc = 0;
        ENTRY;

        /* Create ost log normally, as servers register.  Servers 
           register with their old uuids (from last_rcvd), so old
           (MDT and client) logs should work.
         - new MDT won't know about old OSTs, only the ones that have 
           registered, so we need the old MDT log to get the LOV right 
           in order for old clients to work. 
         - Old clients connect to the MDT, not the MGS, for their logs, and 
           will therefore receive the old client log from the MDT /LOGS dir. 
         - Old clients can continue to use and connect to old or new OSTs
         - New clients will contact the MGS for their log 
        */

        CDEBUG(D_MGS, "upgrading server %s from pre-1.6\n", 
               mti->mti_svname); 
        server_mti_print("upgrade", mti);
        
        rc = mgs_find_or_make_fsdb(obd, mti->mti_fsname, &fsdb);
        if (rc) 
                RETURN(rc);

        if (fsdb->fsdb_flags & FSDB_EMPTY) {
                LCONSOLE_ERROR("The old client log %s-client is missing.  Was "
                               "tunefs.lustre successful?\n",
                               mti->mti_fsname);
                RETURN(-ENOENT);
        }

        if (fsdb->fsdb_gen == 0) {
                /* There were no markers in the client log, meaning we have 
                   not updated the logs for this fs */
                CWARN("info: found old, unupdated client log\n");
        }

        if ((mti->mti_flags & LDD_F_SV_TYPE_MDT) && 
            mgs_log_is_empty(obd, mti->mti_svname)) {
                LCONSOLE_ERROR("The old MDT log %s is missing.  Was "
                               "tunefs.lustre successful?\n",
                               mti->mti_svname);
                RETURN(-ENOENT);
        }

        rc = mgs_write_log_target(obd, mti);
        RETURN(rc);
}
/* end COMPAT_146 */

static int mgs_clear_log(struct obd_device *obd, char *name)
{
        struct lvfs_run_ctxt saved;
        struct llog_handle *llh;
        int rc = 0;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        rc = llog_create(llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT),
                         &llh, NULL, name);
        if (rc == 0) {
                llog_init_handle(llh, LLOG_F_IS_PLAIN, NULL);
                rc = llog_destroy(llh);
                llog_free_handle(llh);
        }
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        if (rc)
                CERROR("failed to clear log %s: %d\n", name, rc);

        return(rc);
}

/* erase all logs for the given fs */
int mgs_erase_logs(struct obd_device *obd, char *fsname)
{
        struct mgs_obd *mgs = &obd->u.mgs;
        static struct fs_db *fsdb;
        struct list_head dentry_list;
        struct l_linux_dirent *dirent, *n;
        int rc, len = strlen(fsname);
        ENTRY;
        
        /* Find all the logs in the CONFIGS directory */
        rc = class_dentry_readdir(obd, mgs->mgs_configs_dir,
                                  mgs->mgs_vfsmnt, &dentry_list);
        if (rc) {
                CERROR("Can't read %s dir\n", MOUNT_CONFIGS_DIR);
                RETURN(rc);
        }
                                                                                
        /* Delete the fs db */
        down(&mgs->mgs_sem);
        fsdb = mgs_find_fsdb(obd, fsname);
        if (fsdb) 
                mgs_free_fsdb(fsdb);
        up(&mgs->mgs_sem);

        list_for_each_entry_safe(dirent, n, &dentry_list, lld_list) {
                list_del(&dirent->lld_list);
                if (strncmp(fsname, dirent->lld_name, len) == 0) {
                        CDEBUG(D_MGS, "Removing log %s\n", dirent->lld_name);
                        mgs_clear_log(obd, dirent->lld_name);
                }
                OBD_FREE(dirent, sizeof(*dirent));
        }
        
        RETURN(rc);
}

/* from llog_swab */
static void print_lustre_cfg(struct lustre_cfg *lcfg)
{
        int i;
        ENTRY;

        CDEBUG(D_MGS, "lustre_cfg: %p\n", lcfg);
        CDEBUG(D_MGS, "\tlcfg->lcfg_version: %#x\n", lcfg->lcfg_version);

        CDEBUG(D_MGS, "\tlcfg->lcfg_command: %#x\n", lcfg->lcfg_command);
        CDEBUG(D_MGS, "\tlcfg->lcfg_num: %#x\n", lcfg->lcfg_num);
        CDEBUG(D_MGS, "\tlcfg->lcfg_flags: %#x\n", lcfg->lcfg_flags);
        CDEBUG(D_MGS, "\tlcfg->lcfg_nid: %s\n", libcfs_nid2str(lcfg->lcfg_nid));

        CDEBUG(D_MGS, "\tlcfg->lcfg_bufcount: %d\n", lcfg->lcfg_bufcount);
        if (lcfg->lcfg_bufcount < LUSTRE_CFG_MAX_BUFCOUNT)
                for (i = 0; i < lcfg->lcfg_bufcount; i++) {
                        CDEBUG(D_MGS, "\tlcfg->lcfg_buflens[%d]: %d %s\n",
                               i, lcfg->lcfg_buflens[i], 
                               lustre_cfg_string(lcfg, i));
                }
        EXIT;
}

/* Set a permanent (config log) param for a target or fs */
int mgs_setparam(struct obd_device *obd, char *fsname, struct lustre_cfg *lcfg)
{
        struct fs_db *fsdb;
        struct mgs_target_info *mti;
        char *devname;
        int rc = 0;
        ENTRY;

        print_lustre_cfg(lcfg);
        
        /* lustre, lustre-mdtlov, lustre-client, lustre-MDT0000 */
        devname = lustre_cfg_string(lcfg, 0);

        if (devname == NULL) {
                /* Global setting across all fs's? */
                LCONSOLE_ERROR("Global settings not implemented yet!\n");
                RETURN(-ENOSYS);
        }
        
        CDEBUG(D_MGS, "target: %s\n", devname);

        rc = mgs_find_or_make_fsdb(obd, fsname, &fsdb); 
        if (rc) 
                RETURN(rc);
        if (fsdb->fsdb_flags & FSDB_EMPTY) {
                CERROR("No filesystem targets for %s\n", fsname);
                RETURN(-EINVAL);
        }

        /* Create a fake mti to hold everything */
        OBD_ALLOC_PTR(mti);
        if (!mti) 
                GOTO(out, rc = -ENOMEM);
        strcpy(mti->mti_fsname, fsname);
        strcpy(mti->mti_svname, devname);
        rc = server_name2index(devname, &mti->mti_stripe_index, NULL);
        if (rc < 0) 
                GOTO(out, rc);
        mti->mti_flags = rc | MTI_F_IOCTL;
        strncpy(mti->mti_params, lustre_cfg_string(lcfg, 1), 
                sizeof(mti->mti_params));

        down(&fsdb->fsdb_sem);
        rc = mgs_write_log_params(obd, fsdb, mti); 
        up(&fsdb->fsdb_sem);

out:
        OBD_FREE_PTR(mti);
        RETURN(rc);
}


#if 0
/******************** unused *********************/
static int mgs_backup_llog(struct obd_device *obd, char* fsname)
{
        struct file *filp, *bak_filp;
        struct lvfs_run_ctxt saved;
        char *logname, *buf;
        loff_t soff = 0 , doff = 0;
        int count = 4096, len;
        int rc = 0;

        OBD_ALLOC(logname, PATH_MAX);
        if (logname == NULL)
                return -ENOMEM;

        OBD_ALLOC(buf, count);
        if (!buf)
                GOTO(out , rc = -ENOMEM);

        len = snprintf(logname, PATH_MAX, "%s/%s.bak",
                       MOUNT_CONFIGS_DIR, fsname);

        if (len >= PATH_MAX - 1) {
                GOTO(out, -ENAMETOOLONG);
        } 

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                
        bak_filp = l_filp_open(logname, O_RDWR|O_CREAT|O_TRUNC, 0660);
        if (IS_ERR(bak_filp)) {
                rc = PTR_ERR(bak_filp);
                CERROR("backup logfile open %s: %d\n", logname, rc);
                GOTO(pop, rc);
        }
        sprintf(logname, "%s/%s", MOUNT_CONFIGS_DIR, fsname);
        filp = l_filp_open(logname, O_RDONLY, 0);
        if (IS_ERR(filp)) {
                rc = PTR_ERR(filp);
                CERROR("logfile open %s: %d\n", logname, rc);
                GOTO(close1f, rc);
        }

        while ((rc = lustre_fread(filp, buf, count, &soff)) > 0) {
                rc = lustre_fwrite(bak_filp, buf, count, &doff);
                break;
        }

        filp_close(filp, 0);
close1f:
        filp_close(bak_filp, 0);
pop:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
out:
        if (buf)
                OBD_FREE(buf, count);
        OBD_FREE(logname, PATH_MAX);
        return rc;
}



#endif
