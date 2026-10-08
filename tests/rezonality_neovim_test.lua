vim.g.rezonality_auto_setup = 0
vim.opt.runtimepath:prepend(vim.env.REZONALITY_TEST_PACKAGE)

local rezonality = require("rezonality")
local control_actions = {}
local closed_panes = {}
local function live_panes()
  local project = vim.fn.fnamemodify(vim.env.REZONALITY_TEST_FIRST, ":h")
  local panes = {}
  for _, pane in ipairs({
    {
      id = "pane-left",
      space_id = "space-flight",
      tab_id = "tab-deck",
      space_name = "Flight",
      tab_name = "Deck",
      name = "Left camera",
      client_plugin_id = "dev.draxul.rezonality",
      client_plugin_config_json = vim.json.encode({
        project_path = project,
        diagnostics_id = "flight-left",
      }),
    },
    {
      id = "pane-right",
      space_id = "space-flight",
      tab_id = "tab-deck",
      space_name = "Flight",
      tab_name = "Deck",
      name = "Right camera",
      client_plugin_id = "dev.draxul.rezonality",
      client_plugin_config_json = vim.json.encode({
        project_path = project,
        diagnostics_id = "flight-right",
      }),
    },
    {
      id = "pane-shell",
      client_plugin_id = "",
    },
  }) do
    if not closed_panes[pane.id] then
      table.insert(panes, pane)
    end
  end
  return panes
end
local function record_control(verb, instance)
  table.insert(control_actions, verb .. ":" .. instance.pane_id)
  return true
end
rezonality.setup({
  auto_refresh = false,
  registry_provider = live_panes,
  control_runner = record_control,
})

vim.cmd.edit(vim.fn.fnameescape(vim.env.REZONALITY_TEST_FIRST))
rezonality.refresh()
local first = vim.diagnostic.get(0)
local shared_sources = 0
if first[1] and first[1].user_data and first[1].user_data.sources then
  for _ in pairs(first[1].user_data.sources) do
    shared_sources = shared_sources + 1
  end
end

vim.cmd.edit(vim.fn.fnameescape(vim.env.REZONALITY_TEST_SECOND))
rezonality.refresh()
local second = vim.diagnostic.get(0)
local problems = rezonality.problems(false)
local quickfix = vim.fn.getqflist()
local instances = rezonality.instances()
local active_files = rezonality.files()
local failed_instances = 0
for _, instance in ipairs(instances) do
  if instance.status == "FAILED" then
    failed_instances = failed_instances + 1
  end
end
rezonality.focus_instance(instances[1])
rezonality.reload_instance(instances[2])
local selected_file = ""
local selected_file_has_both_instances = false
local apply_picker_opened = false
vim.ui.select = function(items, options, callback)
  if options.prompt == "Rezonality active shader files" then
    local selected = items[1]
    for _, item in ipairs(items) do
      if item.kind == "scene" then
        selected = item
      end
    end
    selected_file = selected.display_path
    local formatted = options.format_item(selected)
    selected_file_has_both_instances = formatted:find("Left camera", 1, true)
        ~= nil and formatted:find("Right camera", 1, true) ~= nil
    callback(selected)
  elseif options.prompt == "Apply shader to Rezonality pane" then
    apply_picker_opened = true
    callback(items[1])
  end
end
rezonality.show_files()
local apply_mapping = vim.fn.maparg("<C-CR>", "n", false, true)
local apply_mapping_installed = apply_mapping
    and apply_mapping.desc == "Apply current shader to Rezonality"
local apply_buffer_id = vim.api.nvim_get_current_buf()
vim.api.nvim_buf_set_lines(0, -1, -1, false, { "// apply test" })
rezonality.apply()
local apply_saved = not vim.bo.modified
local apply_flash_started = rezonality._state().flash_tokens[apply_buffer_id]
    ~= nil
rezonality.status()
local status_messages = vim.fn.execute("messages")

local rez_commands = 0
for _, name in ipairs({ "RezRefresh", "RezProblems", "RezInstances",
    "RezFiles", "RezApply", "RezFocus", "RezReload", "RezStatus",
    "RezEnable", "RezDisable" }) do
  if vim.fn.exists(":" .. name) == 2 then
    rez_commands = rez_commands + 1
  end
end
local compatibility_commands = 0
for _, name in ipairs({ "RezonalityRefresh", "RezonalityProblems",
    "RezonalityInstances", "RezonalityFiles", "RezonalityApply",
    "RezonalityFocus", "RezonalityReload", "RezonalityStatus",
    "RezonalityEnable", "RezonalityDisable" }) do
  if vim.fn.exists(":" .. name) == 2 then
    compatibility_commands = compatibility_commands + 1
  end
end

local all_entries = #rezonality._state().entries

