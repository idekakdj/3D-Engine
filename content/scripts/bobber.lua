-- bobber.lua — floats its entity up and down around its starting position.
local easing = require("lib.easing")

properties = {
    height = 0.5,   -- metres
    period = 2.0,   -- seconds per cycle
}

function on_start(self)
    self.origin = self.entity:position()
    self.phase = 0
end

function on_update(self, dt)
    self.phase = (self.phase + dt / self.period) % 1
    local offset = (easing.in_out_sine(self.phase < 0.5 and self.phase * 2 or 2 - self.phase * 2) - 0.5) * 2
    self.entity:set_position(self.origin + Vec3(0, offset * self.height, 0))
end

function on_reload(self)
    self.phase = 0
end
