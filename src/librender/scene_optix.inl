#include <optix.h>
#include <optix_stubs.h>
#include "librender_ptx.h"
#include <iomanip>

#include <mitsuba/render/optix_structs.h>

NAMESPACE_BEGIN(mitsuba)

#define rt_check(err)  __rt_check(s.context, err, __FILE__, __LINE__)

void __rt_check(OptixDeviceContext context, OptixResult errval, const char *file, const int line) {
    if (errval != OPTIX_SUCCESS) {
        const char *message;
        message = optixGetErrorString(errval);
        if (errval == 1546)
            message = "Failed to load OptiX library! Very likely, your NVIDIA graphics "
                "driver is too old and not compatible with the version of OptiX that is "
                "being used. In particular, OptiX 6.5 requires driver revision R435.80 or newer.";
        fprintf(stderr,
                "rt_check(): OptiX API error = %04d (%s) in "
                "%s:%i.\n", (int) errval, message, file, line);
        exit(EXIT_FAILURE);
    }
}

static void context_log_cb( unsigned int level, const char* tag, const char* message, void* /*cbdata */)
{
    std::cerr << "[" << std::setw( 2 ) << level << "][" << std::setw( 12 ) << tag << "]: "
    << message << "\n";
}

constexpr size_t kOptixVariableCount = 30;
constexpr int kDeviceID = 0;

struct OptixState {
    OptixDeviceContext context;
    OptixPipeline pipeline = nullptr;
    OptixModule module = nullptr;
    OptixProgramGroup program_groups[4];
    OptixShaderBindingTable sbt = {};
    OptixTraversableHandle accel;
    void* accel_buffer;
};

template <typename T>
struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) SbtRecord
{
    char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    T data;
};

struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) EmptySbtRecord
{
    char header[OPTIX_SBT_RECORD_HEADER_SIZE];
};

typedef EmptySbtRecord RayGenSbtRecord;
typedef EmptySbtRecord MissSbtRecord;
typedef SbtRecord<HitGroupData>   HitGroupSbtRecord;

#define CHECKPOINT()\
    do {\
        std::cerr << "Reached line " << __LINE__ << " in file" << __FILE__ << "...";\
        cuda_eval(); cuda_sync();\
        std::cerr << "(done)" << std::endl;\
    } while(0)

