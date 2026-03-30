import sys
import os

def replace_in_file(filepath, old_text, new_text):
    """
    Utility script used during development by the Gemini agent to safely perform
    multi-line string replacements in C++ files without dealing with CLI escaping issues.
    """
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
        
    if old_text in content:
        content = content.replace(old_text, new_text)
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"Successfully patched {filepath}")
    else:
        print(f"Error: Old text not found in {filepath}")

if __name__ == '__main__':
    print("This script is a reference for the multi-line replacement logic used during development.")
