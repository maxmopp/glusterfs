/*
  Copyright (c) 2010-2011 Gluster, Inc. <http://www.gluster.com>
  This file is part of GlusterFS.

  GlusterFS is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 3 of the License,
  or (at your option) any later version.

  GlusterFS is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.
*/

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "client.h"
#include "glusterfs3-xdr.h"
#include "glusterfs3.h"
#include "compat-errno.h"

int32_t client3_getspec (call_frame_t *frame, xlator_t *this, void *data);
void client_start_ping (void *data);
rpc_clnt_prog_t clnt3_1_fop_prog;


int
client_submit_vec_request (xlator_t  *this, void *req, call_frame_t  *frame,
                           rpc_clnt_prog_t *prog, int procnum,
                           fop_cbk_fn_t cbkfn,
                           struct iovec  *payload, int payloadcnt,
                           struct iobref *iobref, xdrproc_t xdrproc)
{
        int             ret        = 0;
        clnt_conf_t    *conf       = NULL;
        struct iovec    iov        = {0, };
        struct iobuf   *iobuf      = NULL;
        int             count      = 0;
        int             start_ping = 0;
        struct iobref  *new_iobref = NULL;
        ssize_t         xdr_size   = 0;
        struct rpc_req  rpcreq     = {0, };

        start_ping = 0;

        conf = this->private;

        if (req && xdrproc) {
                xdr_size = xdr_sizeof (xdrproc, req);
                iobuf = iobuf_get2 (this->ctx->iobuf_pool, xdr_size);
                if (!iobuf) {
                        goto unwind;
                };

                new_iobref = iobref_new ();
                if (!new_iobref) {
                        goto unwind;
                }

                if (iobref != NULL) {
                        ret = iobref_merge (new_iobref, iobref);
                        if (ret != 0) {
                                gf_log (this->name, GF_LOG_WARNING,
                                        "cannot merge iobref passed from caller "
                                        "into new_iobref");
                        }
                }

                ret = iobref_add (new_iobref, iobuf);
                if (ret != 0) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "cannot add iobuf into iobref");
                        goto unwind;
                }

                iov.iov_base = iobuf->ptr;
                iov.iov_len  = iobuf_size (iobuf);

                /* Create the xdr payload */
                ret = xdr_serialize_generic (iov, req, xdrproc);
                if (ret == -1) {
                        gf_log_callingfn ("", GF_LOG_WARNING,
                                          "XDR function failed");
                        goto unwind;
                }

                iov.iov_len = ret;
                count = 1;
        }

        /* Send the msg */
        ret = rpc_clnt_submit (conf->rpc, prog, procnum, cbkfn, &iov, count,
                               payload, payloadcnt, new_iobref, frame, NULL, 0,
                               NULL, 0, NULL);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_DEBUG, "rpc_clnt_submit failed");
        }

        if (ret == 0) {
                pthread_mutex_lock (&conf->rpc->conn.lock);
                {
                        if (!conf->rpc->conn.ping_started) {
                                start_ping = 1;
                        }
                }
                pthread_mutex_unlock (&conf->rpc->conn.lock);
        }

        if (start_ping)
                client_start_ping ((void *) this);

        if (new_iobref != NULL) {
                iobref_unref (new_iobref);
        }

        iobuf_unref (iobuf);

        return ret;

unwind:
        rpcreq.rpc_status = -1;
        cbkfn (&rpcreq, NULL, 0, frame);

        if (new_iobref != NULL) {
                iobref_unref (new_iobref);
        }

        iobuf_unref (iobuf);

        return ret;
}

/* CBK */

int
client3_1_symlink_cbk (struct rpc_req *req, struct iovec *iov, int count,
                       void *myframe)
{
        call_frame_t     *frame      = NULL;
        gfs3_symlink_rsp  rsp        = {0,};
        struct iatt       stbuf      = {0,};
        struct iatt       preparent  = {0,};
        struct iatt       postparent = {0,};
        int               ret        = 0;
        clnt_local_t     *local      = NULL;
        inode_t          *inode      = NULL;
        xlator_t         *this       = NULL;
        dict_t           *xdata      = NULL;

        this = THIS;

        frame = myframe;

        local = frame->local;
        inode = local->loc.inode;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_symlink_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.stat, &stbuf);

                gf_stat_to_iatt (&rsp.preparent, &preparent);
                gf_stat_to_iatt (&rsp.postparent, &postparent);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s. Path: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)),
                        (local) ? local->loc.path : "--");
        }

        CLIENT_STACK_UNWIND (symlink, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), inode, &stbuf,
                             &preparent, &postparent, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}


int
client3_1_mknod_cbk (struct rpc_req *req, struct iovec *iov, int count,
                     void *myframe)
{
        call_frame_t     *frame      = NULL;
        gfs3_mknod_rsp    rsp        = {0,};
        struct iatt       stbuf      = {0,};
        struct iatt       preparent  = {0,};
        struct iatt       postparent = {0,};
        int               ret        = 0;
        clnt_local_t     *local      = NULL;
        inode_t          *inode      = NULL;
        xlator_t         *this       = NULL;
        dict_t           *xdata      = NULL;

        this = THIS;

        frame = myframe;

        local = frame->local;

        inode = local->loc.inode;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_mknod_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.stat, &stbuf);

                gf_stat_to_iatt (&rsp.preparent, &preparent);
                gf_stat_to_iatt (&rsp.postparent, &postparent);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s. Path: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)),
                        (local) ? local->loc.path : "--");
        }

        CLIENT_STACK_UNWIND (mknod, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), inode,
                             &stbuf, &preparent, &postparent, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_mkdir_cbk (struct rpc_req *req, struct iovec *iov, int count,
                     void *myframe)
{
        call_frame_t     *frame      = NULL;
        gfs3_mkdir_rsp    rsp        = {0,};
        struct iatt       stbuf      = {0,};
        struct iatt       preparent  = {0,};
        struct iatt       postparent = {0,};
        int               ret        = 0;
        clnt_local_t     *local      = NULL;
        inode_t          *inode      = NULL;
        xlator_t         *this       = NULL;
        dict_t           *xdata      = NULL;

        this = THIS;

        frame = myframe;

        local = frame->local;
        inode = local->loc.inode;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_mkdir_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.stat, &stbuf);

                gf_stat_to_iatt (&rsp.preparent, &preparent);
                gf_stat_to_iatt (&rsp.postparent, &postparent);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s. Path: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)),
                        (local) ? local->loc.path : "--");
        }

        CLIENT_STACK_UNWIND (mkdir, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), inode,
                             &stbuf, &preparent, &postparent, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_open_cbk (struct rpc_req *req, struct iovec *iov, int count,
                    void *myframe)
{
        clnt_local_t  *local = NULL;
        clnt_conf_t   *conf  = NULL;
        clnt_fd_ctx_t *fdctx = NULL;
        call_frame_t  *frame = NULL;
        fd_t          *fd    = NULL;
        int            ret   = 0;
        gfs3_open_rsp  rsp   = {0,};
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;
        local = frame->local;

        conf  = frame->this->private;
        fd    = local->fd;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_open_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                fdctx = GF_CALLOC (1, sizeof (*fdctx),
                                   gf_client_mt_clnt_fdctx_t);
                if (!fdctx) {
                        rsp.op_ret = -1;
                        rsp.op_errno = ENOMEM;
                        goto out;
                }

                fdctx->remote_fd     = rsp.fd;
                fdctx->inode         = inode_ref (fd->inode);
                fdctx->flags         = local->flags;
                fdctx->wbflags       = local->wbflags;
                fdctx->lk_ctx        = fd_lk_ctx_ref (fd->lk_ctx);
                fdctx->lk_heal_state = GF_LK_HEAL_DONE;

                INIT_LIST_HEAD (&fdctx->sfd_pos);
                INIT_LIST_HEAD (&fdctx->lock_list);

                this_fd_set_ctx (fd, frame->this, &local->loc, fdctx);

                pthread_mutex_lock (&conf->lock);
                {
                        list_add_tail (&fdctx->sfd_pos, &conf->saved_fds);
                }
                pthread_mutex_unlock (&conf->lock);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s. Path: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)),
                        (local) ? local->loc.path : "--");
        }

        CLIENT_STACK_UNWIND (open, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), fd, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}


