# Jolt Physics Integration Plan for UPBGE Game Engine

Complete implementation plan to integrate Jolt Physics as an alternative physics backend in UPBGE, mapping all Bullet UI settings to Jolt equivalents, covering every interface method, collision system, vehicle/character controllers, soft bodies, debug drawing, and Python API.

---

## 0. Jolt Physics Integration Approach

### 0.1 Build Strategy and Binary Distribution

**Important:** Jolt Physics does **NOT** provide official prebuilt binaries. Users must compile Jolt from source, but this happens automatically as part of UPBGE's build process.

**Rationale:**
- Jolt Physics provides stable releases (e.g., v5.5.0) but only as source code
- No official prebuilt binaries available from Jolt releases
- vcpkg and Conan packages exist but build from source
- UPBGE's build system will automatically download and compile Jolt
- This is similar to Bullet Physics integration in UPBGE

**Implementation options (in priority order):**

1. **CMake FetchContent (Recommended)**
   - Use `FetchContent` in UPBGE's CMake to download and build Jolt from a specific stable release tag
   - Example: `FetchContent_Declare(joltphysics GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git GIT_TAG v5.5.0)`
   - Jolt compiles automatically as part of UPBGE's normal build process
   - No manual Jolt installation required by users
   - Users only need standard CMake build tools (CMake 3.23+, C++17 compiler)

2. **vcpkg Integration (Alternative)**
   - vcpkg provides `joltphysics` package (versions 5.5.0, 5.4.0, 5.3.0, etc.)
   - Use `find_package(joltphysics)` in CMake
   - vcpkg builds Jolt from source but caches binaries locally
   - Can use vcpkg binary caching to speed up rebuilds across projects

3. **Conan Integration (Alternative)**
   - Conan provides `joltphysics` package (version 5.2.0)
   - Use `conan install` to fetch and integrate
   - Builds from source but can cache binaries

**Version Selection:**
- Target latest stable release (currently v5.5.0 as of documentation review)
- Pin to specific version tag to ensure reproducible builds
- Update version via CMake variable for easy version upgrades
- Test compatibility with each major version upgrade

**Build Configuration (Recommended for Modern Game Engines):**

**Essential Settings (Always Enable):**
- `JPH_CROSS_PLATFORM_DETERMINISTIC` - Cross-platform consistency (8% slower)
- `-ffp-model=precise` (Clang) or `/fp:precise` (MSVC) - Required for determinism
- `JPH_DEBUG_RENDERER` - Debug visualization (on by default in Debug/Release)
- `JPH_OBJECT_LAYER_BITS=32` - More collision groups (16 bits may be limiting)

**Performance Optimizations (Enable All Defaults):**
- `JPH_USE_SSE4_1` / `JPH_USE_SSE4_2` / `JPH_USE_F16C` / `JPH_USE_LZCNT` / `JPH_USE_TZCNT` - CPU instructions (on by default)
- `JPH_USE_AVX` / `JPH_USE_AVX2` - Vector instructions (on by default)
- `JPH_USE_FMADD` - Fused multiply-add (disabled by `JPH_CROSS_PLATFORM_DETERMINISTIC`)
- `JPH_USE_AVX512` - Optional for newer CPUs (can enable for performance)

**Typically Disabled:**
- `JPH_DOUBLE_PRECISION` - Not needed for typical game worlds (enable only for large worlds)
- `JPH_EXTERNAL_PROFILE` - Only when profiling (off by default)
- `JPH_OBJECT_STREAM` - Serialization code (examples only)
- `CPP_RTTI_ENABLED` - C++ RTTI not needed (off by default)

**Build-Specific Settings:**
- Debug builds: `JPH_ENABLE_ASSERTS` enabled, `JPH_PROFILE_ENABLED` optional
- Release builds: `JPH_ENABLE_ASSERTS` disabled, `JPH_PROFILE_ENABLED` disabled

**Optional (Project-Specific):**
- `JPH_DISABLE_TEMP_ALLOCATOR` - Enable only for ASAN debugging
- `JPH_DISABLE_CUSTOM_ALLOCATOR` - Enable only for ASAN debugging

**CMake Requirements:**
- CMake 3.23+
- C++17 compiler

**Summary for UPBGE:**
- Use defaults + `JPH_CROSS_PLATFORM_DETERMINISTIC` + `JPH_OBJECT_LAYER_BITS=32`
- Disable `JPH_DOUBLE_PRECISION` (typical use)
- Keep all CPU instruction sets enabled
- This matches modern game engine best practices

**User Experience:**
- Users build UPBGE normally via CMake
- Jolt is automatically downloaded and compiled as part of UPBGE's build
- First build: Jolt compiles (takes a few minutes)
- Subsequent builds: Only recompile if Jolt version changes
- No separate Jolt installation or configuration required
- Optional: Provide `USE_JOLT` CMake option to enable/disable Jolt backend

**Binary Caching (Optional Enhancement):**
- For faster rebuilds, can set up vcpkg binary cache
- Cache stores compiled Jolt binaries for reuse across projects
- Requires hosting a binary cache server (e.g., Artifactory, Azure Artifacts)
- Not required for basic functionality, but improves developer experience

### 0.2 Multithreading Configuration

**JobSystem Selection:**
Jolt requires a `JobSystem` at initialization for multithreaded physics simulation:

- **`JobSystemThreadPool` (Recommended)** — Jolt manages its own thread pool
  - Specify number of threads: `max(1, num_cores - 1)` for physics
  - Parallelizes collision detection, constraint solving, and broadphase queries
  - Optimal for performance on multi-core systems

- **`JobSystemSingleThreaded`** — All work on calling thread
  - Use only for debugging or single-threaded platforms
  - No threading overhead but significantly slower

**Threading Model:**
- UPBGE runs game logic on the main thread
- Jolt's `PhysicsSystem::Update()` uses jobs for parallel processing
- Cannot read/write bodies during `PhysicsSystem::Update()` — all body manipulation must happen before or after the update call

**Body Access Patterns:**
- Use **locking** `BodyInterface` (`PhysicsSystem::GetBodyInterface()`) for safe access from multiple threads
- Use **non-locking** variant (`GetBodyInterfaceNoLock()`) only when threading guarantees are managed manually
- Contact listeners and step listeners run from multiple threads — must be thread-safe

**Thread Safety Considerations:**
- All `ContactListener` callbacks (`OnContactAdded`, `OnContactPersisted`, `OnContactRemoved`) are called from multiple threads
- `PhysicsStepListener::OnStep()` runs from multiple threads
- Only body reading is safe in callbacks — no body writing during physics update
- Use atomic operations or mutexes for shared state in listeners

**Thread Count Configuration:**
- Add Blender UI setting under physics engine selection: "Physics Threads" (default: auto)
- Allow user to specify number of CPU cores for Jolt physics (1 to max cores)
- Default: `max(1, num_cores - 1)` to reserve 1 core for main thread
- Setting stored in scene/game properties
- Applied when physics system initializes (cannot change at runtime)
- Valid range: 1 to `std::thread::hardware_concurrency()`

### 0.3 CMake Integration Details

**CMake Option for Jolt Backend:**
```cmake
option(WITH_JOLT "Enable Jolt Physics (Alternative Physics Backend)" OFF)
option(WITH_SYSTEM_JOLT "Use system Jolt library instead of bundled" OFF)
```

**FetchContent Integration (Recommended):**
```cmake
if(WITH_JOLT)
  if(WITH_SYSTEM_JOLT)
    find_package(joltphysics CONFIG REQUIRED)
  else()
    include(FetchContent)
    FetchContent_Declare(
      joltphysics
      GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
      GIT_TAG v5.5.0
    )
    FetchContent_MakeAvailable(joltphysics)
  endif()
  
  # Create Jolt library target
  if(NOT TARGET joltphysics)
    add_library(joltphysics INTERFACE IMPORTED)
  endif()
  
  # Link Jolt to game engine
  target_link_libraries(ge_physics_jolt INTERFACE joltphysics)
  target_include_directories(ge_physics_jolt SYSTEM INTERFACE ${Jolt_INCLUDE_DIRS})
  target_compile_definitions(ge_physics_jolt INTERFACE WITH_JOLT)
endif()
```

**Physics Backend Selection:**
```cmake
# In source/gameengine/CMakeLists.txt
# Add Jolt as an additional backend (Bullet is already compiled)
if(WITH_JOLT)
  add_subdirectory(Physics/Jolt)
endif()
```

**Jolt-Specific CMake Options:**
```cmake
# Jolt build configuration
set(JOLT_CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "Enable cross-platform determinism")
set(JOLT_OBJECT_LAYER_BITS 32 CACHE STRING "Object layer bits (16 or 32)")
set(JOLT_DEBUG_RENDERER ON CACHE BOOL "Enable debug renderer")

# Compiler flags for determinism
if(JOLT_CROSS_PLATFORM_DETERMINISTIC)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(joltphysics PRIVATE -ffp-model=precise)
  elseif(MSVC)
    target_compile_options(joltphysics PRIVATE /fp:precise)
  endif()
endif()
```

**Conditional Compilation:**
```cmake
# Physics environment creation
#ifdef WITH_JOLT
  #include "JoltPhysicsEnvironment.h"
  using PhysicsEnvironment = JoltPhysicsEnvironment;
#else
  #include "CcdPhysicsEnvironment.h"
  using PhysicsEnvironment = CcdPhysicsEnvironment;
#endif
```

**Build System Integration Points:**
1. **`extern/CMakeLists.txt`** — Add Jolt subdirectory conditionally
2. **`source/gameengine/CMakeLists.txt`** — Add Jolt physics subdirectory
3. **`source/gameengine/Physics/Jolt/CMakeLists.txt`** — Jolt-specific build rules
4. **`build_files/cmake/platform/dependency_targets.cmake`** — Add `bf_deps_optional_jolt` target

**Example Jolt Physics Subdirectory CMakeLists.txt:**
```cmake
# source/gameengine/Physics/Jolt/CMakeLists.txt
set(INC
  .
  ../Common
  ../../Common
  ../../Converter
  ../../Expressions
  ../../GameLogic
  ../../Ketsji
  ../../Rasterizer
  ../../SceneGraph
  ../../../blender/blenkernel
  ../../../blender/makesrna
)

set(INC_SYS
  ${Jolt_INCLUDE_DIRS}
)

set(SRC
  JoltPhysicsEnvironment.cpp
  JoltPhysicsController.cpp
  JoltGraphicController.cpp
  
  JoltPhysicsEnvironment.h
  JoltPhysicsController.h
  JoltGraphicController.h
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::intern::guardedalloc
  PRIVATE joltphysics
)

if(WITH_PYTHON)
  list(APPEND INC_SYS ${PYTHON_INCLUDE_DIRS})
endif()

blender_add_lib(ge_physics_jolt "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
```

