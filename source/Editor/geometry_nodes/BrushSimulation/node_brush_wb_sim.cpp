
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "GPUContext/compute_context.hpp"
#include "RHI/ResourceManager/resource_allocator.hpp"
#include "brush_sim_common.hpp"
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

// ============================================================================
// brush_wb_sim — Wetbrush (Chen et al. 2015) 仿真主节点
//
// 论文: "Wetbrush: GPU-based 3D Painting Simulation at the Bristle Level",
// ACM TOG 34(6), SIGGRAPH Asia 2015。带图转录稿见
// docs/paper_wetbrush_chen2015/。本节点每帧执行一次, 对应论文 Figure 4
// 的两大组件:
//
//   [动态仿真 §4]
//   1. 笔毛仿真(§4.1, Eq.1-2): 非惯性笔刷系显式积分 + PBD 约束
//   2. 样本重采样(§4.1): 三次 Hermite 样条 + Bishop 最小扭转局部系
//   3. 样本栅格化为 ψ 密度场 + 速度场 → 网格流体的边界条件(§4.1 末)
//   4. 液体粒子更新(§4.3, Eq.8-10): 固体摩擦 + 局部系/画布系两步粘附
//   5. 粒子栅格化 + 并入联合速度场 u(§4.3 末段)
//   6. 网格流体求解(§4.2): 隐式粘度 → 定点加速压力投影(Algorithm 1,
//      Eq.3-5) → 半拉格朗日平流(Stam 1999) → 干燥固化(§4.2 dryness)
//   7. FLIP/PIC 粒子速度修正(Eq.11, γ=0.8)
//   [液体转移 §5]
//   8. 笔毛↔粒子(§5.1, Eq.12-14): 容量 M_j / 吸收 / 发射
//   9. 网格↔粒子(§5.2, Eq.15-16): D0 距离内两种表示互换
//
// 液体的三种表示(§3 / Figure 5): 笔毛样本载量 m_j / 液体粒子 / 网格密度
// 场; 颜料颜色统一为 RYB 三通道(§3/§6), 随质量走完整个流水线。
//
// 与论文的已知偏离(诚实标注, 详见各处注释): 粒子 dt 硬编码 0.016;
// ρ0/M_max 按 1u=1cm 重标定; 活动窗口 320²(论文 128²)。(Eq.14 粒子质量
// 吸收与粒子 Eq.9 的完整惯性项已按论文补齐。)
// ============================================================================

NODE_DEF_OPEN_SCOPE

// 节点参数 ↔ 论文: Resolution Z=32 与论文网格厚度一致; Viscosity=ν
// (§4.2 隐式扩散, 单位 cm²/s); Drying Rate=干燥速率(§4.2 dryness 增量);
// Oil Density=油密度(§3: 控制粘度表现与渲染透明度); Ink Color=RYB 颜料
// 向量(§3/§6)。
NODE_DECLARATION_FUNCTION(brush_wb_sim)
{
    b.add_input<Ruzino::StrokeSample>("Stroke Sample");

    b.add_input<Ruzino::WetbrushZoneState>("State").optional(true);

    b.add_input<int>("Resolution").default_val(512).min(64).max(4096);
    b.add_input<int>("Resolution Z").default_val(32).min(4).max(128);
    b.add_input<float>("Paper Size").default_val(10.0f).min(0.1f).max(50.0f);
    b.add_input<float>("Canvas Center X").default_val(0.0f);
    b.add_input<float>("Canvas Center Y").default_val(0.0f);
    b.add_input<float>("Canvas Z").default_val(0.0f);
    b.add_input<float>("Canvas Height").default_val(0.0f).min(0.0f).max(20.0f);

    b.add_input<float>("Brush Radius").default_val(0.5f).min(0.01f).max(5.0f);
    b.add_input<float>("Brush Pressure").default_val(1.0f).min(0.0f).max(4.0f);
    b.add_input<float>("Ink Amount").default_val(0.8f).min(0.0f).max(2.0f);
    b.add_input<float>("Oil Density").default_val(0.5f).min(0.0f).max(1.0f);

    b.add_input<glm::vec3>("Ink Color").optional(true);

    b.add_input<float>("Viscosity").default_val(2.0f).min(0.0f).max(50.0f);
    b.add_input<float>("Diffusion Rate")
        .default_val(0.0001f)
        .min(0.0f)
        .max(0.01f);

    b.add_input<float>("Drying Rate").default_val(12.0f).min(0.0f).max(50.0f);

    b.add_output<Ruzino::WetbrushZoneState>("State");
}

