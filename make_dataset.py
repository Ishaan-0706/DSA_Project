import os
from PIL import Image, ImageEnhance

os.makedirs('dataset', exist_ok=True)

for name in os.listdir('masters'):
    stem = os.path.splitext(name)[0]
    img = Image.open(f'masters/{name}').convert('RGB')
    w, h = img.size

    img.save(f'dataset/{stem}_master.png')
    img.resize((w // 2, h // 2)).save(f'dataset/{stem}_resized.png')
    img.save(f'dataset/{stem}_recompressed.jpg', quality=60)
    img.crop((w // 10, h // 10, w, h)).save(f'dataset/{stem}_cropped.png')
    ImageEnhance.Brightness(img).enhance(1.15).save(f'dataset/{stem}_bright.png')

print('Done. Check ./dataset/')