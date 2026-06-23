#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>

#include "GCore/Components/CurveComponent.h"
#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "geom_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "nodes/core/io/json.hpp"
#include "spdlog/spdlog.h"


// ---------------------------------------------------------------------------
// Persistence workaround
// ---------------------------------------------------------------------------
// The "official" storage-persistence path (Node::storage_info serialized into
// the node-graph JSON) is currently broken on the *write* side: Node::serialize
// (source/Core/rznode/core/node.cpp) never emits a "storage_info" field, so on
// reload node_tree.cpp reads back an empty string, and get_storage() therefore
// never calls BrushCaptureStorage::deserialize(). In other words, whatever
// set_storage() writes into Node::storage_info is lost across restarts.
//
// Rather than patch the core, this node side steps that path entirely:
//   * serialize()   dumps the trajectory straight to a fixed binary file. It is
//                   still invoked by set_storage() (every cook), so writes
//                   happen automatically.
//   * deserialize() reads that binary file back. Because the core will NOT call
//                   it after a restart, brush_capture's exec function triggers
//                   it manually on the first cook (guarded by
//                   loaded_from_disk).
//
// The helpers below must live OUTSIDE NODE_DEF_OPEN_SCOPE, because that macro
// opens an extern "C" block and C linkage is incompatible with templates.
// ---------------------------------------------------------------------------

namespace {
// File sits next to the process working directory. Fixed name is intentional
// (requested): one brush_capture node per session is the current usage.
constexpr const char* kCacheFilePath = "brush_capture_cache.bin";

// 'BCAP1.0' magic, just to reject obviously-corrupt / foreign files.
constexpr uint64_t kCacheMagic = 0x42434150312E30ull;  // "BCAP1.0"

template<typename T>
void write_pod(std::ofstream& ofs, const T& v)
{
    ofs.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template<typename T>
void write_vector(std::ofstream& ofs, const std::vector<T>& v)
{
    const uint64_t n = v.size();
    write_pod(ofs, n);
    if (n > 0) {
        ofs.write(
            reinterpret_cast<const char*>(v.data()),
            static_cast<std::streamsize>(n * sizeof(T)));
    }
}

template<typename T>
bool read_vector(std::ifstream& ifs, std::vector<T>& v)
{
    uint64_t n = 0;
    ifs.read(reinterpret_cast<char*>(&n), sizeof(n));
    if (!ifs)
        return false;
    v.resize(static_cast<size_t>(n));
    if (n > 0) {
        ifs.read(
            reinterpret_cast<char*>(v.data()),
            static_cast<std::streamsize>(n * sizeof(T)));
    }
    return static_cast<bool>(ifs);
}
}  // namespace

NODE_DEF_OPEN_SCOPE

struct BrushCaptureStorage {
    std::vector<glm::vec3> points;
    std::vector<float> timestamps;
    std::vector<int> stroke_lengths;  // vert_count per stroke
    bool in_stroke = false;

    // True once we have (attempted to) reload the trajectory from disk this
    // session. Guards the manual deserialize() call in the exec function.
    bool loaded_from_disk = false;

    // has_storage stays true so that set_storage() keeps invoking serialize()
    // (our write-to-disk hook) on every cook. We do NOT rely on the core's
    // storage_info round-trip; serialize() returns "" on purpose.
    static constexpr bool has_storage = true;

    // --- disk helpers -------------------------------------------------------
    void save_to_disk() const
    {
        std::ofstream ofs(kCacheFilePath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            spdlog::warn(
                "brush_capture: cannot open cache file '{}' for writing",
                kCacheFilePath);
            return;
        }
        constexpr uint64_t kVersion = 1;
        write_pod(ofs, kCacheMagic);
        write_pod(ofs, kVersion);
        write_vector(ofs, points);
        write_vector(ofs, timestamps);
        write_vector(ofs, stroke_lengths);
        const uint8_t in_stroke_byte = in_stroke ? 1 : 0;
        write_pod(ofs, in_stroke_byte);
    }

