#!/usr/bin/env python3
"""
未使用头文件分析脚本
使用 clang-tidy 或 include-what-you-use 分析 C++ 项目中未使用的头文件
"""

import json
import subprocess
import sys
import os
from pathlib import Path
from typing import List, Dict, Set, Tuple
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import re


@dataclass
class UnusedInclude:
    """未使用的头文件信息"""
    file_path: str
    line_number: int
    include_path: str
    reason: str = ""


class UnusedHeaderAnalyzer:
    """未使用头文件分析器"""

    def __init__(self, compile_commands_path: str, source_root: str = None):
        """
        初始化分析器

        Args:
            compile_commands_path: compile_commands.json 文件路径
            source_root: 源代码根目录，用于过滤外部依赖
        """
        self.compile_commands_path = Path(compile_commands_path)
        self.source_root = Path(source_root) if source_root else self.compile_commands_path.parent.parent
        self.compile_commands = self._load_compile_commands()
        self.unused_includes: List[UnusedInclude] = []

    def _load_compile_commands(self) -> List[Dict]:
        """加载编译数据库"""
        try:
            with open(self.compile_commands_path, 'r', encoding='utf-8') as f:
                return json.load(f)
        except Exception as e:
            print(f"错误：无法加载编译数据库 {self.compile_commands_path}: {e}")
            sys.exit(1)

    def _is_project_file(self, file_path: str) -> bool:
        """判断是否是项目文件（排除外部依赖）"""
        try:
            file_path = Path(file_path).resolve()

            # 排除外部依赖目录（需要在路径中检查）
            excluded_patterns = [
                'external', 'build', '_deps', 'SDK',
                'googletest', 'third_party', 'thirdparty'
            ]

            # 转换为小写进行比较
            path_str = str(file_path).lower().replace('\\', '/')

            for pattern in excluded_patterns:
                if f'/{pattern}/' in path_str or f'\\{pattern}\\' in path_str:
                    return False

            # 只包含 source/ 目录下的文件
            if '/source/' in path_str or '\\source\\' in path_str:
                # 但要排除 build/source
                if '/build/' not in path_str and '\\build\\' not in path_str:
                    return True

            return False

        except Exception as e:
            return False

    def _check_tool_available(self, tool_name: str) -> bool:
        """检查工具是否可用"""
        try:
            result = subprocess.run(
                [tool_name, '--version'],
                capture_output=True,
                timeout=5
            )
            return result.returncode == 0
        except (subprocess.TimeoutExpired, FileNotFoundError):
            return False

    def _run_include_what_you_use(self, compile_entry: Dict) -> List[UnusedInclude]:
        """使用 include-what-you-use 分析单个文件"""
        file_path = compile_entry['file']
        directory = compile_entry['directory']
        command = compile_entry.get('command', '')

        # 构建 IWYU 命令
        # IWYU 使用类似 clang 的命令行参数
        iwyu_cmd = ['include-what-you-use']

        # 解析编译命令，提取编译选项
        # 简化处理：直接从 compile_commands 获取参数
        if 'arguments' in compile_entry:
            iwyu_cmd.extend(compile_entry['arguments'][1:])  # 跳过编译器名称
        else:
            # 从 command 字符串解析（更复杂，这里简化处理）
            iwyu_cmd.append(file_path)

        try:
            result = subprocess.run(
                iwyu_cmd,
                cwd=directory,
                capture_output=True,
                text=True,
                timeout=30
            )

            # IWYU 输出到 stderr
            output = result.stderr
            return self._parse_iwyu_output(file_path, output)

        except subprocess.TimeoutExpired:
            print(f"警告: 分析超时 - {file_path}")
            return []
        except Exception as e:
            print(f"警告: 分析失败 - {file_path}: {e}")
            return []

    def _run_clang_tidy(self, compile_entry: Dict) -> List[UnusedInclude]:
        """使用 clang-tidy 分析单个文件"""
        file_path = compile_entry['file']
        file_name = Path(file_path).name

        # 实时输出正在分析的文件
        print(f"  → 正在分析: {file_name}...", flush=True)

        # clang-tidy 命令
        clang_tidy_cmd = [
            'clang-tidy',
            file_path,
            '-p', str(self.compile_commands_path.parent),
            '--checks=-*,misc-include-cleaner,readability-redundant-preprocessor',
            '--format-style=none'
        ]

        try:
            result = subprocess.run(
                clang_tidy_cmd,
                capture_output=True,
                text=True,
                timeout=60
            )

            output = result.stdout
            return self._parse_clang_tidy_output(file_path, output)

        except subprocess.TimeoutExpired:
            print(f"    ⚠ 分析超时 - {file_name}", flush=True)
            return []
        except Exception as e:
            print(f"    ⚠ 分析失败 - {file_name}: {e}", flush=True)
            return []

    def _run_clangd_diagnostics(self, file_path: str) -> List[UnusedInclude]:
        file_name = Path(file_path).name
        print(f"  → 正在分析: {file_name}...", flush=True)

        # 尝试使用 clangd 的 --check 模式
        clangd_cmd = [
            'clangd',
            '--compile-commands-dir=' + str(self.compile_commands_path.parent),
            '--check=' + file_path
        ]

        try:
            result = subprocess.run(
                clangd_cmd,
                capture_output=True,
                text=True,
                timeout=30
            )

            output = result.stdout + result.stderr
            return self._parse_clangd_output(file_path, output)

        except subprocess.TimeoutExpired:
            print(f"    ⚠ 分析超时 - {file_name}", flush=True)
            return []
        except Exception as e:
            print(f"    ⚠ clangd 分析失败 - {file_name}: {e}", flush=True)
            return []

        except Exception as e:
            print(f"警告: clangd 分析失败 - {file_path}: {e}")
            return []

    def _parse_iwyu_output(self, file_path: str, output: str) -> List[UnusedInclude]:
        """解析 IWYU 输出"""
        unused = []

        # IWYU 输出格式示例：
        # file.cpp:10:1: warning: #include "unused.h" is not used
        pattern = r'([^:]+):(\d+):\d+:.*#include\s+[<"]([^>"]+)[>"].*not.*use'

        for match in re.finditer(pattern, output, re.IGNORECASE):
            unused.append(UnusedInclude(
                file_path=match.group(1),
                line_number=int(match.group(2)),
                include_path=match.group(3),
                reason="未使用 (IWYU)"
            ))

        return unused

    def _parse_clang_tidy_output(self, file_path: str, output: str) -> List[UnusedInclude]:
        """解析 clang-tidy 输出"""
        unused = []

        # clang-tidy 输出格式示例：
        # file.cpp:10:1: warning: #include is unused [misc-include-cleaner]
        lines = output.split('\n')

        for line in lines:
            # 匹配警告行
            match = re.match(r'([^:]+):(\d+):\d+:\s+warning:.*#include.*[<"]([^>"]+)[>"].*unused', line)
            if match:
                unused.append(UnusedInclude(
                    file_path=match.group(1),
                    line_number=int(match.group(2)),
                    include_path=match.group(3),
                    reason="未使用 (clang-tidy)"
                ))
            else:
                # 尝试另一种格式
                match = re.match(r'([^:]+):(\d+):\d+:.*included.*never.*use', line, re.IGNORECASE)
                if match:
                    # 尝试从上下文提取头文件名
                    include_match = re.search(r'[<"]([^>"]+)[>"]', line)
                    if include_match:
                        unused.append(UnusedInclude(
                            file_path=match.group(1),
                            line_number=int(match.group(2)),
                            include_path=include_match.group(1),
                            reason="未使用 (clang-tidy)"
                        ))

        return unused

    def _parse_clangd_output(self, file_path: str, output: str) -> List[UnusedInclude]:
        """解析 clangd 输出"""
        unused = []

        # clangd 输出可能包含诊断信息
        # 格式类似：file.cpp:10:1: warning: unused include
        pattern = r'([^:]+):(\d+):\d+:.*unused.*include.*[<"]([^>"]+)[>"]'

        for match in re.finditer(pattern, output, re.IGNORECASE):
            unused.append(UnusedInclude(
                file_path=match.group(1),
                line_number=int(match.group(2)),
                include_path=match.group(3),
                reason="未使用 (clangd)"
            ))

        return unused

    def analyze(self, max_workers: int = 4, use_tool: str = 'auto', debug: bool = False) -> None:
        """
        分析所有项目文件

        Args:
            max_workers: 并行工作线程数
            use_tool: 使用的工具 ('iwyu', 'clang-tidy', 'clangd', 'auto')
            debug: 启用调试输出
        """
        # 过滤项目文件
        if debug:
            print(f"总编译条目数: {len(self.compile_commands)}")
            print("示例文件路径:")
            for entry in self.compile_commands[:5]:
                file_path = entry['file']
                is_project = self._is_project_file(file_path)
                print(f"  {'✓' if is_project else '✗'} {file_path}")

        project_files = [
            entry for entry in self.compile_commands
            if self._is_project_file(entry['file'])
        ]

        if not project_files:
            print("警告：未找到项目文件，检查源代码根目录设置")
            print(f"源代码根目录: {self.source_root}")
            print("\n提示：使用 -s 参数手动指定源代码根目录，或使用 --debug 查看详细信息")
            return

        print(f"找到 {len(project_files)} 个项目文件需要分析")

        # 选择工具
        if use_tool == 'auto':
            if self._check_tool_available('include-what-you-use'):
                use_tool = 'iwyu'
                print("使用工具: include-what-you-use")
            elif self._check_tool_available('clang-tidy'):
                use_tool = 'clang-tidy'
                print("使用工具: clang-tidy")
            elif self._check_tool_available('clangd'):
                use_tool = 'clangd'
                print("使用工具: clangd")
            else:
                print("错误：未找到可用的分析工具 (include-what-you-use, clang-tidy, 或 clangd)")
                print("\n安装建议:")
                print("  - include-what-you-use: https://include-what-you-use.org/")
                print("  - clang-tidy: 通常随 LLVM/Clang 安装")
                print("  - clangd: https://clangd.llvm.org/")
                sys.exit(1)

        # 选择分析函数
        if use_tool == 'iwyu':
            analyze_func = self._run_include_what_you_use
        elif use_tool == 'clang-tidy':
            analyze_func = self._run_clang_tidy
        elif use_tool == 'clangd':
            def analyze_func(entry):
                return self._run_clangd_diagnostics(entry['file'])
        else:
            print(f"错误：未知工具 '{use_tool}'")
            sys.exit(1)

        # 并行分析
        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            futures = {
                executor.submit(analyze_func, entry): entry
                for entry in project_files
            }

            completed = 0
            total = len(project_files)

            print(f"\n{'='*80}")
            print(f"开始并行分析 (使用 {max_workers} 个工作线程)")
            print(f"{'='*80}\n")

            for future in as_completed(futures):
                completed += 1
                entry = futures[future]
                file_name = Path(entry['file']).name
                progress_percent = (completed / total) * 100

                # 实时输出当前状态
                try:
                    results = future.result()
                    self.unused_includes.extend(results)

                    if results:
                        status_msg = f"[{completed}/{total}] ({progress_percent:.1f}%) ✓ {file_name} - 发现 {len(results)} 个未使用头文件"
                        print(status_msg, flush=True)
                        for r in results:
                            print(f"    → 行 {r.line_number}: {r.include_path}", flush=True)
                    else:
                        print(f"[{completed}/{total}] ({progress_percent:.1f}%) ✓ {file_name}", flush=True)

                except Exception as e:
                    print(f"[{completed}/{total}] ({progress_percent:.1f}%) ✗ {file_name} - 错误: {e}", flush=True)

    def generate_report(self, output_file: str = None) -> str:
        """
        生成分析报告

        Args:
            output_file: 输出文件路径（可选）

        Returns:
            报告内容
        """
        # 按文件分组
        grouped: Dict[str, List[UnusedInclude]] = {}
        for item in self.unused_includes:
            if item.file_path not in grouped:
                grouped[item.file_path] = []
            grouped[item.file_path].append(item)

        # 生成报告
        lines = []
        lines.append("=" * 80)
        lines.append("未使用头文件分析报告")
        lines.append("=" * 80)
        lines.append(f"\n总计: {len(self.unused_includes)} 个未使用的头文件")
        lines.append(f"涉及文件: {len(grouped)} 个\n")

        if not self.unused_includes:
            lines.append("\n✓ 未发现未使用的头文件！\n")
        else:
            # 按文件输出
            for file_path in sorted(grouped.keys()):
                items = grouped[file_path]
                lines.append("-" * 80)
                lines.append(f"\n文件: {file_path}")
                lines.append(f"未使用头文件数量: {len(items)}\n")

                for item in sorted(items, key=lambda x: x.line_number):
                    lines.append(f"  行 {item.line_number}: #include \"{item.include_path}\"")
                    if item.reason:
                        lines.append(f"    原因: {item.reason}")

                lines.append("")

        lines.append("=" * 80)

        report = "\n".join(lines)

        # 输出到文件
        if output_file:
            try:
                with open(output_file, 'w', encoding='utf-8') as f:
                    f.write(report)
                print(f"\n报告已保存到: {output_file}")
            except Exception as e:
                print(f"警告：无法保存报告到文件: {e}")

        return report

    def generate_json_report(self, output_file: str) -> None:
        """生成 JSON 格式报告"""
        data = {
            'summary': {
                'total_unused_includes': len(self.unused_includes),
                'affected_files': len(set(item.file_path for item in self.unused_includes))
            },
            'unused_includes': [
                {
                    'file': item.file_path,
                    'line': item.line_number,
                    'include': item.include_path,
                    'reason': item.reason
                }
                for item in self.unused_includes
            ]
        }

        try:
            with open(output_file, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2, ensure_ascii=False)
            print(f"JSON 报告已保存到: {output_file}")
        except Exception as e:
            print(f"错误：无法保存 JSON 报告: {e}")


