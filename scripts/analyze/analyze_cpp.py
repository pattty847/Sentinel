#!/usr/bin/env python3
"""
analyze_cpp.py - Advanced C++ code analysis tool
Extracts functions, methods, classes, types, and provides comprehensive code overview
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import List, Dict, Set
import json

class CppAnalyzer:
    def __init__(self, filename: str):
        self.filename = Path(filename)
        if not self.filename.exists():
            raise FileNotFoundError(f"File {filename} not found")
        
        with open(self.filename, 'r', encoding='utf-8', errors='ignore') as f:
            self.content = f.read()
        
        self.lines = self.content.split('\n')
    
    def extract_functions(self) -> List[Dict]:
        """Extract all function definitions"""
        functions = []
        
        # Pattern for function definitions with return types
        patterns = [
            # Standard function: return_type ClassName::functionName(params)
            r'^(\s*)([\w:<>*&\s]+)\s+(\w+)::(\w+)\s*\([^)]*\)\s*(?:const)?\s*{?',
            # Constructor/Destructor: ClassName::ClassName() or ClassName::~ClassName()
            r'^(\s*)(\w+)::([~]?\w+)\s*\([^)]*\)\s*{?',
            # Template functions
            r'^(\s*)(template\s*<[^>]*>\s*)?(\w+)\s+(\w+)::(\w+)\s*\([^)]*\)\s*{?'
        ]
        
        for i, line in enumerate(self.lines):
            # Skip comments and preprocessor directives
            if line.strip().startswith('//') or line.strip().startswith('#'):
                continue
                
            for pattern in patterns:
                match = re.match(pattern, line)
                if match:
                    # Extract function info
                    if '::' in line:
                        parts = line.split('::')
                        if len(parts) >= 2:
                            class_name = parts[0].split()[-1]
                            func_part = parts[1].split('(')[0].strip()
                            
                            # Extract return type
                            return_type = 'void'  # default
                            before_class = parts[0].strip()
                            type_match = re.search(r'^(\s*)(\w+(?:\s*\*|\s*&)?)\s+\w+$', before_class)
                            if type_match:
                                return_type = type_match.group(2)
                            
                            functions.append({
                                'line': i + 1,
                                'return_type': return_type,
                                'class_name': class_name,
                                'function_name': func_part,
                                'signature': line.strip(),
                                'is_constructor': func_part == class_name,
                                'is_destructor': func_part.startswith('~')
                            })
        
        return functions
    
    def extract_classes(self) -> List[Dict]:
        """Extract class definitions"""
        classes = []
        class_pattern = r'^(\s*)class\s+(\w+)(?:\s*:\s*([^{]+))?\s*\{'
        
        for i, line in enumerate(self.lines):
            match = re.match(class_pattern, line)
            if match:
                classes.append({
                    'line': i + 1,
                    'name': match.group(2),
                    'inheritance': match.group(3).strip() if match.group(3) else None,
                    'type': 'class'
                })
        
        return classes
    
    def extract_includes(self) -> List[str]:
        """Extract all include statements"""
        includes = []
        for line in self.lines:
            if line.strip().startswith('#include'):
                includes.append(line.strip())
        return includes
    
    def extract_types(self) -> Set[str]:
        """Extract custom types used in the file"""
        types = set()
        
        # Look for Qt types, custom types, etc.
        type_patterns = [
            r'\b(Q\w+)\b',  # Qt types
            r'\b([A-Z]\w*::\w+)\b',  # Namespaced types
            r'\b(std::\w+)\b',  # Standard library types
            r'\bstd::shared_ptr<(\w+)>',  # Smart pointers
            r'\bstd::unique_ptr<(\w+)>',
            r'\bstd::vector<(\w+)>',  # Containers
        ]
        
        for pattern in type_patterns:
            matches = re.findall(pattern, self.content)
            for match in matches:
                if isinstance(match, tuple):
                    types.update(match)
                else:
                    types.add(match)
        
        return types
    
    def get_function_summary(self) -> Dict:
        """Get a summary of functions by return type"""
        functions = self.extract_functions()
        summary = {}
        
        for func in functions:
            ret_type = func['return_type']
            if ret_type not in summary:
                summary[ret_type] = []
            summary[ret_type].append(func['function_name'])
        
        return summary
    
    def analyze(self) -> Dict:
        """Perform complete analysis"""
        return {
            'file': str(self.filename),
            'line_count': len(self.lines),
            'functions': self.extract_functions(),
            'classes': self.extract_classes(),
            'includes': self.extract_includes(),
            'types': sorted(list(self.extract_types())),
            'function_summary': self.get_function_summary()
        }

def main():
    parser = argparse.ArgumentParser(description='Analyze C++ files for functions, classes, and types')
    parser.add_argument('file', help='C++ file to analyze')
    parser.add_argument('--functions', '-f', action='store_true', help='Show functions only')
    parser.add_argument('--classes', '-c', action='store_true', help='Show classes only')
    parser.add_argument('--types', '-t', action='store_true', help='Show types only')
    parser.add_argument('--summary', '-s', action='store_true', help='Show summary by return type')
    parser.add_argument('--names-only', '-n', action='store_true', help='Show names only')
    parser.add_argument('--json', '-j', action='store_true', help='Output as JSON')
    parser.add_argument('--sort', action='store_true', help='Sort output alphabetically')
    
    args = parser.parse_args()
    
    try:
        analyzer = CppAnalyzer(args.file)
        analysis = analyzer.analyze()
        
        if args.json:
            print(json.dumps(analysis, indent=2))
            return
        
        # Default: show everything
        if not any([args.functions, args.classes, args.types, args.summary]):
            print(f"📁 File: {analysis['file']}")
            print(f"📏 Lines: {analysis['line_count']}")
            print(f"🔧 Functions: {len(analysis['functions'])}")
            print(f"🏗️  Classes: {len(analysis['classes'])}")
            print(f"📦 Types: {len(analysis['types'])}")
            print(f"📄 Includes: {len(analysis['includes'])}")
            print()
        
        if args.functions or not any([args.classes, args.types, args.summary]):
            functions = analysis['functions']
            if args.sort:
                functions = sorted(functions, key=lambda x: x['function_name'])
            
            print("🔧 FUNCTIONS:")
            for func in functions:
                if args.names_only:
                    print(f"  {func['function_name']}")
                else:
                    icon = "🏗️" if func['is_constructor'] else "💀" if func['is_destructor'] else "🔧"
                    print(f"  {icon} {func['return_type']} {func['class_name']}::{func['function_name']}")
            print()
        
        if args.classes:
            print("🏗️ CLASSES:")
            classes = analysis['classes']
            if args.sort:
                classes = sorted(classes, key=lambda x: x['name'])
            
            for cls in classes:
                if args.names_only:
                    print(f"  {cls['name']}")
                else:
                    inheritance = f" : {cls['inheritance']}" if cls['inheritance'] else ""
                    print(f"  📦 {cls['name']}{inheritance}")
            print()
        
        if args.types:
            print("📦 TYPES:")
            types = analysis['types']
            if args.sort:
                types = sorted(types)
            
            for typ in types:
                print(f"  📋 {typ}")
            print()
        
        if args.summary:
            print("📊 FUNCTION SUMMARY BY RETURN TYPE:")
            summary = analysis['function_summary']
            for ret_type in sorted(summary.keys()):
                funcs = summary[ret_type]
                if args.sort:
                    funcs = sorted(funcs)
                print(f"  {ret_type} ({len(funcs)}): {', '.join(funcs[:5])}")
                if len(funcs) > 5:
                    print(f"    ... and {len(funcs) - 5} more")
    
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()
