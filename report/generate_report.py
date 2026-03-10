from fpdf import FPDF
from datetime import datetime


class PDF(FPDF):
    def header(self):
        self.set_font("Helvetica", "B", 14)
        self.cell(0, 10, "Task Completion Report", 0, 1, "C")
        self.ln(5)

    def footer(self):
        self.set_y(-15)
        self.set_font("Helvetica", "I", 8)
        self.cell(0, 10, f"Page {self.page_no()}", 0, 0, "C")


pdf = PDF()
pdf.add_page()
pdf.set_auto_page_break(auto=True, margin=15)

# Title
pdf.set_font("Helvetica", "B", 16)
pdf.cell(0, 10, "CMake AddLibrary.cmake Deduplication", 0, 1, "C")
pdf.ln(5)

# Date
pdf.set_font("Helvetica", "", 10)
pdf.cell(0, 8, f"Date: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}", 0, 1, "C")
pdf.ln(10)

# Summary
pdf.set_font("Helvetica", "B", 12)
pdf.cell(0, 8, "Summary", 0, 1, "L")
pdf.set_font("Helvetica", "", 11)
summary = """Eliminated duplicate AddLibrary.cmake files between the main project and the rznode submodule while maintaining rznode's ability to build independently."""
pdf.multi_cell(0, 6, summary)
pdf.ln(5)

# Problem
pdf.set_font("Helvetica", "B", 12)
pdf.cell(0, 8, "Problem", 0, 1, "L")
pdf.set_font("Helvetica", "", 11)
problem = """- Two identical AddLibrary.cmake files existed:
  1. cmake/AddLibrary.cmake (main project)
  2. source/Core/rznode/cmake/AddLibrary.cmake (submodule)
- rznode is a git submodule that needs to remain independently buildable
- Duplicate code creates maintenance issues and potential drift"""
pdf.multi_cell(0, 6, problem)
pdf.ln(5)

# Solution
pdf.set_font("Helvetica", "B", 12)
pdf.cell(0, 8, "Solution", 0, 1, "L")
pdf.set_font("Helvetica", "", 11)
solution = """Implemented a thin wrapper pattern with configurable options:

1. Canonical Source (rznode/cmake/AddLibrary.cmake):
   - Contains all implementation logic
   - Added configurable cache variables:
     * RZNODE_CUDA_EXTRA_FLAGS: Extra CUDA compiler flags
     * RZNODE_LINK_PYTHON_TO_NANOBIND: Python linking behavior

2. Thin Wrapper (cmake/AddLibrary.cmake):
   - Only 12 lines (down from 332)
   - Sets Ruzino-specific configuration
   - Delegates to rznode's canonical version"""
pdf.multi_cell(0, 6, solution)
pdf.ln(5)

# Architecture
pdf.set_font("Helvetica", "B", 12)
pdf.cell(0, 8, "Architecture", 0, 1, "L")
pdf.set_font("Courier", "", 9)
arch = """Main Project (Ruzino):
  cmake/AddLibrary.cmake (wrapper)
    |-- Set: RZNODE_CUDA_EXTRA_FLAGS="--forward-unknown-to-host-compiler;-c"
    |-- Set: RZNODE_LINK_PYTHON_TO_NANOBIND=ON
    +-- include --> source/Core/rznode/cmake/AddLibrary.cmake

rznode Standalone:
  cmake/AddLibrary.cmake (canonical)
    |-- Uses default values for all options
    +-- Fully functional on its own"""
pdf.multi_cell(0, 5, arch)
pdf.ln(5)

# Files Changed
pdf.set_font("Helvetica", "B", 12)
pdf.cell(0, 8, "Files Changed", 0, 1, "L")
pdf.set_font("Helvetica", "", 11)
files = """1. source/Core/rznode/cmake/AddLibrary.cmake
   - Added configurable cache variables (RZNODE_CUDA_EXTRA_FLAGS, RZNODE_LINK_PYTHON_TO_NANOBIND)
   - Made CUDA extra flags configurable
   - Made Python linking behavior configurable

2. cmake/AddLibrary.cmake
   - Reduced from 332 lines to 12 lines
   - Now a thin wrapper that includes rznode's version
   - Sets Ruzino-specific configuration before inclusion"""
pdf.multi_cell(0, 6, files)
pdf.ln(5)

# Benefits
pdf.set_font("Helvetica", "B", 12)
pdf.cell(0, 8, "Benefits", 0, 1, "L")
pdf.set_font("Helvetica", "", 11)
benefits = """- No code duplication: Single source of truth in rznode
- rznode independent: Can be cloned and built standalone
- Backward compatible: Works in both source tree and installed scenarios
- Easy maintenance: Only edit rznode/cmake/AddLibrary.cmake
- Flexible: Projects can customize behavior via cache variables
- Reduced line count: 332 -> 12 lines in main wrapper"""
pdf.multi_cell(0, 6, benefits)
pdf.ln(5)

# Verification
pdf.set_font("Helvetica", "B", 12)
pdf.cell(0, 8, "Verification", 0, 1, "L")
pdf.set_font("Helvetica", "", 11)
verification = """- CMake configuration: PASSED
- Build target nodes_core: PASSED
- Both development and installation paths work correctly"""
pdf.multi_cell(0, 6, verification)

# Save
pdf.output("report_20260311_005740.pdf")
print("PDF report created successfully")