int
client3_1_stat_cbk (struct rpc_req *req, struct iovec *iov, int count,
                    void *myframe)
{
        gfs3_stat_rsp rsp = {0,};
        call_frame_t   *frame = NULL;
        struct iatt  iatt = {0,};
        int ret = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_stat_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.stat, &iatt);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }

        CLIENT_STACK_UNWIND (stat, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &iatt, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_readlink_cbk (struct rpc_req *req, struct iovec *iov, int count,
                        void *myframe)
{
        gfs3_readlink_rsp rsp = {0,};
        call_frame_t   *frame = NULL;
        struct iatt  iatt = {0,};
        int ret = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_readlink_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.buf, &iatt);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }

        CLIENT_STACK_UNWIND (readlink, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), rsp.path,
                             &iatt, xdata);

        /* This is allocated by the libc while decoding RPC msg */
        /* Hence no 'GF_FREE', but just 'free' */
        if (rsp.path)
                free (rsp.path);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_unlink_cbk (struct rpc_req *req, struct iovec *iov, int count,
                      void *myframe)
{
        call_frame_t    *frame      = NULL;
        gfs3_unlink_rsp  rsp        = {0,};
        struct iatt      preparent  = {0,};
        struct iatt      postparent = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_unlink_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.preparent, &preparent);
                gf_stat_to_iatt (&rsp.postparent, &postparent);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (unlink, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &preparent,
                             &postparent, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_rmdir_cbk (struct rpc_req *req, struct iovec *iov, int count,
                     void *myframe)
{
        gfs3_rmdir_rsp rsp = {0,};
        call_frame_t   *frame = NULL;
        struct iatt  preparent  = {0,};
        struct iatt  postparent = {0,};
        int ret = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_rmdir_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.preparent, &preparent);
                gf_stat_to_iatt (&rsp.postparent, &postparent);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (rmdir, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &preparent,
                             &postparent, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}


int
client3_1_truncate_cbk (struct rpc_req *req, struct iovec *iov, int count,
                        void *myframe)
{
        gfs3_truncate_rsp rsp = {0,};
        call_frame_t   *frame = NULL;
        struct iatt  prestat  = {0,};
        struct iatt  poststat = {0,};
        int ret = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_truncate_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.prestat, &prestat);
                gf_stat_to_iatt (&rsp.poststat, &poststat);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (truncate, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &prestat,
                             &poststat, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}


int
client3_1_statfs_cbk (struct rpc_req *req, struct iovec *iov, int count,
                      void *myframe)
{
        gfs3_statfs_rsp rsp = {0,};
        call_frame_t   *frame = NULL;
        struct statvfs  statfs = {0,};
        int ret = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_statfs_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_statfs_to_statfs (&rsp.statfs, &statfs);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (statfs, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &statfs, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}


int
client3_1_writev_cbk (struct rpc_req *req, struct iovec *iov, int count,
                      void *myframe)
{
        gfs3_write_rsp rsp = {0,};
        call_frame_t   *frame = NULL;
        struct iatt  prestat  = {0,};
        struct iatt  poststat = {0,};
        int ret = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_write_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.prestat, &prestat);
                gf_stat_to_iatt (&rsp.poststat, &poststat);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (writev, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &prestat,
                             &poststat, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_writev_vers_cbk (struct rpc_req *req, struct iovec *iov, int count,
                      void *myframe)
{
        gfs3_write_rsp rsp = {0,};
        call_frame_t   *frame = NULL;
        struct iatt  prestat  = {0,};
        struct iatt  poststat = {0,};
        int ret = 0;
        xlator_t         *this       = NULL;

        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_write_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.prestat, &prestat);
                gf_stat_to_iatt (&rsp.poststat, &poststat);
        }

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        STACK_UNWIND_STRICT (writev_vers, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &prestat,
                             &poststat, rsp.vers);

        return 0;
}

int
client3_1_flush_cbk (struct rpc_req *req, struct iovec *iov, int count,
                     void *myframe)
{
        call_frame_t    *frame      = NULL;
        clnt_local_t  *local      = NULL;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;

        gf_common_rsp    rsp        = {0,};
        int              ret        = 0;

        frame = myframe;
        this  = THIS;
        local = frame->local;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gf_common_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (rsp.op_ret >= 0 && !fd_is_anonymous (local->fd)) {
                /* Delete all saved locks of the owner issuing flush */
                ret = delete_granted_locks_owner (local->fd, &local->owner);
                gf_log (this->name, GF_LOG_TRACE,
                        "deleting locks of owner (%s) returned %d",
                        lkowner_utoa (&local->owner), ret);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (flush, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_fsync_cbk (struct rpc_req *req, struct iovec *iov, int count,
                     void *myframe)
{
        gfs3_fsync_rsp rsp = {0,};
        call_frame_t   *frame = NULL;
        struct iatt  prestat  = {0,};
        struct iatt  poststat = {0,};
        int ret = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_truncate_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.prestat, &prestat);
                gf_stat_to_iatt (&rsp.poststat, &poststat);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (fsync, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &prestat,
                             &poststat, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_setxattr_cbk (struct rpc_req *req, struct iovec *iov, int count,
                        void *myframe)
{
        call_frame_t    *frame      = NULL;
        gf_common_rsp    rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gf_common_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (setxattr, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_getxattr_cbk (struct rpc_req *req, struct iovec *iov, int count,
                        void *myframe)
{
        call_frame_t      *frame    = NULL;
        dict_t            *dict     = NULL;
        int                op_errno = EINVAL;
        gfs3_getxattr_rsp  rsp      = {0,};
        int                ret      = 0;
        clnt_local_t    *local    = NULL;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;
        local = frame->local;

        if (-1 == req->rpc_status) {
                rsp.op_ret = -1;
                op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_getxattr_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret = -1;
                op_errno = EINVAL;
                goto out;
        }

        op_errno = gf_error_to_errno (rsp.op_errno);
        if (-1 != rsp.op_ret) {
                GF_PROTOCOL_DICT_UNSERIALIZE (frame->this, dict,
                                              (rsp.dict.dict_val),
                                              (rsp.dict.dict_len), rsp.op_ret,
                                              op_errno, out);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s. Path: %s",
                        strerror (op_errno),
                        (local) ? local->loc.path : "--");
        }

        CLIENT_STACK_UNWIND (getxattr, frame, rsp.op_ret, op_errno, dict, xdata);

        /* don't use GF_FREE, this memory was allocated by libc */
        if (rsp.dict.dict_val)
                free (rsp.dict.dict_val);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        if (dict)
                dict_unref (dict);

        return 0;
}

int
client3_1_fgetxattr_cbk (struct rpc_req *req, struct iovec *iov, int count,
                         void *myframe)
{
        call_frame_t       *frame    = NULL;
        dict_t             *dict     = NULL;
        gfs3_fgetxattr_rsp  rsp      = {0,};
        int                 ret      = 0;
        int                 op_errno = EINVAL;
        clnt_local_t     *local    = NULL;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;
        local = frame->local;

        if (-1 == req->rpc_status) {
                rsp.op_ret = -1;
                op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_fgetxattr_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret = -1;
                op_errno = EINVAL;
                goto out;
        }

        op_errno = gf_error_to_errno (rsp.op_errno);
        if (-1 != rsp.op_ret) {
                GF_PROTOCOL_DICT_UNSERIALIZE (frame->this, dict,
                                              (rsp.dict.dict_val),
                                              (rsp.dict.dict_len), rsp.op_ret,
                                              op_errno, out);
        }
        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s",
                        strerror (op_errno));
        }

        CLIENT_STACK_UNWIND (fgetxattr, frame, rsp.op_ret, op_errno, dict, xdata);

        if (rsp.dict.dict_val)
                free (rsp.dict.dict_val);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        if (dict)
                dict_unref (dict);

        return 0;
}

int
client3_1_removexattr_cbk (struct rpc_req *req, struct iovec *iov, int count,
                           void *myframe)
{
        call_frame_t    *frame      = NULL;
        gf_common_rsp    rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;

        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gf_common_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }

        CLIENT_STACK_UNWIND (removexattr, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_fremovexattr_cbk (struct rpc_req *req, struct iovec *iov, int count,
                            void *myframe)
{
        call_frame_t    *frame      = NULL;
        gf_common_rsp    rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gf_common_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (fremovexattr, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_fsyncdir_cbk (struct rpc_req *req, struct iovec *iov, int count,
                        void *myframe)
{
        call_frame_t    *frame      = NULL;
        gf_common_rsp    rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gf_common_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (fsyncdir, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_access_cbk (struct rpc_req *req, struct iovec *iov, int count,
                      void *myframe)
{
        call_frame_t    *frame      = NULL;
        gf_common_rsp    rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gf_common_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (access, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}


int
client3_1_ftruncate_cbk (struct rpc_req *req, struct iovec *iov, int count,
                         void *myframe)
{
        gfs3_ftruncate_rsp rsp = {0,};
        call_frame_t   *frame = NULL;
        struct iatt  prestat  = {0,};
        struct iatt  poststat = {0,};
        int ret = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_ftruncate_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.prestat, &prestat);
                gf_stat_to_iatt (&rsp.poststat, &poststat);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (ftruncate, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &prestat,
                             &poststat, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_fstat_cbk (struct rpc_req *req, struct iovec *iov, int count,
                     void *myframe)
{
        gfs3_fstat_rsp rsp = {0,};
        call_frame_t   *frame = NULL;
        struct iatt  stat  = {0,};
        int ret = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_fstat_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.stat, &stat);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (fstat, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &stat,  xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}


int
client3_1_inodelk_cbk (struct rpc_req *req, struct iovec *iov, int count,
                       void *myframe)
{
        call_frame_t    *frame      = NULL;
        gf_common_rsp    rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gf_common_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if ((rsp.op_ret == -1) &&
            (EAGAIN != gf_error_to_errno (rsp.op_errno))) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (inodelk, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_finodelk_cbk (struct rpc_req *req, struct iovec *iov, int count,
                        void *myframe)
{
        call_frame_t    *frame      = NULL;
        gf_common_rsp    rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gf_common_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if ((rsp.op_ret == -1) &&
            (EAGAIN != gf_error_to_errno (rsp.op_errno))) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (finodelk, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_entrylk_cbk (struct rpc_req *req, struct iovec *iov, int count,
                       void *myframe)
{
        call_frame_t    *frame      = NULL;
        gf_common_rsp    rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gf_common_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if ((rsp.op_ret == -1) &&
            (EAGAIN != gf_error_to_errno (rsp.op_errno))) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }

        CLIENT_STACK_UNWIND (entrylk, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_fentrylk_cbk (struct rpc_req *req, struct iovec *iov, int count,
                        void *myframe)
{
        call_frame_t    *frame      = NULL;
        gf_common_rsp    rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gf_common_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if ((rsp.op_ret == -1) &&
            (EAGAIN != gf_error_to_errno (rsp.op_errno))) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }

        CLIENT_STACK_UNWIND (fentrylk, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_xattrop_cbk (struct rpc_req *req, struct iovec *iov, int count,
                       void *myframe)
{
        call_frame_t     *frame    = NULL;
        dict_t           *dict     = NULL;
        gfs3_xattrop_rsp  rsp      = {0,};
        int               ret      = 0;
        int               op_errno = EINVAL;
        clnt_local_t   *local    = NULL;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;
        local = frame->local;

        if (-1 == req->rpc_status) {
                rsp.op_ret = -1;
                op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_xattrop_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret = -1;
                op_errno = EINVAL;
                goto out;
        }

        op_errno = rsp.op_errno;
        if (-1 != rsp.op_ret) {
                GF_PROTOCOL_DICT_UNSERIALIZE (frame->this, dict,
                                              (rsp.dict.dict_val),
                                              (rsp.dict.dict_len), rsp.op_ret,
                                              op_errno, out);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s. Path: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)),
                        (local) ? local->loc.path : "--");
        }

        CLIENT_STACK_UNWIND (xattrop, frame, rsp.op_ret,
                             gf_error_to_errno (op_errno), dict, xdata);

        if (rsp.dict.dict_val)
                free (rsp.dict.dict_val);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        if (dict)
                dict_unref (dict);

        return 0;
}

int
client3_1_fxattrop_cbk (struct rpc_req *req, struct iovec *iov, int count,
                        void *myframe)
{
        call_frame_t      *frame    = NULL;
        dict_t            *dict     = NULL;
        dict_t            *xdata    = NULL;
        gfs3_fxattrop_rsp  rsp      = {0,};
        int                ret      = 0;
        int                op_errno = 0;
        clnt_local_t    *local    = NULL;
        xlator_t *this       = NULL;

        this = THIS;

        frame = myframe;
        local = frame->local;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_fxattrop_rsp);
        if (ret < 0) {
                rsp.op_ret = -1;
                op_errno = EINVAL;
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                goto out;
        }
        op_errno = rsp.op_errno;
        if (-1 != rsp.op_ret) {
                GF_PROTOCOL_DICT_UNSERIALIZE (frame->this, dict,
                                              (rsp.dict.dict_val),
                                              (rsp.dict.dict_len), rsp.op_ret,
                                              op_errno, out);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (frame->this, xdata,
                                      (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), rsp.op_ret,
                                      op_errno, out);
out:

        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s",
                        strerror (gf_error_to_errno (op_errno)));
        }
        CLIENT_STACK_UNWIND (fxattrop, frame, rsp.op_ret,
                             gf_error_to_errno (op_errno), dict, xdata);

        if (rsp.dict.dict_val)
                free (rsp.dict.dict_val);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        if (dict)
                dict_unref (dict);

        return 0;
}

int
client3_1_fsetxattr_cbk (struct rpc_req *req, struct iovec *iov, int count,
                         void *myframe)
{
        call_frame_t    *frame      = NULL;
        gf_common_rsp    rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gf_common_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (fsetxattr, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_fsetattr_cbk (struct rpc_req *req, struct iovec *iov, int count,
                        void *myframe)
{
        call_frame_t    *frame      = NULL;
        gfs3_fsetattr_rsp rsp        = {0,};
        struct iatt      prestat    = {0,};
        struct iatt      poststat   = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }
        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_fsetattr_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.statpre, &prestat);
                gf_stat_to_iatt (&rsp.statpost, &poststat);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (fsetattr, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &prestat,
                             &poststat, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}


int
client3_1_setattr_cbk (struct rpc_req *req, struct iovec *iov, int count,
                       void *myframe)
{
        call_frame_t    *frame      = NULL;
        gfs3_setattr_rsp rsp        = {0,};
        struct iatt      prestat    = {0,};
        struct iatt      poststat   = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_setattr_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.statpre, &prestat);
                gf_stat_to_iatt (&rsp.statpost, &poststat);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (setattr, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &prestat,
                             &poststat, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_create_cbk (struct rpc_req *req, struct iovec *iov, int count,
                      void *myframe)
{
        call_frame_t    *frame      = NULL;
        fd_t            *fd         = NULL;
        inode_t         *inode      = NULL;
        struct iatt      stbuf      = {0, };
        struct iatt      preparent  = {0, };
        struct iatt      postparent = {0, };
        int32_t          ret        = -1;
        clnt_local_t    *local      = NULL;
        clnt_conf_t     *conf       = NULL;
        clnt_fd_ctx_t   *fdctx      = NULL;
        gfs3_create_rsp  rsp        = {0,};
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;

        this = THIS;

        frame = myframe;
        local = frame->local;
        conf  = frame->this->private;
        fd    = local->fd;
        inode = local->loc.inode;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_create_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.stat, &stbuf);

                gf_stat_to_iatt (&rsp.preparent, &preparent);
                gf_stat_to_iatt (&rsp.postparent, &postparent);

                fdctx = GF_CALLOC (1, sizeof (*fdctx),
                                   gf_client_mt_clnt_fdctx_t);
                if (!fdctx) {
                        rsp.op_ret = -1;
                        rsp.op_errno = ENOMEM;
                        goto out;
                }

                fdctx->remote_fd     = rsp.fd;
                fdctx->inode         = inode_ref (inode);
                fdctx->flags         = local->flags;
                fdctx->lk_ctx        = fd_lk_ctx_ref (fd->lk_ctx);
                fdctx->lk_heal_state = GF_LK_HEAL_DONE;

                INIT_LIST_HEAD (&fdctx->sfd_pos);
                INIT_LIST_HEAD (&fdctx->lock_list);

                this_fd_set_ctx (fd, frame->this, &local->loc, fdctx);

                pthread_mutex_lock (&conf->lock);
                {
                        list_add_tail (&fdctx->sfd_pos, &conf->saved_fds);
                }
                pthread_mutex_unlock (&conf->lock);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s. Path: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)),
                        (local) ? local->loc.path : "--");
        }

        CLIENT_STACK_UNWIND (create, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), fd, inode,
                             &stbuf, &preparent, &postparent, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}


int
client3_1_rchecksum_cbk (struct rpc_req *req, struct iovec *iov, int count,
                         void *myframe)
{
        call_frame_t *frame = NULL;
        gfs3_rchecksum_rsp rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_rchecksum_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (rchecksum, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno),
                             rsp.weak_checksum,
                             (uint8_t *)rsp.strong_checksum.strong_checksum_val,
                             xdata);

        if (rsp.strong_checksum.strong_checksum_val) {
                /* This is allocated by the libc while decoding RPC msg */
                /* Hence no 'GF_FREE', but just 'free' */
                free (rsp.strong_checksum.strong_checksum_val);
        }

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_lk_cbk (struct rpc_req *req, struct iovec *iov, int count,
                  void *myframe)
{
        call_frame_t    *frame      = NULL;
        clnt_local_t  *local      = NULL;
        struct gf_flock     lock       = {0,};
        gfs3_lk_rsp      rsp        = {0,};
        int              ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;

        this = THIS;

        frame = myframe;
        local = frame->local;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_lk_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (rsp.op_ret >= 0) {
                gf_proto_flock_to_flock (&rsp.flock, &lock);
        }

        /* Save the lock to the client lock cache to be able
           to recover in the case of server reboot.*/
        /*
          temporarily
        if (local->cmd == F_SETLK || local->cmd == F_SETLKW) {
                ret = client_add_lock_for_recovery (local->fd, &lock,
                                                    local->owner, local->cmd);
                if (ret < 0) {
                        rsp.op_ret = -1;
                        rsp.op_errno = -ret;
                }
        }
        */

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if ((rsp.op_ret == -1) &&
            (EAGAIN != gf_error_to_errno (rsp.op_errno))) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }

        CLIENT_STACK_UNWIND (lk, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &lock, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_readdir_cbk (struct rpc_req *req, struct iovec *iov, int count,
                       void *myframe)
{
        call_frame_t     *frame    = NULL;
        gfs3_readdir_rsp  rsp      = {0,};
        int32_t           ret      = 0;
        clnt_local_t     *local    = NULL;
        gf_dirent_t       entries;
        xlator_t         *this     = NULL;
        dict_t           *xdata    = NULL;

        this = THIS;

        frame = myframe;
        local = frame->local;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_readdir_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        INIT_LIST_HEAD (&entries.list);
        if (rsp.op_ret > 0) {
                unserialize_rsp_dirent (&rsp, &entries);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (frame->this, xdata,
                                      (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), rsp.op_ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s remote_fd = %d",
                        strerror (gf_error_to_errno (rsp.op_errno)),
                                  local->cmd);
        }
        CLIENT_STACK_UNWIND (readdir, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &entries, xdata);

        if (rsp.op_ret != -1) {
                gf_dirent_free (&entries);
        }

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        clnt_readdir_rsp_cleanup (&rsp);

        return 0;
}


int
client3_1_readdirp_cbk (struct rpc_req *req, struct iovec *iov, int count,
                        void *myframe)
{
        call_frame_t      *frame = NULL;
        gfs3_readdirp_rsp  rsp   = {0,};
        int32_t            ret   = 0;
        clnt_local_t      *local = NULL;
        gf_dirent_t        entries;
        xlator_t          *this  = NULL;
        dict_t            *xdata = NULL;

        this = THIS;

        frame = myframe;
        local = frame->local;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_readdirp_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        INIT_LIST_HEAD (&entries.list);
        if (rsp.op_ret > 0) {
                unserialize_rsp_direntp (this, local->fd, &rsp, &entries);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (readdirp, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), &entries, xdata);

        if (rsp.op_ret != -1) {
                gf_dirent_free (&entries);
        }
        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        clnt_readdirp_rsp_cleanup (&rsp);

        return 0;
}


int
client3_1_rename_cbk (struct rpc_req *req, struct iovec *iov, int count,
                      void *myframe)
{
        call_frame_t     *frame      = NULL;
        gfs3_rename_rsp   rsp        = {0,};
        struct iatt       stbuf      = {0,};
        struct iatt       preoldparent  = {0,};
        struct iatt       postoldparent = {0,};
        struct iatt       prenewparent  = {0,};
        struct iatt       postnewparent = {0,};
        int               ret        = 0;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;

        this = THIS;

        frame = myframe;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_rename_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.stat, &stbuf);

                gf_stat_to_iatt (&rsp.preoldparent, &preoldparent);
                gf_stat_to_iatt (&rsp.postoldparent, &postoldparent);

                gf_stat_to_iatt (&rsp.prenewparent, &prenewparent);
                gf_stat_to_iatt (&rsp.postnewparent, &postnewparent);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (rename, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno),
                             &stbuf, &preoldparent, &postoldparent,
                             &prenewparent, &postnewparent, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_link_cbk (struct rpc_req *req, struct iovec *iov, int count,
                    void *myframe)
{
        call_frame_t     *frame      = NULL;
        gfs3_link_rsp     rsp        = {0,};
        struct iatt       stbuf      = {0,};
        struct iatt       preparent  = {0,};
        struct iatt       postparent = {0,};
        int               ret        = 0;
        clnt_local_t     *local      = NULL;
        inode_t          *inode      = NULL;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;


        this = THIS;

        frame = myframe;

        local = frame->local;
        inode = local->loc.inode;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_link_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                gf_stat_to_iatt (&rsp.stat, &stbuf);

                gf_stat_to_iatt (&rsp.preparent, &preparent);
                gf_stat_to_iatt (&rsp.postparent, &postparent);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s. Path: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)),
                        (local) ? local->loc.path : "--");
        }

        CLIENT_STACK_UNWIND (link, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), inode,
                             &stbuf, &preparent, &postparent, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}


int
client3_1_opendir_cbk (struct rpc_req *req, struct iovec *iov, int count,
                       void *myframe)
{
        clnt_local_t      *local = NULL;
        clnt_conf_t       *conf = NULL;
        clnt_fd_ctx_t     *fdctx = NULL;
        call_frame_t   *frame = NULL;
        fd_t             *fd = NULL;
        int ret = 0;
        gfs3_opendir_rsp  rsp = {0,};
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;

        this = THIS;

        frame = myframe;
        local = frame->local;

        conf  = frame->this->private;
        fd    = local->fd;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_opendir_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 != rsp.op_ret) {
                fdctx = GF_CALLOC (1, sizeof (*fdctx),
                                   gf_client_mt_clnt_fdctx_t);
                if (!fdctx) {
                        rsp.op_ret = -1;
                        rsp.op_errno = ENOMEM;
                        goto out;
                }

                fdctx->remote_fd = rsp.fd;
                fdctx->inode     = inode_ref (fd->inode);
                fdctx->is_dir    = 1;

                INIT_LIST_HEAD (&fdctx->sfd_pos);
                INIT_LIST_HEAD (&fdctx->lock_list);

                this_fd_set_ctx (fd, frame->this, &local->loc, fdctx);

                pthread_mutex_lock (&conf->lock);
                {
                        list_add_tail (&fdctx->sfd_pos, &conf->saved_fds);
                }
                pthread_mutex_unlock (&conf->lock);
        }

        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s. Path: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)),
                        (local) ? local->loc.path : "--");
        }
        CLIENT_STACK_UNWIND (opendir, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), fd, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}


int
client3_1_lookup_cbk (struct rpc_req *req, struct iovec *iov, int count,
                      void *myframe)
{
        clnt_local_t    *local      = NULL;
        call_frame_t    *frame      = NULL;
        int              ret        = 0;
        gfs3_lookup_rsp  rsp        = {0,};
        struct iatt      stbuf      = {0,};
        struct iatt      postparent = {0,};
        int              op_errno   = EINVAL;
        dict_t          *xdata      = NULL;
        inode_t         *inode      = NULL;
        xlator_t        *this       = NULL;

        this = THIS;

        frame = myframe;
        local = frame->local;
        inode = local->loc.inode;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_lookup_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                op_errno = EINVAL;
                goto out;
        }

        op_errno = gf_error_to_errno (rsp.op_errno);
        gf_stat_to_iatt (&rsp.postparent, &postparent);

        if (rsp.op_ret == -1)
                goto out;

        rsp.op_ret = -1;
        gf_stat_to_iatt (&rsp.stat, &stbuf);

        GF_PROTOCOL_DICT_UNSERIALIZE (frame->this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), rsp.op_ret,
                                      op_errno, out);

        if ((!uuid_is_null (inode->gfid))
            && (uuid_compare (stbuf.ia_gfid, inode->gfid) != 0)) {
                gf_log (frame->this->name, GF_LOG_DEBUG,
                        "gfid changed for %s", local->loc.path);
                rsp.op_ret = -1;
                op_errno = ESTALE;
                goto out;
        }

        rsp.op_ret = 0;

