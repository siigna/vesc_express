-- End-to-end fixture: packed by tools/luapack.py, parsed by script_pack.c,
-- executed by script_lua.c. Exercises a direct require, a dotted module name
-- and a transitive require.
local mod = require("mod")
local sub = require("pkg.sub")
assert(mod.val == 7, "mod did not load")
assert(sub.doubled == 14, "transitive require did not load")
print("packed ok", mod.val + sub.doubled)
