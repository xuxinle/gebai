
import bpy
bpy.ops.mesh.primitive_uv_sphere_add(radius=0.75, segments=16, ring_count=10, location=(0,0,0))
o = bpy.context.active_object
vs = o.data.vertices
xs = []; ys = []; zs = []
for v in vs:
    xs.append(v.co.x); ys.append(v.co.y); zs.append(v.co.z)
result = 'x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f]' % (min(xs),max(xs),min(ys),max(ys),min(zs),max(zs))
