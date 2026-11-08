local profile = reaper.GetResourcePath():gsub('\\', '/')
local output = assert(io.open(profile .. '/state-result.txt', 'w'))
local function log(value) output:write(value, '\n') output:flush() end
local function verify(track, expected)
  assert(reaper.TrackFX_GetCount(track) == #expected, 'Instance count changed')
  for index, value in ipairs(expected) do
    assert(math.abs(reaper.TrackFX_GetParamNormalized(track, index - 1, 0) - value) < 0.000001,
      'Saved instance value changed at ' .. index)
    assert(not reaper.TrackFX_GetOpen(track, index - 1), 'Editor unexpectedly opened')
  end
end
local function run()
  local wave = assert(io.open(profile .. '/state-input.wav', 'wb'))
  local size = 48000 * 8
  wave:write('RIFF', string.pack('<I4', size + 36), 'WAVEfmt ', string.pack('<I4I2I2I4I4I2I2', 16, 3, 2, 48000, 384000, 8, 32), 'data', string.pack('<I4', size))
  local frame = string.pack('<ff', 0.8, 0.8)
  for i = 1, 48000 do wave:write(frame) end
  wave:close()
  local function checkKind(kind)
    reaper.Main_openProject('noprompt:' .. profile .. '/empty-audio.rpp')
    reaper.InsertTrackAtIndex(0, true)
    local track = assert(reaper.GetTrack(0, 0))
    reaper.SetOnlyTrackSelected(track)
    reaper.InsertMedia(profile .. '/state-input.wav', 0)
    local item = assert(reaper.GetTrackMediaItem(track, 0))
    reaper.SetMediaItemInfo_Value(item, 'D_FADEINLEN', 0)
    reaper.SetMediaItemInfo_Value(item, 'D_FADEOUTLEN', 0)
    for index, value in ipairs({0.25, 0.75}) do
      local fx = reaper.TrackFX_AddByName(track, 'VST3: Nytera ' .. kind .. ' Fixture', false, -1)
      assert(fx == index - 1, 'Cannot load fixture')
      assert(reaper.TrackFX_SetParamNormalized(track, fx, 0, value))
      reaper.TrackFX_Show(track, fx, 3)
      reaper.TrackFX_Show(track, fx, 2)
      reaper.TrackFX_Show(track, fx, 0)
    end
    local project = profile .. '/state-' .. kind .. '.rpp'
    reaper.Main_SaveProjectEx(0, project, 8)
    reaper.Main_openProject('noprompt:' .. project)
    track = assert(reaper.GetTrack(0, 0))
    verify(track, {0.25, 0.75})
    log('PASS ' .. kind .. ' two closed-editor instances saved and reopened')
    reaper.TrackFX_CopyToTrack(track, 0, track, 2, false)
    verify(track, {0.25, 0.75, 0.25})
    reaper.Undo_BeginBlock()
    assert(reaper.TrackFX_SetParamNormalized(track, 2, 0, 0.125))
    reaper.Undo_EndBlock('Nytera state verification', 2)
    verify(track, {0.25, 0.75, 0.125})
    reaper.Undo_DoUndo2(0)
    track = assert(reaper.GetTrack(0, 0))
    verify(track, {0.25, 0.75, 0.25})
    reaper.Undo_DoRedo2(0)
    track = assert(reaper.GetTrack(0, 0))
    verify(track, {0.25, 0.75, 0.125})
    reaper.Undo_DoUndo2(0)
    track = assert(reaper.GetTrack(0, 0))
    verify(track, {0.25, 0.75, 0.25})
    log('PASS ' .. kind .. ' duplicate and undo/redo')
    for key, value in pairs({PROJECT_SRATE=48000, PROJECT_SRATE_USE=1, RENDER_SRATE=48000, RENDER_CHANNELS=2,
      RENDER_SETTINGS=0, RENDER_BOUNDSFLAG=0, RENDER_STARTPOS=0, RENDER_ENDPOS=1, RENDER_TAILFLAG=0,
      RENDER_DITHER=16, RENDER_NORMALIZE=0}) do reaper.GetSetProjectInfo(0, key, value, true) end
    for key, value in pairs({RENDER_FILE=profile .. '/renders', RENDER_PATTERN='state-' .. kind,
      RENDER_FORMAT='evaw', RENDER_FORMAT2=''}) do reaper.GetSetProjectInfo_String(0, key, value, true) end
    reaper.Main_SaveProjectEx(0, project, 8)
    os.remove(profile .. '/renders/state-' .. kind .. '.wav')
    reaper.Main_OnCommand(41824, 0)
    local rendered = assert(io.open(profile .. '/renders/state-' .. kind .. '.wav', 'rb'))
    local bytes = rendered:read('*a') rendered:close()
    local position, samples = 13, nil
    while position < #bytes do
      local tag, length = string.unpack('<c4I4', bytes, position)
      if tag == 'data' then samples = position + 8 break end
      position = position + 8 + length + length % 2
    end
    assert(samples, 'Rendered audio has no data')
    for frame = 4800, 38399 do
      local left, right = string.unpack('<i3i3', bytes, samples + frame * 6)
      assert(math.abs(left / 8388608 - 0.0375) < 0.000001 and left == right, 'Restored DSP differs at ' .. frame .. ' value ' .. left / 8388608)
    end
    log('PASS ' .. kind .. ' restored DSP with editors closed')
  end
  checkKind('Chunk')
  reaper.defer(function()
    local ok, error = xpcall(function() checkKind('Program') log('PASS state project recall') end, debug.traceback)
    if not ok then log('FAIL ' .. error) end
    output:close()
  end)
end
reaper.defer(function()
  local ok, error = xpcall(run, debug.traceback)
  if not ok then log('FAIL ' .. error) end
  if not ok then output:close() end
end)
