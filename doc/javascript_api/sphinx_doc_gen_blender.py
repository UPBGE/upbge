#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2024 UPBGE Contributors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Generate JavaScript/TypeScript API documentation from within Blender.

This script runs inside Blender and generates JavaScript API documentation
by converting the Python API documentation. It uses the same approach as
sphinx_doc_gen.py but outputs JavaScript/TypeScript formatted RST files.

Usage:
    blender --background --factory-startup --python doc/javascript_api/sphinx_doc_gen_blender.py

    Or with options:
    blender --background --factory-startup --python doc/javascript_api/sphinx_doc_gen_blender.py -- --output=doc/javascript_api/rst
"""

import os
import sys
import re
import shutil
from pathlib import Path

try:
    import bpy
except ImportError:
    print("\nERROR: this script must run from inside Blender")
    print(__doc__)
    sys.exit(1)

# Import the Python API doc generator to reuse its functions
# We'll adapt it for JavaScript
SCRIPT_DIR = os.path.abspath(os.path.dirname(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, '../..'))
PYTHON_API_DIR = os.path.join(PROJECT_ROOT, 'doc', 'python_api')

# Add Python API directory to path to import sphinx_doc_gen utilities
if PYTHON_API_DIR not in sys.path:
    sys.path.insert(0, PYTHON_API_DIR)

# Now we can import and use Python API generation functions
# But we'll convert the output to JavaScript format


def convert_python_to_javascript_code(code):
    """Convert Python code to JavaScript."""
    result = code
    
    # Remove import statements (bge is global in JS)
    result = re.sub(r'^import\s+bge\s*$', '', result, flags=re.MULTILINE)
    result = re.sub(r'^import\s+mathutils\s*$', '', result, flags=re.MULTILINE)
    
    # Convert print to console.log
    result = re.sub(r'print\(([^)]+)\)', r'console.log(\1)', result)
    
    # Convert boolean literals
    result = re.sub(r'\bTrue\b', 'true', result)
    result = re.sub(r'\bFalse\b', 'false', result)
    result = re.sub(r'\bNone\b', 'null', result)
    
    # Convert None checks
    result = re.sub(r'\bis not None\b', '!== null', result)
    result = re.sub(r'\bis None\b', '=== null', result)
    
    # Convert comments: line-start # and end-of-line # to //
    result = re.sub(r'^(\s*)#\s*', r'\1// ', result, flags=re.MULTILINE)
    result = re.sub(r'\s+#\s+', r' // ', result)
    
    # Convert variable assignments (add const)
    lines = result.split('\n')
    converted_lines = []
    in_function = False
    
    for line in lines:
        # Convert function definitions
        if re.match(r'^\s*def\s+\w+\s*\(', line):
            in_function = True
            line = re.sub(r'def\s+(\w+)\s*\(([^)]*)\):', r'function \1(\2) {', line)
            converted_lines.append(line)
            continue
        
        # Detect function end
        if in_function and (line.strip() == '' or re.match(r'^\s*(def|class)\s+', line)):
            in_function = False
        
        # Convert variable assignments (simple cases)
        if not in_function and re.match(r'^(\s+)([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*', line):
            if not re.match(r'^\s*(const|let|var)\s+', line):
                line = re.sub(r'^(\s+)([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*', r'\1const \2 = ', line)
        
        # Convert Vector access
        line = re.sub(r'\.x\b', '[0]', line)
        line = re.sub(r'\.y\b', '[1]', line)
        line = re.sub(r'\.z\b', '[2]', line)
        line = re.sub(r'mathutils\.Vector\(\(([^)]+)\)\)', r'[\1]', line)
        
        # Convert string formatting
        line = re.sub(r'f"([^"]+)"', r'`\1`', line)
        line = re.sub(r'f\'([^\']+)\'', r'`\1`', line)
        
        converted_lines.append(line)
    
    result = '\n'.join(converted_lines)
    
    # Add semicolons (simple heuristic)
    result = re.sub(r'([^;{}\n])\n(?![\s]*[{}:;])', r'\1;\n', result)
    result = re.sub(r';\s*;', ';', result)

    # Fix common Python variable names in converted code
    result = re.sub(r'\bco\.', 'cont.', result)
    result = re.sub(r'\bcontroller\.', 'cont.', result)

    return result


def convert_rst_content(content):
    """Convert RST content from Python to JavaScript."""
    result = content
    
    # Update title and headers
    result = re.sub(r'Python API', 'JavaScript/TypeScript API', result)
    result = re.sub(r'Python script', 'JavaScript/TypeScript script', result)
    result = re.sub(r'Python controller', 'JavaScript/TypeScript controller', result)
    result = re.sub(r'Gets the Python controller', 'Gets the JavaScript controller', result)
    
    # Convert code blocks from python to javascript
    def replace_code_block(match):
        full_match = match.group(0)
        code_block_type = match.group(1) if match.lastindex >= 1 else 'python'
        code = match.group(2) if match.lastindex >= 2 else ''
        
        # Convert Python code to JavaScript
        js_code = convert_python_to_javascript_code(code)
        
        # Replace code-block directive
        return full_match.replace('.. code-block:: python', '.. code-block:: javascript').replace(code, js_code)
    
    # Match code blocks (capture until next directive at line start or end; allow \n\n inside block)
    pattern = r'(\.\. code-block::\s+)python(\s*\n)(.*?)(?=\n\.\. \w|$)'
    result = re.sub(pattern,
                    lambda m: m.group(1) + 'javascript' + m.group(2) + convert_python_to_javascript_code(m.group(3)),
                    result, flags=re.DOTALL)
    
    # Update type references
    result = re.sub(r':type: dictionary', ':type: object (JavaScript object)', result)
    result = re.sub(r':type: list', ':type: array', result)
    
    # Add JavaScript-specific note
    if 'bge.logic' in result and 'JavaScript/TypeScript' not in result[:1000]:
        note = (
            ".. note::\n"
            "   This is the JavaScript/TypeScript version of the Python API.\n"
            "   The API functionality is the same, but syntax differs. See\n"
            "   :ref:`Python vs JavaScript Differences <info_javascript_python_differences>`\n"
            "   for details.\n\n"
        )
        # Insert after title
        title_match = re.search(r'^([^\n]+)\n(=+)\n', result)
        if title_match:
            insert_pos = title_match.end()
            result = result[:insert_pos] + '\n' + note + result[insert_pos:]
    
    return result


def generate_bge_logic_rst(output_dir):
    """Generate bge.javascript.rst from bge.logic.rst."""
    python_api_rst = os.path.join(PROJECT_ROOT, 'doc', 'python_api', 'rst', 'bge.logic.rst')
    js_api_rst = os.path.join(output_dir, 'bge.javascript.rst')
    
    if not os.path.exists(python_api_rst):
        print(f"Warning: {python_api_rst} not found")
        return False
    
    print(f"Converting: bge.logic.rst -> bge.javascript.rst")
    
    with open(python_api_rst, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Convert content
    js_content = convert_rst_content(content)

    # Prepend reST label for :ref:`bge.javascript`
    if '.. _bge.javascript:' not in js_content:
        js_content = '.. _bge.javascript:\n\n' + js_content

    # Ensure directory exists
    os.makedirs(output_dir, exist_ok=True)

    # Write converted file
    with open(js_api_rst, 'w', encoding='utf-8') as f:
        f.write(js_content)
    
    return True


def copy_static_files(output_dir):
    """Copy static files needed for Sphinx."""
    static_src = os.path.join(SCRIPT_DIR, 'static')
    static_dst = os.path.join(output_dir, '..', 'static')
    
    if os.path.exists(static_src):
        if os.path.exists(static_dst):
            shutil.rmtree(static_dst)
        shutil.copytree(static_src, static_dst)
        print(f"Copied static files to: {static_dst}")


def main():
    """Main entry point."""
    import argparse
    
    # Parse arguments (after --)
    argv = sys.argv
    if '--' in argv:
        argv = argv[argv.index('--') + 1:]
    else:
        argv = []
    
    parser = argparse.ArgumentParser(
        description='Generate JavaScript/TypeScript API documentation from within Blender'
    )
    parser.add_argument(
        '--output',
        default=None,
        help='Output directory for generated RST files'
    )
    parser.add_argument(
        '--force',
        action='store_true',
        help='Overwrite existing files'
    )
    
    args = parser.parse_args(argv)
    
    # Set output directory
    if args.output:
        output_dir = Path(args.output)
    else:
        output_dir = Path(SCRIPT_DIR) / 'rst'
    
    output_dir = str(output_dir.resolve())
    
    print("=" * 70)
    print("JavaScript/TypeScript API Documentation Generator (Blender)")
    print("=" * 70)
    print(f"Blender version: {bpy.app.version_string}")
    print(f"Output directory: {output_dir}")
    print()
    
    # Ensure output directory exists
    os.makedirs(output_dir, exist_ok=True)
    
    # Generate documentation
    success = True
    
    # Generate bge.javascript.rst from bge.logic.rst
    if not generate_bge_logic_rst(output_dir):
        success = False
    
    # Copy static files
    copy_static_files(output_dir)
    
    if success:
        print("\n" + "=" * 70)
        print("Generation complete!")
        print("=" * 70)
        print(f"\nGenerated files in: {output_dir}")
        print("\nNext steps:")
        print("  1. Review the generated files")
        print("  2. Build the documentation:")
        print("     sphinx-build -b html -c doc/javascript_api doc/javascript_api/rst doc/javascript_api/sphinx-out")
        print("  3. Open doc/javascript_api/sphinx-out/index.html in your browser")
        return 0
    else:
        print("\nGeneration completed with warnings. Check output above.")
        return 1


if __name__ == '__main__':
    sys.exit(main())
