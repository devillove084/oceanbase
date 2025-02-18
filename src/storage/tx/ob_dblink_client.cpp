// Copyright (c) 2021 OceanBase
// OceanBase is licensed under Mulan PubL v2.
// You can use this software according to the terms and conditions of the Mulan PubL v2.
// You may obtain a copy of Mulan PubL v2 at:
//          http://license.coscl.org.cn/MulanPubL-2.0
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PubL v2 for more details.

#include "storage/tx/ob_dblink_client.h"
#include "share/ob_define.h"
#include "lib/mysqlclient/ob_mysql_proxy.h"
#include "observer/ob_server_struct.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace share;
using namespace pl;

namespace transaction
{
void ObDBLinkClient::reset()
{
  index_ = 0;
  xid_.reset();
  state_ = ObDBLinkClientState::IDLE;
  dblink_type_ = sqlclient::DblinkDriverProto::DBLINK_UNKNOWN;
  dblink_conn_ = NULL;
  impl_ = NULL;
  if (NULL != impl_) {
    ob_free(impl_);
    impl_ = NULL;
  }
  is_inited_ = false;
}

int ObDBLinkClient::init(const uint32_t index,
                         const DblinkDriverProto dblink_type,
                         const int64_t tx_timeout_us,
                         ObISQLConnection *dblink_conn)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    TRANS_LOG(WARN, "init twice", K(ret), K(*this));
  } else if (DblinkDriverProto::DBLINK_UNKNOWN == dblink_type
      || NULL == dblink_conn
      || 0 > tx_timeout_us
      || 0 == index) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "invalid arguments", K(ret), K(dblink_type), KP(dblink_conn),
        K(tx_timeout_us), K(index));
  } else {
    index_ = index;
    dblink_conn_ = dblink_conn;
    dblink_type_ = dblink_type;
    tx_timeout_us_ = tx_timeout_us;
    is_inited_ = true;
    TRANS_LOG(INFO, "init", K(*this));
  }
  return ret;
}

// execute xa start for dblink client
// 1. if START, return success directly
// 2. if IDLE, execute xa start
// @param[in] xid
int ObDBLinkClient::rm_xa_start(const ObXATransID &xid)
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    TRANS_LOG(WARN, "dblink client is not inited", K(ret), K(xid), K(*this));
  } else if (!xid.is_valid() || xid.empty()) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), K(xid));
  } else if (ObDBLinkClientState::IDLE != state_) {
    if (ObDBLinkClientState::START == state_
        && xid.all_equal_to(xid_)) {
      // return OB_SUCCESS
    } else {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpected dblink client", K(ret), K(xid), K(*this));
    }
  // TODO, check connection
  } else {
    if (OB_FAIL(init_query_impl_())) {
      TRANS_LOG(WARN, "fail to init query impl", K(ret), K(xid), K(*this));
    } else if (NULL == impl_) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpected query impl", K(ret), K(xid), K(*this));
    } else if (OB_FAIL(impl_->xa_start(xid, ObXAFlag::TMNOFLAGS))) {
      TRANS_LOG(WARN, "fail to execute query", K(ret), K(xid), K(*this));
    } else {
      xid_ = xid;
      state_ = ObDBLinkClientState::START;
    }
    TRANS_LOG(INFO, "rm xa start for dblink", K(ret), K(xid));
  }
  return ret;
}

// execute xa end for dblink client
// 1. if END, return success directly
// 2. if START, execute xa end
int ObDBLinkClient::rm_xa_end()
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    TRANS_LOG(WARN, "dblink client is not inited", K(ret), K(*this));
  } else if (!xid_.is_valid() || xid_.empty()) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "invalid xid", K(ret), K(*this));
  } else if (NULL == impl_) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "invalid query impl", K(ret), K(*this));
  // TODO, check connection
  } else {
    ret = rm_xa_end_();
  }
  return ret;
}