### 0.4 Performance Profiling

**JPH_EXTERNAL_PROFILE Integration:**
```cpp
// Custom profiler implementation
class UPBGEProfiler : public JPH::Profiler
{
public:
    virtual void RecordEvent(const char *inName, const char *inFunction, uint32_t inLine, uint32_t inColor) override
    {
        // Forward to UPBGE's profiling system
        UPBGE_RecordPhysicsEvent(inName, inFunction, inLine);
    }
};

// Initialize custom profiler
JPH::Profiler *profiler = new UPBGEProfiler();
JPH::SetProfiler(profiler);
```

**Performance Counters and Metrics:**
- **Body count tracking** — Active vs sleeping bodies
- **Broadphase time** — Time spent in collision detection
- **Narrowphase time** — Time spent in collision resolution
- **Constraint solver time** — Time spent solving constraints
- **Contact count** — Number of active contacts per frame
- **Island count** — Number of sleeping vs active islands
- **Job system utilization** — Thread pool efficiency metrics

**Metrics Collection:**
```cpp
// After PhysicsSystem::Update()
JPH::PhysicsSystem::Stats stats = physicsSystem->GetStats();
printf("Bodies: %d/%d, Contacts: %d, Islands: %d/%d\n",
       stats.mNumActiveBodies, stats.mNumBodies,
       stats.mNumActiveContactConstraints,
       stats.mNumActiveIslands, stats.mNumIslands);
```

**Debug Visualization Tools:**
- **Physics step timing** — Visualize time spent in each physics phase
- **Contact point visualization** — Show where bodies touch
- **Broadphase visualization** — Show AABB overlap regions
- **Constraint visualization** — Draw constraint limits and forces
- **Body state visualization** — Color code by motion type (static/dynamic/kinematic)

### 0.5 Debugging Tools

**Jolt Debug Renderer Integration:**
```cpp
// Custom debug renderer for UPBGE
class UPBGEDebugRenderer : public JPH::DebugRenderer
{
public:
    virtual void DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::Vec4Arg inColor) override
    {
        // Convert to UPBGE's debug line drawing
        UPBGE_DrawDebugLine(
            JPHToFloat3(inFrom),
            JPHToFloat3(inTo),
            JPHToFloat4(inColor)
        );
    }
    
    virtual void DrawTriangle(JPH::RVec3Arg inV1, JPH::RVec3Arg inV2, JPH::RVec3Arg inV3, JPH::Vec4Arg inColor) override
    {
        // Convert to UPBGE's debug triangle drawing
        UPBGE_DrawDebugTriangle(
            JPHToFloat3(inV1),
            JPHToFloat3(inV2),
            JPHToFloat3(inV3),
            JPHToFloat4(inColor)
        );
    }
    
    // Implement other DebugRenderer methods...
};

// Initialize debug renderer
UPBGEDebugRenderer *renderer = new UPBGEDebugRenderer();
JPH::DebugRenderer::sInstance = renderer;

// Enable debug drawing (when debug mode is active)
if (debug_physics) {
    physicsSystem->DrawBodies(renderer, JPH::BodyManager::DrawSettings());
    physicsSystem->DrawConstraints(renderer);
}
```

**Collision Visualization:**
- **Contact manifold rendering** — Show contact points and normals
- **Broadphase AABB rendering** — Visualize broadphase queries
- **Narrowphase penetration** — Show penetration depth and direction
- **Constraint limits** — Draw constraint axes and limits
- **Body velocity vectors** — Visualize linear and angular velocities

**Profiling Tools:**
- **JPH_EXTERNAL_PROFILE** — External profiler integration
- **JPH_PROFILE_ENABLED** — Built-in profiler for debug builds
- **Performance counters** — Track timing metrics per frame
- **Memory profiling** — Track TempAllocator usage
- **Thread profiling** — Job system utilization metrics

**Debug Mode Toggle:**
```cpp
// Enable/disable debug visualization via command line
int visualizePhysics = SYS_GetCommandLineInt(syshandle, "show_physics", 0);
if (visualizePhysics) {
    JPH::DebugRenderer::sInstance = new UPBGEDebugRenderer();
    JPH::SetProfiler(new UPBGEProfiler());
}
```

**Debug Output Integration:**
- Integrate with UPBGE's existing debug visualization system
- Use UPBGE's debug line/triangle drawing functions
- Leverage UPBGE's color scheme for consistency
- Support runtime toggling via Python API or command line

### 0.6 Memory Management

**TempAllocator Integration with UPBGE's Memory System:**
- Jolt requires a temporary memory allocator for per-frame allocations
- Use `TempAllocatorImpl` with **32MB minimum** for maximum object count and best performance
- 32MB+ supports complex scenes with 5000+ active bodies, mesh shapes, and many contacts
- Integrate with UPBGE's memory tracking system for leak detection
- Monitor TempAllocator usage between frames and resize if needed
- `TempAllocatorMalloc` as fallback only for debugging (slower, no size limit)

**Jolt Reference Counting (IMPORTANT):**
Jolt uses reference counting for many classes via `Ref<T>` and `RefConst<T>` smart pointers:
- `Shape`, `ShapeSettings` — collision shapes
- `Constraint`, `ConstraintSettings` — constraints
- `PhysicsMaterial` — material properties
- `GroupFilter` — collision filtering
- `SoftBodySharedSettings` — soft body configuration
- `VehicleCollisionTester`, `VehicleController`, `WheelSettings` — vehicles
- `CharacterBaseSettings`, `CharacterBase` — characters

**Reference Counting Rules:**
- Reference counted objects start with ref count 0
- Call `object->AddRef()` to take ownership
- Call `object->ReleaseRef()` to release ownership
- When passing a ref-counted object to another object (e.g., shape to body), the recipient takes a reference automatically
- Use `Ref<T>` or `RefConst<T>` classes for automatic reference management
- `Body` class is special — destroyed via `BodyInterface::DestroyBody()`, not reference counting

**Custom Allocator Implementation:**
- Implement Jolt's `Allocator` interface to integrate with UPBGE's memory system
- Override `Allocate()` and `Free()` to use UPBGE's memory pools
- Track allocations for debugging and leak detection
- Support memory alignment requirements (16-byte for SIMD)
- Integrate with UPBGE's existing memory profiling tools

**Memory Leak Detection:**
- Track all Jolt allocations through custom allocator
- Report leaked allocations on shutdown
- Use Valgrind/ASAN integration for memory debugging
- Monitor TempAllocator peak usage and adjust size accordingly
- Detect double-free and use-after-free errors

### 0.7 Thread Safety

**JobSystem Integration with UPBGE's Game Loop:**
- Jolt's `JobSystemThreadPool` manages its own thread pool
- Specify number of threads: `max(1, num_cores - 1)` for physics
- JobSystem runs physics simulation in parallel during `PhysicsSystem::Update()`
- UPBGE game logic runs on main thread, physics runs in background threads

**Thread-Safe Body Access Patterns:**
- Use **locking** `BodyInterface` (`PhysicsSystem::GetBodyInterface()`) for safe access from multiple threads
- Use **non-locking** variant (`GetBodyInterfaceNoLock()`) only when threading guarantees are managed manually
- Do not read/write bodies during `PhysicsSystem::Update()` — all body manipulation must happen before or after the update call
- Contact listeners and step listeners run from multiple threads — must be thread-safe

**Locking vs Non-Locking BodyInterface:**
- Locking interface: Safe access from any thread, but has overhead
- Non-locking interface: Faster, but requires manual thread safety management
- Use non-locking only when you can guarantee no concurrent access during physics update
- Prefer locking interface for safety unless performance is critical

**Thread Safety in Callbacks:**
- `ContactListener` callbacks (`OnContactAdded`, `OnContactPersisted`, `OnContactRemoved`) run from multiple threads
- `PhysicsStepListener::OnStep()` runs from multiple threads
- Only body reading is safe in callbacks — no body writing during physics update
- Use atomic operations or mutexes for shared state in listeners
- Avoid modifying physics state from callbacks

### 0.8 Serialization

**Physics State Save/Load:**
- Bullet uses `btDefaultSerializer` for physics state serialization
- Jolt provides `ObjectStream` for serialization (must enable `JPH_OBJECT_STREAM`)
- Implement custom serializer to save/load physics state to UPBGE scene format
- Save body positions, velocities, constraints, and other physics properties
- Load and restore physics state when loading saved games

**Jolt's ObjectStream for Serialization:**
- `JPH_OBJECT_STREAM` must be enabled at build time for serialization support
- Use `StreamIn` and `StreamOut` to serialize/deserialize physics objects
- Serialize bodies, constraints, shapes, and physics system settings
- Integrate with UPBGE's existing scene file format
- Maintain backward compatibility with Bullet-based saves

**Scene File Format Changes:**
- Add version field to scene files to distinguish Bullet vs Jolt physics
- Migrate Bullet physics data to Jolt format when loading old scenes
- Provide fallback to Bullet backend for old scenes if Jolt not available
- Document serialization format for future compatibility
- Test serialization/deserialization with complex scenes

### 0.9 Platform-Specific Issues

**Platform-Specific Considerations:**
- **Windows:** MSVC compiler flags for determinism (`/fp:precise`)
- **Linux:** GCC/Clang compiler flags for determinism (`-ffp-model=precise`)
- **macOS:** Platform-specific threading and memory considerations
- **ARM64:** SIMD instruction set compatibility (SSE4/AVX2 may not be available)

**SIMD Instruction Set Compatibility:**
- Jolt uses SSE4.1, SSE4.2, AVX, AVX2 by default on x86/x64
- ARM64 platforms: Use NEON SIMD instructions (Jolt supports this)
- Fallback to scalar code if SIMD not available (performance penalty)
- Detect CPU capabilities at runtime and use appropriate code paths
- Disable AVX512 by default (not widely supported)

**Platform-Specific Threading:**
- Windows: Use `JobSystemThreadPool` with Windows threading primitives
- Linux: Use pthread-based thread pool
- macOS: Use GCD-based thread pool or pthread
- Ensure thread affinity and priority settings are appropriate for each platform

**Memory Alignment:**
- Jolt requires 16-byte memory alignment for SIMD operations
- Ensure custom allocator provides properly aligned memory
- Use aligned allocation functions on each platform
- Test memory alignment on all supported platforms

**Compiler-Specific Issues:**
- MSVC: Use `/fp:precise` for determinism
- GCC/Clang: Use `-ffp-model=precise` for determinism
- Ensure consistent floating-point behavior across compilers
- Test cross-platform determinism with automated tests

---

## 1. Bullet UI Settings → Jolt Mappings

### 1.1 Rigid Body Properties (from `CcdConstructionInfo`)

