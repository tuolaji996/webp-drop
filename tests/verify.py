import argparse
import hashlib
import json
import shutil
import struct
import subprocess
from datetime import datetime
from pathlib import Path

from PIL import Image, ImageChops, ImageCms, ImageStat


parser = argparse.ArgumentParser(description='Verify WebpDrop with generated fixtures and optional example images.')
parser.add_argument('example_directory', nargs='?', type=Path)
parser.add_argument('--gui', action='store_true')
arguments = parser.parse_args()

ROOT = Path(__file__).resolve().parents[1]
RUN = ROOT / 'test-output' / datetime.now().strftime('%Y%m%d-%H%M%S')
RUN.mkdir(parents=True)
ISOLATED = RUN / 'single-exe'
ISOLATED.mkdir()
EXE = ISOLATED / 'WebpDrop.exe'
shutil.copy2(ROOT / 'WebpDrop.exe', EXE)
CHECKS = []


def record(name):
    CHECKS.append(name)
    print('PASS:', name, flush=True)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def convert(files, *options, expected=0):
    process = subprocess.run(
        [str(EXE), '--convert', *map(str, options), *map(str, files)],
        cwd=ISOLATED, capture_output=True, timeout=90,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    assert process.returncode == expected, (
        process.returncode, expected, process.stdout, process.stderr, files
    )


def equal_pixels(source, output):
    with Image.open(source) as original, Image.open(output) as converted:
        assert original.size == converted.size
        a = original.convert('RGBA')
        b = converted.convert('RGBA')
        assert a.tobytes() == b.tobytes(), (source, output, ImageChops.difference(a, b).getbbox())


def imports(path):
    data = path.read_bytes()
    offset = struct.unpack_from('<I', data, 0x3C)[0]
    machine, sections = struct.unpack_from('<HH', data, offset + 4)
    optional_size = struct.unpack_from('<H', data, offset + 20)[0]
    optional = offset + 24
    assert machine == 0x8664
    assert struct.unpack_from('<H', data, optional)[0] == 0x20B
    assert struct.unpack_from('<H', data, optional + 68)[0] == 2
    import_rva = struct.unpack_from('<I', data, optional + 120)[0]
    mapping = []
    for index in range(sections):
        base = optional + optional_size + index * 40
        vsize, rva, size, raw = struct.unpack_from('<IIII', data, base + 8)
        mapping.append((rva, max(size, vsize), raw))

    def address(rva):
        for start, size, raw in mapping:
            if start <= rva < start + size:
                return raw + rva - start
        raise AssertionError(rva)

    found = []
    cursor = address(import_rva)
    while any(data[cursor:cursor + 20]):
        name = address(struct.unpack_from('<I', data, cursor + 12)[0])
        end = data.index(b'\0', name)
        found.append(data[name:end].decode('ascii').lower())
        cursor += 20
    return found


dependencies = imports(EXE)
system_libraries = {
    'kernel32.dll', 'user32.dll', 'gdi32.dll', 'comctl32.dll', 'comdlg32.dll',
    'ole32.dll', 'oleaut32.dll', 'shell32.dll', 'shlwapi.dll', 'windowscodecs.dll',
    'msvcrt.dll', 'ucrtbase.dll', 'advapi32.dll', 'ntdll.dll',
}
assert all(name in system_libraries or name.startswith('api-ms-win-') for name in dependencies), dependencies
record('64-bit GUI EXE imports only Windows system DLLs; runs from an otherwise empty folder')

fixtures = RUN / 'unicode-input'
fixtures.mkdir()
profile = ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes()
rgba = Image.new('RGBA', (96, 64))
rgba.putdata([
    ((x * 13) % 256, (y * 23) % 256, (x * y) % 256, [0, 64, 128, 255][x // 24])
    for y in range(64) for x in range(96)
])
transparent = fixtures / '\u900f\u660e \u56fe\u7247.WeBp'
rgba.save(transparent, 'WEBP', lossless=True, exact=True, icc_profile=profile)
convert([transparent])
equal_pixels(transparent, transparent.with_suffix('.png'))
with Image.open(transparent.with_suffix('.png')) as png:
    assert png.info.get('icc_profile') == profile
record('default PNG preserves every RGBA pixel and the ICC profile; mixed-case extension works')

original_png_hash = digest(transparent.with_suffix('.png'))
convert([transparent])
equal_pixels(transparent, fixtures / (transparent.stem + ' (2).png'))
assert digest(transparent.with_suffix('.png')) == original_png_hash
record('existing output is preserved and the next file receives a numbered name')

jpegs = RUN / 'chosen-jpeg-output'
jpegs.mkdir()
convert([transparent], '--jpeg', '--output', jpegs, '--quality', 100)
with Image.open(transparent) as image, Image.open(jpegs / (transparent.stem + '.jpg')) as jpeg:
    expected = Image.new('RGBA', image.size, 'white')
    expected.alpha_composite(image.convert('RGBA'))
    difference = ImageStat.Stat(ImageChops.difference(expected.convert('RGB'), jpeg.convert('RGB')))
    assert max(difference.mean) < 12, difference.mean
    assert min(jpeg.getpixel((0, 0))) >= 250
    assert jpeg.info.get('icc_profile') == profile
record('JPEG uses the chosen output folder, preserves the ICC profile, and composites transparency over white')

first = Image.new('RGBA', (48, 32), (200, 30, 70, 255))
second = Image.new('RGBA', (48, 32), (20, 180, 100, 255))
animated = fixtures / 'animated.webp'
first.save(animated, 'WEBP', save_all=True, append_images=[second], duration=100, loop=0, lossless=True)
convert([animated])
equal_pixels(animated, animated.with_suffix('.png'))
record('animated WebP produces the first composed frame')

corrupt = fixtures / 'corrupt.webp'
corrupt.write_bytes(b'RIFF\x1c\x00\x00\x00WEBPVP8 malformed')
empty = fixtures / 'empty.webp'
empty.write_bytes(b'')
convert([corrupt, empty, animated], expected=1)
assert not corrupt.with_suffix('.png').exists()
assert not empty.with_suffix('.png').exists()
assert (fixtures / 'animated (2).png').exists()
assert not list(fixtures.glob('.webpdrop-*.tmp'))
record('corrupt and empty inputs fail cleanly; later valid files still convert; temporary files are cleaned up')

convert([animated], '--output', corrupt, expected=1)
convert([animated], '--quality', 0, expected=2)
record('invalid output locations and invalid JPEG quality produce failure exit codes')

long_folder = RUN / ('long-path-' + 'x' * 70) / ('sub-' + 'y' * 70) / ('sub-' + 'z' * 70)
long_folder.mkdir(parents=True)
long_source = long_folder / '\u957f\u8def\u5f84\u56fe\u7247.webp'
assert len(str(long_source)) > 260
shutil.copy2(transparent, long_source)
convert([long_source])
equal_pixels(long_source, long_source.with_suffix('.png'))
record('a Chinese filename and a path longer than 260 characters convert successfully')

same_name = RUN / 'same-name'
for index, image in enumerate([first, second]):
    folder = same_name / str(index)
    folder.mkdir(parents=True)
    image.save(folder / 'same.webp', 'WEBP', lossless=True)
shared = same_name / 'shared-output'
shared.mkdir()
convert([same_name / '0' / 'same.webp', same_name / '1' / 'same.webp'], '--output', shared)
equal_pixels(same_name / '0' / 'same.webp', shared / 'same.png')
equal_pixels(same_name / '1' / 'same.webp', shared / 'same (2).png')
record('two inputs with the same basename safely share one output directory')

sample_count = 0
copied = []
if arguments.example_directory:
    example = arguments.example_directory
    sources = sorted(example.glob('*.webp'))
    before = {str(path): digest(path) for path in sources}
    copies = RUN / 'example-copies'
    copies.mkdir()
    copied = []
    for path in sources:
        target = copies / path.name
        shutil.copy2(path, target)
        copied.append(target)
    assert copied, 'No WebP files found in the example directory'
    convert(copied)
    for path in copied:
        equal_pixels(path, path.with_suffix('.png'))
    record(f'all {len(copied)} example WebPs convert to PNG with identical decoded pixels')
    convert(copied, '--jpeg', '--output', jpegs)
    for path in copied:
        with Image.open(path) as source, Image.open(jpegs / (path.stem + '.jpg')) as output:
            assert source.size == output.size
            assert output.format == 'JPEG'
    assert before == {str(path): digest(path) for path in sources}
    sample_count = len(copied)
    record(f'all {len(copied)} example WebPs convert to JPEG; source hashes remain unchanged')

if arguments.gui:
    gui_input = RUN / 'gui-input'
    gui_input.mkdir()
    for path in [*copied, transparent, animated, corrupt]:
        shutil.copy2(path, gui_input / path.name)
    delete_input = RUN / 'gui-delete-input'
    delete_input.mkdir()
    delete_pixels = {}
    for name, color in [('a-valid.webp', (40, 120, 220)), ('b-valid.webp', (200, 70, 40))]:
        image = Image.new('RGB', (2048, 2048), color)
        image.save(delete_input / name, 'WEBP', lossless=True)
        delete_pixels[name] = image.convert('RGBA').tobytes()
    shutil.copy2(corrupt, delete_input / 'z-corrupt.webp')
    blocked_input = RUN / 'gui-delete-blocked'
    blocked_input.mkdir()
    shutil.copy2(transparent, blocked_input / 'locked.webp')
    shutil.copy2(transparent, RUN / 'identity-change.webp')
    gui_process = subprocess.run(
        [str(ROOT / 'build' / 'GuiSmoke.exe'), str(gui_input), str(RUN)],
        cwd=ROOT, capture_output=True, timeout=90,
    )
    print(gui_process.stdout.decode('utf-8', errors='replace'), end='', flush=True)
    assert gui_process.returncode == 0, (gui_process.returncode, gui_process.stderr)
    for path in gui_input.glob('*.webp'):
        if path.name != 'corrupt.webp':
            equal_pixels(path, path.with_suffix('.png'))
    for name in ['ui-default.png', 'ui-png-complete.png', 'ui-compact.png', 'ui-jpeg-custom.png', 'ui-delete-complete.png']:
        with Image.open(RUN / name) as screenshot:
            assert min(screenshot.size) >= 400
            assert max(ImageStat.Stat(screenshot.convert('RGB')).stddev) > 15
    assert not list(gui_input.glob('.webpdrop-*.tmp'))
    assert not list((RUN / 'gui-jpeg-output').glob('.webpdrop-*.tmp'))
    record('native GUI drag/drop, defaults, stop/retry, custom JPEG output, minimum window size, and nonblank screenshots')
    for name, pixels in delete_pixels.items():
        source = delete_input / name
        assert not source.exists()
        outputs = list(delete_input.glob(source.stem + '*.png'))
        assert outputs
        for path in outputs:
            with Image.open(path) as converted:
                assert converted.convert('RGBA').tobytes() == pixels
    assert (delete_input / 'z-corrupt.webp').read_bytes() == corrupt.read_bytes()
    assert (blocked_input / 'locked.webp').read_bytes() == transparent.read_bytes()
    assert (RUN / 'identity-change.webp').exists()
    assert not list(delete_input.glob('.webpdrop-*.tmp'))
    equal_pixels(blocked_input / 'locked.webp', blocked_input / 'locked.png')
    equal_pixels(RUN / 'identity-change.webp', RUN / 'identity-change.png')
    record('optional source recycling occurs only after a valid output; cancelled, damaged, locked, and modified originals are protected')

result = {'checks': CHECKS, 'sample_count': sample_count, 'imports': dependencies, 'exe_bytes': EXE.stat().st_size}
(RUN / 'results.json').write_text(json.dumps(result, ensure_ascii=True, indent=2), encoding='utf-8')
print('RESULTS:', RUN, flush=True)
