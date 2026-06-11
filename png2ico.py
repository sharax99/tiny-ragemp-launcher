import sys

def png_to_ico(png_path, ico_path):
    with open(png_path, 'rb') as f:
        png_data = f.read()

    header = b'\x00\x00\x01\x00\x01\x00'
    size = len(png_data)
    entry = bytes([0, 0, 0, 0, 1, 0, 32, 0])
    entry += size.to_bytes(4, byteorder='little')
    entry += (22).to_bytes(4, byteorder='little')

    with open(ico_path, 'wb') as f:
        f.write(header)
        f.write(entry)
        f.write(png_data)

png_to_ico('launcher_logo.png', 'src/launcher.ico')
print("Successfully generated src/launcher.ico")
