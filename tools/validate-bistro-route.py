"""Validate the baked Bistro loop against current OBJ geometry and draw its plan.

Requires numpy, scipy and Pillow (offline tooling only). Run from the repo root.
"""
import hashlib
import json
import math
import re
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import distance_transform_edt, label
from scipy.spatial import cKDTree

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'Assets/Scenes/Amazon Lumberyard Bistro/Exterior/exterior.obj'
OUTPUT = ROOT / 'Build/RouteValidation'
STEP = 10.0  # cm; the actor envelope below includes a rasterization margin.


def is_route_obstacle(group):
    name = group.lower()
    return ('building' in name or 'tree_large' in name or
            name.startswith(('bux_hedge_box', 'bux_hedge_cylinder')) or
            name in ('paris_street_38', 'paris_street_39'))


def geometry():
    vertices, floor, obstacles = [], [], []
    material = group = ''
    digest = hashlib.sha256()
    with SOURCE.open('rb') as stream:
        for raw in stream:
            digest.update(raw)
            if raw.startswith(b'v '):
                vertices.append(tuple(map(float, raw.split()[1:4])))
            elif raw.startswith(b'usemtl '):
                material = raw.split()[1].decode()
            elif raw.startswith(b'g '):
                group = raw.split()[1].decode()
            elif raw.startswith(b'f '):
                ids = [int(p.split(b'/')[0]) for p in raw.split()[1:]]
                points = [vertices[i - 1 if i > 0 else i] for i in ids]
                for i in range(1, len(points) - 1):
                    tri = (points[0], points[i], points[i + 1])
                    low, high = min(p[1] for p in tri), max(p[1] for p in tri)
                    if 'Pavement' in material and high < 55:
                        floor.append(tri)
                    elif is_route_obstacle(group) and 'Pavement' not in material and low < 220 and high > 50:
                        obstacles.append(tri)
    vertices = np.array(vertices)
    lower = np.floor(vertices[:, [0, 2]].min(axis=0) / STEP) * STEP - 50
    upper = np.ceil(vertices[:, [0, 2]].max(axis=0) / STEP) * STEP + 50
    size = np.ceil((upper - lower) / STEP).astype(int) + 1
    masks = []
    for triangles in (floor, obstacles):
        bitmap = Image.new('1', tuple(size))
        draw = ImageDraw.Draw(bitmap)
        for tri in triangles:
            points = np.rint((np.array(tri)[:, [0, 2]] - lower) / STEP).astype(int)
            points = [tuple(p) for p in points]
            draw.polygon(points, fill=1)
            draw.line(points + points[:1], fill=1)
        masks.append(bitmap)
    # The added basin is absent from the original OBJ. Read its current layout.
    water = (ROOT / 'Apps/App/src/BistroWaterLayout.h').read_text()
    values = lambda name: np.array([float(x.rstrip('f')) for x in re.search(name + r'\(([^)]+)\)', water)[1].split(',')])
    center, half = values('Center')[[0, 2]], values('HalfExtent')[[0, 2]] + 8  # Basin wall thickness.
    yaw = math.radians(float(re.search(r'YawDegrees\s*=\s*([\d.]+)', water)[1]))
    rotation = np.array([[math.cos(yaw), -math.sin(yaw)], [math.sin(yaw), math.cos(yaw)]])
    corners = np.array([[-1, -1], [1, -1], [1, 1], [-1, 1]]) * half
    pixels = np.rint((corners @ rotation + center - lower) / STEP).astype(int)
    basin = Image.new('1', tuple(size))
    ImageDraw.Draw(basin).polygon([tuple(p) for p in pixels], fill=1)
    basin = np.array(basin).T
    assert not (basin & (~np.array(masks[0]).T | np.array(masks[1]).T)).any(), 'Basin overlaps original scenery or leaves paving'
    ImageDraw.Draw(masks[1]).polygon([tuple(p) for p in pixels], fill=1)
    return lower, [np.array(m).T for m in masks], digest.hexdigest(), len(floor), len(obstacles), (center, half, rotation)


def curve():
    text = (ROOT / 'Apps/App/src/BistroPeopleLoop.inl').read_text()
    nodes = np.array([[float(v.rstrip('f')) for v in row.split(',')]
                      for row in re.findall(r'\{([^{}]+)\}', text)])
    assert len(nodes) >= 3 and np.isfinite(nodes).all()
    result = []
    # Same 33-point Bezier sampling as BistroRoutes::Network; C++ tests cover wrapping.
    for i, a in enumerate(nodes):
        b = nodes[(i + 1) % len(nodes)]
        length = np.linalg.norm(b - a)
        assert length > 0
        ta, tb = b - nodes[i - 1], a - nodes[(i + 2) % len(nodes)]
        ta *= length / (3 * np.linalg.norm(ta))
        tb *= length / (3 * np.linalg.norm(tb))
        t = np.linspace(0, 1, 33)[:, None]
        u = 1 - t
        result.extend(a*u**3 + 3*(a+ta)*u*u*t + 3*(b+tb)*u*t*t + b*t**3)
    return np.array(result), len(nodes)


