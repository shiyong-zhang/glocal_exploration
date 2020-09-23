#include "glocal_exploration_ros/planning/global/skeleton_a_star.h"

#include <limits>
#include <list>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

namespace glocal_exploration {

bool SkeletonAStar::planPath(const Point& start_point, const Point& goal_point,
                             std::vector<WayPoint>* way_points) {
  map_ = std::dynamic_pointer_cast<VoxgraphMap>(comm_->map());
  if (!map_) {
    LOG(WARNING) << "Could not get pointer to VoxgraphMap from communicator";
    return false;
  }

  // Search the nearest reachable start vertex on the skeleton graphs
  constexpr int kNClosestStartVertices = 5;
  const std::vector<GlobalVertexId> start_vertex_candidates =
      searchNClosestReachableSkeletonVertices(
          start_point, kNClosestStartVertices,
          [this](const Point& start_point, const Point& end_point) {
            return map_->isLineTraversableInActiveSubmap(start_point,
                                                         end_point);
          });
  if (start_vertex_candidates.empty()) {
    LOG(INFO)
        << "Could not find any reachable skeleton vertices near start point ("
        << start_point.x() << ", " << start_point.y() << ", " << start_point.z()
        << ")";
    return false;
  }

  // Search the N closest reachable end vertices on the skeleton graph
  constexpr int kNClosestEndVertices = 30;
  std::vector<GlobalVertexId> end_vertex_candidates =
      searchNClosestReachableSkeletonVertices(
          goal_point, kNClosestEndVertices,
          [this](const Point& start_point, const Point& end_point) {
            return map_->isLineTraversableInGlobalMap(start_point, end_point);
          });
  if (end_vertex_candidates.empty()) {
    LOG(INFO)
        << "Could not find any reachable skeleton vertices near goal point ("
        << goal_point.x() << ", " << goal_point.y() << ", " << goal_point.z()
        << ")";
    return false;
  }

  // Plan path along the skeleton
  std::vector<GlobalVertexId> vertex_path;
  if (!getPathBetweenVertices(start_vertex_candidates, end_vertex_candidates,
                              start_point.cast<voxblox::FloatingPoint>(),
                              goal_point.cast<voxblox::FloatingPoint>(),
                              &vertex_path)) {
    LOG(INFO) << "Could not find global path from start point ("
              << start_point.x() << ", " << start_point.y() << ", "
              << start_point.z() << ") to goal point (" << goal_point.x()
              << ", " << goal_point.y() << ", " << goal_point.z() << ")";
    return false;
  }

  // Convert the path from vertex IDs to waypoints
  convertVertexToWaypointPath(vertex_path, goal_point, way_points);

  return true;
}

bool SkeletonAStar::getPathBetweenVertices(
    const std::vector<GlobalVertexId>& start_vertex_candidates,
    const std::vector<GlobalVertexId>& end_vertex_candidates,
    const voxblox::Point& start_point, const voxblox::Point& goal_point,
    std::vector<GlobalVertexId>* vertex_path) const {
  CHECK_NOTNULL(map_);
  CHECK_NOTNULL(vertex_path);
  CHECK(!start_vertex_candidates.empty());
  CHECK(!end_vertex_candidates.empty());

  std::map<GlobalVertexId, FloatingPoint> g_score_map;
  std::map<GlobalVertexId, FloatingPoint> f_score_map;
  std::map<GlobalVertexId, GlobalVertexId> parent_map;

  std::set<GlobalVertexId> open_set;
  std::set<GlobalVertexId> closed_set;

  // Initialize the search with vertices that can be used as graph entry points
  // i.e. vertices that are closest to the start_point and reachable
  for (const GlobalVertexId& current_vertex_id : start_vertex_candidates) {
    const SkeletonSubmap& current_submap =
        skeleton_submap_collection_.getSubmapById(current_vertex_id.submap_id);
    const voxblox::SparseSkeletonGraph& current_graph =
        current_submap.getSkeletonGraph();
    const voxblox::SkeletonVertex& current_vertex =
        current_graph.getVertex(current_vertex_id.vertex_id);

    const voxblox::Point t_odom_current_vertex =
        current_submap.getPose().cast<voxblox::FloatingPoint>() *
        current_vertex.point;
    g_score_map[current_vertex_id] =
        (t_odom_current_vertex - start_point).norm();
    f_score_map[current_vertex_id] =
        (goal_point - t_odom_current_vertex).norm();
    open_set.insert(current_vertex_id);
  }

  // Indicate which vertices can be used as graph exit points
  // i.e. vertices that are close to the end point and that can reach it
  std::unordered_set<GlobalVertexId, GlobalVertexIdHash>
      end_vertex_candidate_set;
  for (const GlobalVertexId& end_vertex_candidate : end_vertex_candidates) {
    end_vertex_candidate_set.insert(end_vertex_candidate);
  }

  // Run the Astar search
  size_t iteration_counter = 0u;
  voxgraph::SubmapID previous_submap_id = -1;
  const SkeletonSubmap* current_submap = nullptr;
  const voxblox::SparseSkeletonGraph* current_graph = nullptr;
  while (!open_set.empty()) {
    if (kMaxNumAStarIterations <= ++iteration_counter) {
      LOG(WARNING) << "Aborting skeleton planning. Exceeded maximum number of "
                      "iterations ("
                   << iteration_counter << ").";
      return false;
    }

    // Find the smallest f-value in the open set.
    const GlobalVertexId current_vertex_id =
        popSmallestFromOpen(f_score_map, &open_set);

    // Check if we have reached the goal
    if (current_vertex_id == kGoalVertexId) {
      LOG(INFO) << "Found skeleton path to goal in " << iteration_counter
                << " iterations.";
      getSolutionVertexPath(kGoalVertexId, parent_map, vertex_path);
      return true;
    }

    // Get vertex's submap and graph
    if (current_vertex_id.submap_id != previous_submap_id) {
      current_submap = &skeleton_submap_collection_.getSubmapById(
          current_vertex_id.submap_id);
      current_graph = &current_submap->getSkeletonGraph();
    }
    previous_submap_id = current_vertex_id.submap_id;
    closed_set.insert(current_vertex_id);

    // If this vertex is an exit point candidate,
    // hallucinate an edge to the goal
    const voxblox::SkeletonVertex& current_vertex =
        current_graph->getVertex(current_vertex_id.vertex_id);
    if (end_vertex_candidate_set.count(current_vertex_id)) {
      //      LOG(WARNING) << "Found exit point candidate vertex";
      if (open_set.count(kGoalVertexId) == 0) {
        open_set.insert(kGoalVertexId);
      }
      FloatingPoint tentative_g_score =
          g_score_map[current_vertex_id] +
          (goal_point - current_vertex.point).norm();
      if (g_score_map.count(kGoalVertexId) == 0 ||
          g_score_map[kGoalVertexId] < tentative_g_score) {
        g_score_map[kGoalVertexId] = tentative_g_score;
        f_score_map[kGoalVertexId] = tentative_g_score;
        parent_map[kGoalVertexId] = current_vertex_id;
      }
      continue;
    }

    // Unless this vertex already has many neighbors, try to connect to a
    // neighboring skeleton submap
    if (current_vertex.edge_list.size() <= 3) {
      const Point t_odom_current_vertex =
          current_submap->getPose() *
          current_vertex.point.cast<FloatingPoint>();
      for (const voxgraph::SubmapID submap_id :
           map_->getSubmapsAtPosition(t_odom_current_vertex)) {
        // Avoid linking the current vertex against vertices of its own submap
        if (submap_id == current_vertex_id.submap_id) {
          continue;
        }

        SkeletonSubmap::ConstPtr nearby_submap =
            skeleton_submap_collection_.getSubmapConstPtrById(submap_id);
        if (!nearby_submap) {
          continue;
        }

        voxblox::Point t_nearby_submap_current_vertex =
            (nearby_submap->getPose().inverse() * t_odom_current_vertex)
                .cast<voxblox::FloatingPoint>();
        constexpr int kUseNNearestNeighbors = 3;
        constexpr float kMaxLinkingDistance = 2.f;
        std::vector<VertexIdElement> nearest_vertex_ids;
        nearby_submap->getNClosestVertices(t_nearby_submap_current_vertex,
                                           kUseNNearestNeighbors,
                                           &nearest_vertex_ids);

        for (const VertexIdElement& nearby_vertex_id : nearest_vertex_ids) {
          const GlobalVertexId nearby_vertex_global_id{submap_id,
                                                       nearby_vertex_id};
          const Point t_odom_nearby_vertex =
              nearby_submap->getPose() * nearby_submap->getSkeletonGraph()
                                             .getVertex(nearby_vertex_id)
                                             .point.cast<FloatingPoint>();
          const float distance_current_to_nearby_vertex =
              (t_odom_current_vertex - t_odom_nearby_vertex).norm();
          if (distance_current_to_nearby_vertex < kMaxLinkingDistance &&
              map_->isLineTraversableInGlobalMap(t_odom_current_vertex,
                                                 t_odom_nearby_vertex)) {
            if (closed_set.count(nearby_vertex_global_id) > 0) {
              continue;
            }
            if (open_set.count(nearby_vertex_global_id) == 0) {
              open_set.insert(nearby_vertex_global_id);
            }

            FloatingPoint tentative_g_score =
                g_score_map[current_vertex_id] +
                (t_odom_nearby_vertex - t_odom_current_vertex).norm();
            if (g_score_map.count(nearby_vertex_global_id) == 0 ||
                g_score_map[nearby_vertex_global_id] < tentative_g_score) {
              g_score_map[nearby_vertex_global_id] = tentative_g_score;
              f_score_map[nearby_vertex_global_id] =
                  tentative_g_score +
                  (goal_point -
                   t_odom_nearby_vertex.cast<voxblox::FloatingPoint>())
                      .norm();
              parent_map[nearby_vertex_global_id] = current_vertex_id;
            }
          }
        }
      }
    }

    // Evaluate the vertex's neighbors
    for (int64_t edge_id : current_vertex.edge_list) {
      const voxblox::SkeletonEdge& edge = current_graph->getEdge(edge_id);
      GlobalVertexId neighbor_vertex_id = current_vertex_id;
      if (edge.start_vertex == current_vertex_id.vertex_id) {
        neighbor_vertex_id.vertex_id = edge.end_vertex;
      } else {
        neighbor_vertex_id.vertex_id = edge.start_vertex;
      }

      if (closed_set.count(neighbor_vertex_id) > 0) {
        // Already checked this guy as well.
        continue;
      }
      if (open_set.count(neighbor_vertex_id) == 0) {
        open_set.insert(neighbor_vertex_id);
      }

      const voxblox::SkeletonVertex& neighbor_vertex =
          current_graph->getVertex(neighbor_vertex_id.vertex_id);

      FloatingPoint tentative_g_score =
          g_score_map[current_vertex_id] +
          (neighbor_vertex.point - current_vertex.point).norm();
      // NOTE: Since the vertex and its neighbor are already in the same
      //       (submap) frame, we can directly compute their distance above
      if (g_score_map.count(neighbor_vertex_id) == 0 ||
          g_score_map[neighbor_vertex_id] < tentative_g_score) {
        g_score_map[neighbor_vertex_id] = tentative_g_score;
        const voxblox::Point t_odom_neighbor_vertex =
            current_submap->getPose().cast<voxblox::FloatingPoint>() *
            neighbor_vertex.point;
        f_score_map[neighbor_vertex_id] =
            tentative_g_score + (goal_point - t_odom_neighbor_vertex).norm();
        parent_map[neighbor_vertex_id] = current_vertex_id;
      }
    }
  }

  return false;
}

void SkeletonAStar::getSolutionVertexPath(
    GlobalVertexId end_vertex_id,
    const std::map<GlobalVertexId, GlobalVertexId>& parent_map,
    std::vector<GlobalVertexId>* vertex_path) {
  CHECK_NOTNULL(vertex_path);
  vertex_path->clear();
  vertex_path->push_back(end_vertex_id);
  GlobalVertexId vertex_id = end_vertex_id;
  while (parent_map.count(vertex_id) > 0) {
    vertex_id = parent_map.at(vertex_id);
    vertex_path->push_back(vertex_id);
  }
  std::reverse(vertex_path->begin(), vertex_path->end());
}

void SkeletonAStar::convertVertexToWaypointPath(
    const std::vector<GlobalVertexId>& vertex_path, const Point& goal_point,
    std::vector<WayPoint>* way_points) const {
  CHECK_NOTNULL(way_points);
  way_points->clear();
  for (const GlobalVertexId& global_vertex_id : vertex_path) {
    if (global_vertex_id == kGoalVertexId) {
      way_points->emplace_back(WayPoint{goal_point.x(), goal_point.y(),
                                        goal_point.z(), /* yaw */ 0.0});
    } else {
      const SkeletonSubmap& submap =
          skeleton_submap_collection_.getSubmapById(global_vertex_id.submap_id);
      const voxblox::SkeletonVertex& vertex =
          submap.getSkeletonGraph().getVertex(global_vertex_id.vertex_id);
      const Point t_odom_vertex =
          submap.getPose() * vertex.point.cast<FloatingPoint>();
      way_points->emplace_back(WayPoint{t_odom_vertex.x(), t_odom_vertex.y(),
                                        t_odom_vertex.z(), /* yaw */ 0.0});
    }
  }
}

GlobalVertexId SkeletonAStar::popSmallestFromOpen(
    const std::map<GlobalVertexId, FloatingPoint>& f_score_map,
    std::set<GlobalVertexId>* open_set) {
  FloatingPoint min_distance = std::numeric_limits<FloatingPoint>::max();
  auto min_iter = open_set->cbegin();

  for (auto iter = open_set->cbegin(); iter != open_set->cend(); ++iter) {
    FloatingPoint distance = f_score_map.at(*iter);
    if (distance < min_distance) {
      min_distance = distance;
      min_iter = iter;
    }
  }

  GlobalVertexId return_val = *min_iter;
  open_set->erase(min_iter);
  return return_val;
}

std::vector<GlobalVertexId>
SkeletonAStar::searchNClosestReachableSkeletonVertices(
    const Point& point, const int n_closest,
    const std::function<bool(const Point& start_point, const Point& end_point)>&
        traversability_function) const {
  CHECK_NOTNULL(map_);

  struct CandidateVertex {
    GlobalVertexId global_vertex_id;
    Point t_O_point = Point::Zero();
    float distance = -1.f;
  };
  std::list<CandidateVertex> candidate_start_vertices;
  for (const voxgraph::SubmapID submap_id : map_->getSubmapsAtPosition(point)) {
    SkeletonSubmap::ConstPtr skeleton_submap =
        skeleton_submap_collection_.getSubmapConstPtrById(submap_id);
    if (!skeleton_submap) {
      LOG(ERROR) << "Couldn't get pointer to skeleton submap with ID "
                 << submap_id;
      continue;
    }
    for (const auto& vertex_kv :
         skeleton_submap->getSkeletonGraph().getVertexMap()) {
      CandidateVertex candidate_vertex;
      candidate_vertex.global_vertex_id.submap_id = submap_id;
      candidate_vertex.global_vertex_id.vertex_id = vertex_kv.second.vertex_id;
      candidate_vertex.t_O_point = skeleton_submap->getPose() *
                                   vertex_kv.second.point.cast<FloatingPoint>();
      candidate_vertex.distance = (candidate_vertex.t_O_point - point).norm();
      candidate_start_vertices.emplace_back(std::move(candidate_vertex));
    }
  }

  candidate_start_vertices.sort(
      [](const CandidateVertex& lhs, const CandidateVertex& rhs) {
        return lhs.distance < rhs.distance;
      });

  int num_candidates_unreachable = 0;
  std::vector<GlobalVertexId> reachable_skeleton_vertices;
  for (const CandidateVertex& candidate_start_vertex :
       candidate_start_vertices) {
    if (traversability_function(point, candidate_start_vertex.t_O_point)) {
      reachable_skeleton_vertices.emplace_back(
          candidate_start_vertex.global_vertex_id);
      if (n_closest <= reachable_skeleton_vertices.size()) {
        return reachable_skeleton_vertices;
      }
    } else {
      ++num_candidates_unreachable;
    }
  }
}

}  // namespace glocal_exploration
