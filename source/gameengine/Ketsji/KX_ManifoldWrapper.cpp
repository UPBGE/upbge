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

/** \file gameengine/Ketsji/KX_ManifoldWrapper.cpp
 *  \ingroup ketsji
 */

#ifdef WITH_MANIFOLD

#include "KX_ManifoldWrapper.h"

#include <manifold/manifold.h>
#include <iostream>
#include <limits>
#include <sstream>
#include <algorithm>
#include <cmath>

#ifdef WITH_TBB
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#endif

// Thread-local error storage
thread_local std::string KX_ManifoldWrapper::last_error_message_;

// MeshData validation implementation
bool KX_ManifoldWrapper::MeshData::IsValid() const {
    if (vertices.empty() || indices.empty()) {
        return false;
    }
    
    // Vertices must be divisible by 3 (x,y,z)
    if (vertices.size() % 3 != 0) {
        return false;
    }
    
    // Indices must be divisible by 3 (triangle faces)
    if (indices.size() % 3 != 0) {
        return false;
    }
    
    // Check for empty vertices after validation
    if (GetVertexCount() == 0) {
        return false;
    }
    
    // All indices must be valid (within vertex range)
    const size_t max_index = GetVertexCount() - 1;
    for (const int idx : indices) {
        if (idx < 0 || static_cast<size_t>(idx) > max_index) {
            return false;
        }
    }
    
    return true;
}

void KX_ManifoldWrapper::MeshData::Clear() {
    vertices.clear();
    indices.clear();
}

KX_ManifoldWrapper::KX_ManifoldWrapper() {
    // Constructor implementation
}

KX_ManifoldWrapper::~KX_ManifoldWrapper() {
    // Destructor implementation
}

// Helper methods implementation
ManifoldResult<KX_ManifoldWrapper::MeshData> KX_ManifoldWrapper::ConvertToMeshData(
    const manifold::MeshGL& mesh_gl) const {
    
    try {
        MeshData result;
        result.vertices = mesh_gl.vertProperties;
        result.indices.clear();
        result.indices.reserve(mesh_gl.triVerts.size());
        
        // Validate indices are within reasonable range before conversion
        for (uint32_t idx : mesh_gl.triVerts) {
            if (idx > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                return ManifoldResult<MeshData>(ManifoldError::InvalidMesh, 
                    "Index value too large for conversion to int");
            }
            result.indices.push_back(static_cast<int>(idx));
        }
        
        if (!result.IsValid()) {
            return ManifoldResult<MeshData>(ManifoldError::InvalidMesh, 
                "Converted mesh data is invalid");
        }
        
        return ManifoldResult<MeshData>(std::move(result));
    }
    catch (const std::exception& e) {
        LogError("ConvertToMeshData", e);
        return ManifoldResult<MeshData>(ManifoldError::OperationFailed, e.what());
    }
}

ManifoldResult<manifold::MeshGL> KX_ManifoldWrapper::ConvertFromMeshData(
    const MeshData& mesh_data) const {
    
    if (!mesh_data.IsValid()) {
        return ManifoldResult<manifold::MeshGL>(ManifoldError::InvalidInput, 
            "Input mesh data is invalid");
    }
    
    try {
        manifold::MeshGL mesh_gl;
        mesh_gl.numProp = 3; // xyz coordinates
        mesh_gl.vertProperties = mesh_data.vertices;
        mesh_gl.triVerts.reserve(mesh_data.indices.size());
        
        for (int idx : mesh_data.indices) {
            if (idx < 0) {
                return ManifoldResult<manifold::MeshGL>(ManifoldError::InvalidInput, 
                    "Negative index found in mesh data");
            }
            mesh_gl.triVerts.push_back(static_cast<uint32_t>(idx));
        }
        
        return ManifoldResult<manifold::MeshGL>(std::move(mesh_gl));
    }
    catch (const std::exception& e) {
        LogError("ConvertFromMeshData", e);
        return ManifoldResult<manifold::MeshGL>(ManifoldError::OperationFailed, e.what());
    }
}

