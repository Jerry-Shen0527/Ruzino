// #define __GNUC__
#include "GCore/create_geom.h"
#include "geom_node_base.h"

NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(create_grid)
{
    b.add_input<int>("resolution").min(1).max(20).default_val(2);
    b.add_input<float>("size").min(1).max(20).default_val(1.0f);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_grid)
{
    int resolution = params.get_input<int>("resolution");
    float size = params.get_input<float>("size");
    Geometry geometry = create_grid(resolution, size);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_circle)
{
    b.add_input<int>("resolution").min(1).max(100).default_val(10);
    b.add_input<float>("radius").min(1).max(20).default_val(1.0f);
    b.add_output<Geometry>("Circle");
}

NODE_EXECUTION_FUNCTION(create_circle)
{
    int resolution = params.get_input<int>("resolution");
    float radius = params.get_input<float>("radius");
    Geometry geometry = create_circle(resolution, radius);
    params.set_output("Circle", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_circle_face)
{
    b.add_input<int>("resolution").min(1).max(100).default_val(10);
    b.add_input<float>("radius").min(1).max(20).default_val(1.0f);
    b.add_output<Geometry>("Circle Face");
}

NODE_EXECUTION_FUNCTION(create_circle_face)
{
    int resolution = params.get_input<int>("resolution");
    float radius = params.get_input<float>("radius");
    Geometry geometry = create_circle_face(resolution, radius);
    params.set_output("Circle Face", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_cylinder_section)
{
    b.add_input<float>("height").min(0.1f).max(20.0f).default_val(1.0f);
    b.add_input<float>("radius").min(0.1f).max(20.0f).default_val(1.0f);
    b.add_input<float>("angle").min(0.1f).max(6.28f).default_val(
        1.57f);  // In radians, up to 2π
    b.add_input<int>("resolution").min(2).max(100).default_val(16);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_cylinder_section)
{
    float height = params.get_input<float>("height");
    float radius = params.get_input<float>("radius");
    float angle = params.get_input<float>("angle");
    int resolution = params.get_input<int>("resolution");

    Geometry geometry =
        create_cylinder_section(height, radius, angle, resolution);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_spiral)
{
    b.add_input<int>("resolution").min(1).max(100).default_val(10);
    b.add_input<float>("R1").min(0.1f).max(10.0f).default_val(1.0f);
    b.add_input<float>("R2").min(0.1f).max(10.0f).default_val(1.0f);
    b.add_input<float>("Circle Count").min(0.1f).max(10.0f).default_val(2.0f);
    b.add_input<float>("Height").min(0.1f).max(10.0f).default_val(1.0f);
    b.add_output<Geometry>("Curve");
}

NODE_EXECUTION_FUNCTION(create_spiral)
{
    int resolution = params.get_input<int>("resolution");
    float R1 = params.get_input<float>("R1");
    float R2 = params.get_input<float>("R2");
    float circleCount = params.get_input<float>("Circle Count");
    float height = params.get_input<float>("Height");

    Geometry geometry = create_spiral(resolution, R1, R2, circleCount, height);
    params.set_output("Curve", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_uv_sphere)
{
    b.add_input<int>("segments").min(3).max(64).default_val(32);
    b.add_input<int>("rings").min(2).max(64).default_val(16);
    b.add_input<float>("radius").min(0.1).max(20).default_val(1.0f);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_uv_sphere)
{
    int segments = params.get_input<int>("segments");
    int rings = params.get_input<int>("rings");
    float radius = params.get_input<float>("radius");

    Geometry geometry = create_uv_sphere(segments, rings, radius);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_ico_sphere)
{
    b.add_input<int>("subdivisions").min(0).max(5).default_val(2);
    b.add_input<float>("radius").min(0.1).max(20).default_val(1.0f);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_ico_sphere)
{
    int subdivisions = params.get_input<int>("subdivisions");
    float radius = params.get_input<float>("radius");

    Geometry geometry = create_ico_sphere(subdivisions, radius);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_point)
{
    b.add_input<float>("X").default_val(0.0f).min(-10.f).max(10.f);
    b.add_input<float>("Y").default_val(0.0f).min(-10.f).max(10.f);
    b.add_input<float>("Z").default_val(0.0f).min(-10.f).max(10.f);
    b.add_input<float>("Size").min(0.1f).max(10.0f).default_val(1.0f);
    b.add_output<Geometry>("Point");
}

NODE_EXECUTION_FUNCTION(create_point)
{
    float x = params.get_input<float>("X");
    float y = params.get_input<float>("Y");
    float z = params.get_input<float>("Z");
    float size = params.get_input<float>("Size");

    Geometry geometry = create_point(x, y, z, size);
    params.set_output("Point", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_wave_mesh)
{
    b.add_input<int>("resolution").min(2).max(100).default_val(16);
    b.add_input<float>("size").min(0.1).max(20).default_val(1.0f);
    b.add_input<float>("period_count").min(0.1).max(10).default_val(2.0f);
    b.add_input<float>("wave_height").min(0.1).max(5).default_val(0.5f);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_wave_mesh)
{
    int resolution = params.get_input<int>("resolution");
    float size = params.get_input<float>("size");
    float period_count = params.get_input<float>("period_count");
    float wave_height = params.get_input<float>("wave_height");

    Geometry geometry =
        create_wave_mesh(resolution, size, period_count, wave_height);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_diamond)
{
    b.add_input<float>("height").min(0.1).max(20).default_val(2.0f);
    b.add_input<float>("section height").min(0.1).max(20).default_val(0.8f);
    b.add_input<float>("top width").min(0.1).max(20).default_val(1.0f);
    b.add_input<float>("section width").min(0.1).max(20).default_val(1.2f);
    b.add_input<int>("segments").min(3).max(32).default_val(8);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_diamond)
{
    float height = params.get_input<float>("height");
    float sectionHeight = params.get_input<float>("section height");
    float topWidth = params.get_input<float>("top width");
    float sectionWidth = params.get_input<float>("section width");
    int segments = params.get_input<int>("segments");

    Geometry geometry =
        create_diamond(height, sectionHeight, topWidth, sectionWidth, segments);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_trefoil)
{
    b.add_input<int>("resolution").min(10).max(200).default_val(100);
    b.add_input<float>("radius").min(0.1).max(10).default_val(1.0f);
    b.add_input<float>("tube_radius").min(0.1).max(5).default_val(0.2f);
    b.add_output<Geometry>("Curve");
}
NODE_EXECUTION_FUNCTION(create_trefoil)
{
    int resolution = params.get_input<int>("resolution");
    float radius = params.get_input<float>("radius");
    float tubeRadius = params.get_input<float>("tube_radius");

    Geometry geometry = create_trefoil(resolution, radius, tubeRadius);
    params.set_output("Curve", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_cube)
{
    b.add_input<float>("width").min(0.1).max(20).default_val(2.0f);
    b.add_input<float>("height").min(0.1).max(20).default_val(2.0f);
    b.add_input<float>("depth").min(0.1).max(20).default_val(2.0f);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_cube)
{
    float width = params.get_input<float>("width");
    float height = params.get_input<float>("height");
    float depth = params.get_input<float>("depth");

    Geometry geometry = create_cube(width, height, depth);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_cube2)
{
    b.add_input<float>("min x").min(-20).max(20).default_val(-1.0f);
    b.add_input<float>("min y").min(-20).max(20).default_val(-1.0f);
    b.add_input<float>("min z").min(-20).max(20).default_val(-1.0f);
    b.add_input<float>("max x").min(-20).max(20).default_val(1.0f);
    b.add_input<float>("max y").min(-20).max(20).default_val(1.0f);
    b.add_input<float>("max z").min(-20).max(20).default_val(1.0f);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_cube2)
{
    float minx = params.get_input<float>("min x");
    float miny = params.get_input<float>("min y");
    float minz = params.get_input<float>("min z");
    float maxx = params.get_input<float>("max x");
    float maxy = params.get_input<float>("max y");
    float maxz = params.get_input<float>("max z");

    Geometry geometry = create_cube2(minx, miny, minz, maxx, maxy, maxz);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_box_grid)
{
    b.add_input<int>("resolution_x").min(1).max(50).default_val(2);
    b.add_input<int>("resolution_y").min(1).max(50).default_val(2);
    b.add_input<int>("resolution_z").min(1).max(50).default_val(2);
    b.add_input<float>("width").min(0.1).max(20).default_val(2.0f);
    b.add_input<float>("height").min(0.1).max(20).default_val(2.0f);
    b.add_input<float>("depth").min(0.1).max(20).default_val(2.0f);
    b.add_input<bool>("add_diagonal").default_val(false);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_box_grid)
{
    int resolution_x = params.get_input<int>("resolution_x");
    int resolution_y = params.get_input<int>("resolution_y");
    int resolution_z = params.get_input<int>("resolution_z");
    float width = params.get_input<float>("width");
    float height = params.get_input<float>("height");
    float depth = params.get_input<float>("depth");
    bool add_diagonal = params.get_input<bool>("add_diagonal");

    Geometry geometry = create_box_grid(
        resolution_x,
        resolution_y,
        resolution_z,
        width,
        height,
        depth,
        add_diagonal);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_subdivided_tetrahedron)
{
    b.add_input<int>("subdivisions").min(0).max(5).default_val(2);
    b.add_input<float>("size").min(0.1).max(20).default_val(1.0f);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_subdivided_tetrahedron)
{
    int subdivisions = params.get_input<int>("subdivisions");
    float size = params.get_input<float>("size");

    Geometry geometry = create_subdivided_tetrahedron(subdivisions, size);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_double_tetrahedron)
{
    b.add_input<float>("size").min(0.1).max(20).default_val(1.0f);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_double_tetrahedron)
{
    float size = params.get_input<float>("size");

    Geometry geometry = create_double_tetrahedron(size);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_FUNCTION(create_star_box)
{
    b.add_input<float>("width").min(0.1).max(20).default_val(2.0f);
    b.add_input<float>("height").min(0.1).max(20).default_val(2.0f);
    b.add_input<float>("depth").min(0.1).max(20).default_val(1.0f);
    b.add_input<float>("triangle_size").min(0.0).max(1.0).default_val(0.5f);
    b.add_input<float>("star_factor").min(0.0).max(1.0).default_val(0.0f);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(create_star_box)
{
    float width = params.get_input<float>("width");
    float height = params.get_input<float>("height");
    float depth = params.get_input<float>("depth");
    float triangle_size = params.get_input<float>("triangle_size");
    float star_factor = params.get_input<float>("star_factor");

    Geometry geometry =
        create_varying_star(star_factor, triangle_size, width, height, depth);
    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DEF_CLOSE_SCOPE
