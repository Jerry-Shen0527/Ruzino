// node_brush_wb_init_state — seeds the simulation zone with an empty paint
// field on the init frame.
//
// The Wetbrush zone carries TWO boundary slots: the static stroke Geometry
// (mock_stroke -> sim_in) and the WetbrushZoneState paint field (fed back
// commit -> sim_out -> sim_in). On the init frame there is no feedback yet,
// so sim_in's [State] slot would be empty and the whole zone would be skipped
// ("missing required input [State]"). This tiny source node provides that
// initial empty field (state == nullptr), which brush_wb_deposit then
// allocates on first sight.
//
// Topology:
//   brush_wb_init_state --State--> [ simulation_in ]   (init seed)
//   mock_stroke --Stroke Curves--> [ simulation_in ]
//
// On advance frames sim_in replays simulation_out's stored [State] (the
// committed canvas), so this node's output is only consumed on the init frame.

#include "GCore/GOP.h"
#include "brush_sim_common.hpp"  // WetbrushZoneState
#include "geom_node_base.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_init_state)
{
    b.add_output<Ruzino::WetbrushZoneState>("State");
}

NODE_EXECUTION_FUNCTION(brush_wb_init_state)
{
    // An empty field: null state pointer. brush_wb_deposit allocates it.
    Ruzino::WetbrushZoneState zs;
    params.set_output("State", zs);
    return true;
}

NODE_DECLARATION_UI(brush_wb_init_state);

NODE_DEF_CLOSE_SCOPE
