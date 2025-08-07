import os

def convert_directory_to_txt(directory, output_file):
    # Define the file extensions to include
    file_extensions = ('.cpp', '.h', '.hpp', '.CMake', '.qml')
    
    with open(output_file, 'w', encoding='utf-8') as txt_file:
        # Walk through the directory
        for root, _, files in os.walk(directory):
            for file in files:
                if file.endswith(file_extensions):
                    file_path = os.path.join(root, file)
                    txt_file.write(f"File: {file_path}\n")
                    txt_file.write("-" * 80 + "\n")
                    
                    # Read and write the file content
                    with open(file_path, 'r', encoding='utf-8') as f:
                        txt_file.write(f.read())
                    
                    txt_file.write("\n" + "=" * 80 + "\n\n")

# Example usage
convert_directory_to_txt('libs', 'libs.txt')