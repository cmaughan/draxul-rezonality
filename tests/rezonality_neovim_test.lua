vim.g.rezonality_auto_setup = 0
vim.opt.runtimepath:prepend(vim.env.REZONALITY_TEST_PACKAGE)

local rezonality = require("rezonality")
rezonality.setup({
  auto_refresh = false,
  diagnostics_dir = vim.env.REZONALITY_DIAGNOSTICS_DIR,
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

local output = assert(io.open(vim.env.REZONALITY_TEST_RESULT, "wb"))
output:write(vim.json.encode({
  all_entries = #rezonality._state().entries,
  first_inline = #first,
  second_inline = #second,
  quickfix = #quickfix,
  shared_sources = shared_sources,
}))
output:close()
vim.cmd("qa!")
