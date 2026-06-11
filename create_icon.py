from PIL import Image

def generate_icon(png_path, ico_path):
    img = Image.open(png_path)
    icon_sizes = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    img.save(ico_path, format='ICO', sizes=icon_sizes)

if __name__ == '__main__':
    generate_icon('launcher_logo.png', 'src/launcher.ico')
    print("Multi-resolution ICO created successfully!")