// execute xa prepare for dblink client
// 1. if START, execute xa end first
// 2. if END, execute xa prepare
// 3. if PREPARED or RDONLY_PREPARED, return success directly
int ObDBLinkClient::rm_xa_prepare()
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);

  // step 1, execute xa end if necessary
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    TRANS_LOG(WARN, "dblink client is not inited", K(ret), K(*this));
  } else if (!xid_.is_valid() || xid_.empty()) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "invalid xid", K(ret), K(*this));
  } else if (NULL == impl_) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "invalid query impl", K(ret), K(*this));
  // TODO, check connection
  } else if (ObDBLinkClientState::START == state_) {
    if (OB_FAIL(rm_xa_end_())) {
      TRANS_LOG(WARN, "fail to execute xa end", K(ret), K(*this));
    }
  }

  // step 2, execute xa prepare
  if (OB_SUCCESS != ret) {
  } else if (ObDBLinkClientState::END != state_) {
    if (ObDBLinkClientState::PREPARED == state_
        || ObDBLinkClientState::RDONLY_PREPARED == state_) {
      // return OB_SUCCESS
    } else {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpected dblink client", K(ret), K(*this));
    }
  } else {
    state_ = ObDBLinkClientState::PREPARING;
    if (OB_FAIL(impl_->xa_prepare(xid_))) {
      if (OB_TRANS_XA_RDONLY != ret) {
        TRANS_LOG(WARN, "fail to execute query", K(ret), K(*this));
      }
    }
    if (OB_SUCCESS == ret) {
      state_ = ObDBLinkClientState::PREPARED;
    } else if (OB_TRANS_XA_RDONLY == ret) {
      state_ = ObDBLinkClientState::RDONLY_PREPARED;
    } else {
      // TODO, handle exceptions
    }
    TRANS_LOG(INFO, "rm xa prepare for dblink", K(ret), K_(xid));
  }
  return ret;
}

// execute xa commit for dblink client
// NOTE that this function can be called only if all participants are prepared successfully
// 1. if COMMITTED or RDONLY_PREPARED, return success directly
// 2. if PREPARED, execute xa commit
int ObDBLinkClient::rm_xa_commit()
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);
  ObSqlString sql;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    TRANS_LOG(WARN, "dblink client is not inited", K(ret), K(*this));
  } else if (!xid_.is_valid() || xid_.empty()) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "invalid xid", K(ret), K(xid_));
  } else if (NULL == impl_) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "invalid query impl", K(ret), K(*this));
  // TODO, check connection
  } else if (ObDBLinkClientState::PREPARED != state_) {
    if (ObDBLinkClientState::COMMITTED == state_
        || ObDBLinkClientState::RDONLY_PREPARED == state_) {
      // return OB_SUCCESS
    } else {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpected dblink client", K(ret), K(*this));
    }
  } else {
    // two phase commit
    const int64_t flags = ObXAFlag::TMNOFLAGS;
    state_ = ObDBLinkClientState::COMMITTING;
    if (OB_FAIL(impl_->xa_commit(xid_, flags))) {
      TRANS_LOG(WARN, "fail to execute query", K(ret), K(*this));
    } else {
      state_ = ObDBLinkClientState::COMMITTED;
    }
    if (OB_SUCCESS != ret) {
      // TODO, handle exceptions
    }
    TRANS_LOG(INFO, "rm xa commit for dblink", K(ret), K_(xid));
  }
  return ret;
}

// execute xa rollback for dblink client
// 1. if START, execute xa end first
// 2. if END, execute xa rollback
// 3. if RDONLY_PREPARED, return success directly
// 4. if PREPARED, execute xa rollback
int ObDBLinkClient::rm_xa_rollback()
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);
  ObSqlString sql;

  // step 1, execute xa end if necessary
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    TRANS_LOG(WARN, "dblink client is not inited", K(ret), K(*this));
  } else if (!xid_.is_valid() || xid_.empty()) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "invalid xid", K(ret), K(xid_));
  } else if (NULL == impl_) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "invalid query impl", K(ret), K(*this));
  // TODO, check connection
  } else if (ObDBLinkClientState::START == state_) {
    if (OB_FAIL(rm_xa_end_())) {
      TRANS_LOG(WARN, "fail to execute xa end", K(ret), K(*this));
    }
  }

  // step 2, execute xa rollback
  if (OB_SUCCESS != ret) {
  } else if (ObDBLinkClientState::PREPARED != state_
      && ObDBLinkClientState::END != state_
      && ObDBLinkClientState::PREPARING != state_) {
    if (ObDBLinkClientState::ROLLBACKED == state_
        || ObDBLinkClientState::RDONLY_PREPARED == state_) {
      // return OB_SUCCESS
    } else {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpected dblink client", K(ret), K(*this));
    }
  } else {
    state_ = ObDBLinkClientState::ROLLBACKING;
    if (OB_FAIL(impl_->xa_rollback(xid_))) {
      TRANS_LOG(WARN, "fail to execute query", K(ret), K(*this));
    } else {
      state_ = ObDBLinkClientState::ROLLBACKED;
    }
    if (OB_SUCCESS != ret) {
      // TODO, handle exceptions
    }
    TRANS_LOG(INFO, "rm xa rollback for dblink", K(ret), K_(xid));
  }
  return ret;
}

