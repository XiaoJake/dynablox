#include "lidar_motion_detection/evaluation/evaluator.h"

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <string>
#include <vector>

#include <voxblox/utils/timing.h>

namespace motion_detection {

const std::string Evaluator::ranges_file_name_ = "ranges.csv";
const std::string Evaluator::scores_file_name_ = "scores.csv";
const std::string Evaluator::timings_file_name_ = "timings.txt";

void Evaluator::Config::checkParams() const {
  checkParamCond(!output_directory.empty(), "'output_directory' must be set.");
  checkParamGE(min_range, 0.f, "min_range");
  checkParamCond(max_range > min_range,
                 "'max_range' must be larger than 'min_range'.");
  checkParamConfig(ground_truth_config);
}

void Evaluator::Config::setupParamsAndPrinting() {
  setupParam("output_directory", &output_directory);
  setupParam("min_range", &min_range);
  setupParam("max_range", &max_range);
  setupParam("evaluate_point_level", &evaluate_point_level);
  setupParam("evaluate_cluster_level", &evaluate_cluster_level);
  setupParam("evaluate_object_level", &evaluate_object_level);
  setupParam("evaluate_ranges", &evaluate_ranges);
  setupParam("ground_truth", &ground_truth_config, "ground_truth");
}

Evaluator::Evaluator(const Config& config)
    : config_(config.checkValid()),
      ground_truth_handler(config_.ground_truth_config) {
  LOG(INFO) << "\n" << config_.toString();
  setupFiles();
}

void Evaluator::setupFiles() {
  output_directory_ = config_.output_directory;
  if (std::filesystem::exists(config_.output_directory)) {
    // Already exists, create a time-stamped dir instead.
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::stringstream timestamp;
    timestamp << std::put_time(&tm, "%Y_%m_%d-%H_%M_%S");
    output_directory_ = output_directory_ + "/" + timestamp.str();
  }
  std::filesystem::create_directories(output_directory_);
  LOG(INFO) << "Writing evaluation to '" << output_directory_ << ".";

  // Setup the header of the scores file.
  if (config_.evaluate_point_level) {
    evaluated_levels_.push_back("point");
  }
  if (config_.evaluate_cluster_level) {
    evaluated_levels_.push_back("cluster");
  }
  if (config_.evaluate_object_level) {
    evaluated_levels_.push_back("object");
  }

  // Setup scores file.
  std::ofstream writefile;
  writefile.open(output_directory_ + "/" + scores_file_name_, std::ios::trunc);
  writefile << "timestamp,";
  for (const std::string& level : evaluated_levels_) {
    writefile << level + "_IoU," << level + "_Precision," << level + "_Recall,"
              << level + "_TP," << level + "_TN," << level + "_FP,"
              << level + "_FN,";
  }
  writefile << "EvaluatedPoints,TotalPoints" << std::endl;
  writefile.close();

  // Initialize ranges.
  if (config_.evaluate_ranges) {
    ranges_ = std::vector<std::vector<float>>(4);
  }
}

void Evaluator::evaluateFrame(CloudInfo& cloud_info) {
  // Update the timings every frame.
  writeTimingsToFile();

  // If ground truth available, label the cloud and compute the metrics.
  if (ground_truth_handler.labelCloudInfoIfAvailable(cloud_info)) {
    writeScoresToFile(cloud_info);
    gt_frame_counter_++;
    LOG(INFO) << "Evaluated cloud " << gt_frame_counter_ << " with timestamp "
              << cloud_info.timestamp << ".";
  }
}

void Evaluator::writeTimingsToFile() const {
  // Overwrite the timings with the current statistics.
  std::ofstream writefile;
  writefile.open(output_directory_ + "/" + timings_file_name_, std::ios::trunc);
  writefile << voxblox::timing::Timing::Print() << std::endl;
  writefile.close();
}

void Evaluator::writeScoresToFile(CloudInfo& cloud_info) {
  std::ofstream writefile;
  writefile.open(output_directory_ + "/" + scores_file_name_, std::ios::app);

  // Time stamp and preprocessing.
  writefile << cloud_info.timestamp;
  const int evaluated_points = filterEvaluatedPoints(cloud_info);

  // Evaluated levels.
  for (const std::string& level : evaluated_levels_) {
    evaluateCloudAtLevel(cloud_info, level, writefile);
  }

  // Number of evaluated points.
  writefile << "," << evaluated_points << "," << cloud_info.points.size()
            << std::endl;

  // Evalaute ranges.
  if (config_.evaluate_ranges) {
    evaluateRanges(cloud_info);
  }
}

void Evaluator::evaluateRanges(const CloudInfo& cloud_info) {
  // Overwrite all ranges at each iteration with the collected data.
  std::ofstream writefile;
  writefile.open(output_directory_ + "/" + ranges_file_name_, std::ios::trunc);

  // Add all new data to the database.
  for (const PointInfo& point : cloud_info.points) {
    if (!point.ready_for_evaluation) {
      continue;
    }
    // TODO(schmluk): This could also be more general.
    const bool is_dynamic = point.cluster_level_dynamic;
    if (is_dynamic && point.ground_truth_dynamic) {
      ranges_[0].push_back(point.distance_to_sensor);
    } else if (is_dynamic && !point.ground_truth_dynamic) {
      ranges_[1].push_back(point.distance_to_sensor);
    } else if (!is_dynamic && !point.ground_truth_dynamic) {
      ranges_[2].push_back(point.distance_to_sensor);
    } else if (!is_dynamic && point.ground_truth_dynamic) {
      ranges_[3].push_back(point.distance_to_sensor);
    }
  }

  // Write to file.
  writefile << "TP";
  for (float tp : ranges_[0]) {
    writefile << "," << tp;
  }
  writefile << "\nFP";
  for (float fp : ranges_[1]) {
    writefile << "," << fp;
  }
  writefile << "\nTN";
  for (float tn : ranges_[1]) {
    writefile << "," << tn;
  }
  writefile << "\nFN";
  for (float fn : ranges_[1]) {
    writefile << "," << fn;
  }
  writefile << "\n";

  writefile.close();
}

int Evaluator::filterEvaluatedPoints(CloudInfo& cloud_info) const {
  int number_of_points = 0;
  for (PointInfo& point : cloud_info.points) {
    if (point.distance_to_sensor >= config_.min_range &&
        point.distance_to_sensor <= config_.max_range) {
      point.ready_for_evaluation = true;
      number_of_points++;
    }
  }
  return number_of_points;
}

void Evaluator::evaluateCloudAtLevel(const CloudInfo& cloud_info,
                                     const std::string& level,
                                     std::ofstream& output_file) const {
  // Setup.
  std::function<bool(const PointInfo&)> check_level;
  if (level == "point") {
    check_level = [](const PointInfo& point) {
      return point.ever_free_level_dynamic;
    };
  } else if (level == "cluster") {
    check_level = [](const PointInfo& point) {
      return point.cluster_level_dynamic;
    };
  } else if (level == "object") {
    check_level = [](const PointInfo& point) {
      return point.object_level_dynamic;
    };
  } else {
    LOG(ERROR) << "Unknown evaluation level '" << level << "'!";
    return;
  }

  // Compute true/false positives/negatives.
  uint tp = 0u;
  uint fp = 0u;
  uint tn = 0u;
  uint fn = 0u;
  for (const PointInfo& point : cloud_info.points) {
    if (!point.ready_for_evaluation) {
      continue;
    }
    const bool is_dynamic = check_level(point);
    if (is_dynamic && point.ground_truth_dynamic) {
      tp++;
    } else if (is_dynamic && !point.ground_truth_dynamic) {
      fp++;
    } else if (!is_dynamic && !point.ground_truth_dynamic) {
      tn++;
    } else if (!is_dynamic && point.ground_truth_dynamic) {
      fn++;
    }
  }

  // Write metrics to file.
  output_file << "," << computeIntersectionOverUnion(tp, fp, fn) << ","
              << computePrecision(tp, fp) << "," << computeRecall(tp, fn) << ","
              << tp << "," << tn << "," << fp << "," << fn;
}

float Evaluator::computePrecision(const uint tp, const uint fp) {
  if (tp + fp == 0u) {
    return 1.f;
  }
  return static_cast<float>(tp) / static_cast<float>(tp + fp);
}

float Evaluator::computeRecall(const uint tp, const uint fn) {
  if (tp + fn == 0u) {
    return 1.f;
  }
  return static_cast<float>(tp) / static_cast<float>(tp + fn);
}

float Evaluator::computeIntersectionOverUnion(const uint tp, const uint fp,
                                              const uint fn) {
  if (tp + fp + fn == 0u) {
    return 1.f;
  }
  return static_cast<float>(tp) / static_cast<float>(tp + fp + fn);
}

}  // namespace motion_detection