def check_loop_shape(points):
    xz = points[:, [0, 2]]
    arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(xz, axis=0), axis=1))]
    samples = np.arange(0, arc[-1], 25)
    a = np.c_[np.interp(samples, arc, xz[:, 0]), np.interp(samples, arc, xz[:, 1])]
    b = np.roll(a, -1, axis=0)
    pairs = cKDTree((a+b)/2).query_pairs(26, output_type='ndarray')
    separation = np.abs(pairs[:, 0]-pairs[:, 1])
    pairs = pairs[(separation > 1) & (separation < len(a)-1)]
    i, j = pairs.T
    cross = lambda x, y: x[:, 0]*y[:, 1]-x[:, 1]*y[:, 0]
    r, s, delta = b[i]-a[i], b[j]-a[j], a[j]-a[i]
    denominator = cross(r, s)
    nonparallel = abs(denominator) > 1e-7
    t = np.divide(cross(delta, s), denominator, out=np.zeros(len(i)), where=nonparallel)
    u = np.divide(cross(delta, r), denominator, out=np.zeros(len(i)), where=nonparallel)
    crossing = nonparallel & (t >= 0) & (t <= 1) & (u >= 0) & (u <= 1)
    collinear = (~nonparallel) & (abs(cross(delta, r)) < 1e-7)
    overlap = (np.maximum(np.minimum(a[i], b[i]), np.minimum(a[j], b[j])) <=
               np.minimum(np.maximum(a[i], b[i]), np.maximum(a[j], b[j])) + 1e-7).all(axis=1)
    assert not (crossing | (collinear & overlap)).any(), 'Loop has self-intersections or shared segments'
    pairs = cKDTree(a).query_pairs(500, output_type='ndarray')
    distance_along = abs(samples[pairs[:, 0]]-samples[pairs[:, 1]])
    distance_along = np.minimum(distance_along, arc[-1]-distance_along)
    pairs = pairs[distance_along > 400]
    distances = np.linalg.norm(a[pairs[:, 0]]-a[pairs[:, 1]], axis=1)
    assert (distances >= 150).all(), 'Separate loop branches share a narrow passage'
    return float(distances.min()) if len(distances) else None


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    lower, (ground, obstacles), source_hash, floors, blocks, (basin_center, basin_half, basin_rotation) = geometry()
    points, anchors = curve()
    branch_spacing = check_loop_shape(points)
    clearance = distance_transform_edt(ground & ~obstacles) * STEP
    shape = ground.shape

    def cells(xz):
        ids = np.rint((xz - lower) / STEP).astype(int)
        assert (ids >= 0).all() and (ids < shape).all(), 'Route left source bounds'
        return ids[:, 0], ids[:, 1]

    center_clearance = clearance[cells(points[:, [0, 2]])]
    assert center_clearance.min() >= 60, f'Route lost its obstacle margin: {points[center_clearance < 60][:5].tolist()}'
    # The sampled Take1 skin at scale 2.4 spans X=-38.50..36.75, Z=-59.12..56.72 cm.
    # Test a larger oriented rectangle, including the space between all its cells.
    delta = np.roll(points, -1, axis=0) - points
    delta = delta[:, [0, 2]]
    # Adjacent edges repeat their shared endpoint; use the next nonzero tangent there.
    lengths = np.linalg.norm(delta, axis=1)
    zero = lengths < 1e-6
    delta[zero] = np.roll(delta, -1, axis=0)[zero]
    delta /= np.linalg.norm(delta, axis=1)[:, None]
    right = np.c_[delta[:, 1], -delta[:, 0]]
    failed = np.zeros(len(points), dtype=bool)
    for side in np.arange(-45, 46, 5):
        for along in np.arange(-65, 66, 5):
            xz = points[:, [0, 2]] + right*side + delta*along
            idx = cells(xz)
            failed |= obstacles[idx] | ~ground[idx]
    assert not failed.any(), f'Actor envelope hits scenery at {points[failed][:5].tolist()}'
    x, z = np.indices(shape)
    local = (np.stack([x*STEP+lower[0], z*STEP+lower[1]], axis=-1)-basin_center) @ basin_rotation.T
    side_clearances = []
    for sign in (-1, 1):
        nearby = ((abs(local[:, :, 0]) < basin_half[0] + 300) &
                  (sign*local[:, :, 1] > 0) & (sign*local[:, :, 1] < basin_half[1] + 500))
        for radius in range(70, 201, 5):
            components, _ = label(nearby & (clearance >= radius))
            start = set(components[nearby & (local[:, :, 0] < -basin_half[0]-150)])
            end = set(components[nearby & (local[:, :, 0] > basin_half[0]+150)])
            if not ((start & end)-{0}):
                break
        else:
            radius += 5
        assert radius > 70, f'No free passage on basin side {sign}'
        side_clearances.append(radius-5)
    length = np.linalg.norm(np.diff(points, axis=0), axis=1).sum()
    report = dict(source_sha256=source_hash, anchors=anchors, samples=len(points),
                  floor_triangles=floors, obstacle_triangles=blocks, raster_step_cm=STEP,
                  minimum_raster_clearance_cm=float(center_clearance.min()),
                  actor_envelope_half_width_cm=45, actor_envelope_half_length_cm=65,
                  envelope_collisions=int(failed.sum()), length_m=float(length/100),
                  basin_bypass_sides=2, basin_side_center_clearance_cm=side_clearances,
                  self_intersections=0, sampled_nonlocal_branch_spacing_cm=branch_spacing,
                  ignored_objects='Small fences, hydrants, bollards, streetlights and street furniture',
                  lap_seconds_at_95_cm_s=float(length/95),
                  limits='Roads and major objects only; small props are deliberately ignored. No dynamic obstacle avoidance or foot IK. Raster check, not an exact continuous collision proof.')
    (OUTPUT / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    rgb = np.zeros((*shape, 3), dtype=np.uint8)
    rgb[:] = [20, 25, 34]
    rgb[ground] = [112, 122, 126]
    rgb[obstacles] = [42, 49, 62]
    image = Image.fromarray(rgb.transpose(1, 0, 2)).transpose(Image.Transpose.ROTATE_180)
    canvas = Image.new('RGB', (image.width + 430, image.height + 110), '#141922')
    canvas.paste(image, (0, 100))
    draw = ImageDraw.Draw(canvas)
    font_path = 'C:/Windows/Fonts/arial.ttf'
    font = ImageFont.truetype(font_path, 20)
    title = ImageFont.truetype(font_path, 30)
    small = ImageFont.truetype(font_path, 17)

    def pixel(point):
        x, z = (point[[0, 2]] - lower) / STEP
        return np.array([shape[0] - 1 - x, shape[1] - 1 - z + 100])

    path = [tuple(pixel(p)) for p in points]
    draw.line(path, fill='#43e4bc', width=4)
    basin_corners = np.array([[-1, -1], [1, -1], [1, 1], [-1, 1]]) * basin_half @ basin_rotation + basin_center
    basin_outline = [tuple(pixel(np.array([x, 0, z]))) for x, z in basin_corners]
    draw.polygon(basin_outline, fill='#285f78', outline='#68c9ed', width=2)
    arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))]
    for distance in np.arange(700, length, 1800):
        i = int(np.searchsorted(arc, distance))
        p = pixel(points[i])
        direction = -delta[i]
        right = np.array([direction[1], -direction[0]])
        draw.polygon([tuple(p+direction*11), tuple(p-direction*8+right*6), tuple(p-direction*8-right*6)], fill='#ffe09a')
    draw.text((30, 25), 'BISTRO / замкнутый маршрут NPC', font=title, fill='white')
    draw.text((30, 65), 'Схема по исходной геометрии сцены · +X влево, +Z вверх', font=small, fill='#aebbcf')
    x = image.width + 20
    lines = ['Движение по кругу', '', 'Один контур, одно направление.', 'Случайных поворотов нет.', '',
             'Группы идут колонной:', 'интервал вдоль пути — 1,1 м.', 'Бассейн повёрнут на 90°.', 'Обходы с обеих сторон.',
             f'Длина: {length/100:.0f} м', f'Круг при 0,95 м/с: {length/95/60:.1f} мин',
             f'Проверено точек: {len(points):,}', 'Пересечений габарита: 0', '',
             'Зелёный — ось маршрута', 'Жёлтые стрелки — направление', 'Серый — покрытие', 'Тёмный — препятствия / здания', '',
             'Только дороги и крупные объекты.', 'Мелкая декорация игнорируется.', 'Нет петель и общих отрезков.', 'Это схема, не кадр из игры.']
    for i, line in enumerate(lines):
        draw.text((x, 125+i*28), line, font=font if i < 14 else small, fill='#e1e8f3')
    draw.line([(x, 910), (x+100, 910)], fill='white', width=3)
    draw.text((x, 925), '10 м', font=small, fill='white')
    destination = ROOT / 'docs/reports/2026-11-08-bistro-people-loop.png'
    canvas.save(destination)
    pool_pixel = pixel(np.array([basin_center[0], 0, basin_center[1]]))
    detail = canvas.crop((int(pool_pixel[0]-170), int(pool_pixel[1]-200),
                          int(pool_pixel[0]+190), int(pool_pixel[1]+220))).resize((720, 840))
    detail.save(ROOT / 'docs/reports/2026-11-08-bistro-pool-passages.png')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
