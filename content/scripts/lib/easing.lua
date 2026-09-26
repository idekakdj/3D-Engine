-- lib/easing.lua — easing curves on t in [0, 1] (require("lib.easing")).
local M = {}

function M.linear(t) return t end
function M.in_quad(t) return t * t end
function M.out_quad(t) return t * (2 - t) end
function M.in_out_quad(t)
    if t < 0.5 then return 2 * t * t end
    return -1 + (4 - 2 * t) * t
end
function M.in_out_sine(t) return -(math.cos(math.pi * t) - 1) / 2 end

return M
