//========================================================================
//  This software is free: you can redistribute it and/or modify
//  it under the terms of the GNU Lesser General Public License Version 3,
//  as published by the Free Software Foundation.
//
//  This software is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public License
//  Version 3 in the file COPYING that came with this distribution.
//  If not, see <http://www.gnu.org/licenses/>.
//========================================================================
/*!
\file    slam_frontend.cc
\brief   A vision SLAM frontend
\author  Joydeep Biswas, (C) 2018
*/
//========================================================================

#include <stdlib.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "eigen3/Eigen/Dense"
#include "eigen3/Eigen/Geometry"
#include "opencv2/opencv.hpp"
#include "opencv2/features2d.hpp"
#include "opencv2/imgproc.hpp"
#include "opencv2/xfeatures2d.hpp"
#include "glog/logging.h"
#include "nav_msgs/Odometry.h"

#include "slam_frontend.h"

using nav_msgs::Odometry;
using slam_types::VisionFeature;
using std::string;
using std::vector;
using Eigen::Quaternionf;
using Eigen::Vector3f;

/* --- Frontend Implementation Code --- */

namespace slam {

cv::Mat CreateDebugImage(const Frame& frame_one,
                         const Frame& frame_two,
                         const slam_types::VisionCorrespondence& corr) {
  cv::Mat return_image = frame_one.debug_image_.clone();
  cv::cvtColor(return_image, return_image, cv::COLOR_GRAY2RGB);
  for (auto c : corr.feature_matches) {
    cv::circle(return_image,
               frame_two.keypoints_[c.pose_i_id].pt,
               5, CV_RGB(255, 0, 0));
    cv::line(return_image,
             frame_two.keypoints_[c.pose_i_id].pt,
             frame_one.keypoints_[c.pose_j_id].pt,
             CV_RGB(0, 255, 0));
  }
  return return_image;
}

bool Frontend::OdomCheck() {
  if (!odom_initialized_) return false;
  if ((prev_odom_translation_ - odom_translation_).norm() >
      config_.min_odom_translation) {
    return true;
  }
  if (prev_odom_rotation_.angularDistance(odom_rotation_) >
      config_.min_odom_rotation) {
    return true;
    }
  return false;
}

Frontend::Frontend (const string& config_path) {
  last_slam_odom_.pose.pose.position = geometry_msgs::Point();
  fast_feature_detector_ = cv::FastFeatureDetector::create(10, true);
  switch (config_.descriptor_extract_type_) {
    case FrontendConfig::DescriptorExtractorType::AKAZE:
      descriptor_extractor_ = cv::AKAZE::create(cv::AKAZE::DESCRIPTOR_MLDB,
                                                0, 3, 0.0001f, 10, 5,
                                                cv::KAZE::DIFF_PM_G2);
      bf_matcher_param_ = cv::NORM_HAMMING;
      break;
    case FrontendConfig::DescriptorExtractorType::ORB:
      descriptor_extractor_ = cv::ORB::create(10000, 1.04f, 50, 31, 0, 2,
                                              cv::ORB::HARRIS_SCORE,
                                              31, 20);
      bf_matcher_param_ = cv::NORM_HAMMING;
      break;
    case FrontendConfig::DescriptorExtractorType::BRISK:
      descriptor_extractor_ = cv::BRISK::create(20, 7, 1.1f);
      bf_matcher_param_ = cv::NORM_HAMMING;
      break;
    case FrontendConfig::DescriptorExtractorType::SURF:
      descriptor_extractor_ = cv::xfeatures2d::SURF::create();
      bf_matcher_param_ = cv::NORM_L2;
      break;
    case FrontendConfig::DescriptorExtractorType::SIFT:
      descriptor_extractor_ = cv::xfeatures2d::SIFT::create();
      bf_matcher_param_ = cv::NORM_L2;
      break;
    case FrontendConfig::DescriptorExtractorType::FREAK:
      descriptor_extractor_ = cv::xfeatures2d::FREAK::create(false, true,
                                                             40.0f, 20);
      bf_matcher_param_ = cv::NORM_HAMMING;
      break;
    default:
      LOG(ERROR) << "Could not recognize descriptor extractor option.";
      exit(1);
  }
}

void Frontend::ObserveOdometry(const Vector3f& translation,
                               const Quaternionf& rotation,
                               double timestamp) {
  odom_translation_ = translation;
  odom_rotation_ = rotation;
  odom_timestamp_ = timestamp;
}

void Frontend::ObserveImage(const cv::Mat& image,
                                  double time,
                                  const nav_msgs::Odometry& odom_msg) {
  // Check from the odometry if its time to run SLAM
  if (!OdomCheck()) {
    curr_frame_ID_++;
    return;
  } else {
    curr_frame_ID_++;
    last_slam_odom_ = odom_msg;
  }
  std::vector<cv::KeyPoint> frame_keypoints;
  cv::Mat frame_descriptors;
  if (config_.descriptor_extract_type_ ==
      FrontendConfig::DescriptorExtractorType::FREAK) {
    fast_feature_detector_->detect(image, frame_keypoints);
    descriptor_extractor_->compute(image, frame_keypoints, frame_descriptors);
  } else {
    descriptor_extractor_->detectAndCompute(image,
                                            cv::noArray(),
                                            frame_keypoints,
                                            frame_descriptors);
  }
  Frame curr_frame(frame_keypoints,
                   frame_descriptors,
                   config_,
                   curr_frame_ID_);
  if (config_.debug_images_) {
    curr_frame.debug_image_ = image;
  }
  for (uint32_t frame_num = 0; frame_num < frame_list_.size(); frame_num++) {
    std::vector<slam_types::VisionCorrespondencePair> pairs;
    Frame& past_frame = frame_list_[frame_num];
    std::vector<cv::DMatch> matches =
        curr_frame.GetMatches(past_frame, config_.nn_match_ratio_);
    std::sort(matches.begin(), matches.end());
    const int num_good_matches = matches.size() * config_.best_percent_;
    matches.erase(matches.begin() + num_good_matches, matches.end());
    // Restructure matches, add all keypoints to new list.
    for (auto match : matches) {
      std::pair<uint64_t, uint64_t> initial = past_frame.GetInitialFrame(match);
      // Add it to the original frame.
      curr_frame.AddMatchInitial(match, initial);
      pairs.push_back(CreateVisionPair(match.trainIdx,
                                       match.queryIdx,
                                       initial.first,
                                       initial.second));
    }
    correspondences_.push_back(CreateVisionCorrespondence(past_frame.frame_ID_,
                                                         curr_frame.frame_ID_,
                                                         pairs));
  }
  std::vector<slam_types::VisionFeature> features;
  for (uint64_t i = 0; i < curr_frame.keypoints_.size(); i++) {
    features.push_back(CreateVisionFeature(i, curr_frame.keypoints_[i].pt));
  }
  nodes_.push_back(CreateSLAMNode(curr_frame.frame_ID_, features, odom_msg));
  if (config_.debug_images_ && !frame_list_.empty()) {
    debug_images_.push_back(CreateDebugImage(curr_frame,
                                             frame_list_.back(),
                                             correspondences_.back()));
  }
  if (frame_list_.size() >= config_.frame_life_) {
    frame_list_.erase(frame_list_.begin());
  }
  frame_list_.push_back(curr_frame);
}

std::vector<slam_types::VisionCorrespondence>
Frontend::getCorrespondences() {
  return correspondences_;
}

std::vector<slam_types::SLAMNode>
Frontend::getSLAMNodes() {
  return nodes_;
}

std::vector<cv::Mat>
Frontend::getDebugImages() {
  return debug_images_;
}

slam_types::VisionCorrespondencePair
Frontend::CreateVisionPair(uint64_t pose_i_idx,
                                 uint64_t pose_j_idx,
                                 uint64_t pose_initial,
                                 uint64_t pose_initial_idx) {
  slam_types::VisionCorrespondencePair pair;
  pair.pose_i_id = pose_i_idx;
  pair.pose_j_id = pose_j_idx;
  pair.pose_initial = pose_initial;
  pair.pose_initial_id = pose_initial_idx;
  return pair;
}

slam_types::VisionCorrespondence
Frontend::CreateVisionCorrespondence(uint64_t pose_i,
                                     uint64_t pose_j,
                                     const
std::vector<slam_types::VisionCorrespondencePair> pairs) {
  slam_types::VisionCorrespondence corr;
  corr.pose_i = pose_i;
  corr.pose_j = pose_j;
  corr.feature_matches = pairs;
  return corr;
}

slam_types::VisionFeature
Frontend::CreateVisionFeature(uint64_t id, cv::Point2f pixel) {
  slam_types::VisionFeature vis;
  vis.id = id;
  vis.descriptor_id = 0;
  vis.pixel = Vector2f(pixel.x, pixel.y);
  return vis;
}

slam_types::SLAMNode
Frontend::CreateSLAMNode(uint64_t pose_i,
                               const vector<VisionFeature>&  features,
                               const Odometry& odom_msg) {
  slam_types::SLAMNode node;
  node.id = pose_i;
  node.is_vision_node = true;
  node.features = features;
  auto loc = odom_msg.pose.pose.position;
  auto orient = odom_msg.pose.pose.orientation;
  printf("%s:%d TODO: Pose timestamp\n", __FILE__, __LINE__);
  node.pose = slam_types::RobotPose(0,
    Vector3f(loc.x, loc.y, loc.z),
                                    AngleAxisf(Quaternionf(orient.w,
                                                           orient.x,
                                                           orient.y,
                                                           orient.z)));
  return node;
}

/* --- Frame Implementation Code --- */

Frame::Frame(const std::vector<cv::KeyPoint>& keypoints,
                   const cv::Mat& descriptors,
                   const FrontendConfig& config,
                   uint64_t frame_ID) {
  keypoints_ = keypoints;
  descriptors_ = descriptors;
  config_ = config;
  frame_ID_ = frame_ID;
  matcher_ = cv::BFMatcher::create(config_.bf_matcher_param_);
}

std::vector<cv::DMatch> Frame::GetMatches(const Frame& frame,
                                                double nn_match_ratio) {
  std::vector<std::vector<cv::DMatch>> matches;
  matcher_->knnMatch(descriptors_, frame.descriptors_, matches, 2);
  std::vector<cv::DMatch> best_matches;
  for (size_t i = 0; i < matches.size(); i++) {
    cv::DMatch first = matches[i][0];
    float dist1 = matches[i][0].distance;
    float dist2 = matches[i][1].distance;
    if (dist1 < config_.nn_match_ratio_ * dist2) {
      best_matches.push_back(first);
    }
  }
  return best_matches;
}

std::pair<uint64_t, uint64_t> Frame::GetInitialFrame(cv::DMatch match) {
  auto result = initial_appearances.find(match.trainIdx);
  if (result == initial_appearances.end()) {
    // We didn't find it in the database, this must be the first time then.
    auto first_app = std::pair<uint64_t, uint64_t>(frame_ID_, match.trainIdx);
    initial_appearances.insert({match.trainIdx, first_app});
    return first_app;
  }
  return result->second;
}

void Frame::AddMatchInitial(cv::DMatch match,
                                  std::pair<uint64_t, uint64_t> initial) {
  initial_appearances.insert({match.queryIdx, initial});
}

/* --- Config Implementation Code --- */

FrontendConfig::FrontendConfig() {
  // Load Default values
  debug_images_ = true;
  descriptor_extract_type_ = FrontendConfig::DescriptorExtractorType::AKAZE;
  best_percent_ = 0.3f;
  nn_match_ratio_ = 0.8f;
  frame_life_ = 5;
  bf_matcher_param_ = cv::NORM_HAMMING;
}

}  // namespace slam