out:
        rsp.op_errno = op_errno;
        if (rsp.op_ret == -1) {
                /* any error other than ENOENT */
                if (rsp.op_errno != ENOENT)
                        gf_log (this->name, GF_LOG_WARNING,
                                "remote operation failed: %s. Path: %s",
                                strerror (rsp.op_errno),
                                (local) ? local->loc.path : "--");
                else
                        gf_log (this->name, GF_LOG_TRACE, "not found on remote node");

        }

        CLIENT_STACK_UNWIND (lookup, frame, rsp.op_ret, rsp.op_errno, inode,
                             &stbuf, xdata, &postparent);

        if (xdata)
                dict_unref (xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        return 0;
}

int
client3_1_readv_cbk (struct rpc_req *req, struct iovec *iov, int count,
                     void *myframe)
{
        call_frame_t   *frame  = NULL;
        struct iobref  *iobref = NULL;
        struct iovec    vector[MAX_IOVEC] = {{0}, };
        struct iatt     stat   = {0,};
        gfs3_read_rsp   rsp    = {0,};
        int             ret    = 0, rspcount = 0;
        clnt_local_t   *local  = NULL;
        xlator_t *this       = NULL;
        dict_t  *xdata       = NULL;

        this = THIS;

        memset (vector, 0, sizeof (vector));

        frame = myframe;
        local = frame->local;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = ENOTCONN;
                goto out;
        }

        ret = xdr_to_generic (*iov, &rsp, (xdrproc_t)xdr_gfs3_read_rsp);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR, "XDR decoding failed");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (rsp.op_ret != -1) {
                iobref = req->rsp_iobref;
                gf_stat_to_iatt (&rsp.stat, &stat);

                vector[0].iov_len = rsp.op_ret;
                if (rsp.op_ret > 0)
                        vector[0].iov_base = req->rsp[1].iov_base;
                rspcount = 1;
        }
        GF_PROTOCOL_DICT_UNSERIALIZE (this, xdata, (rsp.xdata.xdata_val),
                                      (rsp.xdata.xdata_len), ret,
                                      rsp.op_errno, out);

out:
        if (rsp.op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "remote operation failed: %s",
                        strerror (gf_error_to_errno (rsp.op_errno)));
        }
        CLIENT_STACK_UNWIND (readv, frame, rsp.op_ret,
                             gf_error_to_errno (rsp.op_errno), vector, rspcount,
                             &stat, iobref, xdata);

        if (rsp.xdata.xdata_val)
                free (rsp.xdata.xdata_val);

        if (xdata)
                dict_unref (xdata);

        return 0;
}

int
client3_1_release_cbk (struct rpc_req *req, struct iovec *iov, int count,
                       void *myframe)
{
        call_frame_t   *frame = NULL;

        frame = myframe;
        STACK_DESTROY (frame->root);
        return 0;
}
int
client3_1_releasedir_cbk (struct rpc_req *req, struct iovec *iov, int count,
                          void *myframe)
{
        call_frame_t   *frame = NULL;

        frame = myframe;
        STACK_DESTROY (frame->root);
        return 0;
}

int
client_fdctx_destroy (xlator_t *this, clnt_fd_ctx_t *fdctx)
{
        clnt_conf_t  *conf        = NULL;
        call_frame_t *fr          = NULL;
        int32_t       ret         = -1;
        char          parent_down = 0;
        fd_lk_ctx_t  *lk_ctx      = NULL;

        GF_VALIDATE_OR_GOTO ("client", this, out);
        GF_VALIDATE_OR_GOTO (this->name, fdctx, out);

        conf = (clnt_conf_t *) this->private;

        if (fdctx->remote_fd == -1) {
                gf_log (this->name, GF_LOG_DEBUG, "not a valid fd");
                goto out;
        }

        pthread_mutex_lock (&conf->lock);
        {
                parent_down   = conf->parent_down;
                lk_ctx        = fdctx->lk_ctx;
                fdctx->lk_ctx = NULL;
        }
        pthread_mutex_unlock (&conf->lock);

        if (lk_ctx)
                fd_lk_ctx_unref (lk_ctx);

        if (!parent_down)
                rpc_clnt_ref (conf->rpc);
        else
                goto out;

        fr = create_frame (this, this->ctx->pool);
        if (fr == NULL) {
                goto out;
        }

        ret = 0;

        if (fdctx->is_dir) {
                gfs3_releasedir_req  req = {{0,},};
                req.fd = fdctx->remote_fd;
                gf_log (this->name, GF_LOG_DEBUG, "sending releasedir on fd");
                client_submit_request (this, &req, fr, &clnt3_1_fop_prog,
                                       GFS3_OP_RELEASEDIR,
                                       client3_1_releasedir_cbk,
                                       NULL, NULL, 0, NULL, 0, NULL,
                                       (xdrproc_t)xdr_gfs3_releasedir_req);
        } else {
                gfs3_release_req  req = {{0,},};
                req.fd = fdctx->remote_fd;
                gf_log (this->name, GF_LOG_DEBUG, "sending release on fd");
                client_submit_request (this, &req, fr, &clnt3_1_fop_prog,
                                       GFS3_OP_RELEASE,
                                       client3_1_release_cbk, NULL,
                                       NULL, 0, NULL, 0, NULL,
                                       (xdrproc_t)xdr_gfs3_release_req);
        }

        rpc_clnt_unref (conf->rpc);
out:
        if (fdctx) {
                fdctx->remote_fd = -1;
                inode_unref (fdctx->inode);
                GF_FREE (fdctx);
        }

        if (ret && fr)
                STACK_DESTROY (fr->root);

        return ret;
}

