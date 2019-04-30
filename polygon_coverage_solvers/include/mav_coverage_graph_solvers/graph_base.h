#ifndef MAV_COVERAGE_GRAPH_SOLVERS_GRAPH_BASE_H_
#define MAV_COVERAGE_GRAPH_SOLVERS_GRAPH_BASE_H_

#include <cmath>
#include <limits>
#include <map>
#include <vector>

// Utilities to create graphs.
namespace mav_coverage_planning {

const double kToMilli = 1000;
const double kFromMilli = 1.0 / kToMilli;

// A doubly linked list representing a directed graph.
// idx: node id
// map pair first: neighbor id
// map pair second: cost to go to neighbor
typedef std::vector<std::map<size_t, double>> Graph;

// An edge id.
// first: from node
// second: to node
typedef std::pair<size_t, size_t> EdgeId;

// An edge.
typedef std::pair<EdgeId, double> Edge;

// The solution.
typedef std::vector<size_t> Solution;

// A heuristic.
// first: node
// second: heuristic cost to goal
typedef std::map<size_t, double> Heuristic;

// The base graph class.
template <class NodeProperty, class EdgeProperty>
class GraphBase {
 public:
  // A map from graph node id to node properties.
  using NodeProperties = std::map<size_t, NodeProperty>;
  // A map from graph edge id to edge properties.
  using EdgeProperties = std::map<EdgeId, EdgeProperty>;

  GraphBase()
      : start_idx_(std::numeric_limits<size_t>::max()),
        goal_idx_(std::numeric_limits<size_t>::max()),
        is_created_(false){};

  // Add a node.
  bool addNode(const NodeProperty& node_property);
  // Add a start node, that often follows special construction details.
  virtual bool addStartNode(const NodeProperty& node_property);
  // Add a goal node, that often follows special construction details.
  virtual bool addGoalNode(const NodeProperty& node_property);
  // Clear data structures.
  virtual void clear();
  void clearEdges();
  // Create graph given the internal settings.
  virtual bool create();

  inline size_t size() const { return graph_.size(); }
  inline size_t getNumberOfEdges() const { return edge_properties_.size(); }
  inline void reserve(size_t size) { graph_.reserve(size); }
  inline size_t getStartIdx() const { return start_idx_; }
  inline size_t getGoalIdx() const { return goal_idx_; }
  inline size_t isInitialized() const { return is_created_; }

  bool nodeExists(size_t node_id) const;
  bool nodePropertyExists(size_t node_id) const;
  bool edgeExists(const EdgeId& edge_id) const;
  bool edgePropertyExists(const EdgeId& edge_id) const;

  bool getEdgeCost(const EdgeId& edge_id, double* cost) const;
  const NodeProperty* getNodeProperty(size_t node_id) const;
  const EdgeProperty* getEdgeProperty(const EdgeId& edge_id) const;

  // Solve the graph with Dijkstra using arbitrary start and goal index.
  bool solveDijkstra(size_t start, size_t goal, Solution* solution) const;
  // Solve the graph with Dijkstra using internal start and goal index.
  bool solveDijkstra(Solution* solution) const;
  // Solve the graph with A* using arbitrary start and goal index.
  bool solveAStar(size_t start, size_t goal, Solution* solution) const;
  // Solve the graph with A* using internal start and goal index.
  bool solveAStar(Solution* solution) const;

  // Create the adjacency matrix setting no connectings to INT_MAX and
  // transforming cost into milli int.
  std::vector<std::vector<int>> getAdjacencyMatrix() const;

  // Preserving three decimal digits.
  inline int doubleToMilliInt(double in) const {
    return static_cast<int>(std::round(in * kToMilli));
  }
  inline double milliIntToDouble(int in) const {
    return static_cast<double>(in) * kFromMilli;
  }

 protected:
  // Called from addNode. Creates all edges to the node at the back of the
  // graph.
  virtual bool addEdges();
  // Given the goal, calculate and set the heuristic for all nodes in the graph.
  virtual bool calculateHeuristic(size_t goal, Heuristic* heuristic) const;

  bool addEdge(const EdgeId& edge_id, const EdgeProperty& edge_property,
               double cost);

  Solution reconstructSolution(const std::map<size_t, size_t>& came_from,
                               size_t current) const;

  Graph graph_;
  // Map to store all node properties. Key is the graph node id.
  NodeProperties node_properties_;
  // Map to store all edge properties. Key is the graph edge id.
  EdgeProperties edge_properties_;
  size_t start_idx_;
  size_t goal_idx_;
  bool is_created_;
};
}  // namespace mav_coverage_planning

#include "mav_coverage_graph_solvers/impl/graph_base_impl.h"

#endif  // MAV_COVERAGE_GRAPH_SOLVERS_GRAPH_BASE_H_
