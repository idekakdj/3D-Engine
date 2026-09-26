-- slice/launcher.lua — kicks its (dynamic) rigid body upwards every `interval` seconds, or when
-- the Space key is pressed. Uses the `physics` module registered by the gameplay layer.
properties = {
    interval = 3.0,
    impulse  = 6.0,
}

function on_start(self)
    self.launches = 0
    self.timer = timer.every(self.interval, function() self:launch() end)
    self.contacts = 0
    events.subscribe("physics.contact_begin", function(c)
        if c.a == self.entity or c.b == self.entity then
            self.contacts = self.contacts + 1
        end
    end)
end

function launch(self)
    physics.add_impulse(self.entity, Vec3(0, self.impulse, 0))
    self.launches = self.launches + 1
end

function on_update(self, dt)
    if input.key_pressed("space") then self:launch() end
end
