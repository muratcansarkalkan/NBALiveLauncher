import os

# Set how many characters to strip
X = 6  # Strip first X characters
Y = 2  # Strip last Y characters (before extension)

for filename in os.listdir("."):
    if os.path.isfile(filename):
        name, ext = os.path.splitext(filename)
        
        # Ensure filename is long enough to strip X and Y characters
        if len(name) > (X + Y):
            new_name = name[X:-Y if Y > 0 else None] + ext
            
            os.rename(filename, new_name)
            print(f"Renamed: {filename} -> {new_name}")