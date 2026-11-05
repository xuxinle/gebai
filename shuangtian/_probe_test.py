import bpy
bpy.ops.mesh.primitive_cube_add(size=1.0)
o=bpy.context.active_object
o.location=(1,2,3)
result=1
