// Replay stroke generator — loads a captured stroke from disk (the JSON
// produced by brush_paint_sim's recorder when RUZINO_RECORD_STROKE is set)
// and emits a Curve identical to what brush_capture/brush_input produced in
// the editor. Used by tests/test_brush_sim_replay.py to reproduce the
// editor's exact input headlessly.
//
// The "Frame Points" input slices the captured stroke to its first N points
// so a test can re-cook the graph N times with N = 1, 2, ..., total —
// replicating the editor's frame-by-frame incremental brush movement, where
// brush_capture adds one point per frame and brush_paint_sim deposits only
// at the new (last) vertex each frame.

#include "GCore/Components/CurveComponent.h"
#include "GCore/GOP.h"
#include "geom_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

NODE_DEF_OPEN_SCOPE

// ---- Minimal JSON value parser (enough for our own recorder output) ----
// The recorder writes a flat object of arrays/scalars with no nesting
// beyond the array elements, so a tiny ad-hoc parser is sufficient and
// avoids pulling a JSON dependency into the test node.
namespace {

struct ReplayData {
    std::vector<glm::vec3> points;
    std::vector<float> timestamps;
    std::vector<int> stroke_lengths;
    glm::vec3 ink_color = glm::vec3(1.0f, 0.0f, 0.0f);
    float brush_width = 0.02f;
};

// Skip whitespace.
static const char* skip_ws(const char* p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

// Parse a JSON number starting at p; advance p past it.
static double parse_number(const char*& p)
{
    p = skip_ws(p);
    std::string tok;
    while (*p && *p != ',' && *p != ']' && *p != '}' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        tok.push_back(*p++);
    }
    return std::stod(tok);
}

// Parse a JSON array of numbers into a vector<float>.
static std::vector<float> parse_float_array(const char*& p)
{
    p = skip_ws(p);
    std::vector<float> out;
    if (*p != '[') return out;
    ++p;  // consume '['
    p = skip_ws(p);
    if (*p == ']') { ++p; return out; }
    while (*p) {
        out.push_back(static_cast<float>(parse_number(p)));
        p = skip_ws(p);
        if (*p == ',') { ++p; continue; }
        if (*p == ']') { ++p; break; }
        p = skip_ws(p);
    }
    return out;
}

// Parse a JSON array of [x,y,z] arrays.
static std::vector<glm::vec3> parse_vec3_array(const char*& p)
{
    p = skip_ws(p);
    std::vector<glm::vec3> out;
    if (*p != '[') return out;
    ++p;  // outer '['
    p = skip_ws(p);
    if (*p == ']') { ++p; return out; }
    while (*p) {
        p = skip_ws(p);
        if (*p != '[') break;
        ++p;  // inner '['
        glm::vec3 v;
        v.x = static_cast<float>(parse_number(p));
        p = skip_ws(p);
        if (*p == ',') ++p;
        v.y = static_cast<float>(parse_number(p));
        p = skip_ws(p);
        if (*p == ',') ++p;
        v.z = static_cast<float>(parse_number(p));
        p = skip_ws(p);
        if (*p == ']') ++p;  // inner ']'
        out.push_back(v);
        p = skip_ws(p);
        if (*p == ',') { ++p; continue; }
        if (*p == ']') { ++p; break; }  // outer ']'
        p = skip_ws(p);
    }
    return out;
}

static bool load_replay_file(const std::string& path, ReplayData& out)
{
    std::ifstream f(path);
    if (!f) {
        spdlog::warn("replay_stroke: could not open '{}'", path);
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    const char* p = text.c_str();

    // Walk top-level "key": value pairs.
    if (*skip_ws(p) != '{') return false;
    ++p;
    while (*p) {
        p = skip_ws(p);
        if (*p == '}') break;
        if (*p != '"') { if (*p) ++p; continue; }
        ++p;
        std::string key;
        while (*p && *p != '"') key.push_back(*p++);
        if (*p == '"') ++p;
        p = skip_ws(p);
        if (*p == ':') ++p;
        p = skip_ws(p);

        if (key == "points") {
            out.points = parse_vec3_array(p);
        } else if (key == "timestamps") {
            out.timestamps = parse_float_array(p);
        } else if (key == "stroke_lengths") {
            auto tmp = parse_float_array(p);
            out.stroke_lengths.assign(tmp.begin(), tmp.end());
        } else if (key == "ink_r_ryb") {
            out.ink_color.x = static_cast<float>(parse_number(p));
        } else if (key == "ink_y_ryb") {
            out.ink_color.y = static_cast<float>(parse_number(p));
        } else if (key == "ink_b_ryb") {
            out.ink_color.z = static_cast<float>(parse_number(p));
        } else if (key == "brush_width") {
            out.brush_width = static_cast<float>(parse_number(p));
        } else {
            // Skip arbitrary value: scalar, array, or object.
            if (*p == '[') {
                int depth = 0;
                while (*p) {
                    if (*p == '[') ++depth;
                    else if (*p == ']') { --depth; if (depth == 0) { ++p; break; } }
                    ++p;
                }
            } else if (*p == '{') {
                int depth = 0;
                while (*p) {
                    if (*p == '{') ++depth;
                    else if (*p == '}') { --depth; if (depth == 0) { ++p; break; } }
                    ++p;
                }
            } else {
                while (*p && *p != ',' && *p != '}') ++p;
            }
        }
        p = skip_ws(p);
        if (*p == ',') ++p;
    }
    return !out.points.empty();
}

}  // namespace

NODE_DECLARATION_FUNCTION(replay_stroke)
{
    b.add_input<std::string>("File Path").default_val("");
    b.add_input<int>("Frame Points").default_val(-1).min(-1).max(1000000);
    b.add_output<Geometry>("Stroke Curves");
}

NODE_EXECUTION_FUNCTION(replay_stroke)
{
    auto path = params.get_input<std::string>("File Path");
    int frame_points = params.get_input<int>("Frame Points");

    ReplayData data;
    if (!load_replay_file(path, data)) {
        // Emit an empty curve.
        auto geometry = Geometry::CreateCurve();
        params.set_output("Stroke Curves", std::move(geometry));
        return true;
    }

    int total = static_cast<int>(data.points.size());
    int n = (frame_points < 0 || frame_points > total) ? total : frame_points;
    if (n < 0) n = 0;

    auto geometry = Geometry::CreateCurve();
    auto curve = geometry.get_component<CurveComponent>();
    if (n == 0) {
        params.set_output("Stroke Curves", std::move(geometry));
        return true;
    }

    std::vector<glm::vec3> verts(data.points.begin(), data.points.begin() + n);
    std::vector<glm::vec3> colors(n, data.ink_color);
    std::vector<float> widths(n, data.brush_width);
    std::vector<float> ts;
    ts.reserve(n);
    for (int i = 0; i < n; ++i) {
        ts.push_back(i < static_cast<int>(data.timestamps.size())
                         ? data.timestamps[i]
                         : static_cast<float>(i) / 60.0f);
    }

    // Preserve the multi-stroke segmentation from the capture so that
    // brush_paint_sim can detect stroke boundaries (pen-down/up) and avoid
    // drawing a ghost line between the tail of one stroke and the head of the
    // next. Walk the recorded stroke_lengths and clip each primitive at `n`
    // total points: fully-included primitives keep their count, the partially
    // included tail primitive gets the remainder.
    std::vector<int> vert_counts;
    int remaining = n;
    for (int sl : data.stroke_lengths) {
        if (remaining <= 0) break;
        int take = (sl <= remaining) ? sl : remaining;
        vert_counts.push_back(take);
        remaining -= take;
    }
    if (vert_counts.empty()) {
        vert_counts.push_back(n);  // fallback: single curve if no segmentation
    }

    curve->set_vertices(verts);
    curve->set_vert_count(vert_counts);
    curve->set_display_color(colors);
    curve->set_width(widths);
    curve->add_vertex_scalar_quantity("timestamp", ts);

    params.set_output("Stroke Curves", std::move(geometry));
    return true;
}

NODE_DECLARATION_UI(replay_stroke);

NODE_DEF_CLOSE_SCOPE