def main():
    """主函数"""
    parser = argparse.ArgumentParser(
        description='分析 C++ 项目中未使用的头文件',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s                                    # 使用默认设置
  %(prog)s -c build/compile_commands.json     # 指定编译数据库
  %(prog)s -o report.txt                      # 保存报告到文件
  %(prog)s -j 8                               # 使用 8 个线程
  %(prog)s --tool clang-tidy                  # 使用 clang-tidy
  %(prog)s --json report.json                 # 生成 JSON 报告
        """
    )

    parser.add_argument(
        '-c', '--compile-commands',
        default='build/compile_commands.json',
        help='compile_commands.json 文件路径 (默认: build/compile_commands.json)'
    )

    parser.add_argument(
        '-s', '--source-root',
        help='源代码根目录 (默认: 自动检测)'
    )

    parser.add_argument(
        '-o', '--output',
        help='输出报告文件路径'
    )

    parser.add_argument(
        '--json',
        help='输出 JSON 格式报告文件路径'
    )

    parser.add_argument(
        '-j', '--jobs',
        type=int,
        default=2,
        help='并行工作线程数 (默认: 2，建议不要设置太大以避免系统负载过高)'
    )

    parser.add_argument(
        '--tool',
        choices=['auto', 'iwyu', 'clang-tidy', 'clangd'],
        default='auto',
        help='使用的分析工具 (默认: auto - 自动选择)'
    )

    parser.add_argument(
        '--debug',
        action='store_true',
        help='启用调试输出'
    )

    args = parser.parse_args()

    # 创建分析器
    analyzer = UnusedHeaderAnalyzer(
        compile_commands_path=args.compile_commands,
        source_root=args.source_root
    )

    # 执行分析
    print("开始分析...\n")
    analyzer.analyze(max_workers=args.jobs, use_tool=args.tool)

    # 生成报告
    print("\n" + "=" * 80)
    report = analyzer.generate_report(output_file=args.output)
    print(report)

    # 生成 JSON 报告
    if args.json:
        analyzer.generate_json_report(args.json)


if __name__ == '__main__':
    main()
