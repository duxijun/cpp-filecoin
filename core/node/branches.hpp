/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CPP_FILECOIN_SYNC_BRANCHES_HPP
#define CPP_FILECOIN_SYNC_BRANCHES_HPP

#include "fwd.hpp"

#include <set>

namespace fc::sync {

  struct RenameBranch {
    BranchId old_id = kNoBranch;
    BranchId new_id = kNoBranch;
    Height above_height = 0;
    bool split = false;
  };

  struct BranchInfo {
    BranchId id = kNoBranch;
    TipsetHash top;
    Height top_height = 0;
    TipsetHash bottom;
    Height bottom_height = 0;
    BranchId parent = kNoBranch;
    TipsetHash parent_hash;

    bool synced_to_genesis = false;

    std::set<BranchId> forks;
  };

  using BranchCPtr = std::shared_ptr<const BranchInfo>;

  class Branches {
    using BranchPtr = std::shared_ptr<BranchInfo>;

   public:
    enum class Error {
      BRANCHES_LOAD_ERROR = 1,
      BRANCHES_NO_GENESIS_BRANCH,
      BRANCHES_PARENT_EXPECTED,
      BRANCHES_NO_CURRENT_CHAIN,
      BRANCHES_BRANCH_NOT_FOUND,
      BRANCHES_HEAD_NOT_FOUND,
      BRANCHES_HEAD_NOT_SYNCED,
      BRANCHES_CYCLE_DETECTED,
      BRANCHES_STORE_ERROR,
      BRANCHES_HEIGHT_MISMATCH,
      BRANCHES_NO_COMMON_ROOT,
      BRANCHES_NO_ROUTE,
    };

    bool empty() const;

    using Heads = std::map<TipsetHash, BranchPtr>;

    const Heads &getAllHeads() const;

    outcome::result<BranchId> getBranchAtHeight(Height h,
                                                bool must_exist) const;

    outcome::result<void> setCurrentHead(BranchId head_branch, Height height);

    outcome::result<BranchCPtr> getCommonRoot(BranchId a, BranchId b) const;

    outcome::result<std::vector<BranchId>> getRoute(BranchId from,
                                                    BranchId to) const;

    struct StorePosition {
      BranchId assigned_branch = kNoBranch;
      BranchId at_bottom_of_branch = kNoBranch;
      BranchId on_top_of_branch = kNoBranch;
      boost::optional<RenameBranch> rename;
    };

    outcome::result<StorePosition> findStorePosition(
        const Tipset &tipset,
        const TipsetHash &parent_hash,
        BranchId parent_branch,
        Height parent_height) const;

    void splitBranch(const TipsetHash &new_top,
                     const TipsetHash &new_bottom,
                     Height new_bottom_height,
                     const RenameBranch &pos);

    struct HeadChanges {
      std::vector<TipsetHash> removed;
      std::vector<TipsetHash> added;
    };

    HeadChanges storeTipset(const TipsetCPtr &tipset,
                            const TipsetHash &parent_hash,
                            const StorePosition &pos);

    outcome::result<BranchCPtr> getBranch(BranchId id) const;

    outcome::result<BranchCPtr> getRootBranch(BranchId id) const;

    void clear();

    outcome::result<HeadChanges> init(
        std::map<BranchId, BranchPtr> all_branches);

    outcome::result<void> storeGenesis(const TipsetCPtr &genesis_tipset);

   private:
    void newBranch(const TipsetHash &hash,
                   Height height,
                   const TipsetHash &parent_hash,
                   const StorePosition &pos);

    void mergeBranches(const BranchPtr &branch,
                       BranchPtr &parent_branch,
                       HeadChanges &changes);

    void updateHeads(BranchPtr &branch, bool synced, HeadChanges &changes);

    BranchPtr getBranch(BranchId id);

    BranchId newBranchId() const;

    std::map<BranchId, BranchPtr> all_branches_;
    std::map<TipsetHash, BranchPtr> heads_;
    std::map<TipsetHash, BranchPtr> unloaded_roots_;
    BranchPtr genesis_branch_;
    std::map<Height, BranchPtr> current_chain_;
    BranchId current_top_branch_ = kNoBranch;
    Height current_height_ = 0;
  };
}  // namespace fc::sync

OUTCOME_HPP_DECLARE_ERROR(fc::sync, Branches::Error);

#endif  // CPP_FILECOIN_SYNC_BRANCHES_HPP
