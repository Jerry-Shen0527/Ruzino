#!/usr/bin/env python3
"""
Simple example showing how to use the GUI Python wrapper
"""

import GUI_py

def main():
    print("Testing GUI_py module...")

    try:
        # Test basic Window creation
        print("Creating window...")
        window = GUI_py.Window()
        print(f"Window created successfully!")

        # Test elapsed time
        elapsed = window.get_elapsed_time()
        print(f"Elapsed time: {elapsed}")

        # Test ImVec2
        print("Creating ImVec2...")
        vec = GUI_py.ImVec2(100.0, 200.0)
        print(f"ImVec2 created: x={vec.x}, y={vec.y}")

        # Test ImColor
        print("Creating ImColor...")
        color_rgb = GUI_py.ImColor(1.0, 0.5, 0.0)
        color_rgba = GUI_py.ImColor(0.0, 1.0, 0.0, 0.8)
        print("ImColor objects created successfully!")

        print("All basic tests passed!")

        # Uncomment to actually run the GUI
        # print("Starting GUI...")
        # window.run()

    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()

if __name__ == "__main__":
    main()
