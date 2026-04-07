#include "GCore/Components/MeshComponent.h"
#include "geom_node_base.h"

NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(mesh_compose)
{
    b.add_input<std::vector<glm::vec3>>("Vertices");
    b.add_input<std::vector<int>>("FaceVertexCounts");
    b.add_input<std::vector<int>>("FaceVertexIndices");
    b.add_input<std::vector<glm::vec3>>("Normals");
    b.add_input<std::vector<glm::vec2>>("Texcoords");

    b.add_output<Geometry>("Mesh");
}

NODE_EXECUTION_FUNCTION(mesh_compose)
{
    Geometry geometry;
    auto mesh_component = std::make_shared<MeshComponent>(&geometry);

    auto vertices = params.get_input<std::vector<glm::vec3>>("Vertices");
    auto faceVertexCounts =
        params.get_input<std::vector<int>>("FaceVertexCounts");
    auto faceVertexIndices =
        params.get_input<std::vector<int>>("FaceVertexIndices");
    auto normals = params.get_input<std::vector<glm::vec3>>("Normals");
    auto texcoordsArray = params.get_input<std::vector<glm::vec2>>("Texcoords");

    if (vertices.size() > 0 && faceVertexCounts.size() > 0 &&
        faceVertexIndices.size() > 0) {
        mesh_component->set_vertices(vertices);
        mesh_component->set_face_vertex_counts(faceVertexCounts);
        mesh_component->set_face_vertex_indices(faceVertexIndices);
        mesh_component->set_normals(normals);
        mesh_component->set_texcoords_array(texcoordsArray);
        geometry.attach_component(mesh_component);
    }
    else {
        // TODO: Throw something
    }

    params.set_output("Mesh", geometry);
    return true;
}

NODE_DECLARATION_UI(mesh_compose);
NODE_DEF_CLOSE_SCOPE
