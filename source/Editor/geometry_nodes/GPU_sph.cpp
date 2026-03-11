#include "../../Core/RHI/source/shaderCompiler.h"
#include "GCore/Components/MeshComponent.h"
#include "GCore/Components/PointsComponent.h"
#include "GCore/algorithms/intersection.h"
#include "GPUContext/compute_context.hpp"
#include "geom_node_base.h"
#include "nvrhi/nvrhi.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE
// SPH Constants structure matching the shader
struct SPHConstants {
    float particleDiameter;  // d = 2r, full particle diameter
    float smoothingRadius;   // h, kernel support radius (typically 2d - 4d)
    float gravity;           // g, gravitational acceleration (m/s²)
    float restDensity;       // ρ₀, rest density (kg/m³), e.g., 1000 for water
    float mV;                // Particle mass (kg), calculated as ρ₀ * volume
    float constPress;        // k, pressure stiffness constant
    float constVisc;         // μ, dynamic viscosity coefficient
    float constSurf;         // σ, surface tension coefficient
    float dt;                // Δt, time step (s)
    int numParticles;        // N, total number of particles
    int numPairs;            // Number of neighbor pairs
};

struct SPHStorage {
    constexpr static bool has_storage = false;

    // Persistent buffers
    nvrhi::BufferHandle positions;
    nvrhi::BufferHandle velocities;
    nvrhi::BufferHandle rho;
    nvrhi::BufferHandle pressure;
    nvrhi::BufferHandle viscosity;

    // Cached programs and contexts
    ProgramHandle init_density_program;
    ProgramHandle density_program;
    ProgramHandle pressure_program;
    ProgramHandle viscosity_program;
    ProgramHandle update_pos_program;

    int num_particles = 0;

    ~SPHStorage()
    {
        auto& rc = get_resource_allocator();
        if (positions)
            rc.destroy(positions);
        if (velocities)
            rc.destroy(velocities);
        if (rho)
            rc.destroy(rho);
        if (pressure)
            rc.destroy(pressure);
        if (viscosity)
            rc.destroy(viscosity);
        if (init_density_program)
            rc.destroy(init_density_program);
        if (density_program)
            rc.destroy(density_program);
        if (pressure_program)
            rc.destroy(pressure_program);
        if (viscosity_program)
            rc.destroy(viscosity_program);
        if (update_pos_program)
            rc.destroy(update_pos_program);
    }
};

