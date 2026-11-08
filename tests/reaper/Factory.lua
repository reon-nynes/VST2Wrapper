local profile = reaper.GetResourcePath():gsub('\\', '/')
local output = assert(io.open(profile .. '/factory-result.txt', 'w'))
local function log(value)
  output:write(value, '\n')
  output:flush()
end
local function run()
  log('REAPER ' .. reaper.GetAppVersion())
  reaper.InsertTrackAtIndex(0, true)
  local track = assert(reaper.GetTrack(0, 0))
  for _, name in ipairs({'Nytera Test Alpha', 'Nytera Test Beta'}) do
    local fx = reaper.TrackFX_AddByName(track, 'VST3: ' .. name, false, -1)
    assert(fx >= 0, 'Cannot load ' .. name)
    local ok, actual = reaper.TrackFX_GetFXName(track, fx)
    assert(ok and actual:find(name, 1, true), 'Wrong plugin identity')
    assert(reaper.TrackFX_GetNumParams(track, fx) >= 1, 'Missing parameters')
    local hasName, parameter = reaper.TrackFX_GetParamName(track, fx, 0)
    assert(hasName and parameter == 'Gain', 'Wrong first parameter')
    log('Loaded ' .. actual .. ' with ' .. reaper.TrackFX_GetNumParams(track, fx) .. ' parameters')
  end
  local project = profile .. '/factory-identity.rpp'
  reaper.Main_SaveProjectEx(0, project, 8)
  reaper.Main_openProject('noprompt:' .. project)
  track = assert(reaper.GetTrack(0, 0))
  assert(reaper.TrackFX_GetCount(track) == 2, 'Reload lost an instance')
  for index, name in ipairs({'Nytera Test Alpha', 'Nytera Test Beta'}) do
    local _, actual = reaper.TrackFX_GetFXName(track, index - 1)
    assert(actual:find(name, 1, true), 'Reload changed plugin identity')
  end
  log('PASS factory identity and project reload')
end
local ok, error = xpcall(run, debug.traceback)
if not ok then log('FAIL ' .. error) end
output:close()
