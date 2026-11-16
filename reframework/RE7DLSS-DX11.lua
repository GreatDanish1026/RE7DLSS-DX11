-- RE7DLSS-DX11 companion script (REFramework autorun; REFramework's TDB49 build for the
-- dx11_non-rt branch). Sets Resident Evil 7's Image Quality (render scale) to the render
-- resolution chosen in the RE7DLSS-DX11 ReShade overlay. The add-on writes the wanted scale
-- (0 = leave the game's slider alone) to reframework/data/RE7DLSS-DX11_mode.txt; this script applies
-- it, keeps it applied if the menu changes it, and puts the game's own value back when the mode
-- goes back to the slider or upscaling is switched off. It writes the current value to
-- reframework/data/RE7DLSS-DX11_status.txt for the overlay.
--
-- In this build via.render.RenderConfig is not a native singleton (null); the scale is on the
-- via.render.Renderer singleton (get/set_ImageQualityRate), found by a probe 2026-11-16.

local r_type = sdk.find_type_definition("via.render.Renderer")
local original = nil  -- the game's value before the first override
local frame = 0

local function get_rate(r)
    return sdk.call_native_func(r, r_type, "get_ImageQualityRate")
end

local function set_rate(r, v)
    sdk.call_native_func(r, r_type, "set_ImageQualityRate", v)
end

re.on_frame(function()
    frame = frame + 1
    if frame % 30 ~= 0 then return end
    if not r_type then
        fs.write("RE7DLSS-DX11_status.txt", "error via.render.Renderer not found")
        return
    end
    local ok, err = pcall(function()
        local r = sdk.get_native_singleton("via.render.Renderer")
        if not r then
            fs.write("RE7DLSS-DX11_status.txt", "error via.render.Renderer singleton not found")
            return
        end
        local want = tonumber(fs.read("RE7DLSS-DX11_mode.txt") or "") or 0
        local cur = get_rate(r)
        if type(cur) ~= "number" or cur <= 0.01 then
            fs.write("RE7DLSS-DX11_status.txt", "error the game's render scale reads " .. tostring(cur))
            return
        end
        if want > 0 then
            if original == nil then original = cur end
            if math.abs(cur - want) > 0.001 then
                set_rate(r, want)
                cur = get_rate(r)
            end
        elseif original ~= nil then
            set_rate(r, original)
            original = nil
            cur = get_rate(r)
        end
        fs.write("RE7DLSS-DX11_status.txt", string.format("%.4f %d", cur or -1, frame))
    end)
    if not ok then fs.write("RE7DLSS-DX11_status.txt", "error " .. tostring(err)) end
end)
