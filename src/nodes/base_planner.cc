#include "mav_coverage_planning_ros/nodes/base_planner.h"

#include <functional>

#include "mav_coverage_planning_ros/conversions/msg_from_xml_rpc.h"
#include "mav_coverage_planning_ros/conversions/ros_interface.h"

#include <geometry_msgs/PoseArray.h>
#include <mav_3d_coverage_mesh_conversion/grid_map/conversion.h>
#include <mav_3d_coverage_planning/mesh_processing/clipping.h>
#include <mav_coverage_planning_comm/trajectory_cost_functions.h>
#include <mav_trajectory_generation/trajectory_sampling.h>
#include <mav_trajectory_generation_ros/ros_visualization.h>
#include <visualization_msgs/MarkerArray.h>

namespace mav_coverage_planning {

constexpr double kThrottleRate = 1.0 / 10.0;

BasePlanner::BaseSettings::BaseSettings()
    : trajectory_cost_function(
          std::bind(&computeTrajectoryTime, std::placeholders::_1)),
      altitude(-1.0),
      latch_topics(true),
      local_frame_id("odom"),
      global_frame_id("world"),
      publish_plan_on_planning_complete(false),
      publish_visualization_on_planning_complete(true) {}

BasePlanner::BasePlanner(const ros::NodeHandle& nh,
                         const ros::NodeHandle& nh_private)
    : nh_(nh),
      nh_private_(nh_private),
      planning_complete_(false),
      odometry_set_(false),
      odometry_in_global_frame_(true) {
  // Initial interactions with ROS
  getBaseParametersFromRos();
  subscribeToBaseTopics();
  advertiseBaseTopics();
}

void BasePlanner::subscribeToBaseTopics() {
  odometry_sub_ =
      nh_.subscribe("odometry", 1, &BasePlanner::receiveOdometryCallback, this);
  T_G_L_sub_ =
      nh_.subscribe("T_G_L", 1, &BasePlanner::receiveTransformCallback, this);
}

void BasePlanner::advertiseBaseTopics() {
  // Advertising the visualization and planning messages
  marker_pub_ = nh_private_.advertise<visualization_msgs::MarkerArray>(
      "path_markers", 1, settings_.latch_topics);
  raw_polyhedron_pub_ = nh_private_.advertise<visualization_msgs::MarkerArray>(
      "raw_polyhedron_markers", 1, settings_.latch_topics);
  clipped_polyhedron_pub_ =
      nh_private_.advertise<visualization_msgs::MarkerArray>(
          "clipped_polyhedron_markers", 1, settings_.latch_topics);
  waypoint_list_pub_ = nh_.advertise<geometry_msgs::PoseArray>(
      "waypoint_list", 1, settings_.latch_topics);
  // Services for generating the plan.
  plan_path_srv_ = nh_private_.advertiseService(
      "plan_path", &BasePlanner::planPathCallback, this);
  plan_path_from_and_to_odometry_srv_ = nh_private_.advertiseService(
      "plan_path_from_and_to_odometry",
      &BasePlanner::planPathFromAndToOdometryCallback, this);
  plan_path_from_odometry_to_goal_srv_ = nh_private_.advertiseService(
      "plan_path_from_odometry_to_goal",
      &BasePlanner::planPathFromOdometryToGoalCallback, this);
  // Services for performing publishing and visualization
  publish_all_srv_ = nh_private_.advertiseService(
      "publish_all", &BasePlanner::publishAllCallback, this);
  publish_visualization_srv_ = nh_private_.advertiseService(
      "publish_visualization", &BasePlanner::publishVisualizationCallback,
      this);
  publish_plan_points_srv_ = nh_private_.advertiseService(
      "publish_path_points", &BasePlanner::publishTrajectoryPointsCallback,
      this);
}

void BasePlanner::getBaseParametersFromRos() {
  // Getting control params from the server
  if (!nh_private_.getParam("local_frame_id", settings_.local_frame_id)) {
    ROS_WARN_STREAM("No local frame id specified. Using default value of: "
                    << settings_.local_frame_id);
  }

  // Cost function
  setCostFunction();
  setPolygon();
  setPolyhedronFromGridmap();
  clip();

  nh_private_.getParam("altitude", settings_.altitude);

  // Getting the behaviour flags
  nh_private_.getParam("latch_topics", settings_.latch_topics);
  nh_private_.getParam("publish_plan_on_planning_complete",
                       settings_.publish_plan_on_planning_complete);
  nh_private_.getParam("publish_visualization_on_planning_complete",
                       settings_.publish_visualization_on_planning_complete);
}

void BasePlanner::setCostFunction() {
  int cost_function_type_int = static_cast<int>(settings_.cost_function_type);
  if (!nh_private_.getParam("cost_function_type", cost_function_type_int)) {
    ROS_WARN_STREAM("No cost_function_type specified. Using default value of: "
                    << settings_.cost_function_type << "("
                    << settings_.getCostFunctionTypeName() << ").");
  }
  settings_.cost_function_type =
      static_cast<BaseSettings::CostFunctionType>(cost_function_type_int);
  if (!settings_.checkCostFunctionTypeValid()) {
    settings_.cost_function_type = BaseSettings::CostFunctionType::kTime;
    ROS_WARN_STREAM("cost_function_type not valid. Resetting to default: "
                    << settings_.cost_function_type << "("
                    << settings_.getCostFunctionTypeName() << ").");
  }

  switch (settings_.cost_function_type) {
    case BaseSettings::CostFunctionType::kDistance: {
      settings_.trajectory_cost_function =
          std::bind(&computeTrajectoryLength, std::placeholders::_1);
      break;
    }
    case BaseSettings::CostFunctionType::kTime: {
      settings_.trajectory_cost_function =
          std::bind(&computeTrajectoryTime, std::placeholders::_1);
      break;
    }
    default: {
      ROS_ERROR_STREAM("Cost function type: "
                       << settings_.getCostFunctionTypeName()
                       << "not implemented.");
      break;
    }
  }
}

void BasePlanner::setPolygon() {
  // Load the polygon from polygon message from parameter server.
  // The altitude and the global frame ID are set from the same message.
  XmlRpc::XmlRpcValue polygon_xml_rpc;
  const std::string polygon_param_name = "polygon";
  if (nh_private_.getParam(polygon_param_name, polygon_xml_rpc)) {
    mav_planning_msgs::PolygonWithHolesStamped poly_msg;
    if (PolygonWithHolesStampedMsgFromXmlRpc(polygon_xml_rpc, &poly_msg)) {
      if (polygonFromMsg(poly_msg, &settings_.polygon, &settings_.altitude,
                         &settings_.global_frame_id)) {
        ROS_INFO_STREAM("Successfully loaded polygon.");
        ROS_INFO_STREAM("Altiude: " << settings_.altitude << "m");
        ROS_INFO_STREAM("Global frame: " << settings_.global_frame_id);
        ROS_INFO_STREAM("Polygon:" << settings_.polygon);
      }
    } else {
      ROS_WARN_STREAM("Failed reading polygon message from parameter server.");
    }
  } else {
    ROS_WARN_STREAM(
        "No polygon file specified to parameter "
        "server (parameter \""
        << polygon_param_name
        << "\"). Expecting "
           "polygon from service call.");
  }
}

void BasePlanner::setPolyhedronFromGridmap() {
  ROS_INFO("Load DSM grid map.");
  std::string gridmap_bag;
  if (!nh_private_.getParam("gridmap_bag", gridmap_bag)) {
    ROS_WARN("Gridmap bag filename not set.");
    return;
  }
  ROS_INFO_STREAM("Opening file: " << gridmap_bag);

  if (!loadMeshFromGridMapBag<Polyhedron_3>(
          gridmap_bag, "/grid_map", "elevation", &settings_.raw_polyhedron))
    ROS_WARN("Failed to load grid map.");
}

void BasePlanner::clip() {
  ROS_INFO("Clipping polyhedron.");

  if (!clipPolyhedron<Polyhedron_3, InexactKernel>(
          settings_.polygon.getPolygon(), settings_.raw_polyhedron,
          &settings_.clipped_polyhedron))
    ROS_WARN("Failed clipping.");
}

void BasePlanner::receiveOdometryCallback(const nav_msgs::Odometry& msg) {
  mav_msgs::eigenOdometryFromMsg(msg, &odometry_);
  odometry_set_ = true;
  ROS_INFO_STREAM_ONCE("Received first odometry message.");

  odometry_in_global_frame_ =
      (msg.header.frame_id == settings_.global_frame_id);
  if (!odometry_in_global_frame_) {
    ROS_INFO_STREAM_THROTTLE(kThrottleRate,
                             "Odometry message in frame: \""
                                 << msg.header.frame_id
                                 << "\". Will convert it using T_G_L.");
  }
}

void BasePlanner::receiveTransformCallback(
    const geometry_msgs::TransformStamped& msg) {
  tf::transformMsgToKindr(msg.transform, &T_G_L_);
  if (msg.header.frame_id != settings_.global_frame_id ||
      msg.child_frame_id != settings_.local_frame_id) {
    ROS_WARN_STREAM_ONCE(
        "Expected and received T_G_L frame ids do "
        "not agree. Expected: G = \""
        << settings_.global_frame_id << "\", L = \"" << settings_.local_frame_id
        << "\" Received: G = \"" << msg.header.frame_id << "\", L = \""
        << msg.child_frame_id << "\".");
  }
}

void BasePlanner::solve(const mav_msgs::EigenTrajectoryPoint& start,
                        const mav_msgs::EigenTrajectoryPoint& goal) {
  ROS_INFO_STREAM("Start solving.");
  if ((planning_complete_ = solvePlanner(start, goal))) {
    ROS_INFO_STREAM("Finished plan."
                    << std::endl
                    << "Optimization Criterion: "
                    << settings_.getCostFunctionTypeName() << std::endl
                    << "Number of waypoints: " << waypoints_.size() << std::endl
                    << "Start: " << start.toString() << std::endl
                    << "Goal: " << goal.toString() << std::endl
                    << "Altitude: " << settings_.altitude << " [m]" << std::endl
                    << "Path cost: "
                    << settings_.trajectory_cost_function(trajectory_));
    // Publishing the plan if requested
    if (settings_.publish_plan_on_planning_complete) {
      publishTrajectoryPoints();
    }
    // Publishing the visualization if requested
    if (settings_.publish_visualization_on_planning_complete) {
      publishVisualization();
    }
  } else {
    ROS_ERROR_STREAM("Failed calculating plan.");
  }
}

void BasePlanner::publishVisualization() {
  ROS_INFO_STREAM("Sending visualization messages.");

  // Creating the marker array
  visualization_msgs::MarkerArray markers;

  // The solution.
  if (planning_complete_) {
    // The waypoints:
    visualization_msgs::MarkerArray vertices;
    mav_trajectory_generation::drawVerticesFromTrajectory(
        trajectory_, settings_.global_frame_id, &vertices);
    markers.markers.insert(markers.markers.end(), vertices.markers.begin(),
                           vertices.markers.end());

    // The trajectory:
    visualization_msgs::MarkerArray trajectory_markers;
    const double kMarkerDistance = 0.0;
    mav_trajectory_generation::drawMavTrajectory(trajectory_, kMarkerDistance,
                                                 settings_.global_frame_id,
                                                 &trajectory_markers);
    markers.markers.insert(markers.markers.end(),
                           trajectory_markers.markers.begin(),
                           trajectory_markers.markers.end());

    // Start and end points
    visualization_msgs::Marker start_point, end_point;
    createStartAndEndPointMarkers(waypoints_.front(), waypoints_.back(),
                                  settings_.global_frame_id, "start_and_goal",
                                  &start_point, &end_point);
    markers.markers.push_back(start_point);
    markers.markers.push_back(end_point);
  }

  // The polygon to cover:
  visualization_msgs::MarkerArray polygon;
  createPolygonMarkers(settings_.polygon, settings_.altitude,
                       settings_.global_frame_id, "polygon",
                       mav_visualization::Color::Blue(),
                       mav_visualization::Color::Orange(), &polygon);
  markers.markers.insert(markers.markers.end(), polygon.markers.begin(),
                         polygon.markers.end());

  // The raw polyhedron to cover.
  visualization_msgs::MarkerArray mesh;
  if (!createPolyhedronMarkerArray(settings_.raw_polyhedron,
                                   settings_.global_frame_id, &mesh)) {
    ROS_WARN("Failed to generate raw polyhedron mesh markers.");
  } else {
    raw_polyhedron_pub_.publish(mesh);
  }

  // The clipped polyhedron to cover.
  visualization_msgs::MarkerArray clipped_mesh;
  if (!createPolyhedronMarkerArray(settings_.clipped_polyhedron,
                                   settings_.global_frame_id, &clipped_mesh)) {
    ROS_WARN("Failed to generate clipped polyhedron mesh markers.");
  } else {
    clipped_polyhedron_pub_.publish(clipped_mesh);
  }

  // Publishing
  marker_pub_.publish(markers);
}

bool BasePlanner::publishTrajectoryPoints() {
  if (!planning_complete_) {
    ROS_WARN(
        "Cannot send trajectory messages because plan hasn\'t been made, yet.");
    return false;
  }
  ROS_INFO_STREAM("Sending trajectory messages");

  // Convert path to pose array.
  geometry_msgs::PoseArray trajectory_points_pose_array;
  poseArrayMsgFromEigenTrajectoryPointVector(
      waypoints_, settings_.global_frame_id, &trajectory_points_pose_array);
  trajectory_points_pose_array.header.stamp = ros::Time::now();

  // Publishing
  waypoint_list_pub_.publish(trajectory_points_pose_array);

  // Success
  return true;
}

bool BasePlanner::setPolygonCallback(
    mav_planning_msgs::PolygonService::Request& request,
    mav_planning_msgs::PolygonService::Response& response) {
  planning_complete_ = false;

  if (!polygonFromMsg(request.polygon, &settings_.polygon, &settings_.altitude,
                      &settings_.global_frame_id)) {
    ROS_ERROR_STREAM("Failed loading correct polygon.");
    ROS_ERROR_STREAM("Planner is in an invalid state.");
    settings_.polygon = Polygon();
  }
  response.success = resetPlanner();
  return true;  // Still return true to identify service has been reached.
}

bool BasePlanner::planPathCallback(
    mav_planning_msgs::PlannerService::Request& request,
    mav_planning_msgs::PlannerService::Response& response) {
  mav_msgs::EigenTrajectoryPoint start, goal;
  eigenTrajectoryPointFromPoseMsg(request.start_pose, &start);
  eigenTrajectoryPointFromPoseMsg(request.goal_pose, &goal);
  solve(start, goal);  // Calculate optimal path.
  if (planning_complete_) {
    mav_msgs::EigenTrajectoryPointVector flat_states;
    const double kSamplingTime = 0.01;
    mav_trajectory_generation::sampleWholeTrajectory(trajectory_, kSamplingTime,
                                                     &flat_states);
    mav_msgs::msgMultiDofJointTrajectoryFromEigen(flat_states,
                                                  &response.sampled_plan);
  }
  response.success = planning_complete_;
  return true;
}

bool BasePlanner::planningRequestStartPoseFromOdometry(
    mav_planning_msgs::PlannerService::Request* req) const {
  if (!odometry_set_) {
    ROS_ERROR_STREAM("Did not receive odometry.");
    return false;
  }
  // Convert odometry to global frame id.
  mav_msgs::EigenOdometry odometry_global =
      globalOdometryFromOdometry(odometry_);
  req->start_pose.pose.position.x = odometry_global.position_W.x();
  req->start_pose.pose.position.y = odometry_global.position_W.y();
  return true;
}

bool BasePlanner::planPathFromAndToOdometryCallback(
    mav_planning_msgs::PlannerService::Request& request,
    mav_planning_msgs::PlannerService::Response& response) {
  // Convert odometry msg to planning request.
  if (planningRequestStartPoseFromOdometry(&request)) {
    request.goal_pose = request.start_pose;
    planPathCallback(request, response);
  } else {
    response.success = false;
  }
  return true;
}

bool BasePlanner::planPathFromOdometryToGoalCallback(
    mav_planning_msgs::PlannerService::Request& request,
    mav_planning_msgs::PlannerService::Response& response) {
  // Convert odometry msg to planning request.
  if (planningRequestStartPoseFromOdometry(&request)) {
    planPathCallback(request, response);
  } else {
    response.success = false;
  }
  return true;
}

bool BasePlanner::publishAllCallback(std_srvs::Empty::Request& request,
                                     std_srvs::Empty::Response& response) {
  bool success_publish_trajectory = publishTrajectoryPoints();
  publishVisualization();
  return (success_publish_trajectory);
}

bool BasePlanner::publishVisualizationCallback(
    std_srvs::Empty::Request& request, std_srvs::Empty::Response& response) {
  publishVisualization();
  return true;
}

bool BasePlanner::publishTrajectoryPointsCallback(
    std_srvs::Empty::Request& request, std_srvs::Empty::Response& response) {
  return publishTrajectoryPoints();
}

mav_msgs::EigenOdometry BasePlanner::globalOdometryFromOdometry(
    const mav_msgs::EigenOdometry& odometry) const {
  // Check if odometry is already in local frame.
  if (odometry_in_global_frame_) {
    return odometry;
  } else {
    ROS_INFO_STREAM(
        "Transforming odometry message from local frame using T_G_L:\n"
        << T_G_L_);
    mav_msgs::EigenOdometry odometry_global;
    odometry_global.position_W = T_G_L_ * odometry.position_W;
    odometry_global.orientation_W_B =
        T_G_L_.getRotation().toImplementation() * odometry.orientation_W_B;
    return odometry_global;
  }
}

}  // namespace mav_coverage_planning
