#include "mav_2d_coverage_planning/geometry/polygon.h"

#include <algorithm>
#include <limits>

#include <CGAL/Boolean_set_operations_2.h>
#include <CGAL/Cartesian_converter.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Partition_is_valid_traits_2.h>
#include <CGAL/Partition_traits_2.h>
#include <CGAL/Polygon_2_algorithms.h>
#include <CGAL/Polygon_convex_decomposition_2.h>
#include <CGAL/Polygon_triangulation_decomposition_2.h>
#include <CGAL/Triangle_2.h>
#include <CGAL/connect_holes.h>
#include <CGAL/is_y_monotone_2.h>
#include <CGAL/partition_2.h>
#include <glog/logging.h>
#include <mav_coverage_planning_comm/eigen_conversions.h>
#include <boost/make_shared.hpp>

#include "mav_2d_coverage_planning/geometry/bcd_exact.h"
#include "mav_2d_coverage_planning/geometry/is_approx_y_monotone_2.h"
#include "mav_2d_coverage_planning/geometry/polygon_triangulation.h"

namespace mav_coverage_planning {

Polygon::Polygon() {}

Polygon::Polygon(VertexConstIterator v_begin, VertexConstIterator v_end,
                 const PlaneTransformation<K>& plane_tf)
    : Polygon(Polygon_2(v_begin, v_end), plane_tf) {}

Polygon::Polygon(const Polygon_2& polygon,
                 const PlaneTransformation<K>& plane_tf)
    : Polygon(PolygonWithHoles(polygon), plane_tf) {}

Polygon::Polygon(const PolygonWithHoles& polygon,
                 const PlaneTransformation<K>& plane_tf)
    : polygon_(polygon),
      is_strictly_simple_(checkStrictlySimple()),
      is_convex_(checkConvexity()) {
  plane_tf_ = plane_tf;
  sortCC();
  simplify();
}

std::vector<Point_2> Polygon::getHullVertices() const {
  std::vector<Point_2> vec(polygon_.outer_boundary().size());
  std::vector<Point_2>::iterator vecit = vec.begin();
  for (VertexConstIterator vit = polygon_.outer_boundary().vertices_begin();
       vit != polygon_.outer_boundary().vertices_end(); ++vit, ++vecit)
    *vecit = *vit;
  return vec;
}

std::vector<std::vector<Point_2>> Polygon::getHoleVertices() const {
  std::vector<std::vector<Point_2>> hole_vertices(polygon_.number_of_holes());
  std::vector<std::vector<Point_2>>::iterator hvit = hole_vertices.begin();
  for (PolygonWithHoles::Hole_const_iterator hi = polygon_.holes_begin();
       hi != polygon_.holes_end(); ++hi, ++hvit) {
    hvit->resize(hi->size());
    std::vector<Point_2>::iterator it = hvit->begin();
    for (VertexConstIterator vit = hi->vertices_begin();
         vit != hi->vertices_end(); ++vit, ++it)
      *it = *vit;
  }
  return hole_vertices;
}

bool Polygon::computeTrapezoidalDecompositionFromPolygonWithHoles(
    std::vector<Polygon>* trap_polygons) const {
  CHECK_NOTNULL(trap_polygons);
  trap_polygons->clear();

  std::vector<Polygon_2> traps;
  Polygon_vertical_decomposition_2 decom;
  decom(polygon_, std::back_inserter(traps));

  for (const Polygon_2& p : traps) trap_polygons->emplace_back(p);

  return true;
}

std::vector<Direction_2> Polygon::findEdgeDirections() const {
  // Get all possible polygon directions.
  std::vector<Direction_2> directions;
  for (size_t i = 0; i < polygon_.outer_boundary().size(); ++i) {
    directions.push_back(polygon_.outer_boundary().edge(i).direction());
  }
  for (PolygonWithHoles::Hole_const_iterator hit = polygon_.holes_begin();
       hit != polygon_.holes_end(); ++hit) {
    for (size_t i = 0; i < hit->size(); i++) {
      directions.push_back(hit->edge(i).direction());
    }
  }

  // Remove redundant directions.
  std::set<size_t> to_remove;
  for (size_t i = 0; i < directions.size() - 1; ++i) {
    for (size_t j = i + 1; j < directions.size(); ++j) {
      if (CGAL::orientation(directions[i].vector(), directions[j].vector()) ==
          CGAL::COLLINEAR)
        to_remove.insert(j);
    }
  }
  for (std::set<size_t>::reverse_iterator rit = to_remove.rbegin();
       rit != to_remove.rend(); ++rit) {
    directions.erase(std::next(directions.begin(), *rit));
  }

  // Add opposite directions.
  std::vector<Direction_2> temp_directions = directions;
  for (size_t i = 0; i < temp_directions.size(); ++i) {
    directions.push_back(-temp_directions[i]);
  }

  return directions;
}

std::vector<Direction_2> Polygon::findPerpEdgeDirections() const {
  std::vector<Direction_2> directions = findEdgeDirections();
  for (Direction_2& d : directions) {
    d = Direction_2(-d.dy(), d.dx());
  }

  return directions;
}

std::vector<Polygon> Polygon::rotatePolygon(
    const std::vector<Direction_2>& dirs) const {
  std::vector<Polygon> rotated_polys(dirs.size());
  for (size_t i = 0; i < dirs.size(); ++i) {
    CGAL::Aff_transformation_2<K> rotation(CGAL::ROTATION, dirs[i], 1, 1e3);
    rotation = rotation.inverse();
    PolygonWithHoles rot_pwh = polygon_;
    rot_pwh.outer_boundary() =
        CGAL::transform(rotation, polygon_.outer_boundary());
    PolygonWithHoles::Hole_iterator hit_rot = rot_pwh.holes_begin();
    for (PolygonWithHoles::Hole_const_iterator hit = polygon_.holes_begin();
         hit != polygon_.holes_end(); ++hit) {
      *(hit_rot++) = CGAL::transform(rotation, *hit);
    }

    rotated_polys[i] = Polygon(rot_pwh, plane_tf_);
  }

  return rotated_polys;
}

double Polygon::findMinAltitude(const Polygon& subregion,
                                Direction_2* sweep_dir) const {
  // Get all possible sweep directions.
  std::vector<Direction_2> sweep_dirs = subregion.findEdgeDirections();
  std::vector<Polygon> rotated_polys = subregion.rotatePolygon(sweep_dirs);

  // Find minimum altitude.
  double min_altitude = std::numeric_limits<double>::max();
  for (size_t i = 0; i < sweep_dirs.size(); ++i) {
    const Polygon_2& poly_2 = rotated_polys[i].getPolygon().outer_boundary();
    // Check if sweepable.
    if (!CGAL::is_approx_y_monotone_2(poly_2.vertices_begin(),
                                      poly_2.vertices_end())) {
      DLOG(INFO) << "Polygon is not y-monotone.";
      continue;
    }

    double altitude = poly_2.bbox().ymax() - poly_2.bbox().ymin();
    if (altitude < min_altitude) {
      min_altitude = altitude;
      if (sweep_dir) *sweep_dir = sweep_dirs[i];
    }
  }

  return min_altitude;
}

bool Polygon::computeBestTrapezoidalDecompositionFromPolygonWithHoles(
    std::vector<Polygon>* trap_polygons) const {
  CHECK_NOTNULL(trap_polygons);
  trap_polygons->clear();
  double min_altitude_sum = std::numeric_limits<double>::max();

  // Get all possible decomposition directions.
  std::vector<Direction_2> directions = findPerpEdgeDirections();
  std::vector<Polygon> rotated_polys = rotatePolygon(directions);

  // For all possible rotations:
  Direction_2 best_direction = directions.front();
  CHECK_EQ(rotated_polys.size(), directions.size());
  for (size_t i = 0; i < rotated_polys.size(); ++i) {
    // Calculate decomposition.
    std::vector<Polygon> traps;
    if (!rotated_polys[i].computeTrapezoidalDecompositionFromPolygonWithHoles(
            &traps)) {
      LOG(WARNING) << "Failed to compute trapezoidal decomposition.";
      continue;
    }

    // Calculate minimum altitude sum for each cell.
    double min_altitude_sum_tmp = 0.0;
    for (const Polygon& trap : traps) {
      min_altitude_sum_tmp += findMinAltitude(trap);
    }

    // Update best decomposition.
    if (min_altitude_sum_tmp < min_altitude_sum) {
      min_altitude_sum = min_altitude_sum_tmp;
      *trap_polygons = traps;
      best_direction = directions[i];
    }
  }

  // Reverse trap rotation.
  CGAL::Aff_transformation_2<K> rotation(CGAL::ROTATION, best_direction, 1,
                                         1e3);
  for (Polygon& trap : *trap_polygons) {
    Polygon_2 trap_2 = trap.getPolygon().outer_boundary();
    trap_2 = CGAL::transform(rotation, trap_2);
    trap = Polygon(trap_2, trap.getPlaneTransformation());
  }

  if (trap_polygons->empty())
    return false;
  else
    return true;
}

bool Polygon::computeBestBCDFromPolygonWithHoles(
    std::vector<Polygon>* bcd_polygons) const {
  CHECK_NOTNULL(bcd_polygons);
  bcd_polygons->clear();
  double min_altitude_sum = std::numeric_limits<double>::max();

  // Get all possible decomposition directions.
  std::vector<Direction_2> directions = findPerpEdgeDirections();
  std::vector<Polygon> rotated_polys = rotatePolygon(directions);

  // For all possible rotations:
  Direction_2 best_direction = directions.front();
  CHECK_EQ(rotated_polys.size(), directions.size());
  for (size_t i = 0; i < rotated_polys.size(); ++i) {
    // Calculate decomposition.
    std::vector<Polygon> bcds;
    if (!rotated_polys[i].computeBCDFromPolygonWithHoles(&bcds)) {
      LOG(WARNING) << "Failed to compute boustrophedon decomposition.";
      continue;
    }

    // Calculate minimum altitude sum for each cell.
    double min_altitude_sum_tmp = 0.0;
    for (const Polygon& bcd : bcds) {
      min_altitude_sum_tmp += findMinAltitude(bcd);
    }

    // Update best decomposition.
    if (min_altitude_sum_tmp < min_altitude_sum) {
      min_altitude_sum = min_altitude_sum_tmp;
      *bcd_polygons = bcds;
      best_direction = directions[i];
    }
  }

  // Reverse bcd rotation.
  CGAL::Aff_transformation_2<K> rotation(CGAL::ROTATION, best_direction, 1,
                                         1e3);
  for (Polygon& bcd : *bcd_polygons) {
    Polygon_2 bcd_2 = bcd.getPolygon().outer_boundary();
    bcd_2 = CGAL::transform(rotation, bcd_2);
    bcd = Polygon(bcd_2, bcd.getPlaneTransformation());
  }

  if (bcd_polygons->empty())
    return false;
  else
    return true;
}

std::vector<Direction_2> Polygon::getUniformDirections(const int num) const {
  std::vector<Direction_2> dirs(num);
  double delta = 2.0 * M_PI / num;
  double alpha = 0.0;
  for (int i = 0; i < num; ++i) {
    dirs[i] = Direction_2(std::cos(alpha), std::sin(alpha));
    alpha += delta;
  }
  return dirs;
}

bool Polygon::offsetEdgeWithRadialOffset(const size_t& edge_id,
                                         double radial_offset,
                                         Polygon* offset_polygon) const {
  // Find perpendicular distance.
  Polygon_2::Edge_const_circulator e =
      std::next(polygon_.outer_boundary().edges_circulator(), edge_id);
  Polygon_2::Edge_const_circulator e_prev = std::prev(e);
  Polygon_2::Edge_const_circulator e_next = std::next(e);

  typedef CGAL::Cartesian_converter<InexactKernel, K> IK_to_EK;
  typedef CGAL::Cartesian_converter<K, InexactKernel> EK_to_IK;

  EK_to_IK toInexact;
  InexactKernel::Line_2 l = toInexact(e->supporting_line());
  InexactKernel::Line_2 l_prev = toInexact(e_prev->supporting_line());
  InexactKernel::Line_2 l_next = toInexact(e_next->supporting_line());

  IK_to_EK toExact;
  Line_2 bi_prev = toExact(CGAL::bisector(l, l_prev));
  Line_2 bi_next = toExact(CGAL::bisector(l, l_next));

  Polygon_2::Traits::Equal_2 eq_2;
  CHECK(eq_2(e->source(), e_prev->target()));
  CHECK(eq_2(e->target(), e_next->source()));

  double len_l_prev =
      std::sqrt(CGAL::to_double(bi_prev.to_vector().squared_length()));
  Vector_2 offset_prev = radial_offset / len_l_prev * bi_prev.to_vector();
  Point_2 p_prev = e->source() + offset_prev;

  double len_l_next =
      std::sqrt(CGAL::to_double(bi_next.to_vector().squared_length()));
  Vector_2 offset_next = radial_offset / len_l_next * bi_next.to_vector();
  Point_2 p_next = e->target() + offset_next;

  Point_2 p_prev_proj = e->supporting_line().projection(p_prev);
  Point_2 p_next_proj = e->supporting_line().projection(p_next);

  Segment_2 s_prev(p_prev, p_prev_proj);
  Segment_2 s_next(p_next, p_next_proj);

  FT offset_distance_prev = s_prev.squared_length();
  FT offset_distance_next = s_next.squared_length();

  double offset_distance_sq =
      std::min(CGAL::to_double(radial_offset * radial_offset),
               CGAL::to_double(offset_distance_prev));
  offset_distance_sq =
      std::min(CGAL::to_double(offset_distance_next), offset_distance_sq);

  return offsetEdge(edge_id, std::sqrt(offset_distance_sq), offset_polygon);
}

bool Polygon::offsetEdge(const size_t& edge_id, double offset,
                         Polygon* offset_polygon) const {
  CHECK_NOTNULL(offset_polygon);
  *offset_polygon = *this;

  if (!is_strictly_simple_) {
    DLOG(WARNING) << "Polygon is not strictly simple.";
    return false;
  }
  if (polygon_.number_of_holes() > 0) {
    DLOG(WARNING) << "Polygon has holes.";
    return false;
  }

  // Create mask.
  // Copy of polygon with edge start being first vertex.
  Polygon_2 polygon;
  Polygon_2::Vertex_circulator vc_start =
      std::next(polygon_.outer_boundary().vertices_circulator(), edge_id);
  Polygon_2::Vertex_circulator vc = vc_start;
  do {
    polygon.push_back(*vc);
  } while (++vc != vc_start);

  // Transform polygon to have first vertex in origin and first edge aligned
  // with x-axis.
  CGAL::Aff_transformation_2<K> translation(
      CGAL::TRANSLATION,
      Vector_2(-polygon.vertices_begin()->x(), -polygon.vertices_begin()->y()));
  polygon = CGAL::transform(translation, polygon);

  CGAL::Aff_transformation_2<K> rotation(
      CGAL::ROTATION, polygon.edges_begin()->direction(), 1, 1e9);
  rotation = rotation.inverse();
  polygon = CGAL::transform(rotation, polygon);

  // Calculate all remaining sweeps.
  const double kMaskOffset = 1e-6;  // To cope with numerical imprecision.
  double min_y = -kMaskOffset;
  double max_y = offset + kMaskOffset;
  if (0.5 * polygon.bbox().ymax() <= max_y) {
    max_y = 0.5 * polygon.bbox().ymax() - kMaskOffset;
    DLOG(INFO) << "Offset too large. Re-adjusting.";
  }
  double min_x = polygon.bbox().xmin() - kMaskOffset;
  double max_x = polygon.bbox().xmax() + kMaskOffset;
  // Create sweep mask rectangle.
  Polygon_2 mask;
  mask.push_back(Point_2(min_x, min_y));
  mask.push_back(Point_2(max_x, min_y));
  mask.push_back(Point_2(max_x, max_y));
  mask.push_back(Point_2(min_x, max_y));

  // Intersect mask and polygon.
  std::vector<PolygonWithHoles> intersection_list;
  CGAL::intersection(polygon, mask, std::back_inserter(intersection_list));
  if (intersection_list.size() != 1) {
    DLOG(WARNING) << "Not exactly one resulting intersections."
                  << intersection_list.size();
    return false;
  }
  if (intersection_list[0].number_of_holes() > 0) {
    DLOG(WARNING) << "Mask intersection has holes.";
    return false;
  }
  Polygon_2 intersection = intersection_list[0].outer_boundary();

  // Difference intersection and polygon.
  std::vector<PolygonWithHoles> diff_list;
  CGAL::difference(polygon, intersection, std::back_inserter(diff_list));
  if (diff_list.size() != 1) {
    DLOG(WARNING) << "Not exactle one resulting difference polygon."
                  << diff_list.size();
    return false;
  }
  if (diff_list[0].number_of_holes() > 0) {
    DLOG(WARNING) << "Polygon difference has holes.";
    return false;
  }
  Polygon_2 difference = diff_list[0].outer_boundary();

  // Transform back.
  translation = translation.inverse();
  rotation = rotation.inverse();
  difference = CGAL::transform(rotation, difference);
  difference = CGAL::transform(translation, difference);

  // Create polygon object.
  *offset_polygon = Polygon(difference);
  return true;
}

bool Polygon::computeBCDFromPolygonWithHoles(
    std::vector<Polygon>* bcd_polygons) const {
  CHECK_NOTNULL(bcd_polygons);
  bcd_polygons->clear();

  BCD bcd(polygon_);
  std::vector<Polygon_2> polygons =
      computeBCDExact(polygon_, Direction_2(1, 0));

  for (const Polygon_2& p : polygons) {
    bcd_polygons->emplace_back(p);
  }

  return true;
}

std::stringstream Polygon::printPolygon(const Polygon_2& poly) const {
  std::stringstream stream;
  stream << "Polygon with " << poly.size() << " vertices" << std::endl;
  for (Polygon_2::Vertex_const_iterator vi = poly.vertices_begin();
       vi != poly.vertices_end(); ++vi) {
    stream << "(" << vi->x() << "," << vi->y() << ")" << std::endl;
  }

  return stream;
}

std::ostream& operator<<(std::ostream& stream, const Polygon& p) {
  stream << std::endl
         << "Polygon_with_holes having " << p.polygon_.number_of_holes()
         << " holes" << std::endl;
  stream << "Vertices: " << std::endl;
  stream << p.printPolygon(p.polygon_.outer_boundary()).str();

  size_t i = 0;
  for (PolygonWithHoles::Hole_const_iterator hi = p.polygon_.holes_begin();
       hi != p.polygon_.holes_end(); ++hi) {
    stream << "Hole [" << i++ << "]" << std::endl;
    stream << p.printPolygon(*hi).str();
  }
  return stream;
}

bool Polygon::checkConvexity() const {
  if (polygon_.number_of_holes() > 0) return false;
  return polygon_.outer_boundary().is_convex();
}

void Polygon::sortCC() {
  if (polygon_.outer_boundary().is_clockwise_oriented())
    polygon_.outer_boundary().reverse_orientation();

  for (PolygonWithHoles::Hole_iterator hi = polygon_.holes_begin();
       hi != polygon_.holes_end(); ++hi)
    if (hi->is_counterclockwise_oriented()) hi->reverse_orientation();
}

void Polygon::simplifyPolygon(Polygon_2* polygon) {
  CHECK_NOTNULL(polygon);

  std::vector<Polygon_2::Vertex_circulator> v_to_erase;

  Polygon_2::Vertex_circulator vc = polygon->vertices_circulator();
  // Find collinear vertices.
  do {
    if (CGAL::collinear(*std::prev(vc), *vc, *std::next(vc))) {
      v_to_erase.push_back(vc);
    }
  } while (++vc != polygon->vertices_circulator());

  // Remove intermediate vertices.
  for (std::vector<Polygon_2::Vertex_circulator>::reverse_iterator rit =
           v_to_erase.rbegin();
       rit != v_to_erase.rend(); ++rit) {
    polygon->erase(*rit);
  }
}

void Polygon::simplify() {
  simplifyPolygon(&polygon_.outer_boundary());

  for (PolygonWithHoles::Hole_iterator hi = polygon_.holes_begin();
       hi != polygon_.holes_end(); ++hi)
    simplifyPolygon(&*hi);
}

bool Polygon::computeLineSweepPlan(double max_sweep_distance,
                                   size_t start_vertex_idx,
                                   bool counter_clockwise,
                                   std::vector<Point_2>* waypoints) const {
  CHECK_NOTNULL(waypoints);
  waypoints->clear();

  // Preconditions.
  if (!is_strictly_simple_) {
    DLOG(INFO) << "Polygon is not strictly simple.";
    return false;
  }
  if (polygon_.number_of_holes() > 0) {
    DLOG(INFO) << "Polygon has holes.";
    return false;
  }
  if (polygon_.outer_boundary().size() < 3) {
    DLOG(INFO) << "Polygon has less than 3 vertices.";
    return false;
  }

  // Copy of polygon with start_vertex_idx being first vertex.
  Polygon_2 polygon;
  Polygon_2::Vertex_circulator vc_start = std::next(
      polygon_.outer_boundary().vertices_circulator(), start_vertex_idx);
  Polygon_2::Vertex_circulator vc = vc_start;
  do {
    polygon.push_back(*vc);
  } while (++vc != vc_start);

  // Change orientation according to arguments.
  if (!counter_clockwise) polygon.reverse_orientation();

  // Transform polygon to have first vertex in origin and first edge aligned
  // with x-axis.
  CGAL::Aff_transformation_2<K> translation(
      CGAL::TRANSLATION,
      Vector_2(-polygon.vertices_begin()->x(), -polygon.vertices_begin()->y()));
  polygon = CGAL::transform(translation, polygon);

  CGAL::Aff_transformation_2<K> rotation(
      CGAL::ROTATION, polygon.edges_begin()->direction(), 1, 1e9);
  rotation = rotation.inverse();
  polygon = CGAL::transform(rotation, polygon);

  if (!CGAL::is_approx_y_monotone_2(polygon.vertices_begin(),
                                    polygon.vertices_end())) {
    DLOG(INFO) << "Polygon is not y-monotone.";
    return false;
  }

  // Compute sweep distance for equally spaced sweeps.
  double polygon_length = polygon.bbox().ymax() - polygon.bbox().ymin();
  int num_sweeps =
      static_cast<int>(std::ceil(polygon_length / max_sweep_distance)) + 1;
  double sweep_distance = polygon_length / (num_sweeps - 1);

  // Return a single sweep line if polygon is too narrow.
  const double kMaxOverlapError = 0.01;
  if (polygon_length < kMaxOverlapError * max_sweep_distance) {
    waypoints->push_back(*polygon.vertices_begin());
    waypoints->push_back(*std::next(polygon.vertices_begin()));
    // Transform back waypoints.
    translation = translation.inverse();
    rotation = rotation.inverse();
    for (Point_2& p : *waypoints) {
      p = rotation(p);
      p = translation(p);
    }
    return true;
  }

  // Calculate all remaining sweeps.
  const double kMaskOffset = 1e-6;  // To cope with numerical imprecision.
  double min_y = -kMaskOffset;
  double max_y = sweep_distance + kMaskOffset;
  double min_x = polygon.bbox().xmin() - kMaskOffset;
  double max_x = polygon.bbox().xmax() + kMaskOffset;
  // Create sweep mask rectangle.
  Polygon_2 sweep_mask;
  sweep_mask.push_back(Point_2(min_x, min_y));
  sweep_mask.push_back(Point_2(max_x, min_y));
  sweep_mask.push_back(Point_2(max_x, max_y));
  sweep_mask.push_back(Point_2(min_x, max_y));
  // Compute sweep mask transformation. The sweep line gets shifted in
  // y-direction with every new sweep.
  CGAL::Aff_transformation_2<K> sweep_mask_trafo(CGAL::TRANSLATION,
                                                 Vector_2(0.0, sweep_distance));
  // We start sweeping the mask at the outermost polygon point.
  CGAL::Aff_transformation_2<K> initial_offset(
      CGAL::TRANSLATION, Vector_2(0.0, polygon.bbox().ymin()));

  bool sweep_is_cc = true;  // Toggle.
  // Initially clockwise sweeps get handled slightly different.
  if (!counter_clockwise) {
    sweep_mask_trafo = sweep_mask_trafo.inverse();
    sweep_mask = CGAL::transform(sweep_mask_trafo, sweep_mask);
    sweep_mask.reverse_orientation();
    sweep_is_cc = !sweep_is_cc;
    initial_offset = CGAL::Aff_transformation_2<K>(
        CGAL::TRANSLATION, Vector_2(0.0, polygon.bbox().ymax()));
  }
  sweep_mask = CGAL::transform(initial_offset, sweep_mask);

  for (int i = 0; i < num_sweeps - 1; ++i) {
    // Find intersection between sweep mask and polygon that includes the
    // sweep waypoints to be added.
    std::vector<PolygonWithHoles> intersections;
    CGAL::intersection(polygon, sweep_mask, std::back_inserter(intersections));
    // Some assertions.
    if (intersections.size() != 1) {
      LOG(WARNING) << "Number of mask intersections is not 1.";
      return false;
    }
    if (intersections.front().number_of_holes() != 0) {
      LOG(WARNING) << "Intersection has holes.";
      return false;
    }

    // Orientate intersection according to sweep direction.
    Polygon_2& intersection = intersections.front().outer_boundary();
    CGAL::Orientation intersection_orientation = intersection.orientation();
    if ((intersection_orientation == CGAL::CLOCKWISE && sweep_is_cc) ||
        (intersection_orientation == CGAL::COUNTERCLOCKWISE && !sweep_is_cc))
      intersection.reverse_orientation();

    // Add intersection vertices to waypoints. We skip the previous sweep and
    // stop at the current sweep. For a counter clockwise polygon the two
    // bottom most waypoints represent the vertices after the top most
    // vertices represent the current sweep.

    // 1. Find the two top and bottom most vertices.
    VertexConstCirculator bot_v = intersection.vertices_circulator();
    VertexConstCirculator sec_bot_v =
        std::next(intersection.vertices_circulator());
    if (sec_bot_v->y() < bot_v->y()) std::swap(bot_v, sec_bot_v);

    VertexConstCirculator top_v = intersection.vertices_circulator();
    VertexConstCirculator sec_top_v =
        std::next(intersection.vertices_circulator());
    if (sec_top_v->y() > top_v->y()) std::swap(top_v, sec_top_v);

    VertexConstCirculator vit = intersection.vertices_circulator();
    do {
      // Bottom.
      if (vit->y() < bot_v->y()) {
        sec_bot_v = bot_v;
        bot_v = vit;
      } else if ((vit->y() < sec_bot_v->y()) && (vit != bot_v))
        sec_bot_v = vit;

      // Top.
      if (vit->y() > top_v->y()) {
        sec_top_v = top_v;
        top_v = vit;
      } else if ((vit->y() > sec_top_v->y()) && (vit != top_v))
        sec_top_v = vit;
    } while (++vit != intersection.vertices_circulator());

    // 2. Find the edge that represents the bottom sweep and top sweep.
    EdgeConstCirculator bottom_sweep = intersection.edges_circulator();
    EdgeConstCirculator top_sweep = intersection.edges_circulator();
    EdgeConstCirculator eit = intersection.edges_circulator();

    bool has_bottom_sweep = false, has_top_sweep = false;
    do {
      // Bottom.
      if ((eit->source() == *bot_v && eit->target() == *sec_bot_v) ||
          (eit->source() == *sec_bot_v && eit->target() == *bot_v)) {
        has_bottom_sweep = true;
        bottom_sweep = eit;
      }

      // Top.
      if ((eit->source() == *top_v && eit->target() == *sec_top_v) ||
          (eit->source() == *sec_top_v && eit->target() == *top_v)) {
        has_top_sweep = true;
        top_sweep = eit;
      }
    } while (++eit != intersection.edges_circulator());
    // Assertion.
    if (!has_bottom_sweep || !has_top_sweep) {
      LOG(ERROR) << "Has no bottom or top sweep.";
      return false;
    }

    // 3. Add waypoints from previous sweep to current sweep.
    // Note: The first sweep is handled differently. Here the full polygon
    // contour is followed starting from the next sweeps line.
    // Note: The last sweep is handled differently. Here the full polygon
    // contour is followed until reaching the previous sweep source again.
    EdgeConstCirculator prev_sweep = bottom_sweep;
    EdgeConstCirculator current_sweep = top_sweep;
    if (!counter_clockwise) std::swap(prev_sweep, current_sweep);

    eit = prev_sweep;
    // Add first bottom sweep.
    const double kCoverageOffset = kMaskOffset + 1e-3;
    if (i == 0) {
      std::vector<EdgeConstCirculator> start_edges = {eit};
      // Check if additional waypoints from previous edges need to be added to
      // have full coverage.
      while (!isCovered(current_sweep->target(), start_edges, sweep_distance,
                        kCoverageOffset))
        start_edges.push_back(std::prev(start_edges.back()));
      for (std::vector<EdgeConstCirculator>::reverse_iterator it =
               start_edges.rbegin();
           it != start_edges.rend(); ++it)
        waypoints->push_back((*it)->source());
      waypoints->push_back(start_edges.front()->target());
    }

    if (i < num_sweeps - 1)
      while (++eit != std::next(current_sweep))
        waypoints->push_back(eit->target());

    // Add additional final edges if necessary.
    if (i == num_sweeps - 2) {
      std::vector<EdgeConstCirculator> last_edges = {current_sweep};
      while (!isCovered(prev_sweep->source(), last_edges, sweep_distance,
                        kCoverageOffset))
        last_edges.push_back(std::next(last_edges.back()));

      if (last_edges.size() > 1)
        for (std::vector<EdgeConstCirculator>::iterator it =
                 std::next(last_edges.begin());
             it != last_edges.end(); ++it)
          waypoints->push_back((*it)->target());
    }

    // Prepare for next sweep.
    sweep_mask = CGAL::transform(sweep_mask_trafo, sweep_mask);
    sweep_is_cc = !sweep_is_cc;
  }

  // Transform back waypoints.
  translation = translation.inverse();
  rotation = rotation.inverse();
  for (Point_2& p : *waypoints) {
    p = rotation(p);
    p = translation(p);
  }

  return true;
}

bool Polygon::intersect(const Polygon& p, Polygon* intersection) const {
  CHECK_NOTNULL(intersection);

  std::vector<PolygonWithHoles> intersection_list;
  CGAL::intersection(polygon_, p.getPolygon(),
                     std::back_inserter(intersection_list));
  if (intersection_list.size() != 1) {
    DLOG(WARNING) << "Not exactly one resulting intersections."
                  << intersection_list.size();
    return false;
  }

  *intersection = Polygon(intersection_list[0]);

  return true;
}

bool Polygon::isCovered(const Point_2& p,
                        const std::vector<EdgeConstCirculator>& edges,
                        double sweep_distance, double margin) const {
  if (edges.empty()) return false;

  FT sqr_d_min = CGAL::squared_distance(p, *edges.front());
  for (size_t i = 1; i < edges.size(); ++i) {
    FT sqr_d = CGAL::squared_distance(p, *(edges[i]));
    sqr_d_min = sqr_d < sqr_d_min ? sqr_d : sqr_d_min;
  }

  return std::sqrt(CGAL::to_double(sqr_d_min)) < sweep_distance + margin;
}

Polyhedron_3 Polygon::toMesh() const {
  // Triangulation.
  std::vector<std::vector<Point_2>> faces_2;
  triangulatePolygon(polygon_, &faces_2);

  // To 3D mesh.
  Polyhedron_3 mesh;
  for (const std::vector<Point_2>& face : faces_2) {
    std::vector<Point_3> triangle = plane_tf_.to3d(face);

    // TODO(rikba): Replace this with polyhedron mesh builder.
    mesh.make_triangle(
        convertPoint3<Point_3, Polyhedron_3::Point_3>(triangle[0]),
        convertPoint3<Point_3, Polyhedron_3::Point_3>(triangle[1]),
        convertPoint3<Point_3, Polyhedron_3::Point_3>(triangle[2]));
  }

  return mesh;
}

}  // namespace mav_coverage_planning
