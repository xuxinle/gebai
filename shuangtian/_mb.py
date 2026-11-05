
import bpy
from mathutils import Euler, Vector
bpy.context.scene.world.background_color = (0.05, 0.05, 0.05)
m = bpy.data.materials.new('brass')
m.diffuse_color = (0.72, 0.55, 0.24, 1.0)
m.metallic = 0.95
m.roughness = 0.22
bpy.ops.mesh.primitive_cube_add(size=1.8, location=(0,0,0))
o = bpy.context.active_object
o.rotation_euler = Euler(0.42, 0.62, 0.0)
o.material_slots.append(m)
cd = bpy.data.cameras.new('c'); cd.lens = 55.0
co = bpy.data.objects.new('c', cd); co.location = Vector((0,-6,1.5))
co.rotation_euler = Euler(1.35, 0.0, 0.0)
bpy.context.scene.camera = co
l = bpy.data.lights.new('k'); l.type='SUN'; l.energy=2600.0
lo = bpy.data.objects.new('k', l); lo.location = (3,4,6)
lo.rotation_euler = Euler(0.7, 0.3, -0.5)
result = 'ok'
