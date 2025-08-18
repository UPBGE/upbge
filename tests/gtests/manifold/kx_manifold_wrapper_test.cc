/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file
 * \ingroup tests
 * 
 * Unit tests for KX_ManifoldWrapper architectural improvements.
 * Tests thread safety, error handling, and API consistency.
 */

#include "../testing/testing.h"

#ifdef WITH_MANIFOLD

#include "KX_ManifoldWrapper.h"

#include <thread>
#include <vector>
#include <chrono>
#include <atomic>

namespace blender::tests {

class ManifoldWrapperTest : public testing::Test {
 protected:
  void SetUp() override {
    wrapper = KX_ManifoldWrapper::Create();
    ASSERT_NE(wrapper, nullptr);
  }

  void TearDown() override {
    wrapper.reset();
  }

  // Helper method to create a simple cube mesh
  KX_ManifoldWrapper::MeshData CreateCubeMesh() {
    KX_ManifoldWrapper::MeshData mesh;
    
    // Simple cube vertices (8 vertices)
    mesh.vertices = {
      -1.0f, -1.0f, -1.0f,  // 0
       1.0f, -1.0f, -1.0f,  // 1
       1.0f,  1.0f, -1.0f,  // 2
      -1.0f,  1.0f, -1.0f,  // 3
      -1.0f, -1.0f,  1.0f,  // 4
       1.0f, -1.0f,  1.0f,  // 5
       1.0f,  1.0f,  1.0f,  // 6
      -1.0f,  1.0f,  1.0f   // 7
    };
    
    // Cube faces (12 triangles)
    mesh.indices = {
      // Bottom face
      0, 1, 2,  0, 2, 3,
      // Top face  
      4, 7, 6,  4, 6, 5,
      // Front face
      0, 4, 5,  0, 5, 1,
      // Back face
      2, 6, 7,  2, 7, 3,
      // Left face
      0, 3, 7,  0, 7, 4,
      // Right face
      1, 5, 6,  1, 6, 2
    };
    
    return mesh;
  }

  // Helper method to create an invalid mesh
  KX_ManifoldWrapper::MeshData CreateInvalidMesh() {
    KX_ManifoldWrapper::MeshData mesh;
    // Empty mesh - should be invalid
    return mesh;
  }

