#ifndef GLOCAL_EXPLORATION_ROS_CONVERSIONS_ROS_COMPONENT_FACTORY_H_
#define GLOCAL_EXPLORATION_ROS_CONVERSIONS_ROS_COMPONENT_FACTORY_H_

#include <memory>
#include <string>

#include <ros/node_handle.h>

#include "glocal_exploration/mapping/map_base.h"
#include "glocal_exploration/planning/local_planner/local_planner_base.h"
#include "glocal_exploration/planning/state_machine.h"

namespace glocal_exploration {

class ComponentFactoryROS {
 public:
  virtual ~ComponentFactoryROS() = default;

  static std::shared_ptr<MapBase> createMap(const ros::NodeHandle &nh, std::shared_ptr<StateMachine> state_machine);

  static std::unique_ptr<LocalPlannerBase> createLocalPlanner(const ros::NodeHandle &nh,
                                                              std::shared_ptr<MapBase> map,
                                                              std::shared_ptr<StateMachine> state_machine);

 private:
  ComponentFactoryROS() = default;
  static std::string getType(const ros::NodeHandle &nh);
};

} // namespace glocal_exploration

#endif // GLOCAL_EXPLORATION_ROS_CONVERSIONS_ROS_COMPONENT_FACTORY_H_