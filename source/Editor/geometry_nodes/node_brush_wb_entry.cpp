// node_brush_wb_entry — packs the input Stroke Curves into a WetbrushFrame so
// the simulation zone boundary stays single-typed.
//
// The zone (createSimulationZone + add_sync_group) forces the upstream INPUT
// and the fed-back value to be the SAME type. mock_stroke outputs a Geometry
// (Stroke Curves), but the zone feedbacks a WetbrushFrame (carrying the brush
// state + canvas). This thin node bridges them: it takes the Stroke Curves and
// emits a fresh WetbrushFrame with stroke_curves set (bp inactive, state null
// -- deposit allocates state on the init frame).
//
// Topology:
//   mock_stroke --Stroke Curves--> brush_wb_entry --WetbrushFrame-->
//     [ simulation_in ] --WetbrushFrame--> (interior: emitter reads
//     frame.stroke_curves on first sight and caches it)

#include "GCore/GOP.h"
#include "brush_sim_common.hpp"  // WetbrushFrame
#include "geom_node_base.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_entry)
{
    b.add_input<Geometry>("Stroke Curves");
    b.add_output<Ruzino::WetbrushFrame>("Frame");
}

NODE_EXECUTION_FUNCTION(brush_wb_entry)
{
    Ruzino::WetbrushFrame frame;
    frame.stroke_curves = params.get_input<Geometry>("Stroke Curves");
    // bp stays default (inactive); state stays null -- deposit allocates it.
    params.set_output("Frame", frame);
    return true;
}

NODE_DECLARATION_UI(brush_wb_entry);

NODE_DEF_CLOSE_SCOPE