| Bullet / Blender Field | Jolt API | Notes |
|---|---|---|
| `m_mass` / `blenderobject->mass` | `BodyCreationSettings::mMassPropertiesOverride` | Use `MassProperties::ScaleToMass()` |
| `m_friction` / `blenderobject->friction` | `BodyCreationSettings::mFriction` / `Body::SetFriction()` | Direct 0–1 |
| `m_rollingFriction` / `blenderobject->rolling_friction` | No direct equivalent | Approximate via custom `ContactListener::OnContactAdded` |
| `m_restitution` / `blenderobject->reflect` | `BodyCreationSettings::mRestitution` / `Body::SetRestitution()` | Direct 0–1 |
| `m_linearDamping` / `blenderobject->damping` | `BodyCreationSettings::mLinearDamping` / `Body::GetMotionProperties()->SetLinearDamping()` | Direct |
| `m_angularDamping` / `blenderobject->rdamping` | `BodyCreationSettings::mAngularDamping` / `Body::GetMotionProperties()->SetAngularDamping()` | Direct |
| `m_margin` / `blenderobject->margin` | `ConvexShapeSettings::mConvexRadius` | Per-shape, set at creation |
| `m_gravity` | `Body::GetMotionProperties()->SetGravityFactor()` | Per-body gravity multiplier; global via `PhysicsSystem::SetGravity()` |
| `m_localInertiaTensor` | `BodyCreationSettings::mMassPropertiesOverride.mInertia` | Override inertia tensor |
| `m_inertiaFactor` / `blenderobject->formfactor` | Scale the computed inertia by factor | Custom scaling at creation time |
| `m_linearFactor` | No direct equivalent | Implement via custom velocity clamping in `StepListener` |
| `m_angularFactor` | No direct equivalent | Implement via custom velocity clamping in `StepListener` |
| `m_clamp_vel_min` / `m_clamp_vel_max` | `Body::GetMotionProperties()->SetMaxLinearVelocity()` | Clamp in `SimulationTick` callback |
| `m_clamp_angvel_min` / `m_clamp_angvel_max` | `Body::GetMotionProperties()->SetMaxAngularVelocity()` | Clamp in `SimulationTick` callback |
| `m_ccd_motion_threshold` | `BodyCreationSettings::mMotionQuality = EMotionQuality::LinearCast` | Enable CCD per body |
| `m_ccd_swept_sphere_radius` | No direct equivalent | Jolt CCD is automatic with `LinearCast` quality |
| `m_scaling` | `ScaledShapeSettings` wrapping the base shape | Apply at shape creation |
| `m_collisionGroup` / `m_collisionMask` | `Body::SetCollisionGroup()` with `GroupFilterTable` | Map 16-bit group/mask to Jolt `GroupFilter` |
| `m_collisionFilterGroup` / `m_collisionFilterMask` | `ObjectLayer` + `BroadPhaseLayer` filtering | Map Dynamic/Static/Sensor/Character/Kinematic filters |
| `m_do_anisotropic` / `m_anisotropicFriction` | No direct equivalent | Approximate via custom `ContactListener` modifying friction per contact |

### 1.2 Object Type Mapping

| UPBGE Type (`blenderobject->gameflag`) | Jolt Equivalent |
|---|---|
| `OB_DYNAMIC` (not rigid) | `EMotionType::Dynamic`, lock rotation axes |
| `OB_RIGID_BODY` | `EMotionType::Dynamic` |
| `OB_COLLISION` (static) | `EMotionType::Static` |
| `OB_SENSOR` | `EMotionType::Static` + `Body::SetIsSensor(true)` |
| `OB_CHARACTER` (via `m_bCharacter`) | `CharacterVirtual` or `Character` |
| `OB_SOFT_BODY` (via `m_bSoft`) | `SoftBodyCreationSettings` |
| Kinematic (via `OB_KINEMATIC`) | `EMotionType::Kinematic` |

### 1.3 FH Springs (Floating Height)

| Bullet Field | Jolt Equivalent | Notes |
|---|---|---|
| `m_do_fh` | No direct equivalent | Implement via ray cast in `StepListener` callback |
| `m_do_rot_fh` | No direct equivalent | Custom torque application |
| `m_fh_spring` | Custom spring constant | Apply force = -k * penetration |
| `m_fh_damping` | Custom damping | Apply force = -c * velocity |
| `m_fh_distance` | Ray cast distance | Cast ray downward, check distance |
| `m_fh_normal` | Use hit normal for force direction | From ray cast result |

**Implementation:** Replicate `ProcessFhSprings()` logic using Jolt's `NarrowPhaseQuery::CastRay()` in a per-tick callback.

### 1.4 Soft Body Properties (from `CcdConstructionInfo`)

| Bullet Field | Jolt Equivalent | Notes |
|---|---|---|
| `m_soft_linStiff` (linear stiffness 0–1) | `SoftBodySharedSettings::Edge::mCompliance` | compliance = 1.0/stiffness (invert) |
| `m_soft_angStiff` (angular stiffness 0–1) | `SoftBodySharedSettings::DihedralBend::mCompliance` | Dihedral bend constraint compliance |
| `m_soft_volume` (volume preservation 0–1) | `SoftBodySharedSettings::Volume::mCompliance` | Volume constraint compliance |
| `m_soft_viterations` | `SoftBodyCreationSettings::mNumIterations` | Velocity solver iterations |
| `m_soft_piterations` | `SoftBodyCreationSettings::mNumIterations` | Position solver iterations |
| `m_soft_diterations` | Not directly mappable | Jolt has single iteration count |
| `m_soft_citerations` | Not directly mappable | Combine into `mNumIterations` |
| `m_soft_kDP` (damping 0–1) | `SoftBodySharedSettings::mDamping` | Direct mapping |
| `m_soft_kPR` (pressure) | `SoftBodySharedSettings::mPressure` | Direct mapping |
| `m_soft_kDF` (dynamic friction) | `BodyCreationSettings::mFriction` | Set on the soft body |
| `m_soft_kLF` (lift coefficient) | `SoftBodyUpdateContext::mDeltaPosition` | Approximate via wind forces |
| `m_soft_kDG` (drag coefficient) | Approximate via damping | No direct Jolt equivalent |
| `m_soft_kVCF` (velocity correction) | Not directly mappable | Use Jolt defaults |
| `m_soft_kMT` (pose matching) | Not directly mappable | Jolt doesn't have pose matching |
| `m_soft_kCHR` (rigid contacts hardness) | `SoftBodyContactListener::OnSoftBodyContactValidate` | Override per-contact |
| `m_soft_kKHR` (kinetic contacts hardness) | `SoftBodyContactListener::OnSoftBodyContactValidate` | Override per-contact |
| `m_soft_kSHR` (soft contacts hardness) | Not applicable (Jolt soft-soft collision WIP) | Jolt doesn't support soft-soft yet |
| `m_soft_kAHR` (anchors hardness) | `SoftBodySharedSettings::Skinned` constraints | Map to skinning constraint params |
| `m_soft_kSRHR_CL` (rigid hardness cluster) | `SoftBodyContactListener` | Custom contact override |
| `m_soft_kSKHR_CL` (kinematic hardness cluster) | `SoftBodyContactListener` | Custom contact override |
| `m_soft_kSSHR_CL` (soft hardness cluster) | Not applicable | Jolt soft-soft not supported |
| `m_soft_kSR_SPLT_CL` / `kSK_SPLT_CL` / `kSS_SPLT_CL` | Not directly mappable | Use damping as approximation |
| `m_soft_collisionflags` (SDF_RS, CL_RS, CL_SS, VF_SS) | Jolt handles rigid-soft automatically | Soft-soft not yet supported |
| `m_softBendingDistance` | `SoftBodySharedSettings::CreateConstraints(settings, EBendType::Distance2)` | Bending distance param |
| `m_gamesoftFlag` (shape matching, bending, aero) | Map individually | See below |
| `CCD_BSB_SHAPE_MATCHING` | Not directly mappable | Jolt uses different approach |
| `CCD_BSB_BENDING_CONSTRAINTS` | `SoftBodySharedSettings::CreateConstraints()` | Enable bend constraints |
| `CCD_BSB_AERO_VPOINT` / `CCD_BSB_AERO_VTWOSIDE` | Wind force via `SoftBodyUpdateContext` | Custom wind implementation |

**Jolt Soft Body Limitations:**
- Soft-soft collisions not yet implemented in Jolt
- `AddTorque`/`SetLinearVelocity`/`SetAngularVelocity`/`AddImpulse` have no effect on soft bodies (velocity per-particle)
- Constraints cannot operate on soft bodies directly
- Buoyancy not implemented for soft bodies

### 1.5 Solver / Environment Settings

| Bullet Setting | Jolt Equivalent | Notes |
|---|---|---|
| Solver iterations | `PhysicsSystem::Update(deltaTime, collisionSteps, ...)` | `collisionSteps` parameter |
| `SetNumTimeSubSteps()` | `collisionSteps` in `PhysicsSystem::Update()` | Direct mapping |
| `SetDeactivationTime()` | `PhysicsSettings::mTimeBeforeSleep` | Direct mapping |
| `SetDeactivationLinearTreshold()` | `PhysicsSettings::mPointVelocitySleepThreshold` | Jolt uses single threshold |
| `SetDeactivationAngularTreshold()` | Combined into `mPointVelocitySleepThreshold` | Use max of linear/angular |
| `SetERPNonContact()` / `SetERPContact()` | No equivalent | Jolt iterative solver handles this internally |
| `SetCFM()` | No equivalent | Use Jolt defaults |
| `SetContactBreakingTreshold()` | No direct equivalent | Jolt manages contact cache internally |
| `SetSolverSorConstant()` | No equivalent | Jolt uses different solver |
| `SetSolverType()` | No equivalent | Jolt has one solver type |
| `SetSolverTau()` / `SetSolverDamping()` | No equivalent | Penalty solver params not applicable |

### 1.6 Constraint Mappings

