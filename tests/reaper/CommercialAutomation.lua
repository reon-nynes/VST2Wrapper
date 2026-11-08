local profile = reaper.GetResourcePath():gsub('\\', '/')
local output = assert(io.open(profile .. '/commercial-automation-result.txt', 'w'))
local ready = profile .. '/commercial-editor-ready.txt'
local function log(value) output:write(value, '\n') output:flush() end
local function hostTurn(seconds) coroutine.yield(reaper.time_precise() + (seconds or 0.03)) end
local cases = {{'Decapitator', 2}, {'Little AlterBoy', 1}, {'D82 Sonic Maximizer', 2}}
local execution = coroutine.create(function()
  for index, case in ipairs(cases) do
    os.remove(ready)
    reaper.Main_openProject('noprompt:' .. profile .. '/empty-audio.rpp')
    reaper.GetSetProjectInfo(0, 'PROJECT_SRATE', 48000, true)
    reaper.GetSetProjectInfo(0, 'PROJECT_SRATE_USE', 1, true)
    reaper.InsertTrackAtIndex(0, true)
    local track = assert(reaper.GetTrack(0, 0))
    reaper.SetOnlyTrackSelected(track)
    reaper.InsertMedia(profile .. '/commercial-input.wav', 0)
    local item = assert(reaper.GetTrackMediaItem(track, 0))
    reaper.SetMediaItemInfo_Value(item, 'D_FADEINLEN', 0)
    reaper.SetMediaItemInfo_Value(item, 'D_FADEOUTLEN', 0)
    local fx = reaper.TrackFX_AddByName(track, 'VST3: ' .. case[1] .. ' (Nytera)', false, -1)
    assert(fx >= 0, 'Cannot load ' .. case[1])
    reaper.TrackFX_SetParamNormalized(track, fx, case[2], 0.35)
    hostTurn(0.12)
    local envelope = assert(reaper.GetFXEnvelope(track, fx, case[2], true))
    local ok, chunk = reaper.GetEnvelopeStateChunk(envelope, '', false)
    assert(ok)
    chunk = chunk:gsub('ARM %d+', 'ARM 1'):gsub('ACT %d+', 'ACT 1')
    assert(reaper.SetEnvelopeStateChunk(envelope, chunk, false))
    reaper.SetGlobalAutomationOverride(-1) reaper.SetTrackAutomationMode(track, 3)
    reaper.TrackFX_Show(track, fx, 3)
    hostTurn(5)
    reaper.SetEditCurPos(0, false, false) reaper.OnPlayButton()
    local signalled, deadline = false, reaper.time_precise() + 30
    while reaper.GetPlayPosition() < 1.7 do
      assert(reaper.time_precise() < deadline, 'Recording transport did not advance')
      if reaper.GetPlayPosition() > 0.4 and not signalled then
        local file = assert(io.open(ready, 'w'))
        file:write(index, '\n', case[1], '\n', tostring(reaper.TrackFX_GetFloatingWindow(track, fx))) file:close()
        signalled = true log('Ready for original-editor gesture ' .. case[1])
      end
      hostTurn()
    end
    reaper.OnStopButton() reaper.SetTrackAutomationMode(track, 1)
    os.remove(ready)
    local minimum, maximum, points = 1, 0, reaper.CountEnvelopePoints(envelope)
    for point = 0, points - 1 do
      local hasPoint, time, value = reaper.GetEnvelopePoint(envelope, point)
      assert(hasPoint)
      minimum, maximum = math.min(minimum, value), math.max(maximum, value)
      log(string.format('%s point %.5f %.6f', case[1], time, value))
    end
    assert(points >= 2 and maximum - minimum > 0.001, 'Original-editor automation was not recorded')
    log('PASS ' .. case[1] .. ' original-editor automation recording')
    reaper.TrackFX_Show(track, fx, 2) reaper.TrackFX_Show(track, fx, 0)
    assert(not reaper.TrackFX_GetOpen(track, fx))
    local project = profile .. '/commercial-automation-' .. index .. '.rpp'
    reaper.Main_SaveProjectEx(0, project, 8)
    reaper.Main_openProject('noprompt:' .. project)
    track = assert(reaper.GetTrack(0, 0))
    envelope = assert(reaper.GetFXEnvelope(track, 0, case[2], false))
    assert(reaper.CountEnvelopePoints(envelope) == points, 'Recall changed the automation envelope')
    for key, value in pairs({RENDER_SRATE=48000, RENDER_CHANNELS=2, RENDER_SETTINGS=0, RENDER_BOUNDSFLAG=0,
      RENDER_STARTPOS=0, RENDER_ENDPOS=2, RENDER_TAILFLAG=0, RENDER_DITHER=16, RENDER_NORMALIZE=0}) do reaper.GetSetProjectInfo(0, key, value, true) end
    for key, value in pairs({RENDER_FILE=profile .. '/renders', RENDER_PATTERN='commercial-automation-' .. index,
      RENDER_FORMAT='evaw', RENDER_FORMAT2=''}) do reaper.GetSetProjectInfo_String(0, key, value, true) end
    os.remove(profile .. '/renders/commercial-automation-' .. index .. '.wav')
    reaper.Main_OnCommand(41824, 0)
    local file = assert(io.open(profile .. '/renders/commercial-automation-' .. index .. '.wav', 'rb'))
    local bytes = file:read('*a') file:close()
    local offset, samples, length = 13, nil, nil
    while offset + 7 <= #bytes do
      local tag, size = string.unpack('<c4I4', bytes, offset)
      if tag == 'data' then samples, length = offset + 8, size break end
      offset = offset + 8 + size + size % 2
    end
    assert(samples and length == 96000 * 6, 'Invalid automation render')
    local peak = 0
    for sample = 0, 95999 do
      local left, right = string.unpack('<i3i3', bytes, samples + sample * 6)
      peak = math.max(peak, math.abs(left / 8388608), math.abs(right / 8388608))
    end
    assert(peak > 0.00001, 'Closed-editor automation replay was silent')
    log(string.format('PASS %s closed-editor automation recall/render peak %.6f', case[1], peak))
    hostTurn(0.2)
  end
  log('PASS commercial automation')
end)
local resumeAt = 0
local function tick()
  if reaper.time_precise() < resumeAt then reaper.defer(tick) return end
  local ok, result = coroutine.resume(execution)
  if not ok then log('FAIL ' .. tostring(result) .. '\n' .. debug.traceback(execution)) output:close() return end
  if coroutine.status(execution) == 'dead' then output:close() return end
  resumeAt = result or 0
  reaper.defer(tick)
end
reaper.defer(tick)
