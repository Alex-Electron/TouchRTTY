import os
import glob

def update_markdown_phases():
    """
    Utility script used to batch-rename phase numbers and update text 
    inside markdown files across the entire project when the roadmap changed.
    """
    docs = glob.glob('../docs/*.md')
    docs.append('../README.md')
    
    for file in docs:
        if os.path.exists(file):
            with open(file, 'r', encoding='utf-8') as f:
                content = f.read()
                
            # Example replacement logic used during development
            content = content.replace('Phase 3', 'Phase 4')
            
            with open(file, 'w', encoding='utf-8') as f:
                f.write(content)
            print(f"Updated {file}")

if __name__ == '__main__':
    print("This script is a reference for the batch documentation updating used during development.")