| UPBGE Constraint Type | Jolt Constraint | Mapping Notes |
|---|---|---|
| `PHY_POINT2POINT_CONSTRAINT` | `PointConstraintSettings` | Pivot point in local space of each body |
| `PHY_LINEHINGE_CONSTRAINT` | `HingeConstraintSettings` | Map hinge axis + limits; note Blender Z→Jolt hinge axis swap |
| `PHY_ANGULAR_CONSTRAINT` | `HingeConstraintSettings` (no ball socket) | Hinge without position constraint |
| `PHY_CONE_TWIST_CONSTRAINT` | `ConeConstraintSettings` or `SwingTwistConstraintSettings` | Map half angle + twist limits |
| `PHY_VEHICLE_CONSTRAINT` | `VehicleConstraintSettings` | Complex vehicle setup |
| `PHY_GENERIC_6DOF_CONSTRAINT` | `SixDOFConstraintSettings` | Map all 6 axis limits via `SetParam()` |
| `PHY_GENERIC_6DOF_SPRING2_CONSTRAINT` | `SixDOFConstraintSettings` with spring motors | Map spring stiffness/damping per axis |
| `RBC_TYPE_FIXED` | `FixedConstraintSettings` | All DOFs locked |
| `RBC_TYPE_SLIDER` | `SliderConstraintSettings` | Map linear limit on X axis |
| `RBC_TYPE_PISTON` | `SixDOFConstraintSettings` (X translation + X rotation free) | Custom limit setup |
| `RBC_TYPE_MOTOR` | `SixDOFConstraintSettings` with motors | Map target velocity + max impulse |

**`PHY_IConstraint` Methods to Implement:**
- `GetEnabled()` / `SetEnabled()` → `Constraint::SetEnabled()`
- `SetParam(dof, min, max)` → **Must preserve Bullet's numeric indices**: params 0-5 = limits, 6-8 = translational motors (vel, force), 9-11 = rotational motors (vel, torque), 12-17 = motorized springs (stiffness, damping). Map to Jolt motor/spring settings per index.
- `GetParam(dof)` → Map to Jolt constraint limit getters
- `GetBreakingThreshold()` / `SetBreakingThreshold()` → Custom: track force, remove if exceeded
- `SetSolverIterations()` → `Constraint::SetNumPositionStepsOverride()` / `SetNumVelocityStepsOverride()`
- `GetIdentifier()` → Maintain constraint ID map
- `GetType()` → Return stored constraint type

### 1.7 Shape Type Mappings

| Blender Bound Type | Jolt Shape | Notes |
|---|---|---|
| `OB_BOUND_BOX` | `BoxShapeSettings(halfExtents)` | Direct |
| `OB_BOUND_SPHERE` | `SphereShapeSettings(radius)` | Direct |
| `OB_BOUND_CAPSULE` | `CapsuleShapeSettings(halfHeight, radius)` | Direct |
| `OB_BOUND_CYLINDER` | `CylinderShapeSettings(halfHeight, radius)` | Direct |
| `OB_BOUND_CONE` | `ConvexHullShapeSettings` | Jolt has no ConeShape; generate convex hull |
| `OB_BOUND_CONVEX_HULL` (PHY_SHAPE_POLYTOPE) | `ConvexHullShapeSettings(vertices)` | Direct |
| `OB_BOUND_TRIANGLE_MESH` (PHY_SHAPE_MESH) | `MeshShapeSettings(triangles)` (static) or `ConvexHullShapeSettings` (dynamic with GImpact) | Static only for MeshShape |
| `PHY_SHAPE_COMPOUND` | `StaticCompoundShapeSettings` or `MutableCompoundShapeSettings` | Direct |
| `PHY_SHAPE_EMPTY` | `EmptyShapeSettings` (Jolt >= 5.0) or skip | Jolt added `EmptyShape` recently |

---

## 2. Files to Create

### 2.1 New Directory: `source/gameengine/Physics/Jolt/`

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Build config for Jolt backend |
| `JoltPhysicsEnvironment.h/.cpp` | Implements `PHY_IPhysicsEnvironment` |
| `JoltPhysicsController.h/.cpp` | Implements `PHY_IPhysicsController` |
| `JoltGraphicController.h/.cpp` | Implements `PHY_IGraphicController` for culling |
| `JoltConstraint.h/.cpp` | Implements `PHY_IConstraint` |
| `JoltVehicle.h/.cpp` | Implements `PHY_IVehicle` |
| `JoltCharacter.h/.cpp` | Implements `PHY_ICharacter` |
| `JoltCollData.h/.cpp` | **Correction**: Implement `PHY_ICollData` from `PHY_DynamicTypes.h` (no new header needed). Store contact data from `ContactListener`. |
| `JoltShapeBuilder.h/.cpp` | Shape creation from Blender mesh/bounds data |
| `JoltMotionState.h/.cpp` | Implements `PHY_IMotionState` (or reuse `DefaultMotionState`) |
| `JoltMathUtils.h` | Conversion functions MT_Vector3↔Vec3, MT_Matrix3x3↔Mat44, coordinate system swap |
| `JoltDebugDraw.h/.cpp` | Implements Jolt `DebugRendererSimple` for UPBGE debug lines |
| `JoltContactListener.h/.cpp` | Implements Jolt `ContactListener` for collision callbacks |

**Factory Method:**
```cpp
// JoltPhysicsEnvironment.h
static JoltPhysicsEnvironment *Create(blender::Scene *blenderscene, bool visualizePhysics);
```

Implements the same pattern as `CcdPhysicsEnvironment::Create()`:
- Reads `blenderscene->gm.physics_threads` for thread count
- Reads `blenderscene->gm.jolt_max_bodies` and `jolt_temp_allocator_mb` for configuration
- Initializes `JobSystemThreadPool` with configured thread count
- Initializes `TempAllocatorImpl` with configured size (32MB minimum)
- Sets debug visualization if `show_physics` command line flag is set
- Sets deactivation thresholds from scene settings

### 2.2 Files to Modify

#### Blender Core (DNA/RNA/Versioning)

**`source/blender/makesdna/DNA_scene_types.h`**
- Add `#define WOPHY_JOLT 6` after `#define WOPHY_BULLET 5` (line ~827)
- Add `short physics_threads` field to `GameData` struct (for thread count UI setting)
- Add `int jolt_max_bodies` and `int jolt_temp_allocator_mb` fields (for configurable `PhysicsSystem::Init` params)

**`source/blender/makesrna/intern/rna_scene.cc`**
- Add `{WOPHY_JOLT, "JOLT", 0, "Jolt", "Use the Jolt Physics engine"}` to `physics_engine_items[]` (line ~6394)
- Add RNA properties for `physics_threads`, `jolt_max_bodies`, `jolt_temp_allocator_mb`

**`source/blender/blenloader/intern/versioning_upbge.cc`**
- Add versioning code to set defaults for new fields when loading old files

**`source/blender/blenloader/intern/versioning_defaults.cc`**
- Set default values for new DNA fields

#### Game Engine Core

**`source/gameengine/Ketsji/KX_PhysicsEngineEnums.h`**
- Add `UseJolt = 6` to `e_PhysicsEngine` enum

**`source/gameengine/Ketsji/KX_Scene.cpp`**
- Fix 6 hardcoded `UseBullet` references to use actual physics engine type:
  - `ConvertBlenderObject()` — line 1061
  - `convert_blender_objects_list_synchronous()` — line 1083
  - `ConvertBlenderObjectsList()` async task — line 1146
  - `convert_blender_collection_synchronous()` — line 1185
  - `ConvertBlenderCollection()` async task — line 1252
  - All must read the actual physics engine type from the scene, not hardcode `UseBullet`

**`source/gameengine/Converter/BL_Converter.cpp`**
- Add `case WOPHY_JOLT:` to switch statement in `ConvertScene()` (line 185-203):
```cpp
#ifdef WITH_JOLT
  case WOPHY_JOLT: {
    SYS_SystemHandle syshandle = SYS_GetSystem();
    int visualizePhysics = SYS_GetCommandLineInt(syshandle, "show_physics", 0);
    phy_env = JoltPhysicsEnvironment::Create(blenderscene, visualizePhysics);
    physics_engine = UseJolt;
    break;
  }
#endif
```

#### Build System

**`CMakeLists.txt`** (top-level)
- Add `option(WITH_JOLT "Enable Jolt Physics (Alternative Physics Backend)" OFF)`

**`source/gameengine/CMakeLists.txt`**
- Add Jolt subdirectory (alongside Bullet, not either/or):
```cmake
if(WITH_BULLET)
  add_subdirectory(Physics/Bullet)
endif()
if(WITH_JOLT)
  add_subdirectory(Physics/Jolt)
endif()
```

**`source/gameengine/Physics/CMakeLists.txt`**
- Add Jolt subdirectory conditionally

**`extern/CMakeLists.txt`**
- Add `joltphysics` subdirectory conditionally

---

## 3. Complete Interface Method Checklist

### 3.1 `PHY_IPhysicsEnvironment` (39 methods)

All methods are pure virtual or have default implementations. Must implement:

| Method | Jolt Implementation |
|---|---|
| `ProceedDeltaTime(curTime, timeStep, interval)` | Call in order: Sync motion states → ApplyEffectorForces() → PhysicsSystem::Update() → ProcessFhSprings() → Sync motion states → Sync vehicle wheels → CallbackTriggers(). **Critical**: Must match Bullet's exact order for physics correctness (see Section 4.3). |
| `UpdateSoftBodies()` | Iterate soft body controllers, call `UpdateSoftBody()` on each |
| `DebugDrawWorld()` | Call `PhysicsSystem::DrawBodies()` via `JoltDebugDraw` |
| `SetFixedTimeStep()` / `GetFixedTimeStep()` | Store and use for substep calculation. **Special case**: When `timeStep == interval`, disable internal substepping (pass `maxSubSteps=0` to `Update()`). |
| `GetDebugMode()` / `SetDebugMode()` | Store mode, configure `JoltDebugDraw` |
| `SetNumIterations()` | Map to collision steps count |
| `SetNumTimeSubSteps()` / `GetNumTimeSubSteps()` | Store and pass to `Update()` |
| `SetDeactivationTime()` | Set `PhysicsSettings::mTimeBeforeSleep` |
| `SetDeactivationLinearTreshold()` | Set `PhysicsSettings::mPointVelocitySleepThreshold` |
| `SetDeactivationAngularTreshold()` | Combine with linear into `mPointVelocitySleepThreshold` |
| `SetERPNonContact()` / `SetERPContact()` / `SetCFM()` | Store values but no direct Jolt mapping; use defaults |
| `SetContactBreakingTreshold()` | No direct mapping; store for reference |
| `SetSolverSorConstant()` / `SetSolverType()` / `SetSolverTau()` / `SetSolverDamping()` | No mapping; store for reference |
| `SetGravity()` / `GetGravity()` | `PhysicsSystem::SetGravity(Vec3(x, y, z))` with Y↔Z swap |
| `CreateConstraint(...)` | Create appropriate Jolt constraint based on type enum |
| `CreateVehicle(ctrl)` | Create `VehicleConstraint` wrapping body, return `JoltVehicle` |
| `RemoveConstraintById(id, free)` | Use `std::unordered_map<int, JPH::Constraint*>` for O(1) lookup, remove from `PhysicsSystem` |
| `IsRigidBodyConstraintEnabled(id)` | Look up constraint via ID map, return `Constraint::GetEnabled()` |
| `GetAppliedImpulse(id)` | No direct Jolt equivalent; return 0 or estimate from contacts |
| `GetVehicleConstraint(id)` | Use `std::unordered_map<int, JoltVehicle*>` for O(1) lookup |
| `GetCharacterController(ob)` | Look up `JoltCharacter` associated with game object |
| `RayTest(callback, from, to)` | Implement `PHY_IRayCastFilterCallback::needBroadphaseRayCast()` filtering, then `NarrowPhaseQuery::CastRay()`, fill `PHY_RayCastResult` (skip `m_meshObject`, polygon, UV fields as Bullet-only). |
| `CullingTest(callback, planes, ...)` | Use Jolt broadphase AABB query or custom culling tree |
| `AddSensor()` / `RemoveSensor()` | Add/remove body from physics system with sensor flag |
| `AddCollisionCallback()` | Store callback pointers for response classes. **Critical**: Must implement broadphase filtering for `PHY_BROADPH_RESPONSE` (Near/Radar sensors) via `ObjectLayerFilter`/`BodyFilter` that calls sensor's `BroadPhaseFilterCollision()`. |
| `RequestCollisionCallback()` / `RemoveCollisionCallback()` | Register/unregister controller for collision notifications |
| `CheckCollision(ctrl0, ctrl1)` | Use `NarrowPhaseQuery::CollideShape()` between two bodies |
| `CreateSphereController()` | Create sensor body with `SphereShape`, return controller |
| `CreateConeController()` | Create sensor body with `ConvexHullShape` (cone approximation) |
| `MergeEnvironment()` | Transfer bodies/constraints from another environment |
| `ConvertObject(...)` | Read Blender object properties, create Jolt body (see Phase 4) |
| `SetupObjectConstraints(...)` | Create constraints from `bRigidBodyJointConstraint` data |
| `CreateRigidBodyConstraint(...)` | Create constraint from `RigidBodyCon` data (see Section 1.6) |
| `SetRigidBodyConstraintEnabled(id, enabled)` | `Constraint::SetEnabled()` + activate connected bodies |
| `ExportFile()` | Optional: serialize Jolt world state |
| `GetNumContactPoints()` / `GetContactPoint()` | **NOT in abstract interface** — Bullet-specific methods, implement as JoltPhysicsEnvironment-specific methods (do not add to PHY_IPhysicsEnvironment) |

