vim.g.rezonality_auto_setup = 0
vim.opt.runtimepath:prepend(vim.env.REZONALITY_TEST_PACKAGE)

local rezonality = require("rezonality")
local control_actions = {}
rezonality.setup({
  auto_refresh = false,
  registry_provider = function()
    local project = vim.fn.fnamemodify(vim.env.REZONALITY_TEST_FIRST, ":h")
    return {
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
    }
  end,
  control_runner = function(verb, instance)
    table.insert(control_actions, verb .. ":" .. instance.pane_id)
    return true
  end,
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

local output = assert(io.open(vim.env.REZONALITY_TEST_RESULT, "wb"))
output:write(vim.json.encode({
  all_entries = #rezonality._state().entries,
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
}))
output:close()
vim.cmd("qa!")
