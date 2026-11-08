local profile = reaper.GetResourcePath():gsub('\\', '/')
local output = assert(io.open(profile .. '/recovery-result.txt', 'w'))
local ready = profile .. '/recovery-ready.txt'
local retried = profile .. '/recovery-retried.txt'
os.remove(ready) os.remove(retried)
local function log(value) output:write(value, '\n') output:flush() end
local function hostTurn(seconds) coroutine.yield(reaper.time_precise() + (seconds or 0.12)) end
local execution = coroutine.create(function()
  reaper.Main_openProject('noprompt:' .. profile .. '/empty-audio.rpp')
  reaper.GetSetProjectInfo(0, 'PROJECT_SRATE', 48000, true)
  reaper.GetSetProjectInfo(0, 'PROJECT_SRATE_USE', 1, true)
  local wave = assert(io.open(profile .. '/recovery-input.wav', 'wb'))
  local size = 20 * 48000 * 8
  wave:write('RIFF', string.pack('<I4', size + 36), 'WAVEfmt ', string.pack('<I4I2I2I4I4I2I2', 16, 3, 2, 48000, 384000, 8, 32), 'data', string.pack('<I4', size))
  local frame = string.pack('<ff', 0.8, 0.8)
  for sample = 1, 20 * 48000 do wave:write(frame) end
  wave:close()
  local tracks = {}
  for index, gain in ipairs({0.25, 0.75}) do
    reaper.InsertTrackAtIndex(index - 1, true)
    local track = assert(reaper.GetTrack(0, index - 1)) tracks[index] = track
    reaper.SetOnlyTrackSelected(track) reaper.SetEditCurPos(0, false, false)
    reaper.InsertMedia(profile .. '/recovery-input.wav', 0)
    local item = assert(reaper.GetTrackMediaItem(track, 0))
    reaper.SetMediaItemInfo_Value(item, 'D_FADEINLEN', 0) reaper.SetMediaItemInfo_Value(item, 'D_FADEOUTLEN', 0)
    local fx = reaper.TrackFX_AddByName(track, 'VST3: Nytera Chunk Fixture', false, -1)
    assert(fx == 0, 'Cannot instantiate recovery fixture')
    reaper.TrackFX_SetParamNormalized(track, fx, 0, gain) hostTurn()
  end
  reaper.Main_SaveProjectEx(0, profile .. '/fixture-recovery.rpp', 8)
  reaper.TrackFX_Show(tracks[1], 0, 3) hostTurn()
  reaper.SetEditCurPos(0, false, false) reaper.OnPlayButton()
  local firstPeak, secondPeak, initialDeadline = 0, 0, reaper.time_precise() + 1
  while reaper.time_precise() < initialDeadline do
    firstPeak = math.max(firstPeak, reaper.Track_GetPeakInfo(tracks[1], 0))
    secondPeak = math.max(secondPeak, reaper.Track_GetPeakInfo(tracks[2], 0))
    hostTurn(0.03)
  end
  assert(firstPeak > 0.19, 'First instance is silent before failure')
  assert(secondPeak > 0.59, 'Second instance is silent before failure')
  local signal = assert(io.open(ready, 'w')) signal:write('Terminate only the first original editor helper, then use Retry this plugin') signal:close()
  local deadline = reaper.time_precise() + 30
  local silentSince
  while true do
    local completed = io.open(retried, 'r')
    if completed then completed:close() break end
    assert(reaper.time_precise() < deadline, 'Recovery interaction did not finish')
    local peak = reaper.Track_GetPeakInfo(tracks[2], 0)
    log(string.format('Recovery observation state %d position %.3f independent peak %.6f', reaper.GetPlayState(), reaper.GetPlayPosition(), peak))
    if peak > 0.01 then silentSince = nil else silentSince = silentSince or reaper.time_precise() end
    assert(not silentSince or reaper.time_precise() - silentSince < 0.5, 'Independent instance remained silent')
    hostTurn(0.03)
  end
  local restoredPeak, independentPeak, finish = 0, 0, reaper.time_precise() + 1
  while reaper.time_precise() < finish do
    restoredPeak = math.max(restoredPeak, reaper.Track_GetPeakInfo(tracks[1], 0))
    independentPeak = math.max(independentPeak, reaper.Track_GetPeakInfo(tracks[2], 0))
    hostTurn(0.03)
  end
  assert(math.abs(reaper.TrackFX_GetParamNormalized(tracks[1], 0, 0) - 0.25) < 0.00001, 'Retry lost saved parameter state')
  assert(restoredPeak > 0.19, 'Retry did not resume audible processing')
  assert(independentPeak > 0.59, 'Independent instance stopped after retry')
  reaper.OnStopButton() reaper.TrackFX_Show(tracks[1], 0, 2)
  log('PASS helper failure, independent instance continuity, original-editor retry and saved-state audio recovery')
end)
local resumeAt = 0
local function tick()
  if reaper.time_precise() < resumeAt then reaper.defer(tick) return end
  local ok, result = coroutine.resume(execution)
  if not ok then reaper.OnStopButton() log('FAIL ' .. tostring(result) .. '\n' .. debug.traceback(execution)) output:close() return end
  if coroutine.status(execution) == 'dead' then output:close() return end
  resumeAt = result or 0 reaper.defer(tick)
end
reaper.defer(tick)
