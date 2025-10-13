import os
import argparse
import sys

def convert_directory_to_txt(directory, output_file, include_md=False, include_mdc=False):
    # Define the file extensions to include
    file_extensions = ('.cpp', '.h', '.hpp', '.CMake', '.qml')
    if include_md:
        file_extensions = file_extensions + ('.md', '.MD', '.markdown', '.MARKDOWN')
    if include_mdc:
        file_extensions = file_extensions + ('.mdc', '.MDC')

    with open(output_file, 'w', encoding='utf-8') as txt_file:
        for root, _, files in os.walk(directory):
            for file in files:
                if file.endswith(file_extensions):
                    file_path = os.path.join(root, file)
                    txt_file.write(f"File: {file_path}\n")
                    txt_file.write("-" * 80 + "\n")
                    try:
                        with open(file_path, 'r', encoding='utf-8') as f:
                            txt_file.write(f.read())
                    except Exception as e:
                        txt_file.write(f"[Error reading file: {e}]\n")
                    txt_file.write("\n" + "=" * 80 + "\n\n")

def main():
    parser = argparse.ArgumentParser(
        description="Convert a directory of source files to a single .txt file for review or analysis.",
        formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument(
        "directory",
        help="The root directory to scan for files."
    )
    parser.add_argument(
        "output_file",
        help="The output .txt file to write the combined contents."
    )
    parser.add_argument(
        "--include-md", "--markdown", action="store_true",
        help="Include Markdown files (.md, .markdown) in the output."
    )
    parser.add_argument(
        "--include-mdc", action="store_true",
        help="Include MDC files (.mdc, .MDC) in the output."
    )
    parser.add_argument(
        "-v", "--version", action="version", version="to_txt.py 1.1"
    )

    args = parser.parse_args()

    if not os.path.isdir(args.directory):
        print(f"Error: Directory '{args.directory}' does not exist.", file=sys.stderr)
        sys.exit(1)

    convert_directory_to_txt(
        args.directory,
        args.output_file,
        include_md=args.include_md,
        include_mdc=args.include_mdc
    )

if __name__ == "__main__":
    main()