void KX_ManifoldWrapper::LogError(const std::string& operation, const std::exception& e) const {
    std::stringstream ss;
    ss << "Manifold " << operation << " error: " << e.what();
    last_error_message_ = ss.str();
    std::cerr << "[KX_ManifoldWrapper] " << last_error_message_ << std::endl;
}

std::string KX_ManifoldWrapper::GetLastError() {
    return last_error_message_;
}

std::shared_ptr<KX_ManifoldWrapper> KX_ManifoldWrapper::Create() {
    return std::make_shared<KX_ManifoldWrapper>();
}

std::string KX_ManifoldWrapper::GetMeshStats(const MeshData& mesh) const {
    std::stringstream ss;
    ss << "Mesh Stats: ";
    ss << "Vertices=" << mesh.GetVertexCount() << ", ";
    ss << "Triangles=" << mesh.GetTriangleCount() << ", ";
    ss << "Valid=" << (mesh.IsValid() ? "Yes" : "No");
    return ss.str();
}

ManifoldResult<KX_ManifoldWrapper::MeshData> KX_ManifoldWrapper::PerformBooleanOperation(
    const MeshData& mesh1,
    const MeshData& mesh2,
    BooleanOperation op) const {
    
    std::lock_guard<std::mutex> lock(operation_mutex_);
    
    try {
        // Convert input meshes to Manifold format
        auto mesh1_gl_result = ConvertFromMeshData(mesh1);
        if (!mesh1_gl_result.IsSuccess()) {
            return ManifoldResult<MeshData>(mesh1_gl_result.GetError(), 
                "Failed to convert mesh1: " + mesh1_gl_result.GetErrorMessage());
        }
        
        auto mesh2_gl_result = ConvertFromMeshData(mesh2);
        if (!mesh2_gl_result.IsSuccess()) {
            return ManifoldResult<MeshData>(mesh2_gl_result.GetError(), 
                "Failed to convert mesh2: " + mesh2_gl_result.GetErrorMessage());
        }
        
        // Create Manifold objects
        manifold::Manifold manifold1(mesh1_gl_result.GetValue());
        manifold::Manifold manifold2(mesh2_gl_result.GetValue());
        
        // Validate manifolds
        if (manifold1.Status() != manifold::Manifold::Error::NoError) {
            return ManifoldResult<MeshData>(ManifoldError::ManifoldNotManifold, 
                "Mesh1 is not a valid manifold");
        }
        
        if (manifold2.Status() != manifold::Manifold::Error::NoError) {
            return ManifoldResult<MeshData>(ManifoldError::ManifoldNotManifold, 
                "Mesh2 is not a valid manifold");
        }
        
        // Perform boolean operation
        manifold::Manifold result;
        switch (op) {
            case BooleanOperation::Union:
                result = manifold1 + manifold2;
                break;
            case BooleanOperation::Intersection:
                result = manifold1 ^ manifold2;
                break;
            case BooleanOperation::Difference:
                result = manifold1 - manifold2;
                break;
            default:
                return ManifoldResult<MeshData>(ManifoldError::InvalidInput, 
                    "Invalid boolean operation");
        }
        
        // Validate result
        if (result.Status() != manifold::Manifold::Error::NoError) {
            return ManifoldResult<MeshData>(ManifoldError::OperationFailed, 
                "Boolean operation failed to produce valid manifold");
        }
        
        // Convert result back to MeshData
        return ConvertToMeshData(result.GetMeshGL());
        
    }
    catch (const std::exception& e) {
        LogError("PerformBooleanOperation", e);
        return ManifoldResult<MeshData>(ManifoldError::OperationFailed, e.what());
    }
}

