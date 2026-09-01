from pathlib import Path

# Change this to your root directory path
root_dir = Path("")

for file_path in root_dir.rglob("1.png"):
    folder_name = file_path.parent.name
    new_filename = f"{folder_name}.png"
    
    # Move and rename to the parent's parent directory
    destination = file_path.parent.parent / new_filename
    
    # Or to move into the root_dir directly, use:
    # destination = root_dir / new_filename
    
    file_path.rename(destination)
    print(f"Moved: {file_path} -> {destination}")