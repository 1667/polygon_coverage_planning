#ifndef MAV_2D_COVERAGE_PLANNING_PLANNERS_POLYGON_STRIPMAP_PLANNER_H_
#define MAV_2D_COVERAGE_PLANNING_PLANNERS_POLYGON_STRIPMAP_PLANNER_H_

#include "mav_2d_coverage_planning/definitions.h"
#include "mav_2d_coverage_planning/graphs/sweep_plan_graph.h"
#include "mav_2d_coverage_planning/polygon.h"

namespace mav_coverage_planning {

class PolygonStripmapPlanner {
 public:
  struct Settings {
    Polygon polygon;
    PathCostFunctionType path_cost_function;
    SegmentCostFunctionType segment_cost_function;
    double robot_size;
    double min_view_overlap;
    double wall_dist;
    bool sweep_around_obstacles;

    bool check() const;
  };

  // Create a sweep plan for a 2D polygon with holes.
  //
  // polygon: the polygon to cover.
  // cost_function: the cost function type.
  // altitude: constant MAV altitude in [m].
  // longitudinal_fov: lateral field of view of the sensor in [rad].
  // lateral_fov: lateral field of view of the sensor in [rad].
  // min_view_overlap: the minimum sensor footprint overlap of two sweep rows
  // [0..1).
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

 protected:
  virtual bool setupSolver() { return true; };
  // Default: Heuristic GTSPP solver.
  virtual bool runSolver(const Point_2& start, const Point_2& goal,
                         std::vector<Point_2>* solution) const;
  
  virtual bool sweepAroundObstacles(std::vector<Point_2>* solution) const;
  
  virtual void createCornerSweeps(const std::vector<Point_2>& hull, 
          std::vector<Point_2>* solution) const;

  std::vector<Polygon> convex_decomposition_;
  // The sweep plan graph with all possible waypoints its node connections.
  sweep_plan_graph::SweepPlanGraph sweep_plan_graph_;

 private:
  // Check for valid user input.
  bool checkUserInput() const;

  double computeSweepDistance() const;
  

  // Valid construction.
  bool is_initialized_;
  // User problem settings.
  Settings settings_;
  
  Polygon polygon_orig_;
};

}  // namespace mav_coverage_planning
#endif  // MAV_2D_COVERAGE_PLANNING_PLANNERS_POLYGON_STRIPMAP_PLANNER_H_
