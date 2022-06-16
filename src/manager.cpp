#include <algorithm>
#include <iomanip>
#include <iostream>
#include <unordered_set>

#include "glog/logging.h"

#include "estimator.h"
#include "feature.h"
#include "geometry.h"
#include "group.h"
#include "tracker.h"
#include "mapper.h"
#include "camera_manager.h"

namespace xivo {

void Estimator::ProcessTracks(const timestamp_t &ts,
                              std::list<FeaturePtr> &tracks) {
  instate_features_.clear();
  oos_features_.clear();
  instate_groups_.clear();

  // retrieve the visibility graph
  Graph& graph{*Graph::instance()};

  // increment lifetime of all features and groups
  for (auto f : graph.GetFeatures()) {
    f->IncrementLifetime();
  }
  for (auto g : graph.GetGroups()) {
    g->IncrementLifetime();
  }

  // which lost at least one feature and might be a floating group
  std::unordered_set<GroupPtr> affected_groups;

  // which lost a guage feature and will need new gauge features this update
  std::vector<GroupPtr> needs_new_gauge_features;

  // process instate but failed-to-be-tracked features
  std::vector<FeaturePtr> new_features;
  ;
  for (auto it = tracks.begin(); it != tracks.end();) {
    auto f = *it;

    // if (use_canvas_) {
    //   Canvas::instance()->Draw(f);
    // }

    if (f->track_status() == TrackStatus::CREATED) {
      // just created, must not included in the graph yet
      new_features.push_back(f);
      it = tracks.erase(it);
    } else if ((f->instate() && f->track_status() == TrackStatus::DROPPED) ||
               f->track_status() == TrackStatus::REJECTED) {
      GroupPtr affected_group = f->ref();
#ifdef USE_MAPPER
      Mapper::instance()->AddFeature(f, graph.GetFeatureAdj(f), gbc());
#endif
      graph.RemoveFeature(f);
      if (f->instate()) {
        LOG(INFO) << "Tracker rejected feature #" << f->id();
        if (f->status() == FeatureStatus::GAUGE) {
          needs_new_gauge_features.push_back(affected_group);
          LOG(INFO) << "Group # " << affected_group->id() << " just lost a gauge feature rejected by tracker.";
        }
        RemoveFeatureFromState(f);
        affected_groups.insert(affected_group);
      }
      Feature::Deactivate(f);
      it = tracks.erase(it);
    } else if (!f->instate() && f->track_status() == TrackStatus::DROPPED) {
      if (use_OOS_) {
        // use OOS features for update
        f->SetStatus(FeatureStatus::DROPPED);
        oos_features_.push_back(f);
      } else {
        // just remove
        graph.RemoveFeature(f);
        Feature::Destroy(f);
      }
      it = tracks.erase(it);
    } else {
#ifndef NDEBUG
      CHECK(f->track_status() == TrackStatus::TRACKED);
#endif
      if (f->instate()) {
        // instate feature being tracked -- use in measurement update later on
        ++it;
      } else {
#ifndef NDEBUG
        CHECK(!f->instate());
#endif

        // perform triangulation before
        if (triangulate_pre_subfilter_ && f->size() == 2) {
          // got a second view, triangulate!
          f->Triangulate(gsb(), gbc(), triangulate_options_);
        }

        // out-of-state feature, run depth subfilter to improve depth ...
        f->SubfilterUpdate(gsb(), gbc(), subfilter_options_);
        // std::cout << "outlier score=" << f->outlier_counter() << std::endl;

        if (f->outlier_counter() > remove_outlier_counter_) {
          graph.RemoveFeature(f);
          Feature::Destroy(f);
          it = tracks.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  // remaining in tracks: just created (not in graph yet) and being tracked well
  // (may or may not be in graph, for those in graph, may or may not in state)
  instate_features_ = graph.GetInstateFeatures();

  if (instate_features_.size() < kMaxFeature) {
    int free_slots = std::count(gsel_.begin(), gsel_.end(), false);

    // choose the instate-candidate criterion
    auto criterion =
      vision_counter_ < strict_criteria_timesteps_ ? Criteria::Candidate
                                                   : Criteria::CandidateStrict;
    auto candidates = graph.GetFeaturesIf(criterion);

    MakePtrVectorUnique(candidates);
    std::sort(candidates.begin(), candidates.end(),
        Criteria::CandidateComparison);

    std::vector<FeaturePtr> bad_features;

    for (auto it = candidates.begin();
         it != candidates.end() && instate_features_.size() < kMaxFeature;
         ++it) {

      auto f = *it;

      if (use_depth_opt_) {
        auto obs = graph.GetObservationsOf(f);
        if (obs.size() > 1) {
          if (!f->RefineDepth(gbc(), obs, refinement_options_)) {
            bad_features.push_back(f);
            continue;
          }
        }
        else if (obs.size() == 0) {
          LOG(ERROR) << "A feature with no observations should not be a candidate";
        }
      }

      if (!f->ref()->instate() && free_slots <= 0) {
        // If we turn this feature to instate, its reference group should
        // also be instate, which out-number the available group slots ...
        continue;
      }

      instate_features_.push_back(f);
      AddFeatureToState(f); // insert f to state vector and covariance
      if (!f->ref()->instate()) {
#ifndef NDEBUG
        CHECK(graph.HasGroup(f->ref()));
        CHECK(graph.GetGroupAdj(f->ref()).count(f->id()));
        CHECK(graph.GetFeatureAdj(f).count(f->ref()->id()));
#endif
        // need to add reference group to state if it's not yet instate
        AddGroupToState(f->ref());
        needs_new_gauge_features.push_back(f->ref());
        // use up one more free slot
        --free_slots;
      }
    }
    DestroyFeatures(bad_features);
  }

  // Perform depth refinement before using oos features
  if (!oos_features_.empty() && use_depth_opt_) {
    std::vector<FeaturePtr> bad_features;
    for (auto it = oos_features_.begin(); it != oos_features_.end();) {
      auto f = *it;
      auto obs = graph.GetObservationsOf(f);
      if (obs.size() > 1 && f->RefineDepth(gbc(), obs, refinement_options_)) {
        ++it;
      } else {
        // remove those failed to be optimized in depth refinement
        bad_features.push_back(f);
        it = oos_features_.erase(it);
      }
    }
    DestroyFeatures(bad_features);
  }

  // Drop groups that now have fewer features than can be fixed because features
  // were dropped by the tracker.
  // Then, look for new groups to own them.
  std::vector<GroupPtr> pre_update_discards;
  for (auto g: affected_groups) {
    const auto &adj_f = graph.GetFeaturesOf(g);
    int num_features_instate =
      std::count_if(adj_f.begin(), adj_f.end(), [g](FeaturePtr f) {
        return f->ref() == g && f->instate();
      });
    if ((num_features_instate < cfg_.get("num_gauge_xy_features", 3).asInt()) ||
        (num_features_instate == 0)) {
      pre_update_discards.push_back(g);
    }
  }
  std::vector<FeaturePtr> pre_update_nullrefs =
    FindNewOwnersForFeaturesOf(pre_update_discards);
  DiscardFeatures(pre_update_nullrefs);
  for (auto g: pre_update_discards) {
    affected_groups.erase(g);
  }
  DiscardGroups(pre_update_discards);
  for (auto nf: pre_update_nullrefs) {
    LOG(INFO) << "Removed pre-update nullref feature " << nf->id();
  }

  // Once we have enough instate features, perform state update
  if (!instate_features_.empty() || !oos_features_.empty()) {
    MakePtrVectorUnique(oos_features_);
    MakePtrVectorUnique(instate_features_);

    instate_groups_ =
        graph.GetGroupsIf([](GroupPtr g) { return g->instate(); });
    Update(needs_new_gauge_features);

    MeasurementUpdateInitialized_ = true;
  }

  // remove oos features
  for (auto f : oos_features_) {
#ifndef NDEBUG
    CHECK(!f->instate());
#endif
    graph.RemoveFeature(f);
    Feature::Destroy(f);
  }

  // Post-update feature management
  // For instate features rejected by the filter,
  // 1) remove the fetaure from features_ and free state & covariance
  // 2) detach the feature from the reference group
  // 3) remove the group if it lost all the instate features

  auto rejected_features = graph.GetFeaturesIf([](FeaturePtr f) -> bool {
    return f->status() == FeatureStatus::REJECTED_BY_FILTER;
  });
  // std::cout << "#rejected=" << rejected_features.size() << std::endl;
  if (use_canvas_) {
    for (auto f : rejected_features) {
      Canvas::instance()->Draw(f);
    }
  }
  LOG(INFO) << "Removed " << rejected_features.size() << " rejected features";
  for (auto f : rejected_features) {
#ifndef NDEBUG
    CHECK(f->ref() != nullptr);
#endif
    affected_groups.insert(f->ref());
  }
  graph.RemoveFeatures(rejected_features);
  for (auto f : rejected_features) {
    RemoveFeatureFromState(f);
    Feature::Destroy(f);
  }

  // different strategies to discard groups w & w/o OOS update
  std::vector<GroupPtr> discards;
  if (!use_OOS_) {
    // If OOS update is NOT used, we need to
    // remove floating groups (with no instate features) and
    // floating features (not instate and reference group is floating)
    for (auto g : affected_groups) {
      const auto &adj_f = graph.GetFeaturesOf(g);
      int num_features_instate =
        std::count_if(adj_f.begin(), adj_f.end(), [g](FeaturePtr f) {
          return f->ref() == g && f->instate();
        });
      if ((num_features_instate < cfg_.get("num_gauge_xy_features", 3).asInt()) ||
          (num_features_instate == 0)) {
        discards.push_back(g);
      }
    }

  } else {
    // if not enough slots, remove old instate groups and recycle some spaces
    std::vector<GroupPtr> groups =
        graph.GetGroupsIf([](GroupPtr g) { return g->instate(); });
    if (groups.size() == kMaxGroup) {
      int oos_discard_step = cfg_.get("oos_discard_step", 3).asInt();
      // sort such that oldest groups are at the front of the vector
      std::sort(groups.begin(), groups.end(),
                [](GroupPtr g1, GroupPtr g2) { return g1->id() < g2->id(); });
      // NOTE: start with 1 is NOT a mistake, since the oldest (index 0) one
      // should be kept
      // (0), 1, 2, (3), 4, 5 ...
      // bracketed indices are those to keep
      for (int i = 1; i < groups.size(); ++i) {
        if (i % oos_discard_step != 0) {
          // if the group does not have instate features referring back to
          // itself,
          auto g = groups[i];
          auto adj_f = graph.GetFeaturesOf(g);
          if (std::none_of(adj_f.begin(), adj_f.end(), [g](FeaturePtr f) {
                return f->instate() && f->ref() == g;
              })) {
            discards.push_back(g);
          }
        }
      }
    } // instate group size == kMaxGroup
  }   // use_OOS

  // for the to-be-discarded groups, transfer ownership of features owned by
  // them. `nullref_features` contains references to features that couldn't be
  // assigned to a new group without errors.
  std::vector<FeaturePtr> nullref_features = FindNewOwnersForFeaturesOf(discards);
  DiscardFeatures(nullref_features);
  DiscardGroups(discards);
  for (auto nf: nullref_features) {
    LOG(INFO) << "Removed nullref feature " << nf->id();
  }

  // initialize those newly detected featuers
  // create a new group and associate newly detected features to the new group
  GroupPtr g = Group::Create(X_.Rsb, X_.Tsb);
  graph.AddGroup(g);
  if (use_OOS_) {
    // In OOS mode, always try to add groups to state.
    AddGroupToState(g);
  }

  tracks.clear(); // clear to prepare for re-assemble the feature list
  for (auto f : new_features) {
    // distinguish two cases:
    // 1) feature is truely just created
    // 2) feature just lost its reference
#ifndef NDEBUG
    CHECK(f->track_status() == TrackStatus::CREATED &&
          f->status() == FeatureStatus::CREATED);
    CHECK(f->ref() == nullptr);
#endif
    f->SetRef(g);
    if (triangulate_pre_subfilter_ && !f->TriangulationSuccessful()) {
      f->Initialize(init_z_, {init_std_x_badtri_, init_std_y_badtri_, init_std_z_badtri_});
    } else {
      f->Initialize(init_z_, {init_std_x_, init_std_y_, init_std_z_});
    }
    //std::cout << "feature id: " << f->id() << ", Xc" << f->Xc().transpose() << std::endl;

    graph.AddFeature(f);
    graph.AddFeatureToGroup(f, g);
    graph.AddGroupToFeature(g, f);

    // put back the detected feature
    tracks.push_back(f);
  }

  auto tracked_features = graph.GetFeaturesIf([](FeaturePtr f) -> bool {
    return f->track_status() == TrackStatus::TRACKED;
  });
  for (auto f : tracked_features) {
#ifndef NDEBUG
    CHECK(f->ref() != nullptr);
#endif

    // attach the new group to all the features being tracked
    graph.AddFeatureToGroup(f, g);
    graph.AddGroupToFeature(g, f);

    // put back the tracked feature
    tracks.push_back(f);
  }

  // adapt initial depth to average depth of features currently visible
  auto depth_features = graph.GetFeaturesIf([this](FeaturePtr f) -> bool {
    return f->instate() ||
           (f->status() == FeatureStatus::READY &&
            f->lifetime() > adaptive_initial_depth_options_.min_feature_lifetime);
  });
  if (!depth_features.empty()) {
    std::vector<number_t> depth(depth_features.size());
    std::transform(depth_features.begin(), depth_features.end(), depth.begin(),
                   [](FeaturePtr f) { return f->z(); });
    number_t median_depth = depth[depth.size() >> 1];

    if (median_depth < min_z_ || median_depth > max_z_) {
      VLOG(0) << "Median depth out of bounds: " << median_depth;
      VLOG(0) << "Reuse the old one: " << init_z_;
    } else {
      number_t beta = adaptive_initial_depth_options_.median_weight;
      init_z_ = (1.0-beta) * init_z_ + beta * median_depth;
      VLOG(0) << "Update aptive initial depth: " << init_z_;
    }
  }

  if (!use_OOS_) {
    // remove non-reference groups
    auto all_groups = graph.GetGroups();
    int max_group_lifetime = cfg_.get("max_group_lifetime", 1).asInt();
    for (auto g : all_groups) {
      if (g->lifetime() > max_group_lifetime) {
        const auto &adj = graph.GetGroupAdj(g);
        if (std::none_of(adj.begin(), adj.end(), [&graph, g](int fid) {
              return graph.GetFeature(fid)->ref() == g;
            })) {
          // for groups which have no reference features, they cannot be instate
          // anyway
#ifndef NDEBUG
          CHECK(!g->instate());
#endif

#ifdef USE_MAPPER
          Mapper::instance()->AddGroup(g, graph.GetGroupAdj(g));
#endif
          graph.RemoveGroup(g);
          Group::Deactivate(g);
        }
      }
    }
  }

  // std::cout << "#groups=" << graph.GetGroups().size() << std::endl;
  // check & clean graph
  // graph.SanityCheck();
  // // remove isolated groups
  // auto empty_groups = graph.GetGroupsIf([this](GroupPtr g)->bool {
  //     return graph.GetGroupAdj(g).empty(); });
  // LOG(INFO) << "#empty groups=" << empty_groups.size();
  // graph.RemoveGroups(empty_groups);
  // for (auto g : empty_groups) {
  //   CHECK(!g->instate());
  //   Group::Delete(g);
  // }

  if (use_canvas_) {
    for (auto f : tracks) 
      Canvas::instance()->Draw(f);

    Canvas::instance()->OverlayStateInfo(X_, imu_.State(),
      CameraManager::instance()->GetIntrinsics());
  }

  static int print_counter{0};
  if (print_timing_ && ++print_counter % 50 == 0) {
    std::cout << print_counter << std::endl;
    std::cout << timer_;
  }

  // Save the frame (only if set to true in json file)
  Canvas::instance()->SaveFrame();
}

} // namespace xivo
