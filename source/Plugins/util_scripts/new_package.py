import os
import sys
import argparse
from pathlib import Path
import subprocess


def create_directory_structure(
    package_name, base_path, create_nodes, create_renderer_nodes, create_geometry_nodes
):
    """创建包的目录结构"""
    package_path = base_path / package_name

    directories = [
        package_path / "include" / package_name,
        package_path / "src",
    ]

    if create_nodes:
        directories.append(package_path / "nodes")

    if create_renderer_nodes:
        directories.append(package_path / "renderer_nodes")

    if create_geometry_nodes:
        directories.append(package_path / "geometry_nodes")

    for directory in directories:
        directory.mkdir(parents=True, exist_ok=True)
        print(f"Created directory: {directory}")

    return package_path


def generate_api_header(package_name, package_path):
    """生成api.h文件"""
    api_header_script = Path(__file__).parent / "api_header.py"
    include_dir = package_path / "include" / package_name

    try:
        subprocess.run(
            [
                sys.executable,
                str(api_header_script),
                package_name,
                str(include_dir),
                "--output_file",
                "api.h",
            ],
            check=True,
        )
        print(f"Generated api.h at: {include_dir / 'api.h'}")
    except subprocess.CalledProcessError as e:
        print(f"Error generating api.h: {e}")


def generate_main_header(package_name, package_path):
    """生成主头文件"""
    upper_name = package_name.upper()
    header_content = f"""#pragma once

#include "api.h"

RUZINO_NAMESPACE_OPEN_SCOPE

// Add your API functions here

RUZINO_NAMESPACE_CLOSE_SCOPE
"""

    header_file = package_path / "include" / package_name / f"{package_name}.h"
    with open(header_file, "w", encoding="utf-8") as f:
        f.write(header_content)

    print(f"Generated main header at: {header_file}")


def generate_main_cmake(
    package_name, package_path, public_libs, create_nodes, create_renderer_nodes, create_geometry_nodes
):
    """生成主CMakeLists.txt文件"""
    libs_str = " ".join(public_libs) if public_libs else ""

    # 始终跳过 nodes, renderer_nodes 和 geometry_nodes 目录，防止主库扫描这些目录中的 cpp
    skip_dirs = []
    if create_nodes:
        skip_dirs.append("nodes")
    if create_renderer_nodes:
        skip_dirs.append("renderer_nodes")
    if create_geometry_nodes:
        skip_dirs.append("geometry_nodes")

    skip_str = " ".join(skip_dirs) if skip_dirs else ""

    cmake_content = f"""RUZINO_ADD_LIB(
\t{package_name}
\tSHARED
"""

    if libs_str:
        cmake_content += f"\tPUBLIC_LIBS {libs_str}\n"

    if skip_str:
        cmake_content += f"\tSKIP_DIRS {skip_str}\n"

    cmake_content += ")\n"

    # 添加子目录
    if create_nodes:
        cmake_content += "\nadd_subdirectory(nodes)\n"
    if create_renderer_nodes:
        cmake_content += "add_subdirectory(renderer_nodes)\n"
    if create_geometry_nodes:
        cmake_content += "add_subdirectory(geometry_nodes)\n"

    cmake_file = package_path / "CMakeLists.txt"
    with open(cmake_file, "w", encoding="utf-8") as f:
        f.write(cmake_content)

    print(f"Generated main CMakeLists.txt at: {cmake_file}")


def generate_nodes_cmake(package_name, package_path, dep_libs, json_dir):
    """生成nodes/CMakeLists.txt文件"""
    libs_str = " ".join(dep_libs) if dep_libs else package_name

    cmake_content = f"""add_nodes(
\tTARGET_NAME {package_name}_nodes
\tJSON_DIR {json_dir}
\tDEP_LIBS {libs_str}
)
"""

    cmake_file = package_path / "nodes" / "CMakeLists.txt"
    with open(cmake_file, "w", encoding="utf-8") as f:
        f.write(cmake_content)

    print(f"Generated nodes CMakeLists.txt at: {cmake_file}")


def generate_renderer_nodes_cmake(package_name, package_path, dep_libs, json_dir):
    """生成renderer_nodes/CMakeLists.txt文件"""
    libs_str = " ".join(dep_libs) if dep_libs else package_name

    cmake_content = f"""add_nodes(
\tTARGET_NAME {package_name}_renderer_nodes
\tJSON_DIR {json_dir}
\tDEP_LIBS {libs_str}
\tRENDERER_NODE
)
"""

    cmake_file = package_path / "renderer_nodes" / "CMakeLists.txt"
    with open(cmake_file, "w", encoding="utf-8") as f:
        f.write(cmake_content)

    print(f"Generated renderer_nodes CMakeLists.txt at: {cmake_file}")


