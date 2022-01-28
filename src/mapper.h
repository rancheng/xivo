// The Mapper module.
// Author: Stephanie Tsuei (stephanietsuei@ucla.edu)
#pragma once

#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "json/json.h"
#include "DBoW2/DBoW2.h"

#include "core.h"
#include "feature.h"
#include "group.h"
#include "graphbase.h"
#include "fastbrief.h"

namespace xivo {

/** Singleton class in charge of loop closures and pose-graph optimization.
 *  It manages relationships for all features and groups formerly in the
 *  filter. It also contains functions to search for loop closures and runs
 *  pose-graph optimization.
 */

using FastBriefVocabulary =
  DBoW2::TemplatedVocabulary<FastBrief::TDescriptor, FastBrief>;

using LCMatch = std::pair<FeaturePtr, FeaturePtr>;


class Mapper : public GraphBase {

public:
  ~Mapper();
  static MapperPtr Create(const Json::Value &cfg);
  static MapperPtr instance();

  void AddFeature(FeaturePtr f, const FeatureAdj& feature_adj);
  void AddGroup(GroupPtr f, const GroupAdj& group_adj);
  void RemoveFeature(const FeaturePtr f);
  void RemoveGroup(const GroupPtr g);

  std::vector<FeaturePtr> GetFeaturesOf(GroupPtr g) const;
  std::vector<GroupPtr> GetGroupsOf(FeaturePtr f) const;

  std::mutex features_mtx;
  std::mutex groups_mtx;

  bool UseLoopClosure() const { return use_loop_closure_; }
  std::vector<LCMatch> DetectLoopClosures(
    const std::vector<FeaturePtr>& instate_features);

private:
  Mapper() = default;
  Mapper(const Json::Value &cfg);
  Mapper(const Mapper &) = delete;
  Mapper &operator=(const Mapper &) = delete;

  static std::unique_ptr<Mapper> instance_;

  // Loop closure variables
  bool use_loop_closure_;
  int uplevel_word_search_;
  double nn_dist_thresh_;
  FastBriefVocabulary* voc_;

  /** Maps DBoW2 words to a set of features that map to the same word in the
   *  vocabulary. */
  std::unordered_map<DBoW2::WordId, std::unordered_set<FeaturePtr>> InvIndex_;

  // Functions related to loop closure
  std::unordered_set<FeaturePtr> GetLoopClosureCandidates(const DBoW2::WordId& word_id);
  void UpdateInverseIndex(const DBoW2::WordId &word_id, FeaturePtr f);
};



}