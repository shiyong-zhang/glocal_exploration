#ifndef GLOCAL_EXPLORATION_ROS_PLANNING_GLOBAL_SKELETON_A_STAR_H_
#define GLOCAL_EXPLORATION_ROS_PLANNING_GLOBAL_SKELETON_A_STAR_H_

#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <glocal_exploration/state/communicator.h>
#include <glocal_exploration/state/waypoint.h>
#include <glocal_exploration_ros/planning/global/skeleton_submap_collection.h>

#include "glocal_exploration_ros/mapping/voxgraph_map.h"
#include "glocal_exploration_ros/planning/global/global_vertex_id.h"

namespace glocal_exploration {
class SkeletonAStar {
 public:
  explicit SkeletonAStar(std::shared_ptr<Communicator> communicator)
      : comm_(std::move(communicator)) {}

  bool planPath(const Point& start_point, const Point& goal_point,
                std::vector<WayPoint>* way_points);

  void addSubmap(voxgraph::VoxgraphSubmap::ConstPtr submap_ptr,
                 const float traversability_radius) {
    skeleton_submap_collection_.addSubmap(std::move(submap_ptr),
                                          traversability_radius);
  }
  const SkeletonSubmapCollection& getSkeletonSubmapCollection() {
    return skeleton_submap_collection_;
  }

 protected:
  std::shared_ptr<Communicator> comm_;
  std::shared_ptr<VoxgraphMap> map_;
  SkeletonSubmapCollection skeleton_submap_collection_;

  const GlobalVertexId kGoalVertexId{-1u, -1u};

  static constexpr size_t kMaxNumAStarIterations = 5e3;

  bool getPathBetweenVertices(
      const std::vector<GlobalVertexId>& start_vertex,
      const std::vector<GlobalVertexId>& end_vertex_candidates,
      const voxblox::Point& start_point, const voxblox::Point& goal_point,
      std::vector<GlobalVertexId>* vertex_path) const;
  static void getSolutionVertexPath(
      GlobalVertexId end_vertex_id,
      const std::map<GlobalVertexId, GlobalVertexId>& parent_map,
      std::vector<GlobalVertexId>* vertex_path);
  void convertVertexToWaypointPath(
      const std::vector<GlobalVertexId>& vertex_path, const Point& goal_point,
      std::vector<WayPoint>* way_points) const;

  static GlobalVertexId popSmallestFromOpen(
      const std::map<GlobalVertexId, FloatingPoint>& f_score_map,
      std::set<GlobalVertexId>* open_set);

  std::vector<GlobalVertexId> searchNClosestReachableSkeletonVertices(
      const Point& point, const int n_closest,
      const std::function<bool(const Point& start_point,
                               const Point& end_point)>&
          traversability_function) const;
};
}  // namespace glocal_exploration

#endif  // GLOCAL_EXPLORATION_ROS_PLANNING_GLOBAL_SKELETON_A_STAR_H_