int32_t
client3_1_releasedir (call_frame_t *frame, xlator_t *this,
                      void *data)
{
        clnt_conf_t         *conf        = NULL;
        clnt_fd_ctx_t       *fdctx       = NULL;
        clnt_args_t         *args        = NULL;
        int64_t              remote_fd   = -1;

        if (!this || !data)
                goto out;

        args = data;
        conf = this->private;

        pthread_mutex_lock (&conf->lock);
        {
                fdctx = this_fd_del_ctx (args->fd, this);
                if (fdctx != NULL) {
                        remote_fd = fdctx->remote_fd;

                        /* fdctx->remote_fd == -1 indicates a reopen attempt
                           in progress. Just mark ->released = 1 and let
                           reopen_cbk handle releasing
                        */

                        if (remote_fd != -1)
                                list_del_init (&fdctx->sfd_pos);

                        fdctx->released = 1;
                }
        }
        pthread_mutex_unlock (&conf->lock);

        if (remote_fd != -1)
                client_fdctx_destroy (this, fdctx);
out:

        return 0;
}

int32_t
client3_1_release (call_frame_t *frame, xlator_t *this,
                   void *data)
{
        int64_t           remote_fd     = -1;
        clnt_conf_t      *conf          = NULL;
        clnt_fd_ctx_t    *fdctx         = NULL;
        clnt_args_t      *args          = NULL;
        lk_heal_state_t   lk_heal_state = GF_LK_HEAL_DONE;

        if (!this || !data)
                goto out;

        args = data;
        conf = this->private;

        pthread_mutex_lock (&conf->lock);
        {
                fdctx = this_fd_del_ctx (args->fd, this);
                if (fdctx != NULL) {
                        remote_fd     = fdctx->remote_fd;
                        lk_heal_state = fdctx->lk_heal_state;

                        /* fdctx->remote_fd == -1 indicates a reopen attempt
                           in progress. Just mark ->released = 1 and let
                           reopen_cbk handle releasing
                        */

                        if (remote_fd != -1 &&
                            lk_heal_state == GF_LK_HEAL_DONE)
                                list_del_init (&fdctx->sfd_pos);

                        fdctx->released = 1;
                }
        }
        pthread_mutex_unlock (&conf->lock);

        if (remote_fd != -1 && lk_heal_state == GF_LK_HEAL_DONE)
                client_fdctx_destroy (this, fdctx);
out:
        return 0;
}


int32_t
client3_1_lookup (call_frame_t *frame, xlator_t *this,
                  void *data)
{
        clnt_conf_t     *conf              = NULL;
        clnt_local_t    *local             = NULL;
        clnt_args_t     *args              = NULL;
        gfs3_lookup_req  req               = {{0,},};
        int              ret               = 0;
        int              op_errno          = ESTALE;
        data_t          *content           = NULL;
        struct iovec     vector[MAX_IOVEC] = {{0}, };
        int              count             = 0;
        struct iobref   *rsp_iobref        = NULL;
        struct iobuf    *rsp_iobuf         = NULL;
        struct iovec    *rsphdr            = NULL;

        if (!frame || !this || !data)
                goto unwind;

        memset (vector, 0, sizeof (vector));

        conf = this->private;
        args = data;
        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }

        if (!(args->loc && args->loc->inode))
                goto unwind;

        loc_copy (&local->loc, args->loc);
        frame->local = local;

        if (args->loc->parent) {
                if (!uuid_is_null (args->loc->parent->gfid))
                        memcpy (req.pargfid, args->loc->parent->gfid, 16);
                else
                        memcpy (req.pargfid, args->loc->pargfid, 16);
        } else {
                if (!uuid_is_null (args->loc->inode->gfid))
                        memcpy (req.gfid, args->loc->inode->gfid, 16);
                else
                        memcpy (req.gfid, args->loc->gfid, 16);
        }

        if (args->xdata) {
                content = dict_get (args->xdata, GF_CONTENT_KEY);
                if (content != NULL) {
                        rsp_iobref = iobref_new ();
                        if (rsp_iobref == NULL) {
                                goto unwind;
                        }

                        /* TODO: what is the size we should send ? */
                        /* This change very much depends on quick-read
                           changes */
                        rsp_iobuf = iobuf_get (this->ctx->iobuf_pool);
                        if (rsp_iobuf == NULL) {
                                goto unwind;
                        }

                        iobref_add (rsp_iobref, rsp_iobuf);
                        iobuf_unref (rsp_iobuf);
                        rsphdr = &vector[0];
                        rsphdr->iov_base = iobuf_ptr (rsp_iobuf);
                        rsphdr->iov_len = iobuf_pagesize (rsp_iobuf);
                        count = 1;
                        rsp_iobuf = NULL;
                        local->iobref = rsp_iobref;
                        rsp_iobref = NULL;
                }

                GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata,
                                            (&req.xdata.xdata_val),
                                            req.xdata.xdata_len,
                                            op_errno, unwind);
        }

        if (args->loc->name)
                req.bname = (char *)args->loc->name;
        else
                req.bname = "";

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_LOOKUP, client3_1_lookup_cbk,
                                     NULL, rsphdr, count,
                                     NULL, 0, local->iobref,
                                     (xdrproc_t)xdr_gfs3_lookup_req);

        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val) {
                GF_FREE (req.xdata.xdata_val);
        }

        if (rsp_iobref != NULL) {
                iobref_unref (rsp_iobref);
        }

        return 0;

unwind:
        CLIENT_STACK_UNWIND (lookup, frame, -1, op_errno, NULL, NULL, NULL,
                             NULL);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        if (rsp_iobref != NULL) {
                iobref_unref (rsp_iobref);
        }

        if (rsp_iobuf != NULL) {
                iobuf_unref (rsp_iobuf);
        }

        return 0;
}

int32_t
client3_1_stat (call_frame_t *frame, xlator_t *this,
                void *data)
{
        clnt_conf_t   *conf     = NULL;
        clnt_args_t   *args     = NULL;
        gfs3_stat_req  req      = {{0,},};
        int            ret      = 0;
        int            op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        if (!(args->loc && args->loc->inode))
                goto unwind;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_STAT, client3_1_stat_cbk, NULL,
                                     NULL, 0, NULL, 0, NULL,
                                     (xdrproc_t)xdr_gfs3_stat_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (stat, frame, -1, op_errno, NULL, NULL);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}


int32_t
client3_1_truncate (call_frame_t *frame, xlator_t *this,
                    void *data)
{
        clnt_conf_t       *conf     = NULL;
        clnt_args_t       *args     = NULL;
        gfs3_truncate_req  req      = {{0,},};
        int                ret      = 0;
        int                op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->loc && args->loc->inode))
                goto unwind;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        req.offset = args->offset;

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_TRUNCATE,
                                     client3_1_truncate_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_truncate_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (truncate, frame, -1, op_errno, NULL, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}


int32_t
client3_1_ftruncate (call_frame_t *frame, xlator_t *this,
                     void *data)
{
        clnt_args_t        *args     = NULL;
        int64_t             remote_fd = -1;
        clnt_conf_t        *conf     = NULL;
        gfs3_ftruncate_req  req      = {{0,},};
        int                 op_errno = EINVAL;
        int                 ret      = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.offset = args->offset;
        req.fd     = remote_fd;
        memcpy (req.gfid, args->fd->inode->gfid, 16);

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FTRUNCATE,
                                     client3_1_ftruncate_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_ftruncate_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (ftruncate, frame, -1, op_errno, NULL, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_access (call_frame_t *frame, xlator_t *this,
                  void *data)
{
        clnt_conf_t     *conf     = NULL;
        clnt_args_t     *args     = NULL;
        gfs3_access_req  req      = {{0,},};
        int              ret      = 0;
        int              op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->loc && args->loc->inode))
                goto unwind;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        req.mask = args->mask;

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_ACCESS,
                                     client3_1_access_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_access_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (access, frame, -1, op_errno, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}

int32_t
client3_1_readlink (call_frame_t *frame, xlator_t *this,
                    void *data)
{
        clnt_conf_t       *conf     = NULL;
        clnt_args_t       *args     = NULL;
        gfs3_readlink_req  req      = {{0,},};
        int                ret      = 0;
        int                op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!args->loc) {
                goto unwind;
        }

        if (args->loc->inode && !uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        req.size = args->size;
        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_READLINK,
                                     client3_1_readlink_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_readlink_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (readlink, frame, -1, op_errno, NULL, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}




int32_t
client3_1_unlink (call_frame_t *frame, xlator_t *this,
                  void *data)
{
        clnt_conf_t     *conf     = NULL;
        clnt_args_t     *args     = NULL;
        gfs3_unlink_req  req      = {{0,},};
        int              ret      = 0;
        int              op_errno = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->loc && args->loc->parent))
                goto unwind;

        if (!uuid_is_null (args->loc->parent->gfid))
                memcpy (req.pargfid,  args->loc->parent->gfid, 16);
        else
                memcpy (req.pargfid, args->loc->pargfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.pargfid)),
                                       unwind, op_errno, EINVAL);
        req.bname = (char *)args->loc->name;
        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_UNLINK,
                                     client3_1_unlink_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_unlink_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (unlink, frame, -1, op_errno, NULL, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_rmdir (call_frame_t *frame, xlator_t *this,
                 void *data)
{
        clnt_conf_t    *conf     = NULL;
        clnt_args_t    *args     = NULL;
        gfs3_rmdir_req  req      = {{0,},};
        int             ret      = 0;
        int             op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->loc && args->loc->parent))
                goto unwind;

        if (!uuid_is_null (args->loc->parent->gfid))
                memcpy (req.pargfid,  args->loc->parent->gfid, 16);
        else
                memcpy (req.pargfid, args->loc->pargfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.pargfid)),
                                       unwind, op_errno, EINVAL);
        req.bname = (char *)args->loc->name;
        req.xflags = args->flags;
        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_RMDIR, client3_1_rmdir_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_rmdir_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (rmdir, frame, -1, op_errno, NULL, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_symlink (call_frame_t *frame, xlator_t *this,
                   void *data)
{
        clnt_local_t     *local    = NULL;
        clnt_conf_t      *conf     = NULL;
        clnt_args_t      *args     = NULL;
        gfs3_symlink_req  req      = {{0,},};
        int               ret      = 0;
        int               op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }

        frame->local = local;

        if (!(args->loc && args->loc->parent))
                goto unwind;

        loc_copy (&local->loc, args->loc);

        if (!uuid_is_null (args->loc->parent->gfid))
                memcpy (req.pargfid,  args->loc->parent->gfid, 16);
        else
                memcpy (req.pargfid, args->loc->pargfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.pargfid)),
                                       unwind, op_errno, EINVAL);
        req.linkname = (char *)args->linkname;
        req.bname    = (char *)args->loc->name;
        req.umask = args->umask;

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_SYMLINK, client3_1_symlink_cbk,
                                     NULL,  NULL, 0, NULL,
                                     0, NULL, (xdrproc_t)xdr_gfs3_symlink_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:

        CLIENT_STACK_UNWIND (symlink, frame, -1, op_errno, NULL, NULL, NULL,
                             NULL, NULL);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_rename (call_frame_t *frame, xlator_t *this,
                  void *data)
{
        clnt_conf_t     *conf     = NULL;
        clnt_args_t     *args     = NULL;
        gfs3_rename_req  req      = {{0,},};
        int              ret      = 0;
        int              op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->oldloc && args->newloc && args->oldloc->parent &&
              args->newloc->parent))
                goto unwind;

        if (!uuid_is_null (args->oldloc->parent->gfid))
                memcpy (req.oldgfid,  args->oldloc->parent->gfid, 16);
        else
                memcpy (req.oldgfid, args->oldloc->pargfid, 16);

        if (!uuid_is_null (args->newloc->parent->gfid))
                memcpy (req.newgfid, args->newloc->parent->gfid, 16);
        else
                memcpy (req.newgfid, args->newloc->pargfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.oldgfid)),
                                       unwind, op_errno, EINVAL);
        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.newgfid)),
                                       unwind, op_errno, EINVAL);
        req.oldbname =  (char *)args->oldloc->name;
        req.newbname = (char *)args->newloc->name;
        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_RENAME, client3_1_rename_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_rename_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (rename, frame, -1, op_errno, NULL, NULL, NULL,
                             NULL, NULL, NULL);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_link (call_frame_t *frame, xlator_t *this,
                void *data)
{
        clnt_local_t  *local    = NULL;
        clnt_conf_t   *conf     = NULL;
        clnt_args_t   *args     = NULL;
        gfs3_link_req  req      = {{0,},};
        int            ret      = 0;
        int            op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->oldloc && args->oldloc->inode && args->newloc &&
              args->newloc->parent))
                goto unwind;

        if (!uuid_is_null (args->oldloc->inode->gfid))
                memcpy (req.oldgfid,  args->oldloc->inode->gfid, 16);
        else
                memcpy (req.oldgfid, args->oldloc->gfid, 16);

        if (!uuid_is_null (args->newloc->parent->gfid))
                memcpy (req.newgfid, args->newloc->parent->gfid, 16);
        else
                memcpy (req.newgfid, args->newloc->pargfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.oldgfid)),
                                       unwind, op_errno, EINVAL);
        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.newgfid)),
                                       unwind, op_errno, EINVAL);
        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }

        loc_copy (&local->loc, args->oldloc);
        frame->local = local;

        req.newbname = (char *)args->newloc->name;
        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_LINK, client3_1_link_cbk, NULL,
                                     NULL, 0, NULL, 0, NULL,
                                     (xdrproc_t)xdr_gfs3_link_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (link, frame, -1, op_errno, NULL, NULL, NULL, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_mknod (call_frame_t *frame, xlator_t *this,
                 void *data)
{
        clnt_local_t   *local    = NULL;
        clnt_conf_t    *conf     = NULL;
        clnt_args_t    *args     = NULL;
        gfs3_mknod_req  req      = {{0,},};
        int             ret      = 0;
        int             op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }

        if (!(args->loc && args->loc->parent))
                goto unwind;

        loc_copy (&local->loc, args->loc);
        frame->local = local;

        if (!uuid_is_null (args->loc->parent->gfid))
                memcpy (req.pargfid,  args->loc->parent->gfid, 16);
        else
                memcpy (req.pargfid, args->loc->pargfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.pargfid)),
                                       unwind, op_errno, EINVAL);
        req.bname  = (char *)args->loc->name;
        req.mode   = args->mode;
        req.dev    = args->rdev;
        req.umask = args->umask;

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_MKNOD, client3_1_mknod_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_mknod_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (mknod, frame, -1, op_errno, NULL, NULL, NULL,
                             NULL, NULL);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_mkdir (call_frame_t *frame, xlator_t *this,
                 void *data)
{
        clnt_local_t   *local    = NULL;
        clnt_conf_t    *conf     = NULL;
        clnt_args_t    *args     = NULL;
        gfs3_mkdir_req  req      = {{0,},};
        int             ret      = 0;
        int             op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }

        if (!(args->loc && args->loc->parent))
                goto unwind;

        loc_copy (&local->loc, args->loc);
        frame->local = local;

        if (!uuid_is_null (args->loc->parent->gfid))
                memcpy (req.pargfid,  args->loc->parent->gfid, 16);
        else
                memcpy (req.pargfid, args->loc->pargfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.pargfid)),
                                       unwind, op_errno, EINVAL);

        req.bname = (char *)args->loc->name;
        req.mode  = args->mode;
        req.umask = args->umask;

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_MKDIR, client3_1_mkdir_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_mkdir_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (mkdir, frame, -1, op_errno, NULL, NULL, NULL,
                             NULL, NULL);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}


