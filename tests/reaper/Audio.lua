local profile = reaper.GetResourcePath():gsub('\\', '/')
local output = assert(io.open(profile .. '/audio-result.txt', 'w'))
local function log(value)
  output:write(value, '\n')
  output:flush()
end
local function setting(name, value)
  reaper.GetSetProjectInfo(0, name, value, true)
end
local function stringSetting(name, value)
  reaper.GetSetProjectInfo_String(0, name, value, true)
end
local function makeImpulse()
  local frames, rate = 48000, 48000
  local bytes = frames * 8
  local file = assert(io.open(profile .. '/impulse.wav', 'wb'))
  file:write('RIFF', string.pack('<I4', bytes + 36), 'WAVEfmt ', string.pack('<I4I2I2I4I4I2I2', 16, 3, 2, rate, rate * 8, 8, 32), 'data', string.pack('<I4', bytes))
  for i = 0, frames - 1 do
    local value = i == 12000 and 0.8 or 0
    file:write(string.pack('<ff', value, value))
  end
  file:close()
end
local function verifyRender(name, instrument)
  local file = assert(io.open(profile .. '/renders/' .. name .. '.wav', 'rb'))
  local bytes = file:read('*a')
  file:close()
  assert(bytes:sub(1, 4) == 'RIFF' and bytes:sub(9, 12) == 'WAVE', 'Invalid WAV')
  local position, samples, sampleBytes = 13, nil, nil
  while position + 7 <= #bytes do
    local tag = bytes:sub(position, position + 3)
    local size = string.unpack('<I4', bytes, position + 4)
    if tag == 'fmt ' then
      local format, channels, rate, _, _, bits = string.unpack('<I2I2I4I4I2I2', bytes, position + 8)
      assert((format == 1 or format == 65534) and channels == 2 and rate == 48000 and bits == 24, 'Unexpected WAV format')
    elseif tag == 'data' then
      samples, sampleBytes = position + 8, size
    end
    position = position + 8 + size + size % 2
  end
  assert(samples and sampleBytes == 48000 * 6, 'Wrong rendered length')
  for frame = 0, 47999 do
    local left, right = string.unpack('<i3i3', bytes, samples + frame * 6)
    local expected = instrument and (frame >= 12000 and frame < 24000 and 0.5 or 0) or (frame == 12000 and 0.4 or 0)
    assert(math.abs(left / 8388608 - expected) < 0.000001 and left == right, 'Wrong audio at sample ' .. frame)
  end
  log('PASS sample-exact ' .. name)
end
local function prepare(name, instrument)
  reaper.Main_openProject('noprompt:' .. profile .. '/empty-audio.rpp')
  setting('PROJECT_SRATE', 48000)
  setting('PROJECT_SRATE_USE', 1)
  setting('RENDER_SRATE', 48000)
  setting('RENDER_CHANNELS', 2)
  setting('RENDER_SETTINGS', 0)
  setting('RENDER_BOUNDSFLAG', 0)
  setting('RENDER_STARTPOS', 0)
  setting('RENDER_ENDPOS', 1)
  setting('RENDER_TAILFLAG', 0)
  setting('RENDER_DITHER', 16)
  setting('RENDER_NORMALIZE', 0)
  stringSetting('RENDER_FILE', profile .. '/renders')
  stringSetting('RENDER_PATTERN', name)
  stringSetting('RENDER_FORMAT', 'evaw')
  stringSetting('RENDER_FORMAT2', '')
  reaper.InsertTrackAtIndex(0, true)
  local track = assert(reaper.GetTrack(0, 0))
  reaper.SetOnlyTrackSelected(track)
  if instrument then
    local item = assert(reaper.CreateNewMIDIItemInProj(track, 0, 1, false))
    local take = assert(reaper.GetActiveTake(item))
    local start = reaper.MIDI_GetPPQPosFromProjTime(take, 0.25)
    local finish = reaper.MIDI_GetPPQPosFromProjTime(take, 0.5)
    assert(reaper.MIDI_InsertNote(take, false, false, start, finish, 0, 60, 100, false))
  else
    reaper.InsertMedia(profile .. '/impulse.wav', 0)
    local item = assert(reaper.GetTrackMediaItem(track, 0))
    reaper.SetMediaItemInfo_Value(item, 'D_FADEINLEN', 0)
    reaper.SetMediaItemInfo_Value(item, 'D_FADEOUTLEN', 0)
  end
  local plugin = instrument and 'Nytera MIDI Fixture' or 'Nytera Delay Fixture'
  local fx = reaper.TrackFX_AddByName(track, 'VST3: ' .. plugin, false, -1)
  assert(fx >= 0, 'Cannot load ' .. plugin)
  reaper.Main_SaveProjectEx(0, profile .. '/' .. name .. '.rpp', 8)
  os.remove(profile .. '/renders/' .. name .. '.wav')
  log('Rendering ' .. name)
  reaper.Main_OnCommand(41824, 0)
  log('Render completed ' .. name)
  verifyRender(name, instrument)
  return track
end
local function run()
  makeImpulse()
  local empty = assert(io.open(profile .. '/empty-audio.rpp', 'w'))
  empty:write('<REAPER_PROJECT 0.1 7.85 1\n>\n')
  empty:close()
  prepare('fixture-impulse', false)
  prepare('fixture-midi', true)
  local function playback(name, expected, completed)
    reaper.Main_openProject('noprompt:' .. profile .. '/' .. name .. '.rpp')
    local track = assert(reaper.GetTrack(0, 0))
    reaper.SetEditCurPos(0, false, false)
    reaper.Track_GetPeakHoldDB(track, 0, true)
    reaper.OnPlayButton()
    local startupDeadline, finish = reaper.time_precise() + 30, nil
    local peak = 0
    local function wait()
      peak = math.max(peak, reaper.Track_GetPeakInfo(track, 0))
      if not finish then
        if reaper.GetPlayState() & 1 ~= 0 and reaper.GetPlayPosition() > 0.005 then finish = reaper.time_precise() + 1.2
        elseif reaper.time_precise() >= startupDeadline then log('FAIL playback did not start after closing the render dialog') output:close() return
        else
          if reaper.GetPlayState() & 1 == 0 then reaper.OnPlayButton() end
          reaper.defer(wait) return
        end
      end
      if reaper.time_precise() < finish then reaper.defer(wait) return end
      reaper.OnStopButton()
      log('Realtime ' .. name .. ' peak amplitude ' .. peak)
      if math.abs(peak - expected) < 0.00001 then
        log('PASS realtime ' .. name)
        completed()
      else
        log('FAIL realtime ' .. name)
        output:close()
      end
    end
    reaper.defer(wait)
  end
  playback('fixture-impulse', 0.4, function()
    playback('fixture-midi', 0.5, function()
      log('PASS realtime playback and offline sample checks')
      output:close()
    end)
  end)
end
local ok, error = xpcall(run, debug.traceback)
if not ok then log('FAIL ' .. error) output:close() end
