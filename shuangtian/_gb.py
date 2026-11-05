
import bpy
from mathutils import Euler, Vector
bpy.context.scene.world.background_color = (0.05, 0.05, 0.05)
m = bpy.data.materials.new('white')
m.diffuse_color = (1.0, 1.0, 1.0, 1.0)
m.metallic = 0.0
m.roughness = 1.0
bpy.ops.mesh.primitive_cube_add(size=1.8, location=(0,0,0))
o = bpy.context.active_object
o.material_slots.append(m)
cd = bpy.data.cameras.new('c'); cd.lens = 55.0
co = bpy.data.objects.new('c', cd); co.location = Vector((0,-8,0))
co.rotation_euler = Euler(1.5708, 0.0, 0.0)
bpy.context.scene.camera = co
l = bpy.data.lights.new('k'); l.type='SUN'; l.energy=2600.0
lo = bpy.data.objects.new('k', l); lo.location = (0,-3,0)
lo.rotation_euler = Euler(1.5708, 0.0, 0.0)
result = 'ok'
