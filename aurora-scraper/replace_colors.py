import os
import glob

replacements = {
    "#00dbe9": "#3b82f6",
    "#7701d0": "#1d4ed8",
    "#00c5d4": "#2563eb" # This was hover color for primary
}

def process_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()

    new_content = content
    for old, new in replacements.items():
        new_content = new_content.replace(old, new)
        new_content = new_content.replace(old.upper(), new)

    if new_content != content:
        with open(filepath, 'w') as f:
            f.write(new_content)
        print(f"Updated {filepath}")

for root, _, files in os.walk('pages'):
    for file in files:
        if file.endswith('.lumen'):
            process_file(os.path.join(root, file))

process_file('../vn_modules/lumen.vn')
