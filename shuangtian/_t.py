import bpy
m = bpy.data.meshes.new("m")
o = bpy.data.objects.new("o", m)
bpy.context.collection.objects.link(o)
result = bpy.context.active_object.name
