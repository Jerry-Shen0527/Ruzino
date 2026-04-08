#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>

#include <string>

#include "nodes/core/def/node_def.hpp"
#include "polyscope/curve_network.h"
#include "polyscope/point_cloud.h"
#include "polyscope/polyscope.h"
#include "polyscope/surface_mesh.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(get_polyscope_vertices)
{
    b.add_input<std::string>("Structure Name");
    b.add_output<pxr::VtVec3fArray>("Vertices");
}

NODE_EXECUTION_FUNCTION(get_polyscope_vertices)
{
    auto structure_name = params.get_input<std::string>("Structure Name");
    structure_name = std::string(structure_name.c_str());

    // If the input structure is a surface mesh
    if (polyscope::hasStructure("Surface Mesh", structure_name)) {
        auto structure =
            polyscope::getStructure("Surface Mesh", structure_name);
        auto mesh = dynamic_cast<polyscope::SurfaceMesh*>(structure);
        auto vertices = mesh->vertexPositions.data;
        pxr::VtVec3fArray v;
        for (auto vertex : vertices) {
            v.push_back(pxr::GfVec3f(vertex[0], vertex[1], vertex[2]));
        }
        params.set_output("Vertices", v);
    }
    // If the input structure is a point cloud
    else if (polyscope::hasStructure("Point Cloud", structure_name)) {
        auto structure = polyscope::getStructure("Point Cloud", structure_name);
        auto point_cloud = dynamic_cast<polyscope::PointCloud*>(structure);
        auto points = point_cloud->points.data;
        pxr::VtVec3fArray p;
        for (auto point : points) {
            p.push_back(pxr::GfVec3f(point[0], point[1], point[2]));
        }
        params.set_output("Vertices", p);
    }
    // If the input is a curve network
    else if (polyscope::hasStructure("Curve Network", structure_name)) {
        auto structure =
            polyscope::getStructure("Curve Network", structure_name);
        auto curve_network = dynamic_cast<polyscope::CurveNetwork*>(structure);
        auto nodes = curve_network->nodePositions.data;
        pxr::VtVec3fArray n;
        for (auto node : nodes) {
            n.push_back(pxr::GfVec3f(node[0], node[1], node[2]));
        }
        params.set_output("Vertices", n);
    }
    else {
        std::cerr << "The picked structure is not a surface mesh, point cloud, "
                     "or curve network."
                  << std::endl;
        return false;
    }

    return true;
}

NODE_DECLARATION_UI(get_polyscope_vertices);
NODE_DEF_CLOSE_SCOPE