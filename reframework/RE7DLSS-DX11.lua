-- RE7DLSS-DX11 companion script (REFramework autorun; REFramework's TDB49 build for the
-- dx11_non-rt branch). Sets Resident Evil 7's Image Quality (render scale) to the render
-- resolution chosen in the RE7DLSS-DX11 ReShade overlay. The add-on writes the wanted scale
-- (0 = leave the game's slider alone) to reframework/data/RE7DLSS-DX11_mode.txt; this script applies
-- it through via.render.RenderConfig.ImageQualityRate, keeps it applied if the menu changes it, and
-- puts the game's own value back when the mode goes back to the slider or upscaling is switched off.
-- It writes the current value to reframework/data/RE7DLSS-DX11_status.txt for the overlay.

local rc_type = sdk.find_type_definition("via.render.RenderConfig")
local original = nil  -- the game's value before the first override
local frame = 0
local last_set = nil  -- the scale set while the getter can't be read

local function get_rate(rc)
    return sdk.call_native_func(rc, rc_type, "get_ImageQualityRate")
end

local function set_rate(rc, v)
    sdk.call_native_func(rc, rc_type, "set_ImageQualityRate", v)
end

-- One-off probe (0.1.0): in the TDB49 build get_ImageQualityRate read 0 and set_ImageQualityRate
-- through call_native_func changed nothing. Lists RenderConfig's quality-related members and tries
-- the method objects directly; written to reframework/data/RE7DLSS-DX11_probe.txt.
local probed = false
local function probe()
    probed = true
    local out = {}
    local function add(s) out[#out + 1] = s end
    local ok, err = pcall(function()
        add("RenderConfig type: " .. tostring(rc_type and rc_type:get_full_name()))
        local rc = sdk.get_native_singleton("via.render.RenderConfig")
        add("singleton: " .. tostring(rc))
        if not rc_type then return end
        for _, m in ipairs(rc_type:get_methods()) do
            local n = m:get_name()
            if n:find("Quality") or n:find("Rate") or n:find("Resolution") or n:find("Scale") then
                local rt = m:get_return_type()
                add(string.format("method %s -> %s (%d params)", n, rt and rt:get_full_name() or "?", m:get_num_params()))
            end
        end
        for _, f in ipairs(rc_type:get_fields()) do
            local n = f:get_name()
            if n:find("Quality") or n:find("Rate") or n:find("Resolution") or n:find("Scale") then
                add(string.format("field %s : %s offset %d", n, f:get_type():get_full_name(), f:get_offset_from_base()))
            end
        end
        -- 0.1.1: the native singleton was null in RE7. Other ways to the RenderConfig object.
        add("managed singleton RenderConfig: " .. tostring(sdk.get_managed_singleton("via.render.RenderConfig")))
        local renderer = sdk.get_native_singleton("via.render.Renderer")
        add("native singleton Renderer: " .. tostring(renderer))
        local rt = sdk.find_type_definition("via.render.Renderer")
        if rt then
            for _, m in ipairs(rt:get_methods()) do
                local n = m:get_name()
                if n:find("Config") or n:find("Quality") or n:find("Rate") or n:find("Resolution") then
                    add("Renderer method " .. n .. " -> " .. tostring(m:get_return_type() and m:get_return_type():get_full_name()))
                end
            end
        end
        local tdb = sdk.get_tdb and sdk.get_tdb()
        if tdb then
            local count = 0
            for i = 0, tdb:get_num_types() - 1 do
                local t = tdb:get_type(i)
                local tn = t and t:get_full_name() or ""
                if tn:find("RenderConfig") or tn:find("GraphicsOption") or tn:find("GraphicOption") or
                   (tn:find("^app%.") and (tn:find("Option") or tn:find("Config"))) then
                    for _, m in ipairs(t:get_methods() or {}) do
                        local n = m:get_name()
                        if n:find("ImageQuality") or n:find("RenderScale") or n:find("Resolution") then
                            add("type " .. tn .. " method " .. n)
                            count = count + 1
                        end
                    end
                    add("type " .. tn .. (sdk.get_managed_singleton(tn) and " (managed singleton)" or "") ..
                        (sdk.get_native_singleton(tn) and " (native singleton)" or ""))
                end
                if count > 80 then break end
            end
        else
            add("no sdk.get_tdb")
        end
        local getm = rc_type:get_method("get_ImageQualityRate")
        local setm = rc_type:get_method("set_ImageQualityRate")
        add("get method: " .. tostring(getm) .. ", set method: " .. tostring(setm))
        if getm and rc then
            add("get via method:call -> " .. tostring(getm:call(rc)))
            add("get via call_native_func -> " .. tostring(sdk.call_native_func(rc, rc_type, "get_ImageQualityRate")))
        end
    end)
    if not ok then add("error " .. tostring(err)) end
    fs.write("RE7DLSS-DX11_probe.txt", table.concat(out, "\n"))
end

local function set_rate_method(rc, v)
    local m = rc_type:get_method("set_ImageQualityRate")
    if m then m:call(rc, v) end
end

re.on_frame(function()
    frame = frame + 1
    if not probed and frame > 120 then probe() end
    if frame % 30 ~= 0 then return end
    if not rc_type then
        fs.write("RE7DLSS-DX11_status.txt", "error via.render.RenderConfig not found")
        return
    end
    local ok, err = pcall(function()
        local rc = sdk.get_native_singleton("via.render.RenderConfig")
        if not rc then return end
        local want = tonumber(fs.read("RE7DLSS-DX11_mode.txt") or "") or 0
        local cur = get_rate(rc)
        -- TDB49 build (2026-11-16): the getter returned 0 here; never take 0 as the game's value.
        local known = type(cur) == "number" and cur > 0.01
        if want > 0 then
            if original == nil and known then original = cur end
            if not known then
                if last_set ~= want then
                    set_rate(rc, want)
                    pcall(set_rate_method, rc, want)  -- (probe: the method object directly)
                    last_set = want
                end
                fs.write("RE7DLSS-DX11_status.txt", string.format("%.4f %d set", want, frame))
                return
            end
            if math.abs(cur - want) > 0.001 then
                set_rate(rc, want)
                cur = get_rate(rc)
            end
        elseif original ~= nil then
            set_rate(rc, original)
            original = nil
            cur = get_rate(rc)
        end
        last_set = nil
        fs.write("RE7DLSS-DX11_status.txt", string.format("%.4f %d", cur or -1, frame))
    end)
    if not ok then fs.write("RE7DLSS-DX11_status.txt", "error " .. tostring(err)) end
end)
