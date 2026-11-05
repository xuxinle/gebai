# ============================================================================
# bpy.ops 命名空间（把生成器装上去）+ bmesh
# ============================================================================
class _MeshOps:
    """`bpy.ops.mesh.*`（基本体）"""

    @staticmethod
    def primitive_cube_add(size=2.0, location=(0.0, 0.0, 0.0), **kwargs):
        verts, faces = _make_cube(size)
        obj = _mesh_object('Cube', verts, faces)
        obj.location = Vector(location)
        return None

    @staticmethod
    def primitive_cylinder_add(vertices=32, radius=1.0, depth=2.0,
                               location=(0.0, 0.0, 0.0), **kwargs):
        verts, faces = _make_cylinder(vertices, radius, depth)
        obj = _mesh_object('Cylinder', verts, faces)
        obj.location = Vector(location)
        return None

    @staticmethod
    def primitive_cone_add(vertices=32, radius1=1.0, radius2=0.0, depth=2.0,
                           location=(0.0, 0.0, 0.0), **kwargs):
        verts, faces = _make_cone(vertices, radius1, radius2, depth)
        obj = _mesh_object('Cone', verts, faces)
        obj.location = Vector(location)
        return None

    @staticmethod
    def primitive_uv_sphere_add(segments=32, ring_count=16, radius=1.0,
                                location=(0.0, 0.0, 0.0), **kwargs):
        verts, faces = _make_uv_sphere(segments, ring_count, radius)
        obj = _mesh_object('Sphere', verts, faces)
        obj.location = Vector(location)
        return None

    @staticmethod
    def primitive_plane_add(size=2.0, location=(0.0, 0.0, 0.0), **kwargs):
        verts, faces = _make_plane(size)
        obj = _mesh_object('Plane', verts, faces)
        obj.location = Vector(location)
        return None

    @staticmethod
    def primitive_torus_add(major_radius=1.25, minor_radius=0.25,
                            major_segments=48, minor_segments=12,
                            location=(0.0, 0.0, 0.0), **kwargs):
        verts, faces = _make_torus(major_radius, minor_radius,
                                   major_segments, minor_segments)
        obj = _mesh_object('Torus', verts, faces)
        obj.location = Vector(location)
        return None

    @staticmethod
    def primitive_grid_add(x_subdivisions=10, y_subdivisions=10, size=2.0,
                           location=(0.0, 0.0, 0.0), **kwargs):
        verts, faces = _make_grid(x_subdivisions, y_subdivisions, size)
        obj = _mesh_object('Grid', verts, faces)
        obj.location = Vector(location)
        return None


class _ObjectOps:
    """`bpy.ops.object.*`（选择 / 删除 / 变换）"""

    @staticmethod
    def select_all(action='SELECT'):
        if action == 'SELECT':
            _context.selected_objects = list(_data.objects)
        elif action == 'DESELECT':
            _context.selected_objects = []
        else:
            # bpy 的 'TOGGLE' / 'INVERT'：本子集支持 INVERT
            if action == 'INVERT':
                chosen = [o for o in _data.objects if o not in _context.selected_objects]
                _context.selected_objects = chosen
            else:
                raise ValueError('select_all 的 action 只支持 SELECT / DESELECT / INVERT')
        return None

    @staticmethod
    def delete(use_global=False):
        """删除选中的对象。

        ⚠ bpy 的 `use_global` 语义是"连带删除没被引用的数据块"，
        而本框架不做引用计数（mesh 可以共享），所以忽略它。
        """
        for obj in list(_context.selected_objects):
            if obj in _data.objects:
                _data.objects.remove(obj)
        _context._set_active(None)
        return None

    @staticmethod
    def origin_set(type='ORIGIN_GEOMETRY', center='MEDIAN'):
        """把原点移到几何中心。

        ⚠ 真实 bpy 会改所有顶点坐标并调整 object.location。这里同样做：
        先算几何中心，再把顶点平移到以中心为原点、location 加上偏移。
        这样"移动物体"和"移动顶点"的语义都跟 Blender 一致。
        """
        obj = _context.active_object
        if obj is None or obj.data is None:
            return None
        verts = obj.data.vertices
        if not verts:
            return None
        n = float(len(verts))
        cx = sum(v.co.x for v in verts) / n
        cy = sum(v.co.y for v in verts) / n
        cz = sum(v.co.z for v in verts) / n
        for v in verts:
            v.co.x -= cx
            v.co.y -= cy
            v.co.z -= cz
        obj.location = obj.location + Vector((cx, cy, cz))
        return None


