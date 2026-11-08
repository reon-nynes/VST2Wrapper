local profile = reaper.GetResourcePath():gsub('\\', '/')
local output = assert(io.open(profile .. '/automation-result.txt', 'w'))
local ready = profile .. '/automation-click-ready.txt'
os.remove(ready)
local function log(value) output:write(value, '\n') output:flush() end
local function defer(step)
  reaper.defer(function()
    local ok, error = xpcall(step, debug.traceback)
    if not ok then reaper.OnStopButton() log('FAIL ' .. error) output:close() end
  end)
end
local function run()
  log('Opening empty project')
  reaper.Main_openProject('noprompt:' .. profile .. '/empty-audio.rpp')
  reaper.InsertTrackAtIndex(0, true)
  local track = assert(reaper.GetTrack(0, 0))
  reaper.GetSetProjectInfo(0, 'PROJECT_SRATE', 48000, true)
  reaper.GetSetProjectInfo(0, 'PROJECT_SRATE_USE', 1, true)
  local wave = assert(io.open(profile .. '/automation-input.wav', 'wb'))
  local size = 3 * 48000 * 8
  wave:write('RIFF', string.pack('<I4', size + 36), 'WAVEfmt ', string.pack('<I4I2I2I4I4I2I2', 16, 3, 2, 48000, 384000, 8, 32), 'data', string.pack('<I4', size))
  local frame = string.pack('<ff', 0.8, 0.8)
  for i = 1, 3 * 48000 do wave:write(frame) end
  wave:close()
  reaper.SetOnlyTrackSelected(track)
  reaper.InsertMedia(profile .. '/automation-input.wav', 0)
  local item = assert(reaper.GetTrackMediaItem(track, 0))
  reaper.SetMediaItemInfo_Value(item, 'D_FADEINLEN', 0)
  reaper.SetMediaItemInfo_Value(item, 'D_FADEOUTLEN', 0)
  log('Adding controlled effect')
  local fx = reaper.TrackFX_AddByName(track, 'VST3: Nytera Delay Fixture', false, -1)
  assert(fx >= 0, 'Cannot load controlled effect')
  log('Creating parameter envelope')
  local envelope = assert(reaper.GetFXEnvelope(track, fx, 0, true))
  local ok, chunk = reaper.GetEnvelopeStateChunk(envelope, '', false)
  assert(ok)
  chunk = chunk:gsub('ARM %d+', 'ARM 1'):gsub('ACT %d+', 'ACT 1')
  assert(reaper.SetEnvelopeStateChunk(envelope, chunk, false))
  reaper.SetGlobalAutomationOverride(-1)
  reaper.SetTrackAutomationMode(track, 3)
  log('Opening original editor')
  reaper.TrackFX_Show(track, fx, 3)
  log('Starting automation recording')
  reaper.SetEditCurPos(0, false, false)
  reaper.OnPlayButton()
  local deadline, signalled = reaper.time_precise() + 10, false
  local function record()
    assert(reaper.time_precise() < deadline, 'Recording did not advance')
    local position = reaper.GetPlayPosition()
    if position >= 0.3 and not signalled then
      signalled = true
      local file = assert(io.open(ready, 'w')) file:write('Click the controlled original editor once') file:close()
      log('Ready for original-editor edit')
    end
    if position < 1.2 then defer(record) return end
    reaper.OnStopButton()
    reaper.SetTrackAutomationMode(track, 1)
    reaper.TrackFX_Show(track, fx, 2)
    reaper.TrackFX_Show(track, fx, 0)
    local count, changed = reaper.CountEnvelopePoints(envelope), false
    for i = 0, count - 1 do
      local hasPoint, time, value = reaper.GetEnvelopePoint(envelope, i)
      if hasPoint and math.abs(value - 0.75) < 0.000001 then changed = true end
      log(string.format('Point %.6f %.6f', time, value))
    end
    assert(changed, 'Original-editor edit was not recorded')
    log('PASS original-editor automation recording')
    reaper.Main_SaveProjectEx(0, profile .. '/fixture-automation.rpp', 8)
    assert(not reaper.TrackFX_GetOpen(track, fx), 'Editor is still open')
    for key, value in pairs({RENDER_SRATE=48000, RENDER_CHANNELS=2, RENDER_SETTINGS=0, RENDER_BOUNDSFLAG=0,
      RENDER_STARTPOS=0, RENDER_ENDPOS=1.2, RENDER_TAILFLAG=0, RENDER_DITHER=16, RENDER_NORMALIZE=0}) do
      reaper.GetSetProjectInfo(0, key, value, true)
    end
    for key, value in pairs({RENDER_FILE=profile .. '/renders', RENDER_PATTERN='fixture-automation', RENDER_FORMAT='evaw', RENDER_FORMAT2=''}) do
      reaper.GetSetProjectInfo_String(0, key, value, true)
    end
    os.remove(profile .. '/renders/fixture-automation.wav')
    reaper.Main_OnCommand(41824, 0)
    local rendered = assert(io.open(profile .. '/renders/fixture-automation.wav', 'rb'))
    local bytes = rendered:read('*a') rendered:close()
    local position, samples, sampleBytes = 13, nil, nil
    while position + 7 <= #bytes do
      local tag = bytes:sub(position, position + 3)
      local length = string.unpack('<I4', bytes, position + 4)
      if tag == 'fmt ' then
        local format, channels, rate, _, _, bits = string.unpack('<I2I2I4I4I2I2', bytes, position + 8)
        assert((format == 1 or format == 65534) and channels == 2 and rate == 48000 and bits == 24)
      elseif tag == 'data' then samples, sampleBytes = position + 8, length end
      position = position + 8 + length + length % 2
    end
    assert(samples and sampleBytes == 57600 * 6, 'Wrong render length')
    for _, region in ipairs({{4800, 9600, 0.4}, {43200, 52800, 0.6}}) do
      for sample = region[1], region[2] - 1 do
        local left, right = string.unpack('<i3i3', bytes, samples + sample * 6)
        assert(left == right and math.abs(left / 8388608 - region[3]) < 0.000001, 'Wrong replay audio at ' .. sample)
      end
    end
    log('PASS closed-editor automation replay through rendered audio')
    output:close()
  end
  defer(record)
end
defer(run)
