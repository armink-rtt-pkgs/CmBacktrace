from building import *

cwd     = GetCurrentDir()
src     = Glob('*.c')
path    = [cwd]

group = DefineGroup('cm_backtrace', src, depend = [''], CPPPATH = path)

Return('group')