MTS_VARIANT void Scene<Float, Spectrum>::accel_init_gpu(const Properties &/*props*/) {
    m_accel = new OptixState();
    OptixState &s = *(OptixState *) m_accel;

    CUcontext cuCtx = 0;  // zero means take the current context
    rt_check(optixInit());
    OptixDeviceContextOptions options = {};
    options.logCallbackFunction       = &context_log_cb;
    options.logCallbackLevel          = 4;
    rt_check(optixDeviceContextCreate(cuCtx, &options, &s.context));

    // Pipeline generation
    {
        OptixPipelineCompileOptions pipeline_compile_options = {};
        OptixModuleCompileOptions module_compile_options = {};
        module_compile_options.maxRegisterCount     = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
        module_compile_options.optLevel             = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
        module_compile_options.debugLevel           = OPTIX_COMPILE_DEBUG_LEVEL_LINEINFO;

        pipeline_compile_options.usesMotionBlur        = false;
        pipeline_compile_options.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY; // TODO: ??
        pipeline_compile_options.numPayloadValues      = 3; // TODO: ??
        pipeline_compile_options.numAttributeValues    = 3; // TODO: ??
        pipeline_compile_options.pipelineLaunchParamsVariableName = "params";

        rt_check(optixModuleCreateFromPTX(
            s.context,
            &module_compile_options,
            &pipeline_compile_options,
            (const char *)optix_rt_ptx,
            optix_rt_ptx_size,
            nullptr,
            nullptr,
            &s.module
        ));

        OptixProgramGroupOptions program_group_options   = {}; // Initialize to zeros

        OptixProgramGroupDesc prog_group_descs[4];
        memset(prog_group_descs, 0, sizeof(prog_group_descs));

        prog_group_descs[0].kind                     = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        prog_group_descs[0].raygen.module            = s.module;
        prog_group_descs[0].raygen.entryFunctionName = "__raygen__rg";

        prog_group_descs[1].kind                   = OPTIX_PROGRAM_GROUP_KIND_MISS;
        prog_group_descs[1].miss.module            = s.module;
        prog_group_descs[1].miss.entryFunctionName = "__miss__ms";

        prog_group_descs[2].kind                         = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        prog_group_descs[2].hitgroup.moduleCH            = s.module;
        prog_group_descs[2].hitgroup.entryFunctionNameCH = "__closesthit__ch";

#if !defined(MTS_OPTIX_DEBUG)
        // TODO: find how to enable optix print
        pipeline_compile_options.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
        const unsigned int num_program_groups = 3;
#else
        pipeline_compile_options.exceptionFlags =
              OPTIX_EXCEPTION_FLAG_STACK_OVERFLOW
            | OPTIX_EXCEPTION_FLAG_TRACE_DEPTH
            | OPTIX_EXCEPTION_FLAG_USER
            | OPTIX_EXCEPTION_FLAG_DEBUG;

        prog_group_descs[3].kind                         = OPTIX_PROGRAM_GROUP_KIND_EXCEPTION;
        prog_group_descs[3].hitgroup.moduleCH            = s.module;
        prog_group_descs[3].hitgroup.entryFunctionNameCH = "__exception__err";
        const unsigned int num_program_groups = 4;
#endif

        rt_check(optixProgramGroupCreate(
            s.context,
            prog_group_descs,
            num_program_groups,
            &program_group_options,
            nullptr,
            nullptr,
            s.program_groups
        ));

        OptixPipelineLinkOptions pipeline_link_options = {};
        pipeline_link_options.maxTraceDepth          = 1; // TODO: ??
        pipeline_link_options.debugLevel             = OPTIX_COMPILE_DEBUG_LEVEL_FULL;
        pipeline_link_options.overrideUsesMotionBlur = false;
        rt_check(optixPipelineCreate(
            s.context,
            &pipeline_compile_options,
            &pipeline_link_options,
            s.program_groups,
            num_program_groups,
            nullptr,
            nullptr,
            &s.pipeline
        ));
        // rt_check(optixModuleDestroy(module));
    } // end pipeline generation

    // Shader Binding Table generation and acceleration data structure building
    {
        uint32_t shapes_count = m_shapes.size();
        void* records = cuda_malloc(sizeof(RayGenSbtRecord) + sizeof(MissSbtRecord) + sizeof(HitGroupSbtRecord) * shapes_count);

        RayGenSbtRecord raygen_sbt;
        rt_check(optixSbtRecordPackHeader( s.program_groups[0], &raygen_sbt));
        void* raygen_record = records;
        cuda_memcpy_to_device(raygen_record, &raygen_sbt, sizeof(RayGenSbtRecord));

        MissSbtRecord miss_sbt;
        rt_check(optixSbtRecordPackHeader(s.program_groups[1], &miss_sbt));
        void* miss_record = (char*)records + sizeof(RayGenSbtRecord);
        cuda_memcpy_to_device(miss_record, &miss_sbt, sizeof(MissSbtRecord));

        // Allocate hitgroup records array
        void* hitgroup_records = (char*)records + sizeof(RayGenSbtRecord) + sizeof(MissSbtRecord);

        // Create build input of instances
        OptixBuildInput instances_build_input = {};
        instances_build_input.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
        instances_build_input.instanceArray.numInstances  = shapes_count;
        instances_build_input.instanceArray.instances     = (CUdeviceptr)cuda_malloc(sizeof(OptixInstance) * shapes_count);

        // Setup template instance
        OptixInstance inst = {
            {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},   // transform
            0u,                                     // instance ID <- placeholder
            0u,                                     // sbt offset <- placeholder
            255u,                                   // Visibility mask
            OPTIX_INSTANCE_FLAG_NONE,
            0ull,                                   // traversable handle <- placeholder
            {0u, 0u}                                // padding
        };

        uint32_t shape_index = 0;
        std::vector<HitGroupSbtRecord> hg_sbts(shapes_count);
        std::vector<OptixInstance> instances(shapes_count);

        for (Shape* shape: m_shapes) {
            // compute optix geometry for this shape
            shape->optix_geometry(s.context);

            // Setup the hitgroup record and copy it to the hitgroup records array
            rt_check(optixSbtRecordPackHeader(s.program_groups[2], &hg_sbts[shape_index]));
            memcpy(&hg_sbts[shape_index].data, &shape->optix_hit_group_data(), sizeof(HitGroupData));

            // Setup instance for this shape and copy it into the instances array
            inst.instanceId = shape_index;
            inst.sbtOffset = shape_index;
            inst.traversableHandle = shape->optix_traversable_handle();
            memcpy(&instances[shape_index], &inst, sizeof(OptixInstance));

            ++shape_index;
        }
        // Copy HitGroupRecords to the GPU
        cuda_memcpy_to_device(hitgroup_records, hg_sbts.data(), sizeof(HitGroupSbtRecord) * shapes_count);
        // Copy Instances to GPU
        cuda_memcpy_to_device((void*)instances_build_input.instanceArray.instances, instances.data(), sizeof(OptixInstance) * shapes_count);

        s.sbt.raygenRecord                = (CUdeviceptr)raygen_record;
        s.sbt.missRecordBase              = (CUdeviceptr)miss_record;
        s.sbt.missRecordStrideInBytes     = sizeof(MissSbtRecord);
        s.sbt.missRecordCount             = 1;
        s.sbt.hitgroupRecordBase          = (CUdeviceptr)hitgroup_records;
        s.sbt.hitgroupRecordStrideInBytes = sizeof(HitGroupSbtRecord);
        s.sbt.hitgroupRecordCount         = shapes_count;

        OptixAccelBuildOptions accel_options = {};
        accel_options.buildFlags = OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
        accel_options.operation  = OPTIX_BUILD_OPERATION_BUILD;

        OptixAccelBufferSizes gas_buffer_sizes;
        rt_check(optixAccelComputeMemoryUsage(s.context, &accel_options, &instances_build_input, 1, &gas_buffer_sizes));
        void* d_temp_buffer_gas = cuda_malloc(gas_buffer_sizes.tempSizeInBytes);

        // non-compacted output
        // TODO: check that this allocation logic works
        void* d_buffer_temp_output_gas_and_compacted_size = cuda_malloc(gas_buffer_sizes.outputSizeInBytes + 8);

        OptixAccelEmitDesc emitProperty = {};
        emitProperty.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
        emitProperty.result = (CUdeviceptr)((char*)d_buffer_temp_output_gas_and_compacted_size + gas_buffer_sizes.outputSizeInBytes);

        rt_check(optixAccelBuild(
            s.context,
            0,              // CUDA stream
            &accel_options,
            &instances_build_input,
            1,              // num build inputs
            (CUdeviceptr)d_temp_buffer_gas,
            gas_buffer_sizes.tempSizeInBytes,
            (CUdeviceptr)d_buffer_temp_output_gas_and_compacted_size,
            gas_buffer_sizes.outputSizeInBytes,
            &s.accel,
            &emitProperty,  // emitted property list
            1               // num emitted properties
        ));

        cuda_free((void*)d_temp_buffer_gas);
        // s.accel_buffer = d_buffer_temp_output_gas_and_compacted_size;

        // TODO: check if this is really usefull considering enoki's way of handling GPU memory
        size_t compacted_gas_size;
        cuda_memcpy_from_device(&compacted_gas_size, (void*)emitProperty.result, sizeof(size_t));
        if (compacted_gas_size < gas_buffer_sizes.outputSizeInBytes) {
            s.accel_buffer = cuda_malloc(compacted_gas_size);

            // use handle as input and output
            rt_check(optixAccelCompact(s.context, 0, s.accel, (CUdeviceptr)s.accel_buffer, compacted_gas_size, &s.accel));

            cuda_free((void*)d_buffer_temp_output_gas_and_compacted_size);
        } else {
            s.accel_buffer = d_buffer_temp_output_gas_and_compacted_size;
        }
    } // end shader binding table generation and acceleration data structure building

    // This will trigger the scatter calls to upload geometry to the device
    cuda_eval();

    Log(Info, "Validating and building scene in OptiX.");
    // TODO: check that there is really no equivalent
    // rt_check(rtContextValidate(s.context));

    // TODO: check if we still want to do run a dummy launch
}

