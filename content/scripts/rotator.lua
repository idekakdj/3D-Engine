-- rotator.lua — spins its entity around an axis.
properties = {
    speed = 90,              -- degrees per second
    axis  = Vec3(0, 1, 0),
}

function on_update(self, dt)
    self.entity:rotate(Quat.angle_axis(math.rad(self.speed) * dt, self.axis))
end
