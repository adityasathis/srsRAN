/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "srsenb/hdr/stack/mac/schedulers/sched_time_pf.h"

namespace srsenb {

using srslte::tti_point;

sched_time_pf::sched_time_pf(const sched_cell_params_t& cell_params_, const sched_interface::sched_args_t& sched_args)
{
  cc_cfg = &cell_params_;
  if (not sched_args.sched_policy_args.empty()) {
    fairness_coeff = std::stof(sched_args.sched_policy_args);
  }
}

void sched_time_pf::new_tti(std::map<uint16_t, sched_ue>& ue_db, sf_sched* tti_sched)
{
  current_tti_rx = tti_point{tti_sched->get_tti_rx()};
  // remove deleted users from history
  for (auto it = ue_history_db.begin(); it != ue_history_db.end();) {
    if (not ue_db.count(it->first)) {
      it = ue_history_db.erase(it);
    } else {
      ++it;
    }
  }
  // add new users to history db, and update priority queues
  for (auto& u : ue_db) {
    auto it = ue_history_db.find(u.first);
    if (it == ue_history_db.end()) {
      it = ue_history_db.insert(std::make_pair(u.first, ue_ctxt{u.first, fairness_coeff})).first;
    }
    it->second.new_tti(*cc_cfg, u.second, tti_sched);
    if (it->second.dl_newtx_h != nullptr or it->second.dl_retx_h != nullptr) {
      dl_queue.push(&it->second);
    }
    if (it->second.ul_h != nullptr) {
      ul_queue.push(&it->second);
    }
  }
}

/*****************************************************************
 *                         Dowlink
 *****************************************************************/

void sched_time_pf::sched_dl_users(std::map<uint16_t, sched_ue>& ue_db, sf_sched* tti_sched)
{
  srslte::tti_point tti_rx{tti_sched->get_tti_rx()};
  if (current_tti_rx != tti_rx) {
    new_tti(ue_db, tti_sched);
  }

  while (not dl_queue.empty()) {
    ue_ctxt& ue = *dl_queue.top();
    ue.save_dl_alloc(try_dl_alloc(ue, ue_db[ue.rnti], tti_sched), 0.01);
    dl_queue.pop();
  }
}

uint32_t sched_time_pf::try_dl_alloc(ue_ctxt& ue_ctxt, sched_ue& ue, sf_sched* tti_sched)
{
  alloc_outcome_t code = alloc_outcome_t::ERROR;
  if (ue_ctxt.dl_retx_h != nullptr) {
    code = try_dl_retx_alloc(*tti_sched, ue, *ue_ctxt.dl_retx_h);
    if (code == alloc_outcome_t::SUCCESS) {
      return ue_ctxt.dl_retx_h->get_tbs(0) + ue_ctxt.dl_retx_h->get_tbs(1);
    }
  }
  if (code != alloc_outcome_t::DCI_COLLISION and ue_ctxt.dl_newtx_h != nullptr) {
    rbg_interval req_rbgs = ue.get_required_dl_rbgs(ue_ctxt.ue_cc_idx);
    // Check if there is an empty harq for the newtx
    if (req_rbgs.stop() == 0) {
      return 0;
    }
    // Allocate resources based on pending data
    rbgmask_t newtx_mask = find_available_dl_rbgs(req_rbgs.stop(), tti_sched->get_dl_mask());
    if (newtx_mask.count() >= req_rbgs.start()) {
      // empty RBGs were found
      code = tti_sched->alloc_dl_user(&ue, newtx_mask, ue_ctxt.dl_newtx_h->get_id());
      if (code == alloc_outcome_t::SUCCESS) {
        return ue_ctxt.dl_newtx_h->get_tbs(0) + ue_ctxt.dl_newtx_h->get_tbs(1);
      }
    }
  }
  if (code == alloc_outcome_t::DCI_COLLISION) {
    log_h->info("SCHED: Couldn't find space in PDCCH for DL tx for rnti=0x%x\n", ue.get_rnti());
  }
  return 0;
}

/*****************************************************************
 *                         Uplink
 *****************************************************************/

void sched_time_pf::sched_ul_users(std::map<uint16_t, sched_ue>& ue_db, sf_sched* tti_sched)
{
  srslte::tti_point tti_rx{tti_sched->get_tti_rx()};
  if (current_tti_rx != tti_rx) {
    new_tti(ue_db, tti_sched);
  }

  while (not ul_queue.empty()) {
    ue_ctxt& ue = *ul_queue.top();
    ue.save_ul_alloc(try_ul_alloc(ue, ue_db[ue.rnti], tti_sched), 0.01);
    ul_queue.pop();
  }
}

uint32_t sched_time_pf::try_ul_alloc(ue_ctxt& ue_ctxt, sched_ue& ue, sf_sched* tti_sched)
{
  alloc_outcome_t code = alloc_outcome_t::ERROR;
  if (ue_ctxt.ul_h != nullptr and ue_ctxt.ul_h->has_pending_retx()) {
    code = try_ul_retx_alloc(*tti_sched, ue, *ue_ctxt.ul_h);
  } else if (ue_ctxt.ul_h != nullptr) {
    uint32_t pending_data = ue.get_pending_ul_new_data(tti_sched->get_tti_tx_ul(), ue_ctxt.ue_cc_idx);
    // Check if there is a empty harq, and data to transmit
    if (pending_data == 0) {
      return 0;
    }
    uint32_t     pending_rb = ue.get_required_prb_ul(ue_ctxt.ue_cc_idx, pending_data);
    prb_interval alloc      = find_contiguous_ul_prbs(pending_rb, tti_sched->get_ul_mask());
    if (alloc.empty()) {
      return 0;
    }
    code = tti_sched->alloc_ul_user(&ue, alloc);
  }
  if (code == alloc_outcome_t::DCI_COLLISION) {
    log_h->info("SCHED: Couldn't find space in PDCCH for UL retx of rnti=0x%x\n", ue.get_rnti());
  }
  return code == alloc_outcome_t::SUCCESS ? ue_ctxt.ul_h->get_pending_data() : 0;
}

/*****************************************************************
 *                          UE history
 *****************************************************************/

void sched_time_pf::ue_ctxt::new_tti(const sched_cell_params_t& cell, sched_ue& ue, sf_sched* tti_sched)
{
  dl_retx_h  = nullptr;
  dl_newtx_h = nullptr;
  ul_h       = nullptr;
  dl_prio    = 0;
  ue_cc_idx  = ue.enb_to_ue_cc_idx(cell.enb_cc_idx);
  if (ue_cc_idx < 0) {
    // not active
    return;
  }

  // Calculate DL priority
  dl_retx_h  = get_dl_retx_harq(ue, tti_sched);
  dl_newtx_h = get_dl_newtx_harq(ue, tti_sched);
  if (dl_retx_h != nullptr or dl_newtx_h != nullptr) {
    // calculate DL PF priority
    float r = ue.get_expected_dl_bitrate(ue_cc_idx) / 8;
    float R = dl_avg_rate();
    dl_prio = (R != 0) ? pow(r, fairness_coeff) / R : (r == 0 ? 0 : std::numeric_limits<float>::max());
  }

  // Calculate UL priority
  ul_h = get_ul_retx_harq(ue, tti_sched);
  if (ul_h == nullptr) {
    ul_h = get_ul_newtx_harq(ue, tti_sched);
  }
  if (ul_h != nullptr) {
    float r = ue.get_expected_ul_bitrate(ue_cc_idx) / 8;
    float R = ul_avg_rate();
    ul_prio = (R != 0) ? r / R : (r == 0 ? 0 : std::numeric_limits<float>::max());
  }
}

void sched_time_pf::ue_ctxt::save_dl_alloc(uint32_t alloc_bytes, float exp_avg_alpha)
{
  if (dl_nof_samples < 1 / exp_avg_alpha) {
    // fast start
    dl_avg_rate_ = dl_avg_rate_ + (alloc_bytes - dl_avg_rate_) / (dl_nof_samples + 1);
  } else {
    dl_avg_rate_ = (1 - exp_avg_alpha) * dl_avg_rate_ + (exp_avg_alpha)*alloc_bytes;
  }
  dl_nof_samples++;
}

void sched_time_pf::ue_ctxt::save_ul_alloc(uint32_t alloc_bytes, float exp_avg_alpha)
{
  if (ul_nof_samples < 1 / exp_avg_alpha) {
    // fast start
    ul_avg_rate_ = ul_avg_rate_ + (alloc_bytes - ul_avg_rate_) / (ul_nof_samples + 1);
  } else {
    ul_avg_rate_ = (1 - exp_avg_alpha) * ul_avg_rate_ + (exp_avg_alpha)*alloc_bytes;
  }
  ul_nof_samples++;
}

bool sched_time_pf::ue_dl_prio_compare::operator()(const sched_time_pf::ue_ctxt* lhs,
                                                   const sched_time_pf::ue_ctxt* rhs) const
{
  bool is_retx1 = lhs->dl_retx_h != nullptr, is_retx2 = rhs->dl_retx_h != nullptr;
  return (not is_retx1 and is_retx2) or (is_retx1 == is_retx2 and lhs->dl_prio < rhs->dl_prio);
}

bool sched_time_pf::ue_ul_prio_compare::operator()(const sched_time_pf::ue_ctxt* lhs,
                                                   const sched_time_pf::ue_ctxt* rhs) const
{
  bool is_retx1 = lhs->ul_h->has_pending_retx(), is_retx2 = rhs->ul_h->has_pending_retx();
  return (not is_retx1 and is_retx2) or (is_retx1 == is_retx2 and lhs->ul_prio < rhs->ul_prio);
}

} // namespace srsenb
