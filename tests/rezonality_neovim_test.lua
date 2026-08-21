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
local failed_instances = 0
for _, instance in ipairs(instances) do
  if instance.status == "FAILED" then
    failed_instances = failed_instances + 1
  end
end
rezonality.focus_instance(instances[1])
rezonality.reload_instance(instances[2])

local registry_commands = 0
for _, name in ipairs({ "RezonalityInstances", "RezonalityFocus",
    "RezonalityReload" }) do
  if vim.fn.exists(":" .. name) == 2 then
    registry_commands = registry_commands + 1
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
  failed_instances = failed_instances,
  control_actions = control_actions,
  registry_available = rezonality._state().registry_available,
  registry_commands = registry_commands,
}))
output:close()
vim.cmd("qa!")
