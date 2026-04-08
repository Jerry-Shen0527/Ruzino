"""
Ruzino Render Graph API - A high-level Python interface for render node graphs.

This module provides a clean, Falcor-inspired API for constructing and executing
render graphs in Python, similar to how geometry graphs work.

Example:
    from render_graph import RenderGraph

    # Create graph
    g = RenderGraph("MyRenderPipeline")
    g.loadConfiguration("render_nodes.json")

    # Create nodes
    rng_tex = g.createNode("rng_texture", name="RNG")
    ray_gen = g.createNode("node_render_ray_generation", name="RayGen")
    path_trace = g.createNode("path_tracing", name="PathTrace")
    accumulate = g.createNode("accumulate", name="Accumulate")

    # Connect nodes
    g.addEdge(rng_tex, "Random Number", ray_gen, "random seeds")
    g.addEdge(ray_gen, "Rays", path_trace, "Rays")
    g.addEdge(path_trace, "Output", accumulate, "Texture")

    # Set parameters
    g.setInput(accumulate, "Max Samples", 16)

    # Mark output
    g.markOutput(accumulate, "Accumulated")

    # Execute
    g.prepare_and_execute()

    # Get result
    result_texture = g.getOutput(accumulate, "Accumulated")
"""

import os
import sys
from pathlib import Path
from typing import Any, Optional, Union, Dict, List

# Import base graph class
try:
    # Try to import from the Core module
    import sys
    core_python_path = Path(__file__).parent.parent.parent.parent / "Core" / "rznode" / "python"
    if core_python_path.exists():
        sys.path.insert(0, str(core_python_path))

    from ruzino_graph import RuzinoGraph
    HAS_BASE = True
except ImportError as e:
    HAS_BASE = False
    print(f"WARNING: Could not import RuzinoGraph base class: {e}")
    # Fallback: define a minimal base class
    class RuzinoGraph:
        pass

# Import required modules
try:
    import hd_RUZINO_py as renderer
    HAS_RENDERER = True
except ImportError as e:
    HAS_RENDERER = False
    print(f"WARNING: Renderer modules not available: {e}")


class RenderGraph(RuzinoGraph):
    """High-level interface for render node graph construction and execution.

    Extends RuzinoGraph with render-specific initialization and USD stage support.
    """

    def __init__(self, name: str = "RenderGraph"):
        """
        Create a new render graph.

        Args:
            name: Name of the graph (for debugging/display)
        """
        if not HAS_RENDERER:
            raise RuntimeError(
                "Renderer modules not available. "
                "Make sure hd_RUZINO_py is built."
            )

        super().__init__(name)

    def _create_system(self):
        """Override to create render-specific system."""
        return renderer.create_render_system()

    def initialize(self, config_path: Optional[str] = None, usd_stage_path: Optional[str] = None) -> 'RenderGraph':
        """
        Initialize the render graph system.

        Args:
            config_path: Optional path to render_nodes.json configuration
            usd_stage_path: Optional path to USD stage for scene setup

        Returns:
            self for chaining
        """
        # Create render-specific system if not already initialized
        if self._system is None:
            self._system = self._create_system()

        # Load configuration and initialize using parent class logic
        if config_path:
            loaded = self._system.load_configuration(config_path)
            if not loaded:
                raise RuntimeError(f"Failed to load configuration from {config_path}")

        if not self._initialized:
            self._system.init()
            self._tree = self._system.get_node_tree()
            self._executor = self._system.get_node_tree_executor()
            self._initialized = True

        # Set up USD stage if provided (render-specific)
        if usd_stage_path:
            try:
                import stage_py
                stage = stage_py.Stage(str(usd_stage_path))
                payload = stage_py.create_payload_from_stage(stage, "/geom")
                meta_payload = stage_py.create_meta_any_from_payload(payload)
                self._system.set_global_params(meta_payload)
                print(f"✓ USD stage loaded: {usd_stage_path}")
            except Exception as e:
                print(f"⚠ Warning: Could not load USD stage: {e}")

        return self

    def __repr__(self):
        if not self._initialized:
            return f"RenderGraph('{self.name}', uninitialized)"
        return f"RenderGraph('{self.name}', nodes={len(self.nodes)}, links={len(self.links)})"


# Example usage
if __name__ == "__main__":
    import sys

    print("Ruzino Render Graph API")
    print("="*70)

    # Find binary directory and add to path
    binary_dir = Path.cwd()
    if not (binary_dir / "render_nodes.json").exists():
        binary_dir = binary_dir / "Binaries" / "Release"

    # Add binary dir to Python path for module imports
    sys.path.insert(0, str(binary_dir))

    config_path = binary_dir / "render_nodes.json"

    if not config_path.exists():
        print(f"✗ Configuration file not found: {config_path}")
        exit(1)

    print(f"✓ Using configuration: {config_path}")
    print()

    # Create render graph
    g = RenderGraph("TestPipeline")
    g.loadConfiguration(str(config_path))
    print(f"✓ Created render graph: {g}")
    print()

    # Create some basic nodes
    try:
        rng_tex = g.createNode("rng_texture", name="RNGTexture")
        print(f"✓ Created node: {rng_tex.ui_name}")

        ray_gen = g.createNode("node_render_ray_generation", name="RayGen")
        print(f"✓ Created node: {ray_gen.ui_name}")

        # Connect them
        g.addEdge(rng_tex, "Random Number", ray_gen, "random seeds")
        print(f"✓ Connected: {rng_tex.ui_name} -> {ray_gen.ui_name}")

        print()
        print(f"Final graph: {g}")
        print(f"  Nodes: {[n.ui_name for n in g.nodes]}")
        print(f"  Links: {len(g.links)}")

    except Exception as e:
        print(f"✗ Error: {e}")
        import traceback
        traceback.print_exc()
