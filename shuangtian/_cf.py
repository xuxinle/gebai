
import bpy
from mathutils import Vector, Euler
cd = bpy.data.cameras.new('c')
cd.lens = 55.0
co = bpy.data.objects.new('c', cd)
co.location = Vector((0,-8,0))
co.rotation_euler = Euler(1.5708, 0.0, 0.0)
bpy.context.scene.camera = co
result = 'fov=%.2f lens=%.1f sensor=%.1f' % (cd.angle * 57.2958, cd.lens, cd.sensor_width)