MTS_VARIANT void Scene<Float, Spectrum>::accel_release_gpu() {
    OptixState &s = *(OptixState *) m_accel;
    // TODO: make sure if we realeased everything
    cuda_free(s.accel_buffer);
    rt_check(optixPipelineDestroy(s.pipeline));
    rt_check(optixProgramGroupDestroy(s.program_groups[0]));
    rt_check(optixProgramGroupDestroy(s.program_groups[1]));
    rt_check(optixProgramGroupDestroy(s.program_groups[2]));
#if defined(MTS_OPTIX_DEBUG)
    rt_check(optixProgramGroupDestroy(s.program_groups[3]));
#endif
    rt_check(optixModuleDestroy(s.module));
    rt_check(optixDeviceContextDestroy(s.context));
    delete (OptixState *) m_accel;
    m_accel = nullptr;
}

MTS_VARIANT typename Scene<Float, Spectrum>::SurfaceInteraction3f
Scene<Float, Spectrum>::ray_intersect_gpu(const Ray3f &ray_, Mask active) const {
    if constexpr (is_cuda_array_v<Float>) {
        OptixState &s = *(OptixState *) m_accel;
        Ray3f ray(ray_);
        size_t ray_count = std::max(slices(ray.o), slices(ray.d));
        set_slices(ray, ray_count);
        set_slices(active, ray_count);

        SurfaceInteraction3f si = empty<SurfaceInteraction3f>(ray_count);

        // DEBUG mode: Explicitly instantiate `si` with NaN values.
        // As the integrator should only deal with the lanes of `si` for which
        // `si.is_valid()==true`, this makes it easier to catch bugs in the
        // masking logic implemented in the integrator.
#if !defined(NDEBUG)
            si.t    = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.time = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.p.x() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.p.y() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.p.z() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.uv.x() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.uv.y() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.n.x() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.n.y() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.n.z() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.sh_frame.n.x() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.sh_frame.n.y() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.sh_frame.n.z() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.dp_du.x() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.dp_du.y() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.dp_du.z() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.dp_dv.x() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.dp_dv.y() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
            si.dp_dv.z() = full<Float>(std::numeric_limits<scalar_t<Float>>::quiet_NaN(), ray_count);
#endif  // !defined(NDEBUG)

        cuda_eval();

        const Params params = {
            // Active mask
            active.data(),
            // In: ray origin
            ray.o.x().data(), ray.o.y().data(), ray.o.z().data(),
            // In: ray direction
            ray.d.x().data(), ray.d.y().data(), ray.d.z().data(),
            // In: ray extents
            ray.mint.data(), ray.maxt.data(),
            // Out: Distance along ray
            si.t.data(),
            // Out: UV coordinates
            si.uv.x().data(), si.uv.y().data(),
            // Out: Geometric normal
            si.n.x().data(), si.n.y().data(), si.n.z().data(),
            // Out: Shading normal
            si.sh_frame.n.x().data(), si.sh_frame.n.y().data(), si.sh_frame.n.z().data(),
            // Out: Intersection position
            si.p.x().data(), si.p.y().data(), si.p.z().data(),
            // Out: Texture space derivative (U)
            si.dp_du.x().data(), si.dp_du.y().data(), si.dp_du.z().data(),
            // Out: Texture space derivative (V)
            si.dp_dv.x().data(), si.dp_dv.y().data(), si.dp_dv.z().data(),
            // Out: Shape pointer (on host)
            (unsigned long long*)si.shape.data(),
            // Out: Primitive index
            si.prim_index.data(),
            // Out: Hit flag
            nullptr,
            // top_object
            s.accel,
            // rg_any
            false
        };

        void* d_param = cuda_malloc(sizeof(Params));
        cuda_memcpy_to_device(d_param, &params, sizeof(Params));

        OptixResult rt = optixLaunch(
            s.pipeline,
            0, // default cuda stream
            (CUdeviceptr)d_param,
            sizeof(Params),
            &s.sbt,
            1, // width
            ray_count,
            1 // depth
        );
        if (rt == OPTIX_ERROR_HOST_OUT_OF_MEMORY) {
            cuda_malloc_trim();
            rt = optixLaunch(
                s.pipeline,
                0, // default cuda stream
                (CUdeviceptr)d_param,
                sizeof(Params),
                &s.sbt,
                1, // width
                ray_count,
                1 // depth
            );
        }
        rt_check(rt);

        si.time = ray.time;
        si.wavelengths = ray.wavelengths;
        si.instance = nullptr;
        si.duv_dx = si.duv_dy = 0.f;

        // Gram-schmidt orthogonalization to compute local shading frame
        si.sh_frame.s = normalize(
            fnmadd(si.sh_frame.n, dot(si.sh_frame.n, si.dp_du), si.dp_du));
        si.sh_frame.t = cross(si.sh_frame.n, si.sh_frame.s);

        // Incident direction in local coordinates
        si.wi = select(si.is_valid(), si.to_local(-ray.d), -ray.d);

        return si;
    } else {
        ENOKI_MARK_USED(ray_);
        ENOKI_MARK_USED(active);
        Throw("ray_intersect_gpu() should only be called in GPU mode.");
    }
}

