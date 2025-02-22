/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_LOGSERVICE_OB_ROLE_CHANGE_SERVICE_
#define OCEANBASE_LOGSERVICE_OB_ROLE_CHANGE_SERVICE_
#include "lib/function/ob_function.h"
#include "lib/thread/thread_mgr_interface.h"
#include "lib/utility/ob_macro_utils.h"
#include "logservice/palf/palf_options.h"
#include "share/ob_ls_id.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "logservice/ob_log_handler.h"
#include "logservice/palf/palf_callback.h"
#include "logservice/applyservice/ob_log_apply_service.h"
#include "logservice/replayservice/ob_log_replay_service.h"
#include "logservice/rcservice/ob_role_change_handler.h"
namespace oceanbase
{
namespace logservice
{
enum class RoleChangeEventType {
  INVALID_RC_EVENT_TYPE = 0,
  CHANGE_LEADER_EVENT_TYPE = 1,
  ROLE_CHANGE_CB_EVENT_TYPE = 2,
  MAX_RC_EVENT_TYPE = 3
};

struct RoleChangeEvent {
  RoleChangeEvent(const RoleChangeEventType &event_type,
                  const share::ObLSID &ls_id);
  RoleChangeEvent(const RoleChangeEventType &event_type,
                  const share::ObLSID &ls_id,
                  const common::ObAddr &dst_addr);
  bool is_valid() const;
  RoleChangeEventType event_type_;
  share::ObLSID ls_id_;
  ObAddr dst_addr_;
  TO_STRING_KV(K_(event_type), K_(ls_id), K_(dst_addr));
};

class ObRoleChangeService : public lib::TGTaskHandler , public palf::PalfRoleChangeCb {
public:
  ObRoleChangeService();
  ~ObRoleChangeService();
  int init(storage::ObLSService *ls_service,
           logservice::ObLogApplyService *apply_service,
           logservice::ObILogReplayService *replay_service);
  int start();
  void wait();
  void stop();
  void destroy();
  void handle(void *task);
  int on_role_change(const int64_t id) final override;
  int on_need_change_leader(const int64_t ls_id, const common::ObAddr &dst_addr) final override;

private:
  int submit_role_change_event_(const RoleChangeEvent &event);
  int handle_role_change_event_(const RoleChangeEvent &event);

  int handle_role_change_cb_event_for_restore_handler_(const palf::AccessMode &curr_access_mode,
                                                       ObLS *ls);
  int handle_change_leader_event_for_restore_handler_(const common::ObAddr &dst_addr,
                                                      ObLS *ls);

  int handle_role_change_cb_event_for_log_handler_(const palf::AccessMode &curr_access_mode,
                                                   ObLS *ls);
  int handle_change_leader_event_for_log_handler_(const common::ObAddr &dst_addr,
                                                  ObLS *ls);

  int switch_follower_to_leader_(const int64_t new_proposal_id,
                                 ObLS *ls);
  int switch_leader_to_follower_forcedly_(const int64_t new_proposal_id,
                                          ObLS *ls);
  int switch_leader_to_follower_gracefully_(const int64_t new_proposal_id,
                                            const int64_t curr_proposal_id,
                                            const common::ObAddr &dst_addr,
                                            ObLS *ls);
  int switch_leader_to_leader_(const common::ObRole &new_role,
                               const int64_t new_proposal_id,
                               ObLS *ls);
  int switch_follower_to_follower_(const int64_t new_proposal_id, ObLS *ls);
  int switch_leader_to_leader_(const int64_t new_proposal_id,
                               const int64_t curr_proposal_id,
                               ObLS *ls);

  int switch_follower_to_leader_restore_(const int64_t new_proposal_id,
                                         ObLS *ls);
  int switch_leader_to_follower_forcedly_restore_(const int64_t new_proposal_id,
                                                  ObLS *ls);
  int switch_leader_to_follower_gracefully_restore_(const common::ObAddr &dst_addr,
                                                    const int64_t curr_proposal_id,
                                                    ObLS *ls);
  int switch_follower_to_follower_restore_();
  int switch_leader_to_leader_restore_(const int64_t new_proposal_id,
                                       const int64_t curr_proposal_id,
                                       ObLS *ls);
  int wait_replay_service_replay_done_(const share::ObLSID &ls_id,
                                       const palf::LSN &end_lsn);
  int wait_apply_service_apply_done_(const share::ObLSID &ls_id,
                                     palf::LSN &end_lsn);
  int wait_apply_service_apply_done_when_change_leader_(const ObLogHandler *log_handler,
                                                        const int64_t proposal_id,
                                                        const share::ObLSID &ls_id,
                                                        palf::LSN &end_lsn);
  bool check_need_execute_role_change_(const int64_t curr_proposal_id, const common::ObRole &curr_role,
                                       const int64_t new_proposal_id, const common::ObRole &new_role) const;
private:
  enum class RoleChangeOptType {
    INVALID_RC_OPT_TYPE = 0,
    FOLLOWER_2_LEADER = 1,
    LEADER_2_FOLLOWER = 2,
    FOLLOWER_2_FOLLOWER = 3,
    LEADER_2_LEADER = 4,
    MAX_RC_OPT_TYPE = 5
  };
  RoleChangeOptType get_role_change_opt_type_(const common::ObRole &old_role,
                                              const common::ObRole &new_role,
                                              const bool need_transform_by_access_mode) const;
public:
  static const int64_t MAX_THREAD_NUM = 1;
  static const int64_t MAX_RC_EVENT_TASK = 1024 * 1024;
private:
  DISALLOW_COPY_AND_ASSIGN(ObRoleChangeService);
private:
  static constexpr int64_t EACH_ROLE_CHANGE_COST_MAX_TIME = 1 * 1000 * 1000;
  storage::ObLSService *ls_service_;
  logservice::ObLogApplyService *apply_service_;
  logservice::ObILogReplayService *replay_service_;
  int tg_id_;
  bool is_inited_;
};
} // end namespace logservice
} // end namespce oceanbase
#endif
