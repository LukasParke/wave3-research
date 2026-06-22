-- WirePlumber script for the Elgato Wave:3
-- Installs to ~/.local/share/wireplumber/scripts/
--
-- Creates:
--   * wave3-null-sink  - keeps the Wave:3 microphone source awake
--   * wave3-sink       - playback sink for the Wave:3 headphone output
--
-- Inspired by the Undertone project (https://github.com/polariscli/Undertone)
-- which proved this topology on Linux.

log = Log.open_topic("s-elgato_wave3")

-- Object managers
sourceOM = ObjectManager({
	Interest({
		type = "node",
		Constraint({ "node.name", "matches", "wave3-source" }),
	}),
})

linkOM = ObjectManager({ Interest({ type = "link" }) })
deviceOM = ObjectManager({ Interest({ type = "device" }) })
portOM = ObjectManager({ Interest({ type = "port" }) })

-- Globals
waveSinkNode = nil
nullSinkNode = nil
waveLink = nil

-- Best-effort start of the wave3-daemon user service.
-- WirePlumber runs as the user, so systemctl --user is safe.
local function start_daemon()
	local handle = io.popen("systemctl --user is-active wave3-daemon 2>/dev/null")
	if handle then
		local status = handle:read("*l")
		handle:close()
		if status ~= "active" then
			log:notice("Starting wave3-daemon service")
			os.execute("systemctl --user start wave3-daemon >/dev/null 2>&1")
		end
	end
end

local function create_null_sink()
	local props = {
		["factory.name"] = "support.null-audio-sink",
		["node.name"] = "wave3-null-sink",
		["node.description"] = "Wave:3 Null Sink (internal)",
		["node.nick"] = "Wave:3 Keepalive",
		["media.class"] = "Audio/Sink",
		["audio.channels"] = "1",
		["audio.position"] = "MONO",
		["audio.rate"] = "48000",
		["monitor.channel-volumes"] = "true",
		["monitor.passthrough"] = "true",
		["node.passive"] = "false",
		["node.pause-on-idle"] = "false",
		["session.suspend-timeout-seconds"] = "0",
	}

	log:notice("Creating Wave:3 null sink")
	local node = Node("adapter", props)
	node:activate(Feature.Proxy.BOUND, function(n, err)
		if err then
			log:warning("Failed to create null sink: " .. tostring(err))
			nullSinkNode = nil
		else
			log:notice("Created Wave:3 null sink, id=" .. n.properties["object.id"])
		end
	end)

	return node
end

local function create_wave_sink(source)
	local devInterest = Interest({
		type = "device",
		Constraint({ "object.id", "equals", source.properties["device.id"] }),
	})

	for dev in deviceOM:iterate(devInterest) do
		local props = {
			["device.id"] = source.properties["device.id"],
			["factory.name"] = "api.alsa.pcm.sink",
			["node.name"] = "wave3-sink",
			["node.description"] = "Wave:3 Headphones",
			["node.nick"] = "Wave:3 HP",
			["media.class"] = "Audio/Sink",
			["api.alsa.path"] = source.properties["api.alsa.path"],
			["api.alsa.pcm.card"] = source.properties["api.alsa.pcm.card"],
			["api.alsa.pcm.stream"] = "playback",
			["alsa.resolution_bits"] = "24",
			["audio.channels"] = "2",
			["audio.position"] = "FL,FR",
			["audio.rate"] = "48000",
			["priority.driver"] = "1000",
			["priority.session"] = "1000",
			["node.pause-on-idle"] = "false",
			["node.autoconnect"] = "false",
			["session.suspend-timeout-seconds"] = "0",
		}

		for k, v in pairs(dev.properties) do
			if k:find("^api%.alsa%.card%..*") then
				props[k] = v
			end
		end

		log:notice("Creating Wave:3 playback sink for " .. source.properties["api.alsa.path"])
		waveSinkNode = Node("adapter", props)
		waveSinkNode:activate(Feature.Proxy.BOUND, function(n, err)
			if err then
				log:warning("Failed to create Wave:3 sink: " .. tostring(err))
				waveSinkNode = nil
			else
				log:notice("Created Wave:3 sink, id=" .. n.properties["object.id"])
			end
		end)
	end
end

local function try_link_source_to_null(source)
	if waveLink or not nullSinkNode then
		return
	end

	local outPort, inPort
	local outInterest = Interest({
		type = "port",
		Constraint({ "node.id", "equals", source.properties["object.id"] }),
		Constraint({ "port.direction", "equals", "out" }),
	})
	local inInterest = Interest({
		type = "port",
		Constraint({ "node.id", "equals", nullSinkNode.properties["object.id"] }),
		Constraint({ "port.direction", "equals", "in" }),
	})

	for p in portOM:iterate(outInterest) do
		outPort = p
	end
	for p in portOM:iterate(inInterest) do
		inPort = p
	end

	if not (outPort and inPort) then
		return
	end

	local args = {
		["link.input.node"] = nullSinkNode.properties["object.id"],
		["link.input.port"] = inPort.properties["object.id"],
		["link.output.node"] = source.properties["object.id"],
		["link.output.port"] = outPort.properties["object.id"],
	}

	log:notice("Linking Wave:3 source -> null sink")
	waveLink = Link("link-factory", args)
	waveLink:activate(Feature.Proxy.BOUND, function(n, err)
		if err then
			log:warning("Failed to link Wave:3 source: " .. tostring(err))
			waveLink = nil
		else
			log:notice("Created link Wave:3 source -> null sink")
		end
	end)
end

local function on_source_added(_, node)
	start_daemon()
	try_link_source_to_null(node)
end

local function on_source_removed()
	if waveSinkNode then
		log:notice("Removing Wave:3 sink")
		waveSinkNode:request_destroy()
		waveSinkNode = nil
	end
	if waveLink then
		log:notice("Removing Wave:3 source link")
		waveLink:request_destroy()
		waveLink = nil
	end
end

local function on_link_added(_, link)
	if waveLink and link.properties["object.id"] == waveLink.properties["object.id"] then
		for node in sourceOM:iterate() do
			create_wave_sink(node)
		end
	end
end

local function on_port_added(_, port)
	if not waveLink then
		for node in sourceOM:iterate() do
			try_link_source_to_null(node)
		end
	end
end

-- Activation
nullSinkNode = create_null_sink()
portOM:activate()
portOM:connect("object-added", on_port_added)
deviceOM:activate()
linkOM:activate()
linkOM:connect("object-added", on_link_added)
sourceOM:connect("object-added", on_source_added)
sourceOM:connect("object-removed", on_source_removed)
sourceOM:activate()

log:notice("Elgato Wave:3 WirePlumber script initialized")
