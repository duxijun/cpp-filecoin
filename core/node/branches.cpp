/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "branches.hpp"

#include "common/logger.hpp"

namespace fc::sync {

  namespace {
    auto log() {
      static common::Logger logger = common::createLogger("branches");
      return logger.get();
    }
  }  // namespace

  bool Branches::empty() const {
    return all_branches_.empty();
  }

  const Branches::Heads &Branches::getAllHeads() const {
    return heads_;
  }

  outcome::result<BranchId> Branches::getBranchAtHeight(Height h,
                                                        bool must_exist) const {
    if (current_chain_.empty()) {
      return Error::BRANCHES_NO_CURRENT_CHAIN;
    }

    if (h > current_height_) {
      if (must_exist) {
        return Error::BRANCHES_BRANCH_NOT_FOUND;
      } else {
        return kNoBranch;
      }
    }

    if (h <= genesis_branch_->top_height) {
      return kGenesisBranch;
    }

    auto it = current_chain_.lower_bound(h);
    if (it == current_chain_.end()) {
      if (must_exist) {
        return Error::BRANCHES_BRANCH_NOT_FOUND;
      } else {
        return kNoBranch;
      }
    }

    return it->second->id;
  }

  outcome::result<BranchCPtr> Branches::getCommonRoot(BranchId a,
                                                      BranchId b) const {
    if (a == kNoBranch || b == kNoBranch) {
      return Error::BRANCHES_NO_COMMON_ROOT;
    }

    OUTCOME_TRY(A, getBranch(a));
    OUTCOME_TRY(B, getBranch(b));

    while (a != b) {
      if (A->bottom_height <= B->bottom_height) {
        b = B->parent;
        if (b == kNoBranch) {
          return Error::BRANCHES_NO_COMMON_ROOT;
        }
        OUTCOME_TRYA(B, getBranch(b));
      } else if (B->bottom_height <= A->bottom_height) {
        a = A->parent;
        if (a == kNoBranch) {
          return Error::BRANCHES_NO_COMMON_ROOT;
        }
        OUTCOME_TRYA(A, getBranch(a));
      }
    }

    assert(A == B);

    return std::move(A);
  }

  outcome::result<std::vector<BranchId>> Branches::getRoute(BranchId from,
                                                            BranchId to) const {
    if (from == kNoBranch || to == kNoBranch) {
      return Error::BRANCHES_NO_ROUTE;
    }

    std::vector<BranchId> route;

    if (from == to) {
      route.push_back(from);
      return route;
    }

    bool route_found = false;
    for (;;) {
      route.push_back(to);
      OUTCOME_TRY(info, getBranch(to));
      to = info->parent;
      if (to == from) {
        route_found = true;
        break;
      }
      if (to == kNoBranch || to == kGenesisBranch) {
        break;
      }
    }

    if (!route_found) {
      return Error::BRANCHES_NO_ROUTE;
    }

    route.push_back(from);
    std::reverse(route.begin(), route.end());
    return route;
  }

  outcome::result<void> Branches::setCurrentHead(BranchId head_branch,
                                                 Height height) {
    if (head_branch == kNoBranch) {
      current_chain_.clear();
      current_top_branch_ = kNoBranch;
      current_height_ = 0;
      return outcome::success();
    }

    if (current_top_branch_ == head_branch) {
      if (current_height_ != height) {
        const auto &info = current_chain_.rbegin()->second;
        if (info->top_height < height || info->bottom_height > height) {
          return Error::BRANCHES_HEIGHT_MISMATCH;
        }
        current_height_ = height;
      }

      return outcome::success();
    }

    auto it = all_branches_.find(head_branch);
    if (it == all_branches_.end()) {
      return Error::BRANCHES_HEAD_NOT_FOUND;
    }

    auto &info = it->second;
    if (!info->synced_to_genesis) {
      return Error::BRANCHES_HEAD_NOT_SYNCED;
    }

    if (info->top_height < height || info->bottom_height > height) {
      return Error::BRANCHES_HEIGHT_MISMATCH;
    }

    current_height_ = height;
    current_chain_.clear();
    current_top_branch_ = head_branch;

    // a guard to catch a cycle if it appears in the graph: db inconsistency
    size_t cycle_guard = all_branches_.size() + 1;
    current_chain_[info->top_height] = info;

    BranchId parent = info->parent;
    while (parent != kNoBranch) {
      if (--cycle_guard == 0) {
        current_chain_.clear();
        return Error::BRANCHES_CYCLE_DETECTED;
      }

      assert(all_branches_.count(parent));

      auto branch = all_branches_[parent];
      parent = branch->parent;
      current_chain_[branch->top_height] = std::move(branch);
    }

    return outcome::success();
  }