### 3.2 `PHY_IPhysicsController` (55+ methods)

| Method | Jolt Implementation |
|---|---|
| `SynchronizeMotionStates(time)` | Read body transform from Jolt, write to `PHY_IMotionState` |
| `UpdateSoftBody()` | Read particle positions from `SoftBodyMotionProperties`, update mesh vertices |
| `SetSoftBodyTransform()` | `BodyInterface::SetPositionAndRotation()` on soft body |
| `RemoveSoftBodyModifier()` | Remove modifier from Blender object |
| `WriteMotionStateToDynamics()` | Read `PHY_IMotionState`, set Jolt body transform (for kinematic) |
| `WriteDynamicsToMotionState()` | Read Jolt body transform, write to `PHY_IMotionState` |
| `GetMotionState()` | Return stored motion state pointer |
| `PostProcessReplica()` | Clone Jolt body for replicated game objects |
| `SetPhysicsEnvironment()` | Update stored environment pointer |
| `RelativeTranslate()` | `BodyInterface::SetPosition(current + delta)` |
| `RelativeRotate()` | `BodyInterface::SetRotation(current * delta)` |
| `GetOrientation()` / `SetOrientation()` | `BodyInterface::Get/SetRotation()` → convert Quat to MT_Matrix3x3 |
| `GetPosition()` / `SetPosition()` | `BodyInterface::Get/SetPosition()` with Y↔Z swap |
| `SetScaling()` | Recreate shape with `ScaledShapeSettings` wrapping original |
| `SetTransform()` | Combined position + rotation set |
| `GetMass()` / `SetMass()` | `Body::GetMotionProperties()->GetInverseMass()` (invert); `SetMass()` via `BodyInterface::SetMotionType` recalc |
| `GetFriction()` / `SetFriction()` | `Body::Get/SetFriction()` |
| `ApplyImpulse()` | `BodyInterface::AddImpulse(bodyID, impulse, point)` |
| `ApplyTorque()` | `BodyInterface::AddTorque(bodyID, torque)` |
| `ApplyForce()` | `BodyInterface::AddForce(bodyID, force)` |
| `SetAngularVelocity()` / `SetLinearVelocity()` | `BodyInterface::Set[Linear/Angular]Velocity()` (wakes body) |
| `GetLinearVelocity()` / `GetAngularVelocity()` | `Body::GetLinearVelocity()` / `Body::GetAngularVelocity()` |
| `GetVelocity(point)` | `Body::GetPointVelocity(point)` |
| `GetLocalInertia()` | `Body::GetMotionProperties()->GetLocalSpaceInverseInertia()` (invert diagonal) |
| `GetGravity()` / `SetGravity()` | Get/set gravity factor × world gravity |
| `Get/SetLinearDamping()` | `Body::GetMotionProperties()->Get/SetLinearDamping()` |
| `Get/SetAngularDamping()` | `Body::GetMotionProperties()->Get/SetAngularDamping()` |
| `SetDamping(lin, ang)` | Set both damping values |
| `RefreshCollisions()` | `BodyInterface::ActivateBody()` to force collision re-check |
| `SuspendPhysics()` / `RestorePhysics()` | Remove/re-add body from physics system |
| `SuspendDynamics()` / `RestoreDynamics()` | Switch to `EMotionType::Static` / restore original type |
| `SetActive()` | `BodyInterface::ActivateBody()` or `DeactivateBody()` |
| `Get/SetCollisionGroup()` | `Body::SetCollisionGroup()` group ID |
| `Get/SetCollisionMask()` | `Body::SetCollisionGroup()` mask via `GroupFilterTable` |
| `SetRigidBody(rigid)` | Lock/unlock rotation axes |
| `GetReplica()` / `GetReplicaForSensors()` | Clone body with same settings |
| `Get/SetMargin()` | `ConvexShape::GetConvexRadius()` / recreate shape if changed |
| `Get/SetRadius()` | Store radius for FH spring support |
| `Get/SetLinVelocityMin/Max()` | Store values, clamp in `SimulationTick` |
| `Get/SetAngularVelocityMin/Max()` | Store values, clamp in `SimulationTick` |
| `AddCompoundChild()` / `RemoveCompoundChild()` | `MutableCompoundShape::AddShape()` / `RemoveShape()` |
| `IsDynamic()` / `IsCompound()` / `IsDynamicsSuspended()` / `IsPhysicsSuspended()` | Query stored flags |
| `ReinstancePhysicsShape()` | Rebuild Jolt shape from updated mesh data |
| `ReplacePhysicsShape()` | Replace body's shape via `BodyInterface::SetShape()` |
| `ReplicateConstraints()` | Clone constraints for group instances |
| `SetCcdMotionThreshold()` | `BodyInterface::SetMotionQuality(LinearCast)` if threshold > 0 |
| `SetCcdSweptSphereRadius()` | No direct mapping; CCD is automatic with `LinearCast` |
| `GetNewClientInfo()` / `SetNewClientInfo()` | Store/return `KX_ClientObjectInfo*` pointer |
| `SimulationTick(timestep)` | Clamp velocities (min/max), apply linear/angular factors |

### 3.3 `PHY_IVehicle` (17 methods)

| Method | Jolt Implementation |
|---|---|
| `AddWheel(...)` | `WheeledVehicleControllerSettings::mWheels.push_back(WheelSettingsWV)` |
| `GetNumWheels()` | `VehicleConstraint::GetNumWheels()` |
| `GetWheelPosition(i)` | `VehicleConstraint::GetWheel(i)->GetContactPosition()` |
| `GetWheelOrientationQuaternion(i)` | Compute from wheel rotation + steering |
| `GetWheelRotation(i)` | `Wheel::GetRotationAngle()` |
| `GetUserConstraintId()` / `GetUserConstraintType()` | Return stored IDs |
| `SetSteeringValue(steering, i)` | `WheeledVehicleController::SetSteerAngle(i, angle)` |
| `ApplyEngineForce(force, i)` | `WheeledVehicleController::SetDriverInput(fwd, ...)` or per-wheel torque |
| `ApplyBraking(braking, i)` | `WheeledVehicleController::SetDriverInput(..., brake, ...)` |
| `SetWheelFriction(friction, i)` | `WheelSettingsWV::mLongitudinalFriction` / `mLateralFriction` |
| `SetSuspensionStiffness(stiffness, i)` | `WheelSettings::mSuspensionSettings.mStiffness` |
| `SetSuspensionDamping(damping, i)` | `WheelSettings::mSuspensionSettings.mDamping` |
| `SetSuspensionCompression(compression, i)` | Map to suspension min/max length |
| `SetRollInfluence(influence, i)` | No direct equivalent; approximate via anti-roll bar settings |
| `SetCoordinateSystem(right, up, forward)` | `VehicleConstraintSettings::mUp`, `mForward` |
| `SetRayCastMask()` / `GetRayCastMask()` | Custom `ObjectLayerFilter` for vehicle ray casts |

### 3.4 `PHY_ICharacter` (15 methods)

| Method | Jolt Implementation |
|---|---|
| `Jump()` | `CharacterVirtual::SetLinearVelocity(currentVel + upDir * jumpSpeed)` |
| `OnGround()` | `CharacterVirtual::GetGroundState() == EGroundState::OnGround` |
| `GetGravity()` / `SetGravity()` | Store custom gravity vector, apply during `Update()` |
| `GetMaxJumps()` / `SetMaxJumps()` | Store/return max jump count |
| `GetJumpCount()` | Track jumps since last grounded |
| `SetWalkDirection()` / `GetWalkDirection()` | `CharacterVirtual::SetLinearVelocity()` for movement |
| `GetFallSpeed()` / `SetFallSpeed()` | Clamp downward velocity component |
| `GetMaxSlope()` / `SetMaxSlope()` | `CharacterVirtualSettings::mMaxSlopeAngle` |
| `GetJumpSpeed()` / `SetJumpSpeed()` | Store jump velocity magnitude |
| `SetVelocity(vel, time, local)` | `CharacterVirtual::SetLinearVelocity()` (transform if local) |
| `Reset()` | Reset jump count, velocity, ground state |

### 3.5 `PHY_IGraphicController` (for broadphase culling)

| Method | Jolt Implementation |
|---|---|
| `SetGraphicTransform()` | Update AABB in Jolt broadphase (or custom DBVT) |
| `Activate(active)` | Add/remove from culling broadphase |
| `SetLocalAabb(min, max)` | Store AABB, update broadphase proxy |
| `GetReplica(motionstate)` | Clone graphic controller |
| `GetNewClientInfo()` / `SetNewClientInfo()` | Store `KX_ClientObjectInfo*` for culling callbacks |

