#include <memory>
#include <vector>

#include <Eigen/Dense>
#include <glog/logging.h>

#include <aslam/cameras/camera.h>
#include <aslam/common/memory.h>
#include <aslam/common/statistics/statistics.h>
#include <aslam/common-private/feature-descriptor-ref.h>
#include <aslam/frames/visual-frame.h>
#include <aslam/tracker/feature-tracker-gyro.h>
#include <aslam/tracker/feature-tracker-gyro-matching-data.h>
#include <aslam/tracker/tracking-helpers.h>

namespace aslam {

GyroTracker::GyroTracker(const Camera& camera)
    : camera_(camera) {}

void GyroTracker::track(const Quaternion& q_Ckp1_Ck,
                        const VisualFrame& frame_k,
                        VisualFrame* frame_kp1,
                        MatchesWithScore* matches_with_score_kp1_k) {
  CHECK(frame_k.hasKeypointMeasurements());
  CHECK(CHECK_NOTNULL(frame_kp1)->hasKeypointMeasurements());
  CHECK_EQ(camera_.getId(), CHECK_NOTNULL(frame_k.getCameraGeometry().get())->getId());
  CHECK_EQ(camera_.getId(), CHECK_NOTNULL(frame_kp1->getCameraGeometry().get())->getId());
  CHECK_NOTNULL(matches_with_score_kp1_k)->clear();
  CHECK(frame_k.hasTrackIds());
  CHECK(frame_kp1->hasTrackIds());
  // Make sure the frames are in order time-wise
  CHECK_GT(frame_kp1->getTimestampNanoseconds(), frame_k.getTimestampNanoseconds());
  // Check that the required data is available in the frame
  CHECK(frame_kp1->hasDescriptors());
  CHECK_EQ(frame_kp1->getDescriptors().rows(), frame_kp1->getDescriptorSizeBytes());
  CHECK_EQ(frame_kp1->getKeypointMeasurements().cols(), frame_kp1->getDescriptors().cols());

  // Match the descriptors of frame (k+1) with those of frame k.
  matchFeatures(q_Ckp1_Ck, *frame_kp1, frame_k, matches_with_score_kp1_k);
}

void GyroTracker::matchFeatures(const Quaternion& q_Ckp1_Ck,
                                const VisualFrame& frame_kp1,
                                const VisualFrame& frame_k,
                                MatchesWithScore* matches_with_score_kp1_k) const {
  CHECK_NOTNULL(matches_with_score_kp1_k);
  matches_with_score_kp1_k->clear();

  GyroTrackerMatchingData matching_data(
      q_Ckp1_Ck, frame_kp1, frame_k);

  // corner_row_LUT[i] is the number of keypoints that has an y position
  // lower than i in the image.
  std::vector<int> corner_row_LUT;
  const uint32_t image_height = camera_.imageHeight();
  corner_row_LUT.reserve(image_height);
  int v = 0;
  for (size_t y = 0; y < image_height; ++y) {
    while (v < static_cast<int>(matching_data.num_points_kp1) &&
        y > matching_data.keypoints_kp1_by_y[v].measurement(1)) {
      ++v;
    }
    corner_row_LUT.push_back(v);
  }
  CHECK_EQ(static_cast<int>(corner_row_LUT.size()), image_height);

  const static unsigned int kdescriptorSizeBytes = matching_data.descriptor_size_bytes_;
  // usually binary descriptors size is less or equal to 512 bits.
  CHECK_LE(kdescriptorSizeBytes * 8, 512);
  std::function<unsigned int(const unsigned char*, const unsigned char*)> hammingDistance512 =
      [kdescriptorSizeBytes](const unsigned char* x, const unsigned char* y)->unsigned int {
        unsigned int distance = 0;
        for(unsigned int i = 0; i < kdescriptorSizeBytes; i++) {
          unsigned char val = *(x + i) ^ *(y + i);
          while(val) {
            ++distance;
            val &= val - 1;
          }
        }
        CHECK_LE(distance, kdescriptorSizeBytes * 8);
        return distance;
      };

  matches_with_score_kp1_k->reserve(matching_data.num_points_k);

  // Keep track of matched keypoints of frame (k+1) such that they
  // are not matched again.
  // TODO(magehrig): Improve this by allowing duplicate matches
  // and discarding duplicate matches according to descriptor distances.
  std::vector<bool> is_keypoint_kp1_matched;
  is_keypoint_kp1_matched.resize(matching_data.num_points_kp1, false);

  for (int i = 0; i < matching_data.num_points_k; ++i) {
    Eigen::Matrix<double, 2, 1> predicted_keypoint_position_kp1 =
        matching_data.predicted_keypoint_positions_kp1.block<2, 1>(0, i);
    const unsigned char* const descriptor_k = frame_k.getDescriptor(i);

    std::function<int(int, int, int)> clamp = [](int lower, int upper, int in) {
      return std::min<int>(std::max<int>(in, lower), upper);
    };

    // Get search area for LUT iterators (rowwise).
    int idxnearest[2];  // Min search region.
    idxnearest[0] = clamp(0, image_height - 1, predicted_keypoint_position_kp1(1) + 0.5 - kSmallSearchDistance);
    idxnearest[1] = clamp(0, image_height - 1, predicted_keypoint_position_kp1(1) + 0.5 + kSmallSearchDistance);
    int idxnear[2];  // Max search region.
    idxnear[0] = clamp(0, image_height - 1, predicted_keypoint_position_kp1(1) + 0.5 - kLargeSearchDistance);
    idxnear[1] = clamp(0, image_height - 1, predicted_keypoint_position_kp1(1) + 0.5 + kLargeSearchDistance);

    CHECK_LE(idxnearest[0], idxnearest[1]);
    CHECK_LE(idxnear[0], idxnear[1]);

    CHECK_GE(idxnearest[0], 0);
    CHECK_GE(idxnearest[1], 0);
    CHECK_GE(idxnear[0], 0);
    CHECK_GE(idxnear[1], 0);
    CHECK_LT(idxnearest[0], image_height);
    CHECK_LT(idxnearest[1], image_height);
    CHECK_LT(idxnear[0], image_height);
    CHECK_LT(idxnear[1], image_height);

    int nearest_top = std::min<int>(idxnearest[0], image_height - 1);
    int nearest_bottom = std::min<int>(idxnearest[1] + 1, image_height - 1);
    int near_top = std::min<int>(idxnear[0], image_height - 1);
    int near_bottom = std::min<int>(idxnear[1] + 1, image_height - 1);

    // Get corners in this area.
    typedef typename Aligned<std::vector, KeypointData>::type::const_iterator KeyPointIterator;
    KeyPointIterator nearest_corners_begin = matching_data.keypoints_kp1_by_y.begin() + corner_row_LUT[nearest_top];
    KeyPointIterator nearest_corners_end = matching_data.keypoints_kp1_by_y.begin() + corner_row_LUT[nearest_bottom];
    KeyPointIterator near_corners_begin = matching_data.keypoints_kp1_by_y.begin() + corner_row_LUT[near_top];
    KeyPointIterator near_corners_end = matching_data.keypoints_kp1_by_y.begin() + corner_row_LUT[near_bottom];

    // Get descriptors and match.
    bool found = false;
    int n_processed_corners = 0;
    KeyPointIterator it_best;
    const static unsigned int kdescriptorSizeBits = kdescriptorSizeBytes*8;
    int best_score = static_cast<int>(kdescriptorSizeBits*kMatchingThresholdBitsRatio);
    // Keep track of processed corners s.t. we don't process them again in the
    // large window.
    std::vector<bool> processed_corners_kp1;
    processed_corners_kp1.resize(matching_data.num_points_kp1, false);

    const int bound_left_nearest = predicted_keypoint_position_kp1(0) - kSmallSearchDistance;
    const int bound_right_nearest = predicted_keypoint_position_kp1(0) + kSmallSearchDistance;

    // First search small window.
    for (KeyPointIterator it = nearest_corners_begin; it != nearest_corners_end; ++it) {
      if (it->measurement(0) < bound_left_nearest
          || it->measurement(0) > bound_right_nearest) {
        continue;
      }
      if (is_keypoint_kp1_matched.at(it->index)) {
        continue;
      }

      CHECK_LT(it->index, matching_data.num_points_kp1);
      CHECK_GE(it->index, 0u);
      const unsigned char* const descriptor_kp1 = frame_kp1.getDescriptor(it->index);
      int current_score = kdescriptorSizeBits - hammingDistance512(descriptor_k, descriptor_kp1);
      if (current_score > best_score) {
        best_score = current_score;
        it_best = it;
        found = true;
        CHECK_LT((predicted_keypoint_position_kp1 - it_best->measurement).norm(), kSmallSearchDistance * 2);
      }
      processed_corners_kp1[it->index] = true;
      ++n_processed_corners;
    }

    // If no match in small window, increase window and search again.
    if (!found) {
      const int bound_left_near = predicted_keypoint_position_kp1(0) - kLargeSearchDistance;
      const int bound_right_near = predicted_keypoint_position_kp1(0) + kLargeSearchDistance;

      for (KeyPointIterator it = near_corners_begin; it != near_corners_end; ++it) {
        if (processed_corners_kp1[it->index] || is_keypoint_kp1_matched.at(it->index)) {
          continue;
        }
        if (it->measurement(0) < bound_left_near || it->measurement(0) > bound_right_near) {
          continue;
        }
        CHECK_LT(it->index, matching_data.num_points_kp1);
        CHECK_GE(it->index, 0);
        const unsigned char* const descriptor_kp1 = frame_kp1.getDescriptor(it->index);
        int current_score = kdescriptorSizeBits - hammingDistance512(descriptor_k, descriptor_kp1);
        if (current_score > best_score) {
          best_score = current_score;
          it_best = it;
          found = true;
          CHECK_LT((predicted_keypoint_position_kp1 - it_best->measurement).norm(), kLargeSearchDistance * 2);
        }
        processed_corners_kp1[it->index] = true;
        ++n_processed_corners;
      }
    }

    if (found) {
      is_keypoint_kp1_matched.at(it_best->index) = true;
      // TODO(magehrig): Replace keypoints score with descriptor distance score.
      matches_with_score_kp1_k->emplace_back(
          static_cast<int>(it_best->index), i, 0.0);
      aslam::statistics::StatsCollector stats_distance_match("GyroTracker match bits");
      stats_distance_match.AddSample(best_score);
    } else {
      aslam::statistics::StatsCollector stats_distance_no_match("GyroTracker no-match num_checked");
      stats_distance_no_match.AddSample(n_processed_corners);
    }
  }
}

/*
inline double computeMatchScore(int hamming_distance) {
  return static_cast<double>(384 - hamming_distance) / 384.0;
}

inline int computeHammingDistance(int banana_index, int apple_index) {
  CHECK_LT(apple_index, static_cast<int>(apple_descriptors_.size()))
      << "No descriptor for this apple.";
  CHECK_LT(banana_index, static_cast<int>(banana_descriptors_.size()))
      << "No descriptor for this banana.";
  CHECK_LT(apple_index, static_cast<int>(valid_apples_.size()))
      << "No valid flag for this apple.";
  CHECK_LT(banana_index, static_cast<int>(valid_bananas_.size()))
      << "No valid flag for this apple.";
  CHECK(valid_apples_[apple_index]) << "The given apple is not valid.";
  CHECK(valid_bananas_[banana_index]) << "The given banana is not valid.";

  const common::FeatureDescriptorConstRef& apple_descriptor =
      apple_descriptors_[apple_index];
  const common::FeatureDescriptorConstRef& banana_descriptor =
      banana_descriptors_[banana_index];

  CHECK_NOTNULL(apple_descriptor.data());
  CHECK_NOTNULL(banana_descriptor.data());

  return common::GetNumBitsDifferent(banana_descriptor, apple_descriptor);
}
*/

}  //namespace aslam
