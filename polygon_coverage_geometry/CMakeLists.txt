cmake_minimum_required(VERSION 2.8.4)
project(polygon_coverage_geometry)

find_package(catkin_simple REQUIRED)
catkin_simple(ALL_DEPS_REQUIRED)

find_package(CGAL QUIET COMPONENTS Core)
include(${CGAL_USE_FILE})

add_definitions(-std=c++11)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
set(CMAKE_BUILD_TYPE Release)

#############
# LIBRARIES #
#############
cs_add_library(${PROJECT_NAME}
  src/bcd.cc
  src/cgal_comm.cc
  src/offset.cc
  src/visibility_graph.cc
  src/visibility_polygon.cc
  src/weakly_monotone.cc
  src/sweep.cc
)
target_link_libraries(${PROJECT_NAME} ${CGAL_LIBRARIES} ${CGAL_3RD_PARTY_LIBRARIES})

#########
# TESTS #
#########
catkin_add_gtest(test_bcd
  test/bcd-test.cpp
)
target_link_libraries(test_bcd ${PROJECT_NAME})

catkin_add_gtest(test_cgal_comm
  test/cgal_comm-test.cpp
)
target_link_libraries(test_cgal_comm ${PROJECT_NAME})

catkin_add_gtest(test_offset
  test/offset-test.cpp
)
target_link_libraries(test_offset ${PROJECT_NAME})

catkin_add_gtest(test_sweep
  test/sweep-test.cpp
)
target_link_libraries(test_sweep ${PROJECT_NAME})

catkin_add_gtest(test_visibility_polygon
  test/visibility_polygon-test.cpp
)
target_link_libraries(test_visibility_polygon ${PROJECT_NAME})

##########
# EXPORT #
##########
cs_install()
cs_export()
