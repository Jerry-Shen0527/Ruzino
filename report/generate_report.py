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

timestamp = datetime.now()
timestamp_file = timestamp.strftime("%Y%m%d_%H%M%S")

# Title
pdf.set_font("Helvetica", "B", 16)
pdf.cell(0, 10, "CMake AddLibrary.cmake Cleanup", 0, 1, "C")
pdf.ln(5)

# Date
pdf.set_font("Helvetica", "", 10)
pdf.cell(0, 8, f"Date: {timestamp.strftime('%Y-%m-%d %H:%M:%S')}", 0, 1, "C")
pdf.ln(10)

# Summary
pdf.set_font("Helvetica", "B", 12)
pdf.cell(0, 8, "Summary", 0, 1, "L")
pdf.set_font("Helvetica", "", 11)
summary = """Simplified the AddLibrary.cmake architecture by moving configuration flags to CMakeLists.txt and making cmake/AddLibrary.cmake a pure thin wrapper (7 lines)."""
pdf.multi_cell(0, 6, summary)
pdf.ln(5)

# Changes
pdf.set_font("Helvetica", "B", 12)
pdf.cell(0, 8, "Changes Made", 0, 1, "L")
pdf.set_font("Helvetica", "", 11)
changes = """1. cmake/AddLibrary.cmake: Reduced from 12 lines to 7 lines
   - Removed flag definitions (now pure wrapper)
   - Only includes source/Core/rznode/cmake/AddLibrary.cmake

2. CMakeLists.txt: Added flag definitions before include
   - RZNODE_CUDA_EXTRA_FLAGS: "--forward-unknown-to-host-compiler;-c"
   - RZNODE_LINK_PYTHON_TO_NANOBIND: ON

3. source/Core/rznode/cmake/AddLibrary.cmake: Unchanged (canonical source)"""
pdf.multi_cell(0, 6, changes)
pdf.ln(5)

# Architecture
pdf.set_font("Helvetica", "B", 12)
pdf.cell(0, 8, "Architecture", 0, 1, "L")
pdf.set_font("Courier", "", 9)
arch = """Main Project (Ruzino):
  CMakeLists.txt (sets flags)
       |
       v
  cmake/AddLibrary.cmake (pure wrapper, 7 lines)
       |
       v
  source/Core/rznode/cmake/AddLibrary.cmake (canonical)

rznode Standalone:
  CMakeLists.txt -> cmake/AddLibrary.cmake (uses defaults)"""
pdf.multi_cell(0, 5, arch)
pdf.ln(5)

# Benefits
pdf.set_font("Helvetica", "B", 12)
pdf.cell(0, 8, "Benefits", 0, 1, "L")
pdf.set_font("Helvetica", "", 11)
benefits = """- Clean separation: Flags at project level, wrapper is pure delegation
- Single source of truth: All logic in rznode/cmake/AddLibrary.cmake
- rznode independent: Can still build standalone with defaults
- Minimal wrapper: 7 lines instead of 12"""
pdf.multi_cell(0, 6, benefits)

# Save
pdf.output(f"report_{timestamp_file}.pdf")
print(f"PDF report created: report_{timestamp_file}.pdf")