int32_t
client3_1_create (call_frame_t *frame, xlator_t *this,
                  void *data)
{
        clnt_local_t    *local    = NULL;
        clnt_conf_t     *conf     = NULL;
        clnt_args_t     *args     = NULL;
        gfs3_create_req  req      = {{0,},};
        int              ret      = 0;
        int              op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }
        if (!(args->loc && args->loc->parent))
                goto unwind;

        local->fd = fd_ref (args->fd);
        local->flags = args->flags;

        loc_copy (&local->loc, args->loc);
        frame->local = local;

        if (!uuid_is_null (args->loc->parent->gfid))
                memcpy (req.pargfid,  args->loc->parent->gfid, 16);
        else
                memcpy (req.pargfid, args->loc->pargfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.pargfid)),
                                       unwind, op_errno, EINVAL);
        req.bname = (char *)args->loc->name;
        req.mode  = args->mode;
        req.flags = gf_flags_from_flags (args->flags);
        req.umask = args->umask;

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_CREATE, client3_1_create_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_create_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (create, frame, -1, op_errno, NULL, NULL, NULL,
                             NULL, NULL, NULL);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_open (call_frame_t *frame, xlator_t *this,
                void *data)
{
        clnt_local_t  *local    = NULL;
        clnt_conf_t   *conf     = NULL;
        clnt_args_t   *args     = NULL;
        gfs3_open_req  req      = {{0,},};
        int            ret      = 0;
        int            op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }
        if (!(args->loc && args->loc->inode))
                goto unwind;

        local->fd = fd_ref (args->fd);
        local->flags = args->flags;
        loc_copy (&local->loc, args->loc);
        frame->local = local;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        req.flags = gf_flags_from_flags (args->flags);

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_OPEN, client3_1_open_cbk, NULL,
                                     NULL, 0, NULL, 0, NULL,
                                     (xdrproc_t)xdr_gfs3_open_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (open, frame, -1, op_errno, NULL, NULL);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_readv (call_frame_t *frame, xlator_t *this,
                 void *data)
{
        clnt_args_t    *args       = NULL;
        int64_t         remote_fd  = -1;
        clnt_conf_t    *conf       = NULL;
        int             op_errno   = ESTALE;
        gfs3_read_req   req        = {{0,},};
        int             ret        = 0;
        struct iovec    rsp_vec    = {0, };
        struct iobuf   *rsp_iobuf  = NULL;
        struct iobref  *rsp_iobref = NULL;
        clnt_local_t   *local      = NULL;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.size   = args->size;
        req.offset = args->offset;
        req.fd     = remote_fd;
        req.flag   = args->flags;

        memcpy (req.gfid, args->fd->inode->gfid, 16);

        rsp_iobuf = iobuf_get2 (this->ctx->iobuf_pool, args->size);
        if (rsp_iobuf == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        rsp_iobref = iobref_new ();
        if (rsp_iobref == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        iobref_add (rsp_iobref, rsp_iobuf);
        iobuf_unref (rsp_iobuf);
        rsp_vec.iov_base = iobuf_ptr (rsp_iobuf);
        rsp_vec.iov_len = iobuf_pagesize (rsp_iobuf);

        rsp_iobuf = NULL;

        if (args->size > rsp_vec.iov_len) {
                gf_log (this->name, GF_LOG_WARNING,
                        "read-size (%lu) is bigger than iobuf size (%lu)",
                        (unsigned long)args->size,
                        (unsigned long)rsp_vec.iov_len);
                op_errno = EINVAL;
                goto unwind;
        }

        local = mem_get0 (this->local_pool);
        if (local == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        local->iobref = rsp_iobref;
        rsp_iobref = NULL;
        frame->local = local;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_READ, client3_1_readv_cbk, NULL,
                                     NULL, 0, &rsp_vec, 1,
                                     local->iobref,
                                     (xdrproc_t)xdr_gfs3_read_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        if (rsp_iobuf) {
                iobuf_unref (rsp_iobuf);
        }

        if (rsp_iobref) {
                iobref_unref (rsp_iobref);
        }

        CLIENT_STACK_UNWIND (readv, frame, -1, op_errno, NULL, 0, NULL, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}


int32_t
client3_1_writev (call_frame_t *frame, xlator_t *this, void *data)
{
        clnt_args_t    *args     = NULL;
        int64_t         remote_fd = -1;
        clnt_conf_t    *conf     = NULL;
        gfs3_write_req  req      = {{0,},};
        int             op_errno = ESTALE;
        int             ret        = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.size   = args->size;
        req.offset = args->offset;
        req.fd     = remote_fd;
        req.flag   = args->flags;

        memcpy (req.gfid, args->fd->inode->gfid, 16);

        ret = client_submit_vec_request (this, &req, frame, conf->fops,
                                         GFS3_OP_WRITE, client3_1_writev_cbk,
                                         args->vector, args->count,
                                         args->iobref,
                                         (xdrproc_t)xdr_gfs3_write_req);
        if (ret) {
                /*
                 * If the lower layers fail to submit a request, they'll also
                 * do the unwind for us (see rpc_clnt_submit), so don't unwind
                 * here in such cases.
                 */
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;

unwind:
        CLIENT_STACK_UNWIND (writev, frame, -1, op_errno, NULL, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}


int32_t
client3_1_writev_vers (call_frame_t *frame, xlator_t *this, void *data)
{
        clnt_args_t    *args     = NULL;
        int64_t         remote_fd = -1;
        clnt_conf_t    *conf     = NULL;
        gfs3_write_req  req      = {{0,},};
        int             op_errno = ESTALE;
        int             ret        = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.size   = args->size;
        req.offset = args->offset;
        req.fd     = remote_fd;
        req.flag   = args->flags;
        req.vers   = args->version;

        memcpy (req.gfid, args->fd->inode->gfid, 16);

        ret = client_submit_vec_request (this, &req, frame, conf->fops,
                                         GFS3_OP_WRITE_VERS,
                                         client3_1_writev_vers_cbk,
                                         args->vector,
                                         args->count, args->iobref,
                                         (xdrproc_t)xdr_gfs3_write_req);
        if (ret) {
                /*
                 * If the lower layers fail to submit a request, they'll also
                 * do the unwind for us (see rpc_clnt_submit), so don't unwind
                 * here in such cases.
                 */
                gf_log (this->name, GF_LOG_WARNING,
                        "failed to send the fop: %s", strerror (op_errno));
        }

        return 0;

unwind:
        STACK_UNWIND_STRICT (writev_vers, frame, -1, op_errno, NULL, NULL, 0);
        return 0;
}


int32_t
client3_1_flush (call_frame_t *frame, xlator_t *this,
                 void *data)
{
        clnt_args_t    *args     = NULL;
        gfs3_flush_req  req      = {{0,},};
        int64_t         remote_fd = -1;
        clnt_conf_t    *conf     = NULL;
        clnt_local_t   *local    = NULL;
        int             op_errno = ESTALE;
        int             ret      = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        conf = this->private;

        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }

        local->fd = fd_ref (args->fd);
        local->owner = frame->root->lk_owner;
        frame->local = local;

        req.fd = remote_fd;
        memcpy (req.gfid, args->fd->inode->gfid, 16);

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FLUSH, client3_1_flush_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_flush_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);


        return 0;

unwind:
        CLIENT_STACK_UNWIND (flush, frame, -1, op_errno, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_fsync (call_frame_t *frame, xlator_t *this,
                 void *data)
{
        clnt_args_t    *args      = NULL;
        gfs3_fsync_req  req       = {{0,},};
        int64_t         remote_fd = -1;
        clnt_conf_t    *conf      = NULL;
        int             op_errno  = 0;
        int             ret       = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.fd   = remote_fd;
        req.data = args->flags;
        memcpy (req.gfid, args->fd->inode->gfid, 16);


        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FSYNC, client3_1_fsync_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_fsync_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");

        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;

unwind:
        CLIENT_STACK_UNWIND (fsync, frame, -1, op_errno, NULL, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_fstat (call_frame_t *frame, xlator_t *this,
                 void *data)
{
        clnt_args_t    *args     = NULL;
        gfs3_fstat_req  req      = {{0,},};
        int64_t         remote_fd = -1;
        clnt_conf_t    *conf     = NULL;
        int             op_errno = ESTALE;
        int           ret        = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.fd = remote_fd;
        memcpy (req.gfid, args->fd->inode->gfid, 16);

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FSTAT, client3_1_fstat_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_fstat_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;

unwind:
        CLIENT_STACK_UNWIND (fstat, frame, -1, op_errno, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_opendir (call_frame_t *frame, xlator_t *this,
                   void *data)
{
        clnt_local_t     *local    = NULL;
        clnt_conf_t      *conf     = NULL;
        clnt_args_t      *args     = NULL;
        gfs3_opendir_req  req      = {{0,},};
        int               ret      = 0;
        int               op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }
        if (!(args->loc && args->loc->inode))
                goto unwind;

        local->fd = fd_ref (args->fd);
        loc_copy (&local->loc, args->loc);
        frame->local = local;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);

        conf = this->private;

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_OPENDIR, client3_1_opendir_cbk,
                                     NULL, NULL, 0, NULL, 0, NULL,
                                     (xdrproc_t)xdr_gfs3_opendir_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;

unwind:
        CLIENT_STACK_UNWIND (opendir, frame, -1, op_errno, NULL, NULL);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_fsyncdir (call_frame_t *frame, xlator_t *this, void *data)
{
        clnt_args_t       *args      = NULL;
        int64_t            remote_fd = -1;
        clnt_conf_t       *conf      = NULL;
        gfs3_fsyncdir_req  req       = {{0,},};
        int                ret       = 0;
        int32_t            op_errno  = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.fd   = remote_fd;
        req.data = args->flags;
        memcpy (req.gfid, args->fd->inode->gfid, 16);

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FSYNCDIR, client3_1_fsyncdir_cbk,
                                     NULL, NULL, 0,
                                     NULL, 0, NULL,
                                     (xdrproc_t)xdr_gfs3_fsyncdir_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;

unwind:
        CLIENT_STACK_UNWIND (fsyncdir, frame, -1, op_errno, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_statfs (call_frame_t *frame, xlator_t *this,
                  void *data)
{
        clnt_conf_t     *conf     = NULL;
        clnt_args_t     *args     = NULL;
        gfs3_statfs_req  req      = {{0,},};
        int              ret      = 0;
        int              op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!args->loc)
                goto unwind;

        if (args->loc->inode) {
                if (!uuid_is_null (args->loc->inode->gfid))
                        memcpy (req.gfid,  args->loc->inode->gfid, 16);
                else
                        memcpy (req.gfid, args->loc->gfid, 16);
        } else
                req.gfid[15] = 1;

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_STATFS, client3_1_statfs_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_statfs_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;

unwind:
        CLIENT_STACK_UNWIND (statfs, frame, -1, op_errno, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_setxattr (call_frame_t *frame, xlator_t *this,
                    void *data)
{
        clnt_conf_t       *conf     = NULL;
        clnt_args_t       *args     = NULL;
        gfs3_setxattr_req  req      = {{0,},};
        int                ret      = 0;
        int                op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->loc && args->loc->inode))
                goto unwind;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        if (args->xattr) {
                GF_PROTOCOL_DICT_SERIALIZE (this, args->xattr,
                                            (&req.dict.dict_val),
                                            req.dict.dict_len,
                                            op_errno, unwind);
        }

        req.flags = args->flags;

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_SETXATTR, client3_1_setxattr_cbk,
                                     NULL, NULL, 0, NULL, 0, NULL,
                                     (xdrproc_t)xdr_gfs3_setxattr_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }
        if (req.dict.dict_val)
                GF_FREE (req.dict.dict_val);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (setxattr, frame, -1, op_errno, NULL);
        if (req.dict.dict_val)
                GF_FREE (req.dict.dict_val);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_fsetxattr (call_frame_t *frame, xlator_t *this,
                     void *data)
{
        clnt_args_t        *args     = NULL;
        int64_t             remote_fd = -1;
        clnt_conf_t        *conf     = NULL;
        gfs3_fsetxattr_req  req      = {{0,},};
        int                 op_errno = ESTALE;
        int                 ret      = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.fd    = remote_fd;
        req.flags = args->flags;
        memcpy (req.gfid, args->fd->inode->gfid, 16);

        if (args->xattr) {
                GF_PROTOCOL_DICT_SERIALIZE (this, args->xattr,
                                            (&req.dict.dict_val),
                                            req.dict.dict_len,
                                            op_errno, unwind);
        }

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FSETXATTR, client3_1_fsetxattr_cbk,
                                     NULL, NULL, 0, NULL, 0, NULL,
                                     (xdrproc_t)xdr_gfs3_fsetxattr_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.dict.dict_val)
                GF_FREE (req.dict.dict_val);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (fsetxattr, frame, -1, op_errno, NULL);
        if (req.dict.dict_val)
                GF_FREE (req.dict.dict_val);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}




int32_t
client3_1_fgetxattr (call_frame_t *frame, xlator_t *this,
                     void *data)
{
        clnt_args_t        *args       = NULL;
        int64_t             remote_fd  = -1;
        clnt_conf_t        *conf       = NULL;
        gfs3_fgetxattr_req  req        = {{0,},};
        int                 op_errno   = ESTALE;
        int                 ret        = 0;
        int                 count      = 0;
        clnt_local_t       *local      = NULL;
        struct iobref      *rsp_iobref = NULL;
        struct iobuf       *rsp_iobuf  = NULL;
        struct iovec       *rsphdr     = NULL;
        struct iovec        vector[MAX_IOVEC] = {{0}, };

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }
        frame->local = local;

        rsp_iobref = iobref_new ();
        if (rsp_iobref == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        /* TODO: what is the size we should send ? */
        rsp_iobuf = iobuf_get2 (this->ctx->iobuf_pool, 8 * GF_UNIT_KB);
        if (rsp_iobuf == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        iobref_add (rsp_iobref, rsp_iobuf);
        iobuf_unref (rsp_iobuf);
        rsphdr = &vector[0];
        rsphdr->iov_base = iobuf_ptr (rsp_iobuf);
        rsphdr->iov_len = iobuf_pagesize (rsp_iobuf);;
        count = 1;
        rsp_iobuf = NULL;
        local->iobref = rsp_iobref;
        rsp_iobref = NULL;

        req.namelen = 1; /* Use it as a flag */
        req.fd   = remote_fd;
        req.name = (char *)args->name;
        if (!req.name) {
                req.name = "";
                req.namelen = 0;
        }
        memcpy (req.gfid, args->fd->inode->gfid, 16);

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FGETXATTR,
                                     client3_1_fgetxattr_cbk, NULL,
                                     rsphdr, count,
                                     NULL, 0, local->iobref,
                                     (xdrproc_t)xdr_gfs3_fgetxattr_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (fgetxattr, frame, -1, op_errno, NULL, NULL);

        if (rsp_iobuf) {
                iobuf_unref (rsp_iobuf);
        }

        if (rsp_iobref) {
                iobref_unref (rsp_iobref);
        }
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_getxattr (call_frame_t *frame, xlator_t *this,
                    void *data)
{
        clnt_conf_t       *conf       = NULL;
        clnt_args_t       *args       = NULL;
        gfs3_getxattr_req  req        = {{0,},};
        dict_t            *dict       = NULL;
        int                ret        = 0;
        int32_t            op_ret     = -1;
        int                op_errno   = ESTALE;
        int                count      = 0;
        clnt_local_t      *local      = NULL;
        struct iobref     *rsp_iobref = NULL;
        struct iobuf      *rsp_iobuf  = NULL;
        struct iovec      *rsphdr     = NULL;
        struct iovec       vector[MAX_IOVEC] = {{0}, };

        if (!frame || !this || !data) {
                op_errno = 0;
                goto unwind;
        }
        args = data;

        if (!(args->loc && args->loc->inode)) {
                op_errno = EINVAL;
                goto unwind;
        }

        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }
        frame->local = local;

        rsp_iobref = iobref_new ();
        if (rsp_iobref == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        /* TODO: what is the size we should send ? */
        rsp_iobuf = iobuf_get2 (this->ctx->iobuf_pool, 8 * GF_UNIT_KB);
        if (rsp_iobuf == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        iobref_add (rsp_iobref, rsp_iobuf);
        iobuf_unref (rsp_iobuf);
        rsphdr = &vector[0];
        rsphdr->iov_base = iobuf_ptr (rsp_iobuf);
        rsphdr->iov_len = iobuf_pagesize (rsp_iobuf);
        count = 1;
        rsp_iobuf = NULL;
        local->iobref = rsp_iobref;
        rsp_iobref = NULL;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        req.namelen = 1; /* Use it as a flag */

        req.name = (char *)args->name;
        if (!req.name) {
                req.name = "";
                req.namelen = 0;
        }

        conf = this->private;

        if (args && args->name) {
                if (is_client_dump_locks_cmd ((char *)args->name)) {
                        dict = dict_new ();
                        ret = client_dump_locks ((char *)args->name,
                                                 args->loc->inode,
                                                 dict);
                        if (ret) {
                                gf_log (this->name, GF_LOG_WARNING,
                                        "Client dump locks failed");
                                op_errno = EINVAL;
                        }

                        GF_ASSERT (dict);
                        op_ret = 0;
                        op_errno = 0;
                        goto unwind;
                }
        }

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_GETXATTR,
                                     client3_1_getxattr_cbk, NULL,
                                     rsphdr, count,
                                     NULL, 0, local->iobref,
                                     (xdrproc_t)xdr_gfs3_getxattr_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        if (rsp_iobuf) {
                iobuf_unref (rsp_iobuf);
        }

        if (rsp_iobref) {
                iobref_unref (rsp_iobref);
        }

        CLIENT_STACK_UNWIND (getxattr, frame, op_ret, op_errno, dict, NULL);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_xattrop (call_frame_t *frame, xlator_t *this,
                   void *data)
{
        clnt_conf_t      *conf       = NULL;
        clnt_args_t      *args       = NULL;
        gfs3_xattrop_req  req        = {{0,},};
        int               ret        = 0;
        int               op_errno   = ESTALE;
        int               count      = 0;
        clnt_local_t   *local      = NULL;
        struct iobref    *rsp_iobref = NULL;
        struct iobuf     *rsp_iobuf  = NULL;
        struct iovec     *rsphdr     = NULL;
        struct iovec      vector[MAX_IOVEC] = {{0}, };

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->loc && args->loc->inode))
                goto unwind;

        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }
        frame->local = local;

        rsp_iobref = iobref_new ();
        if (rsp_iobref == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        /* TODO: what is the size we should send ? */
        rsp_iobuf = iobuf_get2 (this->ctx->iobuf_pool, 8 * GF_UNIT_KB);
        if (rsp_iobuf == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        iobref_add (rsp_iobref, rsp_iobuf);
        iobuf_unref (rsp_iobuf);
        rsphdr = &vector[0];
        rsphdr->iov_base = iobuf_ptr (rsp_iobuf);
        rsphdr->iov_len = iobuf_pagesize (rsp_iobuf);
        count = 1;
        rsp_iobuf = NULL;
        local->iobref = rsp_iobref;
        rsp_iobref = NULL;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        if (args->xattr) {
                GF_PROTOCOL_DICT_SERIALIZE (this, args->xattr,
                                            (&req.dict.dict_val),
                                            req.dict.dict_len,
                                            op_errno, unwind);
        }

        req.flags = args->flags;

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_XATTROP,
                                     client3_1_xattrop_cbk, NULL,
                                     rsphdr, count,
                                     NULL, 0, local->iobref,
                                     (xdrproc_t)xdr_gfs3_xattrop_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.dict.dict_val)
                GF_FREE (req.dict.dict_val);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (xattrop, frame, -1, op_errno, NULL, NULL);

        if (req.dict.dict_val) {
                GF_FREE (req.dict.dict_val);
        }

        if (rsp_iobuf) {
                iobuf_unref (rsp_iobuf);
        }

        if (rsp_iobref) {
                iobref_unref (rsp_iobref);
        }
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_fxattrop (call_frame_t *frame, xlator_t *this,
                    void *data)
{
        clnt_args_t       *args       = NULL;
        int64_t            remote_fd  = -1;
        clnt_conf_t       *conf       = NULL;
        gfs3_fxattrop_req  req        = {{0,},};
        int                op_errno   = ESTALE;
        int                ret        = 0;
        int                count      = 0;
        clnt_local_t    *local      = NULL;
        struct iobref     *rsp_iobref = NULL;
        struct iobuf      *rsp_iobuf  = NULL;
        struct iovec      *rsphdr     = NULL;
        struct iovec       vector[MAX_IOVEC] = {{0}, };

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.fd     = remote_fd;
        req.flags  = args->flags;
        memcpy (req.gfid, args->fd->inode->gfid, 16);

        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }
        frame->local = local;

        rsp_iobref = iobref_new ();
        if (rsp_iobref == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        /* TODO: what is the size we should send ? */
        rsp_iobuf = iobuf_get2 (this->ctx->iobuf_pool, 8 * GF_UNIT_KB);
        if (rsp_iobuf == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        iobref_add (rsp_iobref, rsp_iobuf);
        iobuf_unref (rsp_iobuf);
        rsphdr = &vector[0];
        rsphdr->iov_base = iobuf_ptr (rsp_iobuf);
        rsphdr->iov_len = iobuf_pagesize (rsp_iobuf);
        count = 1;
        rsp_iobuf = NULL;
        local->iobref = rsp_iobref;
        rsp_iobref = NULL;

        if (args->xattr) {
                GF_PROTOCOL_DICT_SERIALIZE (this, args->xattr,
                                            (&req.dict.dict_val),
                                            req.dict.dict_len,
                                            op_errno, unwind);
        }

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FXATTROP,
                                     client3_1_fxattrop_cbk, NULL,
                                     rsphdr, count,
                                     NULL, 0, local->iobref,
                                     (xdrproc_t)xdr_gfs3_fxattrop_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.dict.dict_val)
                GF_FREE (req.dict.dict_val);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (fxattrop, frame, -1, op_errno, NULL, NULL);

        if (req.dict.dict_val)
                GF_FREE (req.dict.dict_val);

        if (rsp_iobref)
                iobref_unref (rsp_iobref);

        if (rsp_iobuf)
                iobuf_unref (rsp_iobuf);

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}


int32_t
client3_1_removexattr (call_frame_t *frame, xlator_t *this,
                       void *data)
{
        clnt_conf_t          *conf     = NULL;
        clnt_args_t          *args     = NULL;
        gfs3_removexattr_req  req      = {{0,},};
        int                   ret      = 0;
        int                   op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->loc && args->loc->inode))
                goto unwind;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        req.name = (char *)args->name;

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_REMOVEXATTR,
                                     client3_1_removexattr_cbk, NULL,
                                     NULL, 0, NULL, 0, NULL,
                                     (xdrproc_t)xdr_gfs3_removexattr_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (removexattr, frame, -1, op_errno, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}

int32_t
client3_1_fremovexattr (call_frame_t *frame, xlator_t *this,
                        void *data)
{
        clnt_conf_t           *conf     = NULL;
        clnt_args_t           *args     = NULL;
        gfs3_fremovexattr_req  req      = {{0,},};
        int                    ret      = 0;
        int64_t                remote_fd = -1;
        int                    op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->fd && args->fd->inode))
                goto unwind;

        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        memcpy (req.gfid,  args->fd->inode->gfid, 16);
        req.name = (char *)args->name;
        req.fd = remote_fd;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FREMOVEXATTR,
                                     client3_1_fremovexattr_cbk, NULL,
                                     NULL, 0, NULL, 0, NULL,
                                     (xdrproc_t)xdr_gfs3_fremovexattr_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (fremovexattr, frame, -1, op_errno, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}

int32_t
client3_1_lk (call_frame_t *frame, xlator_t *this,
              void *data)
{
        clnt_args_t     *args       = NULL;
        gfs3_lk_req      req        = {{0,},};
        int32_t          gf_cmd     = 0;
        int32_t          gf_type    = 0;
        int64_t          remote_fd  = -1;
        clnt_local_t    *local      = NULL;
        clnt_conf_t     *conf       = NULL;
        int              op_errno   = ESTALE;
        int              ret        = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;
        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        ret = client_cmd_to_gf_cmd (args->cmd, &gf_cmd);
        if (ret) {
                op_errno = EINVAL;
                gf_log (this->name, GF_LOG_WARNING,
                        "Unknown cmd (%d)!", gf_cmd);
                goto unwind;
        }

        switch (args->flock->l_type) {
        case F_RDLCK:
                gf_type = GF_LK_F_RDLCK;
                break;
        case F_WRLCK:
                gf_type = GF_LK_F_WRLCK;
                break;
        case F_UNLCK:
                gf_type = GF_LK_F_UNLCK;
                break;
        }

        local->owner = frame->root->lk_owner;
        local->cmd   = args->cmd;
        local->fd    = fd_ref (args->fd);
        frame->local = local;

        req.fd    = remote_fd;
        req.cmd   = gf_cmd;
        req.type  = gf_type;
        gf_proto_flock_from_flock (&req.flock, args->flock);

        memcpy (req.gfid, args->fd->inode->gfid, 16);

        ret = client_submit_request (this, &req, frame, conf->fops, GFS3_OP_LK,
                                     client3_1_lk_cbk, NULL,
                                     NULL, 0, NULL, 0, NULL,
                                     (xdrproc_t)xdr_gfs3_lk_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (lk, frame, -1, op_errno, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}


int32_t
client3_1_inodelk (call_frame_t *frame, xlator_t *this,
                   void *data)
{
        clnt_conf_t      *conf     = NULL;
        clnt_args_t      *args    = NULL;
        gfs3_inodelk_req  req     = {{0,},};
        int               ret     = 0;
        int32_t           gf_cmd  = 0;
        int32_t           gf_type = 0;
        int               op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        if (!(args->loc && args->loc->inode))
                goto unwind;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        if (args->cmd == F_GETLK || args->cmd == F_GETLK64)
                gf_cmd = GF_LK_GETLK;
        else if (args->cmd == F_SETLK || args->cmd == F_SETLK64)
                gf_cmd = GF_LK_SETLK;
        else if (args->cmd == F_SETLKW || args->cmd == F_SETLKW64)
                gf_cmd = GF_LK_SETLKW;
        else {
                gf_log (this->name, GF_LOG_WARNING,
                        "Unknown cmd (%d)!", gf_cmd);
                op_errno = EINVAL;
                goto unwind;
        }

        switch (args->flock->l_type) {
        case F_RDLCK:
                gf_type = GF_LK_F_RDLCK;
                break;
        case F_WRLCK:
                gf_type = GF_LK_F_WRLCK;
                break;
        case F_UNLCK:
                gf_type = GF_LK_F_UNLCK;
                break;
        }

        req.volume = (char *)args->volume;
        req.cmd    = gf_cmd;
        req.type   = gf_type;
        gf_proto_flock_from_flock (&req.flock, args->flock);

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_INODELK,
                                     client3_1_inodelk_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_inodelk_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (inodelk, frame, -1, op_errno, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_finodelk (call_frame_t *frame, xlator_t *this,
                    void *data)
{
        clnt_args_t       *args     = NULL;
        gfs3_finodelk_req  req      = {{0,},};
        int32_t            gf_cmd   = 0;
        int32_t            gf_type  = 0;
        int64_t            remote_fd = -1;
        clnt_conf_t       *conf     = NULL;
        int                op_errno = ESTALE;
        int           ret        = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        if (args->cmd == F_GETLK || args->cmd == F_GETLK64)
                gf_cmd = GF_LK_GETLK;
        else if (args->cmd == F_SETLK || args->cmd == F_SETLK64)
                gf_cmd = GF_LK_SETLK;
        else if (args->cmd == F_SETLKW || args->cmd == F_SETLKW64)
                gf_cmd = GF_LK_SETLKW;
        else {
                gf_log (this->name, GF_LOG_WARNING,
                        "Unknown cmd (%d)!", gf_cmd);
                goto unwind;
        }

        switch (args->flock->l_type) {
        case F_RDLCK:
                gf_type = GF_LK_F_RDLCK;
                break;
        case F_WRLCK:
                gf_type = GF_LK_F_WRLCK;
                break;
        case F_UNLCK:
                gf_type = GF_LK_F_UNLCK;
                break;
        }

        req.volume = (char *)args->volume;
        req.fd    = remote_fd;
        req.cmd   = gf_cmd;
        req.type  = gf_type;
        gf_proto_flock_from_flock (&req.flock, args->flock);
        memcpy (req.gfid, args->fd->inode->gfid, 16);

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FINODELK,
                                     client3_1_finodelk_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_finodelk_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);
        return 0;
unwind:
        CLIENT_STACK_UNWIND (finodelk, frame, -1, op_errno, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}


int32_t
client3_1_entrylk (call_frame_t *frame, xlator_t *this,
                   void *data)
{
        clnt_conf_t      *conf     = NULL;
        clnt_args_t      *args     = NULL;
        gfs3_entrylk_req  req      = {{0,},};
        int               ret      = 0;
        int               op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->loc && args->loc->inode))
                goto unwind;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid,  args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        req.cmd = args->cmd_entrylk;
        req.type = args->type;
        req.volume = (char *)args->volume;
        req.name = "";
        if (args->basename) {
                req.name = (char *)args->basename;
                req.namelen = 1;
        }

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_ENTRYLK,
                                     client3_1_entrylk_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_entrylk_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (entrylk, frame, -1, op_errno, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_fentrylk (call_frame_t *frame, xlator_t *this,
                    void *data)
{
        clnt_args_t       *args     = NULL;
        gfs3_fentrylk_req  req      = {{0,},};
        int64_t            remote_fd = -1;
        clnt_conf_t       *conf     = NULL;
        int                op_errno = ESTALE;
        int           ret        = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.fd  = remote_fd;
        req.cmd = args->cmd_entrylk;
        req.type = args->type;
        req.volume = (char *)args->volume;
        req.name = "";
        if (args->basename) {
                req.name = (char *)args->basename;
                req.namelen = 1;
        }
        memcpy (req.gfid, args->fd->inode->gfid, 16);

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FENTRYLK,
                                     client3_1_fentrylk_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_fentrylk_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (fentrylk, frame, -1, op_errno, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}


int32_t
client3_1_rchecksum (call_frame_t *frame, xlator_t *this,
                     void *data)
{
        clnt_args_t        *args     = NULL;
        int64_t             remote_fd = -1;
        clnt_conf_t        *conf     = NULL;
        gfs3_rchecksum_req  req      = {0,};
        int                 op_errno = ESTALE;
        int                 ret        = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.len    = args->len;
        req.offset = args->offset;
        req.fd     = remote_fd;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_RCHECKSUM,
                                     client3_1_rchecksum_cbk, NULL,
                                     NULL, 0, NULL,
                                     0, NULL,
                                     (xdrproc_t)xdr_gfs3_rchecksum_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (rchecksum, frame, -1, op_errno, 0, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



int32_t
client3_1_readdir (call_frame_t *frame, xlator_t *this,
                   void *data)
{
        clnt_args_t      *args       = NULL;
        int64_t           remote_fd  = -1;
        clnt_conf_t      *conf       = NULL;
        gfs3_readdir_req  req        = {{0,},};
        gfs3_readdir_rsp  rsp        = {0, };
        clnt_local_t     *local      = NULL;
        int               op_errno   = ESTALE;
        int               ret        = 0;
        int               count      = 0;
        struct iobref    *rsp_iobref = NULL;
        struct iobuf     *rsp_iobuf  = NULL;
        struct iovec     *rsphdr     = NULL;
        struct iovec      vector[MAX_IOVEC] = {{0}, };
        int               readdir_rsp_size  = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        readdir_rsp_size = xdr_sizeof ((xdrproc_t) xdr_gfs3_readdir_rsp, &rsp)
                + args->size;

        if ((readdir_rsp_size + GLUSTERFS_RPC_REPLY_SIZE + GLUSTERFS_RDMA_MAX_HEADER_SIZE)
            > (GLUSTERFS_RDMA_INLINE_THRESHOLD)) {
                local = mem_get0 (this->local_pool);
                if (!local) {
                        op_errno = ENOMEM;
                        goto unwind;
                }
                frame->local = local;

                rsp_iobref = iobref_new ();
                if (rsp_iobref == NULL) {
                        goto unwind;
                }

                /* TODO: what is the size we should send ? */
                /* This iobuf will live for only receiving the response,
                   so not harmful */
                rsp_iobuf = iobuf_get (this->ctx->iobuf_pool);
                if (rsp_iobuf == NULL) {
                        goto unwind;
                }

                iobref_add (rsp_iobref, rsp_iobuf);
                iobuf_unref (rsp_iobuf);
                rsphdr = &vector[0];
                rsphdr->iov_base = iobuf_ptr (rsp_iobuf);
                rsphdr->iov_len  = iobuf_pagesize (rsp_iobuf);
                count = 1;
                rsp_iobuf = NULL;
                local->iobref = rsp_iobref;
        }

        req.size = args->size;
        req.offset = args->offset;
        req.fd = remote_fd;

        local->cmd = remote_fd;

        memcpy (req.gfid, args->fd->inode->gfid, 16);
        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_READDIR,
                                     client3_1_readdir_cbk, NULL,
                                     rsphdr, count,
                                     NULL, 0, rsp_iobref,
                                     (xdrproc_t)xdr_gfs3_readdir_req);
        rsp_iobref = NULL;

        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;

unwind:
        if (rsp_iobref != NULL) {
                iobref_unref (rsp_iobref);
        }

        if (rsp_iobuf != NULL) {
                iobuf_unref (rsp_iobuf);
        }

        CLIENT_STACK_UNWIND (readdir, frame, -1, op_errno, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}


int32_t
client3_1_readdirp (call_frame_t *frame, xlator_t *this,
                    void *data)
{
        clnt_args_t      *args              = NULL;
        gfs3_readdirp_req req               = {{0,},};
        gfs3_readdirp_rsp rsp               = {0,};
        int64_t           remote_fd         = -1;
        clnt_conf_t      *conf              = NULL;
        int               op_errno          = ESTALE;
        int               ret               = 0;
        int               count             = 0;
        int               readdirp_rsp_size = 0;
        struct iobref    *rsp_iobref        = NULL;
        struct iobuf     *rsp_iobuf         = NULL;
        struct iovec     *rsphdr            = NULL;
        struct iovec      vector[MAX_IOVEC] = {{0}, };
        clnt_local_t     *local             = NULL;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        readdirp_rsp_size = xdr_sizeof ((xdrproc_t) xdr_gfs3_readdirp_rsp, &rsp)
                + args->size;

        local = mem_get0 (this->local_pool);
        if (!local) {
                op_errno = ENOMEM;
                goto unwind;
        }
        frame->local = local;

        if ((readdirp_rsp_size + GLUSTERFS_RPC_REPLY_SIZE
             + GLUSTERFS_RDMA_MAX_HEADER_SIZE)
            > (GLUSTERFS_RDMA_INLINE_THRESHOLD)) {
                rsp_iobref = iobref_new ();
                if (rsp_iobref == NULL) {
                        goto unwind;
                }

                /* TODO: what is the size we should send ? */
                /* This iobuf will live for only receiving the response,
                   so not harmful */
                rsp_iobuf = iobuf_get (this->ctx->iobuf_pool);
                if (rsp_iobuf == NULL) {
                        goto unwind;
                }

                iobref_add (rsp_iobref, rsp_iobuf);
                iobuf_unref (rsp_iobuf);
                rsphdr = &vector[0];
                rsphdr->iov_base = iobuf_ptr (rsp_iobuf);
                rsphdr->iov_len  = iobuf_pagesize (rsp_iobuf);
                count = 1;
                rsp_iobuf = NULL;
                local->iobref = rsp_iobref;
                rsp_iobref = NULL;
        }

        local->fd = fd_ref (args->fd);

        req.size = args->size;
        req.offset = args->offset;
        req.fd = remote_fd;
        memcpy (req.gfid, args->fd->inode->gfid, 16);

        /* dict itself is 'xdata' here */
        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.dict.dict_val),
                                    req.dict.dict_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_READDIRP,
                                     client3_1_readdirp_cbk, NULL,
                                     rsphdr, count, NULL,
                                     0, rsp_iobref,
                                     (xdrproc_t)xdr_gfs3_readdirp_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.dict.dict_val)
                GF_FREE (req.dict.dict_val);

        return 0;
unwind:
        if (rsp_iobref) {
                iobref_unref (rsp_iobref);
        }

        if (rsp_iobuf) {
                iobuf_unref (rsp_iobuf);
        }

        if (req.dict.dict_val)
                GF_FREE (req.dict.dict_val);

        CLIENT_STACK_UNWIND (readdirp, frame, -1, op_errno, NULL, NULL);
        return 0;
}


int32_t
client3_1_setattr (call_frame_t *frame, xlator_t *this,
                   void *data)
{
        clnt_conf_t      *conf     = NULL;
        clnt_args_t      *args     = NULL;
        gfs3_setattr_req  req      = {{0,},};
        int               ret      = 0;
        int               op_errno = ESTALE;

        if (!frame || !this || !data)
                goto unwind;

        args = data;

        if (!(args->loc && args->loc->inode))
                goto unwind;

        if (!uuid_is_null (args->loc->inode->gfid))
                memcpy (req.gfid, args->loc->inode->gfid, 16);
        else
                memcpy (req.gfid, args->loc->gfid, 16);

        GF_ASSERT_AND_GOTO_WITH_ERROR (this->name,
                                       !uuid_is_null (*((uuid_t*)req.gfid)),
                                       unwind, op_errno, EINVAL);
        req.valid = args->valid;
        gf_stat_from_iatt (&req.stbuf, args->stbuf);

        conf = this->private;

        GF_PROTOCOL_DICT_SERIALIZE (this, args->xdata, (&req.xdata.xdata_val),
                                    req.xdata.xdata_len, op_errno, unwind);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_SETATTR,
                                     client3_1_setattr_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_setattr_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (setattr, frame, -1, op_errno, NULL, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}

int32_t
client3_1_fsetattr (call_frame_t *frame, xlator_t *this, void *data)
{
        clnt_args_t       *args     = NULL;
        int64_t            remote_fd = -1;
        clnt_conf_t       *conf     = NULL;
        gfs3_fsetattr_req  req      = {0,};
        int                op_errno = ESTALE;
        int                ret        = 0;

        if (!frame || !this || !data)
                goto unwind;

        args = data;
        conf = this->private;

        CLIENT_GET_REMOTE_FD(conf, args->fd, remote_fd, op_errno, unwind);

        req.fd = remote_fd;
        req.valid = args->valid;
        gf_stat_from_iatt (&req.stbuf, args->stbuf);

        ret = client_submit_request (this, &req, frame, conf->fops,
                                     GFS3_OP_FSETATTR,
                                     client3_1_fsetattr_cbk, NULL,
                                     NULL, 0, NULL, 0,
                                     NULL, (xdrproc_t)xdr_gfs3_fsetattr_req);
        if (ret) {
                gf_log (this->name, GF_LOG_WARNING, "failed to send the fop");
        }

        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
unwind:
        CLIENT_STACK_UNWIND (fsetattr, frame, -1, op_errno, NULL, NULL, NULL);
        if (req.xdata.xdata_val)
                GF_FREE (req.xdata.xdata_val);

        return 0;
}



/* Table Specific to FOPS */


rpc_clnt_procedure_t clnt3_1_fop_actors[GF_FOP_MAXVALUE] = {
        [GF_FOP_NULL]        = { "NULL",        NULL},
        [GF_FOP_STAT]        = { "STAT",        client3_1_stat },
        [GF_FOP_READLINK]    = { "READLINK",    client3_1_readlink },
        [GF_FOP_MKNOD]       = { "MKNOD",       client3_1_mknod },
        [GF_FOP_MKDIR]       = { "MKDIR",       client3_1_mkdir },
        [GF_FOP_UNLINK]      = { "UNLINK",      client3_1_unlink },
        [GF_FOP_RMDIR]       = { "RMDIR",       client3_1_rmdir },
        [GF_FOP_SYMLINK]     = { "SYMLINK",     client3_1_symlink },
        [GF_FOP_RENAME]      = { "RENAME",      client3_1_rename },
        [GF_FOP_LINK]        = { "LINK",        client3_1_link },
        [GF_FOP_TRUNCATE]    = { "TRUNCATE",    client3_1_truncate },
        [GF_FOP_OPEN]        = { "OPEN",        client3_1_open },
        [GF_FOP_READ]        = { "READ",        client3_1_readv },
        [GF_FOP_WRITE]       = { "WRITE",       client3_1_writev },
        [GF_FOP_WRITE_VERS]  = { "WRITE_VERS",  client3_1_writev_vers },
        [GF_FOP_STATFS]      = { "STATFS",      client3_1_statfs },
        [GF_FOP_FLUSH]       = { "FLUSH",       client3_1_flush },
        [GF_FOP_FSYNC]       = { "FSYNC",       client3_1_fsync },
        [GF_FOP_SETXATTR]    = { "SETXATTR",    client3_1_setxattr },
        [GF_FOP_GETXATTR]    = { "GETXATTR",    client3_1_getxattr },
        [GF_FOP_REMOVEXATTR] = { "REMOVEXATTR", client3_1_removexattr },
        [GF_FOP_OPENDIR]     = { "OPENDIR",     client3_1_opendir },
        [GF_FOP_FSYNCDIR]    = { "FSYNCDIR",    client3_1_fsyncdir },
        [GF_FOP_ACCESS]      = { "ACCESS",      client3_1_access },
        [GF_FOP_CREATE]      = { "CREATE",      client3_1_create },
        [GF_FOP_FTRUNCATE]   = { "FTRUNCATE",   client3_1_ftruncate },
        [GF_FOP_FSTAT]       = { "FSTAT",       client3_1_fstat },
        [GF_FOP_LK]          = { "LK",          client3_1_lk },
        [GF_FOP_LOOKUP]      = { "LOOKUP",      client3_1_lookup },
        [GF_FOP_READDIR]     = { "READDIR",     client3_1_readdir },
        [GF_FOP_INODELK]     = { "INODELK",     client3_1_inodelk },
        [GF_FOP_FINODELK]    = { "FINODELK",    client3_1_finodelk },
        [GF_FOP_ENTRYLK]     = { "ENTRYLK",     client3_1_entrylk },
        [GF_FOP_FENTRYLK]    = { "FENTRYLK",    client3_1_fentrylk },
        [GF_FOP_XATTROP]     = { "XATTROP",     client3_1_xattrop },
        [GF_FOP_FXATTROP]    = { "FXATTROP",    client3_1_fxattrop },
        [GF_FOP_FGETXATTR]   = { "FGETXATTR",   client3_1_fgetxattr },
        [GF_FOP_FSETXATTR]   = { "FSETXATTR",   client3_1_fsetxattr },
        [GF_FOP_RCHECKSUM]   = { "RCHECKSUM",   client3_1_rchecksum },
        [GF_FOP_SETATTR]     = { "SETATTR",     client3_1_setattr },
        [GF_FOP_FSETATTR]    = { "FSETATTR",    client3_1_fsetattr },
        [GF_FOP_READDIRP]    = { "READDIRP",    client3_1_readdirp },
        [GF_FOP_RELEASE]     = { "RELEASE",     client3_1_release },
        [GF_FOP_RELEASEDIR]  = { "RELEASEDIR",  client3_1_releasedir },
        [GF_FOP_GETSPEC]     = { "GETSPEC",     client3_getspec },
        [GF_FOP_FREMOVEXATTR] = { "FREMOVEXATTR", client3_1_fremovexattr },
};

/* Used From RPC-CLNT library to log proper name of procedure based on number */
char *clnt3_1_fop_names[GFS3_OP_MAXVALUE] = {
        [GFS3_OP_NULL]        = "NULL",
        [GFS3_OP_STAT]        = "STAT",
        [GFS3_OP_READLINK]    = "READLINK",
        [GFS3_OP_MKNOD]       = "MKNOD",
        [GFS3_OP_MKDIR]       = "MKDIR",
        [GFS3_OP_UNLINK]      = "UNLINK",
        [GFS3_OP_RMDIR]       = "RMDIR",
        [GFS3_OP_SYMLINK]     = "SYMLINK",
        [GFS3_OP_RENAME]      = "RENAME",
        [GFS3_OP_LINK]        = "LINK",
        [GFS3_OP_TRUNCATE]    = "TRUNCATE",
        [GFS3_OP_OPEN]        = "OPEN",
        [GFS3_OP_READ]        = "READ",
        [GFS3_OP_WRITE]       = "WRITE",
        [GFS3_OP_STATFS]      = "STATFS",
        [GFS3_OP_FLUSH]       = "FLUSH",
        [GFS3_OP_FSYNC]       = "FSYNC",
        [GFS3_OP_SETXATTR]    = "SETXATTR",
        [GFS3_OP_GETXATTR]    = "GETXATTR",
        [GFS3_OP_REMOVEXATTR] = "REMOVEXATTR",
        [GFS3_OP_OPENDIR]     = "OPENDIR",
        [GFS3_OP_FSYNCDIR]    = "FSYNCDIR",
        [GFS3_OP_ACCESS]      = "ACCESS",
        [GFS3_OP_CREATE]      = "CREATE",
        [GFS3_OP_FTRUNCATE]   = "FTRUNCATE",
        [GFS3_OP_FSTAT]       = "FSTAT",
        [GFS3_OP_LK]          = "LK",
        [GFS3_OP_LOOKUP]      = "LOOKUP",
        [GFS3_OP_READDIR]     = "READDIR",
        [GFS3_OP_INODELK]     = "INODELK",
        [GFS3_OP_FINODELK]    = "FINODELK",
        [GFS3_OP_ENTRYLK]     = "ENTRYLK",
        [GFS3_OP_FENTRYLK]    = "FENTRYLK",
        [GFS3_OP_XATTROP]     = "XATTROP",
        [GFS3_OP_FXATTROP]    = "FXATTROP",
        [GFS3_OP_FGETXATTR]   = "FGETXATTR",
        [GFS3_OP_FSETXATTR]   = "FSETXATTR",
        [GFS3_OP_RCHECKSUM]   = "RCHECKSUM",
        [GFS3_OP_SETATTR]     = "SETATTR",
        [GFS3_OP_FSETATTR]    = "FSETATTR",
        [GFS3_OP_READDIRP]    = "READDIRP",
        [GFS3_OP_RELEASE]     = "RELEASE",
        [GFS3_OP_RELEASEDIR]  = "RELEASEDIR",
        [GFS3_OP_FREMOVEXATTR] = "FREMOVEXATTR",
};

rpc_clnt_prog_t clnt3_1_fop_prog = {
        .progname  = "GlusterFS 3.1",
        .prognum   = GLUSTER3_1_FOP_PROGRAM,
        .progver   = GLUSTER3_1_FOP_VERSION,
        .numproc   = GLUSTER3_1_FOP_PROCCNT,
        .proctable = clnt3_1_fop_actors,
        .procnames = clnt3_1_fop_names,
};
