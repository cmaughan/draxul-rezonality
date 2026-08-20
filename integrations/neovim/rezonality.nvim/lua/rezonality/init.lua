local M = {}

local namespace = vim.api.nvim_create_namespace("rezonality")
local state = {
  config = nil,
  diagnostics_dir = nil,
  installed_after_ms = 0,
  entries = {},
  by_path = {},
  timer = nil,
  enabled = false,
  commands_created = false,
}

local defaults = {
  refresh_ms = 500,
  auto_refresh = true,
  virtual_text = true,
  signs = true,
  underline = true,
}

local function join(...)
  local parts = { ... }
  return table.concat(parts, "/"):gsub("/+", "/")
end

local function is_windows()
  local uv = vim.uv or vim.loop
  return uv.os_uname().sysname == "Windows_NT"
end

local function normalize(path)
  if not path or path == "" then
    return ""
  end
  local result = path
  if vim.fs and vim.fs.normalize then
    result = vim.fs.normalize(result)
  else
    result = vim.fn.fnamemodify(result, ":p")
  end
  result = result:gsub("\\", "/"):gsub("/$", "")
  return is_windows() and result:lower() or result
end

local function default_diagnostics_dir()
  if vim.env.REZONALITY_DIAGNOSTICS_DIR
      and vim.env.REZONALITY_DIAGNOSTICS_DIR ~= "" then
    return vim.env.REZONALITY_DIAGNOSTICS_DIR
  end

  local base
  if is_windows() then
    base = vim.env.LOCALAPPDATA or vim.env.APPDATA
  elseif (vim.uv or vim.loop).os_uname().sysname == "Darwin" then
    base = join(vim.env.HOME or "", "Library", "Caches")
  else
    base = vim.env.XDG_CACHE_HOME
      or join(vim.env.HOME or "", ".cache")
  end
  return join(base or "", "draxul", "cache", "plugins",
    "dev.draxul.rezonality", "diagnostics")
end

local function decode(path)
  local file = io.open(path, "rb")
  if not file then
    return nil
  end
  local text = file:read("*a")
  file:close()
  local ok, document = pcall(vim.json.decode, text)
  return ok and type(document) == "table" and document or nil
end

local function install_epoch()
  local source = debug.getinfo(1, "S").source
  if source:sub(1, 1) ~= "@" then
    return 0
  end
  local marker_path = join(vim.fn.fnamemodify(source:sub(2), ":h"),
    "installed_at")
  local marker = io.open(marker_path, "rb")
  if not marker then
    return 0
  end
  local value = tonumber(marker:read("*a")) or 0
  marker:close()
  return value
end

local severity = {
  error = vim.diagnostic.severity.ERROR,
  warning = vim.diagnostic.severity.WARN,
  warn = vim.diagnostic.severity.WARN,
  info = vim.diagnostic.severity.INFO,
  hint = vim.diagnostic.severity.HINT,
}

local function source_id(path)
  return vim.fn.fnamemodify(path, ":t:r")
end

local function append_document(result, json_path, document)
  local raw_entries = document.diagnostics
  if type(raw_entries) ~= "table" then
    raw_entries = { document }
  end
  for _, raw in ipairs(raw_entries) do
    if type(raw) == "table" and type(raw.message) == "string"
        and raw.message ~= "" and type(raw.path) == "string"
        and raw.path ~= "" then
      local path = raw.path
      if not path:match("^%a:[/\\]") and path:sub(1, 1) ~= "/"
          and type(document.project_path) == "string" then
        path = join(document.project_path, path)
      end
      local normalized = normalize(path)
      if normalized ~= "" then
        table.insert(result, {
          path = normalized,
          line = math.max(tonumber(raw.line) or 1, 1),
          column = math.max(tonumber(raw.column) or 1, 1),
          severity = severity[string.lower(raw.severity or "error")]
            or vim.diagnostic.severity.ERROR,
          message = raw.message,
          stage = raw.stage or document.stage or "compile",
          source_id = source_id(json_path),
          attempted_generation = document.attempted_generation or 0,
        })
      end
    end
  end
end

local function scan()
  local found = {}
  local pattern = join(state.diagnostics_dir, "*.json")
  for _, path in ipairs(vim.fn.glob(pattern, false, true)) do
    local document = decode(path)
    local timestamp = document and tonumber(document.timestamp_unix_ms)
    if document and (state.installed_after_ms == 0
        or (timestamp and timestamp >= state.installed_after_ms)) then
      append_document(found, path, document)
    end
  end

  local unique = {}
  local ordered = {}
  for _, entry in ipairs(found) do
    local key = table.concat({
      entry.path,
      tostring(entry.line),
      tostring(entry.column),
      tostring(entry.severity),
      entry.message,
    }, "\0")
    local existing = unique[key]
    if existing then
      existing.sources[entry.source_id] = true
    else
      entry.sources = { [entry.source_id] = true }
      unique[key] = entry
      table.insert(ordered, entry)
    end
  end

  table.sort(ordered, function(left, right)
    if left.path ~= right.path then
      return left.path < right.path
    end
    if left.line ~= right.line then
      return left.line < right.line
    end
    if left.column ~= right.column then
      return left.column < right.column
    end
    return left.message < right.message
  end)

  local by_path = {}
  for _, entry in ipairs(ordered) do
    local sources = {}
    for id in pairs(entry.sources) do
      table.insert(sources, id)
    end
    table.sort(sources)
    entry.source_text = "Rezonality (" .. table.concat(sources, ", ") .. ")"
    by_path[entry.path] = by_path[entry.path] or {}
    table.insert(by_path[entry.path], entry)
  end
  return ordered, by_path
