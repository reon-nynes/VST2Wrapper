local profile = reaper.GetResourcePath():gsub('\\', '/')
local output = assert(io.open(profile .. '/commercial-state-result.txt', 'w'))
local function log(value) output:write(value, '\n') output:flush() end
local function hostTurn(seconds) coroutine.yield(reaper.time_precise() + (seconds or 0.12)) end
local cases = {{'Decapitator', 12, 2}, {'Little AlterBoy', 7, 1}, {'D82 Sonic Maximizer', 5, 2}}
local function check(track, values, parameter)
  assert(reaper.TrackFX_GetCount(track) == #values, 'Instance count changed')
  for index, value in ipairs(values) do
    local actual = reaper.TrackFX_GetParamNormalized(track, index - 1, parameter)
    assert(math.abs(actual - value) < 0.00001, 'State mismatch at instance ' .. index .. ' value ' .. actual)
    assert(not reaper.TrackFX_GetOpen(track, index - 1), 'Editor remained open')
  end
end
local function run(index)
  local case = cases[index]
  local name, count, parameter = case[1], case[2], case[3]
  log('Testing ' .. name)
  reaper.Main_openProject('noprompt:' .. profile .. '/empty-audio.rpp')
  reaper.InsertTrackAtIndex(0, true)
  local track = assert(reaper.GetTrack(0, 0))
  reaper.SetOnlyTrackSelected(track)
  reaper.InsertMedia(profile .. '/commercial-input.wav', 0)
  local item = assert(reaper.GetTrackMediaItem(track, 0))
  reaper.SetMediaItemInfo_Value(item, 'D_FADEINLEN', 0)
  reaper.SetMediaItemInfo_Value(item, 'D_FADEOUTLEN', 0)
  for instance, value in ipairs({0.25, 0.75}) do
    local fx = reaper.TrackFX_AddByName(track, 'VST3: ' .. name .. ' (Nytera)', false, -1)
    assert(fx == instance - 1, 'Cannot instantiate ' .. name)
    assert(reaper.TrackFX_GetNumParams(track, fx) >= count, 'Parameters missing')
    for parameter = 0, count - 1 do
      local _, label = reaper.TrackFX_GetParamName(track, fx, parameter)
      if instance == 1 then log('Parameter ' .. parameter .. ' ' .. label) end
    end
    assert(reaper.TrackFX_SetParamNormalized(track, fx, parameter, value))
    hostTurn()
    reaper.TrackFX_Show(track, fx, 3)
    reaper.TrackFX_Show(track, fx, 2)
    reaper.TrackFX_Show(track, fx, 0)
    hostTurn()
  end
  local project = profile .. '/commercial-state-' .. index .. '.rpp'
  reaper.Main_SaveProjectEx(0, project, 8)
  reaper.Main_openProject('noprompt:' .. project)
  track = assert(reaper.GetTrack(0, 0))
  check(track, {0.25, 0.75}, parameter)
  reaper.TrackFX_CopyToTrack(track, 0, track, 2, false)
  hostTurn()
  check(track, {0.25, 0.75, 0.25}, parameter)
  reaper.Undo_BeginBlock()
  assert(reaper.TrackFX_SetParamNormalized(track, 2, parameter, 0.5))
  reaper.Undo_EndBlock('Commercial plugin state verification', 2)
  hostTurn()
  reaper.Undo_DoUndo2(0)
  hostTurn()
  track = assert(reaper.GetTrack(0, 0))
  check(track, {0.25, 0.75, 0.25}, parameter)
  reaper.Undo_DoRedo2(0)
  hostTurn()
  track = assert(reaper.GetTrack(0, 0))
  check(track, {0.25, 0.75, 0.5}, parameter)
  reaper.Undo_DoUndo2(0)
  hostTurn()
  track = assert(reaper.GetTrack(0, 0))
  check(track, {0.25, 0.75, 0.25}, parameter)
  log('PASS ' .. name .. ' two-instance recall, closed editors, duplication and undo/redo')
  for key, value in pairs({PROJECT_SRATE=48000, PROJECT_SRATE_USE=1, RENDER_SRATE=48000, RENDER_CHANNELS=2,
    RENDER_SETTINGS=0, RENDER_BOUNDSFLAG=0, RENDER_STARTPOS=0, RENDER_ENDPOS=2, RENDER_TAILFLAG=0,
    RENDER_DITHER=16, RENDER_NORMALIZE=0}) do reaper.GetSetProjectInfo(0, key, value, true) end
  for key, value in pairs({RENDER_FILE=profile .. '/renders', RENDER_PATTERN='commercial-' .. index,
    RENDER_FORMAT='evaw', RENDER_FORMAT2=''}) do reaper.GetSetProjectInfo_String(0, key, value, true) end
  reaper.Main_SaveProjectEx(0, project, 8)
  os.remove(profile .. '/renders/commercial-' .. index .. '.wav')
  reaper.Main_OnCommand(41824, 0)
  local file = assert(io.open(profile .. '/renders/commercial-' .. index .. '.wav', 'rb'))
  local bytes = file:read('*a') file:close()
  local position, samples, length = 13, nil, nil
  while position + 7 <= #bytes do
    local tag, size = string.unpack('<c4I4', bytes, position)
    if tag == 'data' then samples, length = position + 8, size break end
    position = position + 8 + size + size % 2
  end
  assert(samples and length == 96000 * 6, 'Invalid rendered WAV')
  local peak, energy = 0, 0
  for sample = 0, 95999 do
    local left, right = string.unpack('<i3i3', bytes, samples + sample * 6)
    local value = left / 8388608
    peak, energy = math.max(peak, math.abs(value), math.abs(right / 8388608)), energy + value * value
  end
  assert(peak > 0.00001 and energy > 0.001, 'Source produced no audible output')
  log(string.format('PASS %s offline audio peak %.6f RMS %.6f', name, peak, math.sqrt(energy / 96000)))
  local startupDeadline, finish, realtimePeak = reaper.time_precise() + 30, nil, 0
  reaper.SetEditCurPos(0, false, false) reaper.OnPlayButton()
  while not finish or reaper.time_precise() < finish do
    realtimePeak = math.max(realtimePeak, reaper.Track_GetPeakInfo(track, 0))
    if not finish then
      if reaper.GetPlayState() & 1 ~= 0 and reaper.GetPlayPosition() > 0.005 then finish = reaper.time_precise() + 2.5
      else
        assert(reaper.time_precise() < startupDeadline, 'Close the render dialog to start realtime playback')
        if reaper.GetPlayState() & 1 == 0 then reaper.OnPlayButton() end
      end
    end
    hostTurn(0.02)
  end
  reaper.OnStopButton()
  assert(realtimePeak > 0.00001, 'Realtime source output remained silent')
  log(string.format('PASS %s realtime audio peak %.6f', name, realtimePeak))

end
local execution = coroutine.create(function()
    local file = assert(io.open(profile .. '/commercial-input.wav', 'wb'))
    file:write('RIFF', string.pack('<I4', 96000 * 8 + 36), 'WAVEfmt ', string.pack('<I4I2I2I4I4I2I2', 16, 3, 2, 48000, 384000, 8, 32), 'data', string.pack('<I4', 96000 * 8))
    for sample = 0, 95999 do
      local value = 0.2 * math.sin(2 * math.pi * 220 * sample / 48000) + 0.08 * math.sin(2 * math.pi * 880 * sample / 48000)
      file:write(string.pack('<ff', value, value))
    end
    file:close()
    for index = 1, #cases do run(index) hostTurn() end
    log('PASS commercial state and closed-editor DSP')
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