**Implementation:** Either use Jolt's internal broadphase for culling queries, or maintain a separate DBVT (like Bullet does with `btDbvtBroadphase`) for view frustum culling.

### 3.6 `PHY_ICollData` (for collision contact points)

| Method | Jolt Implementation |
|---|---|
| `GetNumContacts()` | Return stored contact count from `ContactListener` |
| `GetLocalPointA/B(index, first)` | Store contact points from `ContactListener::OnContactAdded` manifold |
| `GetWorldPoint(index, first)` | Store world-space contact points |
| `GetNormal(index, first)` | Store contact normals (flip based on `first`) |
| `GetCombinedFriction(index, first)` | Store from `ContactSettings::mCombinedFriction` |
| `GetCombinedRollingFriction(index, first)` | Not available in Jolt; return 0 |
| `GetCombinedRestitution(index, first)` | Store from `ContactSettings::mCombinedRestitution` |
| `GetAppliedImpulse(index, first)` | Not directly available; estimate from velocity change or return 0 |

---

## 4. Collision Callback System

### 4.1 Jolt `ContactListener` Implementation

The collision event system is critical. `KX_CollisionEventManager` depends on:
1. `AddCollisionCallback(response_class, callback, user)` — store callbacks for `PHY_OBJECT_RESPONSE`, `PHY_SENSOR_RESPONSE`, `PHY_BROADPH_RESPONSE`
2. `RequestCollisionCallback(ctrl)` / `RemoveCollisionCallback(ctrl)` — register/unregister controllers
3. `CallbackTriggers()` — iterate contacts, invoke stored callbacks with `JoltCollData`

