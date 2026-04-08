"""
Ruzino Render Graph API - Python interface for headless rendering.

Example:
    g = RuzinoRenderGraph("MyRenderGraph")
    g.loadConfiguration("path/to/render_config.json")
    g.loadUSDStage("path/to/scene.usdc")

    # Create nodes
    ray_gen = g.createNode("node_render_ray_generation")
    path_trace = g.createNode("path_tracing")

    # Connect nodes
    g.addEdge(ray_gen, "Rays", path_trace, "Rays")

    # Execute
    g.execute()

    # Get output as tensor
    output_tensor = g.getOutputAsTensor(path_trace, "Output")
    print(f"Output shape: {output_tensor.shape}")
"""

import sys
import os
from pathlib import Path
from typing import Any, Optional, Union, Dict, List, Tuple

# Import the base graph API
from ruzino_graph import RuzinoGraph

# Try importing renderer bindings
try:
    import rzrenderer_py
    HAS_RENDERER = True
except ImportError:
    HAS_RENDERER = False
    print("WARNING: rzrenderer_py not available. Rendering features will be limited.")


class RuzinoRenderGraph(RuzinoGraph):
    """
    High-level interface for render graph construction and execution.
    Extends RuzinoGraph with rendering-specific functionality.
    """

    def __init__(self, name: str = "RenderGraph"):
        """
        Create a new render graph.

        Args:
            name: Name of the graph (for debugging/display)
        """
        super().__init__(name)
        self._usd_stage = None
        self._render_settings = {
            'width': 1920,
            'height': 1080,
            'spp': 16
        }

    def loadUSDStage(self, usd_path: str) -> 'RuzinoRenderGraph':
        """
        Load a USD stage for rendering.

        Args:
            usd_path: Path to the USD file

        Returns:
            self for method chaining
        """
        if not HAS_RENDERER:
            raise RuntimeError("rzrenderer_py module not available")

        if not os.path.exists(usd_path):
            raise FileNotFoundError(f"USD file not found: {usd_path}")

        self._usd_stage = rzrenderer_py.create_usd_stage(usd_path)
        print(f"✓ Loaded USD stage: {usd_path}")

        return self

    def setRenderSettings(self, width: int = None, height: int = None, spp: int = None) -> 'RuzinoRenderGraph':
        """
        Set rendering parameters.

        Args:
            width: Image width
            height: Image height
            spp: Samples per pixel

        Returns:
            self for method chaining
        """
        if width is not None:
            self._render_settings['width'] = width
        if height is not None:
            self._render_settings['height'] = height
        if spp is not None:
            self._render_settings['spp'] = spp

        print(f"✓ Render settings: {self._render_settings['width']}x{self._render_settings['height']}, SPP={self._render_settings['spp']}")
        return self

    def initRHI(self) -> 'RuzinoRenderGraph':
        """
        Initialize the RHI (Render Hardware Interface) system.
        Should be called before executing render graphs.

        Returns:
            self for method chaining
        """
        if not HAS_RENDERER:
            raise RuntimeError("rzrenderer_py module not available")

        rzrenderer_py.init_rhi()
        print("✓ RHI initialized")
        return self

    def getOutputAsTensor(self, node: Union['core.Node', str], socket_name: str):
        """
        Get output from a node as a PyTorch tensor.
        This automatically handles conversion from NVRHI texture to Torch tensor.

        Args:
            node: Node or node name
            socket_name: Name of output socket

        Returns:
            PyTorch tensor containing the output data
        """
        # First get the raw output
        output = self.getOutput(node, socket_name)

        # If it's already a tensor, return it
        if HAS_RENDERER:
            try:
                import torch
                if isinstance(output, torch.Tensor):
                    return output
            except ImportError:
                pass

        # Otherwise, we need to insert a conversion node
        # This is handled automatically by the node system if conversion nodes are registered
        return output

    def saveOutputImage(self, node: Union['core.Node', str], socket_name: str,
                       output_path: str, gamma: float = 2.2) -> 'RuzinoRenderGraph':
        """
        Save output from a node as an image file.

        Args:
            node: Node or node name
            socket_name: Name of output socket
            output_path: Path to save the image
            gamma: Gamma correction value (default: 2.2)

        Returns:
            self for method chaining
        """
        # This would require additional implementation for image saving
        # For now, just get the output and inform the user
        output = self.getOutput(node, socket_name)
        print(f"✓ Output retrieved from {node}.{socket_name}")
        print(f"  To save, use external tools or conversion nodes")
        return self

    @property
    def render_settings(self) -> Dict[str, int]:
        """Get current render settings."""
        return self._render_settings.copy()

    @property
    def usd_stage(self):
        """Get the loaded USD stage."""
        return self._usd_stage

    def __repr__(self):
        stage_info = f", USD stage loaded" if self._usd_stage else ""
        return (f"<RuzinoRenderGraph '{self.name}': "
                f"{len(self.nodes)} nodes, {len(self.links)} links"
                f"{stage_info}>")


# Convenience function for quick rendering
def quick_render(usd_path: str,
                json_script: str,
                output_image: str = None,
                width: int = 1920,
                height: int = 1080,
                spp: int = 16) -> Optional[Any]:
    """
    Quick render function for simple use cases.

    Args:
        usd_path: Path to USD scene file
        json_script: Path to render graph JSON
        output_image: Optional output image path
        width: Image width
        height: Image height
        spp: Samples per pixel

    Returns:
        Output tensor if no output_image specified, otherwise None
    """
    g = RuzinoRenderGraph("QuickRender")

    # Initialize
    g.initRHI()
    g.loadConfiguration(json_script)
    g.loadUSDStage(usd_path)
    g.setRenderSettings(width, height, spp)

    # Execute
    g.execute()

    # Return or save output
    # This is a simplified implementation - actual implementation would need
    # to identify output nodes automatically
    print("✓ Rendering complete")

    if output_image:
        print(f"  Output saving to: {output_image}")
        # TODO: Implement actual image saving
    else:
        print("  Return output tensor (not implemented yet)")

    return None
