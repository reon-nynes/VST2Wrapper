local profile = reaper.GetResourcePath():gsub('\\', '/')
local output = assert(io.open(profile .. '/editors-result.txt', 'w'))
local function log(value) output:write(value, '\n'); output:flush() end
local queue = {'Decapitator', 'Little AlterBoy', 'D82 Sonic Maximizer', 'VPRE-73'}
local track
local stage = 1
local deadline = 0
local currentFx = -1
local function advance()
  local ok, error = xpcall(function()
    if stage == 1 then
      reaper.Main_openProject('noprompt:' .. profile .. '/empty-audio.rpp')
      reaper.InsertTrackAtIndex(0, true)
      track = assert(reaper.GetTrack(0, 0))
      log('REAPER ' .. reaper.GetAppVersion())
      stage = 2
    end
    if #queue == 0 then
      log('PASS floating and FX-chain editor lifecycle')
      output:close()
      return
    end
    if stage == 2 then
      currentFx = reaper.TrackFX_AddByName(track, 'VST3: ' .. queue[1] .. ' (Nytera)', false, -1)
      assert(currentFx >= 0, 'Cannot load ' .. queue[1])
      reaper.TrackFX_Show(track, currentFx, 3)
      assert(reaper.TrackFX_GetFloatingWindow(track, currentFx), 'Missing floating editor')
      log('Floating editor opened ' .. queue[1])
      deadline = reaper.time_precise() + 4
      stage = 3
    elseif stage == 3 and reaper.time_precise() >= deadline then
      reaper.TrackFX_Show(track, currentFx, 2)
      reaper.TrackFX_Show(track, currentFx, 1)
      log('FX-chain editor opened ' .. queue[1])
      deadline = reaper.time_precise() + 4
      stage = 4
    elseif stage == 4 and reaper.time_precise() >= deadline then
      reaper.TrackFX_Show(track, currentFx, 0)
      reaper.TrackFX_Show(track, currentFx, 3)
      log('Editor reopened ' .. queue[1])
      deadline = reaper.time_precise() + 4
      stage = 5
    elseif stage == 5 and reaper.time_precise() >= deadline then
      reaper.TrackFX_Show(track, currentFx, 2)
      table.remove(queue, 1)
      stage = 2
    end
    reaper.defer(advance)
  end, debug.traceback)
  if not ok then log('FAIL ' .. error); output:close() end
end
advance()
