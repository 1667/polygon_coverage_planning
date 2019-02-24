#ifndef MAV_2D_COVERAGE_PLANNING_PLANNERS_POLYGON_STRIPMAP_PLANNER_H_
#define MAV_2D_COVERAGE_PLANNING_PLANNERS_POLYGON_STRIPMAP_PLANNER_H_

#include <memory>

#include <mav_coverage_planning_comm/cgal_definitions.h>
#include "mav_2d_coverage_planning/cost_functions/path_cost_functions.h"
#include "mav_2d_coverage_planning/geometry/polygon.h"
#include "mav_2d_coverage_planning/graphs/sweep_plan_graph.h"
#include "mav_2d_coverage_planning/sensor_models/sensor_model_base.h"

namespace mav_coverage_planning {

class PolygonStripmapPlanner {
 public:
  struct Settings {
    Polygon polygon;
    PathCostFunctionType path_cost_function;
    std::shared_ptr<SensorModelBase> sensor_model;
    bool sweep_around_obstacles;
    DecompositionType decomposition_type;
  };

  // Create a sweep plan for a 2D polygon with holes.
  PolygonStripmapPlanner(const Settings& settings);

  // Precompute solver essentials. To be run before solving.
  bool setup();

  // Solve the resulting generalized traveling salesman problem.
  // start: the start point.
  // goal: the goal point.
  // solution: the solution waypoints.
  bool solve(const Point_2& start, const Point_2& goal,
             std::vector<Point_2>* solution) const;

  inline bool isInitialized() const { return is_initialized_; }

  inline std::vector<Polygon> getDecomposition() const {
    return decomposition_;
  }

  // Check which decomposition cells are adjacent. Return whether each cell has
  // at least one neighbor.
  // TODO(rikba): Convex decomposition adjacency is not fully captured because
  // the polygon with holes they have been created from has been slightly
  // altered.
  bool updateDecompositionAdjacency();

  bool offsetRectangularDecomposition();

 protected:
  virtual bool setupSolver() { return true; };
  // Default: Heuristic GTSPP solver.
  virtual bool runSolver(const Point_2& start, const Point_2& goal,
                         std::vector<Point_2>* solution) const;

  virtual bool sweepAroundObstacles(std::vector<Point_2>* solution) const;

  std::vector<Polygon> decomposition_;
  std::map<size_t, std::set<size_t>> decomposition_adjacency_;
  // The sweep plan graph with all possible waypoints its node connections.
  sweep_plan_graph::SweepPlanGraph sweep_plan_graph_;

 private:
  // Valid construction.
  bool is_initialized_;
  // User problem settings.
  Settings settings_;
};

}  // namespace mav_coverage_planning
#endif  // MAV_2D_COVERAGE_PLANNING_PLANNERS_POLYGON_STRIPMAP_PLANNER_H_