  outcome::result<Branches::StorePosition> Branches::findStorePosition(
      const Tipset &tipset,
      const TipsetHash &parent_hash,
      BranchId parent_branch,
      Height parent_height) const {
    StorePosition p;

    auto height = tipset.height();
    const TipsetHash &hash = tipset.key.hash();

    if (height == 0) {
      // inserting genesis
      if (!empty()) {
        return Error::BRANCHES_STORE_ERROR;
      }
      p.assigned_branch = kGenesisBranch;
      return p;
    }

    auto it = unloaded_roots_.find(hash);
    if (it != unloaded_roots_.end()) {
      // the tipset will be linked to the bottom of unloaded subgraph
      p.at_bottom_of_branch = it->second->id;
      p.assigned_branch = p.at_bottom_of_branch;
    }

    assert(parent_height < height);

    bool parent_is_here = (parent_branch != kNoBranch);

    if (parent_is_here) {
      OUTCOME_TRY(info, getBranch(parent_branch));
      if (parent_height > info->top_height
          || parent_height < info->bottom_height) {
        return Error::BRANCHES_HEIGHT_MISMATCH;
      }

      p.on_top_of_branch = parent_branch;

      if (parent_height != info->top_height) {
        p.rename =
            RenameBranch{parent_branch, newBranchId(), parent_height, true};
      } else if (info->forks.empty()) {
        p.assigned_branch = parent_branch;
        if (p.at_bottom_of_branch != kNoBranch) {
          p.rename =
              RenameBranch{p.at_bottom_of_branch, parent_branch, 0, false};
        }
      }
    }

    if (p.assigned_branch == kNoBranch) {
      p.assigned_branch = newBranchId();
    }

    return p;
  }

  void Branches::splitBranch(const TipsetHash &new_top,
                             const TipsetHash &new_bottom,
                             Height new_bottom_height,
                             const RenameBranch &pos) {
    assert(pos.old_id != kNoBranch);
    assert(pos.new_id != kNoBranch);
    assert(pos.new_id != pos.old_id);
    assert(pos.above_height >= 0);
    assert(all_branches_.count(pos.new_id) == 0);

    auto parent = getBranch(pos.old_id);
    assert(parent);
    assert(parent->top_height > pos.above_height);
    assert(parent->bottom_height <= pos.above_height);
    assert(new_bottom_height <= parent->top_height);
    assert(new_bottom_height > pos.above_height);

    auto fork = std::make_shared<BranchInfo>(*parent);

    bool is_head = heads_.count(parent->top) > 0;
    bool in_current_chain = false;
    if (is_head) {
      heads_.erase(parent->top);
    }
    if (!current_chain_.empty() && parent->synced_to_genesis) {
      auto it = current_chain_.find(parent->top_height);
      if (it != current_chain_.end() && it->second == parent) {
        current_chain_.erase(it);
        in_current_chain = true;
      }
    }

    fork->id = pos.new_id;
    fork->bottom = new_bottom;
    fork->parent = parent->id;
    for (auto id : fork->forks) {
      auto b = getBranch(id);
      assert(b);
      b->parent = fork->id;
    }

    all_branches_[fork->id] = fork;

    parent->top = new_top;
    parent->top_height = pos.above_height;
    parent->forks.clear();
    parent->forks.insert(fork->id);

    if (is_head) {
      heads_[fork->top] = fork;
    }
    if (in_current_chain) {
      current_chain_[parent->top_height] = parent;
      current_chain_[fork->top_height] = fork;
    }
  }

  outcome::result<void> Branches::storeGenesis(
      const TipsetCPtr &genesis_tipset) {
    if (!empty()) {
      return Error::BRANCHES_STORE_ERROR;
    }
    StorePosition pos;
    pos.assigned_branch = kGenesisBranch;
    storeTipset(genesis_tipset, TipsetHash{}, pos);
    return outcome::success();
  }