def generate_geometry_nodes_cmake(package_name, package_path, dep_libs, json_dir):
    """生成geometry_nodes/CMakeLists.txt文件"""
    libs_str = " ".join(dep_libs) if dep_libs else package_name

    cmake_content = f"""add_nodes(
\tTARGET_NAME {package_name}_geometry_nodes
\tJSON_DIR {json_dir}
\tDEP_LIBS {libs_str}
)
"""

    cmake_file = package_path / "geometry_nodes" / "CMakeLists.txt"
    with open(cmake_file, "w", encoding="utf-8") as f:
        f.write(cmake_content)

    print(f"Generated geometry_nodes CMakeLists.txt at: {cmake_file}")


def main():
    parser = argparse.ArgumentParser(
        description="Create a new package with directory structure and configuration files."
    )
    parser.add_argument("package_name", help="The name of the package to create")
    parser.add_argument(
        "--base-path",
        default=None,
        help="Base path where the package will be created (default: current directory)",
    )
    parser.add_argument(
        "--public-libs",
        nargs="*",
        default=[],
        help="Public libraries to link (e.g., GUI RHI glm::glm)",
    )
    parser.add_argument(
        "--dep-libs",
        nargs="*",
        default=[],
        help="Dependency libraries for nodes (default: package_name)",
    )
    parser.add_argument(
        "--json-dir",
        default="Plugins",
        help="JSON directory for nodes (default: Plugins)",
    )
    parser.add_argument(
        "--no-nodes",
        action="store_true",
        help="Don't create nodes subdirectory and CMakeLists.txt",
    )
    parser.add_argument(
        "--no-renderer-nodes",
        action="store_true",
        help="Don't create renderer_nodes subdirectory and CMakeLists.txt",
    )
    parser.add_argument(
        "--no-geometry-nodes",
        action="store_true",
        help="Don't create geometry_nodes subdirectory and CMakeLists.txt",
    )

    args = parser.parse_args()

    # 如果没有指定base_path，使用当前目录
    if args.base_path is None:
        base_path = Path.cwd()
    else:
        base_path = Path(args.base_path)

    package_name = args.package_name

    # 确定是否创建 nodes, renderer_nodes 和 geometry_nodes
    create_nodes = not args.no_nodes
    create_renderer_nodes = not args.no_renderer_nodes
    create_geometry_nodes = not args.no_geometry_nodes

    print(f"Creating package: {package_name}")
    print(f"Base path: {base_path}")

    # 创建目录结构
    package_path = create_directory_structure(
        package_name, base_path, create_nodes, create_renderer_nodes, create_geometry_nodes
    )

    # 生成api.h
    generate_api_header(package_name, package_path)

    # 生成主头文件
    generate_main_header(package_name, package_path)

    # 生成主CMakeLists.txt
    generate_main_cmake(
        package_name,
        package_path,
        args.public_libs,
        create_nodes,
        create_renderer_nodes,
        create_geometry_nodes,
    )

    # 生成nodes CMakeLists.txt
    if create_nodes:
        generate_nodes_cmake(package_name, package_path, args.dep_libs, args.json_dir)

    # 生成renderer_nodes CMakeLists.txt
    if create_renderer_nodes:
        generate_renderer_nodes_cmake(
            package_name, package_path, args.dep_libs, args.json_dir
        )

    # 生成geometry_nodes CMakeLists.txt
    if create_geometry_nodes:
        generate_geometry_nodes_cmake(
            package_name, package_path, args.dep_libs, args.json_dir
        )

    print(f"\n✓ Package '{package_name}' created successfully at: {package_path}")
    print(f"\nNext steps:")
    print(f"1. Add your source files to: {package_path / 'src'}")
    print(
        f"2. Implement your API in: {package_path / 'include' / package_name / f'{package_name}.h'}"
    )
    if create_nodes:
        print(f"3. Add node implementations to: {package_path / 'nodes'}")
    if create_renderer_nodes:
        print(
            f"4. Add renderer node implementations to: {package_path / 'renderer_nodes'}"
        )
    if create_geometry_nodes:
        print(
            f"5. Add geometry node implementations to: {package_path / 'geometry_nodes'}"
        )


if __name__ == "__main__":
    main()
