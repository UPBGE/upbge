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

/** \file gameengine/Ketsji/KX_ManifoldWrapper.h
 *  \ingroup ketsji
 */

#pragma once

#ifdef WITH_MANIFOLD

#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <cassert>
#include <manifold/manifold.h>

/**
 * Error codes for Manifold operations
 */
enum class ManifoldError {
    None = 0,
    InvalidInput,
    InvalidMesh,
    ManifoldNotManifold,
    OperationFailed,
    InsufficientMemory,
    Unknown
};

/**
 * Result wrapper for Manifold operations
 */
template<typename T>
class ManifoldResult {
public:
    // Success constructor
    ManifoldResult(T&& value) : value_(std::move(value)), error_(ManifoldError::None) {}
    
    // Error constructor
    ManifoldResult(ManifoldError error, const std::string& message = "")
        : value_(), error_(error), error_message_(message) {}
    
    // Query methods
    bool IsSuccess() const { return error_ == ManifoldError::None; }
    ManifoldError GetError() const { return error_; }
    const std::string& GetErrorMessage() const { return error_message_; }
    
    // Value access (only safe when IsSuccess() is true)
    const T& GetValue() const { 
        assert(IsSuccess() && "Cannot get value from failed result");
        return value_; 
    }
    
    T&& TakeValue() { 
        assert(IsSuccess() && "Cannot take value from failed result");
        return std::move(value_); 
    }

private:
    T value_;
    ManifoldError error_;
    std::string error_message_;
};

/**
 * Thread-safe wrapper class for Manifold 3D geometry operations in UPBGE
 * Provides high-performance mesh boolean operations and processing
 * 
 * Design principles:
 * - Thread-safe by design
 * - RAII resource management
 * - Comprehensive error handling
 * - Modern C++ practices
 */
class KX_ManifoldWrapper
{
public:
    enum class BooleanOperation {
        Union,
        Intersection,
        Difference
    };
    
    /**
     * Mesh data structure with validation
     */
    struct MeshData {
        std::vector<float> vertices;
        std::vector<int> indices;
        
        // Validation methods
        bool IsValid() const;
        size_t GetVertexCount() const { return vertices.size() / 3; }
        size_t GetTriangleCount() const { return indices.size() / 3; }
        void Clear();
        
        // Move semantics
        MeshData() = default;
        MeshData(const MeshData&) = default;
        MeshData(MeshData&&) = default;
        MeshData& operator=(const MeshData&) = default;
        MeshData& operator=(MeshData&&) = default;
    };

private:
    mutable std::mutex operation_mutex_; ///< Thread safety for operations
    static thread_local std::string last_error_message_; ///< Thread-local error storage

    // Internal helper methods
    ManifoldResult<MeshData> ConvertToMeshData(const manifold::MeshGL& mesh_gl) const;
    ManifoldResult<manifold::MeshGL> ConvertFromMeshData(const MeshData& mesh_data) const;
    void LogError(const std::string& operation, const std::exception& e) const;

public:
    KX_ManifoldWrapper();
    virtual ~KX_ManifoldWrapper();

    // Disable copy construction and assignment (use shared_ptr for sharing)
    KX_ManifoldWrapper(const KX_ManifoldWrapper&) = delete;
    KX_ManifoldWrapper& operator=(const KX_ManifoldWrapper&) = delete;
    
    // Move semantics are deleted due to mutex member
    KX_ManifoldWrapper(KX_ManifoldWrapper&&) = delete;
    KX_ManifoldWrapper& operator=(KX_ManifoldWrapper&&) = delete;

    /**
     * Perform boolean operations between two meshes with comprehensive error handling
     * @param mesh1 First input mesh
     * @param mesh2 Second input mesh  
     * @param op Boolean operation to perform
     * @return Result containing output mesh or error information
     * @thread_safety Thread-safe
     */
    ManifoldResult<MeshData> PerformBooleanOperation(
        const MeshData& mesh1,
        const MeshData& mesh2,
        BooleanOperation op) const;

    /**
     * Simplify a mesh by reducing vertex/triangle count
     * @param input_mesh Input mesh to simplify
     * @param tolerance Simplification tolerance (0.0-1.0)
     * @return Result containing simplified mesh or error information
     * @thread_safety Thread-safe
     */
    ManifoldResult<MeshData> SimplifyMesh(
        const MeshData& input_mesh,
        float tolerance) const;

    /**
     * Validate if a mesh forms a valid manifold
     * @param mesh Input mesh to validate
     * @return Result containing validation status and details
     * @thread_safety Thread-safe
     */
    ManifoldResult<bool> ValidateMesh(const MeshData& mesh) const;

    /**
     * Get the last error message for the current thread
     * @return Last error message or empty string
     * @thread_safety Thread-safe
     */
    static std::string GetLastError();

#ifdef WITH_TBB
    /**
     * Perform mesh operations in parallel using Intel TBB
     * @param meshes Input mesh data array
     * @param op Operation to perform on each mesh  
     * @param tolerance Additional parameter for operations that need it
     * @return Result containing array of processed meshes or error information
     * @thread_safety Thread-safe
     */
    ManifoldResult<std::vector<MeshData>> ParallelMeshOperations(
        const std::vector<MeshData>& meshes,
        BooleanOperation op,
        float tolerance = 0.1f) const;
#endif

    /**
     * Factory method to create a shared instance
     * @return Shared pointer to new instance
     */
    static std::shared_ptr<KX_ManifoldWrapper> Create();
    
    /**
     * Get statistics about mesh complexity
     * @param mesh Input mesh
     * @return String containing mesh statistics
     */
    std::string GetMeshStats(const MeshData& mesh) const;
};

#endif  // WITH_MANIFOLD