int ObDBLinkClient::rm_xa_end_()
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (ObDBLinkClientState::START != state_) {
    if (ObDBLinkClientState::END == state_) {
      // return OB_SUCCESS
    } else {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpected dblink client", K(ret), K(*this));
    }
  } else {
    if (OB_FAIL(impl_->xa_end(xid_, ObXAFlag::TMSUCCESS))) {
      TRANS_LOG(WARN, "fail to do xa end", K(ret), K(*this));
    } else {
      state_ = ObDBLinkClientState::END;
    }
    if (OB_SUCCESS != ret) {
      // TODO, handle exceptions
    }
    TRANS_LOG(INFO, "rm xa end for dblink", K(ret), K_(xid));
  }
  return ret;
}

bool ObDBLinkClient::is_started(const ObXATransID &xid)
{
  // TODO, check xid
  return ObDBLinkClientState::START == state_;
}

bool ObDBLinkClient::equal(ObISQLConnection *dblink_conn)
{
  return dblink_conn_ == dblink_conn;
}

int ObDBLinkClient::init_query_impl_()
{
  int ret = OB_SUCCESS;
  if (NULL == impl_) {
    if (DblinkDriverProto::DBLINK_DRV_OB == dblink_type_) {
      void *ptr = NULL;
      if (NULL == (ptr = ob_malloc(sizeof(ObXAQueryObImpl), "ObXAQuery"))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        TRANS_LOG(WARN, "fail to allocate memory", K(ret), K(*this));
      } else {
        impl_ = new(ptr) ObXAQueryObImpl();
        ObXAQueryObImpl *ob_impl = NULL;
        if (NULL == (ob_impl = dynamic_cast<ObXAQueryObImpl*>(impl_))) {
          ret = OB_ERR_UNEXPECTED;
          TRANS_LOG(WARN, "unexpected query impl for ob", K(ret), K(*this));
        } else if (OB_FAIL(ob_impl->init(dblink_conn_))) {
          TRANS_LOG(WARN, "fail to init query impl", K(ret), K(*this));
        } else {
          // set tx variables
          static const int64_t MIN_TIMEOUT_US = 20 * 1000 * 1000;  // 20s
          const int64_t timeout_us = tx_timeout_us_ + MIN_TIMEOUT_US;
          if (OB_FAIL(dblink_conn_->set_session_variable("ob_trx_timeout", timeout_us))) {
            TRANS_LOG(WARN, "fail to set transaction timeout", K(ret), K(timeout_us), K(*this));
          }
        }
        if (OB_SUCCESS != ret) {
          ob_free(impl_);
          impl_ = NULL;
        }
      }
    } else if (DblinkDriverProto::DBLINK_DRV_OCI == dblink_type_) {
      void *ptr = NULL;
      if (NULL == (ptr = ob_malloc(sizeof(ObXAQueryOraImpl), "ObXAQuery"))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        TRANS_LOG(WARN, "fail to allocate memory", K(ret), K(*this));
      } else {
        impl_ = new(ptr) ObXAQueryOraImpl();
        ObXAQueryOraImpl *ora_impl = NULL;
        if (NULL == (ora_impl = dynamic_cast<ObXAQueryOraImpl*>(impl_))) {
          ret = OB_ERR_UNEXPECTED;
          TRANS_LOG(WARN, "unexpected query impl for oracle", K(ret), K(*this));
        } else if (OB_FAIL(ora_impl->init(dblink_conn_))) {
          TRANS_LOG(WARN, "fail to init query impl", K(ret), K(*this));
        } else {
          // do nothing
        }
        if (OB_SUCCESS != ret) {
          ob_free(impl_);
          impl_ = NULL;
        }
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpected dblink type", K(ret), K(*this));
    }
  }
  return ret;
}

bool ObDBLinkClient::is_valid_dblink_type(const DblinkDriverProto dblink_type)
{
  bool ret_bool = true;
  if (DblinkDriverProto::DBLINK_DRV_OB != dblink_type
      && DblinkDriverProto::DBLINK_DRV_OCI != dblink_type) {
    ret_bool = false;
  }
  return ret_bool;
}

} // transaction
} // oceanbase
