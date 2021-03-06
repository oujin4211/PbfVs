#include "CppUnitTest.h"
#include "stdafx.h"

#include "include/aabb.h"
#include "include/cuda_basic.h"
#include "include/helper_math.h"
#include "include/particle_system.h"
#include "include/sh_position_getter.h"
#include "include/shared_math.h"
#include "include/spatial_hash.h"

#include "include/pbf_solver_gpu.h"

#include <sstream>
#include <string>
#include <thrust/execution_policy.h>
#include <thrust/host_vector.h>
#include <time.h> // time
#include <unordered_set>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// How to properly build the application project in the unit test project.
// http://stackoverflow.com/questions/19886397/how-to-solve-the-error-lnk2019-unresolved-external-symbol-function

namespace pbf {
namespace {
// The testing world is a cube of equal size in three dimensions.
// Each cube is consisted of a series of cells. These cells are
// not the same thing as the cell in the data structure we are
// testing on.

const unsigned kNumPoints = 1000u;
// Cell size of the test world.
const float kCellSize = 1.0f;
const float kHalfCellSize = kCellSize / 2;
const int kNumCellsPerDim = 15;
const float kWorldSize = kCellSize * kNumCellsPerDim;
const int kAabbOffsetByCell = 3;
const int kNumIters = 100;
// Cell size of the data structure being tested.
const float kTestDsCellSize = 1.5f;

point_t GenRandomPoint() {
  int x = GenRandom(0, kNumCellsPerDim);
  int y = GenRandom(0, kNumCellsPerDim);
  int z = GenRandom(0, kNumCellsPerDim);

  point_t result;
  result.x = x * kCellSize + kHalfCellSize;
  result.y = y * kCellSize + kHalfCellSize;
  result.z = z * kCellSize + kHalfCellSize;
  return result;
}

AABB GetQueryAABB() {
  point_t kAabbMin{kCellSize * kAabbOffsetByCell};
  point_t kAabbMax{kCellSize * (kNumCellsPerDim - kAabbOffsetByCell)};
  AABB aabb{kAabbMin, kAabbMax};
  return aabb;
}

int Reduce(const d_vector<int> &d_vec) {
  thrust::host_vector<int> h_vec{d_vec};
  int result = 0;
  for (int i : h_vec)
    result += i;
  return result;
}
} // namespace

TEST_CLASS(SpatialHashTest) {
public:
  SpatialHashTest() : query_aabb_(GetQueryAABB()) {}

  TEST_METHOD(TestSpatialHashCorrect) {
    srand(time(nullptr));
    Init();

    for (int iter = 0; iter < kNumIters; ++iter) {
      TestShOneIter();
    }
  }

private:
  void TestShOneIter() {
    RandomScatterPoints();
    spatial_hash_.UpdateAll();
    auto query_result = spatial_hash_.Query(query_aabb_);

    std::stringstream ss;
    ss << "Query result size: " << query_result.size();
    auto log_str = ss.str();
    Logger::WriteMessage(log_str.c_str());
    // Assert::AreEqual(query_result.size(), num_inside_aabb_ref_);
    Assert::AreEqual(query_result.size(), ptcs_inside_aabb_ref_.size());
    for (size_t ptc_i : query_result) {
      Assert::IsTrue(ptcs_inside_aabb_ref_.count(ptc_i) == 1);
    }
  }

  void Init() {
    // init particle system
    for (unsigned i = 0; i < kNumPoints; ++i) {
      ps_.Add(point_t{0.0f}, point_t{0.0f});
    }

    // init spatial hash
    spatial_hash_.set_cell_size(0.583f);
    PositionGetter pg{&ps_};
    spatial_hash_.set_pos_getter(pg);
    spatial_hash_.Clear();
    for (size_t i = 0; i < ps_.NumParticles(); ++i) {
      spatial_hash_.Add(i);
    }
  }

  void RandomScatterPoints() {
    Assert::AreEqual(kNumPoints, (unsigned)ps_.NumParticles());

    ptcs_inside_aabb_ref_.clear();
    for (unsigned ptc_i = 0; ptc_i < kNumPoints; ++ptc_i) {
      auto ptc = ps_.Get(ptc_i);
      auto pos = GenRandomPoint();
      ptc.set_position(pos);
      if (query_aabb_.Contains(pos)) {
        ptcs_inside_aabb_ref_.insert(ptc_i);
      }
    }
  }

  AABB query_aabb_;
  ParticleSystem ps_;
  SpatialHash<size_t, PositionGetter> spatial_hash_;
  std::unordered_set<size_t> ptcs_inside_aabb_ref_;
};

TEST_CLASS(CellGridGpuTest) {
public:
  CellGridGpuTest() : query_aabb_(GetQueryAABB()) {}

  TEST_METHOD(TestCellGridGpu) {
    srand(time(nullptr));

    for (int iter = 0; iter < kNumIters; ++iter) {
      TestCellGridGpuOneIter();
    }
  }

private:
  void TestCellGridGpuOneIter() {
    using thrust::host_vector;

    std::vector<float3> h_positions;
    RandomScatterPoints(&h_positions);

    d_vector<float3> d_positions{h_positions};
    float3 world_sz_dim = make_float3(kWorldSize);
    CellGridGpu cell_grid{world_sz_dim, kTestDsCellSize};

    UpdateCellGrid(d_positions, &cell_grid);

#if 0
			// More verbosed test
			host_vector<int> h_ptc_to_cell{ cell_grid.ptc_to_cell };
			host_vector<int> h_cell_to_active_cell_indices{
				cell_grid.cell_to_active_cell_indices };
			host_vector<int> h_ptc_begins_in_active_cell{
				cell_grid.ptc_begins_in_active_cell };
			host_vector<int> h_ptc_offsets_within_cell{ 
				cell_grid.ptc_offsets_within_cell };
			host_vector<int> h_cell_ptc_indices{ cell_grid.cell_ptc_indices };
			for (int ptc_i = 0; ptc_i < kNumPoints; ++ptc_i) {
				int cell_i = h_ptc_to_cell[ptc_i];
				int ac_idx = h_cell_to_active_cell_indices[cell_i];
				int ac_ptc_begin = h_ptc_begins_in_active_cell[ac_idx];
				int offs = h_ptc_offsets_within_cell[ptc_i];
				int ptc_i_prime = h_cell_ptc_indices[ac_ptc_begin + offs];
				Assert::AreEqual(ptc_i, ptc_i_prime);
			}
#endif
    d_vector<int> cell_num_ptcs_inside;
    Query(d_positions, cell_grid, query_aabb_, &cell_num_ptcs_inside);
    int num_ptcs_inside = Reduce(cell_num_ptcs_inside);
    std::stringstream ss;
    ss << "Query ref size: " << ptcs_inside_aabb_ref_.size()
       << ", cuda computed size: " << num_ptcs_inside;
    auto log_str = ss.str();
    Logger::WriteMessage(log_str.c_str());
    ss.str("");
    Assert::AreEqual(num_ptcs_inside, (int)ptcs_inside_aabb_ref_.size());
  }

  void RandomScatterPoints(std::vector<float3> * positions) {
    positions->clear();
    ptcs_inside_aabb_ref_.clear();

    for (unsigned ptc_i = 0; ptc_i < kNumPoints; ++ptc_i) {
      auto pos = GenRandomPoint();
      positions->push_back(Convert(pos));
      if (query_aabb_.Contains(pos)) {
        ptcs_inside_aabb_ref_.insert(ptc_i);
      }
    }
  }

  AABB query_aabb_;
  std::unordered_set<size_t> ptcs_inside_aabb_ref_;
};

TEST_CLASS(FindNeighborsTest) {
private:
  const float kWorldSize = 4.0f;
  const float kH = 3.2f;
  const float kTestDsCellSize = kH + 0.5f;

  float3 init_ptc_;
  std::unordered_set<size_t> neighbor_ptcs_ref_;

  float3 GenRandomPos() const {
    float x = GenRandom(0.1f, this->kWorldSize - 0.1f);
    float y = GenRandom(0.1f, this->kWorldSize - 0.1f);
    float z = GenRandom(0.1f, this->kWorldSize - 0.1f);
    return make_float3(x, y, z);
  };

  void InitPositions(std::vector<float3> * positions) {
    auto DistSqr = [](const float3 &a, const float3 &b) -> float {
      float x = (a.x - b.x);
      float y = (a.y - b.y);
      float z = (a.z - b.z);
      float result = x * x + y * y + z * z;
      return result;
    };

    const float h_sqr = kH * kH;
    init_ptc_ = GenRandomPos();
    positions->push_back(init_ptc_);
    for (size_t i = 1; i < 30; ++i) {
      float3 ptc = GenRandomPos();
      positions->push_back(ptc);
      if (DistSqr(ptc, init_ptc_) < h_sqr) {
        neighbor_ptcs_ref_.insert(i);
      }
    }
  }

  void TestOneIter() {
    std::vector<float3> h_positions;
    neighbor_ptcs_ref_.clear();
    InitPositions(&h_positions);

    d_vector<float3> d_positions{h_positions};
    float3 world_sz_dim = make_float3(this->kWorldSize);
    CellGridGpu cell_grid{world_sz_dim, this->kTestDsCellSize};

    UpdateCellGrid(d_positions, &cell_grid);

    GpuParticleNeighbors pn;
    FindParticleNeighbors(d_positions, cell_grid, kH, &pn);
    thrust::host_vector<int> h_ptc_num_neighbors{pn.ptc_num_neighbors};
    const int num_neigbors_ref = neighbor_ptcs_ref_.size();
    std::stringstream ss;
    ss << "Num neighbors ref size: " << num_neigbors_ref
       << ", cuda computed size: " << h_ptc_num_neighbors[0] << std::endl;
    auto log_str = ss.str();
    Logger::WriteMessage(log_str.c_str());
    Assert::AreEqual(num_neigbors_ref, h_ptc_num_neighbors[0]);
  }

public:
  TEST_METHOD(TestFindNeighbors) {
    srand(time(nullptr));

    for (int iter = 0; iter < kNumIters; ++iter) {
      TestOneIter();
    }
  }
};
} // namespace pbf
