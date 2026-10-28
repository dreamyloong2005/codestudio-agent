"""SDK macOS 26.5 sys/proc_info.h layouts; readonly exploratory helper."""
import ctypes
class RegionInfo(ctypes.Structure):
    _fields_=[(name,ctypes.c_uint32) for name in ('protection','max_protection','inheritance','flags')]
    _fields_ += [('offset',ctypes.c_uint64)]
    _fields_ += [(name,ctypes.c_uint32) for name in ('behavior','user_wired_count','user_tag','pages_resident','pages_shared_now_private','pages_swapped_out','pages_dirtied','ref_count','shadow_depth','share_mode','private_pages_resident','shared_pages_resident','obj_id','depth')]
    _fields_ += [('address',ctypes.c_uint64),('size',ctypes.c_uint64)]
class VinfoStat(ctypes.Structure):
    _fields_=[('dev',ctypes.c_uint32),('mode',ctypes.c_uint16),('nlink',ctypes.c_uint16),('ino',ctypes.c_uint64),('uid',ctypes.c_uint32),('gid',ctypes.c_uint32)]
    _fields_ += [(name,ctypes.c_int64) for name in ('atime','atimensec','mtime','mtimensec','ctime','ctimensec','birthtime','birthtimensec','size','blocks')]
    _fields_ += [('blksize',ctypes.c_int32),('flags',ctypes.c_uint32),('gen',ctypes.c_uint32),('rdev',ctypes.c_uint32),('qspare',ctypes.c_int64*2)]
class VnodeInfo(ctypes.Structure):
    _fields_=[('stat',VinfoStat),('type',ctypes.c_int),('pad',ctypes.c_int),('fsid',ctypes.c_int32*2)]
class VnodePath(ctypes.Structure):
    _fields_=[('vnode',VnodeInfo),('path',ctypes.c_char*1024)]
class RegionPath(ctypes.Structure):
    _fields_=[('region',RegionInfo),('vnodepath',VnodePath)]
