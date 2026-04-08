# GUI Python Wrapper

This directory contains Python bindings for the C++ GUI Window and Widget classes using nanobind.

## Files

- `gui.cpp` - C++ nanobind module that creates Python bindings
- `test_gui.py` - Comprehensive test file showing all features
- `example_simple.py` - Simple usage example
- `README.md` - This file

## Building

To build the Python module using CMake:

```bash
# Make sure nanobind is installed
pip install nanobind

# Build with CMake
mkdir build
cd build
cmake ..
make  # or ninja, or Visual Studio build
```

Make sure to link against the GUI library and set the correct include paths in CMakeLists.txt.

## Usage

### Basic Window Creation

```python
import gui

# Create a window
window = gui.Window()

# Start the rendering loop
window.run()
```

### Creating Custom Widgets

```python
class MyWidget(gui.IWidget):
    def __init__(self, title="My Widget"):
        super().__init__(title)

    def BuildUI(self):
        # Your ImGui code here
        return True

# Register with window
window = gui.Window()
widget = MyWidget()
window.register_widget(widget)
```

### Drawing Widgets

```python
class DrawingWidget(gui.IWidgetDrawable):
    def BuildUI(self):
        # Draw shapes
        center = gui.ImVec2(100, 100)
        color = gui.ImColor(1.0, 0.0, 0.0)  # Red
        self.DrawCircle(center, 50, 3, color)
        return True
```

### Widget Factories

```python
class MyWidgetFactory(gui.IWidgetFactory):
    def Create(self, others):
        return MyWidget("Factory Created")

# Register with menu
factory = MyWidgetFactory()
window.register_openable_widget(factory, ["File", "New", "My Widget"])
```

### Frame Callbacks

```python
def before_frame(window):
    print(f"Frame starting, elapsed: {window.get_elapsed_time()}")

def after_frame(window):
    print("Frame ended")

window.register_function_before_frame(before_frame)
window.register_function_after_frame(after_frame)
```

## Classes

### Window
- `Window()` - Constructor
- `run()` - Start main loop
- `get_elapsed_time()` - Get elapsed time in seconds
- `register_widget(widget)` - Add a widget
- `register_openable_widget(factory, menu_path)` - Add widget to menu
- `get_widget(name)` - Get widget by name
- `get_widgets()` - Get all widgets
- `register_function_before_frame(callback)` - Before frame callback
- `register_function_after_frame(callback)` - After frame callback

### IWidget
- `IWidget(title)` - Constructor
- `BuildUI()` - Override this method to build UI
- `Width()`, `Height()` - Get dimensions
- `SetCallBack(callback)` - Set widget callback

### IWidgetDrawable (extends IWidget)
- `DrawCircle(center, radius, thickness, color, segments)`
- `DrawLine(p1, p2, thickness, color)`
- `DrawRect(p1, p2, thickness, color)`
- `DrawArc(center, radius, a_min, a_max, thickness, color, segments)`

### Helper Classes
- `ImVec2(x, y)` - 2D vector for positions
- `ImColor(r, g, b)` or `ImColor(r, g, b, a)` - Color values (0.0-1.0)

## Running Tests

```bash
python example_simple.py
```

Note: Make sure the compiled `gui` module is in your Python path before running the tests.

## Notes

- All color values are in the range 0.0-1.0
- Positions are in pixels
- The `BuildUI()` method should return `True` to indicate successful rendering
- Widget callbacks receive `(Window, IWidget)` parameters
- Frame callbacks receive `(Window)` parameter