class _DataOps:
    """本框架扩展（不是 bpy 标准）：批量复制对象。

    放在 `bpy.ops.st` 下而不是污染 `bpy.ops.object`——后者是 bpy 的
    命名空间，往里加非标准操作会让"照抄 bpy 脚本"的人困惑。
    """

    @staticmethod
    def duplicate(name, count, transform=None):
        """把 active_object 复制 `count` 份，每份可选施加变换。"""
        src = _context.active_object
        if src is None:
            raise RuntimeError('没有 active_object')
        out = []
        for i in range(count):
            obj = _data.objects.new('%s.%03d' % (name, i), src.data)
            obj.location = src.location.copy()
            obj.rotation_euler = Euler(src.rotation_euler.x, src.rotation_euler.y,
                                       src.rotation_euler.z, src.rotation_euler.order)
            obj.scale = src.scale.copy()
            obj.material_slots = list(src.material_slots)
            if transform is not None:
                transform(obj, i)
            out.append(obj)
        return out


class _Ops:
    mesh = _MeshOps
    object = _ObjectOps
    st = _DataOps


# ============================================================================
# bmesh —— 低层网格编辑
#
# ## 为什么需要它
#
# `bpy.ops.mesh.primitive_*` 只给标准形体。真实资产（齿轮、涡轮叶片）
# 需要逐顶点构建，而 bpy 语料里那部分几乎全是 bmesh：
#
#     bm = bmesh.new()
#     v0 = bm.verts.new((0, 0, 0))
#     v1 = bm.verts.new((1, 0, 0))
#     bm.faces.new((v0, v1, v2))
#     bm.to_mesh(mesh)
#     bm.free()
#
# 少了它，模型只能拼基本体组合，做不出齿轮齿形那种东西。
# ============================================================================
class BMVert:
    __slots__ = ('co', 'index')

    def __init__(self, co, index):
        self.co = Vector(co)
        self.index = index

    def __repr__(self):
        return 'BMVert(%r)' % (self.co,)


class BMFace:
    __slots__ = ('verts', 'index')

    def __init__(self, verts, index):
        self.verts = list(verts)
        self.index = index

    def __repr__(self):
        return 'BMFace(%d verts)' % len(self.verts)


class _BMVertSeq:
    """`bm.verts`：`new(co)` 加顶点、可遍历、可 `len()`、可下标。"""

    def __init__(self):
        self._items = []
        self._ensure_lookup_table = False

    def new(self, co):
        v = BMVert(co, len(self._items))
        self._items.append(v)
        return v

    def __len__(self):
        return len(self._items)

    def __iter__(self):
        return iter(self._items)

    def __getitem__(self, i):
        return self._items[i]

    def ensure_lookup_table(self):
        """bpy 要求在建面后调它才能按下标访问。

        本实现里下标一直可用（内部就是列表），保留空函数是为了让真实
        bpy 脚本能直接跑——**不实现它会让大量照抄的脚本报 AttributeError**。
        """
        self._ensure_lookup_table = True

    def index_update(self):
        for i, v in enumerate(self._items):
            v.index = i


class _BMFaceSeq:
    def __init__(self):
        self._items = []

    def new(self, verts):
        verts = list(verts)
        if len(verts) < 3:
            raise ValueError('面至少需要 3 个顶点')
        # bpy 会拒绝重复面；这里也拒绝，因为重复面会让三角化出错
        key = tuple(sorted(v.index for v in verts))
        for f in self._items:
            if tuple(sorted(v.index for v in f.verts)) == key:
                raise ValueError('已存在相同的面')
        f = BMFace(verts, len(self._items))
        self._items.append(f)
        return f

    def __len__(self):
        return len(self._items)

    def __iter__(self):
        return iter(self._items)

    def __getitem__(self, i):
        return self._items[i]

    def ensure_lookup_table(self):
        pass


class BMesh:
    """`bmesh.new()` 的返回类型。"""

    def __init__(self):
        self.verts = _BMVertSeq()
        self.faces = _BMFaceSeq()

    def to_mesh(self, mesh):
        """把 bmesh 内容写进一个 Mesh（会先清空它）。"""
        mesh.clear_geometry()
        for v in self.verts:
            mesh.vertices.append(MeshVertex(v.co.x, v.co.y, v.co.z))
        for f in self.faces:
            mesh.polygons.append(MeshPolygon([v.index for v in f.verts]))

    def from_mesh(self, mesh):
        """从 Mesh 读入（顶点 + 面）。"""
        self.verts = _BMVertSeq()
        self.faces = _BMFaceSeq()
        for v in mesh.vertices:
            self.verts.new((v.co.x, v.co.y, v.co.z))
        self.verts.index_update()
        for poly in mesh.polygons:
            self.faces.new([self.verts[i] for i in poly.vertices])

    def free(self):
        """释放（本实现无需动作，保留以兼容 bpy 脚本）。"""
        pass
