import bpy
s = bpy.context.scene
names = []
for n in dir(s):
    names.append(n)
result = ','.join(names)
