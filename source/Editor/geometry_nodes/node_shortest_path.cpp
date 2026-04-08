#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <list>
#include <queue>
#include <string>

#include "GCore/Components/MeshComponent.h"
#include "GCore/GOP.h"
#include "nodes/core/def/node_def.hpp"

typedef OpenMesh::TriMesh_ArrayKernelT<> MyMesh;

// Return true if the shortest path exists, and fill in the shortest path
// vertices and the distance. Otherwise, return false.
bool find_shortest_path(
    const MyMesh::VertexHandle& start_vertex_handle,
    const MyMesh::VertexHandle& end_vertex_handle,
    const MyMesh& omesh,
    std::list<size_t>& shortest_path_vertex_indices,
    float& distance)
{
    // TODO: Implement the shortest path algorithm
    // You need to fill in `shortest_path_vertex_indices` and `distance`
    typedef std::pair<float, MyMesh::VertexHandle> PQElement;
    std::priority_queue<
        PQElement,
        std::vector<PQElement>,
        std::greater<PQElement>>
        pq;

    // 存储到每个顶点的最短距离
    std::map<MyMesh::VertexHandle, float> dist;
    // 存储路径中的前驱顶点
    std::map<MyMesh::VertexHandle, MyMesh::VertexHandle> predecessor;

    // 初始化距离为无穷大
    for (MyMesh::VertexIter v_it = omesh.vertices_begin();
         v_it != omesh.vertices_end();
         ++v_it) {
        dist[*v_it] = std::numeric_limits<float>::infinity();
    }

    // 设置起点距离为0并加入优先队列
    dist[start_vertex_handle] = 0.0f;
    pq.push(std::make_pair(0.0f, start_vertex_handle));

    while (!pq.empty()) {
        MyMesh::VertexHandle current = pq.top().second;
        float current_dist = pq.top().first;
        pq.pop();

        // 如果找到终点，结束搜索
        if (current == end_vertex_handle) {
            break;
        }

        // 如果当前距离大于已知最短距离，跳过
        if (current_dist > dist[current]) {
            continue;
        }

        // 遍历当前顶点的所有邻接顶点
        for (MyMesh::VertexVertexIter vv_it = omesh.cvv_iter(current);
             vv_it.is_valid();
             ++vv_it) {
            MyMesh::VertexHandle neighbor = *vv_it;

            // 计算边的长度（顶点间的欧氏距离）
            OpenMesh::Vec3f p1 = omesh.point(current);
            OpenMesh::Vec3f p2 = omesh.point(neighbor);
            float edge_length = (p2 - p1).length();

            // 计算通过当前顶点到达邻接顶点的总距离
            float new_dist = dist[current] + edge_length;

            // 如果找到更短的路径，更新距离和前驱
            if (new_dist < dist[neighbor]) {
                dist[neighbor] = new_dist;
                predecessor[neighbor] = current;
                pq.push(std::make_pair(new_dist, neighbor));
            }
        }
    }

    // 如果终点的距离仍为无穷大，说明没有路径
    if (dist[end_vertex_handle] == std::numeric_limits<float>::infinity()) {
        return false;
    }

    // 构建最短路径
    shortest_path_vertex_indices.clear();
    MyMesh::VertexHandle current = end_vertex_handle;
    while (current != start_vertex_handle) {
        shortest_path_vertex_indices.push_front(current.idx());
        current = predecessor[current];
    }
    shortest_path_vertex_indices.push_front(start_vertex_handle.idx());

    // 设置最短路径的总距离
    distance = dist[end_vertex_handle];

    return true;
}

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(shortest_path)
{
    b.add_input<std::string>("Picked Mesh [0] Name");
    b.add_input<std::string>("Picked Mesh [1] Name");
    b.add_input<Geometry>("Picked Mesh");
    b.add_input<size_t>("Picked Vertex [0] Index");
    b.add_input<size_t>("Picked Vertex [1] Index");

    b.add_output<std::list<size_t>>("Shortest Path Vertex Indices");
    b.add_output<float>("Shortest Path Distance");
}

NODE_EXECUTION_FUNCTION(shortest_path)
{
    auto picked_mesh_0_name =
        params.get_input<std::string>("Picked Mesh [0] Name");
    auto picked_mesh_1_name =
        params.get_input<std::string>("Picked Mesh [1] Name");
    // Ensure that the two picked meshes are the same
    if (picked_mesh_0_name != picked_mesh_1_name) {
        std::cerr << "Ensure that the two picked meshes are the same"
                  << std::endl;
        return false;
    }

    auto mesh = params.get_input<Geometry>("Picked Mesh")
                    .get_component<MeshComponent>();
    auto vertices = mesh->get_vertices();
    auto face_vertex_counts = mesh->get_face_vertex_counts();
    auto face_vertex_indices = mesh->get_face_vertex_indices();

    // Convert the mesh to OpenMesh
    MyMesh omesh;

    // Add vertices
    std::vector<OpenMesh::VertexHandle> vhandles;
    vhandles.reserve(vertices.size());

    for (auto vertex : vertices) {
        omesh.add_vertex(OpenMesh::Vec3f(vertex[0], vertex[1], vertex[2]));
    }

    // Add faces
    size_t start = 0;
    for (int face_vertex_count : face_vertex_counts) {
        std::vector<OpenMesh::VertexHandle> face;
        face.reserve(face_vertex_count);
        for (int j = 0; j < face_vertex_count; j++) {
            face.push_back(
                OpenMesh::VertexHandle(face_vertex_indices[start + j]));
        }
        omesh.add_face(face);
        start += face_vertex_count;
    }

    auto start_vertex_index =
        params.get_input<size_t>("Picked Vertex [0] Index");
    auto end_vertex_index = params.get_input<size_t>("Picked Vertex [1] Index");

    // Turn the vertex indices into OpenMesh vertex handles
    OpenMesh::VertexHandle start_vertex_handle(start_vertex_index);
    OpenMesh::VertexHandle end_vertex_handle(end_vertex_index);

    // The indices of the vertices on the shortest path, including the start and
    // end vertices
    std::list<size_t> shortest_path_vertex_indices;

    // The distance of the shortest path
    float distance = 0.0f;

    if (find_shortest_path(
            start_vertex_handle,
            end_vertex_handle,
            omesh,
            shortest_path_vertex_indices,
            distance)) {
        params.set_output(
            "Shortest Path Vertex Indices", shortest_path_vertex_indices);
        params.set_output("Shortest Path Distance", distance);
        return true;
    }
    else {
        params.set_output("Shortest Path Vertex Indices", std::list<size_t>());
        params.set_output("Shortest Path Distance", 0.0f);
        return false;
    }

    return true;
}

NODE_DECLARATION_UI(shortest_path);
NODE_DECLARATION_REQUIRED(shortest_path);

NODE_DEF_CLOSE_SCOPE