end

local function apply_buffer(buffer)
  if not vim.api.nvim_buf_is_valid(buffer) then
    return
  end
  vim.diagnostic.reset(namespace, buffer)
  local path = normalize(vim.api.nvim_buf_get_name(buffer))
  local entries = state.by_path[path]
  if not entries then
    return
  end

  local diagnostics = {}
  for _, entry in ipairs(entries) do
    table.insert(diagnostics, {
      lnum = entry.line - 1,
      col = entry.column - 1,
      severity = entry.severity,
      message = entry.message,
      source = entry.source_text,
      user_data = {
        stage = entry.stage,
        sources = entry.sources,
        attempted_generation = entry.attempted_generation,
      },
    })
  end
  vim.diagnostic.set(namespace, buffer, diagnostics, {
    virtual_text = state.config.virtual_text,
    signs = state.config.signs,
    underline = state.config.underline,
    severity_sort = true,
    update_in_insert = false,
  })
end

local function apply_all_buffers()
  for _, buffer in ipairs(vim.api.nvim_list_bufs()) do
    if vim.api.nvim_buf_is_loaded(buffer) then
      apply_buffer(buffer)
    end
  end
end

function M.refresh()
  if not state.enabled then
    return
  end
  state.entries, state.by_path = scan()
  apply_all_buffers()
end

local function quickfix_type(value)
  if value == vim.diagnostic.severity.ERROR then
    return "E"
  elseif value == vim.diagnostic.severity.WARN then
    return "W"
  elseif value == vim.diagnostic.severity.INFO then
    return "I"
  end
  return "N"
end

function M.problems(open_window)
  M.refresh()
  local items = {}
  for _, entry in ipairs(state.entries) do
    table.insert(items, {
      filename = entry.path,
      lnum = entry.line,
      col = entry.column,
      text = entry.message .. " [" .. entry.source_text .. "]",
      type = quickfix_type(entry.severity),
    })
  end
  vim.fn.setqflist({}, "r", {
    title = "Rezonality diagnostics",
    items = items,
  })
  if open_window ~= false and #items > 0 then
    vim.cmd("copen")
  elseif #items == 0 then
    vim.notify("Rezonality: no diagnostics", vim.log.levels.INFO)
  end
  return items
end

function M.status()
  M.refresh()
  local files = {}
  for _, entry in ipairs(state.entries) do
    files[entry.path] = true
  end
  local file_count = 0
  for _ in pairs(files) do
    file_count = file_count + 1
  end
  vim.notify(string.format("Rezonality: %d diagnostics in %d files (%s)",
    #state.entries, file_count, state.diagnostics_dir), vim.log.levels.INFO)
end

local function stop_timer(close)
  if not state.timer then
    return
  end
  state.timer:stop()
  if close and not state.timer:is_closing() then
    state.timer:close()
    state.timer = nil
  end
end

function M.disable()
  state.enabled = false
  stop_timer(false)
  for _, buffer in ipairs(vim.api.nvim_list_bufs()) do
    vim.diagnostic.reset(namespace, buffer)
  end
end

function M.enable()
  state.enabled = true
  M.refresh()
  if state.config.auto_refresh and state.config.refresh_ms > 0 then
    local uv = vim.uv or vim.loop
    state.timer = state.timer or uv.new_timer()
    state.timer:start(state.config.refresh_ms, state.config.refresh_ms,
      vim.schedule_wrap(M.refresh))
  end
end

local function create_commands()
  if state.commands_created then
    return
  end
  state.commands_created = true
  vim.api.nvim_create_user_command("RezonalityRefresh", M.refresh, {})
  vim.api.nvim_create_user_command("RezonalityProblems", function(options)
    M.problems(not options.bang)
  end, { bang = true })
  vim.api.nvim_create_user_command("RezonalityStatus", M.status, {})
  vim.api.nvim_create_user_command("RezonalityEnable", M.enable, {})
  vim.api.nvim_create_user_command("RezonalityDisable", M.disable, {})
end

function M.setup(options)
  state.config = vim.tbl_deep_extend("force", defaults, options or {})
  state.diagnostics_dir = state.config.diagnostics_dir
    or default_diagnostics_dir()
  state.installed_after_ms = install_epoch()
  create_commands()

  local group = vim.api.nvim_create_augroup("RezonalityDiagnostics", {
    clear = true,
  })
  vim.api.nvim_create_autocmd({ "BufReadPost", "BufEnter" }, {
    group = group,
    callback = function(args)
      if state.enabled then
        apply_buffer(args.buf)
      end
    end,
  })
  vim.api.nvim_create_autocmd("VimLeavePre", {
    group = group,
    callback = function()
      stop_timer(true)
    end,
  })
  M.enable()
end

function M._state()
  return state
end

return M
