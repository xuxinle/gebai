
import bpy
import bmesh
import math
from mathutils import Vector, Euler

mat_a = bpy.data.materials.new('upper')
mat_a.diffuse_color = (0.90, 0.12, 0.10, 1.0)   # 红
mat_a.metallic = 0.0
mat_a.roughness = 0.45

mat_b = bpy.data.materials.new('lower')
mat_b.diffuse_color = (0.10, 0.25, 0.95, 1.0)   # 蓝
mat_b.metallic = 0.0
mat_b.roughness = 0.45

mesh = bpy.data.meshes.new('two_tone')
bm = bmesh.new()
h = 1.0
pts = [(-h, -h, -h), (h, -h, -h), (h, h, -h), (-h, h, -h),
       (-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h)]
v = [bm.verts.new(p) for p in pts]
# 6 个面：前 3 个给槽 0，后 3 个给槽 1
bm.faces.new((v[4], v[5], v[6], v[7]))   # 顶
bm.faces.new((v[0], v[1], v[5], v[4]))   # 前
bm.faces.new((v[1], v[2], v[6], v[5]))   # 右
bm.faces.new((v[3], v[2], v[1], v[0]))   # 底
bm.faces.new((v[2], v[3], v[7], v[6]))   # 后
bm.faces.new((v[3], v[0], v[4], v[7]))   # 左
bm.verts.index_update()
# 按 bpy 的法：faces 顺序与 polygons 一致，逐个设 material_index
idx = 0
for face in bm.faces:
    face_index = idx
    idx += 1
bm.to_mesh(mesh)
bm.free()

mesh.polygons[0].material_index = 0
mesh.polygons[1].material_index = 0
mesh.polygons[2].material_index = 0
mesh.polygons[3].material_index = 1
mesh.polygons[4].material_index = 1
mesh.polygons[5].material_index = 1

obj = bpy.data.objects.new('two_tone', mesh)
obj.material_slots.append(mat_a)
obj.material_slots.append(mat_b)
obj.location = Vector((0.0, 0.0, 0.0))
obj.rotation_euler = Euler(0.0, 0.0, 0.0)

cam_data = bpy.data.cameras.new('cam')
cam_data.lens = 55.0
cam_obj = bpy.data.objects.new('cam', cam_data)
cam_obj.location = Vector((0.0, -6.5, 0.0))
cam_obj.rotation_euler = Euler(1.5708, 0.0, 0.0)

lamp = bpy.data.lights.new('key')
lamp.type = 'SUN'
lamp.energy = 2200.0
lamp_obj = bpy.data.objects.new('key', lamp)
lamp_obj.location = Vector((3.0, -4.0, 5.0))
lamp_obj.rotation_euler = Euler(0.7, 0.2, -0.5)

result = obj
