-- spawner.lua — spawns short-lived child markers on a timer and announces them via events.
properties = {
    interval = 1.0,   -- seconds between spawns
    lifetime = 3.0,   -- seconds before a spawned marker is destroyed
    spread   = 2.0,
}

function on_start(self)
    self.spawned = 0
    self.timer = timer.every(self.interval, function()
        self.spawned = self.spawned + 1
        local marker = world.spawn("Marker " .. self.spawned, self.entity)
        marker:set_position(Vec3((math.random() - 0.5) * self.spread, 0, (math.random() - 0.5) * self.spread))
        events.emit("marker_spawned", { marker = marker, index = self.spawned })
        timer.after(self.lifetime, function()
            if marker:valid() then marker:destroy() end
        end)
    end)
end

function on_destroy(self)
    log.info("spawner", self.entity:name(), "made", self.spawned, "markers")
end