-- Idle refresh: an unchanged editor must not reread records or republish
-- diagnostics, while record replacement and pane closure still arrive.
local uv = vim.uv or vim.loop
local idle = { reads = 0, sets = 0, resets = 0, registry = 0 }
local real_open = io.open
io.open = function(path, ...)
  if type(path) == "string" and path:match("%.json$") then
    idle.reads = idle.reads + 1
  end
  return real_open(path, ...)
end
local real_set, real_reset = vim.diagnostic.set, vim.diagnostic.reset
vim.diagnostic.set = function(...)
  idle.sets = idle.sets + 1
  return real_set(...)
end
vim.diagnostic.reset = function(...)
  idle.resets = idle.resets + 1
  return real_reset(...)
end
local function reset_idle_counts()
  for key in pairs(idle) do
    idle[key] = 0
  end
end
local function wait_until(predicate)
  return vim.wait(10000, predicate, 10)
end
local function message_present(buffer, text)
  for _, item in ipairs(vim.diagnostic.get(buffer)) do
    if item.message == text then
      return true
    end
  end
  return false
end

rezonality.setup({
  refresh_ms = 20,
  registry_refresh_ms = 60,
  registry_provider = function()
    idle.registry = idle.registry + 1
    return live_panes()
  end,
  control_runner = record_control,
})
vim.cmd.edit(vim.fn.fnameescape(vim.env.REZONALITY_TEST_FIRST))
local first_buffer = vim.api.nvim_get_current_buf()
wait_until(function()
  return idle.registry >= 2
end)
reset_idle_counts()
vim.wait(600, function()
  return false
end, 10)
local idle_reads, idle_sets, idle_resets = idle.reads, idle.sets, idle.resets
local idle_registry_polled = idle.registry > 0

local diagnostics_dir = rezonality._state().diagnostics_dir
local left_record = diagnostics_dir .. "/flight-left.json"
local template_file = assert(real_open(left_record, "rb"))
local replacement = vim.json.decode(template_file:read("*a"))
template_file:close()
replacement.diagnostics = { {
  path = vim.env.REZONALITY_TEST_FIRST,
  stage = "compile",
  severity = "error",
  line = 1,
  column = 1,
  message = "replaced left error",
} }
local staged = assert(real_open(left_record .. ".tmp", "wb"))
staged:write(vim.json.encode(replacement))
staged:close()
assert(uv.fs_rename(left_record .. ".tmp", left_record))
local replacement_arrived = wait_until(function()
  return message_present(first_buffer, "replaced left error")
    and message_present(first_buffer, "shared shader error")
end)

closed_panes["pane-left"] = true
local closed_pane_removed = wait_until(function()
  return #rezonality._state().instances == 1
    and not message_present(first_buffer, "replaced left error")
end)

vim.cmd("RezDisable")
local timer_stopped = not rezonality._state().timer:is_active()
local disabled_cleared = #vim.diagnostic.get(first_buffer) == 0
reset_idle_counts()
vim.wait(200, function()
  return false
end, 10)
local disabled_idle_work = idle.reads + idle.registry + idle.sets
io.open = real_open
vim.diagnostic.set, vim.diagnostic.reset = real_set, real_reset

local output = assert(io.open(vim.env.REZONALITY_TEST_RESULT, "wb"))
output:write(vim.json.encode({
  all_entries = all_entries,
  first_inline = #first,
  second_inline = #second,
  quickfix = #quickfix,
  shared_sources = shared_sources,
  instances = #instances,
  active_files = #active_files,
  selected_file = selected_file,
  selected_file_has_both_instances = selected_file_has_both_instances,
  apply_mapping_installed = apply_mapping_installed,
  apply_saved = apply_saved,
  apply_flash_started = apply_flash_started,
  apply_picker_opened = apply_picker_opened,
  failed_instances = failed_instances,
  control_actions = control_actions,
  registry_available = rezonality._state().registry_available,
  rez_commands = rez_commands,
  compatibility_commands = compatibility_commands,
  status_visible = status_messages:find("Rezonality: 3 diagnostics in 2 files; 3 active sources; 2 live panes", 1, true) ~= nil,
  server_discovery = rezonality._draxul_executable()
    == vim.env.REZONALITY_TEST_DRAXUL,
  idle_reads = idle_reads,
  idle_diagnostic_sets = idle_sets,
  idle_diagnostic_resets = idle_resets,
  idle_registry_polled = idle_registry_polled,
  replacement_arrived = replacement_arrived,
  closed_pane_removed = closed_pane_removed,
  timer_stopped = timer_stopped,
  disabled_cleared = disabled_cleared,
  disabled_idle_work = disabled_idle_work,
}))
output:close()
vim.cmd("qa!")
