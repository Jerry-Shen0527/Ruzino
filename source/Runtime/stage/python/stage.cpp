/**
 * @file stage.cpp
 * @brief Python bindings for Stage with USD interoperability support
 *
 * This module provides:
 * 1. nanobind bindings for Stage class
 * 2. USD interoperability between Boost.Python (pxr) and nanobind
 * 3. Bridge functions using BOTH Boost.Python and nanobind
 *
 * CRITICAL: This file uses BOTH binding systems to enable true interop!
 */

// CRITICAL: Undefine min/max macros BEFORE any includes
// Even with NOMINMAX, sometimes these macros leak through
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// IMPORTANT: Include standard library headers BEFORE boost/python.hpp
// to avoid macro conflicts (especially std::numeric_limits<>::max())
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <algorithm>
#include <cstddef>
#include <limits>

// Boost.Python headers for interop
// CRITICAL: USD uses renamed Boost (pxr_boost) not standard boost
// We need to use pxr namespace aliases
#define BOOST_PYTHON_NAMESPACE pxr_boost::python
#include <pxr/base/tf/pyUtils.h>
#include <pxr/base/tf/refPtr.h>
#include <pxr/base/tf/weakPtr.h>
#include <pxr/usd/usd/stage.h>

#include <entt/meta/factory.hpp>
#include <entt/meta/meta.hpp>
#include <pxr/external/boost/python.hpp>
#include <pxr/external/boost/python/extract.hpp>
#include <pxr/external/boost/python/object.hpp>

#include "GCore/geom_payload.hpp"
#include "nodes/core/api.hpp"
#include "stage/stage.hpp"

// Need animation.h for Stage implementation details
#include "stage/animation.h"

namespace nb = nanobind;
namespace bp = pxr::pxr_boost::python;  // USD uses renamed Boost
using namespace Ruzino;

/**
 * CRITICAL FUNCTION: Extract UsdStageRefPtr from pxr.Usd.Stage (Boost.Python
 * object)
 *
 * This is the MAGIC that makes interop work!
 * pxr.Usd.Stage is a Boost.Python wrapper around pxr::UsdStageRefPtr.
 * We need to extract the C++ pointer from the Boost.Python object.
 */
pxr::UsdStageRefPtr extract_stage_from_boost_python(PyObject* py_stage)
{
    try {
        // Create Boost.Python object wrapper
        bp::object stage_obj(bp::handle<>(bp::borrowed(py_stage)));

        // Try to extract as TfWeakPtr<UsdStage> first (what pxr actually uses)
        bp::extract<pxr::TfWeakPtr<pxr::UsdStage>> weak_extractor(stage_obj);
        if (weak_extractor.check()) {
            auto weak_ptr = weak_extractor();
            // Convert TfWeakPtr to UsdStageRefPtr
            return pxr::TfCreateRefPtrFromProtectedWeakPtr(weak_ptr);
        }

        // Alternative: Try direct UsdStageRefPtr extraction
        bp::extract<pxr::UsdStageRefPtr> ref_extractor(stage_obj);
        if (ref_extractor.check()) {
            return ref_extractor();
        }

        throw std::runtime_error(
            "Could not extract UsdStageRefPtr from Python object");
    }
    catch (const std::exception& e) {
        throw std::runtime_error(
            std::string(
                "Failed to extract USD stage from Boost.Python object: ") +
            e.what());
    }
}

/**
 * CRITICAL FUNCTION: Convert UsdStageRefPtr to pxr.Usd.Stage (Boost.Python
 * object)
 *
 * This creates a Boost.Python wrapper that can be used with pxr module.
 */
PyObject* wrap_stage_with_boost_python(const pxr::UsdStageRefPtr& stage)
{
    try {
        // Use pxr's own conversion from C++ to Python
        // This creates a proper pxr.Usd.Stage object
        return pxr::TfPyObject(stage).ptr();
    }
    catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("Failed to wrap USD stage with Boost.Python: ") +
            e.what());
    }
}