**Jolt approach:** Implement `ContactListener::OnContactAdded()` and `OnContactPersisted()` to:
- Store contact manifold data in `JoltCollData` objects
- Check if either body's controller is registered for callbacks
- Invoke the stored `PHY_ResponseCallback` with proper response class
- Handle sensor contacts separately (sensors report via `ContactListener` but don't resolve penetration)

### 4.2 Broadphase Response

For `PHY_BROADPH_RESPONSE` (used by Near/Radar sensors):
- Implement `ObjectLayerFilter` or `BodyFilter` to allow/reject broad phase pairs
- Invoke the broadphase callback to let sensors decide if collision should proceed

### 4.3 Sensor Implementation

Jolt sensors: Set `BodyCreationSettings::mIsSensor = true`. They report contacts through `ContactListener` but don't resolve penetrations.

For Near/Radar sensors (`CreateSphereController()` / `CreateConeController()`):
- Create sensor bodies with sphere/cone shapes

### 4.4 Simulation Step Ordering and Performance

**Why Bullet's Sequence is Necessary (Not Just for Python Scripts)**

The simulation step order is critical for **physics correctness**, not just Python API compatibility:

- **Effector forces must be applied BEFORE `PhysicsSystem::Update()`** - Force fields (wind, gravity fields) need to affect the simulation during the physics step, not after
- **Motion states must be synced BEFORE the physics step** - Ensures all objects have correct starting positions/velocities
- **Motion states must be synced AFTER the physics step** - Game objects need to see the final positions after simulation
- **FH springs must be processed AFTER physics but BEFORE callbacks** - Special UPBGE feature that depends on post-physics state
- **Vehicle wheels must be synced AFTER physics** - Wheels need updated positions after simulation
- **Callbacks must fire LAST** - Game logic sees the final post-physics state

If this order is changed, physics behavior becomes **incorrect** (e.g., forces don't apply, wrong positions in callbacks), not just different for Python scripts.

**Jolt Performance Characteristics**

Jolt is designed for this pattern:

- **Job-based multithreaded architecture** - `PhysicsSystem::Update()` runs on worker threads
- **Thread-safe `BodyInterface`** - Designed for frequent position/velocity synchronization
- **No single-threaded bottleneck** - Synchronization overhead is negligible compared to physics computation
- **Optimized for active bodies only** - Use `GetActiveBodiesUnsafe()` to iterate only moving bodies

**Performance Best Practices for Motion State Synchronization**

```cpp
// BEFORE physics step: sync only active bodies
for (BodyID bodyID : physicsSystem.GetActiveBodiesUnsafe()) {
    Vec3 pos, rot;
    bodyInterface.GetPositionAndRotation(bodyID, pos, rot);
    // Write to game object motion state
}

// Apply effector forces...

// Run physics step
physicsSystem.Update(deltaTime, numSteps, subStep);

// AFTER physics step: sync only active bodies
for (BodyID bodyID : physicsSystem.GetActiveBodiesUnsafe()) {
    Vec3 pos, rot;
    bodyInterface.GetPositionAndRotation(bodyID, pos, rot);
    // Write to game object motion state
}
```

**Key Points:**
- Only sync **active bodies** (not static/sleeping) to avoid performance issues
- `BodyInterface::SetPositionAndRotation()` and `GetPositionAndRotation()` are lightweight
- Jolt's job system handles actual physics on worker threads
- Synchronization is just data transfer, not physics calculation
- Performance impact is minimal when following this pattern
- Add to physics system only when sensor is registered (`AddSensor()`)
- Remove when sensor unregisters (`RemoveSensor()`)

---

## 5. Conversion Pipeline (`ConvertObject`)

The `ConvertObject()` method in `JoltPhysicsEnvironment` must replicate the logic from `CcdPhysicsEnvironment::ConvertObject()`:

1. **Read Blender object flags** (`gameflag`, `gameflag2`) to determine body type (dynamic, rigid, static, sensor, character, soft)
2. **Create shape** from bounds type and mesh data via `JoltShapeBuilder`
3. **Build `BodyCreationSettings`** with all mapped properties:
   - Mass, friction, restitution, damping, margin
   - Motion type (Static/Dynamic/Kinematic)
   - Collision group/mask via `ObjectLayer` and `GroupFilter`
   - CCD quality if enabled
   - Gravity factor
4. **Handle compound children** — add child shapes to parent's `MutableCompoundShape`
5. **Handle compound parents** — create `MutableCompoundShape` as root shape
6. **Create body** via `BodyInterface::CreateBody()` and add to system
7. **Create `JoltPhysicsController`** wrapping the body
8. **Set client info** on controller for game object association
9. **Handle soft bodies** — create `SoftBodySharedSettings` from mesh, apply soft body properties
10. **Handle characters** — create `CharacterVirtual` with capsule shape, step height, max slope
11. **Apply linear/angular factors** for non-rigid dynamics
12. **Apply CCD settings** if `OB_CCD_RIGID_BODY` flag set
13. **Suspend dynamics** for parented objects via `BL_SceneConverter::AddPendingSuspendDynamics()`

---

## 6. Jolt Initialization and Threading

### 6.1 Jolt System Setup (in `JoltPhysicsEnvironment` constructor)

```
JPH::RegisterDefaultAllocator();
JPH::Factory::sInstance = new JPH::Factory();
JPH::RegisterTypes();
// Create PhysicsSystem with:
//   - TempAllocator (e.g. TempAllocatorImpl with 10MB)
//   - JobSystem (JobSystemThreadPool or JobSystemSingleThreaded)
//   - BroadPhaseLayerInterface (custom, mapping UPBGE groups)
//   - ObjectVsBroadPhaseLayerFilter
//   - ObjectLayerPairFilter
//   - ContactListener (JoltContactListener)
//   - SoftBodyContactListener (for soft body contacts)
```

### 6.2 Collision Layer Design

Map UPBGE's `CollisionFilterGroups` to Jolt `ObjectLayer`:

| UPBGE Filter | Jolt ObjectLayer | BroadPhaseLayer |
|---|---|---|
| `DynamicFilter` | `LAYER_DYNAMIC = 0` | `BP_MOVING` |
| `StaticFilter` | `LAYER_STATIC = 1` | `BP_NON_MOVING` |
| `KinematicFilter` | `LAYER_KINEMATIC = 2` | `BP_MOVING` |
| `SensorFilter` | `LAYER_SENSOR = 3` | `BP_MOVING` or `BP_NON_MOVING` |
| `CharacterFilter` | `LAYER_CHARACTER = 4` | `BP_MOVING` |
| `DebrisFilter` | `LAYER_DEBRIS = 5` | `BP_MOVING` |

Implement `ObjectLayerPairFilter::ShouldCollide()` to replicate Bullet's `NeedsCollision` logic, respecting `m_collisionGroup` / `m_collisionMask` bitfields via Jolt's `GroupFilterTable`.

### 6.3 Coordinate System

Jolt uses Y-up, Blender uses Z-up. All conversions in `JoltMathUtils.h`:

```cpp
// Blender → Jolt
inline JPH::Vec3 ToJolt(const MT_Vector3 &v) { return JPH::Vec3(v.x(), v.z(), -v.y()); }
inline JPH::Quat ToJolt(const MT_Quaternion &q) { return JPH::Quat(q.x(), q.z(), -q.y(), q.w()); }

// Jolt → Blender
inline MT_Vector3 ToMT(const JPH::Vec3 &v) { return MT_Vector3(v.GetX(), -v.GetZ(), v.GetY()); }
inline MT_Quaternion ToMT(const JPH::Quat &q) { return MT_Quaternion(q.GetX(), -q.GetZ(), q.GetY(), q.GetW()); }
```

---

## 7. Python API Compatibility

The Python API works through abstract interfaces (`PHY_I*`), so most Python bindings in these files work unchanged:

| File | Status |
|---|---|
| `KX_PyConstraintBinding.cpp` | Works via `PHY_IPhysicsEnvironment` — no changes needed |
| `KX_ConstraintWrapper.cpp` | Works via `PHY_IConstraint` — no changes needed |
| `KX_VehicleWrapper.cpp` | Works via `PHY_IVehicle` — no changes needed |
| `KX_CharacterWrapper.cpp` | Works via `PHY_ICharacter` — no changes needed |
| `KX_GameObject.cpp` | Works via `PHY_IPhysicsController` — no changes needed |
| `KX_CollisionContactPoints.cpp` | Works via `PHY_ICollData` — no changes needed |

**Key insight:** Because UPBGE uses abstract interfaces, the Python API should work without modification as long as all `PHY_I*` methods are properly implemented.

---

## 8. Debug Drawing

Implement `JoltDebugDraw` inheriting from Jolt's `DebugRendererSimple`:

| Jolt Method | UPBGE Implementation |
|---|---|
| `DrawLine(from, to, color)` | Convert to UPBGE debug line drawing |
| `DrawTriangle(v1, v2, v3, color)` | Convert to UPBGE triangle drawing |

Call `PhysicsSystem::DrawBodies()` and `PhysicsSystem::DrawConstraints()` in `DebugDrawWorld()`.

Support debug modes from `SetDebugMode()`:
- Wireframe shapes
- AABBs
- Contact points
- Constraint frames

---

## 9. Implementation Phases

### Phase 1: Setup (2–3 days)
- Add Jolt to `extern/joltphysics/`
- CMake integration with `WITH_JOLT`
- Add `UseJolt` to physics engine enum

### Phase 2: Core Backend (3–4 weeks)
- `JoltPhysicsEnvironment` — system init, stepping, gravity, debug mode
- `JoltPhysicsController` — all 55+ methods for rigid bodies
- `JoltMathUtils` — coordinate conversions
- `JoltShapeBuilder` — all 8 shape types
- `JoltContactListener` + `JoltCollData` — collision callbacks
- `JoltMotionState` — transform synchronization

### Phase 3: Conversion Pipeline (1 week)
- `ConvertObject()` — full object conversion from Blender data
- `SetupObjectConstraints()` / `CreateRigidBodyConstraint()`
- Collision group/mask mapping
- FH springs implementation
- Update `BL_Converter.cpp`

### Phase 4: Constraints (1 week)
- `JoltConstraint` — all 7 constraint types
- Spring constraints with motors
- Breaking thresholds
- Rigid body constraint creation from `RigidBodyCon`

### Phase 5: Advanced Features (1–2 weeks)
- `JoltVehicle` — raycast vehicle with wheels
- `JoltCharacter` — character controller (prefer `CharacterVirtual`)
- `JoltGraphicController` — broadphase culling
- Sensor objects (sphere/cone controllers)

### Phase 6: Soft Bodies (1–2 weeks)
- Soft body creation from mesh
- Edge/bend/volume constraints
- Vertex mapping to graphics mesh
- Soft body contact listener
- Property mapping (stiffness → compliance)

### Phase 7: Testing & Polish (2 weeks)
- Test all rigid body properties
- Test all constraint types
- Test vehicles and characters
- Test soft bodies
- Test Python API
- Test collision callbacks and sensors
- Performance comparison with Bullet
- Regression testing

### Phase 8: Documentation (2–3 days)
- Document property mappings and behavioral differences
- Add Jolt option to UI physics engine selector
- Build instructions

**Total Estimated Time: 8–12 weeks**

---

## 10. Risk Mitigation

- **Keep Bullet as default** until Jolt is fully validated
- **Implement incrementally** — rigid bodies first, then constraints, then vehicles/characters, then soft bodies
- **Rolling friction / anisotropic friction:** No direct Jolt equivalent — implement via `ContactListener` customization
- **Linear/angular factors:** No direct Jolt equivalent — implement via velocity clamping in step callback
- **Soft body limitations:** Jolt soft-soft collision not supported; some Bullet soft body params have no mapping
- **ERP/CFM:** Not applicable to Jolt's solver — document behavioral differences
- **FH springs:** Must be reimplemented from scratch using Jolt ray casting

---

## 12. Gaps Identified from Jolt Documentation Review

The following items were missing or incomplete in the original plan and must be addressed.

### 12.1 Body Lifecycle & Batch Operations (CRITICAL)

Jolt bodies have a strict lifecycle: `CreateBody` → `AddBody` → `RemoveBody` → `DestroyBody`. Bodies **cannot** be `new`/`delete`d directly. Constraints must be removed before removing their bodies.

- **Batch body insertion:** Jolt strongly recommends `BodyInterface::AddBodiesPrepare` + `AddBodiesFinalize` when adding many bodies at once (e.g., scene load). Adding bodies one-by-one creates an inefficient broadphase and can exhaust internal nodes. Call `PhysicsSystem::OptimizeBroadPhase()` as fallback after many individual adds.
- **Batch body removal:** `BodyInterface::RemoveBodies` for bulk removal.
- **`ActivateBodiesInAABox`:** When removing a body, Jolt does NOT wake surrounding bodies. Must manually call `BodyInterface::ActivateBodiesInAABox(removedBodyBounds)` if this behavior is desired (Bullet does this automatically).
- **`mAllowDynamicOrKinematic`:** Must be set on static bodies that may later switch to dynamic/kinematic (e.g., `SuspendDynamics`/`RestoreDynamics` converts dynamic→static→dynamic). Without this flag, the body has no `MotionProperties` and the switch will fail.

### 12.2 Breakable Constraints (IMPORTANT — different from Bullet)

Jolt has **no built-in breaking threshold** on constraints. The plan's `SetBreakingThreshold()` mapping must be implemented manually:
1. After each `PhysicsSystem::Update()`, iterate all constraints with a breaking threshold.
2. Check `GetTotalLambdaPosition()` / `GetTotalLambdaRotation()` (the impulse applied to maintain the constraint).
3. If lambda exceeds the threshold, call `Constraint::SetEnabled(false)` and optionally remove the constraint.

This is fundamentally different from Bullet's `btTypedConstraint::m_breakingImpulseThreshold`.

### 12.3 Constraint Motors (IMPORTANT — not fully covered)

Jolt constraint motors (`MotorSettings`) have 3 states: **Off**, **Velocity**, **Position**. The plan references motors for `RBC_TYPE_MOTOR` but doesn't detail:
- `MotorSettings::mSpringSettings.mFrequency` (Hz) — controls position motor stiffness. Valid range: `(0, 0.5 * simulation_frequency]`.
- `MotorSettings::mSpringSettings.mDamping` — 0 = oscillation, 1 = critical damping, >1 = overdamped.
- `MotorSettings::mMinForceLimit` / `mMaxForceLimit` — for linear motors (Newtons). Usually `[-FLT_MAX, FLT_MAX]`.
- `MotorSettings::mMinTorqueLimit` / `mMaxTorqueLimit` — for angular motors (Newton·meters).
- Mapping Bullet's `motor_lin_target_velocity` → Jolt `SliderConstraint::SetMotorState(EMotorState::Velocity)` + `SetTargetVelocity()`.
- Mapping Bullet's `motor_ang_target_velocity` → Jolt `HingeConstraint::SetMotorState(EMotorState::Velocity)` + `SetTargetAngularVelocity()`.

### 12.4 Collision Filtering Pipeline (INCOMPLETE)

The plan covers `ObjectLayer`, `BroadPhaseLayer`, and `GroupFilterTable`, but Jolt's full filtering pipeline has 6 stages:

1. **`ObjectVsBroadPhaseLayerFilter`** — which broadphase trees to visit
2. **`ObjectLayerPairFilter`** — object layer vs object layer
3. **`GroupFilter`** — fine-grained per-body pair (e.g., ragdoll connected bodies). Runs after AABB overlap.
4. **`BodyFilter`** — for collision queries (ray casts, shape casts). Replaces GroupFilter for queries.
5. **`ShapeFilter` / `SimShapeFilter`** — filter individual sub-shapes of compounds. `SimShapeFilter` via `PhysicsSystem::SetSimShapeFilter()`.
6. **`ContactListener`** — last resort, most expensive.

**Missing from plan:**
- `GroupFilter` / `GroupFilterTable` for disabling collisions between constraint-connected bodies (ragdolls, vehicles). Bullet uses `DISABLE_LINKED_COLLISION` flag.
- `SimShapeFilter` for compound sub-shape filtering during simulation.
- `BodyFilter` for ray cast filtering (maps to UPBGE's `PHY_IRayCastFilterCallback`).

**Recommended approach:** Use `ObjectLayerPairFilterMask` + `BroadPhaseLayerInterfaceMask` (Jolt's mask-based system) instead of the table-based approach. This directly mirrors Bullet's `collisionFilterGroup` / `collisionFilterMask` 16-bit bitfields. Consider setting `OBJECT_LAYER_BITS=32` in CMake for more headroom.

### 12.5 Ghost Collisions / Internal Edge Removal

Jolt has `BodyCreationSettings::mEnhancedInternalEdgeRemoval` to fix ghost collisions on mesh shapes (bodies bumping on internal triangle edges). Also `MeshShapeSettings::mActiveEdgeCosThresholdAngle`.

**Action:** Enable `mEnhancedInternalEdgeRemoval` by default for bodies on `MeshShape` / `HeightFieldShape`. This significantly improves collision quality on terrain and static geometry.

### 12.6 Center of Mass Handling

Jolt automatically recenters shapes around their center of mass. All shape-local functions operate in COM space. This affects:
- Transforms returned from `Body::GetPosition()` are at COM, not shape origin.
- `Shape::GetCenterOfMass()` must be used to convert back to shape-space.
- `OffsetCenterOfMassShape` can shift COM (useful for vehicles to lower center of gravity).

**Action:** The `JoltMathUtils` coordinate conversion must account for COM offset when syncing transforms to/from `PHY_IMotionState`.

### 12.7 Shape Scaling at Runtime

Jolt `ScaledShape` has **no `SetScale()` method** (for thread safety). To dynamically rescale a body:
1. Create new `ScaledShape` wrapping the original shape with new scale.
2. Call `BodyInterface::SetShape(bodyID, newShape, ...)`.
3. Use `Shape::ScaleShape()` for safe scaling that handles rotated compound sub-shapes (avoids shearing).
4. `Shape::IsValidScale()` to check if a scale is valid for a given shape type.

This maps to UPBGE's `SetScaling()` and `ReinstancePhysicsShape()`.

### 12.8 Dynamic Mesh Shapes

Jolt allows `MeshShape` on dynamic/kinematic bodies but with restrictions:
- Cannot collide with other mesh or heightfield shapes.
- Mass and inertia must be provided manually via `BodyCreationSettings::mOverrideMassProperties = EOverrideMassProperties::MassAndInertiaProvided`.
- Not recommended for performance.

**Action:** When GImpact is requested (`m_bGimpact`), use `ConvexHullShape` for dynamic meshes (matching Bullet behavior). For static meshes, use `MeshShape`.

### 12.9 `SuspendDynamics(ghost)` Pattern

UPBGE's `SuspendDynamics(ghost=true)` converts a dynamic body to static with `CF_NO_CONTACT_RESPONSE` (ghost mode — collisions detected but not resolved). In Jolt:
1. Save current motion type, mass, collision group/mask.
2. Set `BodyInterface::SetMotionType(bodyID, EMotionType::Static)`.
3. If `ghost=true`, call `Body::SetIsSensor(true)` to make it a sensor (contacts reported but not resolved).
4. On `RestoreDynamics()`, restore original motion type, mass, and clear sensor flag.

**Requires** `mAllowDynamicOrKinematic = true` at body creation (see 12.1).

### 12.10 Tracked Vehicles & Motorcycles

Jolt supports 3 vehicle types: `WheeledVehicleController`, `TrackedVehicleController`, `MotorcycleController`. The plan only covers wheeled vehicles. While UPBGE's `PHY_IVehicle` interface is designed for wheeled vehicles, document:
- `TrackedVehicleController` for tank-like vehicles (uses tracks instead of wheels).
- `MotorcycleController` extends wheeled with lean angle support.

### 12.11 CharacterVirtual Advanced Features (INCOMPLETE)

Missing from plan:
- **Inner body:** `CharacterVirtualSettings::mInnerBodyShape` creates a rigid body that follows CharacterVirtual, making it detectable by sensors and ray casts. Without this, `CastRay` won't hit the character.
- **`CharacterContactListener`:** Separate from regular `ContactListener`. CharacterVirtual uses its own listener for collision callbacks.
- **`CharacterVsCharacterCollision`:** Interface for CharacterVirtual vs CharacterVirtual collision handling.
- **`ExtendedUpdate()`:** Provides stair stepping and ground sticking in one call.
- **`PostSimulation()`:** Must be called after `PhysicsSystem::Update()` for `Character` (rigid body variant) to update ground contacts.
- **Local coordinate system:** `CharacterVirtualSettings::mUp` for custom up-axis.

### 12.12 Contact Lifecycle (INCOMPLETE)

The plan mentions `OnContactAdded` and `OnContactPersisted` but the full `ContactListener` interface is:
- `OnContactValidate(body1, body2, ...)` → Can **reject** contacts before resolution. Returns `ValidateResult`. Maps to `PHY_BROADPH_RESPONSE` callback.
- `OnContactAdded(body1, body2, manifold, settings)` → New contact. Can modify `ContactSettings` (friction, restitution). Maps to `PHY_OBJECT_RESPONSE` / `PHY_SENSOR_RESPONSE`.
- `OnContactPersisted(body1, body2, manifold, settings)` → Existing contact still active. Same callback mapping.
- `OnContactRemoved(subShapePair)` → Contact lost. **Important** for collision event manager to fire "end collision" events.

**Note:** `OnContactValidate` and `OnContactAdded`/`OnContactPersisted` are called from **multiple threads** during `PhysicsSystem::Update()`. Must be thread-safe. Only body reading is safe in callbacks.

### 12.13 `PhysicsSystem::Init` Parameters

The constructor needs careful tuning:
- `maxBodies` — Max number of bodies. Must be pre-allocated. Suggest 65536 as default.
- `numBodyMutexes` — 0 = auto-detect. Good default.
- `maxBodyPairs` — Max body pairs for broadphase. Suggest 65536.
- `maxContactConstraints` — Max contact constraints. Suggest 65536.

These affect memory usage and hard limits. The plan should expose these as configurable settings.

### 12.14 `PhysicsStepListener` for Custom Per-Step Logic

Jolt's `PhysicsStepListener::OnStep()` is called at the start of each simulation step from within `PhysicsSystem::Update()`. This is the correct place for:
- FH springs implementation (ray cast + force application)
- Velocity clamping (min/max linear/angular velocity)
- Linear/angular factor enforcement
- Anisotropic friction application
- Any custom force/torque application

Register via `PhysicsSystem::AddStepListener()`. Note: step listeners run from **multiple threads** — must be thread-safe or use the non-threaded variant.

### 12.15 `BodyActivationListener`

Jolt provides `BodyActivationListener::OnBodyActivated(bodyID)` / `OnBodyDeactivated(bodyID)`. Useful for:
- Tracking active body count for performance monitoring.
- Triggering UPBGE activation state updates.
- Called from within `PhysicsSystem::Update()` — only read body data in callbacks.

### 12.16 `PhysicsSettings` Tuning Parameters

Beyond sleep settings, `PhysicsSettings` has important tunable parameters not mentioned:
- `mNumVelocitySteps` (default 10) — velocity solver iterations per step
- `mNumPositionSteps` (default 2) — position solver iterations per step
- `mBaumgarte` (default 0.2) — position correction factor (replaces Bullet ERP to some extent)
- `mSpeculativeContactDistance` — distance for speculative contacts
- `mPenetrationSlop` — allowed penetration
- `mMinVelocityForRestitution` — minimum velocity for restitution to apply

### 12.17 `Body::SetUserData` / `Shape::GetUserData`

Jolt provides `uint64` user data per body and per shape:
- `Body::SetUserData(uint64)` — store game object pointer (cast `KX_ClientObjectInfo*` to `uint64`).
- `Shape::SetUserData(uint64)` — store shape-specific data.

This replaces Bullet's `setUserPointer()` and is how `JoltContactListener` will look up game objects from body IDs.

### 12.18 Soft Body Constraint Types (INCOMPLETE)

Jolt soft bodies support more constraint types than listed:
- **Edge constraints** (basic distance)
- **Dihedral bend constraints** (angle between adjacent triangles)
- **Cosserat rod constraints** (oriented edges for plant leaves, hair)
- **Tetrahedron volume constraints** (3D volume preservation)
- **Long range attachment (tether) constraints** (prevent excessive stretch)
- **Skinning constraints** (bind to animated skeleton)

The plan should map Bullet's `CCD_BSB_BENDING_CONSTRAINTS` to Jolt's dihedral bend or distance-based bend, and consider using tether constraints for stability.

### 12.19 `MergeEnvironment` Complexity

UPBGE's `MergeEnvironment()` transfers controllers between physics worlds (for scene merging). In Bullet, this is straightforward (remove from one `btDynamicsWorld`, add to another). In Jolt:
- Bodies cannot be shared between `PhysicsSystem` instances.
- Must `RemoveBody` + `DestroyBody` from source, then `CreateBody` (using `Body::GetBodyCreationSettings`) + `AddBody` in destination.
- Constraints must be recreated similarly using `Constraint::GetConstraintSettings`.
- This is significantly more complex than Bullet's approach.

### 12.20 Culling Tree for `CullingTest`

Bullet uses a **separate** `btDbvtBroadphase` (`m_cullingTree`) with a `btNullPairCache` purely for view frustum culling (no collision, just spatial queries). This is used by `PHY_IGraphicController` / `CcdGraphicController`.

In Jolt, options:
1. Use Jolt's `BroadPhaseQuery::CollideAABox()` for culling (but this queries the physics broadphase, which may not contain all renderable objects).
2. Maintain a **separate Jolt broadphase** or custom AABB tree for culling (matching Bullet's approach).
3. Use Jolt's broadphase but add non-physics bodies with `EmptyShape` just for culling.

**Recommended:** Option 2 — maintain a separate spatial data structure for culling, decoupled from physics.

### 12.21 Threading Model

Jolt requires a `JobSystem` at initialization:
- `JobSystemThreadPool` — Jolt manages its own thread pool. Specify number of threads.
- `JobSystemSingleThreaded` — All work on calling thread.

**Thread Count Configuration:**
```cpp
// Read from Blender UI setting or command line
int physicsThreads = SYS_GetCommandLineInt(syshandle, "physics_threads", -1);
if (physicsThreads <= 0) {
    physicsThreads = max(1, std::thread::hardware_concurrency() - 1);
}
physicsThreads = clamp(physicsThreads, 1, std::thread::hardware_concurrency());

JobSystemThreadPool* jobSystem = new JobSystemThreadPool(
    "JoltPhysics",
    physicsThreads,
    0,              // Max jobs (0 = unlimited)
    maxBarriers,    // Max barriers (0 = auto)
    16 * 1024       // Stack size per thread
);
```

**Blender UI Setting:**
- Add "Physics Threads" control under physics engine selection in Game Properties
- Type: Integer spinner
- Range: 1 to max CPU cores
- Default: -1 (auto = max(1, cores-1))
- Stored in scene/game properties
- Applied at physics system initialization

**Thread Count Impact:**
| Threads | Use Case | Performance |
|---------|----------|-------------|
| 1 | Debugging, single-core systems | Slowest, no parallelism |
| 2-4 | Typical dual/quad-core systems | Good parallelism |
| max(1, cores-1) | Recommended for production | Best performance |
| All cores | May cause contention with game logic | Can hurt performance |

**Important:**
- Thread count must be set at initialization (cannot change after PhysicsSystem created)
- Don't use all cores - reserve at least 1 for main thread (game logic, rendering, input)
- JobSystem manages its own threads, separate from UPBGE's threading
- Contact callbacks run from multiple threads - must be thread-safe

UPBGE runs game logic on the main thread. Consider:
- Use `JobSystemThreadPool` with configured thread count for physics
- UPBGE cannot read/write bodies during `PhysicsSystem::Update()`. All body manipulation must happen before or after the update call
- Use the **locking** `BodyInterface` (`PhysicsSystem::GetBodyInterface()`) for safe access, or the **non-locking** variant (`GetBodyInterfaceNoLock()`) when threading guarantees are managed manually.

### 12.22 `TempAllocator` Sizing

**Recommended for maximum object count and best performance:**

Use `TempAllocatorImpl` with **32MB minimum**:
- Fast arena allocator (bump allocation, O(1), no fragmentation)
- Best cache performance with contiguous memory
- Supports complex scenes with 5000+ active bodies, mesh shapes, many contacts

**Configuration:**
```cpp
TempAllocatorImpl* allocator = new TempAllocatorImpl(32 * 1024 * 1024);

// Monitor peak usage and adjust
size_t peakUsage = 0;
void PhysicsUpdate() {
    size_t used = allocator->GetNumAllocatedBytes();
    peakUsage = max(peakUsage, used);
    // PhysicsSystem::Update()...
    // Allocator auto-resets after each frame
}
```

**When to increase size:**
- Peak usage approaches 80% of current size
- Supporting scenes with 5000+ active bodies, complex mesh shapes, many contacts

**`TempAllocatorMalloc`** only as fallback for debugging (slower, uses system heap).

### 12.23 Water Buoyancy

Jolt supports buoyancy calculations for rigid bodies (not yet for soft bodies). Bullet also has buoyancy. UPBGE does **not** currently expose buoyancy in its physics UI, so this is low priority but worth noting for future use.

---

## 13. Updated Success Criteria

- All `PHY_I*` interface methods implemented and functional
- All Blender UI physics properties properly mapped
- Collision callbacks work correctly with `KX_CollisionEventManager`
- Python API (`bge.constraints`, `bge.types.KX_GameObject` physics attributes) works unchanged
- Vehicles, characters, and soft bodies functional
- Debug visualization working
- No regressions in Bullet backend
- Performance comparable or better than Bullet
- Batch body operations used for scene loading
- Breakable constraints working via lambda checking
- Ghost collision mitigation enabled on mesh shapes
- `SuspendDynamics(ghost)` pattern correctly implemented
- Constraint motors working for all applicable constraint types
- Full collision filtering pipeline (6 stages) functional
