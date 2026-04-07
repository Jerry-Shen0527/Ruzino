#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

#include "GCore/Components/MeshComponent.h"
#include "GCore/GOP.h"
#include "nodes/core/def/node_def.hpp"

using namespace Ruzino;

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(set_scalar_as_height)
{
    b.add_input<Geometry>("Geometry");
    b.add_input<std::string>("Scalar Name").default_val("result");
    b.add_input<float>("Scale").default_val(1.0f);
    b.add_input<bool>("Auto Range").default_val(false);
    b.add_input<float>("Min Value").default_val(0.0f);
    b.add_input<float>("Max Value").default_val(1.0f);
    b.add_input<bool>("Center at Zero").default_val(false);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(set_scalar_as_height)
{
    // 获取输入参数
    Geometry input_geometry = params.get_input<Geometry>("Geometry");
    std::string scalar_name = params.get_input<std::string>("Scalar Name");
    float scale = params.get_input<float>("Scale");
    bool auto_range = params.get_input<bool>("Auto Range");
    float min_value = params.get_input<float>("Min Value");
    float max_value = params.get_input<float>("Max Value");
    bool center_at_zero = params.get_input<bool>("Center at Zero");

    // 获取网格组件
    auto mesh_component = input_geometry.get_component<MeshComponent>();
    if (!mesh_component) {
        return false;
    }

    // 获取标量数据
    std::vector<float> scalar_data =
        mesh_component->get_vertex_scalar_quantity(scalar_name);
    if (scalar_data.empty()) {
        return false;
    }

    // 获取顶点数据
    std::vector<glm::vec3> vertices = mesh_component->get_vertices();
    if (vertices.size() != scalar_data.size()) {
        return false;  // 顶点数量与标量数据数量不匹配
    }

    // 计算数据范围
    if (auto_range) {
        auto minmax =
            std::minmax_element(scalar_data.begin(), scalar_data.end());
        min_value = *minmax.first;
        max_value = *minmax.second;
    }

    // 避免除零
    if (std::abs(max_value - min_value) < 1e-8f) {
        max_value = min_value + 1.0f;
    }

    // 更新顶点的z坐标
    for (size_t i = 0; i < vertices.size(); ++i) {
        float value = scalar_data[i];

        // 归一化或缩放处理
        if (center_at_zero) {
            // 将数据居中到零点
            float mid = (min_value + max_value) * 0.5f;
            vertices[i].z = (value - mid) * scale;
        }
        else if (auto_range) {
            // 归一化到[0,1]然后缩放
            float normalized = (value - min_value) / (max_value - min_value);
            vertices[i].z = normalized * scale;
        }
        else {
            // 直接使用原始值缩放
            vertices[i].z = value * scale;
        }
    }

    // 更新网格顶点
    mesh_component->set_vertices(vertices);

    // 输出结果
    params.set_output("Geometry", input_geometry);
    return true;
}

NODE_DECLARATION_UI(set_scalar_as_height);

NODE_DEF_CLOSE_SCOPE