NB_MODULE(stage_py, m)
{
    m.doc() =
        "Stage module with TRUE USD interoperability (Boost.Python + nanobind)";
    nb::class_<GeomPayload>(m, "GeomPayload")
        .def(nb::init<>())
        // Don't bind USD types directly - they cause cross-binding issues
        .def_rw("pick", &GeomPayload::pick, "Pick event data")
        .def_rw(
            "delta_time", &GeomPayload::delta_time, "Delta time for simulation")
        .def_rw(
            "has_simulation",
            &GeomPayload::has_simulation,
            "Whether simulation is available")
        .def_rw(
            "is_simulating",
            &GeomPayload::is_simulating,
            "Whether currently simulating")
        .def_rw(
            "stage_filepath_", &GeomPayload::stage_filepath_, "Stage file path")
        .def_rw(
            "is_modifier_mode",
            &GeomPayload::is_modifier_mode,
            "Whether in modifier mode")
        .def_rw(
            "current_modifier_index",
            &GeomPayload::current_modifier_index,
            "Current modifier index in stack");

    // Helper functions for setting USD types on GeomPayload
    m.def(
        "set_payload_stage_and_prim",
        [](GeomPayload& payload,
           nb::handle py_stage,
           const std::string& prim_path) {
            pxr::UsdStageRefPtr cpp_stage =
                extract_stage_from_boost_python(py_stage.ptr());
            if (!cpp_stage) {
                throw std::runtime_error("Extracted stage is null");
            }
            payload.stage = cpp_stage;
            payload.prim_path = pxr::SdfPath(prim_path);
            payload.current_time = pxr::UsdTimeCode(0);
        },
        nb::arg("payload"),
        nb::arg("stage"),
        nb::arg("prim_path"),
        "Set stage and prim_path on GeomPayload from pxr.Usd.Stage");

    m.def(
        "set_payload_modifier_paths",
        [](GeomPayload& payload,
           const std::string& output_path,
           const std::string& input_path) {
            payload.modifier_output_path = pxr::SdfPath(output_path);
            payload.modifier_input_path = pxr::SdfPath(input_path);
        },
        nb::arg("payload"),
        nb::arg("output_path"),
        nb::arg("input_path"),
        "Set modifier input/output paths on GeomPayload");

    // Stage class binding
    nb::class_<Stage>(m, "Stage")
        .def(nb::init<>(), "Create an empty stage")
        .def(
            nb::init<const std::string&>(),
            nb::arg("stage_path"),
            "Create stage from USD file path")
        .def(
            "get_usd_stage",
            &Stage::get_usd_stage,
            "Get the underlying UsdStageRefPtr")
        .def(
            "save",
            [](Stage& stage) {
                auto usd_stage = stage.get_usd_stage();
                if (usd_stage) {
                    usd_stage->GetRootLayer()->Save();
                    stage.save_modifier_layer();
                    return true;
                }
                return false;
            },
            "Save the stage and modifier layer to file")
        .def(
            "open_stage",
            [](Stage& stage, const std::string& path) {
                return stage.OpenStage(path);
            },
            nb::arg("path"),
            "Open a USD stage and load modifier layer")
        .def(
            "export_to_string",
            [](const Stage& stage) { return stage.stage_content(); },
            "Export stage as USD text")
        .def(
            "get_pxr_stage",
            [](Stage& stage) -> nb::object {
                auto usd_stage = stage.get_usd_stage();
                if (!usd_stage) {
                    throw std::runtime_error("Stage has no USD stage");
                }
                // Convert C++ UsdStageRefPtr to pxr.Usd.Stage (Boost.Python)
                PyObject* pxr_stage = wrap_stage_with_boost_python(usd_stage);
                // Wrap in nanobind object
                return nb::borrow<nb::object>(pxr_stage);
            },
            "Get pxr.Usd.Stage (Boost.Python) object - TRUE INTEROP!");

    // USD Stage interoperability functions

    /**
     * THE KEY FUNCTION: Convert pxr.Usd.Stage to GeomPayload
     * This uses Boost.Python to extract the C++ pointer!
     */
    m.def(
        "create_payload_from_pxr_stage",
        [](nb::handle py_stage, const std::string& prim_path = "/geom") {
            // Extract C++ UsdStageRefPtr from pxr.Usd.Stage (Boost.Python
            // object)
            pxr::UsdStageRefPtr cpp_stage =
                extract_stage_from_boost_python(py_stage.ptr());

            if (!cpp_stage) {
                throw std::runtime_error("Extracted stage is null");
            }

            // Create GeomPayload
            GeomPayload payload;
            payload.stage = cpp_stage;
            payload.prim_path = pxr::SdfPath(prim_path);
            payload.current_time = pxr::UsdTimeCode(0);
            payload.delta_time = 0.0f;
            payload.has_simulation = false;
            payload.is_simulating = false;

            return payload;
        },
        nb::arg("pxr_stage"),
        nb::arg("prim_path") = "/geom",
        "Create GeomPayload from pxr.Usd.Stage - TRUE INTEROP! Works with "
        "Boost.Python!");

    /**
     * Convert GeomPayload to pxr.Usd.Stage
     */
    m.def(
        "get_pxr_stage_from_payload",
        [](GeomPayload& payload) -> nb::object {
            if (!payload.stage) {
                throw std::runtime_error("GeomPayload has no stage");
            }
            // Convert C++ UsdStageRefPtr to pxr.Usd.Stage (Boost.Python)
            PyObject* pxr_stage = wrap_stage_with_boost_python(payload.stage);
            // Wrap in nanobind object
            return nb::steal<nb::object>(pxr_stage);
        },
        nb::arg("payload"),
        "Get pxr.Usd.Stage from GeomPayload - TRUE INTEROP!");

    /**
     * Create a GeomPayload from an existing Stage object.
     */
    m.def(
        "create_payload_from_stage",
        [](Stage& stage, const std::string& prim_path = "/geom") {
            GeomPayload payload;
            payload.stage = stage.get_usd_stage();
            payload.prim_path = pxr::SdfPath(prim_path);
            payload.current_time = pxr::UsdTimeCode(0);
            payload.delta_time = 0.0f;
            payload.has_simulation = false;
            payload.is_simulating = false;
            return payload;
        },
        nb::arg("stage"),
        nb::arg("prim_path") = "/geom",
        "Create GeomPayload from Stage object for use with node graph");

    /**
     * Create Stage wrapper from pxr.Usd.Stage
     */
    m.def(
        "create_stage_from_pxr",
        [](nb::handle py_stage) -> Stage* {
            // Extract C++ UsdStageRefPtr
            pxr::UsdStageRefPtr cpp_stage =
                extract_stage_from_boost_python(py_stage.ptr());

            if (!cpp_stage) {
                throw std::runtime_error("Extracted stage is null");
            }

            // Create a new Stage wrapper
            // NOTE: Stage class needs a constructor that accepts UsdStageRefPtr
            // For now, we return nullptr and document the limitation
            throw std::runtime_error(
                "Creating Stage from pxr stage requires Stage class to have "
                "a constructor accepting UsdStageRefPtr. Use "
                "create_payload_from_pxr_stage instead.");
        },
        nb::arg("pxr_stage"),
        "Create Stage from pxr.Usd.Stage (requires Stage class extension)");

    /**
     * CRITICAL: Register GeomPayload type in entt meta system
     * This allows it to be used with NodeSystem::set_global_params_any
     */
    m.def(
        "register_geom_payload_type",
        []() { Ruzino::register_cpp_type<GeomPayload>(); },
        "Register GeomPayload type in entt meta system for node graph usage");

    /**
     * Create meta_any from GeomPayload for use with
     *
     * NodeSystem::set_global_params This is the bridge function that allows

     * * type-erased parameter passing
     */
    m.def(
        "create_meta_any_from_payload",
        [](const GeomPayload& payload) -> entt::meta_any {
            // Ensure type is registered with the correct context
            Ruzino::register_cpp_type<GeomPayload>();

            // Create meta_any with the GeomPayload using the same context
            // CRITICAL: Must use the same entt context as the node system!
            auto& ctx = Ruzino::get_entt_ctx();
            auto type = entt::resolve<GeomPayload>(ctx);

            if (!type) {
                throw std::runtime_error(
                    "GeomPayload type not registered in entt meta system");
            }

            // Create meta_any with explicit context
            return entt::meta_any{ ctx, payload };
        },
        nb::arg("payload"),
        "Create entt::meta_any from GeomPayload for use with "
        "NodeSystem::set_global_params. "
        "This automatically registers the type if not already registered.");

    /**
     * Directly set GeomPayload as global params on a NodeSystem
     *
     * This is a convenience function that handles the meta_any conversion
     * internally
     */
    m.def(
        "set_global_payload",
        [](NodeSystem& system, const GeomPayload& payload) {
            Ruzino::register_cpp_type<GeomPayload>();
            system.set_global_params(payload);
        },
        nb::arg("system"),
        nb::arg("payload"),
        "Set GeomPayload as global parameters on a NodeSystem");
}
