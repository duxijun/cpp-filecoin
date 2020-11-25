/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "peers.hpp"

#include "events.hpp"

namespace fc::sync {
  void Peers::start(std::vector<std::string> protocols_required,
                    events::Events &events,
                    RatingChangeFunction rating_fn,
                    RatingChangeLatencyFunction rating_latency_fn) {
    protocols_ = std::move(protocols_required);

    if (rating_fn) {
      rating_fn_ = std::move(rating_fn);
    } else {
      rating_fn_ = [](Rating current_rating, Rating delta) {
        return current_rating + delta;
      };
    }

    if (rating_latency_fn) {
      rating_latency_fn_ = std::move(rating_latency_fn);
    } else {
      rating_latency_fn_ = [](Rating current_rating, uint64_t latency) {
        // TODO(artem) : this is example default fn of latency to rating
        // influence, experiments required to calibrate this

        static constexpr uint64_t ten_seconds = 10000000;
        if (latency >= ten_seconds) {
          // slow peers are not distinguished between
          return current_rating - 100;
        }
        return current_rating
               + Rating((ten_seconds - latency) / (ten_seconds / 100));
      };
    }

    peer_connected_event_ =
        events.subscribePeerConnected([this](const events::PeerConnected &e) {
          if (!protocols_.empty()) {
            // TODO(artem): consider moving this operation to event emitter
            // and dont repeat it everytime

            std::string all;
            all.reserve(2048);
            for (const auto &p : e.protocols) {
              all.append(p);
              all += ":";
            }

            for (const auto &p : protocols_) {
              if (all.find(p) == std::string::npos) {
                // required protocol is not supported by peer
                return;
              }
            }
          }
          peers_[e.peer_id] = 0;
          ratings_.insert({0, e.peer_id});
        });

    peer_disconnected_event_ = events.subscribePeerDisconnected(
        [this](const events::PeerDisconnected &event) {
          auto it = peers_.find(event.peer_id);
          if (it != peers_.end()) {
            removeFromRatings(it);
            peers_.erase(it);
          }
        });

    peer_latency_event_ =
        events.subscribePeerLatency([this](const events::PeerLatency &e) {
          auto it = peers_.find(e.peer_id);
          if (it != peers_.end()) {
            changeRating(it, rating_latency_fn_(it->second, e.latency_usec));
          }
        });
  }

  bool Peers::isConnected(const PeerId &peer) const {
    return peers_.count(peer) != 0;
  }

  const Peers::PeersAndRatings & Peers::getAllPeers() const {
    return peers_;
  }

  boost::optional<PeerId> Peers::selectBestPeer() const {
    // TODO(artem): select random peer weighted by rating

    if (ratings_.empty()) {
      return boost::none;
    }
    return ratings_.begin()->second;
  }

  void Peers::changeRating(const PeerId &peer, Rating delta) {
    if (delta != 0) {
      auto it = peers_.find(peer);
      if (it != peers_.end()) {
        changeRating(it, rating_fn_(it->second, delta));
      }
    }
  }

  void Peers::removeFromRatings(const PeersAndRatings::iterator &it) {
    auto [b, e] = ratings_.equal_range(it->second);
    for (auto i2 = b; i2 != e; ++i2) {
      if (i2->second == it->first) {
        ratings_.erase(i2);
        break;
      }
    }
  }

  void Peers::changeRating(PeersAndRatings::iterator &it, Rating new_rating) {
    if (new_rating != it->second) {
      removeFromRatings(it);
      ratings_.insert( {new_rating, it->first} );
      it->second = new_rating;
    }
  }

}  // namespace fc::sync
