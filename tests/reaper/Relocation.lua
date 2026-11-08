local profile = reaper.GetResourcePath():gsub('\\', '/')
local output = assert(io.open(profile .. '/relocation-result.txt', 'w'))
local function log(value) output:write(value, '\n') output:flush() end
local function run()
  reaper.Main_openProject('noprompt:' .. profile .. '/empty-audio.rpp')
  reaper.GetSetProjectInfo(0, 'PROJECT_SRATE', 48000, true)
  reaper.GetSetProjectInfo(0, 'PROJECT_SRATE_USE', 1, true)
  local wave = assert(io.open(profile .. '/portable-input.wav', 'wb'))
  local size = 2 * 48000 * 8
  wave:write('RIFF', string.pack('<I4', size + 36), 'WAVEfmt ', string.pack('<I4I2I2I4I4I2I2', 16, 3, 2, 48000, 384000, 8, 32), 'data', string.pack('<I4', size))
  local frame = string.pack('<ff', 0.8, 0.8)
  for sample = 1, 2 * 48000 do wave:write(frame) end
  wave:close()
  reaper.InsertTrackAtIndex(0, true)
  local track = assert(reaper.GetTrack(0, 0))
  reaper.SetOnlyTrackSelected(track) reaper.SetEditCurPos(0, false, false)
  reaper.InsertMedia(profile .. '/portable-input.wav', 0)
  local item = assert(reaper.GetTrackMediaItem(track, 0))
  reaper.SetMediaItemInfo_Value(item, 'D_FADEINLEN', 0) reaper.SetMediaItemInfo_Value(item, 'D_FADEOUTLEN', 0)
  local fx = reaper.TrackFX_AddByName(track, 'VST3: Nytera Fixture (Nytera)', false, -1)
  assert(fx == 0, 'Cannot load relocated single-file fixture')
  assert(reaper.TrackFX_GetNumParams(track, fx) >= 2, 'Source parameter or bypass missing')
  local _, name = reaper.TrackFX_GetParamName(track, fx, 0)
  assert(name == 'Gain', 'Wrong source parameter')
  assert(math.abs(reaper.TrackFX_GetParamNormalized(track, fx, 0) - 0.5) < 0.00001, 'Wrong bound source')
  reaper.Main_SaveProjectEx(0, profile .. '/fixture-relocation.rpp', 8)
  reaper.SetEditCurPos(0, false, false) reaper.OnPlayButton()
  local peak, deadline = 0, reaper.time_precise() + 2
  local function observe()
    local ok, error = xpcall(function()
      peak = math.max(peak, reaper.Track_GetPeakInfo(track, 0))
      if reaper.time_precise() < deadline then reaper.defer(observe) return end
      reaper.OnStopButton()
      assert(peak > 0.35 and peak < 0.45, 'Relocated source peak ' .. peak)
      reaper.Main_openProject('noprompt:' .. profile .. '/fixture-relocation.rpp')
      local restored = assert(reaper.GetTrack(0, 0))
      assert(math.abs(reaper.TrackFX_GetParamNormalized(restored, 0, 0) - 0.5) < 0.00001, 'Relocation recall changed source')
      log('PASS renamed single-file load, embedded-source audio and project recall, peak ' .. peak)
      output:close()
    end, debug.traceback)
    if not ok then log('FAIL ' .. error) output:close() end
  end
  reaper.defer(observe)
end
reaper.defer(function()
  local ok, error = xpcall(run, debug.traceback)
  if not ok then log('FAIL ' .. error) output:close() end
end)
