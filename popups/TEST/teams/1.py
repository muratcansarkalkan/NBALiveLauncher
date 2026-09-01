import os
import re
import glob
import subprocess

# 1. Extract all .big files into matching directory (e.g. large~bo.big -> bo)
for big_file in glob.glob("*.big"):
    match = re.search(r'~([^.]+)\.big$', big_file)
    if match:
        out_dir = match.group(1)
        subprocess.run(["big_extract", big_file, out_dir], check=True)

# 2. Process .fsh files in each directory
for root, _, files in os.walk("."):
    fsh_files = [f for f in files if f.endswith(".fsh")]
    if not fsh_files:
        continue
    
    # Target parameter replacing *.fsh with absolute target format: -=<dir>\libs\%s.png
    # Passing single '%' directly to gx (bypassing batch-file '%%' escaping)
    out_param = f"-={root}\\%s.png"
    
    cmd = ["gx", "-f!*_mm*", out_param] + fsh_files
    subprocess.run(cmd, cwd=root, check=True)