    void load_from_disk()
    {
        std::ifstream ifs(kCacheFilePath, std::ios::binary);
        if (!ifs)
            return;  // no cache yet — fresh session, nothing to restore

        uint64_t magic = 0, version = 0;
        ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (magic != kCacheMagic) {
            spdlog::warn(
                "brush_capture: cache file '{}' magic mismatch, ignoring",
                kCacheFilePath);
            return;
        }

        std::vector<glm::vec3> pts;
        std::vector<float> ts;
        std::vector<int> sl;
        if (!read_vector(ifs, pts) || !read_vector(ifs, ts) ||
            !read_vector(ifs, sl)) {
            spdlog::warn(
                "brush_capture: cache file '{}' truncated, ignoring",
                kCacheFilePath);
            return;
        }
        uint8_t in_stroke_byte = 0;
        ifs.read(reinterpret_cast<char*>(&in_stroke_byte), 1);

        points = std::move(pts);
        timestamps = std::move(ts);
        stroke_lengths = std::move(sl);
        // Always restart with the pen up: a stroke that was mid-air when the
        // process died should not keep being appended to.
        in_stroke = false;
    }

    // --- core-facing hooks --------------------------------------------------
    std::string serialize() const
    {
        // Persist directly to disk; return "" so Node::storage_info stays empty
        // (we intentionally bypass the broken JSON round-trip).
        save_to_disk();
        return "";
    }

    void deserialize(const std::string& /*str*/)
    {
        // str is expected to be empty (see serialize()). Ignore it and read the
        // binary file instead. NOTE: the core does not call this after a
        // restart, so brush_capture's exec function calls it manually.
        load_from_disk();
    }
};

NODE_DECLARATION_FUNCTION(brush_capture)
{
    b.add_output<Geometry>("Stroke Curves");
}

NODE_EXECUTION_FUNCTION(brush_capture)
{
    auto& storage = params.get_storage<BrushCaptureStorage&>();

    // Core never invokes BrushCaptureStorage::deserialize() after a restart
    // (Node::serialize omits storage_info), so do the disk read ourselves on
    // the first cook of the session.
    if (!storage.loaded_from_disk) {
        storage.loaded_from_disk = true;
        storage.deserialize({});
    }

    auto payload = params.get_global_payload<GeomPayload>();

    if (payload.brush_new_point) {
        if (payload.brush_active) {
            storage.points.push_back(payload.brush_point);
            storage.timestamps.push_back(payload.brush_time);
            if (!storage.in_stroke) {
                storage.in_stroke = true;
                storage.stroke_lengths.push_back(0);
            }
            storage.stroke_lengths.back()++;
        }
        else if (storage.in_stroke) {
            // Pen up — finalize current stroke
            storage.in_stroke = false;
        }
    }

    // Build output curve with all accumulated strokes
    auto geometry = Geometry::CreateCurve();
    auto curve = geometry.get_component<CurveComponent>();

    if (!storage.points.empty()) {
        curve->set_vertices(storage.points);
        curve->set_vert_count(storage.stroke_lengths);
        curve->set_width(std::vector<float>(storage.points.size(), 0.01f));
        curve->add_vertex_scalar_quantity("timestamp", storage.timestamps);
        curve->set_display_color(
            std::vector<glm::vec3>(
                storage.points.size(), glm::vec3(0.1f, 0.1f, 0.9f)));
    }

    params.set_output("Stroke Curves", geometry);
    // set_storage() -> serialize() -> save_to_disk() every cook, so the cache
    // file tracks the live trajectory.
    params.set_storage(storage);
    return true;
}

NODE_DECLARATION_UI(brush_capture);
NODE_DECLARATION_ALWAYS_DIRTY(brush_capture);
// brush_capture must always re-cook: its real input is the live mouse
// payload (GeomPayload::brush_*), which is not a graph socket and so does
// not propagate dirty state through the executor. Without ALWAYS_DIRTY the
// node cooks once (empty), caches that empty result, and never picks up
// subsequent mouse points — the symptom of "brush capture not working".

NODE_DEF_CLOSE_SCOPE