ManifoldResult<KX_ManifoldWrapper::MeshData> KX_ManifoldWrapper::SimplifyMesh(
    const MeshData& input_mesh,
    float tolerance) const {
    
    std::lock_guard<std::mutex> lock(operation_mutex_);
    
    // Validate input
    if (!input_mesh.IsValid()) {
        return ManifoldResult<MeshData>(ManifoldError::InvalidInput, 
            "Input mesh is invalid");
    }
    
    if (tolerance < 0.0f || tolerance > 1.0f) {
        return ManifoldResult<MeshData>(ManifoldError::InvalidInput, 
            "Tolerance must be between 0.0 and 1.0");
    }
    
    try {
        // Convert to Manifold format
        auto mesh_gl_result = ConvertFromMeshData(input_mesh);
        if (!mesh_gl_result.IsSuccess()) {
            return ManifoldResult<MeshData>(mesh_gl_result.GetError(), 
                "Failed to convert input mesh: " + mesh_gl_result.GetErrorMessage());
        }
        
        // Create Manifold object
        manifold::Manifold manifold_obj(mesh_gl_result.GetValue());
        
        // Validate manifold
        if (manifold_obj.Status() != manifold::Manifold::Error::NoError) {
            return ManifoldResult<MeshData>(ManifoldError::ManifoldNotManifold, 
                "Input mesh is not a valid manifold");
        }
        
        // Apply simplification with bounds checking
        if (tolerance <= 0.0f) {
            return ManifoldResult<MeshData>(ManifoldError::InvalidInput, 
                "Tolerance must be positive");
        }
        
        int refine_level = std::max(1, std::min(100, static_cast<int>(1.0f / tolerance)));
        manifold::Manifold simplified = manifold_obj.Refine(refine_level);
        
        // Validate result
        if (simplified.Status() != manifold::Manifold::Error::NoError) {
            return ManifoldResult<MeshData>(ManifoldError::OperationFailed, 
                "Simplification operation failed");
        }
        
        // Convert result back
        return ConvertToMeshData(simplified.GetMeshGL());
        
    }
    catch (const std::exception& e) {
        LogError("SimplifyMesh", e);
        return ManifoldResult<MeshData>(ManifoldError::OperationFailed, e.what());
    }
}

ManifoldResult<bool> KX_ManifoldWrapper::ValidateMesh(const MeshData& mesh) const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    
    try {
        // Basic validation first
        if (!mesh.IsValid()) {
            return ManifoldResult<bool>(ManifoldError::InvalidMesh, 
                "Basic mesh validation failed");
        }
        
        // Convert to Manifold format for advanced validation
        auto mesh_gl_result = ConvertFromMeshData(mesh);
        if (!mesh_gl_result.IsSuccess()) {
            return ManifoldResult<bool>(mesh_gl_result.GetError(), 
                "Failed to convert mesh for validation: " + mesh_gl_result.GetErrorMessage());
        }
        
        // Create Manifold object
        manifold::Manifold manifold_obj(mesh_gl_result.GetValue());
        
        // Check if it's a valid manifold
        bool is_valid = (manifold_obj.Status() == manifold::Manifold::Error::NoError);
        
        if (!is_valid) {
            std::string error_msg = "Mesh is not a valid manifold (Status: " + 
                                  std::to_string(static_cast<int>(manifold_obj.Status())) + ")";
            return ManifoldResult<bool>(ManifoldError::ManifoldNotManifold, error_msg);
        }
        
        return ManifoldResult<bool>(std::move(is_valid));
        
    }
    catch (const std::exception& e) {
        LogError("ValidateMesh", e);
        return ManifoldResult<bool>(ManifoldError::OperationFailed, e.what());
    }
}