MTS_VARIANT typename Scene<Float, Spectrum>::Mask
Scene<Float, Spectrum>::ray_test_gpu(const Ray3f &ray_, Mask active) const {
    if constexpr (is_cuda_array_v<Float>) {
        OptixState &s = *(OptixState *) m_accel;
        Ray3f ray(ray_);
        size_t ray_count = std::max(slices(ray.o), slices(ray.d));
        Mask hit = empty<Mask>(ray_count);

        set_slices(ray, ray_count);
        set_slices(active, ray_count);

        cuda_eval();

        const Params params = {
            // Active mask
            active.data(),
            // In: ray origin
            ray.o.x().data(), ray.o.y().data(), ray.o.z().data(),
            // In: ray direction
            ray.d.x().data(), ray.d.y().data(), ray.d.z().data(),
            // In: ray extents
            ray.mint.data(), ray.maxt.data(),
            // Out: Distance along ray
            nullptr,
            // Out: UV coordinates
            nullptr, nullptr,
            // Out: Geometric normal
            nullptr, nullptr, nullptr,
            // Out: Shading normal
            nullptr, nullptr, nullptr,
            // Out: Intersection position
            nullptr, nullptr, nullptr,
            // Out: Texture space derivative (U)
            nullptr, nullptr, nullptr,
            // Out: Texture space derivative (V)
            nullptr, nullptr, nullptr,
            // Out: Shape pointer (on host)
            nullptr,
            // Out: Primitive index
            nullptr,
            // Out: Hit flag
            hit.data(),
            // top_object
            s.accel,
            // rg_any
            true
        };

        void* d_param = cuda_malloc(sizeof(Params));
        cuda_memcpy_to_device(d_param, &params, sizeof( params ));

        OptixResult rt = optixLaunch(
            s.pipeline,
            0, // default cuda stream
            (CUdeviceptr)d_param,
            sizeof( Params ),
            &s.sbt,
            1, // width
            ray_count,
            1 // depth
        );
        // TODO: check that this is the error code we're looking for
        if (rt == OPTIX_ERROR_HOST_OUT_OF_MEMORY) {
            cuda_malloc_trim();
            rt = optixLaunch(
                s.pipeline,
                0, // default cuda stream
                (CUdeviceptr)d_param,
                sizeof( Params ),
                &s.sbt,
                1, // width
                ray_count,
                1 // depth
            );
        }
        rt_check(rt);

        return hit;
    } else {
        ENOKI_MARK_USED(ray_);
        ENOKI_MARK_USED(active);
        Throw("ray_test_gpu() should only be called in GPU mode.");
    }
}

NAMESPACE_END(msiuba)
