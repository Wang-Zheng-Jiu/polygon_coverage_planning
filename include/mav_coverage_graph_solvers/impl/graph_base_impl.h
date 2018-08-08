#ifndef MAV_COVERAGE_PLANNING_GRAPH_GRAPH_BASE_IMPL_H_
#define MAV_COVERAGE_PLANNING_GRAPH_GRAPH_BASE_IMPL_H_

#include <set>

#include <glog/logging.h>
#include <ros/ros.h>

#include "mav_coverage_planning/common.h"

namespace mav_coverage_planning {

const std::string kGraphBasePrefix = kOutputPrefix + "graph_base]: ";

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::addNode(
    const NodeProperty& node_property) {
  graph_.push_back(std::map<size_t, double>());  // Add node.

  // Add node properties.
  const size_t idx = graph_.size() - 1;
  node_properties_.insert(std::make_pair(idx, node_property));
  // Create all adjacent edges.
  if (!addEdges()) {
    graph_.pop_back();
    node_properties_.erase(node_properties_.find(idx));
    return false;
  }
  return true;
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::addStartNode(
    const NodeProperty& node_property) {
  // Add start node like any other node.
  start_idx_ = graph_.size();
  if (addNode(node_property)) {
    return true;
  } else {
    ROS_ERROR_STREAM(kGraphBasePrefix << "Failed adding start node.");
    return false;
  }
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::addGoalNode(
    const NodeProperty& node_property) {
  // Add goal node like any other node.
  goal_idx_ = graph_.size();
  if (addNode(node_property)) {
    return true;
  } else {
    ROS_ERROR_STREAM(kGraphBasePrefix << "Failed adding goal node.");
    return false;
  }
}

template <class NodeProperty, class EdgeProperty>
void GraphBase<NodeProperty, EdgeProperty>::clear() {
  graph_.clear();
  node_properties_.clear();
  edge_properties_.clear();
  start_idx_ = std::numeric_limits<size_t>::max();
  goal_idx_ = std::numeric_limits<size_t>::max();
  is_created_ = false;
}

template <class NodeProperty, class EdgeProperty>
void GraphBase<NodeProperty, EdgeProperty>::clearEdges() {
  edge_properties_.clear();
  for (std::map<size_t, double>& neighbors : graph_) {
    neighbors.clear();
  }
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::create() {
  ROS_ERROR_STREAM(kGraphBasePrefix << "create() not implemented.");
  return false;
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::nodeExists(size_t node_id) const {
  return node_id < graph_.size();
}
template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::nodePropertyExists(
    size_t node_id) const {
  return node_properties_.count(node_id) > 0;
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::edgeExists(
    const EdgeId& edge_id) const {
  return nodeExists(edge_id.first) &&
         graph_[edge_id.first].count(edge_id.second) > 0;
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::edgePropertyExists(
    const EdgeId& edge_id) const {
  return edge_properties_.count(edge_id) > 0;
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::getEdgeCost(const EdgeId& edge_id,
                                                        double* cost) const {
  CHECK_NOTNULL(cost);

  if (edgeExists(edge_id)) {
    *cost = graph_.at(edge_id.first).at(edge_id.second);
    return true;
  } else {
    ROS_ERROR_STREAM(kGraphBasePrefix << "Edge from " << edge_id.first << " to "
                                      << edge_id.second << " does not exist.");
    *cost = -1.0;
    return false;
  }
}

template <class NodeProperty, class EdgeProperty>
const NodeProperty* GraphBase<NodeProperty, EdgeProperty>::getNodeProperty(
    size_t node_id) const {
  if (nodePropertyExists(node_id)) {
    return &(node_properties_.at(node_id));
  } else {
    ROS_ERROR_STREAM(kGraphBasePrefix << "Cannot access node property "
                                      << node_id << ".");
    return nullptr;
  }
}

template <class NodeProperty, class EdgeProperty>
const EdgeProperty*
GraphBase<NodeProperty, EdgeProperty>::GraphBase::getEdgeProperty(
    const EdgeId& edge_id) const {
  if (edgePropertyExists(edge_id)) {
    return &(edge_properties_.at(edge_id));
  } else {
    ROS_ERROR_STREAM(kGraphBasePrefix << "Cannot access edge property from "
                                      << edge_id.first << " to "
                                      << edge_id.second << ".");
    return nullptr;
  }
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::solveDijkstra(
    size_t start, size_t goal, Solution* solution) const {
  CHECK_NOTNULL(solution);
  solution->clear();
  if (!nodeExists(start) || !nodeExists(goal)) {
    return false;
  }

  // https://en.wikipedia.org/wiki/Dijkstra%27s_algorithm
  // Initialization.
  std::set<size_t> open_set = {start};  // Nodes to evaluate.
  std::set<size_t> closed_set;          // Nodes already evaluated.
  std::map<size_t, size_t> came_from;   // Get previous node on optimal path.
  std::map<size_t, double> cost;        // Optimal cost from start.
  for (size_t i = 0; i < graph_.size(); i++) {
    cost[i] = std::numeric_limits<double>::max();
  }
  cost[start] = 0.0;

  while (!open_set.empty()) {
    // Pop vertex with lowest score from open set.
    size_t current = *std::min_element(
        open_set.begin(), open_set.end(),
        [&cost](size_t i, size_t j) { return cost[i] < cost[j]; });
    if (current == goal) {  // Reached goal.
      *solution = reconstructSolution(came_from, current);
      return true;
    }
    open_set.erase(current);
    closed_set.insert(current);

    // Check all neighbors.
    for (const std::pair<size_t, double>& n : graph_[current]) {
      if (closed_set.count(n.first) > 0) {
        continue;  // Ignore already evaluated neighbors.
      }
      open_set.insert(n.first);  // Add to open set if not already in.

      // The distance from start to a neighbor.
      const double tentative_cost = cost[current] + n.second;
      if (tentative_cost >= cost[n.first]) {
        continue;  // This is not a better path to n.
      } else {
        // This path is the best path to n until now.
        came_from[n.first] = current;
        cost[n.first] = tentative_cost;
      }
    }
  }

  return false;
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::solveDijkstra(
    Solution* solution) const {
  return GraphBase::solveDijkstra(start_idx_, goal_idx_, solution);
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::calculateHeuristic(
    size_t goal, Heuristic* heuristic) const {
  ROS_ERROR_STREAM(kGraphBasePrefix << "Heuristic not implemented.");
  return false;
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::solveAStar(
    size_t start, size_t goal, Solution* solution) const {
  CHECK_NOTNULL(solution);
  if (!nodeExists(start) || !nodeExists(goal)) {
    return false;
  }

  Heuristic heuristic;
  if (!calculateHeuristic(goal, &heuristic)) {
    return false;
  }

  // https://en.wikipedia.org/wiki/A*_search_algorithm
  // Initialization.
  std::set<size_t> open_set = {start};  // Nodes to evaluate.
  std::set<size_t> closed_set;          // Nodes already evaluated.
  std::map<size_t, size_t> came_from;   // Get previous node on optimal path.
  std::map<size_t, double> cost;        // Optimal cost from start.
  std::map<size_t, double> cost_with_heuristic;  // cost + heuristic
  for (size_t i = 0; i < graph_.size(); i++) {
    cost[i] = std::numeric_limits<double>::max();
    cost_with_heuristic[i] = std::numeric_limits<double>::max();
  }
  cost[start] = 0.0;
  const Heuristic::const_iterator start_heuristic_it = heuristic.find(start);
  if (start_heuristic_it == heuristic.end()) {
    return false;  // Heuristic not found.
  }
  cost_with_heuristic[start] = start_heuristic_it->second;

  while (!open_set.empty()) {
    // Pop vertex with lowest cost with heuristic from open set.
    size_t current = *std::min_element(
        open_set.begin(), open_set.end(),
        [&cost_with_heuristic](size_t i, size_t j) {
          return cost_with_heuristic[i] < cost_with_heuristic[j];
        });
    if (current == goal) {  // Reached goal.
      *solution = reconstructSolution(came_from, current);
      return true;
    }
    open_set.erase(current);
    closed_set.insert(current);

    // Check all neighbors.
    for (const std::pair<size_t, double>& n : graph_[current]) {
      if (closed_set.count(n.first) > 0) {
        continue;  // Ignore already evaluated neighbors.
      }
      open_set.insert(n.first);  // Add to open set if not already in.

      // The distance from start to a neighbor.
      const double tentative_cost = cost[current] + n.second;
      if (tentative_cost >= cost[n.first]) {
        continue;  // This is not a better path to n.
      } else {
        // This path is the best path to n until now.
        came_from[n.first] = current;
        cost[n.first] = tentative_cost;
        const Heuristic::const_iterator heuristic_it = heuristic.find(n.first);
        if (heuristic_it == heuristic.end()) {
          return false;  // Heuristic not found.
        }
        cost_with_heuristic[n.first] = cost[n.first] + heuristic_it->second;
      }
    }
  }

  return false;
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::solveAStar(
    Solution* solution) const {
  return GraphBase::solveAStar(start_idx_, goal_idx_, solution);
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::addEdges() {
  ROS_ERROR_STREAM(kGraphBasePrefix << "addEdges not implemented.");
  return false;
}

template <class NodeProperty, class EdgeProperty>
bool GraphBase<NodeProperty, EdgeProperty>::addEdge(
    const EdgeId& edge_id, const EdgeProperty& edge_property, double cost) {
  if (cost >= 0.0 && nodeExists(edge_id.first)) {
    graph_[edge_id.first][edge_id.second] = cost;
    edge_properties_.insert(std::make_pair(edge_id, edge_property));
    return true;
  } else {
    return false;
  }
}

template <class NodeProperty, class EdgeProperty>
Solution GraphBase<NodeProperty, EdgeProperty>::reconstructSolution(
    const std::map<size_t, size_t>& came_from, size_t current) const {
  Solution solution = {current};
  while (came_from.find(current) != came_from.end()) {
    current = came_from.at(current);
    solution.push_back(current);
  }
  std::reverse(solution.begin(), solution.end());
  return solution;
}

template <class NodeProperty, class EdgeProperty>
std::vector<std::vector<int>>
GraphBase<NodeProperty, EdgeProperty>::getAdjacencyMatrix() const {
  std::vector<std::vector<int>> m(graph_.size(),
                                  std::vector<int>(graph_.size()));

  for (size_t i = 0; i < m.size(); ++i) {
    for (size_t j = 0; j < m[i].size(); ++j) {
      const EdgeId edge(i, j);
      double cost;
      if (edgeExists(edge) && getEdgeCost(edge, &cost)) {
        m[i][j] = doubleToMilliInt(cost);
      } else {
        m[i][j] = std::numeric_limits<int>::max();
      }
    }
  }

  return m;
}

}  // namespace mav_coverage_planning

#endif  // MAV_COVERAGE_PLANNING_GRAPH_GRAPH_BASE_IMPL_H_