#ifdef WITH_TBB
ManifoldResult<std::vector<KX_ManifoldWrapper::MeshData>> KX_ManifoldWrapper::ParallelMeshOperations(
    const std::vector<MeshData>& meshes,
    BooleanOperation op,
    float tolerance) const {
    
    std::lock_guard<std::mutex> lock(operation_mutex_);
    
    // Validate inputs
    if (meshes.empty()) {
        return ManifoldResult<std::vector<MeshData>>(ManifoldError::InvalidInput, 
            "Input mesh array is empty");
    }
    
    if (tolerance < 0.0f || tolerance > 1.0f) {
        return ManifoldResult<std::vector<MeshData>>(ManifoldError::InvalidInput, 
            "Tolerance must be between 0.0 and 1.0");
    }
    
    try {
        // Pre-validate all meshes
        for (size_t i = 0; i < meshes.size(); ++i) {
            if (!meshes[i].IsValid()) {
                std::string error_msg = "Invalid mesh at index " + std::to_string(i);
                return ManifoldResult<std::vector<MeshData>>(ManifoldError::InvalidInput, error_msg);
            }
        }
        
        std::vector<MeshData> results(meshes.size());
        std::atomic<bool> has_error{false};
        std::string error_message;
        
        // Parallel processing of mesh operations using TBB
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, meshes.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end() && !has_error.load(); ++i) {
                    try {
                        const MeshData& input_mesh = meshes[i];
                        
                        // Convert to Manifold format using our helper method
                        auto mesh_gl_result = ConvertFromMeshData(input_mesh);
                        if (!mesh_gl_result.IsSuccess()) {
                            has_error.store(true);
                            error_message = "Failed to convert mesh at index " + std::to_string(i) + 
                                          ": " + mesh_gl_result.GetErrorMessage();
                            return;
                        }
                        
                        // Create Manifold object
                        manifold::Manifold manifold_obj(mesh_gl_result.GetValue());
                        
                        // Validate manifold
                        if (manifold_obj.Status() != manifold::Manifold::Error::NoError) {
                            has_error.store(true);
                            error_message = "Invalid manifold at index " + std::to_string(i);
                            return;
                        }
                        
                        // Apply operation based on type
                        manifold::Manifold processed;
                        switch (op) {
                            case BooleanOperation::Union:
                                // For single mesh operations, apply smoothing
                                processed = manifold::Manifold::Smooth(mesh_gl_result.GetValue());
                                break;
                            case BooleanOperation::Intersection:
                                // Apply mesh refinement based on tolerance with bounds checking
                                if (tolerance > 0.0f) {
                                    int refine_level = std::max(1, std::min(100, static_cast<int>(1.0f / tolerance)));
                                    processed = manifold_obj.Refine(refine_level);
                                } else {
                                    processed = manifold_obj;
                                }
                                break;
                            case BooleanOperation::Difference:
                                // Apply identity operation (no change)
                                processed = manifold_obj;
                                break;
                            default:
                                processed = manifold_obj;
                                break;
                        }
                        
                        // Validate processed result
                        if (processed.Status() != manifold::Manifold::Error::NoError) {
                            has_error.store(true);
                            error_message = "Processing failed for mesh at index " + std::to_string(i);
                            return;
                        }
                        
                        // Convert result back using our helper method
                        auto result_mesh = ConvertToMeshData(processed.GetMeshGL());
                        if (!result_mesh.IsSuccess()) {
                            has_error.store(true);
                            error_message = "Failed to convert result for mesh at index " + std::to_string(i) + 
                                          ": " + result_mesh.GetErrorMessage();
                            return;
                        }
                        
                        results[i] = result_mesh.TakeValue();
                    }
                    catch (const std::exception& e) {
                        has_error.store(true);
                        error_message = "Exception processing mesh at index " + std::to_string(i) + ": " + e.what();
                        return;
                    }
                }
            }
        );
        
        if (has_error.load()) {
            return ManifoldResult<std::vector<MeshData>>(ManifoldError::OperationFailed, error_message);
        }
        
        return ManifoldResult<std::vector<MeshData>>(std::move(results));
    }
    catch (const std::exception& e) {
        LogError("ParallelMeshOperations", e);
        return ManifoldResult<std::vector<MeshData>>(ManifoldError::OperationFailed, e.what());
    }
}
#endif

#endif  // WITH_MANIFOLD