  Branches::HeadChanges Branches::storeTipset(const TipsetCPtr &tipset,
                                              const TipsetHash &parent_hash,
                                              const StorePosition &pos) {
    HeadChanges changes;

    auto height = tipset->height();
    const TipsetHash &hash = tipset->key.hash();

    bool new_standalone_branch = (pos.at_bottom_of_branch == kNoBranch
                                  && pos.on_top_of_branch == kNoBranch);

    if (new_standalone_branch) {
      // branch id must be assigned at the moment

      newBranch(hash, height, parent_hash, pos);
      return {};
    }

    assert(height > 0 && !parent_hash.empty());

    BranchPtr linked_to_bottom;

    if (pos.at_bottom_of_branch != kNoBranch) {
      // link to the bottom
      auto it = unloaded_roots_.find(hash);

      assert(it != unloaded_roots_.end());

      auto &b = it->second;

      assert(b->bottom_height > height);
      assert(b->parent == kNoBranch);
      assert(b->id == pos.at_bottom_of_branch);

      b->bottom_height = height;
      b->bottom = hash;
      b->parent_hash = parent_hash;

      linked_to_bottom = b;

      unloaded_roots_.erase(it);

      if (pos.on_top_of_branch == kNoBranch) {
        unloaded_roots_[parent_hash] = std::move(linked_to_bottom);
        return changes;
      }
    }

    assert(pos.on_top_of_branch != kNoBranch);

    if (pos.assigned_branch == pos.on_top_of_branch) {
      // linking without fork

      auto it = heads_.find(parent_hash);

      assert(it != heads_.end());

      auto parent_branch = std::move(it->second);
      heads_.erase(it);

      assert(parent_branch->top_height < height);
      assert(parent_branch->forks.empty());

      if (!linked_to_bottom) {
        // appending tipset on top of new head

        parent_branch->top_height = height;
        parent_branch->top = parent_hash;

        bool notify_change = parent_branch->synced_to_genesis;

        heads_[hash] = std::move(parent_branch);

        if (notify_change) {
          changes.removed.push_back(parent_hash);
          changes.added.push_back(hash);
        }
      } else {
        // merging branches by renaming

        assert(pos.at_bottom_of_branch != kNoBranch);
        assert(all_branches_[pos.at_bottom_of_branch] == linked_to_bottom);

        mergeBranches(linked_to_bottom, parent_branch, changes);
      }

      return changes;
    }

    // make fork from the non-head branch top

    auto branch = getBranch(pos.on_top_of_branch);

    assert(branch);
    assert(parent_hash == branch->top);
    assert(heads_.count(parent_hash) == 0);
    assert(!branch->forks.empty());

    if (!linked_to_bottom) {
      // create new branch
      newBranch(hash, height, parent_hash, pos);
      linked_to_bottom = getBranch(pos.assigned_branch);
    }

    assert(linked_to_bottom);
    branch->forks.insert(pos.assigned_branch);
    linked_to_bottom->parent = branch->id;
    updateHeads(linked_to_bottom, branch->synced_to_genesis, changes);

    return changes;
  }

  void Branches::newBranch(const TipsetHash &hash,
                           Height height,
                           const TipsetHash &parent_hash,
                           const Branches::StorePosition &pos) {
    assert(pos.assigned_branch != kNoBranch);
    assert(all_branches_.count(pos.assigned_branch) == 0);

    auto ptr = std::make_shared<BranchInfo>();
    ptr->id = pos.assigned_branch;
    ptr->top = hash;
    ptr->top_height = height;
    ptr->bottom = hash;
    ptr->bottom_height = height;
    ptr->parent_hash = parent_hash;

    all_branches_[ptr->id] = ptr;
    heads_[hash] = ptr;

    if (parent_hash.empty()) {
      // here is genesis
      assert(pos.assigned_branch == kGenesisBranch);
      assert(height == 0);

      ptr->synced_to_genesis = true;
      genesis_branch_ = std::move(ptr);
      return;
    }

    assert(height > 0);
    unloaded_roots_[parent_hash] = std::move(ptr);
  }

  void Branches::mergeBranches(const BranchPtr &branch,
                               BranchPtr &parent_branch,
                               HeadChanges &changes) {
    parent_branch->top_height = branch->top_height;
    parent_branch->top = std::move(branch->top);
    parent_branch->forks = std::move(branch->forks);
    all_branches_.erase(branch->id);
    updateHeads(parent_branch, parent_branch->synced_to_genesis, changes);
  }