  std::shared_ptr<KX_ManifoldWrapper> wrapper;
};

// Test basic functionality
TEST_F(ManifoldWrapperTest, BasicFunctionality) {
  EXPECT_NE(wrapper, nullptr);
  
  auto stats = wrapper->GetMeshStats(CreateCubeMesh());
  EXPECT_FALSE(stats.empty());
}

// Test mesh validation
TEST_F(ManifoldWrapperTest, MeshValidation) {
  auto valid_mesh = CreateCubeMesh();
  auto invalid_mesh = CreateInvalidMesh();
  
  EXPECT_TRUE(valid_mesh.IsValid());
  EXPECT_FALSE(invalid_mesh.IsValid());
  
  // Test validation through wrapper
  auto valid_result = wrapper->ValidateMesh(valid_mesh);
  EXPECT_TRUE(valid_result.IsSuccess());
  EXPECT_TRUE(valid_result.GetValue());
  
  auto invalid_result = wrapper->ValidateMesh(invalid_mesh);
  EXPECT_FALSE(invalid_result.IsSuccess());
  EXPECT_EQ(invalid_result.GetError(), ManifoldError::InvalidMesh);
}

// Test error handling infrastructure
TEST_F(ManifoldWrapperTest, ErrorHandling) {
  auto invalid_mesh = CreateInvalidMesh();
  
  // Test boolean operations with invalid mesh
  auto result = wrapper->PerformBooleanOperation(
    invalid_mesh, CreateCubeMesh(), KX_ManifoldWrapper::BooleanOperation::Union);
  
  EXPECT_FALSE(result.IsSuccess());
  EXPECT_NE(result.GetError(), ManifoldError::None);
  EXPECT_FALSE(result.GetErrorMessage().empty());
}

// Test simplification
TEST_F(ManifoldWrapperTest, MeshSimplification) {
  auto cube = CreateCubeMesh();
  
  auto result = wrapper->SimplifyMesh(cube, 0.1f);
  if (result.IsSuccess()) {
    auto simplified = result.GetValue();
    EXPECT_TRUE(simplified.IsValid());
    // Simplified mesh should still be valid
    EXPECT_GT(simplified.GetVertexCount(), 0);
  }
  
  // Test invalid tolerance
  auto invalid_result = wrapper->SimplifyMesh(cube, -1.0f);
  EXPECT_FALSE(invalid_result.IsSuccess());
  EXPECT_EQ(invalid_result.GetError(), ManifoldError::InvalidInput);
}

// Test boolean operations
TEST_F(ManifoldWrapperTest, BooleanOperations) {
  auto cube1 = CreateCubeMesh();
  auto cube2 = CreateCubeMesh();
  
  // Offset cube2 slightly to create interesting intersection
  for (size_t i = 0; i < cube2.vertices.size(); i += 3) {
    cube2.vertices[i] += 0.5f; // offset x coordinate
  }
  
  // Test union operation
  auto union_result = wrapper->PerformBooleanOperation(
    cube1, cube2, KX_ManifoldWrapper::BooleanOperation::Union);
  
  if (union_result.IsSuccess()) {
    auto result_mesh = union_result.GetValue();
    EXPECT_TRUE(result_mesh.IsValid());
    EXPECT_GT(result_mesh.GetVertexCount(), 0);
  }
}

// Test thread safety
TEST_F(ManifoldWrapperTest, ThreadSafety) {
  const int num_threads = 4;
  const int operations_per_thread = 10;
  std::atomic<int> successful_operations(0);
  std::vector<std::thread> threads;
  
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([this, &successful_operations, operations_per_thread]() {
      for (int j = 0; j < operations_per_thread; ++j) {
        auto cube = CreateCubeMesh();
        auto result = wrapper->ValidateMesh(cube);
        
        if (result.IsSuccess() && result.GetValue()) {
          successful_operations.fetch_add(1);
        }
        
        // Small delay to increase chance of concurrent access
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
    });
  }
  
  // Wait for all threads to complete
  for (auto& thread : threads) {
    thread.join();
  }
  
  // All operations should have succeeded
  EXPECT_EQ(successful_operations.load(), num_threads * operations_per_thread);
}

// Test error propagation in concurrent environment
TEST_F(ManifoldWrapperTest, ConcurrentErrorHandling) {
  const int num_threads = 4;
  std::atomic<int> error_count(0);
  std::vector<std::thread> threads;
  
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([this, &error_count]() {
      auto invalid_mesh = CreateInvalidMesh();
      auto result = wrapper->ValidateMesh(invalid_mesh);
      
      if (!result.IsSuccess()) {
        error_count.fetch_add(1);
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }
  
  // All threads should have detected the error
  EXPECT_EQ(error_count.load(), num_threads);
}

#ifdef WITH_TBB
// Test parallel mesh operations (if TBB is available)
TEST_F(ManifoldWrapperTest, ParallelMeshOperations) {
  std::vector<KX_ManifoldWrapper::MeshData> meshes;
  const int num_meshes = 8;
  
  // Create multiple cube meshes
  for (int i = 0; i < num_meshes; ++i) {
    meshes.push_back(CreateCubeMesh());
  }
  
  auto result = wrapper->ParallelMeshOperations(
    meshes, KX_ManifoldWrapper::BooleanOperation::Union, 0.1f);
  
  if (result.IsSuccess()) {
    auto processed_meshes = result.GetValue();
    EXPECT_EQ(processed_meshes.size(), num_meshes);
    
    for (const auto& mesh : processed_meshes) {
      EXPECT_TRUE(mesh.IsValid());
    }
  }
}

// Test parallel operations with invalid data
TEST_F(ManifoldWrapperTest, ParallelOperationsErrorHandling) {
  std::vector<KX_ManifoldWrapper::MeshData> meshes;
  
  // Mix valid and invalid meshes
  meshes.push_back(CreateCubeMesh());
  meshes.push_back(CreateInvalidMesh());  // This should cause failure
  meshes.push_back(CreateCubeMesh());
  
  auto result = wrapper->ParallelMeshOperations(
    meshes, KX_ManifoldWrapper::BooleanOperation::Union, 0.1f);
  
  // Should fail due to invalid mesh
  EXPECT_FALSE(result.IsSuccess());
  EXPECT_EQ(result.GetError(), ManifoldError::InvalidInput);
}
#endif

// Test move semantics and RAII
TEST_F(ManifoldWrapperTest, ResourceManagement) {
  {
    auto cube = CreateCubeMesh();
    auto moved_cube = std::move(cube);
    
    EXPECT_TRUE(moved_cube.IsValid());
    // Original cube should be in valid but unspecified state
  }
  
  // Test that wrapper manages resources properly
  auto another_wrapper = KX_ManifoldWrapper::Create();
  EXPECT_NE(another_wrapper, nullptr);
  
  // Wrapper should be safe to destroy (RAII)
  another_wrapper.reset();
}

// Test edge cases and boundary conditions
TEST_F(ManifoldWrapperTest, EdgeCases) {
  // Test empty mesh operations
  KX_ManifoldWrapper::MeshData empty_mesh;
  auto result = wrapper->ValidateMesh(empty_mesh);
  EXPECT_FALSE(result.IsSuccess());
  
  // Test mesh with invalid indices
  KX_ManifoldWrapper::MeshData invalid_indices_mesh;
  invalid_indices_mesh.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
  invalid_indices_mesh.indices = {0, 1, 10}; // Index 10 out of range
  
  EXPECT_FALSE(invalid_indices_mesh.IsValid());
  
  // Test simplification with extreme tolerance values
  auto cube = CreateCubeMesh();
  auto min_tolerance_result = wrapper->SimplifyMesh(cube, 0.0f);
  auto max_tolerance_result = wrapper->SimplifyMesh(cube, 1.0f);
  
  if (min_tolerance_result.IsSuccess()) {
    EXPECT_TRUE(min_tolerance_result.GetValue().IsValid());
  }
  if (max_tolerance_result.IsSuccess()) {
    EXPECT_TRUE(max_tolerance_result.GetValue().IsValid());
  }
}

// Test error message quality
TEST_F(ManifoldWrapperTest, ErrorMessages) {
  auto invalid_mesh = CreateInvalidMesh();
  
  auto result = wrapper->ValidateMesh(invalid_mesh);
  EXPECT_FALSE(result.IsSuccess());
  
  std::string error_msg = result.GetErrorMessage();
  EXPECT_FALSE(error_msg.empty());
  EXPECT_GT(error_msg.length(), 10); // Should be descriptive
  
  // Error message should contain useful information
  EXPECT_TRUE(error_msg.find("validation") != std::string::npos ||
              error_msg.find("invalid") != std::string::npos ||
              error_msg.find("mesh") != std::string::npos);
}

}  // namespace blender::tests

#endif  // WITH_MANIFOLD