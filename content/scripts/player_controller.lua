-- player_controller.lua — WASD + Space/Ctrl movement relative to the entity's facing.
properties = {
    speed      = 5.0,    -- metres per second
    turn_speed = 120.0,  -- degrees per second (Q/E)
    sprint     = 2.0,    -- multiplier while Shift is held
}

function on_update(self, dt)
    if not input.available() then return end
    local e = self.entity
    local move = e:forward() * input.axis("w", "s")
               + e:right() * input.axis("d", "a")
               + Vec3.up() * input.axis("space", "ctrl")
    if move:length2() > 0 then
        local speed = self.speed * (input.key_down("shift") and self.sprint or 1)
        e:set_world_position(e:world_position() + move:normalized() * speed * dt)
    end
    local turn = input.axis("q", "e")
    if turn ~= 0 then
        e:rotate(Quat.angle_axis(math.rad(self.turn_speed * turn) * dt, Vec3.up()))
    end
end