  void Branches::updateHeads(BranchPtr &branch,
                             bool synced,
                             HeadChanges &changes) {
    branch->synced_to_genesis = synced;
    if (branch->forks.empty()) {
      heads_[branch->top] = branch;
      if (synced) {
        changes.added.push_back(branch->top);
      }
    } else {
      for (auto id : branch->forks) {
        auto fork = getBranch(id);
        assert(fork);
        assert(!fork->synced_to_genesis);
        updateHeads(fork, synced, changes);
      }
    }
  }

  outcome::result<BranchCPtr> Branches::getBranch(BranchId id) const {
    auto it = all_branches_.find(id);
    if (it == all_branches_.end()) {
      return Error::BRANCHES_BRANCH_NOT_FOUND;
    }
    return it->second;
  }

  outcome::result<BranchCPtr> Branches::getRootBranch(BranchId id) const {
    for (;;) {
      OUTCOME_TRY(info, getBranch(id));
      if (info->parent == kNoBranch) {
        return std::move(info);
      }
      id = info->parent;
    }
    return Error::BRANCHES_BRANCH_NOT_FOUND;
  }

  Branches::BranchPtr Branches::getBranch(BranchId id) {
    auto it = all_branches_.find(id);
    if (it == all_branches_.end()) {
      return BranchPtr{};
    }
    return it->second;
  }

  BranchId Branches::newBranchId() const {
    if (!all_branches_.empty()) {
      return all_branches_.crbegin()->first + 1;
    }
    return kGenesisBranch + 1;
  }

  void Branches::clear() {
    all_branches_.clear();
    heads_.clear();
    unloaded_roots_.clear();
    genesis_branch_.reset();
    current_chain_.clear();
    current_top_branch_ = kNoBranch;
    current_height_ = 0;
  }

  outcome::result<Branches::HeadChanges> Branches::init(
      std::map<BranchId, BranchPtr> all_branches) {
    clear();

    auto loadFailed = [this] {
      clear();
      return Error::BRANCHES_LOAD_ERROR;
    };

    HeadChanges heads;

    if (all_branches.empty()) {
      return heads;
    }

    all_branches_ = std::move(all_branches);

    for (auto &[id, ptr] : all_branches_) {
      if (!ptr) {
        log()->error("cannot load graph: invalid branch info, id={}", id);
        return loadFailed();
      }

      auto &b = *ptr;

      if (id != b.id || id == kNoBranch) {
        log()->error("cannot load graph: inconsistent branch id {}", id);
        return loadFailed();
      }

      if (b.top_height < b.bottom_height) {
        log()->error(
            "cannot load graph: heights inconsistent ({} and {}) for id {}",
            b.top_height,
            b.bottom_height,
            b.id);
        return loadFailed();
      }

      if (b.parent != kNoBranch) {
        if (b.parent == b.id) {
          log()->error(
              "cannot load graph: parent and branch id are the same ({})",
              b.id);
          return loadFailed();
        }
        auto it = all_branches_.find(b.parent);
        if (it == all_branches_.end()) {
          log()->error("cannot load graph: parent {} not found for branch {}",
                       b.parent,
                       b.id);
          return loadFailed();
        }

        auto &parent = it->second;

        if (parent->top_height >= b.bottom_height) {
          log()->error(
              "cannot load graph: parent height inconsistent ({} and {}) for "
              "id {} and parent {}",
              b.bottom_height,
              parent->top_height,
              b.id,
              b.parent);
          return loadFailed();
        }

        it->second->forks.insert(b.id);
      } else {
        if (b.id == kGenesisBranch) {
          genesis_branch_ = ptr;
        } else {
          if (b.parent_hash.empty()) {
            log()->error(
                "cannot load graph: expected parent hash for "
                "branch id={}",
                b.id);
            return Error::BRANCHES_PARENT_EXPECTED;
          }
          unloaded_roots_[b.parent_hash] = ptr;
        }
      }
    }

    if (!genesis_branch_) {
      clear();
      return Error::BRANCHES_NO_GENESIS_BRANCH;
    }

    updateHeads(genesis_branch_, true, heads);

    // unsynced heads also needed
    for (const auto &[_, ptr] : all_branches_) {
      if (ptr->forks.empty() && !ptr->synced_to_genesis) {
        heads_[ptr->top] = ptr;
      } else if (ptr->forks.size() == 1) {
        // this is intermediate state between splitBranch and storeTipset,
        // should not be stored

        log()->warn("inconsistent # of forks (1) for branch {}, must be merged",
                    ptr->id);
      }
    }

    return heads;
  }

}  // namespace fc::sync