NODE_DECLARATION_FUNCTION(gpu_sph)
{
    b.add_input<Geometry>("Geometry");
    b.add_input<float>("Radius").default_val(0.02f).min(0.001f).max(0.1f);
    b.add_input<int>("Substeps").default_val(5).min(1).max(20);
    b.add_input<float>("Rest Density")
        .default_val(1000.0f)  // Water density: 1000 kg/m³
        .min(100.0f)
        .max(10000.0f);
    b.add_input<float>("Pressure Constant")
        .default_val(10000.0f)  // Adjusted for better stability
        .min(1000.0f)
        .max(100000.0f);
    b.add_input<float>("Viscosity")
        .default_val(0.1f)  // Increased for more viscous behavior
        .min(0.001f)
        .max(10.0f);  // Allow much higher viscosity
    b.add_input<float>("Surface Tension")
        .default_val(0.01f)  // Reduced surface tension
        .min(0.0f)
        .max(1.0f);
    b.add_input<float>("Gravity").default_val(-9.81f).min(-20.0f).max(0.0f);

    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(gpu_sph)
{
    auto& global_payload = params.get_global_payload<GeomPayload&>();

    auto& storage = params.get_storage<SPHStorage&>();
    auto& resource_allocator = get_resource_allocator();

    // Get inputs
    auto input_geom = params.get_input<Geometry>("Geometry");
    input_geom.apply_transform();

    auto radius = params.get_input<float>("Radius");
    auto dt = global_payload.delta_time;
    auto substeps = params.get_input<int>("Substeps");
    auto rest_density = params.get_input<float>("Rest Density");
    auto pressure_constant = params.get_input<float>("Pressure Constant");
    auto viscosity = params.get_input<float>("Viscosity");
    auto surface_tension = params.get_input<float>("Surface Tension");
    auto gravity = params.get_input<float>("Gravity");

    // Get points component
    auto points_component = input_geom.get_component<PointsComponent>();
    if (!points_component) {
        params.set_output<Geometry>("Geometry", std::move(input_geom));
        return true;
    }

    auto positions_cpu = points_component->get_vertices();
    int num_particles = positions_cpu.size();

    if (num_particles == 0) {
        params.set_output<Geometry>("Geometry", std::move(input_geom));
        return true;
    }

    auto device = RHI::get_device();

    // Initialize or recreate buffers if particle count changed
    if (storage.num_particles != num_particles) {
        // Clean up old buffers
        if (storage.positions)
            resource_allocator.destroy(storage.positions);
        if (storage.velocities)
            resource_allocator.destroy(storage.velocities);
        if (storage.rho)
            resource_allocator.destroy(storage.rho);
        if (storage.pressure)
            resource_allocator.destroy(storage.pressure);
        if (storage.viscosity)
            resource_allocator.destroy(storage.viscosity);

        storage.num_particles = num_particles;

        // Create positions buffer
        storage.positions = resource_allocator.create(
            nvrhi::BufferDesc{}
                .setByteSize(num_particles * sizeof(glm::vec3))
                .setStructStride(sizeof(glm::vec3))
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setCanHaveUAVs(true)
                .setDebugName("sph_positions"));

        // Create velocities buffer (initialized to zero)
        storage.velocities = resource_allocator.create(
            nvrhi::BufferDesc{}
                .setByteSize(num_particles * sizeof(glm::vec3))
                .setStructStride(sizeof(glm::vec3))
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setCanHaveUAVs(true)
                .setDebugName("sph_velocities"));

        // Create density buffer (uint format)
        storage.rho = resource_allocator.create(
            nvrhi::BufferDesc{}
                .setByteSize(num_particles * sizeof(unsigned int))
                .setStructStride(sizeof(unsigned int))
                .setFormat(nvrhi::Format::R32_UINT)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setCanHaveTypedViews(true)
                .setCanHaveUAVs(true)
                .setDebugName("sph_rho"));

        // Create pressure buffer (int3 as flat int array)
        storage.pressure = resource_allocator.create(
            nvrhi::BufferDesc{}
                .setByteSize(num_particles * 3 * sizeof(int))
                .setStructStride(sizeof(int))
                .setFormat(nvrhi::Format::R32_SINT)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setCanHaveTypedViews(true)
                .setCanHaveUAVs(true)
                .setDebugName("sph_pressure"));

        // Create viscosity buffer (int3 as flat int array)
        storage.viscosity = resource_allocator.create(
            nvrhi::BufferDesc{}
                .setByteSize(num_particles * 3 * sizeof(int))
                .setStructStride(sizeof(int))
                .setFormat(nvrhi::Format::R32_SINT)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setCanHaveTypedViews(true)
                .setCanHaveUAVs(true)
                .setDebugName("sph_viscosity"));

        // Upload initial positions
        auto upload_cmd = resource_allocator.create(CommandListDesc{});
        upload_cmd->open();
        upload_cmd->writeBuffer(
            storage.positions,
            positions_cpu.data(),
            num_particles * sizeof(glm::vec3));

        // Zero initialize other buffers
        std::vector<glm::vec3> zeros_vec3(num_particles, glm::vec3(0));
        upload_cmd->writeBuffer(
            storage.velocities,
            zeros_vec3.data(),
            num_particles * sizeof(glm::vec3));

        std::vector<unsigned int> zeros_uint(num_particles, 0);
        upload_cmd->writeBuffer(
            storage.rho,
            zeros_uint.data(),
            num_particles * sizeof(unsigned int));

        std::vector<int> zeros_int(num_particles * 3, 0);
        upload_cmd->writeBuffer(
            storage.pressure,
            zeros_int.data(),
            num_particles * 3 * sizeof(int));
        upload_cmd->writeBuffer(
            storage.viscosity,
            zeros_int.data(),
            num_particles * 3 * sizeof(int));

        upload_cmd->close();
        device->executeCommandList(upload_cmd);
        device->waitForIdle();
        resource_allocator.destroy(upload_cmd);
    }

    // Create shader programs if not cached
    if (!storage.init_density_program) {
        ProgramDesc desc;
        desc.shaderType = nvrhi::ShaderType::Compute;
        desc.set_path(
            SlangShaderCompiler::get_shader_dir(ShaderDirType::GeomNodes).string() + "/SPH/shaders/SPH_init_density.slang");
        desc.set_entry_name("main");
        storage.init_density_program = resource_allocator.create(desc);
        if (!storage.init_density_program->get_error_string().empty()) {
            spdlog::error(
                "Failed to compile init_density shader: {}",
                storage.init_density_program->get_error_string());
            resource_allocator.destroy(storage.init_density_program);
            storage.init_density_program = nullptr;
            params.set_output<Geometry>("Geometry", std::move(input_geom));
            return false;
        }
    }

    if (!storage.density_program) {
        ProgramDesc desc;
        desc.shaderType = nvrhi::ShaderType::Compute;
        desc.set_path(
            SlangShaderCompiler::get_shader_dir(ShaderDirType::GeomNodes).string() + "/SPH/shaders/SPH_update_density.slang");
        desc.set_entry_name("main");
        storage.density_program = resource_allocator.create(desc);
        if (!storage.density_program->get_error_string().empty()) {
            spdlog::error(
                "Failed to compile density shader: {}",
                storage.density_program->get_error_string());
            resource_allocator.destroy(storage.density_program);
            storage.density_program = nullptr;
            params.set_output<Geometry>("Geometry", std::move(input_geom));
            return false;
        }
    }

    if (!storage.pressure_program) {
        ProgramDesc desc;
        desc.shaderType = nvrhi::ShaderType::Compute;
        desc.set_path(
            SlangShaderCompiler::get_shader_dir(ShaderDirType::GeomNodes).string() + "/SPH/shaders/SPH_update_pressure.slang");
        desc.set_entry_name("main");
        storage.pressure_program = resource_allocator.create(desc);
        if (!storage.pressure_program->get_error_string().empty()) {
            spdlog::error(
                "Failed to compile pressure shader: {}",
                storage.pressure_program->get_error_string());
            resource_allocator.destroy(storage.pressure_program);
            storage.pressure_program = nullptr;
            params.set_output<Geometry>("Geometry", std::move(input_geom));
            return false;
        }
    }

    if (!storage.viscosity_program) {
        ProgramDesc desc;
        desc.shaderType = nvrhi::ShaderType::Compute;
        desc.set_path(
            SlangShaderCompiler::get_shader_dir(ShaderDirType::GeomNodes).string() + "/SPH/shaders/SPH_update_viscosity.slang");
        desc.set_entry_name("main");
        storage.viscosity_program = resource_allocator.create(desc);
        if (!storage.viscosity_program->get_error_string().empty()) {
            spdlog::error(
                "Failed to compile viscosity shader: {}",
                storage.viscosity_program->get_error_string());
            resource_allocator.destroy(storage.viscosity_program);
            storage.viscosity_program = nullptr;
            params.set_output<Geometry>("Geometry", std::move(input_geom));
            return false;
        }
    }

    if (!storage.update_pos_program) {
        ProgramDesc desc;
        desc.shaderType = nvrhi::ShaderType::Compute;
        desc.set_path(SlangShaderCompiler::get_shader_dir(ShaderDirType::GeomNodes).string() + "/SPH/shaders/SPH_update_pos.slang");
        desc.set_entry_name("main");
        storage.update_pos_program = resource_allocator.create(desc);
        if (!storage.update_pos_program->get_error_string().empty()) {
            spdlog::error(
                "Failed to compile update_pos shader: {}",
                storage.update_pos_program->get_error_string());
            resource_allocator.destroy(storage.update_pos_program);
            storage.update_pos_program = nullptr;
            params.set_output<Geometry>("Geometry", std::move(input_geom));
            return false;
        }
    }

    // Find neighbors (contact detection) - ONLY for initial setup
    // We will rebuild this in each substep using the updated positions
    unsigned pair_count = 0;

    // Adjust dt for substeps
    float dt_substep = dt / substeps;

    // Setup SPH constants
    SPHConstants sph_constants;
    sph_constants.particleDiameter = radius * 2.0f;  // Full diameter
    sph_constants.smoothingRadius =
        radius * 4.0f;  // h = 2 * diameter for good support
    sph_constants.gravity = gravity;
    sph_constants.restDensity = rest_density;

    // Calculate particle mass based on rest density and particle volume
    // SPH theory: mass = rest_density * particle_volume
    // Using spherical volume: V = (4/3)πr³
    float particle_volume =
        (4.0f / 3.0f) * 3.14159265f * radius * radius * radius;
    float particle_mass = rest_density * particle_volume;

    // IMPORTANT: mV should store MASS, not mass*volume!
    // The shader uses: ρᵢ = Σⱼ mⱼ * W(rᵢ - rⱼ, h)
    sph_constants.mV = particle_mass;

    sph_constants.constPress = pressure_constant;
    sph_constants.constVisc = viscosity;
    sph_constants.constSurf = surface_tension;
    sph_constants.dt = dt_substep;  // Use substep dt
    sph_constants.numParticles = num_particles;
    sph_constants.numPairs = 0;  // Will be updated each substep

    // Create constant buffer
    auto sph_cb = resource_allocator.create(
        nvrhi::BufferDesc{}
            .setByteSize(sizeof(SPHConstants))
            .setIsConstantBuffer(true)
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true)
            .setDebugName("sph_constants"));

    // Substep loop for better stability
    for (int substep = 0; substep < substeps; ++substep) {
        // CRITICAL: Rebuild neighbor pairs each substep with updated positions!
        // Use smoothing radius (h) for neighbor search
        float search_radius = sph_constants.smoothingRadius;
        auto contacts = FindNeighborsFromPositionBuffer(
            storage.positions, num_particles, search_radius, pair_count);

        if (!contacts || pair_count == 0) {
            spdlog::warn("No neighbors found in substep {}", substep);
            if (contacts)
                resource_allocator.destroy(contacts);
            continue;
        }

        // Update pair count in constants
        sph_constants.numPairs = pair_count;

        // Upload updated constants
        auto upload_cmd = resource_allocator.create(CommandListDesc{});
        upload_cmd->open();
        upload_cmd->writeBuffer(sph_cb, &sph_constants, sizeof(SPHConstants));
        upload_cmd->close();
        device->executeCommandList(upload_cmd);
        device->waitForIdle();
        resource_allocator.destroy(upload_cmd);
        // Step 1: Initialize density with self-contribution
        {
            ProgramVars vars(resource_allocator, storage.init_density_program);
            vars["sph_constants"] = sph_cb.Get();
            vars["rho"] = storage.rho.Get();
            vars.finish_setting_vars();

            ComputeContext ctx(resource_allocator, vars);
            ctx.finish_setting_pso();
            ctx.begin();
            ctx.dispatch({}, vars, num_particles, 32);
            ctx.finish();
        }

        // Step 2: Update density from pair interactions
        {
            ProgramVars vars(resource_allocator, storage.density_program);
            vars["sph_constants"] = sph_cb.Get();
            vars["positions"] = storage.positions.Get();
            vars["ContactPairs"] = contacts.Get();
            vars["rho"] = storage.rho.Get();
            vars.finish_setting_vars();

            ComputeContext ctx(resource_allocator, vars);
            ctx.finish_setting_pso();
            ctx.begin();
            ctx.dispatch({}, vars, pair_count, 32);
            ctx.finish();
        }

        // Step 3: Update viscosity
        {
            ProgramVars vars(resource_allocator, storage.viscosity_program);
            vars["sph_constants"] = sph_cb.Get();
            vars["ContactPairs"] = contacts.Get();
            vars["positions"] = storage.positions.Get();
            vars["velocities"] = storage.velocities.Get();
            vars["rho"] = storage.rho.Get();
            vars["viscosity"] = storage.viscosity.Get();
            vars.finish_setting_vars();

            ComputeContext ctx(resource_allocator, vars);
            ctx.finish_setting_pso();
            ctx.begin();
            ctx.dispatch({}, vars, pair_count, 32);
            ctx.finish();
        }

        // Step 4: Update pressure
        {
            ProgramVars vars(resource_allocator, storage.pressure_program);
            vars["sph_constants"] = sph_cb.Get();
            vars["ContactPairs"] = contacts.Get();
            vars["positions"] = storage.positions.Get();
            vars["pressure"] = storage.pressure.Get();
            vars["rho"] = storage.rho.Get();
            vars.finish_setting_vars();

            ComputeContext ctx(resource_allocator, vars);
            ctx.finish_setting_pso();
            ctx.begin();
            ctx.dispatch({}, vars, pair_count, 32);
            ctx.finish();
        }

        // Step 5: Update positions
        {
            ProgramVars vars(resource_allocator, storage.update_pos_program);
            vars["sph_constants"] = sph_cb.Get();
            vars["positions"] = storage.positions.Get();
            vars["velocities"] = storage.velocities.Get();
            vars["rho"] = storage.rho.Get();
            vars["pressure"] = storage.pressure.Get();
            vars["viscosity"] = storage.viscosity.Get();
            vars.finish_setting_vars();

            ComputeContext ctx(resource_allocator, vars);
            ctx.finish_setting_pso();
            ctx.begin();
            ctx.dispatch({}, vars, num_particles, 32);
            ctx.finish();
        }

        // Clean up contacts for this substep
        resource_allocator.destroy(contacts);
    }

    // Clean up constant buffer
    resource_allocator.destroy(sph_cb);

    // Read back positions from GPU
    auto readback_buffer = resource_allocator.create(
        nvrhi::BufferDesc{}
            .setByteSize(num_particles * sizeof(glm::vec3))
            .setCpuAccess(nvrhi::CpuAccessMode::Read)
            .setDebugName("sph_readback"));

    auto copy_cmd = resource_allocator.create(CommandListDesc{});
    copy_cmd->open();
    copy_cmd->copyBuffer(
        readback_buffer,
        0,
        storage.positions,
        0,
        num_particles * sizeof(glm::vec3));
    copy_cmd->close();
    device->executeCommandList(copy_cmd);
    device->waitForIdle();

    void* mapped_data =
        device->mapBuffer(readback_buffer, nvrhi::CpuAccessMode::Read);
    std::vector<glm::vec3> new_positions(num_particles);
    memcpy(
        new_positions.data(), mapped_data, num_particles * sizeof(glm::vec3));
    device->unmapBuffer(readback_buffer);

    resource_allocator.destroy(readback_buffer);
    resource_allocator.destroy(copy_cmd);

    // Update geometry with new positions
    points_component->set_vertices(new_positions);

    params.set_output<Geometry>("Geometry", std::move(input_geom));

    return true;
}

NODE_DECLARATION_UI(gpu_sph);

NODE_DEF_CLOSE_SCOPE