NODE_EXECUTION_FUNCTION(brush_wb_sim)
{
    using Ruzino::WetbrushSimState;
    using Ruzino::WetbrushZoneState;

    // 上一帧状态经仿真区反馈环(simulation_out → simulation_in)回到这里;
    // 对应论文中系统持续维护的画布/笔刷/粒子状态。
    WetbrushZoneState zs;
    if (params.has_input("State"))
        zs = params.get_input<Ruzino::WetbrushZoneState>("State");
    auto& field = zs.state;

    Ruzino::StrokeSample bp =
        params.get_input<Ruzino::StrokeSample>("Stroke Sample");
    int resolution = params.get_input<int>("Resolution");
    int resolution_z = params.get_input<int>("Resolution Z");
    float paper_size = params.get_input<float>("Paper Size");
    glm::vec2 canvas_center_xy(
        params.get_input<float>("Canvas Center X"),
        params.get_input<float>("Canvas Center Y"));
    float canvas_z = params.get_input<float>("Canvas Z");
    float canvas_height_in = params.get_input<float>("Canvas Height");
    float brush_radius = params.get_input<float>("Brush Radius");
    float brush_pressure = params.get_input<float>("Brush Pressure");
    float ink_amount = params.get_input<float>("Ink Amount");
    float oil_density_in = params.get_input<float>("Oil Density");

    glm::vec3 ink_color = params.has_input("Ink Color")
                              ? params.get_input<glm::vec3>("Ink Color")
                              : glm::vec3(1.0f, 0.0f, 0.0f);
    if (bp.active) {
        ink_color = bp.color;
    }

    if (field) {
        field->pen_down = bp.active;
        field->dip_frame = bp.stroke_start;
    }

    auto& rc = get_resource_allocator();
    auto device = RHI::get_device();
    auto payload = params.get_global_payload<GeomPayload>();
    // 帧长 dt(缺省 1/60s)。论文强调大时间步稳定性(30-110FPS); 本实现的
    // 稳定性手段: 网格子步 + 隐式粘度 + 非惯性系位置式跟随(§4.1/§4.2)。
    float dt = payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);

    // WB_REDIP_EVERY=<秒>（默认 0 = 关）：行笔途中周期性重触发论文自己的
    // §5.1 dip（把全部样本重灌到 M'_j）—— 即真实画家"提笔回蘸再画"的工作
    // 流。2026-09-03 供墨诊断：只有笔尖接触带（R_j≈0.05-0.18cm 内）的样本
    // 会出墨，约占蘸墨量 46%；不回蘸，长笔画在该预算耗尽后必然断墨，按压/
    // 降速/ε/颗粒大小四个杠杆均已 A/B 证伪。重灌是与（未仿真的）调色盘
    // 交换质量，账本台阶属预期语义；不改任何论文方程。
    static const float redip_every = [] {
        const char* e = std::getenv("WB_REDIP_EVERY");
        float v = e ? static_cast<float>(std::atof(e)) : 0.0f;
        return v > 0.0f ? v : 0.0f;
    }();
    if (field) {
        if (field->pen_down && redip_every > 0.0f) {
            field->redip_clock += dt;
            if (field->redip_clock >= redip_every) {
                field->redip_clock -= redip_every;
                field->dip_frame = true;
            }
        }
        else if (!field->pen_down) {
            field->redip_clock = 0.0f;
        }
    }

    // 缓冲区分两组(下文称全局组 / 窗口组):
    //   全局组(res²·rz): density / 颜色 RYB / wetness / oil_density ——
    //     §4.2 的画布标量场, 全画布持久存在(窗口外的沉积与干燥不能丢);
    //   窗口组(win²·rz): 速度 / 压力 / 散度 / 临时标量 / 笔毛栅格 / 粒子
    //     栅格 —— §4.2 "把网格仿真限制在笔刷附近的小活动窗口"(论文
    //     128×128×32; 这里 320², 相对笔刷半径的余量与论文一致)。
    // 分辨率 / 画布几何变化时全部销毁重建并清零。
    bool need_alloc = !field || !field->center_initialized ||
                      field->grid_alloc_res != resolution ||
                      field->grid_alloc_res_z != resolution_z;

    if (need_alloc) {
        if (!field)
            field = std::make_shared<WetbrushSimState>();
        int rz = resolution_z > 0 ? resolution_z : 32;
        float height = canvas_height_in > 1e-6f
                           ? canvas_height_in
                           : paper_size * static_cast<float>(rz) /
                                 static_cast<float>(resolution);
        field->grid_center = canvas_center_xy;
        field->grid_height = height;
        field->grid_center_z = canvas_z + height * 0.5f;
        field->grid_paper = paper_size;
        field->grid_res = resolution;
        field->grid_res_z = rz;
        field->center_initialized = true;
        field->grid_alloc_res = resolution;
        field->grid_alloc_res_z = rz;
        field->win_alloc_z = rz;
        field->win_origin_set = false;
        field->deposited_count = 0;
        field->last_sim_time = -1.0f;
        field->has_prev_brush_pos = false;
        field->prev_brush_vel = glm::vec3(0.0f);
        field->prev_angular_vel = glm::vec3(0.0f);
        field->bristles_initialized = false;
        field->particles_initialized = false;

        int alloc_win_n3d = resolution * resolution * rz;

        int win_alloc_n3d = WetbrushSimState::win_alloc_xy() *
                            WetbrushSimState::win_alloc_xy() * rz;

        auto safe_destroy = [&](nvrhi::BufferHandle& h) {
            if (h) {
                rc.destroy(h);
                h = nullptr;
            }
        };

        auto destroy_buffers = [&](auto&... bufs) {
            (safe_destroy(bufs), ...);
        };

        destroy_buffers(
            field->density,
            field->density_tmp,
            field->color_r,
            field->color_y,
            field->color_b,
            field->color_r_tmp,
            field->color_y_tmp,
            field->color_b_tmp,
            field->vel_x,
            field->vel_x_tmp,
            field->vel_y,
            field->vel_y_tmp,
            field->vel_z,
            field->vel_z_tmp,
            field->wetness,
            field->wetness_tmp,
            field->oil_density,
            field->oil_density_tmp,
            field->height_field,
            field->pressure_a,
            field->pressure_b,
            field->divergence_buf,
            field->bristle_density,
            field->bristle_vel_x,
            field->bristle_vel_y,
            field->bristle_vel_z,
            field->bristle_color_r,
            field->bristle_color_y,
            field->bristle_color_b,
            field->ptcl_density,
            field->ptcl_vel_x,
            field->ptcl_vel_y,
            field->ptcl_vel_z,
            field->ptcl_rast_r,
            field->ptcl_rast_y,
            field->ptcl_rast_b,
            field->vel_x_old,
            field->vel_y_old,
            field->vel_z_old,
            field->packed_paint);

        auto make_buf = [&](const char* name) {
            return Ruzino::brush_create_field_buffer(rc, alloc_win_n3d, name);
        };

        auto make_win_buf = [&](const char* name) {
            return Ruzino::brush_create_field_buffer(rc, win_alloc_n3d, name);
        };

        field->density = make_buf("wb_density");
        field->color_r = make_buf("wb_color_r");
        field->color_y = make_buf("wb_color_y");
        field->color_b = make_buf("wb_color_b");
        field->wetness = make_buf("wb_wetness");
        field->oil_density = make_buf("wb_oil_density");

        field->density_tmp = make_win_buf("wb_density_tmp");
        field->color_r_tmp = make_win_buf("wb_color_r_tmp");
        field->color_y_tmp = make_win_buf("wb_color_y_tmp");
        field->color_b_tmp = make_win_buf("wb_color_b_tmp");
        field->vel_x = make_win_buf("wb_vel_x");
        field->vel_x_tmp = make_win_buf("wb_vel_x_tmp");
        field->vel_y = make_win_buf("wb_vel_y");
        field->vel_y_tmp = make_win_buf("wb_vel_y_tmp");
        field->vel_z = make_win_buf("wb_vel_z");
        field->vel_z_tmp = make_win_buf("wb_vel_z_tmp");
        field->wetness_tmp = make_win_buf("wb_wetness_tmp");
        field->oil_density_tmp = make_win_buf("wb_oil_density_tmp");
        field->pressure_a = make_win_buf("wb_pressure_a");
        field->pressure_b = make_win_buf("wb_pressure_b");
        field->divergence_buf = make_win_buf("wb_divergence");

        field->bristle_density = make_win_buf("wb_bristle_density");
        field->bristle_vel_x = make_win_buf("wb_bristle_vel_x");
        field->bristle_vel_y = make_win_buf("wb_bristle_vel_y");
        field->bristle_vel_z = make_win_buf("wb_bristle_vel_z");
        field->bristle_color_r = make_win_buf("wb_bristle_color_r");
        field->bristle_color_y = make_win_buf("wb_bristle_color_y");
        field->bristle_color_b = make_win_buf("wb_bristle_color_b");

        field->ptcl_density = make_win_buf("wb_ptcl_density");
        field->ptcl_vel_x = make_win_buf("wb_ptcl_vel_x");
        field->ptcl_vel_y = make_win_buf("wb_ptcl_vel_y");
        field->ptcl_vel_z = make_win_buf("wb_ptcl_vel_z");
        field->ptcl_rast_r = make_win_buf("wb_ptcl_rast_r");
        field->ptcl_rast_y = make_win_buf("wb_ptcl_rast_y");
        field->ptcl_rast_b = make_win_buf("wb_ptcl_rast_b");
        field->vel_x_old = make_win_buf("wb_vel_x_old");
        field->vel_y_old = make_win_buf("wb_vel_y_old");
        field->vel_z_old = make_win_buf("wb_vel_z_old");

        if (!field->field_clear_program)
            field->field_clear_program =
                Ruzino::brush_compile_shader(rc, "field_clear.slang");
        nvrhi::BufferHandle* rast_bufs[] = {
            std::addressof(field->ptcl_density),
            std::addressof(field->ptcl_rast_r),
            std::addressof(field->ptcl_rast_y),
            std::addressof(field->ptcl_rast_b),
        };
        for (nvrhi::BufferHandle* buf : rast_bufs) {
            Ruzino::brush_dispatch(
                rc,
                field->field_clear_program,
                {},
                { { "field", *buf } },
                nullptr,
                win_alloc_n3d);
        }

        field->packed_paint = rc.create(
            nvrhi::BufferDesc{}
                .setByteSize(
                    static_cast<size_t>(alloc_win_n3d) * sizeof(float) * 4)
                .setStructStride(sizeof(float) * 4)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setCanHaveUAVs(true)
                .setCanHaveTypedViews(true)
                .setCanHaveRawViews(true)
                .setDebugName("wb_packed_paint"));

        std::vector<float> zeros3d(alloc_win_n3d, 0.0f);
        std::vector<float> zeros_win(win_alloc_n3d, 0.0f);
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        auto write_3d = [&](auto&... bufs) {
            (cmd->writeBuffer(
                 bufs, zeros3d.data(), alloc_win_n3d * sizeof(float)),
             ...);
        };
        auto write_win = [&](auto&... bufs) {
            (cmd->writeBuffer(
                 bufs, zeros_win.data(), win_alloc_n3d * sizeof(float)),
             ...);
        };

        write_3d(
            field->density,
            field->color_r,
            field->color_y,
            field->color_b,
            field->wetness,
            field->oil_density);
        write_win(
            field->density_tmp,
            field->color_r_tmp,
            field->color_y_tmp,
            field->color_b_tmp,
            field->vel_x,
            field->vel_x_tmp,
            field->vel_y,
            field->vel_y_tmp,
            field->vel_z,
            field->vel_z_tmp,
            field->wetness_tmp,
            field->oil_density_tmp,
            field->pressure_a,
            field->pressure_b,
            field->divergence_buf,
            field->vel_x_old,
            field->vel_y_old,
            field->vel_z_old,
            field->bristle_density,
            field->bristle_vel_x,
            field->bristle_vel_y,
            field->bristle_vel_z,
            field->bristle_color_r,
            field->bristle_color_y,
            field->bristle_color_b,
            field->ptcl_density,
            field->ptcl_vel_x,
            field->ptcl_vel_y,
            field->ptcl_vel_z,
            field->ptcl_rast_r,
            field->ptcl_rast_y,
            field->ptcl_rast_b);
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);

        spdlog::info(
            "brush_wb_sim: allocated global 3D grid {}x{}x{}, "
            "paper={:.3f}, height={:.3f}",
            resolution,
            resolution,
            rz,
            paper_size,
            height);
    }

    // 本帧活动窗口尺寸(分配上限与网格分辨率取小)与格距 cell_sz;
    // Nb/M/S = 笔毛数 / 每毛顶点数 / 每毛样本数。
    const int WIN_XY =
        std::min(WetbrushSimState::win_alloc_xy(), field->grid_res);
    const int WIN_Z = field->grid_res_z;
    const int win_n3d = WIN_XY * WIN_XY * WIN_Z;
    const float cell_sz =
        field->grid_paper / static_cast<float>(field->grid_res);

    const int Nb = WetbrushSimState::NUM_BRISTLES;
    const int M = WetbrushSimState::VERTS_PER_BRISTLE;
    const int S = WetbrushSimState::SAMPLES_PER_BRISTLE;

    // ---- 笔毛/样本缓冲(§4.1 / §5.1) ----
    // Nb=600 根笔毛 × M=10 顶点(§7: "each bristle contains 10 vertices"),
    // 每毛重采样 S=128 个样本(与论文同值)。样本槽位:
    //   pos/vel/frame —— §4.1 局部系(t/n/b 三轴 + 角速度, 最小扭转);
    //   color —— 供给颜料(RYB+量, 由 bristle_input_color 缓冲刷新);
    //   liquid —— §5.1 样本载量 {m_j, c_j}, 蘸笔帧置为满载;
    //   supply —— 旧供给槽, 已不参与仿真(仅保留缓冲)。
    if (!field->bristles_initialized) {
        auto safe_destroy = [&](nvrhi::BufferHandle& h) {
            if (h) {
                rc.destroy(h);
                h = nullptr;
            }
        };
        auto destroy_buffers = [&](auto&... bufs) {
            (safe_destroy(bufs), ...);
        };
        destroy_buffers(
            field->bristle_data,
            field->lambda_buf,
            field->sample_pos,
            field->sample_vel,
            field->sample_color,
            field->sample_frame,
            field->sample_liquid,
            field->sample_liquid_b,
            field->sample_supply,
            field->bristle_input_color_buf);

        field->bristle_data = Ruzino::brush_create_typed_buffer(
            rc,
            Nb * M,
            WetbrushSimState::BRISTLE_VERTEX_STRIDE,
            "wb_bristle_data");
        field->lambda_buf = Ruzino::brush_create_typed_buffer(
            rc, Nb * M, sizeof(float), "wb_lambda");
        field->sample_pos = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "wb_sample_pos");
        field->sample_vel = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "wb_sample_vel");
        field->sample_color = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "wb_sample_color");
        field->sample_frame = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4 * 3, "wb_sample_frame");
        field->sample_liquid = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "wb_sample_liquid");
        field->sample_liquid_b = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "wb_sample_liquid_b");
        field->sample_supply = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float), "wb_sample_supply");
        field->bristle_input_color_buf = Ruzino::brush_create_typed_buffer(
            rc, 1, sizeof(float) * 4, "wb_bristle_input_color");

        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        std::vector<float> z_bristle(
            Nb * M * (WetbrushSimState::BRISTLE_VERTEX_STRIDE / sizeof(float)),
            0.0f);
        cmd->writeBuffer(
            field->bristle_data,
            z_bristle.data(),
            z_bristle.size() * sizeof(float));
        std::vector<float> z_sample(Nb * S * 4, 0.0f);
        auto write_sample4 = [&](auto&... bufs) {
            (cmd->writeBuffer(
                 bufs, z_sample.data(), Nb * S * sizeof(float) * 4),
             ...);
        };
        write_sample4(
            field->sample_pos,
            field->sample_vel,
            field->sample_color,
            field->sample_liquid_b);

        std::vector<float> liquid_init(Nb * S * 4, 0.0f);
        for (int i = 0; i < Nb * S; ++i) {
            liquid_init[i * 4 + 0] = 0.0f;
            liquid_init[i * 4 + 1] = ink_color.r;
            liquid_init[i * 4 + 2] = ink_color.g;
            liquid_init[i * 4 + 3] = ink_color.b;
        }
        cmd->writeBuffer(
            field->sample_liquid,
            liquid_init.data(),
            Nb * S * sizeof(float) * 4);
        std::vector<float> z_frame(Nb * S * 4 * 3, 0.0f);
        cmd->writeBuffer(
            field->sample_frame,
            z_frame.data(),
            Nb * S * sizeof(float) * 4 * 3);
        float input_color[4] = {
            ink_color.r, ink_color.g, ink_color.b, ink_amount
        };
        cmd->writeBuffer(
            field->bristle_input_color_buf, input_color, sizeof(float) * 4);
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);

        field->bristles_initialized = true;
    }

    // 落笔期间把当前墨色/墨量刷进供给缓冲(支持边画边换色)。
    if (field->bristles_initialized && bp.active) {
        float input_color[4] = {
            ink_color.r, ink_color.g, ink_color.b, ink_amount
        };
        auto color_cmd = rc.create(CommandListDesc{});
        color_cmd->open();
        color_cmd->writeBuffer(
            field->bristle_input_color_buf, input_color, sizeof(float) * 4);
        color_cmd->close();
        device->executeCommandList(color_cmd);
        device->waitForIdle();
        rc.destroy(color_cmd);
    }

    // ---- 液体粒子池(§4.3) ----
    // 论文以 20 万~100 万粒子承载亚像素细节(§4.3), 大笔刷可达 2M(§7);
    // 本实现池上限 2^20。粒子由 §5.1 emit / §5.2 g2p 生成、§5.2 p2g 消亡,
    // 池与 ptcl_counter 动态增删; *_b 为 ping-pong 交换缓冲。
    if (!field->particles_initialized) {
        int max_ptcl = WetbrushSimState::MAX_PARTICLES;
        auto safe_destroy = [&](nvrhi::BufferHandle& h) {
            if (h) {
                rc.destroy(h);
                h = nullptr;
            }
        };
        auto destroy_buffers = [&](auto&... bufs) {
            (safe_destroy(bufs), ...);
        };
        destroy_buffers(
            field->ptcl_pos,
            field->ptcl_vel,
            field->ptcl_color,
            field->ptcl_alive,
            field->ptcl_counter,
            field->emit_budget,
            field->ptcl_pos_b,
            field->ptcl_vel_b,
            field->ptcl_color_b,
            field->ptcl_alive_b);

        field->ptcl_pos = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_pos");
        field->ptcl_vel = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_vel");
        field->ptcl_color = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_color");
        field->ptcl_alive = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(uint32_t), "wb_ptcl_alive");
        field->ptcl_counter = Ruzino::brush_create_byte_buffer(
            rc, sizeof(uint32_t), "wb_ptcl_counter");
        field->emit_budget = Ruzino::brush_create_byte_buffer(
            rc, sizeof(uint32_t), "wb_emit_budget");
        field->ptcl_pos_b = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_pos_b");
        field->ptcl_vel_b = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_vel_b");
        field->ptcl_color_b = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_color_b");
        field->ptcl_alive_b = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(uint32_t), "wb_ptcl_alive_b");

        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        std::vector<float> z_ptcl(max_ptcl * 4, 0.0f);
        auto write_ptcl4 = [&](auto&... bufs) {
            (cmd->writeBuffer(
                 bufs, z_ptcl.data(), max_ptcl * sizeof(float) * 4),
             ...);
        };
        write_ptcl4(
            field->ptcl_pos,
            field->ptcl_vel,
            field->ptcl_color,
            field->ptcl_color_b);
        auto write_ptcl3 = [&](auto&... bufs) {
            (cmd->writeBuffer(
                 bufs, z_ptcl.data(), max_ptcl * sizeof(float) * 3),
             ...);
        };
        write_ptcl3(field->ptcl_pos_b, field->ptcl_vel_b);
        std::vector<uint32_t> z_u(max_ptcl, 0);
        auto write_u = [&](auto&... bufs) {
            (cmd->writeBuffer(bufs, z_u.data(), max_ptcl * sizeof(uint32_t)),
             ...);
        };
        write_u(field->ptcl_alive, field->ptcl_alive_b);
        uint32_t zero_c = 0;
        cmd->writeBuffer(field->ptcl_counter, &zero_c, sizeof(uint32_t));
        cmd->writeBuffer(field->emit_budget, &zero_c, sizeof(uint32_t));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);

        field->particles_initialized = true;
    }

    // 惰性编译本帧所需计算 shader(句柄缓存于 field, 只编译一次)。
    auto ensure_prog = [&](ProgramHandle& slot, const char* fn) {
        if (!slot)
            slot = Ruzino::brush_compile_shader(rc, fn);
    };
    ensure_prog(field->field_clear_program, "field_clear.slang");
    ensure_prog(field->pack_program, "pack_float4.slang");
    ensure_prog(field->bristle_sim_program, "bristle_simulate.slang");
    ensure_prog(
        field->bristle_density_constraint_program,
        "bristle_density_constraint.slang");
    ensure_prog(field->bristle_resample_program, "bristle_resample.slang");
    ensure_prog(field->bristle_raster_program, "bristle_rasterize.slang");
    ensure_prog(field->bristle_merge_program, "bristle_merge.slang");

    if (!payload.is_simulating) {
        params.set_output("State", zs);
        return true;
    }

    // ---- 笔刷运动学(§4.1: "User directly controls brush motion") ----
    // 位姿来自 StrokeSample(采集的 pos/quat + 解析 vel/omega); 无动力学
    // 输入时用相邻帧位置差分逼近速度/角加速度。单帧位移可以任意大 ——
    // 由下方笔画子步 + 窗口跟随共同覆盖。
    glm::vec3 brush_pos_3d = bp.pos;
    brush_pos_3d.x -= field->grid_center.x;
    brush_pos_3d.y -= field->grid_center.y;

    glm::vec3 brush_vel_3d(0.0f);
    glm::vec3 brush_accel_3d(0.0f);
    glm::vec3 brush_angular_vel(0.0f);
    glm::vec3 brush_angular_accel(0.0f);

    float brush_rotation = 0.0f;

    if (bp.stroke_start) {
        field->has_prev_brush_pos = false;
        if (bp.has_dynamics) {
            brush_vel_3d = bp.vel;
            brush_angular_vel = bp.angular_vel;
        }
        field->prev_brush_vel = glm::vec3(0.0f);
        field->prev_angular_vel = glm::vec3(0.0f);
    }
    else if (field->has_prev_brush_pos) {
        if (bp.has_dynamics) {
            brush_vel_3d = bp.vel;
            if (dt > 1e-6f)
                brush_accel_3d = (bp.vel - field->prev_brush_vel) / dt;
            brush_angular_vel = bp.angular_vel;
            if (dt > 1e-6f)
                brush_angular_accel =
                    (bp.angular_vel - field->prev_angular_vel) / dt;
            brush_rotation = brush_vel_3d.x != 0.0f || brush_vel_3d.y != 0.0f
                                 ? atan2(brush_vel_3d.y, brush_vel_3d.x)
                                 : 0.0f;
        }
        else {
            glm::vec3 new_vel = (brush_pos_3d - field->prev_brush_pos) / dt;
            if (dt > 1e-6f)
                brush_accel_3d = (new_vel - field->prev_brush_vel) / dt;
            brush_vel_3d = new_vel;
            brush_rotation = atan2(brush_vel_3d.y, brush_vel_3d.x);

            float prev_rot =
                atan2(field->prev_brush_vel.y, field->prev_brush_vel.x);
            float dtheta = brush_rotation - prev_rot;
            dtheta = atan2(sin(dtheta), cos(dtheta));
            if (dt > 1e-6f) {
                glm::vec3 new_omega(0.0f, 0.0f, dtheta / dt);
                brush_angular_accel =
                    (new_omega - field->prev_angular_vel) / dt;
                brush_angular_vel = new_omega;
            }
        }
    }

    glm::quat brush_orientation = bp.orientation;
    glm::mat3 brush_rot_m = glm::mat3_cast(brush_orientation);

    // 诊断探针(WB_DEBUG_DUMP_PTCL): 打印每帧笔刷位姿, 非论文内容。
    if (std::getenv("WB_DEBUG_DUMP_PTCL")) {
        static int pose_frame = 0;

        float tilt_deg = glm::degrees(
            acosf(std::min(1.0f, std::max(-1.0f, brush_rot_m[2].z))));
        spdlog::info(
            "[wb-pose] f={} t={:.3f} pos=({:.3f},{:.3f},{:.3f}) "
            "|v|={:.3f} |a|={:.3f} |w|={:.4f} tilt={:.1f}deg dyn={}",
            pose_frame++,
            bp.time,
            brush_pos_3d.x,
            brush_pos_3d.y,
            brush_pos_3d.z,
            glm::length(brush_vel_3d),
            glm::length(brush_accel_3d),
            glm::length(brush_angular_vel),
            tilt_deg,
            bp.has_dynamics ? 1 : 0);
    }

    // ---- 活动窗口跟随(§4.2) ----
    // "We update the window location at the beginning of each time step."
    // 窗口中心跟随笔刷足印、夹取在画布内; 仅当整数原点变化时才把速度/
    // 压力场滚动(window_scroll.slang)进新窗口系 —— 标量场是全局的,
    // 无需滚动。每个笔画子步都调用, 保证足印始终落在窗口内。
    auto position_window = [&](float bx, float by) {
        int old_ox = field->win_origin_x;
        int old_oy = field->win_origin_y;
        bool first = !field->win_origin_set;

        float half_p = field->grid_paper * 0.5f;
        float bgx = (bx - field->grid_center.x + half_p) / cell_sz;
        float bgy = (by - field->grid_center.y + half_p) / cell_sz;
        int new_wox = static_cast<int>(bgx) - WIN_XY / 2;
        int new_woy = static_cast<int>(bgy) - WIN_XY / 2;
        new_wox = std::max(0, std::min(new_wox, field->grid_res - WIN_XY));
        new_woy = std::max(0, std::min(new_woy, field->grid_res - WIN_XY));

        field->win_origin_x = new_wox;
        field->win_origin_y = new_woy;
        field->win_origin_z = 0;
        field->win_origin_set = true;

        if (first || (new_wox == old_ox && new_woy == old_oy))
            return;

        if (!field->window_scroll_program)
            field->window_scroll_program =
                Ruzino::brush_compile_shader(rc, "window_scroll.slang");
        struct ScrollCB {
            int old_ox, old_oy, old_oz;
            int new_ox, new_oy, new_oz;
            int wsx, wsz;
        };
        ScrollCB scb{ old_ox, old_oy, 0, new_wox, new_woy, 0, WIN_XY, WIN_Z };
        nvrhi::BufferHandle scroll_cb;
        Ruzino::brush_upload_cb(
            rc, device, &scb, sizeof(scb), "wb_scroll_cb", scroll_cb);
        auto scroll = [&](nvrhi::BufferHandle& field_buf,
                          nvrhi::BufferHandle& scratch) {
            Ruzino::brush_dispatch(
                rc,
                field->window_scroll_program,
                { { "src_field", field_buf } },
                { { "dst_field", scratch } },
                scroll_cb,
                win_n3d);
            std::swap(field_buf, scratch);
        };
        scroll(field->vel_x, field->vel_x_tmp);
        scroll(field->vel_y, field->vel_y_tmp);
        scroll(field->vel_z, field->vel_z_tmp);
        scroll(field->pressure_a, field->pressure_b);
        rc.destroy(scroll_cb);
    };

    // ---- §4.1 一次求值的完整序列(参数为子步插值后的位姿) ----
    //  1) bristle_simulate: Eq.1 顶点变换入笔刷系; Eq.2 显式积分四项惯性
    //     加速度(平动/离心/Euler/科氏), β_B=0.05 ∈ 论文 [0,0.1] —— β_B
    //     越小越接近"位置式钉在笔刷上"; a_i 只含重力 + 网格液拖拽(结构/
    //     剪切内力不走 Eq.2, 由 PBD 约束承担, 见 shader 头注)。
    //  2) 密度约束(§4.1 末段, PBF 式最小密度约束): mode0 算 ρ 与 Lagrange
    //     乘子 λ, mode1 施加位置修正, 共 3 轮 —— 复现笔毛受压簇拥效应。
    //  3) bristle_resample: 三次 Hermite 样条上取 S 个样本 + 最小扭转局部
    //     系(Bishop 1975 / Bergou 2008; §4.1 "Bristle samples and frames")。
    //  4) 清空笔毛栅格并(可选)栅格化: 样本写入 ψ 密度 / 速度 / 颜色场,
    //     作为网格流体的边界条件(§4.1 末句); 抬笔悬空时 deposit=false。
    auto deposit_at = [&](const glm::vec3& sub_pos,
                          const glm::vec3& sub_vel,
                          const glm::vec3& sub_accel,
                          float sub_rot,
                          const glm::vec3& sub_omega,
                          const glm::vec3& sub_omega_dot,
                          float dt_sub,
                          bool deposit) {
        Ruzino::BristleConstants bc = {};
        bc.num_bristles = Nb;
        bc.verts_per_bristle = M;
        bc.samples_per_bristle = S;
        bc.beta_B = 0.05f;
        bc.dt = dt_sub;
        bc.brush_pos_x = sub_pos.x;
        bc.brush_pos_y = sub_pos.y;
        bc.brush_pos_z = sub_pos.z;
        bc.brush_vel_x = sub_vel.x;
        bc.brush_vel_y = sub_vel.y;
        bc.brush_vel_z = sub_vel.z;
        bc.brush_angular_vel_x = sub_omega.x;
        bc.brush_angular_vel_y = sub_omega.y;
        bc.brush_angular_vel_z = sub_omega.z;
        bc.brush_rotation = sub_rot;
        bc.brush_accel_x = sub_accel.x;
        bc.brush_accel_y = sub_accel.y;
        bc.brush_accel_z = sub_accel.z;
        bc.brush_angular_accel_x = sub_omega_dot.x;
        bc.brush_angular_accel_y = sub_omega_dot.y;
        bc.brush_angular_accel_z = sub_omega_dot.z;
        bc.brush_pressure = brush_pressure;
        bc.canvas_z = field->grid_center_z - field->grid_height * 0.5f;
        bc.brush_radius = brush_radius;
        bc.spring_k = 50.0f;
        bc.damping = 5.0f;
        bc.grid_res = field->grid_res;
        bc.grid_res_z = WIN_Z;
        bc.height_extent = field->grid_height;
        bc.grid_center_z = field->grid_center_z;
        bc.cell_size = cell_sz;
        bc.paper_size = field->grid_paper;
        bc.grid_center_x = field->grid_center.x;
        bc.grid_center_y = field->grid_center.y;
        bc.window_origin_x = field->win_origin_x;
        bc.window_origin_y = field->win_origin_y;
        bc.window_origin_z = 0;
        bc.window_size_x = WIN_XY;
        bc.window_size_z = WIN_Z;
        bc.prev_brush_pos_x = 0.0f;
        bc.prev_brush_pos_y = 0.0f;
        bc.prev_brush_pos_z = 0.0f;
        bc.has_prev_brush_pos = 0;
        bc.sweep_steps = 1;
        bc._sweep_pad0 = 0.0f;
        bc._sweep_pad1 = 0.0f;

        bc.brush_R0 = glm::vec4(brush_rot_m[0], 0.0f);
        bc.brush_R1 = glm::vec4(brush_rot_m[1], 0.0f);
        bc.brush_R2 = glm::vec4(brush_rot_m[2], 0.0f);

        nvrhi::BufferHandle bristle_cb;
        Ruzino::brush_upload_cb(
            rc, device, &bc, sizeof(bc), "wb_bristle_cb", bristle_cb);

        Ruzino::brush_dispatch(
            rc,
            field->bristle_sim_program,
            { { "grid_vel_x", field->vel_x },
              { "grid_vel_y", field->vel_y },
              { "grid_vel_z", field->vel_z } },
            { { "bristle_data", field->bristle_data } },
            bristle_cb,
            Nb);

        int total_verts = Nb * M;
        for (int dc_iter = 0; dc_iter < 3; dc_iter++) {
            for (int mode : { 0, 1 }) {
                Ruzino::ConstraintModeCB mcb = { mode, { 0, 0, 0 } };
                nvrhi::BufferHandle mode_cb;
                Ruzino::brush_upload_cb(
                    rc, device, &mcb, sizeof(mcb), "wb_dc_mode_cb", mode_cb);
                ProgramVars v(rc, field->bristle_density_constraint_program);
                v["cb"] = bristle_cb.Get();
                v["bristle_data"] = field->bristle_data.Get();
                v["lambda_buf"] = field->lambda_buf.Get();
                v["mode_cb"] = mode_cb.Get();
                v.finish_setting_vars();
                ComputeContext c(rc, v);
                c.finish_setting_pso();
                c.begin();
                c.dispatch({}, v, total_verts, 256);
                c.finish();
                rc.destroy(mode_cb);
            }
        }

        Ruzino::brush_dispatch(
            rc,
            field->bristle_resample_program,
            { { "bristle_data", field->bristle_data },
              { "bristle_input_color", field->bristle_input_color_buf } },
            { { "sample_pos", field->sample_pos },
              { "sample_vel", field->sample_vel },
              { "sample_color", field->sample_color },
              { "sample_frame", field->sample_frame } },
            bristle_cb,
            Nb);

        auto clear_bristle_grid = [&](auto& buf) {
            Ruzino::brush_dispatch(
                rc,
                field->field_clear_program,
                {},
                { { "field", buf } },
                nullptr,
                win_n3d);
        };
        clear_bristle_grid(field->bristle_density);
        clear_bristle_grid(field->bristle_vel_x);
        clear_bristle_grid(field->bristle_vel_y);
        clear_bristle_grid(field->bristle_vel_z);
        clear_bristle_grid(field->bristle_color_r);
        clear_bristle_grid(field->bristle_color_y);
        clear_bristle_grid(field->bristle_color_b);

        if (deposit) {
            Ruzino::brush_dispatch(
                rc,
                field->bristle_raster_program,
                { { "sample_pos", field->sample_pos },
                  { "sample_color", field->sample_color },
                  { "sample_vel", field->sample_vel } },
                { { "bristle_density", field->bristle_density },
                  { "bristle_vel_x", field->bristle_vel_x },
                  { "bristle_vel_y", field->bristle_vel_y },
                  { "bristle_vel_z", field->bristle_vel_z },
                  { "bristle_color_r", field->bristle_color_r },
                  { "bristle_color_y", field->bristle_color_y },
                  { "bristle_color_b", field->bristle_color_b } },
                bristle_cb,
                Nb * S);
        }

        rc.destroy(bristle_cb);
    };

    // ---- 笔画子步 ----
    // 论文允许单帧笔刷位移任意大; 若整帧只求值一次, 足印会在画布上"瞬移"
    // 并撕裂窗口覆盖。按位移 ≤ max(笔刷直径, 一格) 切成 n_sub 段(上限
    // 128), 每子步在段中点插值位置重跑窗口跟随 + §4.1 序列, dt_sub=dt/n_sub。
    {
        int n_sub = 1;
        if (field->has_prev_brush_pos) {
            glm::vec3 delta = brush_pos_3d - field->prev_brush_pos;
            float frame_disp = glm::length(delta);
            float diam = std::max(brush_radius * 2.0f, cell_sz);
            n_sub = std::max(1, static_cast<int>(std::ceil(frame_disp / diam)));
            const int N_SUB_CAP = 128;
            if (n_sub > N_SUB_CAP)
                n_sub = N_SUB_CAP;
        }

        if (bp.active) {
            for (int s = 0; s < n_sub; s++) {
                float t =
                    (static_cast<float>(s) + 0.5f) / static_cast<float>(n_sub);
                glm::vec3 sub_pos =
                    field->has_prev_brush_pos
                        ? glm::mix(field->prev_brush_pos, brush_pos_3d, t)
                        : brush_pos_3d;

                glm::vec3 sub_vel = brush_vel_3d;
                glm::vec3 sub_accel = brush_accel_3d;
                float sub_rot = brush_rotation;
                glm::vec3 sub_omega = brush_angular_vel;
                glm::vec3 sub_omega_dot = brush_angular_accel;
                float dt_sub = dt / static_cast<float>(n_sub);

                position_window(sub_pos.x, sub_pos.y);
                deposit_at(
                    sub_pos,
                    sub_vel,
                    sub_accel,
                    sub_rot,
                    sub_omega,
                    sub_omega_dot,
                    dt_sub,
                    true);
            }
        }
        else {
            position_window(brush_pos_3d.x, brush_pos_3d.y);
            deposit_at(
                brush_pos_3d,
                brush_vel_3d,
                brush_accel_3d,
                brush_rotation,
                brush_angular_vel,
                brush_angular_accel,
                dt,
                false);
        }

        field->prev_brush_pos = brush_pos_3d;
        field->has_prev_brush_pos = true;
        field->prev_brush_vel = brush_vel_3d;
        field->prev_angular_vel = brush_angular_vel;
    }

    const int max_ptcl = WetbrushSimState::MAX_PARTICLES;

    // ---- 笔毛↔粒子液体转移(§5.1, 落笔时; 抬笔帧整段跳过 —— 残余
    // swarm 落布不被吸回, 见 particle_to_grid 的 pen-up 分支) ----
    // 常数取 Table 1: μ=0.5(Eq.12 拥挤折扣), ε=0.1(发射迟滞), ρ0=12.3
    // (论文 1.0e3 kg/m³ 按 1u=1cm 重标定), M_max=0.30。两次 dispatch:
    //   transfer(PASS 0 吸收): 蘸笔帧把载量置为 M'_j(Eq.12 按当前拥挤
    //     度打折的容量); 非蘸笔帧执行 Eq.14 粒子质量吸收 —— 未饱和
    //     样本(m_j < M_j)认领半径 R_j 内的一个粒子: 质量并入 m_j、
    //     颜料按 Eq.14 混入 c_j、粒子消亡(原子认领防双花)。这是论文
    //     "笔从画布拾回颜料"的通路(§5.2 g2p 转出的粒子被毛尖吸走,
    //     随笔带走); 饱和样本只做色彩渗染、不取质量(§5.1: 饱和仍可
    //     拾色)。每样本每步至多处理一个接触(与发射侧对偶节流)。
    //   emit(PASS 1 发射): m_j > (1+ε)·M_j 时按 Fig.10 下半球模式发射
    //     新粒子(继承样本速度/颜料), 每样本每步限 max_emit_per_step=1
    //     (§5.1: "set a limit on the maximum number of particles that
    //     can be absorbed or emitted by a sample per time step"),
    //     另有全局出生预算 WB_EMIT_BUDGET 兜底。
    if (field->pen_down) {
        Ruzino::BristleLiquidConstants blc = {};
        blc.num_bristles = Nb;
        blc.samples_per_bristle = S;
        blc.mu = 0.5f;

        blc.M_max = WetbrushSimState::WB_M_MAX;
        blc.M_min = 0.005f;
        blc.rho_0 = 12.3f;
        // ε 出墨滞回带 —— 论文 Table 1 值 0.1。2026-09-03 A/B 实测（账本
        // probe _wb_inkvalve_*）证伪了"0.002 过冲跨不过滞回带"假设：
        // ε=0.01 vs 0.1 落布 47% vs 46%、尾段增长 1,484 vs 1,444，无实质
        // 差异 —— 样本平台的真正机制是 m_j = M_j 完美停滞（无表面 → 无压
        // 缩 → 容量不降 → 按设计不出墨），根本不在滞回带里。默认保持论文
        // 原文值；WB_EMIT_EPS=<v> 仅作 A/B 用。
        blc.eps_emit = [] {
            const char* e = std::getenv("WB_EMIT_EPS");
            float v = e ? static_cast<float>(std::atof(e)) : 0.1f;
            return v > 0.0f ? v : 0.1f;
        }();

        // Emission throttle sweep knobs (paper §5.1 sets "a limit on the
        // maximum number of particles ... per time step" without a value):
        // WB_EMIT_PER_STEP   — per-sample births/step (paper cap; default 1)
        // WB_EMIT_MASS_SCALE — multiplier on per-birth mass
        //                      max(M_j*0.05, 0.002) (default 1)
        static const int emit_per_step = [] {
            const char* env = std::getenv("WB_EMIT_PER_STEP");
            return env ? std::max(std::atoi(env), 1) : 1;
        }();
        blc.max_emit_per_step = emit_per_step;
        static const float emit_mass_scale = [] {
            const char* env = std::getenv("WB_EMIT_MASS_SCALE");
            float v = env ? static_cast<float>(std::atof(env)) : 1.0f;
            return v > 0.0f ? v : 1.0f;
        }();
        blc.emit_mass_scale = emit_mass_scale;
        blc.grid_res = field->grid_res;
        blc.grid_res_z = WIN_Z;
        blc.height_extent = field->grid_height;
        blc.grid_center_z = field->grid_center_z;
        blc.cell_size = cell_sz;
        blc.paper_size = field->grid_paper;
        blc.grid_center_x = field->grid_center.x;
        blc.grid_center_y = field->grid_center.y;
        blc.D0 = brush_radius * 3.0f;
        blc.max_particles = max_ptcl;

        static const int emit_budget = [] {
            const char* env = std::getenv("WB_EMIT_BUDGET");
            return env ? std::max(std::atoi(env), 0) : 0;
        }();
        blc.emit_budget = emit_budget;

        blc.brush_pos_x = field->prev_brush_pos.x;
        blc.brush_pos_y = field->prev_brush_pos.y;
        blc.brush_pos_z = field->prev_brush_pos.z;
        blc.window_origin_x = field->win_origin_x;
        blc.window_origin_y = field->win_origin_y;
        blc.window_origin_z = 0;
        blc.window_size_x = WIN_XY;
        blc.window_size_z = WIN_Z;

        blc.dip_frame = field->dip_frame ? 1 : 0;
        field->dip_frame = false;

        nvrhi::BufferHandle liquid_cb;
        Ruzino::brush_upload_cb(
            rc, device, &blc, sizeof(blc), "wb_liquid_cb", liquid_cb);

        if (!field->bri_liquid_transfer_program)
            field->bri_liquid_transfer_program = Ruzino::brush_compile_shader(
                rc, "bristle_liquid_transfer.slang");
        if (!field->bri_liquid_emit_program)
            field->bri_liquid_emit_program =
                Ruzino::brush_compile_shader(rc, "bristle_liquid_emit.slang");

        Ruzino::brush_dispatch(
            rc,
            field->bri_liquid_transfer_program,
            { { "sample_pos", field->sample_pos },
              { "sample_color", field->sample_color },
              { "sample_liquid_in", field->sample_liquid },
              { "bristle_psi", field->bristle_density },
              { "grid_density", field->density },
              { "grid_color_r", field->color_r },
              { "grid_color_y", field->color_y },
              { "grid_color_b", field->color_b },
              // Eq.14 吸收: 读上一帧压实后的粒子池([0, counter) 紧凑存活),
              // 经 pt_alive 的原子认领杀掉被吸收粒子(仅 PASS 0 声明这些绑定)
              { "ptcl_pos", field->ptcl_pos },
              { "ptcl_color", field->ptcl_color },
              { "ptcl_counter_srv", field->ptcl_counter } },
            { { "sample_liquid_out", field->sample_liquid_b },
              { "pt_alive", field->ptcl_alive } },
            liquid_cb,
            Nb * S);
        std::swap(field->sample_liquid, field->sample_liquid_b);

        Ruzino::brush_reset_counter(rc, device, field->emit_budget);
        Ruzino::brush_dispatch(
            rc,
            field->bri_liquid_emit_program,
            { { "sample_pos", field->sample_pos },
              { "sample_color", field->sample_color },
              { "sample_vel", field->sample_vel },
              { "sample_liquid_in", field->sample_liquid },
              { "bristle_psi", field->bristle_density },
              { "grid_density", field->density },
              { "grid_color_r", field->color_r },
              { "grid_color_y", field->color_y },
              { "grid_color_b", field->color_b } },
            { { "sample_liquid_out", field->sample_liquid_b },
              { "ptcl_counter", field->ptcl_counter },
              { "emit_budget", field->emit_budget },
              { "ptcl_pos_out", field->ptcl_pos },
              { "ptcl_vel_out", field->ptcl_vel },
              { "ptcl_color_out", field->ptcl_color },
              { "ptcl_alive_out", field->ptcl_alive } },
            liquid_cb,
            Nb * S);
        std::swap(field->sample_liquid, field->sample_liquid_b);

        rc.destroy(liquid_cb);
    }

    float viscosity = params.get_input<float>("Viscosity");
    float diffusion = params.get_input<float>("Diffusion Rate");
    float drying_rate = params.get_input<float>("Drying Rate");

    const int window_total = win_n3d;

    const int global_n3d = field->grid_res * field->grid_res * WIN_Z;

    static int xfer_frame = 0;
    const bool ledger_probe = [] {
        const char* e = std::getenv("WB_LEDGER_PROBE");
        return e && e[0] == '1';
    }();
    const int xf = xfer_frame++;
    auto probe_grid_sum = [&](const char* tag) {
        if (!ledger_probe)
            return;
        if (!(xf >= 108 || xf == 30))
            return;
        std::vector<float> data(global_n3d);
        auto rb = rc.create(
            nvrhi::BufferDesc{}
                .setByteSize(static_cast<size_t>(global_n3d) * sizeof(float))
                .setCpuAccess(nvrhi::CpuAccessMode::Read)
                .setDebugName("wb_xfer_rb"));
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->copyBuffer(
            rb,
            0,
            field->density,
            0,
            static_cast<size_t>(global_n3d) * sizeof(float));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
        memcpy(
            data.data(),
            mapped,
            static_cast<size_t>(global_n3d) * sizeof(float));
        device->unmapBuffer(rb);
        rc.destroy(rb);
        rc.destroy(cmd);
        double sum = 0.0;
        int negs = 0;
        for (int i = 0; i < global_n3d; ++i) {
            sum += data[i];
            if (data[i] < -1e-4f)
                ++negs;
        }
        spdlog::info("[wb-xfer] f={} {} sum={:.1f} neg={}", xf, tag, sum, negs);
    };
    probe_grid_sum("A_in");  // 质量账本探针(WB_LEDGER_PROBE), 非论文内容

    // 笔刷平动/角加速度按论文原样下传: 粒子 Eq.9 的牵连平动项 a_B 与
    // Euler 角加速度项经 β_L=0.1 衰减后进入局部系积分(此前这里强制清零
    // 属旧行为遗留, 已按论文复位; 若再现加速度尖峰伪影, 先查输入数据)。

    // Table 1 参数(1u=1cm): D0=1cm 网格↔粒子转换半径(§5.2 / Figure 5),
    // D1=0.3cm 笔毛粘附力程(Eq.10)。
    const float D0 = 1.0f;
    const float D1 = 0.3f;

    // §5.2 Eq.16 的 "moves slowly" 门(论文未给数值): 慢于 ~2cm/s 才把
    // 质量还给网格; WB_DEPOSIT_SLOW 可覆盖, 0 = 关门(无条件沉积)。
    const float slow_deposit_speed = [] {
        const char* env = std::getenv("WB_DEPOSIT_SLOW");
        if (!env)
            return 2.0f;
        float v = std::atof(env);
        return v > 0.0f ? v : 1.0e30f;
    }();

    // 重力(默认 -981 cm/s², 1u=1cm 标定): 供粒子 Eq.9 的外部加速度 a_k
    // 与网格体力(damp_dry)使用。
    const glm::vec3 gravity = [] {
        auto envf = [](const char* k, float d) {
            const char* v = std::getenv(k);
            return v ? std::atof(v) : d;
        };
        return glm::vec3(
            envf("WB_GRAVITY_X", 0.0f),
            envf("WB_GRAVITY_Y", 0.0f),
            envf("WB_GRAVITY_Z", -981.0f));
    }();

    ensure_prog(field->advect_program, "fluid_advect.slang");

    static const char* scalar_advect_shader = [] {
        const char* env = std::getenv("WB_SCALAR_ADVECT");
        return env && std::strcmp(env, "upwind") == 0
                   ? "fluid_advect_upwind.slang"
                   : "fluid_advect.slang";
    }();
    ensure_prog(field->advect_scalar_program, scalar_advect_shader);
    ensure_prog(field->jacobi_program, "fluid_jacobi.slang");
    ensure_prog(field->divergence_program, "fluid_divergence.slang");
    ensure_prog(field->gradient_program, "fluid_gradient.slang");
    ensure_prog(field->damp_dry_program, "fluid_damp_dry.slang");
    ensure_prog(field->field_clear_program, "field_clear.slang");
    ensure_prog(field->ptcl_emit_program, "particle_emit.slang");
    ensure_prog(field->ptcl_update_program, "particle_update.slang");
    ensure_prog(field->ptcl_raster_program, "particle_rasterize.slang");
    ensure_prog(field->bristle_merge_program, "bristle_merge.slang");
    ensure_prog(field->ptcl_flip_pic_program, "particle_flip_pic.slang");
    ensure_prog(field->ptcl_compact_program, "particle_compact.slang");
    ensure_prog(field->ptcl_to_grid_program, "particle_to_grid.slang");
    ensure_prog(field->grid_to_ptcl_program, "grid_to_particle.slang");
    ensure_prog(field->field_copy_window_program, "field_copy_window.slang");

    // ---- 粒子液体更新 + 联合速度场(§4.3, 落笔时) ----
    // 常数: δ=friction_delta=5/D0=1/0.2cm(Table 1 摩擦力程), γ=flip_gamma
    // =0.8(Eq.11 混合系数), D1=0.3cm(Eq.10 粘附力程)。
    // [偏离] 粒子 dt 硬编码 0.016s(60fps 帧长), 不随真实帧时长缩放。
    // 流程 = §4.3:
    //   ① particle_update — Eq.8 固体摩擦(1-δ·d_k)² + Eq.9 最近笔毛样本
    //     局部系积分(β_L=0.1 ∈ 论文 [0,0.2], 四项惯性加速度) + Eq.10 按
    //     max(1-d_B/D1, 0) 与画布系显式积分结果混合 —— 两步法以位置方式
    //     实现粘附(Figure 8b), 避开刚性粘附力的显式积分不稳定;
    //   ② 清空后 particle_rasterize — 粒子栅格化为密度/速度/颜色场
    //     (§4.3 末段 "we rasterize them into density and velocity fields");
    //   ③ bristle_merge — 粒子动量并入网格速度, 得联合速度场 u, 下方
    //     粘度/压力投影即在联合场上进行(§4.3: "we perform them on the
    //     joint velocity field"), 实现粒子↔网格液体的双向耦合。合并只改
    //     速度: 质量入网格仅经 §5.2 的 particle_to_grid(否则供给→发射→
    //     栅格化的回路每帧凭空造质量)。
    if (field->particles_initialized && bp.active) {
        Ruzino::ParticleConstants pc = {};
        pc.max_particles = max_ptcl;
        pc.dt = 0.016f;
        pc.D0 = D0;
        pc.pen_down = 1;
        pc.friction_delta = 5.0f / D0;
        pc.flip_gamma = 0.8f;
        pc.grid_res = field->grid_res;
        pc.grid_res_z = WIN_Z;
        pc.height_extent = field->grid_height;
        pc.grid_center_z = field->grid_center_z;
        pc.cell_size = cell_sz;
        pc.paper_size = field->grid_paper;
        pc.grid_center_x = field->grid_center.x;
        pc.grid_center_y = field->grid_center.y;
        pc.window_origin_x = field->win_origin_x;
        pc.window_origin_y = field->win_origin_y;
        pc.window_origin_z = 0;
        pc.window_size_x = WIN_XY;
        pc.window_size_z = WIN_Z;
        pc.brush_pos_x = brush_pos_3d.x;
        pc.brush_pos_y = brush_pos_3d.y;
        pc.brush_pos_z = brush_pos_3d.z;
        pc.brush_radius = brush_radius;
        pc.D1 = D1;

        pc.brush_vel_x = field->prev_brush_vel.x;
        pc.brush_vel_y = field->prev_brush_vel.y;
        pc.brush_vel_z = field->prev_brush_vel.z;
        pc.num_bristles = Nb;
        pc.samples_per_bristle = S;
        pc.gravity_x = gravity.x;
        pc.gravity_y = gravity.y;
        pc.gravity_z = gravity.z;
        pc.brush_accel_x = brush_accel_3d.x;
        pc.brush_accel_y = brush_accel_3d.y;
        pc.brush_accel_z = brush_accel_3d.z;
        pc.brush_angular_accel_x = brush_angular_accel.x;
        pc.brush_angular_accel_y = brush_angular_accel.y;
        pc.brush_angular_accel_z = brush_angular_accel.z;

        nvrhi::BufferHandle ptcl_cb;
        Ruzino::brush_upload_cb(
            rc, device, &pc, sizeof(pc), "wb_ptcl_cb", ptcl_cb);

        Ruzino::brush_dispatch(
            rc,
            field->ptcl_update_program,
            { { "ptcl_pos", field->ptcl_pos },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_color", field->ptcl_color },
              { "ptcl_alive", field->ptcl_alive },
              { "sample_pos", field->sample_pos },
              { "sample_frame", field->sample_frame },
              { "grid_vel_x", field->vel_x },
              { "grid_vel_y", field->vel_y },
              { "grid_vel_z", field->vel_z } },
            { { "ptcl_pos_out", field->ptcl_pos_b },
              { "ptcl_vel_out", field->ptcl_vel_b },
              { "ptcl_alive_out", field->ptcl_alive_b } },
            ptcl_cb,
            max_ptcl);
        std::swap(field->ptcl_pos, field->ptcl_pos_b);
        std::swap(field->ptcl_vel, field->ptcl_vel_b);
        std::swap(field->ptcl_alive, field->ptcl_alive_b);

        auto clear_grid = [&](auto& buf) {
            Ruzino::brush_dispatch(
                rc,
                field->field_clear_program,
                {},
                { { "field", buf } },
                nullptr,
                win_n3d);
        };
        clear_grid(field->ptcl_density);
        clear_grid(field->ptcl_vel_x);
        clear_grid(field->ptcl_vel_y);
        clear_grid(field->ptcl_vel_z);
        clear_grid(field->ptcl_rast_r);
        clear_grid(field->ptcl_rast_y);
        clear_grid(field->ptcl_rast_b);

        Ruzino::brush_dispatch(
            rc,
            field->ptcl_raster_program,
            { { "ptcl_pos", field->ptcl_pos },
              { "ptcl_color", field->ptcl_color },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_alive", field->ptcl_alive } },
            { { "ptcl_density", field->ptcl_density },
              { "ptcl_vel_x", field->ptcl_vel_x },
              { "ptcl_vel_y", field->ptcl_vel_y },
              { "ptcl_vel_z", field->ptcl_vel_z },
              { "ptcl_color_r", field->ptcl_rast_r },
              { "ptcl_color_y", field->ptcl_rast_y },
              { "ptcl_color_b", field->ptcl_rast_b } },
            ptcl_cb,
            max_ptcl);

        Ruzino::SimConstants mc2 = {};
        mc2.res = field->grid_res;
        mc2.cell_size = cell_sz;
        mc2.paper_size = field->grid_paper;
        mc2.ink_amount = ink_amount;
        mc2.oil_density_base = oil_density_in;
        mc2.window_origin_x = field->win_origin_x;
        mc2.window_origin_y = field->win_origin_y;
        mc2.window_origin_z = 0;
        mc2.window_size_x = WIN_XY;
        mc2.window_size_y = WIN_XY;
        mc2.window_size_z = WIN_Z;

        static const float vel_inject_scale = [] {
            const char* env = std::getenv("WB_VEL_INJECT");
            return env ? std::max(std::atof(env), 0.0) : 1.0;
        }();
        mc2.velocity_inject_scale = vel_inject_scale;
        nvrhi::BufferHandle merge_cb;
        Ruzino::brush_upload_cb(
            rc, device, &mc2, sizeof(mc2), "wb_ptcl_merge_cb", merge_cb);

        static const bool disable_merge = [] {
            const char* env = std::getenv("WB_DISABLE_MERGE");
            return env && std::atoi(env) == 1;
        }();
        if (!disable_merge) {
            Ruzino::brush_dispatch(
                rc,
                field->bristle_merge_program,
                { { "bristle_density", field->ptcl_density },
                  { "bristle_vel_x", field->ptcl_vel_x },
                  { "bristle_vel_y", field->ptcl_vel_y },
                  { "bristle_vel_z", field->ptcl_vel_z } },
                { { "vel_x", field->vel_x },
                  { "vel_y", field->vel_y },
                  { "vel_z", field->vel_z } },
                merge_cb,
                win_n3d);
        }
        rc.destroy(merge_cb);
        rc.destroy(ptcl_cb);
    }
    // 抬笔(不做粒子更新): 清空粒子/笔毛栅格累加器, 避免陈旧足印在压力
    // 投影中被当作固体边界。
    else if (field->particles_initialized) {
        nvrhi::BufferHandle* rast_bufs[] = {
            std::addressof(field->ptcl_density),
            std::addressof(field->ptcl_rast_r),
            std::addressof(field->ptcl_rast_y),
            std::addressof(field->ptcl_rast_b),
            std::addressof(field->bristle_density),
            std::addressof(field->bristle_vel_x),
            std::addressof(field->bristle_vel_y),
            std::addressof(field->bristle_vel_z),
        };
        for (nvrhi::BufferHandle* buf : rast_bufs) {
            Ruzino::brush_dispatch(
                rc,
                field->field_clear_program,
                {},
                { { "field", *buf } },
                nullptr,
                win_n3d);
        }
    }

    // ---- 网格流体求解(§4.2, 活动窗口内) ----
    // sim_dt 上限 0.05s; 子步长 ≤ 2 格(CFL 式), 至多 16 子步 —— 粘度
    // 扩散与半拉格朗日平流在大步长下的稳定性手段之一。
    float sim_dt = std::min(dt, 0.05f);
    int wox = field->win_origin_x;
    int woy = field->win_origin_y;

    if (sim_dt > 1e-6f) {
        float max_sub_dt = 2.0f / static_cast<float>(field->grid_res);
        int substeps =
            std::max(1, static_cast<int>(std::ceil(sim_dt / max_sub_dt)));
        substeps = std::min(substeps, 16);
        float sub_dt = sim_dt / static_cast<float>(substeps);

        static const float vel_damp_frame = [] {
            const char* env = std::getenv("WB_VEL_DAMP");
            float v = env ? std::atof(env) : 1.0f;
            return std::min(std::max(v, 0.0f), 1.0f);
        }();
        const float vel_damp_sub =
            vel_damp_frame > 0.0f
                ? std::pow(vel_damp_frame, 1.0f / static_cast<float>(substeps))
                : 1.0f;

        static const bool stage_dump = [] {
            const char* env = std::getenv("WB_STAGE_DUMP");
            return env && std::atoi(env) == 1;
        }();
        static int stage_frame = 0;
        ++stage_frame;
        auto buf_absmax = [&](const nvrhi::BufferHandle& buf) -> float {
            std::vector<float> data(win_n3d);
            auto rb = rc.create(
                nvrhi::BufferDesc{}
                    .setByteSize(static_cast<size_t>(win_n3d) * sizeof(float))
                    .setCpuAccess(nvrhi::CpuAccessMode::Read)
                    .setDebugName("wb_stage_rb"));
            auto cmd = rc.create(CommandListDesc{});
            cmd->open();
            cmd->copyBuffer(
                rb, 0, buf, 0, static_cast<size_t>(win_n3d) * sizeof(float));
            cmd->close();
            device->executeCommandList(cmd);
            device->waitForIdle();
            void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
            memcpy(
                data.data(),
                mapped,
                static_cast<size_t>(win_n3d) * sizeof(float));
            device->unmapBuffer(rb);
            rc.destroy(rb);
            rc.destroy(cmd);
            float mx = 0.0f;
            for (float v : data)
                mx = std::max(mx, std::fabs(v));
            return mx;
        };

        auto buf_absmax_loc = [&](const nvrhi::BufferHandle& buf,
                                  int& lx,
                                  int& ly,
                                  int& lz) -> float {
            std::vector<float> data(win_n3d);
            auto rb = rc.create(
                nvrhi::BufferDesc{}
                    .setByteSize(static_cast<size_t>(win_n3d) * sizeof(float))
                    .setCpuAccess(nvrhi::CpuAccessMode::Read)
                    .setDebugName("wb_stage_rb"));
            auto cmd = rc.create(CommandListDesc{});
            cmd->open();
            cmd->copyBuffer(
                rb, 0, buf, 0, static_cast<size_t>(win_n3d) * sizeof(float));
            cmd->close();
            device->executeCommandList(cmd);
            device->waitForIdle();
            void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
            memcpy(
                data.data(),
                mapped,
                static_cast<size_t>(win_n3d) * sizeof(float));
            device->unmapBuffer(rb);
            rc.destroy(rb);
            rc.destroy(cmd);
            float mx = 0.0f;
            int best = 0;
            for (int i = 0; i < win_n3d; ++i) {
                float a = std::fabs(data[i]);
                if (a > mx) {
                    mx = a;
                    best = i;
                }
            }
            const int wxy = WIN_XY * WIN_XY;
            lz = best / wxy;
            int rem = best - lz * wxy;
            ly = rem / WIN_XY;
            lx = rem - ly * WIN_XY;
            return data[best];
        };
        auto vel_stage = [&](const char* stage) {
            if (!stage_dump)
                return;
            int lx = -1, ly = -1, lz = -1;
            float mv = 0.0f;
            int comp = -1;
            float sv = buf_absmax_loc(field->vel_x, lx, ly, lz);
            if (std::fabs(sv) >= std::fabs(mv)) {
                mv = sv;
                comp = 0;
            }
            int x2, y2, z2;
            sv = buf_absmax_loc(field->vel_y, x2, y2, z2);
            if (std::fabs(sv) > std::fabs(mv)) {
                mv = sv;
                comp = 1;
                lx = x2;
                ly = y2;
                lz = z2;
            }
            sv = buf_absmax_loc(field->vel_z, x2, y2, z2);
            if (std::fabs(sv) > std::fabs(mv)) {
                mv = sv;
                comp = 2;
                lx = x2;
                ly = y2;
                lz = z2;
            }
            spdlog::info(
                "[wb-stage] f={} {} vabsmax={:.5f} at (lx{},ly{},lz{},c{})",
                stage_frame,
                stage,
                mv,
                lx,
                ly,
                lz,
                comp);
        };
        auto solve_stage = [&](const char* stage) {
            if (!stage_dump)
                return;
            int lx, ly, lz;
            float dv = buf_absmax_loc(field->divergence_buf, lx, ly, lz);
            spdlog::info(
                "[wb-stage] f={} {} div={} at (lx{},ly{},lz{}) pmax={:.5f}",
                stage_frame,
                stage,
                dv,
                lx,
                ly,
                lz,
                buf_absmax(field->pressure_a));
        };

        // 每子步顺序(§4.2 标准欧拉求解, 作用在 §4.3 的联合速度场上):
        //  1) vel→vel_old 备份(Eq.11 里的旧场 u(p_k));
        //  2) 粘度: 每分量 3 次隐式 Jacobi, α=dt·ν/h²;
        //  3) 压力投影 #1(Algorithm 1, 见 project);
        //  4) 速度半拉格朗日平流(Stam 1999, 窗口局部系);
        //  5) 压力投影 #2(平流后再投一次; 论文 Algorithm 1 每步一次,
        //     此处属加强, 抵制平流引入的散度);
        //  6) 标量场平流: density/颜色 RYB/wetness/oil_density(全局场,
        //     默认半拉格朗日; WB_SCALAR_ADVERT=upwind 为守恒迎风逃生口);
        //  7) 干燥/固化: 全局 wetness 衰减(§4.2 "increase the dryness of
        //     every grid cell") + 窗口速度阻尼/重力体力/干化单元清零
        //     (§4.2 "we ignore its velocity ... as a solid cell");
        //  8) FLIP/PIC 粒子速度修正(Eq.11)。
        for (int s = 0; s < substeps; s++) {
            Ruzino::SimConstants fluid_cb = {};
            fluid_cb.res = field->grid_res;
            fluid_cb.res_z = WIN_Z;
            fluid_cb.height_extent = field->grid_height;
            fluid_cb.grid_center_z = field->grid_center_z;
            fluid_cb.cell_size = cell_sz;
            fluid_cb.paper_size = field->grid_paper;
            fluid_cb.dt = sub_dt;
            fluid_cb.viscosity = viscosity;
            fluid_cb.diffusion = diffusion;
            fluid_cb.drying_rate = drying_rate;
            fluid_cb.oil_density_base = oil_density_in;
            fluid_cb.window_origin_x = wox;
            fluid_cb.window_origin_y = woy;
            fluid_cb.window_origin_z = 0;
            fluid_cb.window_size_x = WIN_XY;
            fluid_cb.window_size_y = WIN_XY;
            fluid_cb.window_size_z = WIN_Z;

            static const bool no_project = [] {
                const char* env = std::getenv("WB_NO_PROJECT");
                return env && std::atoi(env) == 1;
            }();
            static const float wall_gate = [] {
                const char* env = std::getenv("WB_NO_BRUSH_WALL");
                if (env && std::atoi(env) == 1)
                    return 1e9f;

                const char* gate_env = std::getenv("WB_WALL_GATE");
                return gate_env ? static_cast<float>(
                                      std::max(std::atof(gate_env), 0.0))
                                : 0.01f;
            }();
            fluid_cb.brush_boundary_gate = wall_gate;
            fluid_cb.velocity_damp = vel_damp_sub;
            fluid_cb.copy_mode = 0;
            fluid_cb.advect_field_window_local = 0;
            fluid_cb.gravity_x = gravity.x;
            fluid_cb.gravity_y = gravity.y;
            fluid_cb.gravity_z = gravity.z;

            nvrhi::BufferHandle cb_buf;
            Ruzino::brush_upload_cb(
                rc, device, &fluid_cb, sizeof(fluid_cb), "wb_fluid_cb", cb_buf);

            Ruzino::brush_dispatch(
                rc,
                field->field_copy_window_program,
                { { "src_field", field->vel_x } },
                { { "dst_field", field->vel_x_old } },
                cb_buf,
                win_n3d);
            Ruzino::brush_dispatch(
                rc,
                field->field_copy_window_program,
                { { "src_field", field->vel_y } },
                { { "dst_field", field->vel_y_old } },
                cb_buf,
                win_n3d);
            Ruzino::brush_dispatch(
                rc,
                field->field_copy_window_program,
                { { "src_field", field->vel_z } },
                { { "dst_field", field->vel_z_old } },
                cb_buf,
                win_n3d);

            device->waitForIdle();
            if (s == 0)
                vel_stage("pre_diffuse");

            fluid_cb.jacobi_mode = 0;

            // 隐式粘度扩散: (I - dt·ν∇²) v_new = v_old 的 Jacobi 迭代,
            // α=dt·ν/h²(ν 单位 cm²/s); 每分量 3 次, 欠收敛的近似粘性
            // (fluid_jacobi mode 0, Neumann 边界)。
            fluid_cb.jacobi_alpha = sub_dt * viscosity / (cell_sz * cell_sz);
            {
                nvrhi::BufferHandle jcb;
                Ruzino::brush_upload_cb(
                    rc,
                    device,
                    &fluid_cb,
                    sizeof(fluid_cb),
                    "wb_jacobi_cb",
                    jcb);

                nvrhi::BufferHandle* vel_pairs[3][2] = {
                    { std::addressof(field->vel_x),
                      std::addressof(field->vel_x_tmp) },
                    { std::addressof(field->vel_y),
                      std::addressof(field->vel_y_tmp) },
                    { std::addressof(field->vel_z),
                      std::addressof(field->vel_z_tmp) },
                };
                for (auto& pp : vel_pairs) {
                    nvrhi::BufferHandle& in = *pp[0];
                    nvrhi::BufferHandle& out = *pp[1];

                    for (int vs = 0; vs < 3; vs++) {
                        Ruzino::brush_dispatch(
                            rc,
                            field->jacobi_program,
                            { { "field_in", in },
                              { "rhs", in },
                              { "wetness", field->wetness },
                              { "density", field->density },
                              { "bristle_density", field->bristle_density },

                              { "ptcl_density", field->ptcl_density } },
                            { { "field_out", out } },
                            jcb,
                            window_total);
                        std::swap(in, out);
                    }
                }
                rc.destroy(jcb);
            }
            if (s == 0)
                vel_stage("post_diffuse");

            // 定点加速压力投影(Algorithm 1): L=3 轮, 每轮 = 散度 D=∇·u →
            // 2 次 Jacobi(Eq.3 的 y-更新) → u -= ∇P; 共 6 次 Jacobi, α=1
            // 免调参(§4.2: "three fixed-point iterations, or six Jacobi
            // iterations")。Eq.5 的误差递推与 Chebyshev 半迭代法同构,
            // 复数特征值使前几轮收敛最快(§4.2 / Figure 6)。边界条件
            // (fluid_jacobi/divergence/gradient): 干化颜料与笔毛足印 =
            // 固体(Neumann 镜面, §4.2 "treat it as a solid cell"), 空气
            // = 自由面(Dirichlet p=0), 笔毛速度场作墙面速度。
            auto project = [&]() {
                for (int fp = 0; fp < 3; fp++) {
                    Ruzino::brush_dispatch(
                        rc,
                        field->divergence_program,
                        { { "vel_x", field->vel_x },
                          { "vel_y", field->vel_y },
                          { "vel_z", field->vel_z },
                          { "wetness", field->wetness },
                          { "density", field->density },
                          { "bristle_density", field->bristle_density },
                          { "bristle_vel_x", field->bristle_vel_x },
                          { "bristle_vel_y", field->bristle_vel_y },
                          { "bristle_vel_z", field->bristle_vel_z },
                          { "ptcl_density", field->ptcl_density } },
                        { { "div_out", field->divergence_buf } },
                        cb_buf,
                        window_total);

                    fluid_cb.jacobi_mode = 1;
                    nvrhi::BufferHandle pcb;
                    Ruzino::brush_upload_cb(
                        rc,
                        device,
                        &fluid_cb,
                        sizeof(fluid_cb),
                        "wb_press_cb",
                        pcb);
                    for (int ji = 0; ji < 2; ji++) {
                        Ruzino::brush_dispatch(
                            rc,
                            field->jacobi_program,
                            { { "field_in", field->pressure_a },
                              { "rhs", field->divergence_buf },
                              { "wetness", field->wetness },
                              { "density", field->density },
                              { "bristle_density", field->bristle_density },
                              { "ptcl_density", field->ptcl_density } },
                            { { "field_out", field->pressure_b } },
                            pcb,
                            window_total);
                        std::swap(field->pressure_a, field->pressure_b);
                    }
                    rc.destroy(pcb);

                    Ruzino::brush_dispatch(
                        rc,
                        field->gradient_program,
                        { { "pressure", field->pressure_a },
                          { "wetness", field->wetness },
                          { "density", field->density },
                          { "bristle_density", field->bristle_density },
                          { "bristle_vel_x", field->bristle_vel_x },
                          { "bristle_vel_y", field->bristle_vel_y },
                          { "bristle_vel_z", field->bristle_vel_z },
                          { "ptcl_density", field->ptcl_density } },
                        { { "vel_x", field->vel_x },
                          { "vel_y", field->vel_y },
                          { "vel_z", field->vel_z } },
                        cb_buf,
                        window_total);
                }
            };

            if (!no_project)
                project();
            if (s == 0) {
                vel_stage("post_project1");
                solve_stage("after_project1");
            }

            fluid_cb.advect_field_window_local = 1;
            nvrhi::BufferHandle advect_vel_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &fluid_cb,
                sizeof(fluid_cb),
                "wb_adv_vel_cb",
                advect_vel_cb);
            nvrhi::BufferHandle* advect_pairs[3][2] = {
                { std::addressof(field->vel_x),
                  std::addressof(field->vel_x_tmp) },
                { std::addressof(field->vel_y),
                  std::addressof(field->vel_y_tmp) },
                { std::addressof(field->vel_z),
                  std::addressof(field->vel_z_tmp) },
            };
            for (auto& pp : advect_pairs) {
                nvrhi::BufferHandle& in = *pp[0];
                nvrhi::BufferHandle& out = *pp[1];
                Ruzino::brush_dispatch(
                    rc,
                    field->advect_program,
                    { { "field_in", in },
                      { "vel_x", field->vel_x },
                      { "vel_y", field->vel_y },
                      { "vel_z", field->vel_z } },
                    { { "field_out", out } },
                    advect_vel_cb,
                    window_total);
                std::swap(in, out);
            }
            rc.destroy(advect_vel_cb);
            if (s == 0)
                vel_stage("post_advect");

            if (!no_project)
                project();
            if (s == 0) {
                vel_stage("post_project2");
                solve_stage("after_project2");
            }

            // 标量场平流(§4.2: "we advect all of the fields using the
            // semi-Lagrangian method"): 画布标量是全局场, 回溯在全局坐标,
            // 结果写窗口 tmp 后整窗拷回(copy_mode=2)。上方的速度平流则
            // 全程在窗口局部系内进行。
            fluid_cb.copy_mode = 2;

            fluid_cb.advect_field_window_local = 0;
            nvrhi::BufferHandle advect_scalar_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &fluid_cb,
                sizeof(fluid_cb),
                "wb_adv_scalar_cb",
                advect_scalar_cb);

            auto advect_scalar = [&](nvrhi::BufferHandle& f,
                                     nvrhi::BufferHandle& tmp) {
                Ruzino::brush_dispatch(
                    rc,
                    field->advect_scalar_program,
                    { { "field_in", f },
                      { "vel_x", field->vel_x },
                      { "vel_y", field->vel_y },
                      { "vel_z", field->vel_z } },
                    { { "field_out", tmp } },
                    advect_scalar_cb,
                    window_total);
                Ruzino::brush_dispatch(
                    rc,
                    field->field_copy_window_program,
                    { { "src_field", tmp } },
                    { { "dst_field", f } },
                    advect_scalar_cb,
                    win_n3d);
            };
            advect_scalar(field->density, field->density_tmp);
            advect_scalar(field->color_r, field->color_r_tmp);
            advect_scalar(field->color_y, field->color_y_tmp);
            advect_scalar(field->color_b, field->color_b_tmp);
            advect_scalar(field->wetness, field->wetness_tmp);
            advect_scalar(field->oil_density, field->oil_density_tmp);
            rc.destroy(advect_scalar_cb);

            // 干燥/固化两连发(§4.2): mode0 全局 —— wetness 按 drying_rate
            // 衰减(即 dryness 增加; §4.2 对"每个网格单元"执行, 与窗口无
            // 关); mode1 窗口 —— 速度阻尼(WB_VEL_DAMP, 默认关) + 流体单元
            // 重力体力(经投影转为沿画布的摊铺) + 干化颜料单元速度清零
            // ("once the dryness ... reaches a threshold, we ignore its
            // velocity")。注意空单元 ≠ 干化单元(见 fluid_damp_dry.slang)。
            fluid_cb.damp_mode = 0;
            nvrhi::BufferHandle damp_global_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &fluid_cb,
                sizeof(fluid_cb),
                "wb_damp_g_cb",
                damp_global_cb);
            Ruzino::brush_dispatch(
                rc,
                field->damp_dry_program,
                { { "density", field->density } },
                { { "wetness", field->wetness } },
                damp_global_cb,
                global_n3d);
            rc.destroy(damp_global_cb);

            fluid_cb.damp_mode = 1;
            nvrhi::BufferHandle damp_window_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &fluid_cb,
                sizeof(fluid_cb),
                "wb_damp_w_cb",
                damp_window_cb);
            Ruzino::brush_dispatch(
                rc,
                field->damp_dry_program,
                { { "density", field->density } },
                { { "vel_x", field->vel_x },
                  { "vel_y", field->vel_y },
                  { "vel_z", field->vel_z },
                  { "wetness", field->wetness } },
                damp_window_cb,
                window_total);
            rc.destroy(damp_window_cb);

            // FLIP/PIC 粒子速度修正(Eq.11):
            //   v_k = γ·ū(p_k) + (1-γ)·(v_k + ū(p_k) - u(p_k))
            // 分别采样子步求解前(vel_*_old)与求解后(vel_*)的网格速度;
            // γ=0.8 偏向 PIC(平滑稳定, Table 1)。论文: 网格速度场保留到
            // 下一帧, 兼作笔毛拖拽(§4.1)与粒子/网格边界的追踪依据(免
            // 额外速度外推)。
            if (field->particles_initialized) {
                Ruzino::ParticleConstants pc = {};
                pc.max_particles = max_ptcl;
                pc.dt = sub_dt;
                pc.D0 = D0;
                pc.flip_gamma = 0.8f;
                pc.grid_res = field->grid_res;
                pc.grid_res_z = WIN_Z;
                pc.height_extent = field->grid_height;
                pc.grid_center_z = field->grid_center_z;
                pc.cell_size = cell_sz;
                pc.paper_size = field->grid_paper;
                pc.grid_center_x = field->grid_center.x;
                pc.grid_center_y = field->grid_center.y;
                pc.window_origin_x = field->win_origin_x;
                pc.window_origin_y = field->win_origin_y;
                pc.window_origin_z = 0;
                pc.window_size_x = WIN_XY;
                pc.window_size_z = WIN_Z;
                pc.brush_pos_x = brush_pos_3d.x;
                pc.brush_pos_y = brush_pos_3d.y;
                pc.brush_pos_z = brush_pos_3d.z;
                pc.brush_radius = brush_radius;
                pc.brush_vel_x = field->prev_brush_vel.x;
                pc.brush_vel_y = field->prev_brush_vel.y;
                pc.brush_vel_z = field->prev_brush_vel.z;

                nvrhi::BufferHandle flip_cb;
                Ruzino::brush_upload_cb(
                    rc, device, &pc, sizeof(pc), "wb_flip_cb", flip_cb);
                Ruzino::brush_dispatch(
                    rc,
                    field->ptcl_flip_pic_program,
                    { { "ptcl_pos", field->ptcl_pos },
                      { "ptcl_alive", field->ptcl_alive },
                      { "vel_x_old", field->vel_x_old },
                      { "vel_y_old", field->vel_y_old },
                      { "vel_z_old", field->vel_z_old },
                      { "vel_x_new", field->vel_x },
                      { "vel_y_new", field->vel_y },
                      { "vel_z_new", field->vel_z } },
                    { { "ptcl_vel", field->ptcl_vel } },
                    flip_cb,
                    max_ptcl);
                rc.destroy(flip_cb);
            }

            rc.destroy(cb_buf);
        }
    }

    // ---- 网格↔粒子液体转移(§5.2, 每帧一次) ----
    // ① particle_to_grid(Eq.16): 离全部笔毛样本超过 D0 且行进缓慢的粒子,
    //    按归一化 W 核把质量/RYB 颜色 scatter 回全局密度场后消亡 ——
    //    颜料"落布"即此步; 落点 wetness 置满(新沉积为湿, 由干燥步风干)。
    // ② grid_to_particle(Eq.15): 窗口内密度>0、未干透(§4.2 干透的颜料
    //    是固体, 不得被重新液化)、位于笔毛 D0 邻域的单元, 分层采样 27
    //    个候选转成粒子, 并按 W 核从密度/颜色场扣除对应质量 —— host 先
    //    把全局场拷进窗口 tmp 作种子(copy_mode=1), shader 原子减, 再整窗
    //    拷回(copy_mode=2), 保证 Eq.15 读-改-写与网格↔粒子往返严格守恒。
    // ③ particle_compact: 压实粒子池(剔除消亡粒子, 复位计数器)。
    if (field->particles_initialized) {
        Ruzino::ParticleConstants pc = {};
        pc.max_particles = max_ptcl;
        pc.dt = 0.016f;
        pc.D0 = D0;

        pc.D1 = D1;
        pc.grid_res = field->grid_res;
        pc.grid_res_z = WIN_Z;
        pc.height_extent = field->grid_height;
        pc.grid_center_z = field->grid_center_z;
        pc.cell_size = cell_sz;
        pc.paper_size = field->grid_paper;
        pc.grid_center_x = field->grid_center.x;
        pc.grid_center_y = field->grid_center.y;
        pc.window_origin_x = field->win_origin_x;
        pc.window_origin_y = field->win_origin_y;
        pc.window_origin_z = 0;
        pc.window_size_x = WIN_XY;
        pc.window_size_z = WIN_Z;
        pc.brush_pos_x = brush_pos_3d.x;
        pc.brush_pos_y = brush_pos_3d.y;
        pc.brush_pos_z = brush_pos_3d.z;
        pc.brush_radius = brush_radius;
        pc.brush_vel_x = field->prev_brush_vel.x;
        pc.brush_vel_y = field->prev_brush_vel.y;
        pc.brush_vel_z = field->prev_brush_vel.z;

        pc.num_bristles = Nb;
        pc.samples_per_bristle = S;
        pc.slow_deposit_speed = slow_deposit_speed;
        pc.gravity_x = gravity.x;
        pc.gravity_y = gravity.y;
        pc.gravity_z = gravity.z;

        pc.pen_down = bp.active ? 1 : 0;

        nvrhi::BufferHandle maint_cb;
        Ruzino::brush_upload_cb(
            rc, device, &pc, sizeof(pc), "wb_maint_cb", maint_cb);

        probe_grid_sum("B_solve");

        Ruzino::brush_dispatch(
            rc,
            field->ptcl_to_grid_program,
            { { "ptcl_pos", field->ptcl_pos },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_color", field->ptcl_color },
              { "ptcl_alive", field->ptcl_alive },
              { "sample_pos", field->sample_pos } },
            { { "density", field->density },
              { "color_r", field->color_r },
              { "color_y", field->color_y },
              { "color_b", field->color_b },
              { "ptcl_alive_out", field->ptcl_alive_b },
              { "wetness", field->wetness } },
            maint_cb,
            max_ptcl);
        std::swap(field->ptcl_alive, field->ptcl_alive_b);
        probe_grid_sum("C_p2g");

        {
            Ruzino::SimConstants seed_cb = {};
            seed_cb.res = field->grid_res;
            seed_cb.res_z = field->grid_res_z;
            seed_cb.window_origin_x = field->win_origin_x;
            seed_cb.window_origin_y = field->win_origin_y;
            seed_cb.window_origin_z = 0;
            seed_cb.window_size_x = WIN_XY;
            seed_cb.window_size_y = WIN_XY;
            seed_cb.window_size_z = WIN_Z;
            seed_cb.copy_mode = 1;
            nvrhi::BufferHandle g2p_copy_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &seed_cb,
                sizeof(seed_cb),
                "wb_g2p_copy_cb",
                g2p_copy_cb);
            auto win_copy = [&](nvrhi::BufferHandle& src,
                                nvrhi::BufferHandle& dst) {
                Ruzino::brush_dispatch(
                    rc,
                    field->field_copy_window_program,
                    { { "src_field", src } },
                    { { "dst_field", dst } },
                    g2p_copy_cb,
                    win_n3d);
            };
            win_copy(field->density, field->density_tmp);
            win_copy(field->color_r, field->color_r_tmp);
            win_copy(field->color_y, field->color_y_tmp);
            win_copy(field->color_b, field->color_b_tmp);
            rc.destroy(g2p_copy_cb);
        }
        Ruzino::brush_dispatch(
            rc,
            field->grid_to_ptcl_program,
            { { "density", field->density },
              { "color_r", field->color_r },
              { "color_y", field->color_y },
              { "color_b", field->color_b },
              { "vel_x", field->vel_x },
              { "vel_y", field->vel_y },
              { "vel_z", field->vel_z },

              { "wetness", field->wetness } },
            { { "ptcl_counter", field->ptcl_counter },
              { "ptcl_pos", field->ptcl_pos },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_color", field->ptcl_color },
              { "ptcl_alive", field->ptcl_alive },
              { "density_out", field->density_tmp },
              { "color_r_out", field->color_r_tmp },
              { "color_y_out", field->color_y_tmp },
              { "color_b_out", field->color_b_tmp } },
            maint_cb,
            win_n3d);

        {
            Ruzino::SimConstants writeback_cb = {};
            writeback_cb.res = field->grid_res;
            writeback_cb.res_z = field->grid_res_z;
            writeback_cb.window_origin_x = field->win_origin_x;
            writeback_cb.window_origin_y = field->win_origin_y;
            writeback_cb.window_origin_z = 0;
            writeback_cb.window_size_x = WIN_XY;
            writeback_cb.window_size_y = WIN_XY;
            writeback_cb.window_size_z = WIN_Z;
            writeback_cb.copy_mode = 2;
            nvrhi::BufferHandle g2p_wb_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &writeback_cb,
                sizeof(writeback_cb),
                "wb_g2p_wb_cb",
                g2p_wb_cb);
            auto copy_back = [&](nvrhi::BufferHandle& src,
                                 nvrhi::BufferHandle& dst) {
                Ruzino::brush_dispatch(
                    rc,
                    field->field_copy_window_program,
                    { { "src_field", src } },
                    { { "dst_field", dst } },
                    g2p_wb_cb,
                    win_n3d);
            };
            copy_back(field->density_tmp, field->density);
            copy_back(field->color_r_tmp, field->color_r);
            copy_back(field->color_y_tmp, field->color_y);
            copy_back(field->color_b_tmp, field->color_b);
            rc.destroy(g2p_wb_cb);
        }
        probe_grid_sum("D_g2p");

        Ruzino::brush_dispatch(
            rc,
            field->field_clear_program,
            {},
            { { "field", field->ptcl_alive_b } },
            nullptr,
            max_ptcl);
        Ruzino::brush_reset_counter(rc, device, field->ptcl_counter);
        Ruzino::brush_dispatch(
            rc,
            field->ptcl_compact_program,
            { { "ptcl_alive", field->ptcl_alive },
              { "ptcl_pos", field->ptcl_pos },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_color", field->ptcl_color } },
            { { "ptcl_counter", field->ptcl_counter },
              { "ptcl_pos_out", field->ptcl_pos_b },
              { "ptcl_vel_out", field->ptcl_vel_b },
              { "ptcl_color_out", field->ptcl_color_b },
              { "ptcl_alive_out", field->ptcl_alive_b } },
            maint_cb,
            max_ptcl);
        std::swap(field->ptcl_pos, field->ptcl_pos_b);
        std::swap(field->ptcl_vel, field->ptcl_vel_b);
        std::swap(field->ptcl_color, field->ptcl_color_b);
        std::swap(field->ptcl_alive, field->ptcl_alive_b);

        rc.destroy(maint_cb);
    }

    // 状态写回输出, 由仿真区在帧末搬入 simulation_in 供下一帧使用(反馈环)。
    params.set_output("State", zs);
    return true;
}

NODE_DECLARATION_UI(brush_wb_sim);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_sim);

NODE_DEF_CLOSE_SCOPE
